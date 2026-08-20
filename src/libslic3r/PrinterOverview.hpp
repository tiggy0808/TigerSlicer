///|/ TigerSlicer Phase N10/N19: information-rich printer cards and the
///|/ multi-printer overview dashboard. Decision logic only; the wx sidebar
///|/ renders what is returned here.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterOverview_hpp_
#define slic3r_PrinterOverview_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// The activity a printer is in. Distinct from its connection state: a printer
// can be Connected (transport) and Printing (activity), or Connected and
// Error. Offline is the one activity that also implies no live transport.
enum class PrinterActivity : uint8_t
{
    Ready,
    Printing,
    Paused,
    Error,
    Maintenance,
    Offline,
    // Reachable but the provider does not report activity. Not the same as
    // Ready: nothing said it is ready.
    Unknown,
};

const char *printer_activity_name(PrinterActivity activity);

// How far a reported value can be trusted. A card never shows a sensed value
// and an assumed one the same way, and never shows a stale value as current.
enum class Freshness : uint8_t
{
    // Reported within the staleness window.
    Live,
    // Older than the window. Shown with its age, never as current.
    Stale,
    // Never reported.
    Absent,
};

const char *freshness_name(Freshness freshness);

// A measured quantity with its own timestamp, so each field ages on its own.
struct CardMetric
{
    std::optional<double> value;
    // Seconds since the epoch, supplied by the caller. Absent means never
    // reported.
    std::optional<int64_t> reported_s;
};

// What density a card is drawn at. The compact default must stay scannable, so
// the model reports which fields belong at each density rather than the view
// deciding.
enum class CardDensity : uint8_t
{
    Compact,
    Detailed,
};

const char *card_density_name(CardDensity density);

// What the provider can report at all, before asking whether it did. A field
// the provider does not support is not shown as missing data; it is simply not
// part of the card.
struct CardCapabilities
{
    bool reports_job { false };
    bool reports_temperatures { false };
    bool reports_materials { false };
    bool reports_camera { false };
    bool reports_queue { false };
};

// One printer's raw reported state, each field with its own freshness.
struct PrinterReport
{
    std::string     id;
    std::string     display_name;
    PrinterActivity activity { PrinterActivity::Unknown };
    // When the activity itself was last confirmed.
    std::optional<int64_t> activity_reported_s;

    CardCapabilities capabilities;

    std::string             job_name;
    std::optional<int64_t>  job_reported_s;
    CardMetric              progress_fraction;
    CardMetric              remaining_s;
    std::optional<int>      current_tool;

    CardMetric nozzle_c;
    CardMetric bed_c;
    CardMetric chamber_c;

    std::vector<std::string> loaded_materials;
    std::optional<int64_t>   materials_reported_s;

    bool                   camera_configured { false };
    std::optional<int64_t> camera_frame_s;

    std::optional<int>     queue_count;
    std::optional<int64_t> queue_reported_s;

    // True when this card is collapsed, hidden, or scrolled offscreen. The
    // model uses it to say that expensive polling should be skipped; it never
    // polls, but it decides whether the caller should.
    bool visible { true };
};

struct OverviewPolicy
{
    // Older than this and a field is Stale.
    int64_t staleness_window_s { 30 };
    // Camera and other expensive fields are not refreshed for a card that is
    // not visible.
    bool suppress_polling_when_hidden { true };
};

// One field on a rendered card: its label, its formatted value, and how fresh
// it is. A Stale field carries its age so the view can mark it.
struct CardField
{
    std::string label;
    std::string value;
    Freshness   freshness { Freshness::Absent };
    std::optional<int64_t> age_s;
};

struct PrinterCard
{
    std::string     id;
    std::string     display_name;
    PrinterActivity activity { PrinterActivity::Unknown };
    Freshness       activity_freshness { Freshness::Absent };

    // Fields for the compact row, and the additional fields a detailed card
    // adds. Compact stays short on purpose.
    std::vector<CardField> compact_fields;
    std::vector<CardField> detailed_fields;

    // True when the printer needs the user's attention (error, or maintenance
    // due, or authentication). Drives the attention badge.
    bool attention { false };
    std::string attention_reason;

    // True when the card should show a camera thumbnail, and false when it is
    // configured but polling is suppressed because the card is not visible.
    bool show_camera { false };
    // Fields whose live refresh should be skipped because the card is hidden.
    std::vector<std::string> polling_suppressed;

    // True when any shown field is stale, so the whole card can carry a
    // staleness marker in addition to the per-field ones.
    bool has_stale_data { false };
};

// Build a card from a report. `now_s` is supplied by the caller; nothing here
// reads a clock. Fields the provider cannot report are absent from the card
// rather than shown as missing; fields it can report but has not refreshed
// recently are Stale with their age, never shown as current.
PrinterCard build_printer_card(const PrinterReport &report, CardDensity density,
                               const OverviewPolicy &policy, int64_t now_s);

// --- overview dashboard (N19) ----------------------------------------------

// The categories the overview counts and can filter by. A printer contributes
// to exactly one, so the counts sum to the number of printers.
enum class OverviewCategory : uint8_t
{
    Printing,
    Ready,
    Paused,
    Offline,
    NeedsAttention,   // faulted, or otherwise needing the user
    MaintenanceDue,
};

const char *overview_category_name(OverviewCategory category);

struct OverviewCounts
{
    size_t printing { 0 };
    size_t ready { 0 };
    size_t paused { 0 };
    size_t offline { 0 };
    size_t needs_attention { 0 };
    size_t maintenance_due { 0 };

    size_t queued_jobs { 0 };
    size_t total { 0 };

    size_t of(OverviewCategory category) const;
};

// One printer's place in the overview, so the sidebar can be filtered.
struct OverviewEntry
{
    std::string      id;
    OverviewCategory category { OverviewCategory::Offline };
    // Lower sorts first. Actionable problems come before quiet printers, but
    // nothing is hidden.
    int  attention_rank { 0 };
    bool stale { false };
};

struct PrinterOverview
{
    OverviewCounts             counts;
    std::vector<OverviewEntry> entries;

    // Ids in the order the sidebar should list them: attention first, then by
    // id for stability. Offline and unsupported printers are present, just
    // lower.
    std::vector<std::string> ordered_ids;

    // True when at least one entry is stale, so the dashboard can show a
    // staleness marker of its own.
    bool has_stale_data { false };
};

// Whether maintenance is due, supplied per printer because the maintenance
// model (Phase N17) owns that decision. Kept separate from the report so the
// overview does not re-derive it.
struct MaintenanceFlag
{
    std::string id;
    bool        due { false };
};

// Aggregate many printers into overview counts and a filterable, attention-
// ordered listing. Offline and unsupported printers are counted and listed,
// never dropped. Efficient by construction: one pass, no per-printer polling.
PrinterOverview build_overview(const std::vector<PrinterReport> &reports,
                               const std::vector<MaintenanceFlag> &maintenance,
                               const OverviewPolicy &policy, int64_t now_s);

// The ids that a filter on one category selects, from an overview.
std::vector<std::string> filter_overview(const PrinterOverview &overview,
                                         OverviewCategory       category);

} // namespace Slic3r

#endif // slic3r_PrinterOverview_hpp_
