///|/ TigerSlicer Phase N10/N19: printer cards and the overview dashboard.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/PrinterOverview.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

constexpr int64_t NOW = 1'700'000'000;

CardMetric metric(double value, int64_t age_s)
{
    CardMetric m;
    m.value      = value;
    m.reported_s = NOW - age_s;
    return m;
}

CardMetric never()
{
    return CardMetric {};
}

PrinterReport printing_printer(const std::string &id)
{
    PrinterReport report;
    report.id                  = id;
    report.display_name        = id;
    report.activity            = PrinterActivity::Printing;
    report.activity_reported_s = NOW - 2;
    report.capabilities.reports_job          = true;
    report.capabilities.reports_temperatures = true;
    report.capabilities.reports_materials    = true;
    report.capabilities.reports_camera       = true;
    report.capabilities.reports_queue        = true;
    report.job_name          = "two-towers.gcode";
    report.job_reported_s    = NOW - 2;
    report.progress_fraction = metric(0.42, 2);
    report.remaining_s       = metric(3660., 2);
    report.current_tool      = 0;
    report.nozzle_c          = metric(210., 2);
    report.bed_c             = metric(60., 2);
    report.chamber_c         = never();
    report.loaded_materials  = { "PLA", "PETG" };
    report.materials_reported_s = NOW - 2;
    report.camera_configured = true;
    report.camera_frame_s    = NOW - 2;
    report.queue_count       = 3;
    report.queue_reported_s  = NOW - 2;
    return report;
}

const CardField *field(const std::vector<CardField> &fields, const std::string &label)
{
    for (const CardField &f : fields)
        if (f.label == label)
            return &f;
    return nullptr;
}

bool suppressed(const PrinterCard &card, const std::string &what)
{
    return std::find(card.polling_suppressed.begin(), card.polling_suppressed.end(), what) !=
           card.polling_suppressed.end();
}

} // namespace

TEST_CASE("N10: a card shows only what the provider reports, gated by capability",
          "[printer-overview]")
{
    OverviewPolicy policy;

    SECTION("a full card carries job and temperatures where they are reported")
    {
        const PrinterCard card =
            build_printer_card(printing_printer("mk4"), CardDensity::Detailed, policy, NOW);
        CHECK(card.activity == PrinterActivity::Printing);
        CHECK(field(card.compact_fields, "job") != nullptr);
        CHECK(field(card.compact_fields, "progress")->value == "42%");
        CHECK(field(card.compact_fields, "remaining")->value == "1h 1m");
        CHECK(field(card.compact_fields, "tool")->value == "T0");
        CHECK(field(card.detailed_fields, "nozzle")->value == "210 C");
        CHECK(field(card.detailed_fields, "materials")->value == "PLA, PETG");
        CHECK(field(card.detailed_fields, "queue")->value == "3");
        CHECK_FALSE(card.has_stale_data);
    }

    SECTION("a capability the provider lacks is not shown as missing, it is simply absent")
    {
        PrinterReport report          = printing_printer("mini");
        report.capabilities.reports_temperatures = false;
        report.capabilities.reports_camera       = false;
        const PrinterCard card =
            build_printer_card(report, CardDensity::Detailed, policy, NOW);
        CHECK(field(card.detailed_fields, "nozzle") == nullptr);
        CHECK_FALSE(card.show_camera);
        // But the things it does report are still there.
        CHECK(field(card.detailed_fields, "materials") != nullptr);
    }

    SECTION("a field the provider supports but never reported is absent, not zero")
    {
        PrinterReport report = printing_printer("mk4");
        report.chamber_c     = never();
        const PrinterCard card =
            build_printer_card(report, CardDensity::Detailed, policy, NOW);
        CHECK(field(card.detailed_fields, "chamber") == nullptr);
        // Nozzle, which was reported, is present.
        CHECK(field(card.detailed_fields, "nozzle") != nullptr);
    }
}

