#ifndef slic3r_MSlicerResinPrint_hpp_
#define slic3r_MSlicerResinPrint_hpp_

#include "MSlicerResinJob.hpp"
#include "PrintBase.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace Slic3r {

// TigerSlicer-owned Resin print state. It preserves PrintBase integration and
// output-template behavior without invoking the legacy SLA processing engine.
class MSlicerResinPrint final : public PrintBase
{
public:
    PrinterTechnology technology() const noexcept override { return ptSLA; }
    void clear() override;
    bool empty() const override;
    std::vector<ObjectID> print_object_ids() const override;
    std::string validate(std::vector<std::string> *warnings = nullptr) const override;
    ApplyStatus apply(const Model &model, DynamicPrintConfig config,
                      std::vector<std::string> *warnings = nullptr,
                      const DynamicPrintConfig *original_config = nullptr) override;
    void set_task(const TaskParams &params) override;
    void process() override;
    void finalize() override {}
    void cleanup() override {}
    bool finished() const override { return m_finished.load(std::memory_order_acquire); }
    std::string output_filename(const std::string &filename_base = {}) const override;

    std::shared_ptr<const MSlicerResinJob> job() const;
    void request_cancel();
    std::vector<uint8_t> encode() const;
    void export_print(const std::string &path) const;

private:
    mutable std::mutex m_job_mutex;
    std::shared_ptr<MSlicerResinJob> m_job;
    std::atomic<bool> m_finished { false };
};

} // namespace Slic3r

#endif
