///|/ TigerSlicer Phase N: exact reprint history.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrintHistory_hpp_
#define slic3r_PrintHistory_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class PrintOutcome
{
    Unknown,
    Completed,
    Failed,
    Cancelled,
};

// A record of one attempt to print something. The point of this record is that
// "print this again" can mean the *same bytes*, so the artifact identity is
// kept alongside the settings that produced it.
struct PrintHistoryEntry
{
    std::string id;
    std::string job_name;

    // Exact artifact that was sent.
    std::string artifact_path;      // may no longer exist
    std::string artifact_sha256;
    uint64_t    artifact_bytes { 0 };
    std::string manifest_id;        // reproducibility manifest

    std::string printer_name;
    std::string printer_identity;   // connection identity at the time

    // Versions in effect when the artifact was produced.
    std::string print_profile;
    std::string printer_profile;
    std::string calibration_id;

    std::vector<std::string> tool_materials;   // per tool, as configured
    std::vector<double>      tool_nozzles_mm;

    // Absolute timestamps supplied by the caller (seconds since epoch), so
    // this unit stays free of clock dependencies and remains deterministic.
    std::optional<int64_t> uploaded_at;
    std::optional<int64_t> started_at;
    std::optional<int64_t> finished_at;

    PrintOutcome outcome { PrintOutcome::Unknown };
    std::string  notes;
};

// Why an exact reprint cannot proceed. Reported rather than silently
// substituting a freshly sliced file, which would not be the same print.
enum class ReprintBlocker
{
    None,
    ArtifactMissing,
    ArtifactModified,
    PrinterUnavailable,
};

struct ReprintCheck
{
    ReprintBlocker blocker { ReprintBlocker::None };
    std::string    detail;
    bool ok() const { return blocker == ReprintBlocker::None; }
};

// State of the world needed to judge whether an exact reprint is possible.
struct ReprintContext
{
    bool        artifact_present { true };
    // Hash of the file as it exists now; compared against the recorded hash.
    std::string artifact_sha256_now;
    bool        printer_available { true };
};

class PrintHistory
{
public:
    void record(PrintHistoryEntry entry);

    const std::vector<PrintHistoryEntry> &entries() const { return m_entries; }
    const PrintHistoryEntry *find(const std::string &id) const;
    size_t size() const { return m_entries.size(); }

    // Verify that "reprint exact file" would really send the original bytes.
    ReprintCheck check_exact_reprint(const std::string &id, const ReprintContext &context) const;

    // Retention: keep at most `max_entries`, dropping the oldest first. Returns
    // how many were removed.
    size_t prune(size_t max_entries);
    // Privacy control: forget everything, or one entry.
    void clear();
    bool forget(const std::string &id);

    std::string serialize() const;
    bool deserialize(const std::string &json);

private:
    std::vector<PrintHistoryEntry> m_entries;   // oldest first
};

const char *to_string(PrintOutcome outcome);
std::optional<PrintOutcome> print_outcome_from_string(const std::string &text);

} // namespace Slic3r

#endif // slic3r_PrintHistory_hpp_
