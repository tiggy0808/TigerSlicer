///|/ TigerSlicer Phase N10/N19: information-rich printer cards and the
///|/ multi-printer overview dashboard.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterOverview.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

const char *printer_activity_name(PrinterActivity activity)
{
    switch (activity) {
    case PrinterActivity::Ready:       return "ready";
    case PrinterActivity::Printing:    return "printing";
    case PrinterActivity::Paused:      return "paused";
    case PrinterActivity::Error:       return "error";
    case PrinterActivity::Maintenance: return "maintenance";
    case PrinterActivity::Offline:     return "offline";
    case PrinterActivity::Unknown:     return "unknown";
    }
    return "unknown";
}

const char *freshness_name(Freshness freshness)
{
    switch (freshness) {
    case Freshness::Live:   return "live";
    case Freshness::Stale:  return "stale";
    case Freshness::Absent: return "absent";
    }
    return "absent";
}

const char *card_density_name(CardDensity density)
{
    switch (density) {
    case CardDensity::Compact:  return "compact";
    case CardDensity::Detailed: return "detailed";
    }
    return "compact";
}

const char *overview_category_name(OverviewCategory category)
{
    switch (category) {
    case OverviewCategory::Printing:       return "printing";
    case OverviewCategory::Ready:          return "ready";
    case OverviewCategory::Paused:         return "paused";
    case OverviewCategory::Offline:        return "offline";
    case OverviewCategory::NeedsAttention: return "needs attention";
    case OverviewCategory::MaintenanceDue: return "maintenance due";
    }
    return "offline";
}

size_t OverviewCounts::of(OverviewCategory category) const
{
    switch (category) {
    case OverviewCategory::Printing:       return printing;
    case OverviewCategory::Ready:          return ready;
    case OverviewCategory::Paused:         return paused;
    case OverviewCategory::Offline:        return offline;
    case OverviewCategory::NeedsAttention: return needs_attention;
    case OverviewCategory::MaintenanceDue: return maintenance_due;
    }
    return 0;
}

// --- card ------------------------------------------------------------------

namespace {

Freshness freshness_of(const std::optional<int64_t> &reported_s, int64_t now_s,
                       int64_t window_s)
{
    if (! reported_s.has_value())
        return Freshness::Absent;
    if (now_s - *reported_s > window_s)
        return Freshness::Stale;
    return Freshness::Live;
}

std::string format_number(double value, int decimals)
{
    std::ostringstream out;
    out.precision(decimals);
    out << std::fixed << value;
    return out.str();
}

std::string format_duration(double seconds)
{
    const long total = long(std::llround(seconds));
    const long h     = total / 3600;
    const long m     = (total % 3600) / 60;
    std::ostringstream out;
    if (h > 0)
        out << h << "h ";
    out << m << "m";
    return out.str();
}

// Add a field only when it was reported at all. A Stale field is added with its
// age; a Live one without. An absent field is not added, so the card does not
// list data the printer never sent.
void add_metric_field(std::vector<CardField> &fields, const std::string &label,
                      const CardMetric &metric, const std::string &unit, int decimals,
                      int64_t now_s, int64_t window_s, bool &any_stale)
{
    const Freshness freshness = freshness_of(metric.reported_s, now_s, window_s);
    if (freshness == Freshness::Absent || ! metric.value.has_value())
        return;
    CardField field;
    field.label     = label;
    field.value     = format_number(*metric.value, decimals) + unit;
    field.freshness = freshness;
    if (freshness == Freshness::Stale) {
        field.age_s = now_s - *metric.reported_s;
        any_stale   = true;
    }
    fields.push_back(field);
}

} // namespace

