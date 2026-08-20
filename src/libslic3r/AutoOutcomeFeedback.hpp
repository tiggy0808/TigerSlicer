///|/ TigerSlicer Phase AUTO6: local outcome feedback without AI.
///|/
///|/ Optional local-only outcome records that help a user compare planner
///|/ choices. This trains and runs no model. It stores no geometry, keeps
///|/ everything on the machine, and turns outcomes into bounded, explainable
///|/ soft-weight nudges that apply only to the exact machine/material/resin
///|/ combination they came from - never generalized, never touching a hard
///|/ safety constraint, and off by default so planning stays deterministic.
///|/ Testable libslic3r logic.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_AutoOutcomeFeedback_hpp_
#define slic3r_AutoOutcomeFeedback_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "AutoPlanOptimizer.hpp"

namespace Slic3r {

// How a print turned out. Generic categories, so no per-model detail is
// implied or stored.
enum class OutcomeCategory : uint8_t
{
    Success,
    FailureAdhesion,
    FailureWarping,
    FailureSupport,
    FailureExposure,
    FailureToolchange,
    FailureOther,
};

const char *outcome_category_name(OutcomeCategory category);
bool        outcome_is_success(OutcomeCategory category);

// The exact combination an outcome belongs to. Adjustments never cross a
// combination boundary without a reviewed compatibility rule, so the key is
// what scopes them.
struct OutcomeKey
{
    std::string machine;
    std::string material;   // filament or resin
    std::string nozzle_or_screen;

    bool operator==(const OutcomeKey &o) const
    {
        return machine == o.machine && material == o.material &&
               nozzle_or_screen == o.nozzle_or_screen;
    }
    bool operator<(const OutcomeKey &o) const;
    std::string to_string() const;
};

// One local outcome record. It carries no model geometry - that is a hard rule,
// and add() refuses a record that violates it.
struct OutcomeRecord
{
    OutcomeKey      key;
    OutcomeCategory category { OutcomeCategory::Success };
    // The user's rating, 1..5, or absent.
    std::optional<int> rating;
    // Which planner choice produced this print, for comparison. An id, not
    // geometry.
    std::string plan_id;
    // Caller-supplied timestamp; nothing here reads a clock.
    int64_t recorded_s { 0 };

    // Must be empty. A non-empty value is geometry that must never be stored,
    // and add() rejects the record.
    std::string forbidden_geometry;
};

enum class RecordError : uint8_t
{
    None,
    CarriesGeometry,   // the record carried geometry; refused
    EmptyKey,
    BadRating,
};

const char *record_error_name(RecordError error);

// The local store. Everything is inspectable, exportable and deletable by the
// user; nothing leaves the machine.
class OutcomeStore
{
public:
    // Add a record. Refuses one that carries geometry, has no key, or has a
    // rating outside 1..5.
    RecordError add(const OutcomeRecord &record);

    // Every record, in insertion order.
    const std::vector<OutcomeRecord> &records() const { return m_records; }
    // Records for one exact combination.
    std::vector<OutcomeRecord> for_key(const OutcomeKey &key) const;

    size_t size() const { return m_records.size(); }

    // Delete every record (the user's right to erase). Returns how many went.
    size_t clear();
    // Delete every record for one combination.
    size_t clear_key(const OutcomeKey &key);

    // A serializable export - counts and ratings only, never geometry.
    std::string export_summary() const;

private:
    std::vector<OutcomeRecord> m_records;
};

// --- bounded scoring adjustment --------------------------------------------

struct OutcomeAdjustmentPolicy
{
    // Off by default: planning is deterministic and history-free unless the
    // user turns this on.
    bool enabled { false };

    // The most an adjustment may move a soft weight, up or down. A local
    // history can nudge preferences; it can never dominate them.
    double max_weight_delta { 0.25 };

    // How many records of a combination are needed before any adjustment is
    // made. Below this, one bad print does not reshape planning.
    size_t min_records { 3 };
};

struct WeightAdjustment
{
    std::string objective;
    double      delta { 0. };   // added to the soft weight, within the bound
    std::string reason;         // explainable: why this nudge
};

struct OutcomeAdjustment
{
    // True when the policy is enabled and there is enough history for this
    // exact combination; false leaves objectives untouched.
    bool applied { false };
    std::vector<WeightAdjustment> adjustments;
    std::string                   explanation;
};

// Compute a bounded, explainable soft-weight adjustment for one combination
// from its local records.
//
// Only the records for this exact combination are used - a result is never
// generalized to another machine, material or nozzle. The adjustment is
// bounded by the policy, is explainable, and only ever nudges soft objective
// weights: it cannot touch a hard constraint. When the policy is disabled, or
// there are too few records, nothing is adjusted and planning stays
// deterministic.
OutcomeAdjustment adjust_objectives(const OutcomeStore &store, const OutcomeKey &key,
                                    const ObjectiveVector         &base,
                                    const OutcomeAdjustmentPolicy &policy);

// Apply an adjustment to an objective vector, clamping every weight to be
// non-negative. Returns the adjusted vector; the input is not modified.
ObjectiveVector apply_adjustment(const ObjectiveVector &base, const OutcomeAdjustment &adjustment);

} // namespace Slic3r

#endif // slic3r_AutoOutcomeFeedback_hpp_
