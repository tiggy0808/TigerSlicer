///|/ TigerSlicer Phase N: local multi-printer job queue.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrintJobQueue.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace Slic3r {

const char *to_string(JobState state)
{
    switch (state) {
    case JobState::Queued:    return "queued";
    case JobState::Held:      return "held";
    case JobState::Uploading: return "uploading";
    case JobState::Uploaded:  return "uploaded";
    case JobState::Started:   return "started";
    case JobState::Done:      return "done";
    case JobState::Failed:    return "failed";
    }
    return "queued";
}

std::optional<JobState> job_state_from_string(const std::string &text)
{
    if (text == "queued")    return JobState::Queued;
    if (text == "held")      return JobState::Held;
    if (text == "uploading") return JobState::Uploading;
    if (text == "uploaded")  return JobState::Uploaded;
    if (text == "started")   return JobState::Started;
    if (text == "done")      return JobState::Done;
    if (text == "failed")    return JobState::Failed;
    return std::nullopt;
}

void PrintJobQueue::add(PrintJob job)
{
    if (job.id.empty())
        return;
    // Adding an id that already exists replaces it rather than creating a
    // duplicate the user cannot tell apart.
    if (PrintJob *existing = find_mutable(job.id)) {
        *existing = std::move(job);
        return;
    }
    m_jobs.push_back(std::move(job));
}

bool PrintJobQueue::remove(const std::string &id)
{
    const auto it = std::find_if(m_jobs.begin(), m_jobs.end(),
        [&id](const PrintJob &job) { return job.id == id; });
    if (it == m_jobs.end())
        return false;
    m_jobs.erase(it);
    return true;
}

bool PrintJobQueue::reorder(const std::string &id, size_t new_index)
{
    const auto it = std::find_if(m_jobs.begin(), m_jobs.end(),
        [&id](const PrintJob &job) { return job.id == id; });
    if (it == m_jobs.end())
        return false;

    PrintJob job = std::move(*it);
    m_jobs.erase(it);
    new_index = std::min(new_index, m_jobs.size());
    m_jobs.insert(m_jobs.begin() + new_index, std::move(job));
    return true;
}

bool PrintJobQueue::hold(const std::string &id)
{
    PrintJob *job = find_mutable(id);
    // Holding only makes sense before the job leaves the queue; a job already
    // uploaded or printing must be dealt with on the printer.
    if (job == nullptr || job->state != JobState::Queued)
        return false;
    job->state = JobState::Held;
    return true;
}

bool PrintJobQueue::release(const std::string &id)
{
    PrintJob *job = find_mutable(id);
    if (job == nullptr || job->state != JobState::Held)
        return false;
    job->state = JobState::Queued;
    return true;
}

RetargetResult PrintJobQueue::retarget(const std::string &id, const std::string &printer)
{
    PrintJob *job = find_mutable(id);
    if (job == nullptr)
        return { false, "That job is no longer in the queue." };

    if (job->state != JobState::Queued && job->state != JobState::Held)
        return { false, "This job has already left the queue and cannot be retargeted." };

    const bool compatible = std::find(job->compatible_printers.begin(),
                                      job->compatible_printers.end(),
                                      printer) != job->compatible_printers.end();
    if (!compatible)
        return { false, "That printer was not found compatible with this job." };

    job->target_printer = printer;
    return { true, {} };
}

const PrintJob *PrintJobQueue::next_for(const std::string &printer) const
{
    for (const PrintJob &job : m_jobs) {
        if (job.state != JobState::Queued)
            continue;   // held, in flight, or finished
        if (!job.target_printer.empty()) {
            if (job.target_printer == printer)
                return &job;
            continue;
        }
        if (std::find(job.compatible_printers.begin(), job.compatible_printers.end(), printer) !=
            job.compatible_printers.end())
            return &job;
    }
    return nullptr;
}

bool PrintJobQueue::set_state(const std::string &id, JobState state)
{
    PrintJob *job = find_mutable(id);
    if (job == nullptr)
        return false;
    job->state = state;
    return true;
}

