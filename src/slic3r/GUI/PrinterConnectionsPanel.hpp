///|/ TigerSlicer Phase N: Physical Printer workspace.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterConnectionsPanel_hpp_
#define slic3r_PrinterConnectionsPanel_hpp_

#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "slic3r/Utils/ConnectionsViewModel.hpp"
#include "slic3r/Utils/PrinterDiscovery.hpp"

class wxButton;
class wxSearchCtrl;
class wxSimplebook;
class wxSizer;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

namespace Slic3r {
namespace GUI {

class ConnectionSidebar;

// One row in the connections sidebar. A row is navigation only: selecting it
// never connects, uploads, moves, heats, or changes the active slicing preset.
struct ConnectionEntry
{
    enum class Section { Connected, Saved, Discovered, Offline };

    Section     section { Section::Discovered };
    std::string key;            // stable identity for selection/dedup
    std::string title;          // printer name shown to the user
    std::string subtitle;       // compact model/address line
    std::string status;         // actionable status badge text
    bool        saved { false };        // has a physical-printer profile
    bool        technology_known { false };
    PrinterTechnology technology { ptFFF };
    DiscoveredPrinter device;   // empty identity when saved-only
};

// The Physical Printer workspace: a persistent connections sidebar beside the
// regular printer interface. Available whether or not a physical-printer
// profile exists, so discovery never requires creating a profile first.
class PrinterConnectionsPanel : public wxPanel
{
public:
    explicit PrinterConnectionsPanel(wxWindow *parent);
    ~PrinterConnectionsPanel() override;

    // Begin/refresh discovery. Called when the tab becomes visible and on
    // explicit user refresh; never on construction alone.
    void start_discovery();
    // Reduce activity when the workspace is not visible.
    void stop_discovery();

    void on_activate();
    void on_deactivate();

    void msw_rescale();
    void sys_color_changed();

    // The existing connected-printer web interface lives inside this
    // workspace's main pane. It is constructed with the returned parent so
    // no runtime reparenting is required.
    wxWindow *regular_interface_parent() const;
    void attach_regular_interface(wxWindow *regular_page);
    void show_regular_interface();
    void show_discovery_interface();
    bool regular_interface_shown() const;

    // Rebuild rows from saved profiles plus current discovery results.
    void reload_entries();

    // Counts for the multi-printer overview strip (N19).
    struct OverviewCounts
    {
        int connected { 0 };
        int saved { 0 };
        int discovered { 0 };
        int offline { 0 };
        int needs_attention { 0 };
    };
    OverviewCounts overview_counts() const;

private:
    void build_layout();
    void apply_filter();
    void update_overview();
    // Describe a discovered device against installed profiles (N8). Reports
    // only; never selects or applies a preset.
    wxString describe_profile_match(const DiscoveredPrinter &device) const;
    void select_entry(const std::string &key);
    void update_main_pane();
    void on_discovery_update(DiscoveredPrinters devices, DiscoveryStatus status);
    void add_manual_printer();

    // The plain-data row the Phase N decision models decide from.
    ConnectionRow row_from(const ConnectionEntry &entry) const;
    // N4: the guided connect flow for a discovered device. Sends nothing;
    // reports the plan and only then hands off to the connection dialog.
    void run_connect_flow(const ConnectionEntry &entry);
    // N5: perform one offered lifecycle action for a saved connection.
    void run_action(ConnectionAction action, const ConnectionEntry &entry);
    // N5: rebuild the action row on the detail page for this entry.
    void rebuild_actions(const ConnectionEntry *entry);

    // Saved physical-printer profiles folded together with discovered devices.
    std::vector<ConnectionEntry> collect_entries() const;

    std::unique_ptr<PrinterDiscoveryService> m_discovery;
    std::vector<ConnectionEntry> m_entries;
    std::string      m_selected_key;
    std::string      m_filter;
    DiscoveryStatus  m_status { DiscoveryStatus::Idle };
    bool             m_active { false };

    ConnectionSidebar *m_sidebar { nullptr };
    wxSearchCtrl      *m_search { nullptr };
    wxButton          *m_refresh { nullptr };
    wxButton          *m_add_manual { nullptr };
    wxSimplebook      *m_main_pane { nullptr };
    wxStaticText      *m_empty_title { nullptr };
    wxStaticText      *m_empty_body { nullptr };
    wxStaticText      *m_detail_title { nullptr };
    wxStaticText      *m_detail_body { nullptr };
    wxStaticText      *m_overview { nullptr };
    wxButton          *m_connect { nullptr };
    // N5 lifecycle actions for the selected saved connection.
    wxSizer           *m_actions { nullptr };
    wxWindow          *m_regular_interface { nullptr };
    // True only while MainFrame has an actually created and addressed printer
    // web interface. Selecting a saved profile must never reveal an empty
    // browser page whose loading screen is not TigerSlicer's own interface.
    bool               m_regular_interface_live { false };

    // Main-pane page indices for the documented states.
    enum Page { PageEmpty = 0, PageDiscovered = 1, PageSaved = 2 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrinterConnectionsPanel_hpp_
