///|/ TigerSlicer Phase N: exact reprint history.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrintHistory.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace Slic3r {

const char *to_string(PrintOutcome outcome)
{
    switch (outcome) {
    case PrintOutcome::Unknown:   return "unknown";
    case PrintOutcome::Completed: return "completed";
    case PrintOutcome::Failed:    return "failed";
    case PrintOutcome::Cancelled: return "cancelled";
    }
    return "unknown";
}

std::optional<PrintOutcome> print_outcome_from_string(const std::string &text)
{
    if (text == "unknown")   return PrintOutcome::Unknown;
    if (text == "completed") return PrintOutcome::Completed;
    if (text == "failed")    return PrintOutcome::Failed;
    if (text == "cancelled") return PrintOutcome::Cancelled;
    return std::nullopt;
}

void PrintHistory::record(PrintHistoryEntry entry)
{
    if (entry.id.empty())
        return;
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [&entry](const PrintHistoryEntry &existing) { return existing.id == entry.id; });
    if (it != m_entries.end()) {
        // Updating an attempt in place (e.g. recording its outcome) must not
        // create a second history row for the same print.
        *it = std::move(entry);
        return;
    }
    m_entries.push_back(std::move(entry));
}

const PrintHistoryEntry *PrintHistory::find(const std::string &id) const
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [&id](const PrintHistoryEntry &entry) { return entry.id == id; });
    return it == m_entries.end() ? nullptr : &*it;
}

ReprintCheck PrintHistory::check_exact_reprint(const std::string &id,
                                               const ReprintContext &context) const
{
    const PrintHistoryEntry *entry = find(id);
    if (entry == nullptr)
        return { ReprintBlocker::ArtifactMissing, "That history entry no longer exists." };

    if (!context.artifact_present)
        return { ReprintBlocker::ArtifactMissing,
                 "The original file is no longer available, so it cannot be reprinted exactly. "
                 "Reslice with current settings instead." };

    // A file that changed underneath us is not the print that was recorded,
    // even if the path still resolves.
    if (!entry->artifact_sha256.empty() && !context.artifact_sha256_now.empty() &&
        entry->artifact_sha256 != context.artifact_sha256_now)
        return { ReprintBlocker::ArtifactModified,
                 "The file at the recorded location has changed since this print, so it is not "
                 "the same artifact." };

    if (!context.printer_available)
        return { ReprintBlocker::PrinterUnavailable,
                 "The printer used for this print is not currently available." };

    return { ReprintBlocker::None, {} };
}

size_t PrintHistory::prune(size_t max_entries)
{
    if (m_entries.size() <= max_entries)
        return 0;
    const size_t removed = m_entries.size() - max_entries;
    m_entries.erase(m_entries.begin(), m_entries.begin() + removed);
    return removed;
}

void PrintHistory::clear()
{
    m_entries.clear();
}

bool PrintHistory::forget(const std::string &id)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [&id](const PrintHistoryEntry &entry) { return entry.id == id; });
    if (it == m_entries.end())
        return false;
    m_entries.erase(it);
    return true;
}

std::string PrintHistory::serialize() const
{
    nlohmann::json root;
    root["version"] = 1;
    nlohmann::json list = nlohmann::json::array();
    for (const PrintHistoryEntry &entry : m_entries) {
        nlohmann::json item;
        item["id"]               = entry.id;
        item["job_name"]         = entry.job_name;
        item["artifact_path"]    = entry.artifact_path;
        item["artifact_sha256"]  = entry.artifact_sha256;
        item["artifact_bytes"]   = entry.artifact_bytes;
        item["manifest_id"]      = entry.manifest_id;
        item["printer_name"]     = entry.printer_name;
        item["printer_identity"] = entry.printer_identity;
        item["print_profile"]    = entry.print_profile;
        item["printer_profile"]  = entry.printer_profile;
        item["calibration_id"]   = entry.calibration_id;
        item["tool_materials"]   = entry.tool_materials;
        item["tool_nozzles_mm"]  = entry.tool_nozzles_mm;
        item["outcome"]          = to_string(entry.outcome);
        item["notes"]            = entry.notes;
        if (entry.uploaded_at) item["uploaded_at"] = *entry.uploaded_at;
        if (entry.started_at)  item["started_at"]  = *entry.started_at;
        if (entry.finished_at) item["finished_at"] = *entry.finished_at;
        list.push_back(std::move(item));
    }
    root["entries"] = std::move(list);
    return root.dump();
}

bool PrintHistory::deserialize(const std::string &json)
{
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;
    if (!root.contains("version") || !root["version"].is_number_integer() ||
        root["version"].get<int>() != 1)
        return false;
    if (!root.contains("entries") || !root["entries"].is_array())
        return false;

    std::vector<PrintHistoryEntry> parsed;
    for (const nlohmann::json &item : root["entries"]) {
        if (!item.is_object() || !item.contains("id"))
            return false;
        PrintHistoryEntry entry;
        entry.id               = item.value("id", std::string());
        entry.job_name         = item.value("job_name", std::string());
        entry.artifact_path    = item.value("artifact_path", std::string());
        entry.artifact_sha256  = item.value("artifact_sha256", std::string());
        entry.artifact_bytes   = item.value("artifact_bytes", uint64_t(0));
        entry.manifest_id      = item.value("manifest_id", std::string());
        entry.printer_name     = item.value("printer_name", std::string());
        entry.printer_identity = item.value("printer_identity", std::string());
        entry.print_profile    = item.value("print_profile", std::string());
        entry.printer_profile  = item.value("printer_profile", std::string());
        entry.calibration_id   = item.value("calibration_id", std::string());
        entry.notes            = item.value("notes", std::string());
        if (item.contains("tool_materials") && item["tool_materials"].is_array())
            entry.tool_materials = item["tool_materials"].get<std::vector<std::string>>();
        if (item.contains("tool_nozzles_mm") && item["tool_nozzles_mm"].is_array())
            entry.tool_nozzles_mm = item["tool_nozzles_mm"].get<std::vector<double>>();
        if (item.contains("uploaded_at") && item["uploaded_at"].is_number_integer())
            entry.uploaded_at = item["uploaded_at"].get<int64_t>();
        if (item.contains("started_at") && item["started_at"].is_number_integer())
            entry.started_at = item["started_at"].get<int64_t>();
        if (item.contains("finished_at") && item["finished_at"].is_number_integer())
            entry.finished_at = item["finished_at"].get<int64_t>();
        const std::optional<PrintOutcome> outcome =
            print_outcome_from_string(item.value("outcome", std::string("unknown")));
        if (!outcome.has_value())
            return false;
        entry.outcome = *outcome;
        parsed.push_back(std::move(entry));
    }

    m_entries = std::move(parsed);
    return true;
}

} // namespace Slic3r
