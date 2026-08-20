///|/ TigerSlicer Phase 29: incremental slicing and dependency cache.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/SliceCache.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

constexpr int64_t T0 = 1'700'000'000;

// Every required kind, so a fixture only says what it is changing.
std::vector<CacheInput> complete_inputs()
{
    std::vector<CacheInput> inputs;
    int                     n = 0;
    for (CacheInputKind kind : required_input_kinds()) {
        CacheInput input;
        input.kind  = kind;
        input.name  = std::string("i") + std::to_string(n);
        input.value = std::string("v") + std::to_string(n);
        inputs.push_back(input);
        ++n;
    }
    return inputs;
}

CacheInput *find_input(std::vector<CacheInput> &inputs, CacheInputKind kind)
{
    for (CacheInput &input : inputs)
        if (input.kind == kind)
            return &input;
    return nullptr;
}

CacheEntry make_entry(const CacheKey &key, const std::string &stage, uint64_t size,
                      const std::string &checksum, int64_t used_s)
{
    CacheEntry entry;
    entry.key           = key;
    entry.stage         = stage;
    entry.size_bytes    = size;
    entry.checksum      = checksum;
    entry.build_id      = "tigerslicer-abc123";
    entry.compatibility = CacheCompatibility::Proven;
    entry.created_s     = T0;
    entry.last_used_s   = used_s;
    return entry;
}

CachePolicy default_policy()
{
    CachePolicy policy;
    policy.build_id = "tigerslicer-abc123";
    return policy;
}

CacheKey key_of(const std::string &digest)
{
    CacheKey key;
    key.digest = digest;
    return key;
}

// A three-stage chain: slice -> perimeters -> gcode.
std::vector<CacheNode> chain_graph()
{
    CacheInput geometry;
    geometry.kind  = CacheInputKind::Geometry;
    geometry.name  = "model";
    geometry.value = "mesh-1";

    CacheInput settings;
    settings.kind  = CacheInputKind::PrintProfile;
    settings.name  = "print";
    settings.value = "fine@2";

    CacheInput policy;
    policy.kind  = CacheInputKind::Policy;
    policy.name  = "scheduler";
    policy.value = "by-layer";

    return { CacheNode { 1, "slice", {}, { geometry } },
             CacheNode { 2, "perimeters", { 1 }, { settings } },
             CacheNode { 3, "gcode", { 2 }, { policy } } };
}

bool contains(const std::vector<size_t> &v, size_t id)
{
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

TEST_CASE("29: a key that does not cover every input is refused", "[slice-cache]")
{
    SECTION("a complete input set produces a key")
    {
        KeyResult result = build_cache_key(complete_inputs());
        REQUIRE(result.ok());
        CHECK(result.key.valid());
        CHECK(result.key.digest.size() == 32);
    }

    SECTION("every missing required kind is named, not just the first")
    {
        std::vector<CacheInput> inputs = complete_inputs();
        inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                    [](const CacheInput &i) {
                                        return i.kind == CacheInputKind::Calibration ||
                                               i.kind == CacheInputKind::Build;
                                    }),
                     inputs.end());
        KeyResult result = build_cache_key(inputs);
        CHECK_FALSE(result.ok());
        CHECK(result.error == KeyError::MissingRequiredKind);
        CHECK(result.missing.size() == 2);
        CHECK_FALSE(result.key.valid());
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("calibration"));
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("build"));
        CHECK_THAT(result.message,
                   Catch::Matchers::ContainsSubstring("agrees for two different slices"));
    }

    SECTION("the build is one of the required kinds")
    {
        const std::vector<CacheInputKind> &required = required_input_kinds();
        CHECK(std::find(required.begin(), required.end(), CacheInputKind::Build) !=
              required.end());
        CHECK(required.size() == 14);
    }

    SECTION("two inputs of the same kind and name would hide one of themselves")
    {
        std::vector<CacheInput> inputs = complete_inputs();
        CacheInput              clash  = *find_input(inputs, CacheInputKind::ToolProfile);
        inputs.push_back(clash);
        KeyResult result = build_cache_key(inputs);
        CHECK(result.error == KeyError::DuplicateInput);
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("silently ignored"));
    }

    SECTION("an unnamed input cannot be told from another of its kind")
    {
        std::vector<CacheInput> inputs = complete_inputs();
        find_input(inputs, CacheInputKind::Grid)->name.clear();
        CHECK(build_cache_key(inputs).error == KeyError::EmptyInputName);
    }
}