TEST_CASE("N10: stale data is never shown as current", "[printer-overview]")
{
    OverviewPolicy policy;
    policy.staleness_window_s = 30;

    PrinterReport report = printing_printer("mk4");
    // The nozzle reading is two minutes old; everything else is fresh.
    report.nozzle_c = metric(210., 120);

    const PrinterCard card = build_printer_card(report, CardDensity::Detailed, policy, NOW);

    const CardField *nozzle = field(card.detailed_fields, "nozzle");
    REQUIRE(nozzle != nullptr);
    CHECK(nozzle->freshness == Freshness::Stale);
    REQUIRE(nozzle->age_s.has_value());
    CHECK(nozzle->age_s.value() == 120);

    // The bed, reported fresh, is Live.
    CHECK(field(card.detailed_fields, "bed")->freshness == Freshness::Live);

    // And the card as a whole is marked stale.
    CHECK(card.has_stale_data);
}

TEST_CASE("N10: a hidden card does not poll the camera or temperatures", "[printer-overview]")
{
    OverviewPolicy policy;

    PrinterReport report = printing_printer("mk4");
    report.visible       = false;

    const PrinterCard card = build_printer_card(report, CardDensity::Detailed, policy, NOW);
    CHECK_FALSE(card.show_camera);
    CHECK(suppressed(card, "camera"));
    CHECK(suppressed(card, "temperatures"));

    // Visible again, the camera is shown.
    report.visible = true;
    const PrinterCard shown = build_printer_card(report, CardDensity::Detailed, policy, NOW);
    CHECK(shown.show_camera);
    CHECK(shown.polling_suppressed.empty());
}

TEST_CASE("N10: an errored printer raises an attention badge with a reason", "[printer-overview]")
{
    OverviewPolicy policy;
    PrinterReport  report = printing_printer("mk4");
    report.activity       = PrinterActivity::Error;

    const PrinterCard card = build_printer_card(report, CardDensity::Compact, policy, NOW);
    CHECK(card.attention);
    CHECK_FALSE(card.attention_reason.empty());
}

TEST_CASE("N19: the overview counts every printer into exactly one category",
          "[printer-overview]")
{
    OverviewPolicy policy;

    std::vector<PrinterReport> reports;
    auto with = [&](const std::string &id, PrinterActivity activity) {
        PrinterReport r;
        r.id                  = id;
        r.display_name        = id;
        r.activity            = activity;
        r.activity_reported_s = NOW - 1;
        return r;
    };
    reports.push_back(with("a", PrinterActivity::Printing));
    reports.push_back(with("b", PrinterActivity::Printing));
    reports.push_back(with("c", PrinterActivity::Ready));
    reports.push_back(with("d", PrinterActivity::Paused));
    reports.push_back(with("e", PrinterActivity::Error));
    reports.push_back(with("f", PrinterActivity::Offline));
    reports.push_back(with("g", PrinterActivity::Unknown)); // reachable but silent

    std::vector<MaintenanceFlag> maintenance = { { "c", true } }; // c is due

    const PrinterOverview overview = build_overview(reports, maintenance, policy, NOW);

    CHECK(overview.counts.printing == 2);
    CHECK(overview.counts.paused == 1);
    CHECK(overview.counts.needs_attention == 1);
    // c was Ready but is maintenance-due, so it counts there, not in Ready.
    CHECK(overview.counts.ready == 0);
    CHECK(overview.counts.maintenance_due == 1);
    // Offline and the silent-unknown printer are both Offline, and both listed.
    CHECK(overview.counts.offline == 2);

    // The categories partition the printers: the counts sum to the total.
    const size_t summed = overview.counts.printing + overview.counts.ready +
                          overview.counts.paused + overview.counts.offline +
                          overview.counts.needs_attention + overview.counts.maintenance_due;
    CHECK(summed == overview.counts.total);
    CHECK(overview.counts.total == 7);
}

