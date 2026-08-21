#include "MSlicerResinPrint.hpp"

#include "Model.hpp"
#include "ResinProfileMigration.hpp"

#include <boost/nowide/fstream.hpp>

namespace Slic3r {

namespace {

bool same_native_resin_input(const Model &old_model, const Model &new_model)
{
    if (old_model.id() != new_model.id() || old_model.objects.size() != new_model.objects.size())
        return false;
    for (size_t object_idx = 0; object_idx < old_model.objects.size(); ++object_idx) {
        const ModelObject &old_object = *old_model.objects[object_idx];
        const ModelObject &new_object = *new_model.objects[object_idx];
        if (old_object.id() != new_object.id() || old_object.name != new_object.name ||
            old_object.input_file != new_object.input_file || old_object.printable != new_object.printable ||
            !old_object.config.timestamp_matches(new_object.config) ||
            old_object.sla_points_status != new_object.sla_points_status ||
            old_object.sla_support_points != new_object.sla_support_points ||
            old_object.sla_drain_holes != new_object.sla_drain_holes ||
            old_object.volumes.size() != new_object.volumes.size() ||
            old_object.instances.size() != new_object.instances.size())
            return false;
        for (size_t volume_idx = 0; volume_idx < old_object.volumes.size(); ++volume_idx) {
            const ModelVolume &old_volume = *old_object.volumes[volume_idx];
            const ModelVolume &new_volume = *new_object.volumes[volume_idx];
            if (old_volume.id() != new_volume.id() || old_volume.type() != new_volume.type() ||
                old_volume.get_mesh_shared_ptr().get() != new_volume.get_mesh_shared_ptr().get() ||
                !old_volume.get_matrix().isApprox(new_volume.get_matrix()))
                return false;
        }
        for (size_t instance_idx = 0; instance_idx < old_object.instances.size(); ++instance_idx) {
            const ModelInstance &old_instance = *old_object.instances[instance_idx];
            const ModelInstance &new_instance = *new_object.instances[instance_idx];
            if (old_instance.id() != new_instance.id() || old_instance.printable != new_instance.printable ||
                old_instance.print_volume_state != new_instance.print_volume_state ||
                !old_instance.get_matrix().isApprox(new_instance.get_matrix()))
                return false;
        }
    }
    return true;
}

} // namespace

void MSlicerResinPrint::clear()
{
    std::lock_guard<std::mutex> state_lock(state_mutex());
    std::lock_guard<std::mutex> job_lock(m_job_mutex);
    m_job.reset();
    m_model.clear_objects();
    m_full_print_config.clear();
    m_finished.store(false, std::memory_order_release);
    restart();
}

bool MSlicerResinPrint::empty() const
{
    for (const ModelObject *object : m_model.objects)
        for (const ModelInstance *instance : object->instances)
            if (instance->is_printable())
                return false;
    return true;
}

std::vector<ObjectID> MSlicerResinPrint::print_object_ids() const
{
    std::vector<ObjectID> ids;
    ids.reserve(m_model.objects.size());
    for (const ModelObject *object : m_model.objects)
        ids.emplace_back(object->id());
    return ids;
}

StringObjectException MSlicerResinPrint::validate(std::vector<StringObjectException> *, Polygons *, std::vector<std::pair<Polygon, float>> *) const
{
    try {
        if (const std::string error = validate_resin_profile_contract(m_full_print_config); !error.empty())
            return { error };
        MSlicerResinJob::validate_model_input(m_model, m_full_print_config);
        MSlicerResinJob::validate(MSlicerResinJob::settings_from_config(m_full_print_config));
        (void)MSlicerResinJob::format_from_config(m_full_print_config);
        if (empty())
            return { "The Resin project has no printable model instance fully inside the build volume." };
        // No support gate: MSLA supports exist when they have been generated
        // in the Supports workspace and not otherwise. There is no separate
        // switch that could disagree with what is on the plate.
        return {};
    } catch (const std::exception &error) {
        return { error.what() };
    }
}

PrintBase::ApplyStatus MSlicerResinPrint::apply(const Model &model, DynamicPrintConfig config, bool /*extruder_applied*/)
{
    std::lock_guard<std::mutex> lock(state_mutex());
    if (same_native_resin_input(m_model, model) &&
        m_full_print_config.diff(config).empty() && config.diff(m_full_print_config).empty())
        return APPLY_STATUS_UNCHANGED;
    // BackgroundSlicingProcess::stop_internal temporarily unlocks this mutex
    // while waiting for the worker. Match the PrintBase invalidation contract
    // by invoking it only while the state mutex is owned.
    call_cancel_callback();
    m_model.assign_copy(model);
    m_full_print_config = std::move(config);
    {
        std::lock_guard<std::mutex> job_lock(m_job_mutex);
        m_job.reset();
    }
    m_finished.store(false, std::memory_order_release);
    restart();
    return APPLY_STATUS_INVALIDATED;
}

void MSlicerResinPrint::set_task(const TaskParams &params)
{
    if (params.single_model_object.valid() || params.single_model_instance_only ||
        params.to_object_step >= 0 || params.to_print_step >= 0)
        throw InvalidArgument("Partial legacy SLA processing steps are unavailable with the MSlicer Resin engine.");
}

void MSlicerResinPrint::process(long long *, bool)
{
    throw_if_canceled();
    if (const std::string error = validate().string; !error.empty())
        throw SlicingError(error);
    throw_if_canceled();
    set_status(0, "Preparing MSlicer Resin input");
    auto job = std::make_shared<MSlicerResinJob>(MSlicerResinJob::settings_from_config(m_full_print_config));
    // Prepared support geometry is part of the plate whenever it exists.
    // Passing the configuration prepares each object for printing: hollowing
    // and drain-hole drilling happen here, on the slicing copy only.
    job->add_model(m_model, true, &m_full_print_config);
    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        m_job = job;
    }
    try {
        throw_if_canceled();
        set_status(10, "Slicing Resin layers with MSlicer");
        job->slice();
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        if (m_job == job)
            m_job.reset();
        m_finished.store(false, std::memory_order_release);
        throw;
    }
    m_finished.store(true, std::memory_order_release);
    set_status(100, "MSlicer Resin slicing complete");
}

