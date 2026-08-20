///|/ TigerSlicer Phase 29: incremental slicing and dependency cache.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SliceCache.hpp"

#include <algorithm>
#include <sstream>

namespace Slic3r {

const char *cache_input_kind_name(CacheInputKind kind)
{
    switch (kind) {
    case CacheInputKind::Geometry:        return "geometry";
    case CacheInputKind::Ownership:       return "ownership";
    case CacheInputKind::Painting:        return "painting";
    case CacheInputKind::Modifier:        return "modifier";
    case CacheInputKind::PrinterProfile:  return "printer profile";
    case CacheInputKind::PrintProfile:    return "print profile";
    case CacheInputKind::FilamentProfile: return "filament profile";
    case CacheInputKind::ToolProfile:     return "tool profile";
    case CacheInputKind::Calibration:     return "calibration";
    case CacheInputKind::Transition:      return "transition";
    case CacheInputKind::Grid:            return "grid";
    case CacheInputKind::Support:         return "support";
    case CacheInputKind::Policy:          return "policy";
    case CacheInputKind::Build:           return "build";
    }
    return "policy";
}

const std::vector<CacheInputKind> &required_input_kinds()
{
    // Everything a slice's result actually depends on. Leaving one out would
    // make the key agree for two slices that differ, which is the one failure
    // mode a cache must not have.
    static const std::vector<CacheInputKind> kinds = {
        CacheInputKind::Geometry,        CacheInputKind::Ownership,
        CacheInputKind::Painting,        CacheInputKind::Modifier,
        CacheInputKind::PrinterProfile,  CacheInputKind::PrintProfile,
        CacheInputKind::FilamentProfile, CacheInputKind::ToolProfile,
        CacheInputKind::Calibration,     CacheInputKind::Transition,
        CacheInputKind::Grid,            CacheInputKind::Support,
        CacheInputKind::Policy,          CacheInputKind::Build,
    };
    return kinds;
}

const char *key_error_name(KeyError error)
{
    switch (error) {
    case KeyError::None:                return "none";
    case KeyError::MissingRequiredKind: return "missing required kind";
    case KeyError::DuplicateInput:      return "duplicate input";
    case KeyError::EmptyInputName:      return "empty input name";
    }
    return "none";
}

const char *graph_error_name(GraphError error)
{
    switch (error) {
    case GraphError::None:              return "none";
    case GraphError::UnknownDependency: return "unknown dependency";
    case GraphError::Cycle:             return "cycle";
    case GraphError::DuplicateNode:     return "duplicate node";
    case GraphError::KeyRefused:        return "key refused";
    }
    return "none";
}

const char *cache_compatibility_name(CacheCompatibility compatibility)
{
    switch (compatibility) {
    case CacheCompatibility::Proven:       return "proven";
    case CacheCompatibility::Unproven:     return "unproven";
    case CacheCompatibility::Incompatible: return "incompatible";
    }
    return "unproven";
}

const char *lookup_verdict_name(LookupVerdict verdict)
{
    switch (verdict) {
    case LookupVerdict::Hit:                   return "hit";
    case LookupVerdict::Miss:                  return "miss";
    case LookupVerdict::Disabled:              return "disabled";
    case LookupVerdict::UnprovenCompatibility: return "unproven compatibility";
    case LookupVerdict::IncompatibleBuild:     return "incompatible build";
    case LookupVerdict::Unverifiable:          return "unverifiable";
    case LookupVerdict::Corrupt:               return "corrupt";
    }
    return "miss";
}

const char *verify_verdict_name(VerifyVerdict verdict)
{
    switch (verdict) {
    case VerifyVerdict::Intact:       return "intact";
    case VerifyVerdict::Corrupt:      return "corrupt";
    case VerifyVerdict::Unverifiable: return "unverifiable";
    }
    return "unverifiable";
}

// --- hashing ---------------------------------------------------------------

namespace {

// A 128-bit FNV-1a, written out here rather than taken from std::hash, whose
// values are not stable between implementations or runs. A cache key that
// changes when the standard library changes is a cache that silently misses
// everything.
struct Digest128
{
    uint64_t hi { 0x6c62272e07bb0142ull };
    uint64_t lo { 0x62b821756295c58dull };
};

void absorb(Digest128 &digest, const std::string &data)
{
    constexpr uint64_t PRIME_LO = 0x100000001b3ull;
    constexpr uint64_t PRIME_HI = 0x1000000000000013ull;
    for (unsigned char c : data) {
        digest.lo ^= uint64_t(c);
        digest.lo *= PRIME_LO;
        // Carry the low lane's state into the high lane, so the two are not
        // two independent 64-bit hashes of the same string.
        digest.hi ^= digest.lo + uint64_t(c) + 0x9e3779b97f4a7c15ull;
        digest.hi *= PRIME_HI;
    }
    // A length-delimiting step, so ("ab","c") and ("a","bc") do not collide.
    digest.lo ^= data.size() + 0x9e3779b97f4a7c15ull;
    digest.lo *= PRIME_LO;
    digest.hi ^= digest.lo;
}

std::string to_hex(const Digest128 &digest)
{
    static const char *HEX = "0123456789abcdef";
    std::string        out;
    out.reserve(32);
    for (uint64_t lane : { digest.hi, digest.lo })
        for (int shift = 60; shift >= 0; shift -= 4)
            out.push_back(HEX[(lane >> shift) & 0xf]);
    return out;
}

KeyResult build_key(const std::vector<CacheInput> &inputs, bool require_all)
{
    KeyResult result;

    std::vector<const CacheInput *> sorted;
    for (const CacheInput &input : inputs) {
        if (input.name.empty()) {
            result.error   = KeyError::EmptyInputName;
            result.message = std::string("a ") + cache_input_kind_name(input.kind) +
                             " input was supplied with no name, so two of them could not be told "
                             "apart";
            return result;
        }
        sorted.push_back(&input);
    }

    std::sort(sorted.begin(), sorted.end(), [](const CacheInput *a, const CacheInput *b) {
        if (a->kind != b->kind)
            return uint8_t(a->kind) < uint8_t(b->kind);
        return a->name < b->name;
    });

    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i]->kind == sorted[i - 1]->kind && sorted[i]->name == sorted[i - 1]->name) {
            result.error   = KeyError::DuplicateInput;
            result.message = std::string("two ") + cache_input_kind_name(sorted[i]->kind) +
                             " inputs are both named \"" + sorted[i]->name +
                             "\"; one of them would be silently ignored";
            return result;
        }