TEST_CASE("N19: actionable problems sort first, but nothing is hidden", "[printer-overview]")
{
    OverviewPolicy policy;

    std::vector<PrinterReport> reports;
    auto with = [&](const std::string &id, PrinterActivity activity) {
        PrinterReport r;
        r.id                  = id;
        r.display_name        = id;
        r.activity            = activity;
        r.activity_reported_s = NOW - 1;
        return r;
    };
    reports.push_back(with("ready-1", PrinterActivity::Ready));
    reports.push_back(with("offline-1", PrinterActivity::Offline));
    reports.push_back(with("error-1", PrinterActivity::Error));
    reports.push_back(with("printing-1", PrinterActivity::Printing));

    const PrinterOverview overview = build_overview(reports, {}, policy, NOW);

    // The errored printer is first; the offline one is present, just last.
    CHECK(overview.ordered_ids.front() == "error-1");
    CHECK(overview.ordered_ids.back() == "offline-1");
    CHECK(overview.ordered_ids.size() == 4);
    CHECK(std::find(overview.ordered_ids.begin(), overview.ordered_ids.end(), "offline-1") !=
          overview.ordered_ids.end());
}

TEST_CASE("N19: queued jobs are summed, and a filter selects one category", "[printer-overview]")
{
    OverviewPolicy policy;

    std::vector<PrinterReport> reports;
    for (int i = 0; i < 3; ++i) {
        PrinterReport r;
        r.id                            = "p" + std::to_string(i);
        r.display_name                  = r.id;
        r.activity                      = PrinterActivity::Printing;
        r.activity_reported_s           = NOW - 1;
        r.capabilities.reports_queue    = true;
        r.queue_count                   = i + 1; // 1 + 2 + 3
        r.queue_reported_s              = NOW - 1;
        reports.push_back(r);
    }
    reports[1].activity = PrinterActivity::Ready;

    const PrinterOverview overview = build_overview(reports, {}, policy, NOW);
    CHECK(overview.counts.queued_jobs == 6);

    const std::vector<std::string> printing =
        filter_overview(overview, OverviewCategory::Printing);
    CHECK(printing.size() == 2);
    CHECK(std::find(printing.begin(), printing.end(), "p1") == printing.end());

    CHECK(filter_overview(overview, OverviewCategory::Ready) ==
          std::vector<std::string> { "p1" });
}

TEST_CASE("N19: a stale printer is marked stale but still counted and listed",
          "[printer-overview]")
{
    OverviewPolicy policy;
    policy.staleness_window_s = 30;

    PrinterReport fresh;
    fresh.id                  = "fresh";
    fresh.activity            = PrinterActivity::Printing;
    fresh.activity_reported_s = NOW - 5;

    PrinterReport stale;
    stale.id                  = "stale";
    stale.activity            = PrinterActivity::Printing;
    stale.activity_reported_s = NOW - 300; // five minutes old

    const PrinterOverview overview = build_overview({ fresh, stale }, {}, policy, NOW);
    CHECK(overview.has_stale_data);
    CHECK(overview.counts.printing == 2); // the stale one still counts

    for (const OverviewEntry &entry : overview.entries)
        if (entry.id == "stale")
            CHECK(entry.stale);
}

TEST_CASE("N19: the same printers aggregate the same way every time", "[printer-overview]")
{
    OverviewPolicy policy;
    std::vector<PrinterReport> reports;
    for (int i = 0; i < 5; ++i) {
        PrinterReport r;
        r.id                  = "printer-" + std::to_string(i);
        r.display_name        = r.id;
        r.activity            = i % 2 == 0 ? PrinterActivity::Ready : PrinterActivity::Printing;
        r.activity_reported_s = NOW - 1;
        reports.push_back(r);
    }

    const PrinterOverview first = build_overview(reports, {}, policy, NOW);
    std::reverse(reports.begin(), reports.end());
    const PrinterOverview second = build_overview(reports, {}, policy, NOW);

    CHECK(first.ordered_ids == second.ordered_ids);
    CHECK(first.counts.ready == second.counts.ready);
    CHECK(first.counts.printing == second.counts.printing);
}