const PrintJob *PrintJobQueue::find(const std::string &id) const
{
    const auto it = std::find_if(m_jobs.begin(), m_jobs.end(),
        [&id](const PrintJob &job) { return job.id == id; });
    return it == m_jobs.end() ? nullptr : &*it;
}

PrintJob *PrintJobQueue::find_mutable(const std::string &id)
{
    const auto it = std::find_if(m_jobs.begin(), m_jobs.end(),
        [&id](const PrintJob &job) { return job.id == id; });
    return it == m_jobs.end() ? nullptr : &*it;
}

std::string PrintJobQueue::serialize() const
{
    nlohmann::json root;
    root["version"] = 1;
    nlohmann::json jobs = nlohmann::json::array();
    for (const PrintJob &job : m_jobs) {
        nlohmann::json entry;
        entry["id"]                  = job.id;
        entry["name"]                = job.name;
        entry["artifact_path"]       = job.artifact_path;
        entry["artifact_sha256"]     = job.artifact_sha256;
        entry["artifact_bytes"]      = job.artifact_bytes;
        entry["manifest_id"]         = job.manifest_id;
        entry["target_printer"]      = job.target_printer;
        entry["compatible_printers"] = job.compatible_printers;
        entry["state"]               = to_string(job.state);
        entry["start_authorized"]    = job.start_authorized;
        entry["last_error"]          = job.last_error;
        jobs.push_back(std::move(entry));
    }
    root["jobs"] = std::move(jobs);
    return root.dump();
}

bool PrintJobQueue::deserialize(const std::string &json)
{
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;
    // Refuse a version we do not understand rather than guessing at its shape.
    if (!root.contains("version") || !root["version"].is_number_integer() ||
        root["version"].get<int>() != 1)
        return false;
    if (!root.contains("jobs") || !root["jobs"].is_array())
        return false;

    std::vector<PrintJob> parsed;
    for (const nlohmann::json &entry : root["jobs"]) {
        if (!entry.is_object() || !entry.contains("id"))
            return false;
        PrintJob job;
        job.id              = entry.value("id", std::string());
        job.name            = entry.value("name", std::string());
        job.artifact_path   = entry.value("artifact_path", std::string());
        job.artifact_sha256 = entry.value("artifact_sha256", std::string());
        job.artifact_bytes  = entry.value("artifact_bytes", uint64_t(0));
        job.manifest_id     = entry.value("manifest_id", std::string());
        job.target_printer  = entry.value("target_printer", std::string());
        job.start_authorized = entry.value("start_authorized", false);
        job.last_error      = entry.value("last_error", std::string());
        if (entry.contains("compatible_printers") && entry["compatible_printers"].is_array())
            job.compatible_printers =
                entry["compatible_printers"].get<std::vector<std::string>>();
        const std::optional<JobState> state =
            job_state_from_string(entry.value("state", std::string("queued")));
        if (!state.has_value())
            return false;
        job.state = *state;
        // A job that was mid-upload when the application stopped is not known
        // to have arrived; return it to the queue rather than claiming it did.
        if (job.state == JobState::Uploading)
            job.state = JobState::Queued;
        parsed.push_back(std::move(job));
    }

    m_jobs = std::move(parsed);
    return true;
}

void PrintJobQueue::reconcile_remote(const std::string &printer,
                                     const std::vector<std::string> &remote_job_ids)
{
    for (PrintJob &job : m_jobs) {
        if (job.target_printer != printer)
            continue;
        const bool present_remotely =
            std::find(remote_job_ids.begin(), remote_job_ids.end(), job.id) != remote_job_ids.end();

        if (present_remotely) {
            // The printer has it; do not downgrade a state that is further
            // along than "uploaded".
            if (job.state == JobState::Queued || job.state == JobState::Uploading)
                job.state = JobState::Uploaded;
        } else if (job.state == JobState::Uploaded) {
            // We believed it was on the printer and it is not. Do not assume it
            // printed - put it back and let the user decide.
            job.state = JobState::Queued;
            job.last_error = "The printer no longer reports this job; it was returned to the queue.";
        }
    }
}

} // namespace Slic3r