    if (require_all) {
        std::set<CacheInputKind> present;
        for (const CacheInput *input : sorted)
            present.insert(input->kind);
        for (CacheInputKind kind : required_input_kinds())
            if (! present.count(kind))
                result.missing.push_back(kind);
        if (! result.missing.empty()) {
            result.error   = KeyError::MissingRequiredKind;
            result.message = "a slice key was asked for without ";
            for (size_t i = 0; i < result.missing.size(); ++i) {
                if (i > 0)
                    result.message += ", ";
                result.message += cache_input_kind_name(result.missing[i]);
            }
            result.message += "; a key that does not cover every input is a key that agrees for "
                              "two different slices";
            return result;
        }
    }

    Digest128 digest;
    for (const CacheInput *input : sorted) {
        absorb(digest, cache_input_kind_name(input->kind));
        absorb(digest, input->name);
        absorb(digest, input->value);
    }
    result.key.digest = to_hex(digest);
    return result;
}

} // namespace

KeyResult build_cache_key(const std::vector<CacheInput> &inputs)
{
    return build_key(inputs, true);
}

KeyResult build_partial_key(const std::vector<CacheInput> &inputs)
{
    return build_key(inputs, false);
}

// --- graph -----------------------------------------------------------------

GraphResult resolve_graph(const std::vector<CacheNode> &nodes)
{
    GraphResult result;

    std::map<size_t, const CacheNode *> by_id;
    for (const CacheNode &node : nodes) {
        if (by_id.count(node.id)) {
            result.error   = GraphError::DuplicateNode;
            result.message = "node " + std::to_string(node.id) + " is defined twice";
            return result;
        }
        by_id[node.id] = &node;
    }

    for (const CacheNode &node : nodes)
        for (size_t dependency : node.depends_on)
            if (! by_id.count(dependency)) {
                result.error   = GraphError::UnknownDependency;
                result.message = "node " + std::to_string(node.id) + " (" + node.stage +
                                 ") depends on " + std::to_string(dependency) +
                                 ", which is not in the graph";
                return result;
            }

    // Kahn's algorithm over ids in ascending order, so the order is the same
    // every run for the same graph.
    std::map<size_t, size_t> indegree;
    std::map<size_t, std::vector<size_t>> dependents;
    for (const auto &entry : by_id)
        indegree[entry.first] = 0;
    for (const CacheNode &node : nodes) {
        // A node listing the same dependency twice depends on it once.
        std::set<size_t> unique(node.depends_on.begin(), node.depends_on.end());
        indegree[node.id] = unique.size();
        for (size_t dependency : unique)
            dependents[dependency].push_back(node.id);
    }

    std::set<size_t> ready;
    for (const auto &entry : indegree)
        if (entry.second == 0)
            ready.insert(entry.first);

    while (! ready.empty()) {
        const size_t id = *ready.begin();
        ready.erase(ready.begin());
        result.order.push_back(id);

        const CacheNode *node = by_id.at(id);

        // The node's key folds in its own inputs and the resolved keys of
        // everything it was computed from, so a change anywhere upstream
        // changes it without anyone having to remember to say so.
        std::vector<CacheInput> combined = node->inputs;
        std::set<size_t>        unique(node->depends_on.begin(), node->depends_on.end());
        for (size_t dependency : unique) {
            CacheInput folded;
            folded.kind  = CacheInputKind::Policy;
            folded.name  = "dependency:" + std::to_string(dependency);
            folded.value = result.keys.at(dependency).digest;
            combined.push_back(folded);
        }
        CacheInput stage;
        stage.kind  = CacheInputKind::Policy;
        stage.name  = "stage";
        stage.value = node->stage;
        combined.push_back(stage);

        KeyResult key = build_partial_key(combined);
        if (! key.ok()) {
            result.error   = GraphError::KeyRefused;
            result.message = "node " + std::to_string(id) + " (" + node->stage +
                             ") could not be keyed: " + key.message;
            return result;
        }
        result.keys[id] = key.key;

        for (size_t dependent : dependents[id])
            if (--indegree[dependent] == 0)
                ready.insert(dependent);
    }

    if (result.order.size() != by_id.size()) {
        result.error = GraphError::Cycle;
        std::vector<size_t> stuck;
        for (const auto &entry : indegree)
            if (entry.second > 0)
                stuck.push_back(entry.first);
        std::ostringstream message;
        message << "the dependency graph has a cycle through node"
                << (stuck.size() > 1 ? "s " : " ");
        for (size_t i = 0; i < stuck.size(); ++i) {
            if (i > 0)
                message << ", ";
            message << stuck[i];
        }
        message << "; nothing can be keyed, because each key would depend on itself";
        result.message = message.str();
        result.keys.clear();
        result.order.clear();
        return result;
    }

    return result;
}

