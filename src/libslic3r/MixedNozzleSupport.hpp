///|/ TigerSlicer Phase 9: mixed-nozzle support planning and validation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_MixedNozzleSupport_hpp_
#define slic3r_MixedNozzleSupport_hpp_

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class SupportStyle : uint8_t
{
    Traditional,
    Organic,
};

const char *support_style_name(SupportStyle style);

// The three structurally distinct parts of a support, each of which may be
// printed by a different tool at its own layer height.
enum class SupportPart : uint8_t
{
    Body,
    Interface,
    Raft,
};

const char *support_part_name(SupportPart part);

// What a tool actually brings to support generation. Support is generated
// with these values, not with the smallest nozzle on the machine.
struct SupportToolProfile
{
    int         tool { 0 };
    double      nozzle_diameter_mm { 0. };
    double      extrusion_width_mm { 0. };
    double      max_flow_mm3_s { 0. };
    double      min_layer_height_mm { 0. };
    double      max_layer_height_mm { 0. };
    std::string material;
    bool        soluble { false };
};

struct SupportRequest
{
    SupportStyle style { SupportStyle::Traditional };

    // Tools assigned to each part. A part with no tool is not generated.
    std::optional<int> body_tool;
    std::optional<int> interface_tool;
    std::optional<int> raft_tool;

    // Independent heights per part, which is the point of the phase.
    double body_layer_height_mm { 0. };
    double interface_layer_height_mm { 0. };
    double raft_layer_height_mm { 0. };

    // Gap between the support and the part it holds up. Zero is only valid
    // with a soluble interface.
    double contact_distance_mm { 0. };

    // The overhang being held up.
    double supported_area_mm2 { 0. };
    // Narrowest feature the support has to make contact with.
    double minimum_contact_width_mm { 0. };
    // Smallest gap the support has to fit into.
    std::optional<double> minimum_clearance_mm;

    // Set when the supported surface must come out to a finished quality.
    bool surface_support_required { false };

    // Resolution used when checking that heights synchronize.
    double resolution_mm { 0.001 };
};

enum class SupportIssueSeverity : uint8_t
{
    Info,
    Warning,
    Blocked,
};

const char *support_issue_severity_name(SupportIssueSeverity severity);

struct SupportIssue
{
    SupportIssueSeverity severity { SupportIssueSeverity::Info };
    std::optional<SupportPart> part;
    std::string message;
};

// What one part will actually be printed with.
struct SupportPartPlan
{
    SupportPart part { SupportPart::Body };
    int         tool { -1 };
    double      nozzle_diameter_mm { 0. };
    double      extrusion_width_mm { 0. };
    double      layer_height_mm { 0. };
    std::string material;
    bool        generated { false };
};

struct SupportPlan
{
    std::vector<SupportPartPlan> parts;
    std::vector<SupportIssue>    issues;

    // How many body <-> interface transitions the plan implies per interface
    // band, so the toolchange cost of this support is visible rather than
    // discovered at export.
    int toolchanges_per_interface_band { 0 };

    std::string error;

    bool is_valid() const;
    const SupportPartPlan *part(SupportPart part) const;
};

// Plan and validate a mixed-nozzle support.
//
// Each part is generated with its assigned tool's real nozzle, width and
// grid. Nothing is forced to the smallest nozzle on the machine, and every
// constraint that fails is reported with the part responsible.
SupportPlan plan_mixed_nozzle_support(const SupportRequest                  &request,
                                      const std::vector<SupportToolProfile> &tools);

} // namespace Slic3r

#endif // slic3r_MixedNozzleSupport_hpp_