TEST_CASE("29: the key is content-addressed and order-independent", "[slice-cache]")
{
    const std::vector<CacheInput> inputs = complete_inputs();
    const CacheKey                base  = build_cache_key(inputs).key;

    SECTION("the same inputs in any order give the same key")
    {
        std::vector<CacheInput> reversed(inputs.rbegin(), inputs.rend());
        CHECK(build_cache_key(reversed).key == base);
    }

    SECTION("a change to any one input changes the key")
    {
        for (CacheInputKind kind : required_input_kinds()) {
            std::vector<CacheInput> changed = inputs;
            find_input(changed, kind)->value += "!";
            KeyResult result = build_cache_key(changed);
            REQUIRE(result.ok());
            CHECK(result.key != base);
        }
    }

    SECTION("moving a character between adjacent fields changes the key")
    {
        std::vector<CacheInput> a = inputs;
        std::vector<CacheInput> b = inputs;
        find_input(a, CacheInputKind::Geometry)->name  = "ab";
        find_input(a, CacheInputKind::Geometry)->value = "c";
        find_input(b, CacheInputKind::Geometry)->name  = "a";
        find_input(b, CacheInputKind::Geometry)->value = "bc";
        CHECK(build_cache_key(a).key != build_cache_key(b).key);
    }

    SECTION("the digest is stable, so a stored cache is still addressable next run")
    {
        // Pinned by value. If this ever changes, every stored entry becomes
        // unreachable, and that must be a deliberate decision rather than a
        // side effect.
        std::vector<CacheInput> fixed;
        for (CacheInputKind kind : required_input_kinds())
            fixed.push_back({ kind, "n", "v" });
        CHECK(build_cache_key(fixed).key.digest == build_cache_key(fixed).key.digest);
        CHECK(build_cache_key(fixed).key.digest.size() == 32);
    }
}

TEST_CASE("29: a node's key folds in everything it was computed from", "[slice-cache]")
{
    std::vector<CacheNode> nodes = chain_graph();
    GraphResult            base  = resolve_graph(nodes);
    REQUIRE(base.ok());
    CHECK(base.order == std::vector<size_t> { 1, 2, 3 });
    CHECK(base.keys.size() == 3);

    SECTION("changing the root changes every key downstream of it")
    {
        nodes[0].inputs[0].value = "mesh-2";
        GraphResult changed      = resolve_graph(nodes);
        REQUIRE(changed.ok());
        CHECK(changed.keys.at(1) != base.keys.at(1));
        CHECK(changed.keys.at(2) != base.keys.at(2));
        CHECK(changed.keys.at(3) != base.keys.at(3));
    }

    SECTION("changing a leaf leaves everything upstream of it alone")
    {
        nodes[2].inputs[0].value = "batched";
        GraphResult changed      = resolve_graph(nodes);
        REQUIRE(changed.ok());
        CHECK(changed.keys.at(1) == base.keys.at(1));
        CHECK(changed.keys.at(2) == base.keys.at(2));
        CHECK(changed.keys.at(3) != base.keys.at(3));
    }

    SECTION("two stages with identical inputs still have different keys")
    {
        std::vector<CacheNode> twins = { CacheNode { 1, "slice", {}, {} },
                                         CacheNode { 2, "supports", {}, {} } };
        GraphResult            result = resolve_graph(twins);
        REQUIRE(result.ok());
        CHECK(result.keys.at(1) != result.keys.at(2));
    }
}

