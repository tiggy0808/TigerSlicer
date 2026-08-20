// Phase N: maintenance counters and reminders. Maintenance decisions rest on
// these numbers, so the tests concentrate on never presenting an estimate as a
// measurement and never implying a manufacturer requirement we cannot source.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/PrinterMaintenance.hpp"

using namespace Slic3r;

namespace {

MaintenanceTask task(const std::string &id, double interval, const std::string &counter)
{
    MaintenanceTask item;
    item.id          = id;
    item.title       = id;
    item.counter_key = counter;
    item.interval    = interval;
    return item;
}

const TaskEvaluation *find(const std::vector<TaskEvaluation> &results, const std::string &id)
{
    const auto it = std::find_if(results.begin(), results.end(),
        [&id](const TaskEvaluation &item) { return item.id == id; });
    return it == results.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("A task is ok, due soon, then overdue as its counter advances", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.add_task(task("lubricate", 100., "print_hours"));

    maintenance.accumulate("print_hours", std::nullopt, 10., CounterEvidence::Measured);
    std::vector<TaskEvaluation> results = maintenance.evaluate();
    CHECK(find(results, "lubricate")->status == TaskStatus::Ok);

    maintenance.accumulate("print_hours", std::nullopt, 85., CounterEvidence::Measured);
    results = maintenance.evaluate();
    CHECK(find(results, "lubricate")->status == TaskStatus::DueSoon);

    maintenance.accumulate("print_hours", std::nullopt, 10., CounterEvidence::Measured);
    results = maintenance.evaluate();
    const TaskEvaluation *overdue = find(results, "lubricate");
    CHECK(overdue->status == TaskStatus::Overdue);
    CHECK(overdue->remaining.value_or(1.) <= 0.);
}

TEST_CASE("Completing a task resets its baseline and records history", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.add_task(task("lubricate", 100., "print_hours"));
    maintenance.accumulate("print_hours", std::nullopt, 120., CounterEvidence::Measured);
    std::vector<TaskEvaluation> before = maintenance.evaluate();
    REQUIRE(find(before, "lubricate")->status == TaskStatus::Overdue);

    CompletionRecord record;
    record.task_id       = "lubricate";
    record.counter_value = 120.;
    record.at            = 1000;
    record.part_replaced = "belt";
    REQUIRE(maintenance.complete_task(record));

    const std::vector<TaskEvaluation> after = maintenance.evaluate();
    CHECK(find(after, "lubricate")->status == TaskStatus::Ok);
    REQUIRE(maintenance.history().size() == 1);
    CHECK(maintenance.history().front().part_replaced == "belt");
}

TEST_CASE("Snoozing suppresses a reminder until the counter passes", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.add_task(task("filter", 50., "print_hours"));
    maintenance.accumulate("print_hours", std::nullopt, 60., CounterEvidence::Measured);
    std::vector<TaskEvaluation> results = maintenance.evaluate();
    REQUIRE(find(results, "filter")->status == TaskStatus::Overdue);

    REQUIRE(maintenance.snooze_task("filter", 10.));
    results = maintenance.evaluate();
    CHECK(find(results, "filter")->status == TaskStatus::Snoozed);

    maintenance.accumulate("print_hours", std::nullopt, 15., CounterEvidence::Measured);
    results = maintenance.evaluate();
    CHECK(find(results, "filter")->status == TaskStatus::Overdue);
}

TEST_CASE("Completing a task clears an active snooze", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.add_task(task("filter", 50., "print_hours"));
    maintenance.accumulate("print_hours", std::nullopt, 60., CounterEvidence::Measured);
    REQUIRE(maintenance.snooze_task("filter", 100.));

    CompletionRecord record;
    record.task_id       = "filter";
    record.counter_value = 60.;
    REQUIRE(maintenance.complete_task(record));

    CHECK_FALSE(maintenance.task("filter")->snoozed_until.has_value());
    const std::vector<TaskEvaluation> results = maintenance.evaluate();
    CHECK(find(results, "filter")->status == TaskStatus::Ok);
}

TEST_CASE("A task with no counter reports unknown, not ok", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.add_task(task("belt tension", 100., "toolchanges"));

    const std::vector<TaskEvaluation> results = maintenance.evaluate();
    const TaskEvaluation *evaluation = find(results, "belt tension");
    REQUIRE(evaluation != nullptr);
    // Claiming "ok" for something never measured would be a false assurance.
    CHECK(evaluation->status == TaskStatus::Unknown);
    CHECK_FALSE(evaluation->remaining.has_value());
}

TEST_CASE("Mixing estimated data into a measured counter downgrades its evidence", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.accumulate("print_hours", std::nullopt, 10., CounterEvidence::Measured);
    REQUIRE(maintenance.counter("print_hours")->evidence == CounterEvidence::Measured);

    maintenance.accumulate("print_hours", std::nullopt, 5., CounterEvidence::Estimated);
    // The total is now partly estimated, so it must not still claim measured.
    CHECK(maintenance.counter("print_hours")->evidence == CounterEvidence::Estimated);
    CHECK(maintenance.counter("print_hours")->value == 15.);
}

TEST_CASE("Adding measured data never downgrades to estimated", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.accumulate("print_hours", std::nullopt, 5., CounterEvidence::Estimated);
    maintenance.accumulate("print_hours", std::nullopt, 5., CounterEvidence::Measured);
    // Still partly estimated overall.
    CHECK(maintenance.counter("print_hours")->evidence == CounterEvidence::Estimated);
}

TEST_CASE("Evidence level is reported alongside the verdict", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.add_task(task("nozzle", 100., "print_hours"));
    maintenance.accumulate("print_hours", std::nullopt, 10., CounterEvidence::Estimated);

    const std::vector<TaskEvaluation> results = maintenance.evaluate();
    const TaskEvaluation *evaluation = find(results, "nozzle");
    REQUIRE(evaluation != nullptr);
    CHECK(evaluation->evidence == CounterEvidence::Estimated);
    CHECK(evaluation->detail.find("estimated") != std::string::npos);
}

TEST_CASE("Intervals are not presented as manufacturer requirements without a source",
          "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.add_task(task("unsourced", 100., "print_hours"));
    MaintenanceTask sourced = task("sourced", 100., "print_hours");
    sourced.manufacturer_specified = true;
    maintenance.add_task(sourced);
    maintenance.accumulate("print_hours", std::nullopt, 1., CounterEvidence::Measured);

    const std::vector<TaskEvaluation> results = maintenance.evaluate();
    CHECK(find(results, "unsourced")->detail.find("not a manufacturer requirement") !=
          std::string::npos);
    CHECK(find(results, "sourced")->detail.find("not a manufacturer requirement") ==
          std::string::npos);
}

TEST_CASE("Per-tool counters are tracked independently", "[maintenance]")
{
    PrinterMaintenance maintenance;
    maintenance.accumulate("throughput", 0, 100., CounterEvidence::Measured);
    maintenance.accumulate("throughput", 1, 5., CounterEvidence::Measured);

    CHECK(maintenance.counter("throughput", 0)->value == 100.);
    CHECK(maintenance.counter("throughput", 1)->value == 5.);
    // A tool-less counter is a different counter again.
    CHECK(maintenance.counter("throughput") == nullptr);
}
