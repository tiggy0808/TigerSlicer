#include "MSlicerSDCPHost.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "../../mslicer/cpp/MSlicerBridge.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

namespace Slic3r {

namespace {

struct UploadCallbacks {
    const PrintHost::ProgressFn *progress_fn { nullptr };
    std::atomic_bool cancelled { false };
    std::mutex callback_mutex;
    uint64_t completed { 0 };
    uint64_t total { 0 };
    std::string empty_buffer;

    void poll(uint64_t done, uint64_t size) noexcept
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        completed = done;
        total = size;
        if (cancelled.load(std::memory_order_acquire) || progress_fn == nullptr)
            return;
        bool cancel = false;
        try {
            (*progress_fn)(Http::Progress(0, 0, size_t(total), size_t(completed), empty_buffer), cancel);
        } catch (...) {
            cancel = true;
        }
        if (cancel)
            cancelled.store(true, std::memory_order_release);
    }
};

uint8_t is_cancelled(void *user_data) noexcept
{
    auto *callbacks = static_cast<UploadCallbacks *>(user_data);
    callbacks->poll(callbacks->completed, callbacks->total);
    return callbacks->cancelled.load(std::memory_order_acquire) ? 1 : 0;
}

void report_progress(void *user_data, uint64_t completed, uint64_t total) noexcept
{
    static_cast<UploadCallbacks *>(user_data)->poll(completed, total);
}

std::vector<uint8_t> read_artifact(const boost::filesystem::path &path)
{
    std::ifstream input(path.string(), std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("The Resin artifact could not be opened for SDCP upload.");
    const std::streamoff end = input.tellg();
    if (end <= 0 || uint64_t(end) > uint64_t(std::numeric_limits<uint32_t>::max()))
        throw std::runtime_error("The Resin artifact is empty or exceeds the SDCP 4 GiB size limit.");
    std::vector<uint8_t> bytes(static_cast<size_t>(end), uint8_t(0));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), end))
        throw std::runtime_error("The Resin artifact could not be read completely for SDCP upload.");
    return bytes;
}

} // namespace

MSlicerSDCPHost::MSlicerSDCPHost(DynamicPrintConfig *config)
    : m_host(config->opt_string("print_host"))
    , m_resolution_x(uint32_t(std::max(0, config->opt_int("display_pixels_x"))))
    , m_resolution_y(uint32_t(std::max(0, config->opt_int("display_pixels_y"))))
{
}

bool MSlicerSDCPHost::test(wxString &message) const
{
    ts_mslicer_remote_probe_request request {};
    request.struct_size = sizeof(request);
    request.address = reinterpret_cast<const uint8_t *>(m_host.data());
    request.address_size = m_host.size();
    request.port = 3000;
    request.resolution_x = m_resolution_x;
    request.resolution_y = m_resolution_y;
    request.timeout_ms = 3000;
    try {
        const MSlicer::Buffer result = MSlicer::Remote::probe(request);
        message = wxString::FromUTF8(reinterpret_cast<const char *>(result.data()), result.size());
        return true;
    } catch (const std::exception &error) {
        message = wxString::FromUTF8(error.what());
        return false;
    }
}

wxString MSlicerSDCPHost::get_test_ok_msg() const
{
    return "The local SDCP Resin printer identity, capability, and resolution were verified.";
}

wxString MSlicerSDCPHost::get_test_failed_msg(wxString &message) const
{
    return "The local SDCP Resin printer could not be verified: " + message;
}

bool MSlicerSDCPHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    if (upload_data.post_action != PrintHostPostUploadAction::None) {
        error_fn("ChiTu SDCP upload does not start a print. Upload the file first, then explicitly start it at the printer.");
        return false;
    }
    try {
        const std::vector<uint8_t> artifact = read_artifact(upload_data.source_path);
        const std::string filename = upload_data.upload_path.filename().string();
        UploadCallbacks callbacks { &progress_fn };
        callbacks.total = artifact.size();
        callbacks.poll(0, artifact.size());

        ts_mslicer_remote_upload_request request {};
        request.struct_size = sizeof(request);
        request.address = reinterpret_cast<const uint8_t *>(m_host.data());
        request.address_size = m_host.size();
        request.filename = reinterpret_cast<const uint8_t *>(filename.data());
        request.filename_size = filename.size();
        request.artifact = artifact.data();
        request.artifact_size = artifact.size();
        request.port = 3000;
        request.resolution_x = m_resolution_x;
        request.resolution_y = m_resolution_y;
        request.probe_timeout_ms = 3000;
        request.connect_timeout_ms = 10000;
        request.transfer_timeout_ms = 300000;
        request.cancelled = is_cancelled;
        request.progress = report_progress;
        request.user_data = &callbacks;

        const MSlicer::Buffer receipt = MSlicer::Remote::upload(request);
        info_fn("SDCP upload", wxString::FromUTF8(reinterpret_cast<const char *>(receipt.data()), receipt.size()));
        return true;
    } catch (const MSlicer::Error &error) {
        if (error.status() != TS_MSLICER_CANCELLED)
            error_fn(wxString::FromUTF8(error.what()));
        return false;
    } catch (const std::exception &error) {
        error_fn(wxString::FromUTF8(error.what()));
        return false;
    }
}

} // namespace Slic3r
