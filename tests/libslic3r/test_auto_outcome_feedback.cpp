///|/ TigerSlicer Phase AUTO6: local outcome feedback without AI.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/AutoOutcomeFeedback.hpp"

using namespace Slic3r;

namespace {

OutcomeKey key(const std::string &machine, const std::string &material,
               const std::string &nozzle = "0.4")
{
    return { machine, material, nozzle };
}

OutcomeRecord record(const OutcomeKey &k, OutcomeCategory category)
{
    OutcomeRecord r;
    r.key      = k;
    r.category = category;
    r.plan_id  = "plan-1";
    return r;
}

ObjectiveVector fdm_base()
{
    ObjectiveVector v;
    v.objectives = {
        { "quality", ObjectiveDirection::HigherIsBetter, 0.5 },
        { "time_s", ObjectiveDirection::LowerIsBetter, 0.5 },
        { "support_burden", ObjectiveDirection::LowerIsBetter, 0.15 },
    };
    return v;
}

double weight_of(const ObjectiveVector &v, const std::string &key)
{
    for (const AutoObjective &o : v.objectives)
        if (o.key == key)
            return o.weight;
    return -1.;
}

} // namespace

TEST_CASE("AUTO6: a record never stores geometry", "[auto-outcome]")
{
    OutcomeStore store;

    SECTION("a clean record is accepted")
    {
        CHECK(store.add(record(key("MK4", "PLA"), OutcomeCategory::Success)) ==
              RecordError::None);
        CHECK(store.size() == 1);
    }

    SECTION("a record carrying geometry is refused outright")
    {
        OutcomeRecord r = record(key("MK4", "PLA"), OutcomeCategory::Success);
        r.forbidden_geometry = "<a mesh>";
        CHECK(store.add(r) == RecordError::CarriesGeometry);
        CHECK(store.size() == 0);
    }

    SECTION("a record with no machine or material is refused")
    {
        CHECK(store.add(record(key("", "PLA"), OutcomeCategory::Success)) ==
              RecordError::EmptyKey);
    }

    SECTION("a rating outside 1..5 is refused")
    {
        OutcomeRecord r = record(key("MK4", "PLA"), OutcomeCategory::Success);
        r.rating        = 9;
        CHECK(store.add(r) == RecordError::BadRating);
    }
}

TEST_CASE("AUTO6: the user can inspect, export and delete all records", "[auto-outcome]")
{
    OutcomeStore store;
    store.add(record(key("MK4", "PLA"), OutcomeCategory::Success));
    store.add(record(key("MK4", "PLA"), OutcomeCategory::FailureAdhesion));
    store.add(record(key("Saturn", "Grey"), OutcomeCategory::Success));

    CHECK(store.size() == 3);
    CHECK(store.for_key(key("MK4", "PLA")).size() == 2);

    // Export carries counts, never geometry or model references.
    const std::string summary = store.export_summary();
    CHECK_THAT(summary, Catch::Matchers::ContainsSubstring("MK4"));
    CHECK_THAT(summary, Catch::Matchers::ContainsSubstring("records"));

    CHECK(store.clear_key(key("MK4", "PLA")) == 2);
    CHECK(store.size() == 1);
    CHECK(store.clear() == 1);
    CHECK(store.size() == 0);
}

TEST_CASE("AUTO6: disabled feedback leaves planning deterministic", "[auto-outcome]")
{
    OutcomeStore store;
    for (int i = 0; i < 5; ++i)
        store.add(record(key("MK4", "PLA"), OutcomeCategory::FailureAdhesion));

    OutcomeAdjustmentPolicy policy; // enabled defaults to false
    const OutcomeAdjustment a = adjust_objectives(store, key("MK4", "PLA"), fdm_base(), policy);
    CHECK_FALSE(a.applied);
    CHECK(a.adjustments.empty());
    // Applying a non-adjustment returns the base untouched.
    const ObjectiveVector out = apply_adjustment(fdm_base(), a);
    CHECK(weight_of(out, "quality") == 0.5);
}

