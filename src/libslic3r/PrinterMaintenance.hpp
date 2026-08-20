///|/ TigerSlicer Phase N: printer maintenance counters and reminders.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterMaintenance_hpp_
#define slic3r_PrinterMaintenance_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Where a counter's value came from. This distinction is the point of the
// module: an estimate must never be presented as a measurement, because
// maintenance decisions are made on it.
enum class CounterEvidence
{
    Measured,   // reported by the printer's own telemetry
    Estimated,  // derived by TigerSlicer from slicing/job data
    UserEntered,
};

struct MaintenanceCounter
{
    std::string     key;        // e.g. "print_hours", "toolchanges"
    std::string     label;
    double          value { 0. };
    std::string     unit;       // "h", "changes", "g", ...
    CounterEvidence evidence { CounterEvidence::Estimated };
    // Which tool this counter belongs to, when it is per-tool.
    std::optional<int> tool;
};

enum class TaskStatus
{
    Ok,
    DueSoon,
    Overdue,
    Snoozed,
    // The interval is known but the counter driving it is not being tracked,
    // so no claim can be made either way.
    Unknown,
};

// A maintenance task with an interval expressed in some counter's units.
struct MaintenanceTask
{
    std::string id;
    std::string title;
    std::string counter_key;     // which counter drives it
    std::optional<int> tool;

    double interval { 0. };      // e.g. every 200 h
    // Counter value when the task was last completed.
    double last_completed_at { 0. };
    bool   ever_completed { false };

    // Suppress reminders until the counter passes this value.
    std::optional<double> snoozed_until;

    // Only set when the interval came from sourced profile metadata. Without
    // it, TigerSlicer must not imply a manufacturer requirement.
    bool manufacturer_specified { false };

    // Fraction of the interval at which "due soon" begins.
    double due_soon_fraction { 0.9 };
};

struct TaskEvaluation
{
    std::string id;
    TaskStatus  status { TaskStatus::Unknown };
    // Counter units remaining until due; absent when it cannot be computed.
    std::optional<double> remaining;
    CounterEvidence evidence { CounterEvidence::Estimated };
    std::string detail;
};

struct CompletionRecord
{
    std::string task_id;
    double      counter_value { 0. };
    int64_t     at { 0 };          // caller-supplied timestamp
    std::string notes;
    std::string part_replaced;
};

// Per-printer maintenance state.
class PrinterMaintenance
{
public:
    void set_counter(const MaintenanceCounter &counter);
    // Add to a counter, keeping its evidence level. Adding measured data to an
    // estimated counter promotes it; the reverse never downgrades it.
    void accumulate(const std::string &key, std::optional<int> tool, double delta,
                    CounterEvidence evidence);
    const MaintenanceCounter *counter(const std::string &key, std::optional<int> tool = std::nullopt) const;

    void add_task(MaintenanceTask task);
    const MaintenanceTask *task(const std::string &id) const;

    // Record completion and reset the task's baseline.
    bool complete_task(const CompletionRecord &record);
    bool snooze_task(const std::string &id, double additional);

    std::vector<TaskEvaluation> evaluate() const;

    const std::vector<CompletionRecord> &history() const { return m_history; }
    const std::vector<MaintenanceTask> &tasks() const { return m_tasks; }
    const std::vector<MaintenanceCounter> &counters() const { return m_counters; }

private:
    MaintenanceCounter *find_counter(const std::string &key, std::optional<int> tool);
    MaintenanceTask *find_task(const std::string &id);

    std::vector<MaintenanceCounter> m_counters;
    std::vector<MaintenanceTask>    m_tasks;
    std::vector<CompletionRecord>   m_history;
};

const char *to_string(CounterEvidence evidence);
const char *to_string(TaskStatus status);

} // namespace Slic3r

#endif // slic3r_PrinterMaintenance_hpp_
