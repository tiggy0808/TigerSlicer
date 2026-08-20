///|/ TigerSlicer Phase N: local multi-printer job queue.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrintJobQueue_hpp_
#define slic3r_PrintJobQueue_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class JobState
{
    Queued,     // waiting, eligible to upload
    Held,       // explicitly paused by the user
    Uploading,
    Uploaded,   // on the printer, not started
    Started,    // printing was authorized and begun
    Done,
    Failed,
};

// A queued job always refers to an artifact that was already sliced. The queue
// never reslices: profiles changing underneath must not silently alter what
// gets printed.
struct PrintJob
{
    std::string id;
    std::string name;

    // Exact artifact this job will send, and its hash for integrity checks.
    std::string artifact_path;
    std::string artifact_sha256;
    uint64_t    artifact_bytes { 0 };
    // Reproducibility manifest describing how the artifact was produced.
    std::string manifest_id;

    // Printer this job targets. Empty means "any compatible candidate".
    std::string target_printer;
    // Printers previously determined compatible, with the reason recorded for
    // any that were rejected.
    std::vector<std::string> compatible_printers;

    JobState state { JobState::Queued };
    // Set when the user explicitly authorized starting, not merely uploading.
    bool start_authorized { false };

    std::string last_error;
};

// Result of attempting to retarget a job, so the UI can explain refusals
// instead of silently doing nothing.
struct RetargetResult
{
    bool        accepted { false };
    std::string reason;
};

// An ordered, persistable local queue. All operations are explicit; nothing
// here contacts a printer or starts a print.
class PrintJobQueue
{
public:
    void add(PrintJob job);
    bool remove(const std::string &id);

    // Move a job to a new index, clamped to the queue bounds.
    bool reorder(const std::string &id, size_t new_index);

    bool hold(const std::string &id);
    bool release(const std::string &id);

    // Retarget only to a printer already recorded as compatible. Refusing with
    // a reason is required: silently moving a job to an incompatible printer
    // is the failure this guards against.
    RetargetResult retarget(const std::string &id, const std::string &printer);

    // Next job eligible to send to the given printer: queued (not held), and
    // either targeted at it or compatible with it.
    const PrintJob *next_for(const std::string &printer) const;

    bool set_state(const std::string &id, JobState state);

    const std::vector<PrintJob> &jobs() const { return m_jobs; }
    const PrintJob *find(const std::string &id) const;
    size_t size() const { return m_jobs.size(); }

    // Round-trips the queue so it survives an application restart.
    std::string serialize() const;
    // Returns false and leaves the queue untouched when the payload is not
    // understood, rather than partially applying it.
    bool deserialize(const std::string &json);

    // Reconcile against what a printer reports it already has. Conflicts are
    // resolved conservatively: the queue never invents progress, and never
    // downgrades a job the printer says is further along.
    void reconcile_remote(const std::string &printer,
                          const std::vector<std::string> &remote_job_ids);

private:
    PrintJob *find_mutable(const std::string &id);

    std::vector<PrintJob> m_jobs;
};

const char *to_string(JobState state);
std::optional<JobState> job_state_from_string(const std::string &text);

} // namespace Slic3r

#endif // slic3r_PrintJobQueue_hpp_