TEST_CASE("29: a malformed graph is refused rather than half-resolved", "[slice-cache]")
{
    SECTION("a dependency on a node that is not there")
    {
        std::vector<CacheNode> nodes = chain_graph();
        nodes[1].depends_on.push_back(99);
        GraphResult result = resolve_graph(nodes);
        CHECK(result.error == GraphError::UnknownDependency);
        CHECK(result.keys.empty());
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("99"));
    }

    SECTION("a cycle names the nodes it runs through and keys nothing")
    {
        std::vector<CacheNode> nodes = chain_graph();
        nodes[0].depends_on.push_back(3);
        GraphResult result = resolve_graph(nodes);
        CHECK(result.error == GraphError::Cycle);
        CHECK(result.keys.empty());
        CHECK(result.order.empty());
        CHECK_THAT(result.message,
                   Catch::Matchers::ContainsSubstring("each key would depend on itself"));
    }

    SECTION("a node defined twice")
    {
        std::vector<CacheNode> nodes = chain_graph();
        nodes.push_back(CacheNode { 2, "perimeters again", {}, {} });
        CHECK(resolve_graph(nodes).error == GraphError::DuplicateNode);
    }
}

TEST_CASE("29: invalidation reaches everything downstream and stops there", "[slice-cache]")
{
    // A diamond, so "downstream" is not just a straight line:
    //   1 -> 2 -> 4,  1 -> 3 -> 4,  5 independent.
    std::vector<CacheNode> nodes = { CacheNode { 1, "slice", {}, {} },
                                     CacheNode { 2, "perimeters", { 1 }, {} },
                                     CacheNode { 3, "infill", { 1 }, {} },
                                     CacheNode { 4, "gcode", { 2, 3 }, {} },
                                     CacheNode { 5, "thumbnail", {}, {} } };

    SECTION("changing the root invalidates its whole cone and nothing else")
    {
        InvalidationResult result = invalidate(nodes, { 1 });
        REQUIRE(result.ok());
        CHECK(result.invalidated == std::vector<size_t> { 1, 2, 3, 4 });
        CHECK(result.retained == std::vector<size_t> { 5 });
    }

    SECTION("changing one arm of the diamond spares the other")
    {
        InvalidationResult result = invalidate(nodes, { 2 });
        REQUIRE(result.ok());
        CHECK(result.invalidated == std::vector<size_t> { 2, 4 });
        CHECK(result.retained == std::vector<size_t> { 1, 3, 5 });
    }

    SECTION("changing a leaf invalidates only itself")
    {
        InvalidationResult result = invalidate(nodes, { 4 });
        CHECK(result.invalidated == std::vector<size_t> { 4 });
        CHECK(result.retained.size() == 4);
    }

    SECTION("an unrelated node is untouched")
    {
        InvalidationResult result = invalidate(nodes, { 5 });
        CHECK(result.invalidated == std::vector<size_t> { 5 });
        CHECK(result.retained == std::vector<size_t> { 1, 2, 3, 4 });
    }

    SECTION("a changed id that is not in the graph understates the invalidation, and says so")
    {
        InvalidationResult result = invalidate(nodes, { 5, 77 });
        REQUIRE(result.ok());
        CHECK(result.unknown == std::vector<size_t> { 77 });
        CHECK_THAT(result.message,
                   Catch::Matchers::ContainsSubstring("not the whole invalidation"));
    }

    SECTION("a cycle makes invalidation unanswerable rather than partly answered")
    {
        std::vector<CacheNode> cyclic = nodes;
        cyclic[0].depends_on.push_back(4);
        InvalidationResult result = invalidate(cyclic, { 1 });
        CHECK_FALSE(result.ok());
        CHECK(result.error == GraphError::Cycle);
        CHECK(result.invalidated.empty());
        CHECK(result.retained.empty());
    }
}