std::string MSlicerResinPrint::output_filename(const std::string &filename_base) const
{
    const ts_mslicer_format format = MSlicerResinJob::format_from_config(m_full_print_config);
    const char *extension = MSlicerResinJob::format_extension(format);
    if (extension == nullptr)
        throw InvalidArgument("The selected MSlicer Resin output format has no file extension.");
    return PrintBase::output_filename(m_full_print_config.opt_string("output_filename_format"),
                                      extension, filename_base);
}

std::vector<uint8_t> MSlicerResinPrint::encode() const
{
    std::shared_ptr<const MSlicerResinJob> completed_job;
    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        completed_job = m_job;
    }
    if (!m_finished.load(std::memory_order_acquire) || !completed_job)
        throw ExportError("The MSlicer Resin job has not finished successfully.");
    return completed_job->encode(MSlicerResinJob::format_from_config(m_full_print_config));
}

std::shared_ptr<const MSlicerResinJob> MSlicerResinPrint::job() const
{
    if (!m_finished.load(std::memory_order_acquire))
        return nullptr;
    std::lock_guard<std::mutex> lock(m_job_mutex);
    return m_job;
}

void MSlicerResinPrint::request_cancel()
{
    std::shared_ptr<MSlicerResinJob> active_job;
    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        active_job = m_job;
    }
    if (active_job)
        active_job->request_cancel();
}

void MSlicerResinPrint::export_print(const std::string &path) const
{
    const std::vector<uint8_t> artifact = encode();
    if (artifact.empty())
        throw ExportError("MSlicer produced an empty Resin artifact.");
    boost::nowide::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw ExportError("Could not open the Resin output file: " + path);
    stream.write(reinterpret_cast<const char *>(artifact.data()), std::streamsize(artifact.size()));
    stream.close();
    if (!stream)
        throw ExportError("Writing the Resin output file failed: " + path);
}

} // namespace Slic3r
