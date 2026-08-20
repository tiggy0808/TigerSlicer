///|/ TigerSlicer Phase 29: incremental slicing and dependency cache.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_SliceCache_hpp_
#define slic3r_SliceCache_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

// Everything a slice depends on. The list is closed on purpose: a cache keyed
// on a subset of what a slice depends on is a cache that returns the wrong
// answer, so the builder refuses to produce a key until every required kind
// has been supplied.
enum class CacheInputKind : uint8_t
{
    Geometry,
    Ownership,
    Painting,
    Modifier,
    PrinterProfile,
    PrintProfile,
    FilamentProfile,
    ToolProfile,
    Calibration,
    Transition,
    Grid,
    Support,
    Policy,
    // The TigerSlicer build the entry was produced by. A slice from a
    // different build is a different slice until proven otherwise.
    Build,
};

const char *cache_input_kind_name(CacheInputKind kind);

// The kinds that must be present before a key can be produced at all.
const std::vector<CacheInputKind> &required_input_kinds();

struct CacheInput
{
    CacheInputKind kind { CacheInputKind::Policy };
    // Distinguishes two inputs of the same kind. Two inputs with the same kind
    // and name are a defect, not a merge.
    std::string name;
    // The content itself, or a digest of it. What matters is that equal
    // content gives an equal string.
    std::string value;
};

// A content address. Empty until a key was actually produced; a key that could
// not be produced is never a usable one.
struct CacheKey
{
    std::string digest;

    bool        valid() const { return ! digest.empty(); }
    bool        operator==(const CacheKey &o) const { return digest == o.digest; }
    bool        operator!=(const CacheKey &o) const { return digest != o.digest; }
    bool        operator<(const CacheKey &o) const { return digest < o.digest; }
};

enum class KeyError : uint8_t
{
    None,
    MissingRequiredKind,
    DuplicateInput,
    EmptyInputName,
};

const char *key_error_name(KeyError error);

struct KeyResult
{
    CacheKey    key;
    KeyError    error { KeyError::None };
    std::string message;
    // The required kinds that were not supplied, sorted, so the caller is told
    // all of them rather than the first one found.
    std::vector<CacheInputKind> missing;

    bool ok() const { return error == KeyError::None; }
};

// Build a content address over a set of inputs. Order-independent by
// construction: inputs are sorted by (kind, name) before hashing, so the same
// inputs presented in any order give the same key, and a different value
// anywhere gives a different one.
KeyResult build_cache_key(const std::vector<CacheInput> &inputs);

// The same, without the requirement that every required kind be present. For
// keying a stage inside a slice, where the whole-slice inputs are already
// folded in through the node's dependencies.
KeyResult build_partial_key(const std::vector<CacheInput> &inputs);

// --- dependency graph ------------------------------------------------------

struct CacheNode
{
    size_t      id { 0 };
    std::string stage;

    // Ids this node's output is computed from. A node's key folds in its
    // dependencies' keys, so a change anywhere upstream changes it.
    std::vector<size_t> depends_on;

    // The node's own inputs, before its dependencies are folded in.
    std::vector<CacheInput> inputs;
};

enum class GraphError : uint8_t
{
    None,
    UnknownDependency,
    Cycle,
    DuplicateNode,
    KeyRefused,
};

const char *graph_error_name(GraphError error);

struct GraphResult
{
    GraphError  error { GraphError::None };
    std::string message;

    // Every node's resolved key, by id.
    std::map<size_t, CacheKey> keys;
    // Topological order, so a caller evaluates in a deterministic sequence.
    std::vector<size_t> order;

    bool ok() const { return error == GraphError::None; }
};

GraphResult resolve_graph(const std::vector<CacheNode> &nodes);

// What an edit invalidates, and - just as importantly - what it does not.
struct InvalidationResult
{
    GraphError  error { GraphError::None };
    std::string message;

    // The nodes given as changed, plus everything downstream of them, sorted.
    std::vector<size_t> invalidated;
    // Everything else, sorted. Named explicitly so "only the affected nodes"
    // is a checkable claim rather than an intention.
    std::vector<size_t> retained;

    // Changed ids that are not in the graph at all. Reported rather than
    // ignored, because ignoring them would understate what was invalidated.
    std::vector<size_t> unknown;

    bool ok() const { return error == GraphError::None; }
};

InvalidationResult invalidate(const std::vector<CacheNode> &nodes,
                              const std::vector<size_t>    &changed);

// --- the store -------------------------------------------------------------

// Whether an entry may be reused. Unproven is its own state: nobody showed
// this entry was produced under inputs compatible with the ones now in force,
// and an unproven entry is never handed back.
enum class CacheCompatibility : uint8_t
{
    Proven,
    Unproven,
    Incompatible,
};

const char *cache_compatibility_name(CacheCompatibility compatibility);

struct CacheEntry
{
    CacheKey    key;
    std::string stage;

    uint64_t size_bytes { 0 };
    // A digest of the stored payload, for verification. Absent means the entry
    // cannot be verified, which is not the same as being intact.
    std::optional<std::string> checksum;

    // The build that produced it, carried separately from the key so a
    // mismatch can be explained rather than merely missing.
    std::string build_id;

    CacheCompatibility compatibility { CacheCompatibility::Unproven };

    // Caller-supplied, monotonic. Nothing here reads a clock.
    int64_t created_s { 0 };
    int64_t last_used_s { 0 };