InvalidationResult invalidate(const std::vector<CacheNode> &nodes,
                              const std::vector<size_t>    &changed)
{
    InvalidationResult result;

    const GraphResult graph = resolve_graph(nodes);
    if (! graph.ok()) {
        result.error   = graph.error;
        result.message = graph.message;
        return result;
    }

    std::set<size_t> known;
    for (const CacheNode &node : nodes)
        known.insert(node.id);

    std::set<size_t> dirty;
    std::set<size_t> unknown;
    for (size_t id : changed) {
        if (known.count(id))
            dirty.insert(id);
        else
            unknown.insert(id);
    }

    // Walk in topological order, so a node is visited after everything it
    // depends on. One pass suffices.
    for (size_t id : graph.order) {
        if (dirty.count(id))
            continue;
        for (const CacheNode &node : nodes) {
            if (node.id != id)
                continue;
            for (size_t dependency : node.depends_on)
                if (dirty.count(dependency)) {
                    dirty.insert(id);
                    break;
                }
        }
    }

    for (size_t id : known) {
        if (dirty.count(id))
            result.invalidated.push_back(id);
        else
            result.retained.push_back(id);
    }
    result.unknown.assign(unknown.begin(), unknown.end());

    if (! result.unknown.empty()) {
        result.message = std::to_string(result.unknown.size()) +
                         " changed node(s) are not in this graph; what they affect could not be "
                         "worked out, so the invalidation below is not the whole invalidation";
    }

    return result;
}