PrinterCard build_printer_card(const PrinterReport &report, CardDensity density,
                               const OverviewPolicy &policy, int64_t now_s)
{
    PrinterCard card;
    card.id           = report.id;
    card.display_name = report.display_name;
    card.activity     = report.activity;
    card.activity_freshness =
        freshness_of(report.activity_reported_s, now_s, policy.staleness_window_s);

    bool any_stale = false;

    // Attention: an error, or a printer whose activity says it needs the user.
    if (report.activity == PrinterActivity::Error) {
        card.attention        = true;
        card.attention_reason = "the printer reported an error";
    } else if (report.activity == PrinterActivity::Maintenance) {
        card.attention        = true;
        card.attention_reason = "maintenance is in progress";
    }

    const bool poll_hidden = policy.suppress_polling_when_hidden && ! report.visible;

    // Compact fields: the scannable minimum. Job progress when there is a job,
    // and the current tool.
    if (report.capabilities.reports_job && ! report.job_name.empty()) {
        const Freshness job_freshness =
            freshness_of(report.job_reported_s, now_s, policy.staleness_window_s);
        if (job_freshness != Freshness::Absent) {
            CardField job;
            job.label     = "job";
            job.value     = report.job_name;
            job.freshness = job_freshness;
            if (job_freshness == Freshness::Stale) {
                job.age_s = now_s - *report.job_reported_s;
                any_stale = true;
            }
            card.compact_fields.push_back(job);
        }
        CardMetric percent = report.progress_fraction;
        if (percent.value.has_value())
            percent.value = *percent.value * 100.;
        add_metric_field(card.compact_fields, "progress", percent, "%", 0, now_s,
                         policy.staleness_window_s, any_stale);
        add_metric_field(card.compact_fields, "remaining",
                         { report.remaining_s.value.has_value()
                               ? std::optional<double>(*report.remaining_s.value)
                               : std::nullopt,
                           report.remaining_s.reported_s },
                         "", 0, now_s, policy.staleness_window_s, any_stale);
        // Remaining is a duration, not a bare number; fix its formatting.
        if (! card.compact_fields.empty() &&
            card.compact_fields.back().label == "remaining" &&
            report.remaining_s.value.has_value())
            card.compact_fields.back().value = format_duration(*report.remaining_s.value);
    }
    if (report.current_tool.has_value()) {
        CardField tool;
        tool.label     = "tool";
        tool.value     = "T" + std::to_string(*report.current_tool);
        tool.freshness = card.activity_freshness == Freshness::Absent ? Freshness::Live
                                                                      : card.activity_freshness;
        card.compact_fields.push_back(tool);
    }

    // Detailed fields: temperatures, materials, queue. Each capability-gated.
    if (report.capabilities.reports_temperatures) {
        add_metric_field(card.detailed_fields, "nozzle", report.nozzle_c, " C", 0, now_s,
                         policy.staleness_window_s, any_stale);
        add_metric_field(card.detailed_fields, "bed", report.bed_c, " C", 0, now_s,
                         policy.staleness_window_s, any_stale);
        add_metric_field(card.detailed_fields, "chamber", report.chamber_c, " C", 0, now_s,
                         policy.staleness_window_s, any_stale);
    }
    if (report.capabilities.reports_materials && ! report.loaded_materials.empty()) {
        const Freshness freshness =
            freshness_of(report.materials_reported_s, now_s, policy.staleness_window_s);
        if (freshness != Freshness::Absent) {
            std::string joined;
            for (size_t i = 0; i < report.loaded_materials.size(); ++i) {
                if (i > 0)
                    joined += ", ";
                joined += report.loaded_materials[i];
            }
            CardField materials;
            materials.label     = "materials";
            materials.value     = joined;
            materials.freshness = freshness;
            if (freshness == Freshness::Stale) {
                materials.age_s = now_s - *report.materials_reported_s;
                any_stale       = true;
            }
            card.detailed_fields.push_back(materials);
        }
    }
    if (report.capabilities.reports_queue && report.queue_count.has_value()) {
        const Freshness freshness =
            freshness_of(report.queue_reported_s, now_s, policy.staleness_window_s);
        if (freshness != Freshness::Absent) {
            CardField queue;
            queue.label     = "queue";
            queue.value     = std::to_string(*report.queue_count);
            queue.freshness = freshness;
            if (freshness == Freshness::Stale) {
                queue.age_s = now_s - *report.queue_reported_s;
                any_stale   = true;
            }
            card.detailed_fields.push_back(queue);
        }
    }

    // Camera: shown only when configured, the provider reports it, and the card
    // is visible. A hidden card does not poll the camera.
    if (report.capabilities.reports_camera && report.camera_configured) {
        if (poll_hidden) {
            card.show_camera = false;
            card.polling_suppressed.push_back("camera");
        } else {
            card.show_camera = true;
        }
    }
    if (poll_hidden && report.capabilities.reports_temperatures)
        card.polling_suppressed.push_back("temperatures");

    // The compact card only ever carries the compact fields; a detailed card
    // carries both. The distinction is which the caller renders, so both are
    // populated and density selects.
    (void) density;

    card.has_stale_data = any_stale || card.activity_freshness == Freshness::Stale;
    return card;
}

