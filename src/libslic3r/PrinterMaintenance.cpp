///|/ TigerSlicer Phase N: printer maintenance counters and reminders.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterMaintenance.hpp"

#include <algorithm>

namespace Slic3r {

const char *to_string(CounterEvidence evidence)
{
    switch (evidence) {
    case CounterEvidence::Measured:    return "measured";
    case CounterEvidence::Estimated:   return "estimated";
    case CounterEvidence::UserEntered: return "user-entered";
    }
    return "estimated";
}

const char *to_string(TaskStatus status)
{
    switch (status) {
    case TaskStatus::Ok:      return "ok";
    case TaskStatus::DueSoon: return "due soon";
    case TaskStatus::Overdue: return "overdue";
    case TaskStatus::Snoozed: return "snoozed";
    case TaskStatus::Unknown: return "unknown";
    }
    return "unknown";
}

namespace {

// Measured data is the strongest claim, then user-entered (a human asserted
// it), then estimated. Merging must never weaken what we already know.
int evidence_rank(CounterEvidence evidence)
{
    switch (evidence) {
    case CounterEvidence::Measured:    return 2;
    case CounterEvidence::UserEntered: return 1;
    case CounterEvidence::Estimated:   return 0;
    }
    return 0;
}

} // namespace

MaintenanceCounter *PrinterMaintenance::find_counter(const std::string &key, std::optional<int> tool)
{
    const auto it = std::find_if(m_counters.begin(), m_counters.end(),
        [&](const MaintenanceCounter &counter) {
            return counter.key == key && counter.tool == tool;
        });
    return it == m_counters.end() ? nullptr : &*it;
}

const MaintenanceCounter *PrinterMaintenance::counter(const std::string &key,
                                                      std::optional<int> tool) const
{
    const auto it = std::find_if(m_counters.begin(), m_counters.end(),
        [&](const MaintenanceCounter &counter) {
            return counter.key == key && counter.tool == tool;
        });
    return it == m_counters.end() ? nullptr : &*it;
}

void PrinterMaintenance::set_counter(const MaintenanceCounter &counter)
{
    if (MaintenanceCounter *existing = find_counter(counter.key, counter.tool)) {
        *existing = counter;
        return;
    }
    m_counters.push_back(counter);
}

void PrinterMaintenance::accumulate(const std::string &key, std::optional<int> tool,
                                    double delta, CounterEvidence evidence)
{
    MaintenanceCounter *existing = find_counter(key, tool);
    if (existing == nullptr) {
        MaintenanceCounter counter;
        counter.key      = key;
        counter.tool     = tool;
        counter.value    = delta;
        counter.evidence = evidence;
        m_counters.push_back(std::move(counter));
        return;
    }
    existing->value += delta;
    // A counter that has any estimated contribution cannot claim to be purely
    // measured, so only promote, never demote.
    if (evidence_rank(evidence) < evidence_rank(existing->evidence))
        existing->evidence = evidence;
}

void PrinterMaintenance::add_task(MaintenanceTask task)
{
    if (task.id.empty())
        return;
    if (MaintenanceTask *existing = find_task(task.id)) {
        *existing = std::move(task);
        return;
    }
    m_tasks.push_back(std::move(task));
}

MaintenanceTask *PrinterMaintenance::find_task(const std::string &id)
{
    const auto it = std::find_if(m_tasks.begin(), m_tasks.end(),
        [&id](const MaintenanceTask &task) { return task.id == id; });
    return it == m_tasks.end() ? nullptr : &*it;
}

const MaintenanceTask *PrinterMaintenance::task(const std::string &id) const
{
    const auto it = std::find_if(m_tasks.begin(), m_tasks.end(),
        [&id](const MaintenanceTask &task) { return task.id == id; });
    return it == m_tasks.end() ? nullptr : &*it;
}

bool PrinterMaintenance::complete_task(const CompletionRecord &record)
{
    MaintenanceTask *task = find_task(record.task_id);
    if (task == nullptr)
        return false;
    task->last_completed_at = record.counter_value;
    task->ever_completed    = true;
    task->snoozed_until.reset();   // completing clears any snooze
    m_history.push_back(record);
    return true;
}

bool PrinterMaintenance::snooze_task(const std::string &id, double additional)
{
    MaintenanceTask *task = find_task(id);
    if (task == nullptr || additional <= 0.)
        return false;
    const MaintenanceCounter *counter = this->counter(task->counter_key, task->tool);
    if (counter == nullptr)
        return false;   // nothing to measure the snooze against
    task->snoozed_until = counter->value + additional;
    return true;
}

std::vector<TaskEvaluation> PrinterMaintenance::evaluate() const
{
    std::vector<TaskEvaluation> results;
    results.reserve(m_tasks.size());

    for (const MaintenanceTask &task : m_tasks) {
        TaskEvaluation evaluation;
        evaluation.id = task.id;

        const MaintenanceCounter *counter = this->counter(task.counter_key, task.tool);
        if (counter == nullptr || task.interval <= 0.) {
            // Without a driving counter there is no honest claim to make.
            evaluation.status = TaskStatus::Unknown;
            evaluation.detail = "No counter is being tracked for this task.";
            results.push_back(std::move(evaluation));
            continue;
        }

        evaluation.evidence = counter->evidence;

        const double baseline = task.ever_completed ? task.last_completed_at : 0.;
        const double used     = counter->value - baseline;
        const double remaining = task.interval - used;
        evaluation.remaining = remaining;

        if (task.snoozed_until.has_value() && counter->value < *task.snoozed_until) {
            evaluation.status = TaskStatus::Snoozed;
            evaluation.detail = "Reminder snoozed.";
        } else if (remaining <= 0.) {
            evaluation.status = TaskStatus::Overdue;
        } else if (used >= task.interval * task.due_soon_fraction) {
            evaluation.status = TaskStatus::DueSoon;
        } else {
            evaluation.status = TaskStatus::Ok;
        }

        if (evaluation.detail.empty()) {
            evaluation.detail = std::string("Based on ") + to_string(counter->evidence) +
                                " " + counter->key + ".";
            if (!task.manufacturer_specified)
                // Never imply a vendor-mandated interval we cannot source.
                evaluation.detail += " Interval is a TigerSlicer default, not a "
                                     "manufacturer requirement.";
        }

        results.push_back(std::move(evaluation));
    }

    return results;
}

} // namespace Slic3r