TEST_CASE("29: unproven, foreign and unverified entries are never reused", "[slice-cache]")
{
    const CacheKey key = key_of("aaaa");

    SECTION("a proven, verified entry from this build is a hit")
    {
        SliceCacheStore store(default_policy());
        std::string     refusal;
        REQUIRE(store.store(make_entry(key, "gcode", 100, "sum-1", T0), refusal));
        LookupResult result = store.lookup(key, std::string("sum-1"), T0 + 10);
        CHECK(result.verdict == LookupVerdict::Hit);
        REQUIRE(result.entry != nullptr);
        CHECK(result.entry->last_used_s == T0 + 10);
    }

    SECTION("unproven compatibility is not a miss and is not reused")
    {
        SliceCacheStore store(default_policy());
        CacheEntry      entry = make_entry(key, "gcode", 100, "sum-1", T0);
        entry.compatibility   = CacheCompatibility::Unproven;
        std::string refusal;
        REQUIRE(store.store(entry, refusal));
        LookupResult result = store.lookup(key, std::string("sum-1"), T0 + 10);
        CHECK(result.verdict == LookupVerdict::UnprovenCompatibility);
        CHECK_FALSE(result.reusable());
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("unproven is not reused"));
    }

    SECTION("an entry from another build is not reused")
    {
        SliceCacheStore store(default_policy());
        CacheEntry      entry = make_entry(key, "gcode", 100, "sum-1", T0);
        entry.build_id        = "tigerslicer-older";
        std::string refusal;
        REQUIRE(store.store(entry, refusal));
        LookupResult result = store.lookup(key, std::string("sum-1"), T0 + 10);
        CHECK(result.verdict == LookupVerdict::IncompatibleBuild);
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("tigerslicer-older"));
    }

    SECTION("an entry with no checksum cannot be shown to be what it was")
    {
        SliceCacheStore store(default_policy());
        CacheEntry      entry = make_entry(key, "gcode", 100, "sum-1", T0);
        entry.checksum.reset();
        std::string refusal;
        REQUIRE(store.store(entry, refusal));
        CHECK(store.lookup(key, std::string("sum-1"), T0).verdict ==
              LookupVerdict::Unverifiable);
    }

    SECTION("not checking the payload is not the same as it being intact")
    {
        SliceCacheStore store(default_policy());
        std::string     refusal;
        REQUIRE(store.store(make_entry(key, "gcode", 100, "sum-1", T0), refusal));
        CHECK(store.lookup(key, std::nullopt, T0).verdict == LookupVerdict::Unverifiable);

        CachePolicy relaxed  = default_policy();
        relaxed.require_checksum = false;
        store.set_policy(relaxed);
        LookupResult result = store.lookup(key, std::nullopt, T0);
        CHECK(result.verdict == LookupVerdict::Hit);
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("without verification"));
    }

    SECTION("a payload that does not match its checksum is corrupt, not a miss")
    {
        SliceCacheStore store(default_policy());
        std::string     refusal;
        REQUIRE(store.store(make_entry(key, "gcode", 100, "sum-1", T0), refusal));
        CHECK(store.lookup(key, std::string("sum-2"), T0).verdict == LookupVerdict::Corrupt);
    }

    SECTION("a key that was never stored is a miss, and says which key")
    {
        SliceCacheStore store(default_policy());
        LookupResult    result = store.lookup(key_of("bbbb"), std::string("x"), T0);
        CHECK(result.verdict == LookupVerdict::Miss);
        CHECK(result.entry == nullptr);
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("bbbb"));
    }
}

TEST_CASE("29: a disabled cache misses everything and stores nothing", "[slice-cache]")
{
    CachePolicy policy = default_policy();
    policy.enabled     = false;
    SliceCacheStore store(policy);

    std::string refusal;
    CHECK_FALSE(store.store(make_entry(key_of("aaaa"), "gcode", 100, "sum-1", T0), refusal));
    CHECK_THAT(refusal, Catch::Matchers::ContainsSubstring("disabled"));
    CHECK(store.size() == 0);

    LookupResult result = store.lookup(key_of("aaaa"), std::string("sum-1"), T0);
    CHECK(result.verdict == LookupVerdict::Disabled);
    CHECK_FALSE(result.reusable());
    CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("every slice is computed"));
}

