#ifndef slic3r_GUI_ResinRasterPreview_hpp_
#define slic3r_GUI_ResinRasterPreview_hpp_

#include "libslic3r/MSlicerResinJob.hpp"

#include <wx/panel.h>

#include <cstdint>
#include <memory>

class wxStaticBitmap;
class wxStaticText;

namespace Slic3r::GUI {

class ResinRasterPreviewPanel final : public wxPanel
{
public:
    explicit ResinRasterPreviewPanel(wxWindow *parent);

    void set_job(std::shared_ptr<const MSlicerResinJob> job, uint64_t output_size_bytes);
    void clear();
    void set_layer(uint32_t layer);
    void msw_rescale();
    void sys_color_changed();

private:
    void update_layer();
    void update_bitmap(const std::vector<uint8_t> &pixels);
    void on_size(wxSizeEvent &event);

    std::shared_ptr<const MSlicerResinJob> m_job;
    uint64_t m_output_size_bytes { 0 };
    uint32_t m_layer { 0 };
    std::vector<uint8_t> m_layer_pixels;
    wxStaticBitmap *m_bitmap { nullptr };
    wxStaticText *m_title { nullptr };
    wxStaticText *m_coordinate_note { nullptr };
    wxStaticText *m_layer_label { nullptr };
    wxStaticText *m_metrics_label { nullptr };
    wxStaticText *m_timeline_label { nullptr };
    wxStaticText *m_diagnostics_label { nullptr };
    bool m_updating_bitmap { false };
};

} // namespace Slic3r::GUI

#endif
