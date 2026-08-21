#include "ResinRasterPreview.hpp"

#include "GUI_App.hpp"

#include <wx/bmpbuttn.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {

namespace {

wxString duration_text(double seconds)
{
    const uint64_t rounded = uint64_t(std::max(0., std::round(seconds)));
    const uint64_t hours = rounded / 3600;
    const uint64_t minutes = (rounded % 3600) / 60;
    const uint64_t secs = rounded % 60;
    return hours > 0 ? wxString::Format("%lluh %02llum", hours, minutes) :
           minutes > 0 ? wxString::Format("%llum %02llus", minutes, secs) :
                         wxString::Format("%llus", secs);
}

MSlicerExposureSettings layer_exposure(const MSlicerResinSettings &settings, uint32_t layer)
{
    if (layer < settings.bottom_layers)
        return settings.bottom;
    if (settings.transition_layers == 0 ||
        layer >= settings.bottom_layers + settings.transition_layers)
        return settings.normal;

    const float t = float(layer - settings.bottom_layers) / float(settings.transition_layers);
    auto lerp = [t](float first, float normal) { return first + (normal - first) * t; };
    MSlicerExposureSettings value;
    value.exposure_seconds = lerp(settings.bottom.exposure_seconds, settings.normal.exposure_seconds);
    value.delay_seconds = lerp(settings.bottom.delay_seconds, settings.normal.delay_seconds);
    value.pwm = uint8_t(std::lround(lerp(float(settings.bottom.pwm), float(settings.normal.pwm))));
    value.lift_distance_mm = lerp(settings.bottom.lift_distance_mm, settings.normal.lift_distance_mm);
    value.lift_speed_mm_min = lerp(settings.bottom.lift_speed_mm_min, settings.normal.lift_speed_mm_min);
    value.retract_distance_mm = lerp(settings.bottom.retract_distance_mm, settings.normal.retract_distance_mm);
    value.retract_speed_mm_min = lerp(settings.bottom.retract_speed_mm_min, settings.normal.retract_speed_mm_min);
    return value;
}

} // namespace