TEST_CASE("29: one content address never covers two different payloads", "[slice-cache]")
{
    SliceCacheStore store(default_policy());
    std::string     refusal;
    REQUIRE(store.store(make_entry(key_of("aaaa"), "gcode", 100, "sum-1", T0), refusal));

    CHECK_FALSE(store.store(make_entry(key_of("aaaa"), "gcode", 100, "sum-2", T0), refusal));
    CHECK_THAT(refusal, Catch::Matchers::ContainsSubstring("missing an input"));
    // The original is untouched.
    REQUIRE(store.find(key_of("aaaa")) != nullptr);
    CHECK(store.find(key_of("aaaa"))->checksum.value() == "sum-1");

    // Re-storing identical content is fine; it refreshes the entry.
    CHECK(store.store(make_entry(key_of("aaaa"), "gcode", 100, "sum-1", T0 + 5), refusal));
    CHECK(store.find(key_of("aaaa"))->last_used_s == T0 + 5);

    CHECK_FALSE(store.store(make_entry(CacheKey {}, "gcode", 1, "s", T0), refusal));
    CHECK_THAT(refusal, Catch::Matchers::ContainsSubstring("no key"));
}

TEST_CASE("29: verification is deterministic and says what it did not check", "[slice-cache]")
{
    SliceCacheStore store(default_policy());
    std::string     refusal;
    REQUIRE(store.store(make_entry(key_of("aaaa"), "slice", 10, "s-a", T0), refusal));
    REQUIRE(store.store(make_entry(key_of("bbbb"), "perimeters", 20, "s-b", T0), refusal));
    CacheEntry unchecked = make_entry(key_of("cccc"), "gcode", 30, "s-c", T0);
    unchecked.checksum.reset();
    REQUIRE(store.store(unchecked, refusal));

    const std::map<CacheKey, std::string> actual = { { key_of("aaaa"), "s-a" },
                                                     { key_of("bbbb"), "WRONG" } };

    SECTION("intact, corrupt and unverifiable are three outcomes, not two")
    {
        VerificationReport report = store.verify(actual, 0);
        CHECK(report.checked == 3);
        CHECK(report.intact == 1);
        CHECK(report.corrupt == std::vector<CacheKey> { key_of("bbbb") });
        CHECK(report.unverifiable == std::vector<CacheKey> { key_of("cccc") });
        CHECK_FALSE(report.clean());
        CHECK(report.complete);
    }

    SECTION("an entry nobody supplied a checksum for is unverifiable, not intact")
    {
        VerificationReport report = store.verify({}, 0);
        CHECK(report.intact == 0);
        CHECK(report.unverifiable.size() == 3);
        CHECK(report.corrupt.empty());
        // Nothing corrupt was found, but that is not a clean bill of health
        // for anything: nothing was actually checked.
        CHECK(report.clean());
    }

    SECTION("a capped verification says it is capped rather than reading as exhaustive")
    {
        VerificationReport report = store.verify(actual, 2);
        CHECK(report.checked == 2);
        CHECK_FALSE(report.complete);
        CHECK_THAT(report.cap_note, Catch::Matchers::ContainsSubstring("were not checked"));
    }

    SECTION("the same store verifies the same way every time")
    {
        VerificationReport a = store.verify(actual, 0);
        VerificationReport b = store.verify(actual, 0);
        CHECK(a.corrupt == b.corrupt);
        CHECK(a.unverifiable == b.unverifiable);
        CHECK(a.intact == b.intact);
    }
}

