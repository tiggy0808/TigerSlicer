///|/ TigerSlicer Phase 9: mixed-nozzle support planning and validation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "MixedNozzleSupport.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

const char *support_style_name(SupportStyle style)
{
    switch (style) {
    case SupportStyle::Traditional: return "traditional";
    case SupportStyle::Organic:     return "organic";
    }
    return "unknown";
}

const char *support_part_name(SupportPart part)
{
    switch (part) {
    case SupportPart::Body:      return "body";
    case SupportPart::Interface: return "interface";
    case SupportPart::Raft:      return "raft";
    }
    return "unknown";
}

const char *support_issue_severity_name(SupportIssueSeverity severity)
{
    switch (severity) {
    case SupportIssueSeverity::Info:    return "info";
    case SupportIssueSeverity::Warning: return "warning";
    case SupportIssueSeverity::Blocked: return "blocked";
    }
    return "unknown";
}

bool SupportPlan::is_valid() const
{
    if (!error.empty())
        return false;
    for (const SupportIssue &issue : issues)
        if (issue.severity == SupportIssueSeverity::Blocked)
            return false;
    return true;
}

const SupportPartPlan *SupportPlan::part(SupportPart which) const
{
    for (const SupportPartPlan &plan : parts)
        if (plan.part == which)
            return &plan;
    return nullptr;
}

namespace {

std::string tool_label(int tool)
{
    return "T" + std::to_string(tool);
}

std::string mm(double value, int precision = 2)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value << " mm";
    return out.str();
}

const SupportToolProfile *find(const std::vector<SupportToolProfile> &tools, int tool)
{
    for (const SupportToolProfile &profile : tools)
        if (profile.tool == tool)
            return &profile;
    return nullptr;
}

// Two heights synchronize when the finer one divides the coarser one a whole
// number of times: that is the only way the two grids can share a plane.
bool heights_synchronize(double coarse, double fine, double resolution)
{
    if (coarse <= 0. || fine <= 0.)
        return false;
    if (fine > coarse + resolution)
        return false;
    const double ratio   = coarse / fine;
    const double nearest = std::round(ratio);
    if (nearest < 1.)
        return false;
    return std::abs(coarse - nearest * fine) <= resolution;
}

} // namespace

