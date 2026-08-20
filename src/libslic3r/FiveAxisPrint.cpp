///|/ TigerSlicer five-axis printing. See FiveAxisPrint.hpp.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FiveAxisPrint.hpp"

#include <algorithm>
#include <cstdio>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

namespace {

double object_option(const ModelObject &object, const char *key, double fallback)
{
    if (const ConfigOption *opt = object.config.option(key); opt != nullptr)
        return opt->getFloat();
    return fallback;
}

// Where the object sits on the plate, which is where its slice plane sits.
Vec2d instance_xy(const ModelObject &object)
{
    if (object.instances.empty())
        return Vec2d(0., 0.);
    const Vec3d offset = object.instances.front()->get_offset();
    return Vec2d(offset.x(), offset.y());
}

} // namespace

FiveAxisSegment five_axis_segment_of(const ModelObject &object)
{
    FiveAxisSegment segment;
    segment.theta = object_option(object, "five_axis_segment_theta", 0.);
    segment.phi   = object_option(object, "five_axis_segment_phi", 0.);
    const Vec2d xy = instance_xy(object);
    segment.origin = Vec3d(xy.x(), xy.y(),
                           object_option(object, "five_axis_segment_origin_z", 0.));
    return segment;
}

bool five_axis_model_has_tilted_segments(const Model &model)
{
    for (const ModelObject *object : model.objects) {
        if (object == nullptr || !object->printable || object->instances.empty())
            continue;
        if (std::abs(five_axis_segment_of(*object).theta) > EPSILON)
            return true;
    }
    return false;
}

