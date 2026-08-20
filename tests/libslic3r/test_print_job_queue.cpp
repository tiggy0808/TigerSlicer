// Phase N: local multi-printer job queue. The queue decides what gets sent to
// a printer, so the properties under test are that it never retargets to an
// incompatible machine, never claims progress it did not observe, and survives
// a restart without losing or inventing state.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintJobQueue.hpp"

using namespace Slic3r;

namespace {

PrintJob job(const std::string &id, const std::string &target = {},
             std::vector<std::string> compatible = {})
{
    PrintJob entry;
    entry.id                  = id;
    entry.name                = "job " + id;
    entry.artifact_path       = "/tmp/" + id + ".gcode";
    entry.artifact_sha256     = "abc" + id;
    entry.artifact_bytes      = 1024;
    entry.target_printer      = target;
    entry.compatible_printers = std::move(compatible);
    return entry;
}

} // namespace

TEST_CASE("Jobs queue, reorder, and remove predictably", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a"));
    queue.add(job("b"));
    queue.add(job("c"));
    REQUIRE(queue.size() == 3);

    CHECK(queue.reorder("c", 0));
    CHECK(queue.jobs().front().id == "c");

    // Out-of-range index clamps rather than failing or corrupting order.
    CHECK(queue.reorder("c", 99));
    CHECK(queue.jobs().back().id == "c");

    CHECK(queue.remove("b"));
    CHECK(queue.size() == 2);
    CHECK_FALSE(queue.remove("nonexistent"));
}

TEST_CASE("Adding an existing id replaces rather than duplicates", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a"));
    PrintJob updated = job("a");
    updated.name = "renamed";
    queue.add(updated);

    REQUIRE(queue.size() == 1);
    CHECK(queue.find("a")->name == "renamed");
}

TEST_CASE("Held jobs are skipped until released", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1"));
    queue.add(job("b", "printer1"));

    REQUIRE(queue.hold("a"));
    const PrintJob *next = queue.next_for("printer1");
    REQUIRE(next != nullptr);
    CHECK(next->id == "b");

    REQUIRE(queue.release("a"));
    next = queue.next_for("printer1");
    REQUIRE(next != nullptr);
    CHECK(next->id == "a");
}

TEST_CASE("Retargeting to an incompatible printer is refused with a reason", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1", { "printer1", "printer2" }));

    const RetargetResult rejected = queue.retarget("a", "printer9");
    CHECK_FALSE(rejected.accepted);
    CHECK_FALSE(rejected.reason.empty());
    // The original target must be untouched after a refusal.
    CHECK(queue.find("a")->target_printer == "printer1");

    const RetargetResult accepted = queue.retarget("a", "printer2");
    CHECK(accepted.accepted);
    CHECK(queue.find("a")->target_printer == "printer2");
}

TEST_CASE("A job that already left the queue cannot be retargeted", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1", { "printer1", "printer2" }));
    REQUIRE(queue.set_state("a", JobState::Uploaded));

    const RetargetResult result = queue.retarget("a", "printer2");
    CHECK_FALSE(result.accepted);
    CHECK(queue.find("a")->target_printer == "printer1");
}

TEST_CASE("An untargeted job is offered to any compatible printer", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "", { "printer1", "printer2" }));

    CHECK(queue.next_for("printer2") != nullptr);
    // ...but never to one it was not found compatible with.
    CHECK(queue.next_for("printer9") == nullptr);
}

TEST_CASE("Starting is never implied by queueing", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1"));
    // Authorization to start is separate from being queued or uploaded.
    CHECK_FALSE(queue.find("a")->start_authorized);
}

TEST_CASE("The queue round-trips through serialization", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1", { "printer1" }));
    queue.add(job("b", "", { "printer1", "printer2" }));
    REQUIRE(queue.hold("b"));

    PrintJobQueue restored;
    REQUIRE(restored.deserialize(queue.serialize()));
    REQUIRE(restored.size() == 2);
    CHECK(restored.find("a")->artifact_sha256 == "abca");
    CHECK(restored.find("b")->state == JobState::Held);
    CHECK(restored.find("b")->compatible_printers.size() == 2);
}

TEST_CASE("An interrupted upload returns to the queue on restart", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1"));
    REQUIRE(queue.set_state("a", JobState::Uploading));

    PrintJobQueue restored;
    REQUIRE(restored.deserialize(queue.serialize()));
    // We cannot know the upload completed, so it must not be recorded as if
    // it had arrived.
    CHECK(restored.find("a")->state == JobState::Queued);
}

TEST_CASE("Malformed or unknown-version payloads leave the queue untouched", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a"));

    CHECK_FALSE(queue.deserialize("not json at all"));
    CHECK_FALSE(queue.deserialize(R"({"version":99,"jobs":[]})"));
    CHECK_FALSE(queue.deserialize(R"({"version":1,"jobs":[{"id":"x","state":"teleported"}]})"));
    // Every refusal must be non-destructive.
    CHECK(queue.size() == 1);
    CHECK(queue.find("a") != nullptr);
}

TEST_CASE("Reconciliation promotes only what the printer actually reports", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1"));
    queue.add(job("b", "printer1"));

    queue.reconcile_remote("printer1", { "a" });
    CHECK(queue.find("a")->state == JobState::Uploaded);
    // 'b' was not reported, so it stays queued rather than being assumed sent.
    CHECK(queue.find("b")->state == JobState::Queued);
}

TEST_CASE("Reconciliation never downgrades a job that is further along", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1"));
    REQUIRE(queue.set_state("a", JobState::Started));

    queue.reconcile_remote("printer1", { "a" });
    CHECK(queue.find("a")->state == JobState::Started);
}

TEST_CASE("A job the printer forgot is returned to the queue, not assumed printed", "[job-queue]")
{
    PrintJobQueue queue;
    queue.add(job("a", "printer1"));
    REQUIRE(queue.set_state("a", JobState::Uploaded));

    queue.reconcile_remote("printer1", {});
    CHECK(queue.find("a")->state == JobState::Queued);
    CHECK_FALSE(queue.find("a")->last_error.empty());
}