// --- overview --------------------------------------------------------------

namespace {

OverviewCategory categorize(const PrinterReport &report, bool maintenance_due)
{
    // Attention outranks everything actionable; a faulted printer is
    // NeedsAttention whatever else is true of it. Maintenance-due is its own
    // category so it is not lost inside Ready.
    if (report.activity == PrinterActivity::Error)
        return OverviewCategory::NeedsAttention;
    if (report.activity == PrinterActivity::Offline ||
        report.activity == PrinterActivity::Unknown)
        return OverviewCategory::Offline;
    if (maintenance_due)
        return OverviewCategory::MaintenanceDue;
    if (report.activity == PrinterActivity::Printing)
        return OverviewCategory::Printing;
    if (report.activity == PrinterActivity::Paused)
        return OverviewCategory::Paused;
    if (report.activity == PrinterActivity::Maintenance)
        return OverviewCategory::MaintenanceDue;
    return OverviewCategory::Ready;
}

int rank_of(OverviewCategory category)
{
    // Actionable problems first; offline and quiet printers last but never
    // hidden.
    switch (category) {
    case OverviewCategory::NeedsAttention: return 0;
    case OverviewCategory::MaintenanceDue: return 1;
    case OverviewCategory::Paused:         return 2;
    case OverviewCategory::Printing:       return 3;
    case OverviewCategory::Ready:          return 4;
    case OverviewCategory::Offline:        return 5;
    }
    return 5;
}

} // namespace

PrinterOverview build_overview(const std::vector<PrinterReport>   &reports,
                               const std::vector<MaintenanceFlag> &maintenance,
                               const OverviewPolicy &policy, int64_t now_s)
{
    PrinterOverview overview;

    for (const PrinterReport &report : reports) {
        bool due = false;
        for (const MaintenanceFlag &flag : maintenance)
            if (flag.id == report.id)
                due = flag.due;

        const OverviewCategory category = categorize(report, due);

        OverviewEntry entry;
        entry.id             = report.id;
        entry.category       = category;
        entry.attention_rank = rank_of(category);
        entry.stale =
            freshness_of(report.activity_reported_s, now_s, policy.staleness_window_s) ==
            Freshness::Stale;
        overview.entries.push_back(entry);

        switch (category) {
        case OverviewCategory::Printing:       ++overview.counts.printing; break;
        case OverviewCategory::Ready:          ++overview.counts.ready; break;
        case OverviewCategory::Paused:         ++overview.counts.paused; break;
        case OverviewCategory::Offline:        ++overview.counts.offline; break;
        case OverviewCategory::NeedsAttention: ++overview.counts.needs_attention; break;
        case OverviewCategory::MaintenanceDue: ++overview.counts.maintenance_due; break;
        }

        if (report.capabilities.reports_queue && report.queue_count.has_value())
            overview.counts.queued_jobs += size_t(std::max(0, *report.queue_count));

        if (entry.stale)
            overview.has_stale_data = true;
    }

    overview.counts.total = reports.size();

    // Attention first, then by id, so the order is stable and reproducible.
    std::vector<const OverviewEntry *> sorted;
    for (const OverviewEntry &entry : overview.entries)
        sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(),
              [](const OverviewEntry *a, const OverviewEntry *b) {
                  if (a->attention_rank != b->attention_rank)
                      return a->attention_rank < b->attention_rank;
                  return a->id < b->id;
              });
    for (const OverviewEntry *entry : sorted)
        overview.ordered_ids.push_back(entry->id);

    return overview;
}

std::vector<std::string> filter_overview(const PrinterOverview &overview,
                                         OverviewCategory       category)
{
    std::vector<std::string> ids;
    // Walk in the overview's own order, so a filtered list keeps the
    // attention-first ordering.
    for (const std::string &id : overview.ordered_ids)
        for (const OverviewEntry &entry : overview.entries)
            if (entry.id == id && entry.category == category)
                ids.push_back(id);
    return ids;
}

} // namespace Slic3r