// --- store -----------------------------------------------------------------

bool SliceCacheStore::store(const CacheEntry &entry, std::string &refusal)
{
    if (! m_policy.enabled) {
        refusal = "the cache is disabled; nothing was stored";
        return false;
    }
    if (! entry.key.valid()) {
        refusal = "an entry was offered with no key";
        return false;
    }
    auto existing = m_entries.find(entry.key);
    if (existing != m_entries.end()) {
        if (existing->second.checksum != entry.checksum) {
            // Two different payloads under one content address means the key
            // does not cover something it should. Overwriting would hide that.
            refusal = "an entry already exists under " + entry.key.digest +
                      " with a different checksum; a content address that covers two different "
                      "payloads is a key that is missing an input";
            return false;
        }
        m_entries[entry.key] = entry;
        return true;
    }
    m_entries.emplace(entry.key, entry);
    return true;
}

LookupResult SliceCacheStore::lookup(const CacheKey                   &key,
                                     const std::optional<std::string> &actual_checksum,
                                     int64_t                           now_s)
{
    LookupResult result;

    if (! m_policy.enabled) {
        result.verdict = LookupVerdict::Disabled;
        result.message = "the cache is disabled; every slice is computed";
        return result;
    }

    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        result.verdict = LookupVerdict::Miss;
        result.message = "nothing is stored under " + key.digest;
        return result;
    }

    CacheEntry &entry = it->second;
    result.entry      = &entry;

    if (! m_policy.build_id.empty() && entry.build_id != m_policy.build_id) {
        result.verdict = LookupVerdict::IncompatibleBuild;
        result.message = "the entry was produced by build \"" + entry.build_id +
                         "\" and this is build \"" + m_policy.build_id +
                         "\"; a slice from another build is a different slice until somebody "
                         "shows otherwise";
        return result;
    }

    if (entry.compatibility == CacheCompatibility::Incompatible) {
        result.verdict = LookupVerdict::IncompatibleBuild;
        result.message = "the entry is recorded as incompatible with the inputs now in force";
        return result;
    }
    if (entry.compatibility != CacheCompatibility::Proven) {
        result.verdict = LookupVerdict::UnprovenCompatibility;
        result.message = "nothing has shown this entry is compatible with the inputs now in "
                         "force; unproven is not reused";
        return result;
    }

    if (! entry.checksum.has_value()) {
        result.verdict = LookupVerdict::Unverifiable;
        result.message = "the entry has no recorded checksum, so it cannot be shown to be the "
                         "payload it was";
        return result;
    }
    if (! actual_checksum.has_value()) {
        if (m_policy.require_checksum) {
            result.verdict = LookupVerdict::Unverifiable;
            result.message = "the payload was not checked against its recorded checksum, and this "
                             "cache does not reuse what it has not verified";
            return result;
        }
        result.verdict = LookupVerdict::Hit;
        result.message = "reused without verification, because this cache does not require it";
        entry.last_used_s = now_s;
        return result;
    }
    if (*actual_checksum != *entry.checksum) {
        result.verdict = LookupVerdict::Corrupt;
        result.message = "the stored payload does not match its recorded checksum";
        return result;
    }

    entry.last_used_s = now_s;
    result.verdict    = LookupVerdict::Hit;
    result.message    = "verified against its recorded checksum";
    return result;
}