    // Keys this entry was derived from. Used to cascade eviction when
    // something it depends on turns out to be corrupt.
    std::vector<CacheKey> depends_on;
};

struct CachePolicy
{
    // A disabled cache misses every lookup and stores nothing. It does not
    // quietly become a smaller cache.
    bool enabled { true };

    uint64_t max_bytes { 1024ull * 1024ull * 1024ull };
    size_t   max_entries { 4096 };

    // The build in force. An entry from another build is never reused.
    std::string build_id;

    // When true, an entry with no checksum is treated as unverifiable and not
    // reused. When false it may be reused, and the lookup says it could not be
    // verified.
    bool require_checksum { true };
};

enum class LookupVerdict : uint8_t
{
    Hit,
    Miss,
    Disabled,
    // Present, but not reusable. Each of these is distinct from a miss,
    // because a caller that treats them alike cannot tell a cold cache from a
    // cache full of things it must not touch.
    UnprovenCompatibility,
    IncompatibleBuild,
    Unverifiable,
    Corrupt,
};

const char *lookup_verdict_name(LookupVerdict verdict);

struct LookupResult
{
    LookupVerdict     verdict { LookupVerdict::Miss };
    std::string       message;
    const CacheEntry *entry { nullptr };

    bool reusable() const { return verdict == LookupVerdict::Hit; }
};

enum class VerifyVerdict : uint8_t
{
    Intact,
    Corrupt,
    // No checksum was recorded, so nothing was verified. Reported as its own
    // outcome, never folded into "intact".
    Unverifiable,
};

const char *verify_verdict_name(VerifyVerdict verdict);

struct VerificationReport
{
    size_t checked { 0 };
    size_t intact { 0 };

    // Sorted by key, so two verifications of the same store read alike.
    std::vector<CacheKey> corrupt;
    std::vector<CacheKey> unverifiable;

    // True when the report covered every entry. False when it was capped, and
    // the report then says so rather than reading as exhaustive.
    bool complete { true };
    std::string cap_note;

    bool clean() const { return corrupt.empty(); }
};

struct RecoveryReport
{
    // Directly corrupt.
    std::vector<CacheKey> evicted_corrupt;
    // Evicted because something they were derived from was corrupt. A cache
    // that keeps these has kept data derived from data it knows is wrong.
    std::vector<CacheKey> evicted_dependent;

    uint64_t bytes_freed { 0 };
    // What the caller now has to recompute, in the same terms it stored them.
    std::vector<std::string> stages_lost;
};

struct EvictionPlan
{
    // In the order they would be evicted: least recently used first, ties
    // broken by key so the plan is deterministic.
    std::vector<CacheKey> evict;
    uint64_t              bytes_freed { 0 };

    // True when evicting everything listed still leaves the store over its
    // limits - the caller is told rather than left believing the cache fits.
    bool        insufficient { false };
    std::string message;
};

class SliceCacheStore
{
public:
    explicit SliceCacheStore(CachePolicy policy) : m_policy(std::move(policy)) {}

    const CachePolicy &policy() const { return m_policy; }
    void               set_policy(CachePolicy policy) { m_policy = std::move(policy); }

    // Refuses when the cache is disabled, when the key is invalid, or when an
    // entry is already stored under that key with different content.
    bool store(const CacheEntry &entry, std::string &refusal);

    // actual_checksum is what the caller computed from the payload it holds
    // now. Absent means the caller did not check; the lookup then reports
    // Unverifiable rather than assuming the payload is what it was.
    LookupResult lookup(const CacheKey &key, const std::optional<std::string> &actual_checksum,
                        int64_t now_s);

    VerificationReport verify(const std::map<CacheKey, std::string> &actual_checksums,
                              size_t                                 max_entries) const;

    RecoveryReport recover(const std::vector<CacheKey> &corrupt);

    EvictionPlan plan_eviction() const;
    RecoveryReport apply_eviction(const EvictionPlan &plan);

    // Removes everything and reports what was removed. Clearing a disabled
    // cache is still allowed: disabling stops use, not cleanup.
    RecoveryReport clear();

    size_t   size() const { return m_entries.size(); }
    uint64_t bytes() const;
    const CacheEntry *find(const CacheKey &key) const;
    std::vector<CacheKey> keys() const;

private:
    CachePolicy                     m_policy;
    std::map<CacheKey, CacheEntry> m_entries;
};

// --- cached versus clean equivalence --------------------------------------

// One stage's output, as a digest, from one of the two runs.
struct StageOutput
{
    std::string stage;
    std::string digest;
};

struct StageDifference
{
    std::string stage;
    // Absent on the side where the stage did not appear at all.
    std::optional<std::string> cached_digest;
    std::optional<std::string> clean_digest;
    std::string                what;
};

struct EquivalenceReport
{
    bool equivalent { false };

    size_t compared { 0 };
    // Sorted by stage.
    std::vector<StageDifference> differences;

    // Stages that appeared twice in one run's output. A duplicate stage means
    // the comparison cannot be trusted either way, so it is a difference in
    // its own right.
    std::vector<std::string> duplicated;

    std::string summary;
};

// Compare a cached slice against a clean one. Any stage present in one and not
// the other is a difference, not a skip: the phase's gate is that the two runs
// produce the same thing, and a stage that only one run produced is exactly
// the failure that gate exists to catch.
EquivalenceReport compare_slice_outputs(const std::vector<StageOutput> &cached,
                                        const std::vector<StageOutput> &clean);

} // namespace Slic3r

#endif // slic3r_SliceCache_hpp_
