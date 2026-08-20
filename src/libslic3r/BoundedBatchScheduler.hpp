///|/ TigerSlicer Phase 25B: bounded lead/lag scheduler for the experimental
///|/ bounded-Z colour/tool batching mode.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BoundedBatchScheduler_hpp_
#define slic3r_BoundedBatchScheduler_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "DependencyComponents.hpp"

namespace Slic3r {

enum class BatchPolicy : uint8_t
{
    Conservative,
    Balanced,
    Aggressive,
};

const char *batch_policy_name(BatchPolicy policy);

enum class ScheduleMode : uint8_t
{
    // The ordinary by-layer schedule, because batching was never asked for.
    Legacy,
    BoundedBatch,
    // The ordinary schedule, arrived at after batching was ruled out.
    FallbackLegacy,
};

const char *schedule_mode_name(ScheduleMode mode);

enum class BatchVerdict : uint8_t
{
    Batched,
    // Batched, but every batch was cut short by something.
    Reduced,
    FellBack,
    // Batching was ruled out and falling back was not permitted, so nothing
    // is offered for export at all.
    Blocked,
    Disabled,
};

const char *batch_verdict_name(BatchVerdict verdict);

// One layer of one component, in that component's own grid.
struct BatchLayer
{
    double z_bottom_mm { 0. };
    double z_top_mm { 0. };
    int    tool { 0 };
    double print_time_s { 0. };
};

struct BatchComponent
{
    // Must match a component id from the Phase 25A decomposition.
    size_t id { 0 };

    double min_x { 0. }, max_x { 0. };
    double min_y { 0. }, max_y { 0. };

    std::vector<BatchLayer> layers;
};

struct BatchingOptions
{
    // Experimental and off. Nothing here runs until it is turned on.
    bool        enabled { false };
    BatchPolicy policy { BatchPolicy::Conservative };

    // Automatic mode takes the largest batch that is both useful and allowed;
    // a fixed size is an upper bound, never permission to exceed a limit.
    bool   automatic_batch_size { true };
    size_t max_layers_per_batch { 8 };

    double max_z_lead_mm { 1.0 };
    size_t max_z_lead_layers { 20 };

    double min_component_separation_mm { 30.0 };

    // How long a component may sit untouched before it is come back to.
    double max_idle_s { 300.0 };

    double toolchange_cost_s { 25.0 };
    // What it costs to pick a component back up: reprime, reheat, restart.
    double reprime_cost_s { 8.0 };
    // Batching has to beat the ordinary schedule by at least this much.
    double toolchange_saving_threshold_s { 0.0 };

    // Returning to a lagging component means going back down. Off by default.
    bool allow_downward_z { false };

    // Whether ruling batching out may quietly produce the ordinary schedule,
    // or must block instead.
    bool allow_reported_fallback { true };

    size_t max_operations { 20000 };
    double tolerance_mm { 1e-6 };
};

struct ScheduledOperation
{
    size_t component { 0 };
    size_t layer { 0 };
    int    tool { 0 };

    double z_bottom_mm { 0. };
    double z_top_mm { 0. };
    double duration_s { 0. };

    bool starts_batch { false };
    bool toolchange_before { false };
    // True when this operation prints below the highest z reached so far.
    bool downward_move { false };

    double idle_before_s { 0. };
};

struct ScheduleMetrics
{
    size_t toolchanges { 0 };
    size_t downward_moves { 0 };
    double max_z_lead_mm { 0. };
    size_t max_z_lead_layers { 0 };
    double total_time_s { 0. };
    double max_idle_s { 0. };

    // The closest two components come to each other in xy.
    std::optional<double> clearance_margin_mm;
};

struct BatchPlan
{
    ScheduleMode mode { ScheduleMode::Legacy };
    BatchVerdict verdict { BatchVerdict::Disabled };

    std::vector<ScheduledOperation> operations;

    ScheduleMetrics metrics;
    // The ordinary by-layer schedule, always computed, so the comparison the
    // spec asks for is always available.
    ScheduleMetrics legacy_metrics;

    std::optional<long>   toolchanges_saved;
    std::optional<double> time_saved_s;

    // Why each batch stopped where it did.
    std::vector<std::string> limits;
    std::vector<std::string> notes;
    std::string              error;
};

// The ordinary by-layer schedule: every component's layers interleaved in z
// order, which is what TigerSlicer does without this mode.
std::vector<ScheduledOperation> ordinary_schedule(
    const std::vector<BatchComponent> &components);

// The complete-object schedule: each component printed all the way through
// before the next one is started. It is the other thing bounded batching has
// to be compared against - it has the fewest toolchanges of any schedule and
// the worst collision exposure, which is exactly why it is not the default.
std::vector<ScheduledOperation> complete_object_schedule(
    const std::vector<BatchComponent> &components);

// Measure any of the three schedules on the same terms, so the comparison is
// a comparison rather than three differently-computed numbers.
ScheduleMetrics measure_schedule(const std::vector<ScheduledOperation> &operations,
                                 const std::vector<BatchComponent>     &components,
                                 const BatchingOptions                 &options);

// Plan a bounded lead/lag schedule.
//
// Groups consecutive layers of one component under one tool while limiting how
// far it may advance above any other component, in millimetres and in layers.
// Automatic mode takes the largest batch that is both useful and allowed; a
// fixed batch size is an upper bound and never permission to exceed a
// dependency, a lead limit or a separation requirement.
//
// Batching requires the Phase 25A decomposition to have proven every component
// independent. Where it has not, or where the components are too close, or
// where returning to a lagging component is not permitted, batching is ruled
// out - and it produces the ordinary schedule only when a reported fallback is
// allowed, otherwise it blocks rather than exporting something unverified.
//
// The result always carries the ordinary schedule's metrics alongside its own,
// so the two can be compared rather than taken on trust.
BatchPlan plan_bounded_batches(const std::vector<BatchComponent> &components,
                               const ComponentDecomposition      &decomposition,
                               const BatchingOptions             &options);

} // namespace Slic3r

#endif // slic3r_BoundedBatchScheduler_hpp_
