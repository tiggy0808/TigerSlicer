// Phase 16: the directional material-transition matrix. The property the
// whole phase exists for is that A -> B is not B -> A: putting white after
// black costs far more than putting black after white, and a matrix that
// mirrors its rules would quietly halve one of them. The rest is the usual
// discipline - uncharacterized never reads as compatible, an incompatible pair
// is a first-class fact that blocks export, and a transition that cannot be
// priced does not contribute a confident zero to the total.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/TransitionMatrix.hpp"

using namespace Slic3r;

namespace {

TransitionEndpoint at(int tool, const std::string &material, const std::string &color)
{
    TransitionEndpoint endpoint;
    endpoint.tool     = tool;
    endpoint.material = material;
    endpoint.color    = color;
    return endpoint;
}

TransitionEndpoint pattern(const std::string &material, const std::string &color)
{
    TransitionEndpoint endpoint;
    endpoint.material = material;
    endpoint.color    = color;
    return endpoint;
}

TransitionRule rule(const TransitionEndpoint &from, const TransitionEndpoint &to,
                    TransitionCompatibility compatibility, double purge)
{
    TransitionRule value;
    value.from             = from;
    value.to               = to;
    value.compatibility    = compatibility;
    value.purge_volume_mm3 = purge;
    value.prime_volume_mm3 = 5.;
    return value;
}

TransitionMatrix matrix()
{
    TransitionMatrix value;
    // Black into white needs a long purge; white into black barely any. This
    // asymmetry is the whole point of the phase.
    value.rules.push_back(rule(pattern("PLA", "black"), pattern("PLA", "white"),
                               TransitionCompatibility::RequiresExtraPurge, 200.));
    value.rules.push_back(rule(pattern("PLA", "white"), pattern("PLA", "black"),
                               TransitionCompatibility::Compatible, 20.));
    value.timing.purge_rate_mm3_s = 10.;
    value.timing.brush_pass_s     = 2.;
    value.timing.heating_rate_c_s = 1.;
    value.timing.cooling_rate_c_s = 0.5;
    return value;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("A transition costs what its own direction says, not the reverse",
          "[transition-matrix]")
{
    const TransitionMatrix rules = matrix();

    const TransitionCost forward =
        resolve_transition(rules, at(0, "PLA", "black"), at(1, "PLA", "white"), 210.);
    const TransitionCost backward =
        resolve_transition(rules, at(1, "PLA", "white"), at(0, "PLA", "black"), 210.);

    CHECK(forward.purge_volume_mm3 == Catch::Approx(200.));
    CHECK(backward.purge_volume_mm3 == Catch::Approx(20.));
    CHECK(forward.compatibility == TransitionCompatibility::RequiresExtraPurge);
    CHECK(backward.compatibility == TransitionCompatibility::Compatible);
}

TEST_CASE("Declaring one direction does not imply the other", "[transition-matrix]")
{
    TransitionMatrix rules;
    rules.rules.push_back(rule(pattern("ABS", ""), pattern("PC", ""),
                               TransitionCompatibility::Compatible, 40.));

    // The declared direction resolves.
    CHECK(resolve_transition(rules, at(0, "ABS", ""), at(1, "PC", ""), 250.).compatibility ==
          TransitionCompatibility::Compatible);

    // The reverse was never stated, so it is uncharacterized - not mirrored.
    const TransitionCost reverse =
        resolve_transition(rules, at(1, "PC", ""), at(0, "ABS", ""), 250.);
    CHECK(reverse.compatibility == TransitionCompatibility::Unknown);
    CHECK(mentions(reverse.reasons, "nothing in the matrix describes"));
}

TEST_CASE("The most specific matching rule wins", "[transition-matrix]")
{
    TransitionMatrix rules = matrix();
    // A rule about one physical tool beats the material-and-colour rule.
    TransitionRule specific = rule(at(2, "PLA", "black"), at(3, "PLA", "white"),
                                   TransitionCompatibility::Compatible, 60.);
    rules.rules.push_back(specific);

    const TransitionCost on_those_tools =
        resolve_transition(rules, at(2, "PLA", "black"), at(3, "PLA", "white"), 210.);
    CHECK(on_those_tools.purge_volume_mm3 == Catch::Approx(60.));
    CHECK(on_those_tools.provenance.find("on T2") != std::string::npos);

    // Any other pair of tools still gets the general rule.
    CHECK(resolve_transition(rules, at(0, "PLA", "black"), at(1, "PLA", "white"), 210.)
              .purge_volume_mm3 == Catch::Approx(200.));
}

TEST_CASE("A tie between equally specific rules is reported, not hidden",
          "[transition-matrix]")
{
    TransitionMatrix rules;
    rules.rules.push_back(rule(pattern("PLA", "black"), pattern("PLA", "white"),
                               TransitionCompatibility::Compatible, 100.));
    rules.rules.push_back(rule(pattern("PLA", "black"), pattern("PLA", "white"),
                               TransitionCompatibility::Compatible, 300.));

    const TransitionCost cost =
        resolve_transition(rules, at(0, "PLA", "black"), at(1, "PLA", "white"), 210.);
    CHECK(cost.purge_volume_mm3 == Catch::Approx(100.));
    CHECK(mentions(cost.reasons, "equally specific"));
}

TEST_CASE("A sensitive destination is purged harder", "[transition-matrix]")
{
    TransitionMatrix rules;
    TransitionRule   sensitive = rule(pattern("PLA", "black"), pattern("PLA", "white"),
                                      TransitionCompatibility::Compatible, 100.);
    sensitive.contamination_sensitivity = 0.75;
    rules.rules.push_back(sensitive);

    const TransitionCost cost =
        resolve_transition(rules, at(0, "PLA", "black"), at(1, "PLA", "white"), 210.);
    CHECK(cost.purge_volume_mm3 == Catch::Approx(175.));
    CHECK(mentions(cost.reasons, "tolerates little contamination"));
}

TEST_CASE("A temperature transition is measured from where the source actually was",
          "[transition-matrix]")
{
    TransitionMatrix rules;
    TransitionRule   hotter = rule(pattern("PLA", ""), pattern("ABS", ""),
                                   TransitionCompatibility::Compatible, 50.);
    hotter.destination_temperature_c = 250.;
    hotter.standby                   = StandbyBehavior::MustDropToStandby;
    rules.rules.push_back(hotter);
    rules.timing.purge_rate_mm3_s = 10.;
    rules.timing.heating_rate_c_s = 1.;

    const TransitionCost cost =
        resolve_transition(rules, at(0, "PLA", ""), at(1, "ABS", ""), 210.);
    REQUIRE(cost.temperature_change_c.has_value());
    CHECK(*cost.temperature_change_c == Catch::Approx(40.));
    CHECK(cost.standby == StandbyBehavior::MustDropToStandby);
    // 55 mm3 at 10 mm3/s plus 40 C at 1 C/s.
    REQUIRE(cost.estimated_time_s.has_value());
    CHECK(*cost.estimated_time_s == Catch::Approx(45.5));
}

TEST_CASE("A transition with no configured rate is unpriced, not free",
          "[transition-matrix]")
{
    TransitionMatrix rules = matrix();
    rules.timing           = TransitionTimingModel {};

    const TransitionCost cost =
        resolve_transition(rules, at(0, "PLA", "black"), at(1, "PLA", "white"), 210.);
    CHECK_FALSE(cost.estimated_time_s.has_value());
    CHECK(mentions(cost.reasons, "cannot be priced"));
}

TEST_CASE("An unmatched transition is uncharacterized, never compatible",
          "[transition-matrix]")
{
    const TransitionCost cost =
        resolve_transition(matrix(), at(0, "TPU", "clear"), at(1, "PC", "grey"), 230.);
    CHECK(cost.compatibility == TransitionCompatibility::Unknown);
    CHECK(cost.blocks_export() == false);
    CHECK(cost.provenance.find("fallback") != std::string::npos);
}

TEST_CASE("An incompatible transition blocks export and names its reason",
          "[transition-matrix]")
{
    TransitionMatrix rules  = matrix();
    TransitionRule   banned = rule(pattern("PLA", ""), pattern("PC", ""),
                                   TransitionCompatibility::Incompatible, 0.);
    banned.reason = "PLA residue chars at PC printing temperature";
    rules.rules.push_back(banned);

    const std::vector<TransitionEndpoint> sequence { at(0, "PLA", "black"),
                                                     at(1, "PC", "grey") };
    const TransitionValidation validation =
        validate_transitions(rules, sequence, { 210., 280. }, TransitionPolicy {});

    CHECK_FALSE(validation.may_export());
    CHECK(mentions(validation.blocking, "chars at PC printing temperature"));
}

TEST_CASE("An incompatible transition with no recorded reason still says so",
          "[transition-matrix]")
{
    TransitionMatrix rules;
    rules.rules.push_back(rule(pattern("PLA", ""), pattern("PC", ""),
                               TransitionCompatibility::Incompatible, 0.));

    const TransitionCost cost =
        resolve_transition(rules, at(0, "PLA", ""), at(1, "PC", ""), 210.);
    CHECK(cost.blocks_export());
    CHECK(mentions(cost.reasons, "no reason recorded"));
}

TEST_CASE("An uncharacterized transition blocks export unless it is allowed",
          "[transition-matrix]")
{
    const std::vector<TransitionEndpoint> sequence { at(0, "TPU", "clear"),
                                                     at(1, "PC", "grey") };

    SECTION("by default it blocks") {
        const TransitionValidation validation =
            validate_transitions(matrix(), sequence, {}, TransitionPolicy {});
        CHECK_FALSE(validation.may_export());
        CHECK(mentions(validation.blocking, "has not been characterized"));
    }

    SECTION("explicitly allowed, it warns loudly instead") {
        TransitionPolicy policy;
        policy.allow_uncharacterized = true;
        const TransitionValidation validation =
            validate_transitions(matrix(), sequence, {}, policy);
        CHECK(validation.may_export());
        CHECK(mentions(validation.warnings, "explicitly allowed"));
    }
}

TEST_CASE("A sequence accumulates purge, prime and time", "[transition-matrix]")
{
    const std::vector<TransitionEndpoint> sequence {
        at(0, "PLA", "white"),
        at(1, "PLA", "black"),
        at(0, "PLA", "white"),
    };

    const TransitionValidation validation =
        validate_transitions(matrix(), sequence, { 210., 210., 210. }, TransitionPolicy {});

    REQUIRE(validation.may_export());
    REQUIRE(validation.costs.size() == 2);
    // 20 mm3 one way, 200 the other; the asymmetry survives into the total.
    CHECK(validation.total_purge_mm3 == Catch::Approx(220.));
    CHECK(validation.total_prime_mm3 == Catch::Approx(10.));
    REQUIRE(validation.total_time_s.has_value());
    CHECK(*validation.total_time_s == Catch::Approx(23.));
    CHECK(mentions(validation.warnings, "needs 200.0 mm3 of purge"));
}

TEST_CASE("Staying on the same material is not a transition", "[transition-matrix]")
{
    const std::vector<TransitionEndpoint> sequence {
        at(0, "PLA", "white"),
        at(0, "PLA", "white"),
        at(0, "PLA", "white"),
    };
    const TransitionValidation validation =
        validate_transitions(matrix(), sequence, {}, TransitionPolicy {});
    CHECK(validation.costs.empty());
    CHECK(validation.total_purge_mm3 == Catch::Approx(0.));
    CHECK(validation.may_export());
}

TEST_CASE("One unpriced transition withholds the whole total", "[transition-matrix]")
{
    TransitionMatrix rules = matrix();
    rules.timing.purge_rate_mm3_s.reset();

    const std::vector<TransitionEndpoint> sequence { at(0, "PLA", "white"),
                                                     at(1, "PLA", "black") };
    const TransitionValidation validation =
        validate_transitions(rules, sequence, {}, TransitionPolicy {});

    // A partial total would understate the print.
    CHECK_FALSE(validation.total_time_s.has_value());
    CHECK(mentions(validation.warnings, "no total transition time is reported"));
}

TEST_CASE("A temperature list that does not match the sequence is refused",
          "[transition-matrix]")
{
    const std::vector<TransitionEndpoint> sequence { at(0, "PLA", "white"),
                                                     at(1, "PLA", "black") };
    const TransitionValidation validation =
        validate_transitions(matrix(), sequence, { 210. }, TransitionPolicy {});

    CHECK_FALSE(validation.may_export());
    CHECK(mentions(validation.blocking, "cannot be priced"));
}

TEST_CASE("A single-state sequence has nothing to transition", "[transition-matrix]")
{
    const TransitionValidation validation =
        validate_transitions(matrix(), { at(0, "PLA", "white") }, {}, TransitionPolicy {});
    CHECK(validation.may_export());
    REQUIRE(validation.total_time_s.has_value());
    CHECK(*validation.total_time_s == Catch::Approx(0.));
}