SupportPlan plan_mixed_nozzle_support(const SupportRequest                  &request,
                                      const std::vector<SupportToolProfile> &tools)
{
    SupportPlan plan;

    if (tools.empty()) {
        plan.error = "no tool profiles were supplied";
        return plan;
    }
    if (!request.body_tool.has_value() && !request.interface_tool.has_value() &&
        !request.raft_tool.has_value()) {
        plan.error = "no support part was assigned a tool";
        return plan;
    }

    auto add_issue = [&](SupportIssueSeverity severity, std::optional<SupportPart> part,
                         const std::string &message) {
        plan.issues.push_back({ severity, part, message });
    };

    // --- resolve each part with its own tool --------------------------------
    auto resolve = [&](SupportPart part, const std::optional<int> &assigned,
                       double layer_height_mm) {
        SupportPartPlan resolved;
        resolved.part            = part;
        resolved.layer_height_mm = layer_height_mm;
        if (!assigned.has_value()) {
            plan.parts.push_back(resolved);
            return;
        }

        const SupportToolProfile *profile = find(tools, *assigned);
        if (profile == nullptr) {
            add_issue(SupportIssueSeverity::Blocked, part,
                      "the " + std::string(support_part_name(part)) + " is assigned to " +
                          tool_label(*assigned) + ", which has no profile");
            plan.parts.push_back(resolved);
            return;
        }

        // The part is generated with this tool's real nozzle and width. It is
        // never narrowed to the finest nozzle on the machine.
        resolved.tool               = profile->tool;
        resolved.nozzle_diameter_mm = profile->nozzle_diameter_mm;
        resolved.extrusion_width_mm = profile->extrusion_width_mm > 0.
                                          ? profile->extrusion_width_mm
                                          : profile->nozzle_diameter_mm;
        resolved.material           = profile->material;
        resolved.generated          = true;

        if (layer_height_mm <= 0.)
            add_issue(SupportIssueSeverity::Blocked, part,
                      "the " + std::string(support_part_name(part)) +
                          " has no layer height");
        else {
            if (profile->min_layer_height_mm > 0. &&
                layer_height_mm + request.resolution_mm < profile->min_layer_height_mm)
                add_issue(SupportIssueSeverity::Blocked, part,
                          tool_label(profile->tool) + " cannot print the " +
                              support_part_name(part) + " at " + mm(layer_height_mm) +
                              "; its minimum is " + mm(profile->min_layer_height_mm));
            if (profile->max_layer_height_mm > 0. &&
                layer_height_mm > profile->max_layer_height_mm + request.resolution_mm)
                add_issue(SupportIssueSeverity::Blocked, part,
                          tool_label(profile->tool) + " cannot print the " +
                              support_part_name(part) + " at " + mm(layer_height_mm) +
                              "; its maximum is " + mm(profile->max_layer_height_mm));
        }

        plan.parts.push_back(resolved);
    };

    resolve(SupportPart::Body, request.body_tool, request.body_layer_height_mm);
    resolve(SupportPart::Interface, request.interface_tool, request.interface_layer_height_mm);
    resolve(SupportPart::Raft, request.raft_tool, request.raft_layer_height_mm);

    const SupportPartPlan *body      = plan.part(SupportPart::Body);
    const SupportPartPlan *interface_ = plan.part(SupportPart::Interface);
    const SupportPartPlan *raft      = plan.part(SupportPart::Raft);

    // --- contact distance ----------------------------------------------------
    const SupportToolProfile *interface_profile =
        request.interface_tool.has_value() ? find(tools, *request.interface_tool) : nullptr;
    const bool soluble_interface = interface_profile != nullptr && interface_profile->soluble;

    if (request.contact_distance_mm <= 0.) {
        if (!soluble_interface)
            // A zero gap with a non-soluble interface fuses the support to the
            // part. That is not a warning; the part cannot be recovered.
            add_issue(SupportIssueSeverity::Blocked, SupportPart::Interface,
                      "a zero contact distance requires a soluble interface, but " +
                          (interface_profile == nullptr
                               ? std::string("no interface tool is assigned")
                               : tool_label(interface_profile->tool) + " is loaded with " +
                                     (interface_profile->material.empty()
                                          ? std::string("an unreported material")
                                          : interface_profile->material)));
    } else if (soluble_interface) {
        add_issue(SupportIssueSeverity::Info, SupportPart::Interface,
                  "a soluble interface is assigned, so the " + mm(request.contact_distance_mm) +
                      " contact gap could be closed for a better surface");
    }

    // --- synchronization between parts ---------------------------------------
    if (body != nullptr && body->generated && interface_ != nullptr && interface_->generated) {
        const double coarse = std::max(body->layer_height_mm, interface_->layer_height_mm);
        const double fine   = std::min(body->layer_height_mm, interface_->layer_height_mm);
        if (!heights_synchronize(coarse, fine, request.resolution_mm))
            add_issue(SupportIssueSeverity::Blocked, SupportPart::Interface,
                      "the body at " + mm(body->layer_height_mm) + " and the interface at " +
                          mm(interface_->layer_height_mm) +
                          " share no common plane, so there is no valid transition between them");
        else if (std::abs(coarse - fine) > request.resolution_mm)
            plan.toolchanges_per_interface_band =
                2;   // into the interface tool and back out again
    }

    if (raft != nullptr && raft->generated && body != nullptr && body->generated) {
        const double coarse = std::max(raft->layer_height_mm, body->layer_height_mm);
        const double fine   = std::min(raft->layer_height_mm, body->layer_height_mm);
        if (!heights_synchronize(coarse, fine, request.resolution_mm))
            add_issue(SupportIssueSeverity::Blocked, SupportPart::Raft,
                      "the raft at " + mm(raft->layer_height_mm) + " and the body at " +
                          mm(body->layer_height_mm) + " share no common plane");
    }

    // --- contact quality ------------------------------------------------------
    const SupportPartPlan *contact = (interface_ != nullptr && interface_->generated)
                                         ? interface_
                                         : body;
    if (contact != nullptr && contact->generated && request.minimum_contact_width_mm > 0.) {
        if (contact->extrusion_width_mm > request.minimum_contact_width_mm + request.resolution_mm)
            add_issue(SupportIssueSeverity::Warning, contact->part,
                      "the " + std::string(support_part_name(contact->part)) + " extrudes at " +
                          mm(contact->extrusion_width_mm) + ", wider than the " +
                          mm(request.minimum_contact_width_mm) +
                          " feature it has to make contact with");
    }

    if (request.surface_support_required) {
        if (interface_ == nullptr || !interface_->generated)
            add_issue(SupportIssueSeverity::Blocked, SupportPart::Interface,
                      "a finished supported surface was required but no interface is generated");
        else if (body != nullptr && body->generated &&
                 interface_->extrusion_width_mm >= body->extrusion_width_mm)
            add_issue(SupportIssueSeverity::Warning, SupportPart::Interface,
                      "the interface is no finer than the body, so a finished supported "
                      "surface is unlikely");
    }

    // --- collision clearance ---------------------------------------------------
    if (request.minimum_clearance_mm.has_value() && body != nullptr && body->generated) {
        if (body->nozzle_diameter_mm > *request.minimum_clearance_mm + request.resolution_mm)
            add_issue(SupportIssueSeverity::Blocked, SupportPart::Body,
                      "the body's " + mm(body->nozzle_diameter_mm) +
                          " nozzle does not fit the " + mm(*request.minimum_clearance_mm) +
                          " clearance it must reach into");
    } else if (!request.minimum_clearance_mm.has_value()) {
        // Not checked is not the same as fine.
        add_issue(SupportIssueSeverity::Info, std::nullopt,
                  "no minimum clearance was supplied, so nozzle-to-part collision could not be "
                  "checked");
    }

    // --- material transition ----------------------------------------------------
    if (body != nullptr && body->generated && interface_ != nullptr && interface_->generated &&
        body->tool != interface_->tool) {
        if (body->material.empty() || interface_->material.empty())
            add_issue(SupportIssueSeverity::Warning, std::nullopt,
                      "the body and interface use different tools but at least one has not "
                      "reported its material, so their transition could not be checked");
        else if (body->material != interface_->material)
            add_issue(SupportIssueSeverity::Info, std::nullopt,
                      "each interface band costs a " + body->material + " -> " +
                          interface_->material + " transition and back");
    }

    // --- stability --------------------------------------------------------------
    if (request.supported_area_mm2 > 0. && body != nullptr && body->generated) {
        const double footprint = body->extrusion_width_mm * body->extrusion_width_mm;
        if (footprint > request.supported_area_mm2)
            add_issue(SupportIssueSeverity::Warning, SupportPart::Body,
                      "a single " + mm(body->extrusion_width_mm) +
                          " extrusion already covers more than the supported area, so the "
                          "support may not be stable");
    }

    if (request.style == SupportStyle::Organic && body != nullptr && body->generated &&
        request.minimum_clearance_mm.has_value() &&
        body->nozzle_diameter_mm > *request.minimum_clearance_mm * 0.5)
        add_issue(SupportIssueSeverity::Warning, SupportPart::Body,
                  "organic supports taper to a tip, which a " + mm(body->nozzle_diameter_mm) +
                      " nozzle cannot form inside this clearance");

    return plan;
}

} // namespace Slic3r