TEST_CASE("AUTO6: an adjustment is bounded, explainable and scoped to the exact combination",
          "[auto-outcome]")
{
    OutcomeStore store;
    for (int i = 0; i < 4; ++i)
        store.add(record(key("MK4", "PLA"), OutcomeCategory::FailureAdhesion));

    OutcomeAdjustmentPolicy policy;
    policy.enabled          = true;
    policy.max_weight_delta = 0.25;
    policy.min_records      = 3;

    SECTION("adhesion failures nudge towards quality, within the bound")
    {
        const OutcomeAdjustment a =
            adjust_objectives(store, key("MK4", "PLA"), fdm_base(), policy);
        CHECK(a.applied);
        REQUIRE_FALSE(a.adjustments.empty());
        CHECK(a.adjustments.front().objective == "quality");
        CHECK(a.adjustments.front().delta > 0.);
        CHECK(a.adjustments.front().delta <= 0.25);
        CHECK_FALSE(a.adjustments.front().reason.empty());

        const ObjectiveVector out = apply_adjustment(fdm_base(), a);
        CHECK(weight_of(out, "quality") > 0.5);
        CHECK(weight_of(out, "quality") <= 0.5 + 0.25);
    }

    SECTION("the adjustment never crosses to a different combination")
    {
        // No records for this other combination: nothing is borrowed from the
        // MK4/PLA history.
        const OutcomeAdjustment a =
            adjust_objectives(store, key("MK4", "PETG"), fdm_base(), policy);
        CHECK_FALSE(a.applied);
        CHECK_THAT(a.explanation, Catch::Matchers::ContainsSubstring("below the"));
    }

    SECTION("too few records makes no adjustment")
    {
        OutcomeStore few;
        few.add(record(key("MK4", "PLA"), OutcomeCategory::FailureAdhesion));
        const OutcomeAdjustment a = adjust_objectives(few, key("MK4", "PLA"), fdm_base(), policy);
        CHECK_FALSE(a.applied);
    }

    SECTION("a support failure nudges support burden, not quality")
    {
        OutcomeStore supports;
        for (int i = 0; i < 3; ++i)
            supports.add(record(key("MK4", "PLA"), OutcomeCategory::FailureSupport));
        const OutcomeAdjustment a =
            adjust_objectives(supports, key("MK4", "PLA"), fdm_base(), policy);
        REQUIRE(a.applied);
        bool touched_support = false;
        for (const WeightAdjustment &w : a.adjustments)
            if (w.objective == "support_burden")
                touched_support = true;
        CHECK(touched_support);
    }

    SECTION("an adjustment only touches soft objectives the vector actually has")
    {
        // The base has no "peel_risk"; a resin-style failure cannot invent it.
        OutcomeStore resin;
        for (int i = 0; i < 3; ++i)
            resin.add(record(key("Saturn", "Grey", "mono"),
                             OutcomeCategory::FailureExposure));
        const OutcomeAdjustment a =
            adjust_objectives(resin, key("Saturn", "Grey", "mono"), fdm_base(), policy);
        // Exposure failure nudges quality (which exists); it does not add a new
        // objective.
        const ObjectiveVector out = apply_adjustment(fdm_base(), a);
        CHECK(out.objectives.size() == fdm_base().objectives.size());
    }

    SECTION("all-success history adjusts nothing to act on")
    {
        OutcomeStore good;
        for (int i = 0; i < 5; ++i)
            good.add(record(key("MK4", "PLA"), OutcomeCategory::Success));
        const OutcomeAdjustment a = adjust_objectives(good, key("MK4", "PLA"), fdm_base(), policy);
        CHECK_FALSE(a.applied);
    }
}

TEST_CASE("AUTO6: an applied adjustment never drives a weight negative", "[auto-outcome]")
{
    OutcomeStore store;
    for (int i = 0; i < 5; ++i)
        store.add(record(key("Saturn", "Grey", "mono"), OutcomeCategory::FailureSupport));

    OutcomeAdjustmentPolicy policy;
    policy.enabled          = true;
    policy.max_weight_delta = 0.25;

    // A base where support_burden has a tiny weight that a negative nudge would
    // push below zero.
    ObjectiveVector base;
    base.objectives = { { "support_burden", ObjectiveDirection::LowerIsBetter, 0.05 } };

    const OutcomeAdjustment a =
        adjust_objectives(store, key("Saturn", "Grey", "mono"), base, policy);
    const ObjectiveVector out = apply_adjustment(base, a);
    CHECK(weight_of(out, "support_burden") >= 0.);
}