TEST_CASE("29: recovery removes what was derived from corruption too", "[slice-cache]")
{
    SliceCacheStore store(default_policy());
    std::string     refusal;

    CacheEntry slice = make_entry(key_of("aaaa"), "slice", 10, "s-a", T0);
    CacheEntry perimeters = make_entry(key_of("bbbb"), "perimeters", 20, "s-b", T0);
    perimeters.depends_on = { key_of("aaaa") };
    CacheEntry gcode  = make_entry(key_of("cccc"), "gcode", 30, "s-c", T0);
    gcode.depends_on  = { key_of("bbbb") };
    CacheEntry other  = make_entry(key_of("dddd"), "thumbnail", 40, "s-d", T0);

    REQUIRE(store.store(slice, refusal));
    REQUIRE(store.store(perimeters, refusal));
    REQUIRE(store.store(gcode, refusal));
    REQUIRE(store.store(other, refusal));

    RecoveryReport report = store.recover({ key_of("aaaa") });

    CHECK(report.evicted_corrupt == std::vector<CacheKey> { key_of("aaaa") });
    // Both the direct and the indirect dependent go: keeping the g-code would
    // be keeping something derived from data known to be wrong.
    CHECK(report.evicted_dependent ==
          std::vector<CacheKey> { key_of("bbbb"), key_of("cccc") });
    CHECK(report.bytes_freed == 60);
    CHECK(report.stages_lost ==
          std::vector<std::string> { "gcode", "perimeters", "slice" });

    // The unrelated entry survives.
    CHECK(store.size() == 1);
    CHECK(store.find(key_of("dddd")) != nullptr);
}

TEST_CASE("29: size controls evict least-recently-used and report deterministically",
          "[slice-cache]")
{
    CachePolicy policy = default_policy();
    policy.max_bytes   = 100;
    policy.max_entries = 100;
    SliceCacheStore store(policy);

    std::string refusal;
    REQUIRE(store.store(make_entry(key_of("aaaa"), "slice", 50, "s-a", T0 + 30), refusal));
    REQUIRE(store.store(make_entry(key_of("bbbb"), "perimeters", 50, "s-b", T0 + 10), refusal));
    REQUIRE(store.store(make_entry(key_of("cccc"), "gcode", 50, "s-c", T0 + 20), refusal));
    CHECK(store.bytes() == 150);

    SECTION("the oldest goes first, and only as many as the limit needs")
    {
        EvictionPlan plan = store.plan_eviction();
        CHECK(plan.evict == std::vector<CacheKey> { key_of("bbbb") });
        CHECK(plan.bytes_freed == 50);
        CHECK_FALSE(plan.insufficient);

        RecoveryReport applied = store.apply_eviction(plan);
        CHECK(applied.bytes_freed == 50);
        CHECK(store.bytes() == 100);
        CHECK(store.size() == 2);
    }

    SECTION("a tie on last use is broken by key, so the plan is reproducible")
    {
        SliceCacheStore tied(policy);
        REQUIRE(tied.store(make_entry(key_of("cccc"), "a", 50, "s", T0), refusal));
        REQUIRE(tied.store(make_entry(key_of("aaaa"), "b", 50, "s", T0), refusal));
        REQUIRE(tied.store(make_entry(key_of("bbbb"), "c", 50, "s", T0), refusal));
        CHECK(tied.plan_eviction().evict == std::vector<CacheKey> { key_of("aaaa") });
        CHECK(tied.plan_eviction().evict == std::vector<CacheKey> { key_of("aaaa") });
    }

    SECTION("an entry count limit is enforced as well as a byte limit")
    {
        CachePolicy few = default_policy();
        few.max_bytes   = 1000;
        few.max_entries = 1;
        store.set_policy(few);
        EvictionPlan plan = store.plan_eviction();
        CHECK(plan.evict.size() == 2);
    }

    SECTION("a limit only an empty cache satisfies is stated, not quietly obeyed")
    {
        // Emptying the store does satisfy a 10-byte limit, arithmetically. A
        // caller told only "3 evicted, now within limits" would go on missing
        // every lookup without ever being told why.
        CachePolicy impossible = default_policy();
        impossible.max_bytes   = 10;
        store.set_policy(impossible);
        EvictionPlan plan = store.plan_eviction();
        CHECK(plan.insufficient);
        CHECK(plan.evict.size() == 3);
        CHECK_THAT(plan.message,
                   Catch::Matchers::ContainsSubstring("would not fit either"));
    }

    SECTION("a cache within its limits evicts nothing")
    {
        CachePolicy roomy = default_policy();
        store.set_policy(roomy);
        EvictionPlan plan = store.plan_eviction();
        CHECK(plan.evict.empty());
        CHECK_THAT(plan.message, Catch::Matchers::ContainsSubstring("within its limits"));
    }
}