VerificationReport SliceCacheStore::verify(const std::map<CacheKey, std::string> &actual_checksums,
                                           size_t max_entries) const
{
    VerificationReport report;

    for (const auto &pair : m_entries) {
        if (max_entries > 0 && report.checked >= max_entries) {
            report.complete = false;
            report.cap_note = "verification stopped after " + std::to_string(max_entries) +
                              " entries; " + std::to_string(m_entries.size() - report.checked) +
                              " were not checked and nothing is claimed about them";
            break;
        }
        ++report.checked;

        const CacheEntry &entry = pair.second;
        if (! entry.checksum.has_value()) {
            report.unverifiable.push_back(entry.key);
            continue;
        }
        auto actual = actual_checksums.find(entry.key);
        if (actual == actual_checksums.end()) {
            report.unverifiable.push_back(entry.key);
            continue;
        }
        if (actual->second == *entry.checksum)
            ++report.intact;
        else
            report.corrupt.push_back(entry.key);
    }

    // m_entries is a std::map keyed by digest, so both lists are already
    // sorted; the sorts keep that true if the container ever changes.
    std::sort(report.corrupt.begin(), report.corrupt.end());
    std::sort(report.unverifiable.begin(), report.unverifiable.end());
    return report;
}

RecoveryReport SliceCacheStore::recover(const std::vector<CacheKey> &corrupt)
{
    RecoveryReport report;

    std::set<CacheKey> doomed(corrupt.begin(), corrupt.end());
    for (const CacheKey &key : corrupt)
        if (m_entries.count(key))
            report.evicted_corrupt.push_back(key);

    // Cascade: anything derived from something corrupt is itself suspect.
    // Repeat until nothing new is added, because the dependency chains here
    // are not otherwise ordered.
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto &pair : m_entries) {
            if (doomed.count(pair.first))
                continue;
            for (const CacheKey &dependency : pair.second.depends_on)
                if (doomed.count(dependency)) {
                    doomed.insert(pair.first);
                    report.evicted_dependent.push_back(pair.first);
                    grew = true;
                    break;
                }
        }
    }

    std::set<std::string> stages;
    for (const CacheKey &key : doomed) {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            continue;
        report.bytes_freed += it->second.size_bytes;
        stages.insert(it->second.stage);
        m_entries.erase(it);
    }
    report.stages_lost.assign(stages.begin(), stages.end());
    std::sort(report.evicted_corrupt.begin(), report.evicted_corrupt.end());
    std::sort(report.evicted_dependent.begin(), report.evicted_dependent.end());
    return report;
}

EvictionPlan SliceCacheStore::plan_eviction() const
{
    EvictionPlan plan;

    uint64_t current_bytes = bytes();
    size_t   current_count = m_entries.size();
    if (current_bytes <= m_policy.max_bytes && current_count <= m_policy.max_entries) {
        plan.message = "the cache is within its limits; nothing would be evicted";
        return plan;
    }

    std::vector<const CacheEntry *> candidates;
    for (const auto &pair : m_entries)
        candidates.push_back(&pair.second);
    // Least recently used first; ties broken by key so the plan is the same
    // every time it is made.
    std::sort(candidates.begin(), candidates.end(),
              [](const CacheEntry *a, const CacheEntry *b) {
                  if (a->last_used_s != b->last_used_s)
                      return a->last_used_s < b->last_used_s;
                  return a->key.digest < b->key.digest;
              });

    for (const CacheEntry *entry : candidates) {
        if (current_bytes <= m_policy.max_bytes && current_count <= m_policy.max_entries)
            break;
        plan.evict.push_back(entry->key);
        plan.bytes_freed += entry->size_bytes;
        current_bytes -= entry->size_bytes;
        --current_count;
    }

    if (! m_entries.empty() && plan.evict.size() == m_entries.size()) {
        // Emptying the cache does meet the limits, arithmetically. But a limit
        // that only nothing satisfies is not a size control, it is a disabled
        // cache that has not said so, and the caller is told rather than left
        // to notice that every slice misses.
        plan.insufficient = true;
        plan.message      = "these limits cannot hold anything currently stored; the cache would "
                            "be emptied and the next slice would not fit either";
    } else {
        plan.message = "evicting " + std::to_string(plan.evict.size()) +
                       " least recently used entr" + (plan.evict.size() == 1 ? "y" : "ies") +
                       " brings the cache within its limits";
    }
    return plan;
}

