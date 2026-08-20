// Phase N: exact reprint history. "Reprint exact file" must mean the same
// bytes, so these tests concentrate on refusing to reprint when the artifact
// is gone or has changed, and on the retention/privacy controls.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintHistory.hpp"

using namespace Slic3r;

namespace {

PrintHistoryEntry entry(const std::string &id, const std::string &hash = "hash-a")
{
    PrintHistoryEntry record;
    record.id              = id;
    record.job_name        = "bracket";
    record.artifact_path   = "/prints/" + id + ".gcode";
    record.artifact_sha256 = hash;
    record.artifact_bytes  = 2048;
    record.manifest_id     = "manifest-" + id;
    record.printer_name    = "Reference toolchanger";
    record.print_profile   = "0.20 balanced";
    record.tool_materials  = { "PLA", "PETG" };
    record.tool_nozzles_mm = { 0.4, 0.2 };
    record.outcome         = PrintOutcome::Completed;
    return record;
}

ReprintContext context(bool present = true, const std::string &hash_now = "hash-a",
                       bool printer = true)
{
    ReprintContext ctx;
    ctx.artifact_present    = present;
    ctx.artifact_sha256_now = hash_now;
    ctx.printer_available   = printer;
    return ctx;
}

} // namespace

TEST_CASE("An unchanged artifact can be reprinted exactly", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a"));

    const ReprintCheck check = history.check_exact_reprint("a", context());
    CHECK(check.ok());
    CHECK(check.blocker == ReprintBlocker::None);
}

TEST_CASE("A missing artifact blocks exact reprint with guidance", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a"));

    const ReprintCheck check = history.check_exact_reprint("a", context(false));
    CHECK_FALSE(check.ok());
    CHECK(check.blocker == ReprintBlocker::ArtifactMissing);
    // The user needs to be told the alternative, not just refused.
    CHECK_FALSE(check.detail.empty());
}

TEST_CASE("A changed artifact is not the same print", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a", "hash-original"));

    const ReprintCheck check = history.check_exact_reprint("a", context(true, "hash-different"));
    CHECK_FALSE(check.ok());
    CHECK(check.blocker == ReprintBlocker::ArtifactModified);
}

TEST_CASE("An unavailable printer blocks exact reprint", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a"));

    const ReprintCheck check = history.check_exact_reprint("a", context(true, "hash-a", false));
    CHECK_FALSE(check.ok());
    CHECK(check.blocker == ReprintBlocker::PrinterUnavailable);
}

TEST_CASE("An unknown history id cannot be reprinted", "[print-history]")
{
    PrintHistory history;
    const ReprintCheck check = history.check_exact_reprint("nope", context());
    CHECK_FALSE(check.ok());
}

TEST_CASE("Recording the same attempt twice updates rather than duplicates", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a"));

    PrintHistoryEntry finished = entry("a");
    finished.outcome     = PrintOutcome::Failed;
    finished.finished_at = 1000;
    history.record(finished);

    REQUIRE(history.size() == 1);
    CHECK(history.find("a")->outcome == PrintOutcome::Failed);
    CHECK(history.find("a")->finished_at.has_value());
}

TEST_CASE("Retention drops the oldest entries first", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a"));
    history.record(entry("b"));
    history.record(entry("c"));

    CHECK(history.prune(2) == 1);
    CHECK(history.size() == 2);
    CHECK(history.find("a") == nullptr);
    CHECK(history.find("c") != nullptr);

    // Pruning below the limit removes nothing.
    CHECK(history.prune(10) == 0);
}

TEST_CASE("History can be forgotten entirely or per entry", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a"));
    history.record(entry("b"));

    CHECK(history.forget("a"));
    CHECK_FALSE(history.forget("a"));
    CHECK(history.size() == 1);

    history.clear();
    CHECK(history.size() == 0);
}

TEST_CASE("History round-trips through serialization", "[print-history]")
{
    PrintHistory history;
    PrintHistoryEntry record = entry("a");
    record.uploaded_at = 111;
    record.started_at  = 222;
    history.record(record);

    PrintHistory restored;
    REQUIRE(restored.deserialize(history.serialize()));
    REQUIRE(restored.size() == 1);

    const PrintHistoryEntry *back = restored.find("a");
    REQUIRE(back != nullptr);
    CHECK(back->artifact_sha256 == "hash-a");
    CHECK(back->manifest_id == "manifest-a");
    CHECK(back->tool_nozzles_mm.size() == 2);
    CHECK(back->uploaded_at.value_or(0) == 111);
    CHECK(back->outcome == PrintOutcome::Completed);
    // An outcome that was never observed must not become one that was.
    CHECK_FALSE(back->finished_at.has_value());
}

TEST_CASE("Malformed history payloads leave existing history untouched", "[print-history]")
{
    PrintHistory history;
    history.record(entry("a"));

    CHECK_FALSE(history.deserialize("{"));
    CHECK_FALSE(history.deserialize(R"({"version":42,"entries":[]})"));
    CHECK_FALSE(history.deserialize(R"({"version":1,"entries":[{"id":"x","outcome":"melted"}]})"));
    CHECK(history.size() == 1);
}