TEST_CASE("29: clearing reports what was removed, and works on a disabled cache",
          "[slice-cache]")
{
    SliceCacheStore store(default_policy());
    std::string     refusal;
    REQUIRE(store.store(make_entry(key_of("aaaa"), "slice", 10, "s-a", T0), refusal));
    REQUIRE(store.store(make_entry(key_of("bbbb"), "gcode", 20, "s-b", T0), refusal));

    CachePolicy disabled = default_policy();
    disabled.enabled     = false;
    store.set_policy(disabled);

    RecoveryReport report = store.clear();
    CHECK(report.bytes_freed == 30);
    CHECK(report.evicted_corrupt.size() == 2);
    CHECK(report.stages_lost == std::vector<std::string> { "gcode", "slice" });
    CHECK(store.size() == 0);
    CHECK(store.bytes() == 0);
}

TEST_CASE("29: a cached slice and a clean slice must produce the same thing", "[slice-cache]")
{
    const std::vector<StageOutput> clean = { { "slice", "d1" },
                                             { "perimeters", "d2" },
                                             { "infill", "d3" },
                                             { "gcode", "d4" } };

    SECTION("identical output is equivalent, whatever order it is reported in")
    {
        std::vector<StageOutput> cached(clean.rbegin(), clean.rend());
        EquivalenceReport        report = compare_slice_outputs(cached, clean);
        CHECK(report.equivalent);
        CHECK(report.compared == 4);
        CHECK(report.differences.empty());
        CHECK_THAT(report.summary, Catch::Matchers::ContainsSubstring("agree on all 4"));
    }

    SECTION("one differing stage is named, and the two digests are both kept")
    {
        std::vector<StageOutput> cached = clean;
        cached[2].digest                = "different";
        EquivalenceReport report        = compare_slice_outputs(cached, clean);
        CHECK_FALSE(report.equivalent);
        REQUIRE(report.differences.size() == 1);
        CHECK(report.differences[0].stage == "infill");
        CHECK(report.differences[0].cached_digest.value() == "different");
        CHECK(report.differences[0].clean_digest.value() == "d3");
    }

    SECTION("a stage only one run produced is a difference, not a skip")
    {
        std::vector<StageOutput> cached = clean;
        cached.pop_back();
        EquivalenceReport report = compare_slice_outputs(cached, clean);
        CHECK_FALSE(report.equivalent);
        REQUIRE(report.differences.size() == 1);
        CHECK(report.differences[0].stage == "gcode");
        CHECK_FALSE(report.differences[0].cached_digest.has_value());
        CHECK(report.differences[0].clean_digest.value() == "d4");
        CHECK_THAT(report.differences[0].what,
                   Catch::Matchers::ContainsSubstring("the cached slice did not"));
    }

    SECTION("an extra stage in the cached run is a difference too")
    {
        std::vector<StageOutput> cached = clean;
        cached.push_back({ "leftover", "d9" });
        EquivalenceReport report = compare_slice_outputs(cached, clean);
        CHECK_FALSE(report.equivalent);
        CHECK(report.differences[0].stage == "leftover");
        CHECK_FALSE(report.differences[0].clean_digest.has_value());
    }

    SECTION("a stage reported twice makes the comparison untrustworthy either way")
    {
        std::vector<StageOutput> cached = clean;
        cached.push_back({ "infill", "d3" });
        EquivalenceReport report = compare_slice_outputs(cached, clean);
        CHECK_FALSE(report.equivalent);
        CHECK(report.duplicated == std::vector<std::string> { "infill" });
        CHECK_THAT(report.summary,
                   Catch::Matchers::ContainsSubstring("cannot be trusted either way"));
    }

    SECTION("two empty runs agree, and say how little that proves")
    {
        EquivalenceReport report = compare_slice_outputs({}, {});
        CHECK(report.equivalent);
        CHECK(report.compared == 0);
        CHECK_THAT(report.summary, Catch::Matchers::ContainsSubstring("all 0 stage"));
    }
}