RecoveryReport SliceCacheStore::apply_eviction(const EvictionPlan &plan)
{
    RecoveryReport report;
    std::set<std::string> stages;
    for (const CacheKey &key : plan.evict) {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            continue;
        report.bytes_freed += it->second.size_bytes;
        stages.insert(it->second.stage);
        report.evicted_corrupt.push_back(key);
        m_entries.erase(it);
    }
    report.stages_lost.assign(stages.begin(), stages.end());
    return report;
}

RecoveryReport SliceCacheStore::clear()
{
    RecoveryReport report;
    std::set<std::string> stages;
    for (const auto &pair : m_entries) {
        report.bytes_freed += pair.second.size_bytes;
        report.evicted_corrupt.push_back(pair.first);
        stages.insert(pair.second.stage);
    }
    report.stages_lost.assign(stages.begin(), stages.end());
    m_entries.clear();
    return report;
}

uint64_t SliceCacheStore::bytes() const
{
    uint64_t total = 0;
    for (const auto &pair : m_entries)
        total += pair.second.size_bytes;
    return total;
}

const CacheEntry *SliceCacheStore::find(const CacheKey &key) const
{
    auto it = m_entries.find(key);
    return it == m_entries.end() ? nullptr : &it->second;
}

std::vector<CacheKey> SliceCacheStore::keys() const
{
    std::vector<CacheKey> out;
    for (const auto &pair : m_entries)
        out.push_back(pair.first);
    return out;
}

// --- equivalence -----------------------------------------------------------

EquivalenceReport compare_slice_outputs(const std::vector<StageOutput> &cached,
                                        const std::vector<StageOutput> &clean)
{
    EquivalenceReport report;

    std::map<std::string, std::string> cached_by_stage, clean_by_stage;
    std::set<std::string>              duplicated;

    for (const StageOutput &output : cached)
        if (! cached_by_stage.emplace(output.stage, output.digest).second)
            duplicated.insert(output.stage);
    for (const StageOutput &output : clean)
        if (! clean_by_stage.emplace(output.stage, output.digest).second)
            duplicated.insert(output.stage);

    report.duplicated.assign(duplicated.begin(), duplicated.end());

    std::set<std::string> stages;
    for (const auto &pair : cached_by_stage)
        stages.insert(pair.first);
    for (const auto &pair : clean_by_stage)
        stages.insert(pair.first);

    for (const std::string &stage : stages) {
        auto in_cached = cached_by_stage.find(stage);
        auto in_clean  = clean_by_stage.find(stage);

        StageDifference difference;
        difference.stage = stage;
        if (in_cached != cached_by_stage.end())
            difference.cached_digest = in_cached->second;
        if (in_clean != clean_by_stage.end())
            difference.clean_digest = in_clean->second;

        if (in_cached == cached_by_stage.end()) {
            difference.what = "the clean slice produced this stage and the cached slice did not";
            report.differences.push_back(difference);
            continue;
        }
        if (in_clean == clean_by_stage.end()) {
            difference.what = "the cached slice produced this stage and the clean slice did not";
            report.differences.push_back(difference);
            continue;
        }

        ++report.compared;
        if (in_cached->second != in_clean->second) {
            difference.what = "the two runs produced different output for this stage";
            report.differences.push_back(difference);
        }
    }

    report.equivalent = report.differences.empty() && report.duplicated.empty();

    if (report.equivalent) {
        report.summary = "the cached and clean slices agree on all " +
                         std::to_string(report.compared) + " stage(s)";
    } else if (! report.duplicated.empty() && report.differences.empty()) {
        report.summary = std::to_string(report.duplicated.size()) +
                         " stage(s) appear more than once in one of the runs, so the comparison "
                         "cannot be trusted either way";
    } else {
        report.summary = std::to_string(report.differences.size()) + " of " +
                         std::to_string(stages.size()) +
                         " stage(s) differ between the cached and clean slices";
        if (! report.duplicated.empty())
            report.summary += "; " + std::to_string(report.duplicated.size()) +
                              " stage(s) also appear more than once";
    }

    return report;
}

} // namespace Slic3r