FiveAxisPrintResult five_axis_slice(const Model              &model,
                                    const DynamicPrintConfig &config,
                                    std::function<void(const std::string &)> status)
{
    FiveAxisPrintResult result;
    const FiveAxisMachine machine = FiveAxisMachine::from_config(config);

    // Collect the printable objects and put the flat ones first: the plate
    // has to be built up from what rests on it before anything is tilted.
    std::vector<const ModelObject *> objects;
    for (const ModelObject *object : model.objects)
        if (object != nullptr && object->printable && !object->instances.empty() &&
            !object->volumes.empty())
            objects.push_back(object);

    if (objects.empty()) {
        result.error = "There is nothing on the plate to slice.";
        return result;
    }

    std::stable_sort(objects.begin(), objects.end(),
        [](const ModelObject *a, const ModelObject *b) {
            return std::abs(five_axis_segment_of(*a).theta) <
                   std::abs(five_axis_segment_of(*b).theta);
        });

    // Where the slicer's coordinates are measured from. Known before slicing,
    // because a segment is placed against it rather than read back off it.
    Vec2d bed_center(0., 0.);
    if (const auto *shape = config.opt<ConfigOptionPoints>("bed_shape");
        shape != nullptr && !shape->values.empty())
        bed_center = BoundingBoxf(shape->values).center();

    std::vector<FiveAxisSegmentProgram> programs;
    programs.reserve(objects.size());

    for (size_t k = 0; k < objects.size(); ++k) {
        const ModelObject    &object  = *objects[k];
        const FiveAxisSegment segment = five_axis_segment_of(object);

        if (const std::string bad = five_axis_validate_segment(segment, machine); !bad.empty()) {
            result.error = object.name.empty()
                ? bad : ("\"" + object.name + "\": " + bad);
            return result;
        }

        if (status) {
            char message[256];
            std::snprintf(message, sizeof(message), "Slicing segment %zu of %zu (%s)",
                          k + 1, objects.size(),
                          object.name.empty() ? "object" : object.name.c_str());
            status(message);
        }

        // One part, in one place: the segment is sliced on its own, keeping
        // whatever the user did to the object on the plate.
        Model one;
        ModelObject *copy = one.add_object(object);
        copy->config.reset();               // the segment keys are ours, not the slicer's
        const Transform3d placed =
            object.instances.front()->get_transformation().get_matrix();
        copy->clear_instances();
        ModelInstance *instance = copy->add_instance();

        // An upright opening segment is written out exactly as sliced, so it
        // has to be sliced exactly where it stands - composition mirrors this
        // condition. Anything else is stood up in its own frame and mapped
        // back, so where it was sliced does not matter, only that it is
        // recorded.
        const bool as_sliced = k == 0 && std::abs(segment.theta) < EPSILON;
        Vec3d placement = Vec3d::Zero();

        if (as_sliced) {
            instance->set_transformation(Geometry::Transformation(placed));
        } else {
            const Matrix3d rotation = five_axis_segment_rotation(
                segment.theta, segment.phi, machine.kinematics);
            // Local = R^T (p - S), taken about the segment's own origin - the
            // point on the plate the build direction is measured from. Carried
            // on the instance rather than baked into the mesh, so the slicer
            // sees the segment standing upright and the model is left alone.
            Transform3d to_local = Transform3d::Identity();
            to_local.linear() = rotation.transpose();
            to_local = to_local * Eigen::Translation3d(-segment.origin);
            instance->set_transformation(Geometry::Transformation(to_local * placed));

            // That frame has its origin wherever the part happened to be, and
            // once tilted it can sit under the bed. Move it somewhere the
            // slicer will accept - centred on the bed and resting on it.
            const BoundingBoxf3 box = copy->instance_bounding_box(0);
            placement = Vec3d(bed_center.x() - box.center().x(),
                              bed_center.y() - box.center().y(),
                              -box.min.z());
            // The centring is taken back off during composition; the drop is
            // not. It seats the segment on its own slice plane, so local z in
            // the sliced G-code is height above that plane. A segment that was
            // not already resting there has been moved, and that is said.
            if (std::abs(box.min.z()) > 0.5) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer),
                    "Segment %zu (%s) started %+.2f mm off its slice plane and has been "
                    "seated on it.", k, object.name.empty() ? "object" : object.name.c_str(),
                    box.min.z());
                result.warnings.push_back(buffer);
            }
            instance->set_transformation(Geometry::Transformation(
                Transform3d(Eigen::Translation3d(placement)) * to_local * placed));
        }

        // Slice it with the same configuration the plate uses.
        DynamicPrintConfig segment_config = config;
        // Arcs cannot be rotated into the machine frame, so they are turned
        // off here rather than refused later.
        segment_config.set_key_value("arc_fitting",
            new ConfigOptionEnum<ArcFittingType>(ArcFittingType::Disabled));

        // The job starts and ends once, not once per segment. Homing and the
        // prime line belong to the first segment, the shutdown to the last;
        // in between they would home into what is already printed.
        const bool last = k + 1 == objects.size();
        if (k > 0) {
            segment_config.set_key_value("start_gcode", new ConfigOptionString(""));
            segment_config.set_key_value("start_filament_gcode",
                new ConfigOptionStrings({ std::string() }));
        }
        if (!last) {
            segment_config.set_key_value("end_gcode", new ConfigOptionString(""));
            segment_config.set_key_value("end_filament_gcode",
                new ConfigOptionStrings({ std::string() }));
        }
        if (std::abs(segment.theta) > EPSILON) {
            // A skirt or brim rings the part at a radius; on a tilted table
            // that ring dives under the bed. The reorientation primes instead.
            segment_config.set_key_value("skirts", new ConfigOptionInt(0));
            segment_config.set_key_value("brim_type",
                new ConfigOptionEnum<BrimType>(btNoBrim));
            segment_config.set_key_value("brim_width", new ConfigOptionFloat(0.));
            // A wipe tower stands at fixed plate coordinates, which no longer
            // mean anything once the table has turned.
            segment_config.set_key_value("wipe_tower", new ConfigOptionBool(false));
        }

        Print print;
        std::string apply_error;
        try {
            print.apply(one, segment_config);
            if (const std::string invalid = print.validate(); !invalid.empty()) {
                result.error = "Segment " + std::to_string(k + 1) + ": " + invalid;
                return result;
            }
            print.set_status_silent();
            print.process();
        } catch (const std::exception &ex) {
            result.error = "Segment " + std::to_string(k + 1) + " could not be sliced: " + ex.what();
            return result;
        }

        const boost::filesystem::path temp =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path(
                "tigerslicer-5axis-%%%%-%%%%.gcode");
        try {
            print.export_gcode(temp.string(), nullptr, nullptr);
        } catch (const std::exception &ex) {
            result.error = "Segment " + std::to_string(k + 1) + " could not be written: " + ex.what();
            return result;
        }

        FiveAxisSegmentProgram program;
        program.segment = segment;
        program.bed_center = bed_center;
        // Composition subtracts the bed centre and this; together they are
        // the centring done above, so what is left is the segment's own frame
        // - or, for a segment printed as sliced, nothing at all. The z drop is
        // deliberately left in: it is what puts local z on the slice plane.
        program.local_offset = Vec3d(placement.x() - bed_center.x(),
                                     placement.y() - bed_center.y(), 0.);
        {
            boost::nowide::ifstream in(temp.string());
            program.gcode.assign(std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>());
        }
        boost::system::error_code ignore;
        boost::filesystem::remove(temp, ignore);

        if (program.gcode.empty()) {
            result.error = "Segment " + std::to_string(k + 1) + " produced no G-code.";
            return result;
        }

        programs.push_back(std::move(program));
    }

    FiveAxisComposeOptions options;
    options.relative_e = config.opt_bool("use_relative_e_distances");
    // Retraction is per extruder; the joins are written for the tool the
    // print starts on.
    if (const auto *retract = config.opt<ConfigOptionFloats>("retract_length");
        retract != nullptr && !retract->values.empty())
        options.retract_length = retract->values.front();

    if (status)
        status("Composing the machine program");

    const FiveAxisComposeResult composed = five_axis_compose(programs, machine, options);
    result.gcode = composed.gcode;
    result.error = composed.error;
    // Composition's warnings join the slicing ones rather than replacing them.
    result.warnings.insert(result.warnings.end(),
                           composed.warnings.begin(), composed.warnings.end());
    result.moves_transformed = composed.moves_transformed;
    result.max_z = composed.max_z;
    result.segments = programs.size();
    return result;
}

} // namespace Slic3r
