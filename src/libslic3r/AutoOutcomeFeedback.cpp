///|/ TigerSlicer Phase AUTO6: local outcome feedback without AI.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "AutoOutcomeFeedback.hpp"

#include <algorithm>
#include <sstream>

namespace Slic3r {

const char *outcome_category_name(OutcomeCategory category)
{
    switch (category) {
    case OutcomeCategory::Success:           return "success";
    case OutcomeCategory::FailureAdhesion:   return "adhesion failure";
    case OutcomeCategory::FailureWarping:    return "warping failure";
    case OutcomeCategory::FailureSupport:    return "support failure";
    case OutcomeCategory::FailureExposure:   return "exposure failure";
    case OutcomeCategory::FailureToolchange: return "toolchange failure";
    case OutcomeCategory::FailureOther:      return "other failure";
    }
    return "other failure";
}

bool outcome_is_success(OutcomeCategory category)
{
    return category == OutcomeCategory::Success;
}

const char *record_error_name(RecordError error)
{
    switch (error) {
    case RecordError::None:            return "none";
    case RecordError::CarriesGeometry: return "carries geometry";
    case RecordError::EmptyKey:        return "empty key";
    case RecordError::BadRating:       return "bad rating";
    }
    return "none";
}

bool OutcomeKey::operator<(const OutcomeKey &o) const
{
    if (machine != o.machine)
        return machine < o.machine;
    if (material != o.material)
        return material < o.material;
    return nozzle_or_screen < o.nozzle_or_screen;
}

std::string OutcomeKey::to_string() const
{
    return machine + " / " + material + " / " + nozzle_or_screen;
}

// --- store ------------------------------------------------------------------

RecordError OutcomeStore::add(const OutcomeRecord &record)
{
    // Geometry must never be stored. This is checked first and is absolute.
    if (! record.forbidden_geometry.empty())
        return RecordError::CarriesGeometry;
    if (record.key.machine.empty() || record.key.material.empty())
        return RecordError::EmptyKey;
    if (record.rating.has_value() && (*record.rating < 1 || *record.rating > 5))
        return RecordError::BadRating;

    m_records.push_back(record);
    return RecordError::None;
}

std::vector<OutcomeRecord> OutcomeStore::for_key(const OutcomeKey &key) const
{
    std::vector<OutcomeRecord> out;
    for (const OutcomeRecord &record : m_records)
        if (record.key == key)
            out.push_back(record);
    return out;
}

size_t OutcomeStore::clear()
{
    const size_t n = m_records.size();
    m_records.clear();
    return n;
}

size_t OutcomeStore::clear_key(const OutcomeKey &key)
{
    const size_t before = m_records.size();
    m_records.erase(std::remove_if(m_records.begin(), m_records.end(),
                                   [&key](const OutcomeRecord &r) { return r.key == key; }),
                    m_records.end());
    return before - m_records.size();
}

std::string OutcomeStore::export_summary() const
{
    // Counts and ratings only - never geometry, never a model reference.
    std::map<std::string, std::pair<size_t, size_t>> by_key; // key -> (records, successes)
    for (const OutcomeRecord &record : m_records) {
        auto &entry = by_key[record.key.to_string()];
        ++entry.first;
        if (outcome_is_success(record.category))
            ++entry.second;
    }
    std::ostringstream out;
    for (const auto &entry : by_key)
        out << entry.first << "\t" << entry.second.first << " records, "
            << entry.second.second << " successes\n";
    return out.str();
}

// --- adjustment -------------------------------------------------------------

namespace {

bool has_objective(const ObjectiveVector &v, const std::string &key)
{
    for (const AutoObjective &o : v.objectives)
        if (o.key == key)
            return true;
    return false;
}

} // namespace

OutcomeAdjustment adjust_objectives(const OutcomeStore &store, const OutcomeKey &key,
                                    const ObjectiveVector         &base,
                                    const OutcomeAdjustmentPolicy &policy)
{
    OutcomeAdjustment adjustment;

    // Disabled: nothing is adjusted, planning stays deterministic.
    if (! policy.enabled) {
        adjustment.explanation = "local outcome adjustment is disabled; planning is deterministic";
        return adjustment;
    }

    // Only this exact combination's records - never generalized across a
    // machine, material or nozzle boundary.
    const std::vector<OutcomeRecord> records = store.for_key(key);
    if (records.size() < policy.min_records) {
        adjustment.explanation = "only " + std::to_string(records.size()) +
                                 " record(s) for " + key.to_string() + "; below the " +
                                 std::to_string(policy.min_records) +
                                 " needed before any adjustment";
        return adjustment;
    }

    // Count failure modes. Each maps to a bounded nudge of a soft objective the
    // vector actually has, and only that. Adhesion/warping failures nudge the
    // plan towards quality (away from raw speed); support failures nudge
    // towards support burden being taken more seriously.
    size_t adhesion = 0, warping = 0, support = 0, exposure = 0, total_failures = 0;
    for (const OutcomeRecord &record : records) {
        switch (record.category) {
        case OutcomeCategory::FailureAdhesion: ++adhesion; ++total_failures; break;
        case OutcomeCategory::FailureWarping:  ++warping; ++total_failures; break;
        case OutcomeCategory::FailureSupport:  ++support; ++total_failures; break;
        case OutcomeCategory::FailureExposure: ++exposure; ++total_failures; break;
        case OutcomeCategory::FailureToolchange:
        case OutcomeCategory::FailureOther:    ++total_failures; break;
        case OutcomeCategory::Success:         break;
        }
    }

    const double fraction = double(total_failures) / double(records.size());
    // The nudge scales with the failure fraction but is hard-bounded.
    const double magnitude = std::min(policy.max_weight_delta, policy.max_weight_delta * fraction);

    auto clamp_delta = [&](double d) {
        return std::max(-policy.max_weight_delta, std::min(policy.max_weight_delta, d));
    };
    auto nudge = [&](const std::string &objective, double delta, const std::string &reason) {
        if (! has_objective(base, objective) || std::abs(delta) < 1e-9)
            return;
        // Merge into any existing adjustment for the same objective and re-clamp
        // the combined total. apply_adjustment sums every matching delta, so
        // clamping each entry in isolation would let two nudges to the same
        // objective (e.g. adhesion and exposure both raising "quality") push the
        // weight past max_weight_delta, which the policy forbids.
        for (WeightAdjustment &existing : adjustment.adjustments)
            if (existing.objective == objective) {
                existing.delta   = clamp_delta(existing.delta + delta);
                existing.reason += "; " + reason;
                return;
            }
        WeightAdjustment w;
        w.objective = objective;
        w.delta     = clamp_delta(delta);
        w.reason    = reason;
        adjustment.adjustments.push_back(w);
    };

    if (adhesion + warping > 0)
        nudge("quality", magnitude,
              std::to_string(adhesion + warping) +
                  " adhesion/warping failure(s) here; nudging towards quality over speed");
    if (support > 0)
        // support_burden is LowerIsBetter and scored as -raw*abs(weight), so a
        // LARGER weight penalizes high support burden MORE. To take support
        // failures more seriously the weight must increase: use +magnitude (a
        // negative delta would drive it toward zero and do the opposite).
        nudge("support_burden", magnitude,
              std::to_string(support) +
                  " support failure(s) here; taking support burden more seriously");
    if (exposure > 0)
        nudge("quality", magnitude * 0.5,
              std::to_string(exposure) + " exposure failure(s) here");

    adjustment.applied = ! adjustment.adjustments.empty();
    adjustment.explanation =
        adjustment.applied
            ? "adjusted from " + std::to_string(records.size()) + " local record(s) for " +
                  key.to_string() + ", bounded to " + std::to_string(policy.max_weight_delta)
            : "enough records, but no failure pattern to act on";
    return adjustment;
}

ObjectiveVector apply_adjustment(const ObjectiveVector &base, const OutcomeAdjustment &adjustment)
{
    ObjectiveVector out = base;
    if (! adjustment.applied)
        return out;
    for (const WeightAdjustment &w : adjustment.adjustments)
        for (AutoObjective &objective : out.objectives)
            if (objective.key == w.objective) {
                objective.weight += w.delta;
                if (objective.weight < 0.)
                    objective.weight = 0.;
            }
    return out;
}

} // namespace Slic3r