ResinRasterPreviewPanel::ResinRasterPreviewPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
{
    m_title = new wxStaticText(this, wxID_ANY, "MSlicer Raster");
    wxFont title_font = m_title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_title->SetFont(title_font);

    m_coordinate_note = new wxStaticText(this, wxID_ANY, "Exact grayscale mask | printer-ready output coordinates");

    m_bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(1, 1));
    m_layer_label = new wxStaticText(this, wxID_ANY, "No Resin raster available");
    m_metrics_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_timeline_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_diagnostics_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_metrics_label->Wrap(-1);
    m_timeline_label->Wrap(-1);
    m_diagnostics_label->Wrap(-1);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    const int gap = std::max(6, wxGetApp().em_unit() / 2);
    sizer->Add(m_title, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    sizer->Add(m_coordinate_note, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    sizer->Add(m_bitmap, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, gap);
    sizer->Add(m_layer_label, 0, wxLEFT | wxRIGHT, gap);
    sizer->Add(m_metrics_label, 0, wxEXPAND | wxALL, gap);
    sizer->Add(m_timeline_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);
    sizer->Add(m_diagnostics_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);
    SetSizer(sizer);

    Bind(wxEVT_SIZE, &ResinRasterPreviewPanel::on_size, this);
    msw_rescale();
    sys_color_changed();
    clear();
}

void ResinRasterPreviewPanel::set_job(std::shared_ptr<const MSlicerResinJob> job,
                                      uint64_t output_size_bytes)
{
    m_job = std::move(job);
    m_output_size_bytes = output_size_bytes;
    m_layer = 0;
    if (!m_job || m_job->layer_count() == 0) {
        clear();
        return;
    }
    update_layer();
}

void ResinRasterPreviewPanel::clear()
{
    m_job.reset();
    m_output_size_bytes = 0;
    m_layer = 0;
    m_layer_pixels.clear();
    m_layer_label->SetLabel("No Resin raster available");
    m_metrics_label->SetLabel("Slice a model in the Resin workspace to inspect exact engine masks and estimates.");
    m_timeline_label->SetLabel(wxEmptyString);
    m_diagnostics_label->SetLabel(wxEmptyString);
    m_bitmap->SetBitmap(wxBitmap(1, 1));
}

void ResinRasterPreviewPanel::set_layer(uint32_t layer)
{
    if (!m_job || m_job->layer_count() == 0)
        return;
    m_layer = std::min(layer, m_job->layer_count() - 1);
    update_layer();
}

void ResinRasterPreviewPanel::update_layer()
{
    if (!m_job)
        return;
    const MSlicerResinSettings &settings = m_job->settings();
    const MSlicerResinMetrics metrics = m_job->metrics();
    const Vec3f input_size = m_job->input_size_mm();
    const Vec3f input_min = m_job->input_min_mm();
    const Vec3f input_max = m_job->input_max_mm();
    const MSlicerExposureSettings exposure = layer_exposure(settings, m_layer);
    const char *kind = m_layer < settings.bottom_layers ? "Bottom" :
        m_layer < settings.bottom_layers + settings.transition_layers ? "Transition" : "Normal";

    m_layer_label->SetLabel(wxString::Format("Layer %u of %u | Z %.3f mm | %s",
        m_layer + 1, m_job->layer_count(), double(settings.layer_height_mm) * (m_layer + 1), kind));
    m_metrics_label->SetLabel(wxString::Format(
        "%.2f ml resin\n%s estimated | %u layers\n%.2f MiB encoded output\n"
        "Input size %.2f x %.2f x %.2f mm | mesh %.2f ml\n"
        "Bounds X %.2f..%.2f | Y %.2f..%.2f | Z %.2f..%.2f mm",
        metrics.resin_volume_mm3 / 1000., duration_text(metrics.estimated_seconds),
        metrics.layer_count, double(m_output_size_bytes) / (1024. * 1024.),
        input_size.x(), input_size.y(), input_size.z(), m_job->input_mesh_volume_mm3() / 1000.,
        input_min.x(), input_max.x(), input_min.y(), input_max.y(), input_min.z(), input_max.z()));
    m_timeline_label->SetLabel(wxString::Format(
        "Exposure %.2fs | wait %.2fs | PWM %u\nLift %.2f mm @ %.0f mm/min\nRetract %.2f mm @ %.0f mm/min",
        exposure.exposure_seconds, exposure.delay_seconds, unsigned(exposure.pwm),
        exposure.lift_distance_mm, exposure.lift_speed_mm_min,
        exposure.retract_distance_mm, exposure.retract_speed_mm_min));
    const std::string diagnostics = m_job->diagnostics_json();
    m_diagnostics_label->SetLabel(diagnostics.find('{') == std::string::npos ? "Validation: no engine warnings" :
        wxString::FromUTF8("Engine diagnostics: " + diagnostics));
    m_layer_pixels = m_job->layer(m_layer);
    update_bitmap(m_layer_pixels);
    Layout();
}

void ResinRasterPreviewPanel::update_bitmap(const std::vector<uint8_t> &pixels)
{
    if (!m_job || m_updating_bitmap)
        return;
    m_updating_bitmap = true;
    const MSlicerResinSettings &settings = m_job->settings();
    const int width = int(settings.portrait ? settings.resolution_y : settings.resolution_x);
    const int height = int(settings.portrait ? settings.resolution_x : settings.resolution_y);
    if (width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height)) {
        m_updating_bitmap = false;
        return;
    }

    wxImage image(width, height, false);
    unsigned char *rgb = image.GetData();
    for (size_t i = 0; i < pixels.size(); ++i)
        rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = pixels[i];

    const int max_width = std::max(1, GetClientSize().x - wxGetApp().em_unit());
    const int max_height = std::max(1, 14 * wxGetApp().em_unit());
    const double scale = std::min(double(max_width) / width, double(max_height) / height);
    const int target_width = std::max(1, int(std::lround(width * scale)));
    const int target_height = std::max(1, int(std::lround(height * scale)));
    m_bitmap->SetBitmap(wxBitmap(image.Scale(target_width, target_height, wxIMAGE_QUALITY_HIGH)));
    m_updating_bitmap = false;
}

void ResinRasterPreviewPanel::on_size(wxSizeEvent &event)
{
    if (m_job && !m_layer_pixels.empty())
        update_bitmap(m_layer_pixels);
    event.Skip();
}

void ResinRasterPreviewPanel::msw_rescale()
{
    SetMinSize(wxSize(24 * wxGetApp().em_unit(), -1));
    if (m_bitmap)
        m_bitmap->SetMinSize(wxSize(20 * wxGetApp().em_unit(), 12 * wxGetApp().em_unit()));
    Layout();
}

void ResinRasterPreviewPanel::sys_color_changed()
{
    wxGetApp().UpdateDarkUI(this);
    SetBackgroundColour(wxColour(21, 21, 24));
    if (m_title) m_title->SetForegroundColour(wxColour(246, 165, 193));
    if (m_coordinate_note) m_coordinate_note->SetForegroundColour(wxColour(166, 162, 168));
    if (m_layer_label) m_layer_label->SetForegroundColour(wxColour(250, 250, 250));
    if (m_metrics_label) m_metrics_label->SetForegroundColour(wxColour(250, 250, 250));
    if (m_timeline_label) m_timeline_label->SetForegroundColour(wxColour(246, 165, 193));
    if (m_diagnostics_label) m_diagnostics_label->SetForegroundColour(wxColour(166, 162, 168));
    Refresh();
}

} // namespace Slic3r::GUI