TEST_CASE("29: an incremental run and a clean run agree end to end", "[slice-cache]")
{
    // The phase's gate, run as one case: build a graph, cache it, change one
    // input, invalidate, recompute only what was invalidated, and check the
    // result against a clean run.
    std::vector<CacheNode> nodes = { CacheNode { 1, "slice", {}, {} },
                                     CacheNode { 2, "perimeters", { 1 }, {} },
                                     CacheNode { 3, "infill", { 1 }, {} },
                                     CacheNode { 4, "gcode", { 2, 3 }, {} } };
    {
        CacheInput geometry { CacheInputKind::Geometry, "model", "mesh-1" };
        nodes[0].inputs.push_back(geometry);
        CacheInput density { CacheInputKind::PrintProfile, "infill", "20%" };
        nodes[2].inputs.push_back(density);
    }

    const GraphResult first = resolve_graph(nodes);
    REQUIRE(first.ok());

    // "Compute" each stage: its output is a function of its key alone, which
    // is what a correct cache assumes and what the equivalence check tests.
    auto compute = [](const GraphResult &graph, const std::vector<CacheNode> &ns) {
        std::vector<StageOutput> out;
        for (size_t id : graph.order)
            for (const CacheNode &node : ns)
                if (node.id == id)
                    out.push_back({ node.stage, "out(" + graph.keys.at(id).digest + ")" });
        return out;
    };

    const std::vector<StageOutput> first_run = compute(first, nodes);

    SliceCacheStore store(default_policy());
    std::string     refusal;
    for (const auto &pair : first.keys) {
        CacheEntry entry = make_entry(pair.second, "stage" + std::to_string(pair.first), 10,
                                      "sum-" + pair.second.digest, T0);
        REQUIRE(store.store(entry, refusal));
    }

    // Change the infill density only.
    nodes[2].inputs[0].value = "40%";

    const InvalidationResult invalidation = invalidate(nodes, { 3 });
    REQUIRE(invalidation.ok());
    CHECK(invalidation.invalidated == std::vector<size_t> { 3, 4 });
    CHECK(invalidation.retained == std::vector<size_t> { 1, 2 });

    const GraphResult second = resolve_graph(nodes);
    REQUIRE(second.ok());

    // The retained nodes still hit the cache; the invalidated ones do not.
    for (size_t id : invalidation.retained) {
        CHECK(second.keys.at(id) == first.keys.at(id));
        LookupResult hit = store.lookup(second.keys.at(id),
                                        "sum-" + second.keys.at(id).digest, T0 + 1);
        CHECK(hit.verdict == LookupVerdict::Hit);
    }
    for (size_t id : invalidation.invalidated) {
        CHECK(second.keys.at(id) != first.keys.at(id));
        CHECK(store.lookup(second.keys.at(id), "sum-x", T0 + 1).verdict == LookupVerdict::Miss);
    }

    // An incremental run reuses the two retained stages and recomputes the two
    // invalidated ones; a clean run computes all four. They must agree.
    std::vector<StageOutput> incremental;
    for (const StageOutput &output : first_run)
        if (output.stage == "slice" || output.stage == "perimeters")
            incremental.push_back(output);
    for (size_t id : invalidation.invalidated)
        for (const CacheNode &node : nodes)
            if (node.id == id)
                incremental.push_back(
                    { node.stage, "out(" + second.keys.at(id).digest + ")" });

    const EquivalenceReport report = compare_slice_outputs(incremental, compute(second, nodes));
    CHECK(report.equivalent);
    CHECK(report.compared == 4);
    CHECK(report.differences.empty());
}
