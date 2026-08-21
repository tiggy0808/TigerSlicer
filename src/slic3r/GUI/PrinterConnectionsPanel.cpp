///|/ TigerSlicer Phase N: Physical Printer workspace.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterConnectionsPanel.hpp"

#include <algorithm>
#include <chrono>
#include <map>

#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/treectrl.h>

#include <boost/algorithm/string.hpp>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "PhysicalPrinterDialog.hpp"
#include "libslic3r/PhysicalPrinterFlow.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrinterCapabilityMatch.hpp"
#include "libslic3r/PrinterOverview.hpp"

namespace Slic3r {
namespace GUI {

namespace {

const wxColour kPink(246, 165, 193);
const wxColour kBlack(24, 24, 27);
const wxColour kDimmed(150, 143, 146);

wxString section_label(ConnectionEntry::Section section)
{
    switch (section) {
    case ConnectionEntry::Section::Connected:  return _L("Connected");
    case ConnectionEntry::Section::Saved:      return _L("Saved");
    case ConnectionEntry::Section::Discovered: return _L("Discovered on LAN");
    case ConnectionEntry::Section::Offline:    return _L("Offline / recently seen");
    }
    return wxEmptyString;
}

// Per-row payload so a tree item maps back to its entry without relying on
// label text (which is localized and may repeat).
class EntryData : public wxTreeItemData
{
public:
    explicit EntryData(std::string key) : key(std::move(key)) {}
    std::string key;
};

// N4: the providers TigerSlicer actually ships connectors for. The flow
// selects from these; it never invents one. Credential kinds are what each
// provider's dialog will actually ask for - the flow only reports the kind,
// never a value.
std::vector<ProviderDescriptor> known_providers()
{
    auto provider = [](const char *id, const char *name, CredentialKind credentials,
                       ConnectionTechnology technology, bool advertises) {
        ProviderDescriptor descriptor;
        descriptor.id             = id;
        descriptor.display_name   = name;
        descriptor.credentials    = credentials;
        descriptor.technology     = technology;
        descriptor.self_advertises = advertises;
        return descriptor;
    };
    return {
        provider("prusalink", "PrusaLink",  CredentialKind::UsernamePassword, ConnectionTechnology::FDM,     true),
        provider("octoprint", "OctoPrint",  CredentialKind::ApiKey,           ConnectionTechnology::FDM,     true),
        provider("moonraker", "Moonraker",  CredentialKind::ApiKey,           ConnectionTechnology::FDM,     true),
        provider("duet",      "Duet",       CredentialKind::UsernamePassword, ConnectionTechnology::FDM,     false),
        provider("astrobox",  "AstroBox",   CredentialKind::ApiKey,           ConnectionTechnology::FDM,     true),
        provider("mks",       "MKS",        CredentialKind::None,             ConnectionTechnology::FDM,     false),
        provider("flashair",  "FlashAir",   CredentialKind::None,             ConnectionTechnology::FDM,     false),
        provider("sdcp",      "Resin (SDCP)", CredentialKind::None,           ConnectionTechnology::Resin,   true),
    };
}

wxString action_label(ConnectionAction action)
{
    switch (action) {
    case ConnectionAction::Connect:         return _L("Connect");
    case ConnectionAction::Disconnect:      return _L("Disconnect");
    case ConnectionAction::Forget:          return _L("Forget...");
    case ConnectionAction::EditCredentials: return _L("Edit connection...");
    case ConnectionAction::TestConnection:  return _L("Test connection...");
    case ConnectionAction::Rediscover:      return _L("Rediscover");
    }
    return wxEmptyString;
}

} // namespace

// Sidebar tree: sections with printer rows underneath. Sections with no rows
// are omitted rather than shown empty.
class ConnectionSidebar : public wxTreeCtrl
{
public:
    explicit ConnectionSidebar(wxWindow *parent)
        : wxTreeCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxTR_HAS_BUTTONS | wxTR_HIDE_ROOT | wxTR_NO_LINES |
                     wxTR_SINGLE | wxTR_FULL_ROW_HIGHLIGHT | wxBORDER_NONE)
    {
        AddRoot("root");
    }

    void rebuild(const std::vector<ConnectionEntry> &entries, const std::string &selected)
    {
        Freeze();
        DeleteChildren(GetRootItem());

        const ConnectionEntry::Section order[] = {
            ConnectionEntry::Section::Connected,
            ConnectionEntry::Section::Saved,
            ConnectionEntry::Section::Discovered,
            ConnectionEntry::Section::Offline,
        };

        wxTreeItemId to_select;
        for (const ConnectionEntry::Section section : order) {
            std::vector<const ConnectionEntry *> rows;
            for (const ConnectionEntry &entry : entries)
                if (entry.section == section)
                    rows.push_back(&entry);
            if (rows.empty())
                continue; // collapse empty sections

            wxTreeItemId parent = AppendItem(GetRootItem(), section_label(section));
            SetItemTextColour(parent, kDimmed);
            for (const ConnectionEntry *row : rows) {
                wxString label = from_u8(row->title);
                if (!row->status.empty())
                    label += "  -  " + from_u8(row->status);
                wxTreeItemId item = AppendItem(parent, label, -1, -1, new EntryData(row->key));
                if (row->key == selected)
                    to_select = item;
            }
            Expand(parent);
        }

        if (to_select.IsOk())
            SelectItem(to_select);
        Thaw();
    }

    std::string selected_key() const
    {
        const wxTreeItemId item = GetSelection();
        if (!item.IsOk())
            return {};
        auto *data = dynamic_cast<EntryData *>(GetItemData(item));
        return data == nullptr ? std::string() : data->key;
    }

    std::string key_at(const wxPoint &point) const
    {
        int flags = 0;
        const wxTreeItemId item = HitTest(point, flags);
        if (!item.IsOk())
            return {};
        auto *data = dynamic_cast<EntryData *>(GetItemData(item));
        return data == nullptr ? std::string() : data->key;
    }
};

PrinterConnectionsPanel::PrinterConnectionsPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
    , m_discovery(new PrinterDiscoveryService())
{
    SetBackgroundColour(kBlack);
    build_layout();

    // Discovery callbacks arrive on a worker thread; marshal to the UI thread.
    m_discovery->set_update_callback([this](DiscoveredPrinters devices, DiscoveryStatus status) {
        CallAfter([this, devices = std::move(devices), status]() mutable {
            on_discovery_update(std::move(devices), status);
        });
    });

    reload_entries();
}

PrinterConnectionsPanel::~PrinterConnectionsPanel()
{
    // Stop before the callback's captured `this` can go away.
    m_discovery->stop();
}

void PrinterConnectionsPanel::build_layout()
{
    const int em = wxGetApp().em_unit();

    auto *root = new wxBoxSizer(wxHORIZONTAL);

    // ---- persistent connections sidebar -------------------------------
    auto *side_panel = new wxPanel(this);
    side_panel->SetBackgroundColour(kBlack);
    auto *side = new wxBoxSizer(wxVERTICAL);

    // N19: compact multi-printer overview above the list. Counts only - it
    // never hides offline or unsupported devices, it just summarises them.
    m_overview = new wxStaticText(side_panel, wxID_ANY, wxEmptyString);
    m_overview->SetForegroundColour(kDimmed);
    m_overview->SetFont(wxGetApp().small_font());
    side->Add(m_overview, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, int(0.5 * em));

    m_search = new wxSearchCtrl(side_panel, wxID_ANY);
    m_search->SetDescriptiveText(_L("Search printers"));
    // Native default is a white field, which reads as a hole in this workspace.
    wxGetApp().UpdateDarkUI(m_search);
    side->Add(m_search, 0, wxEXPAND | wxALL, int(0.5 * em));

    m_sidebar = new ConnectionSidebar(side_panel);
    m_sidebar->SetBackgroundColour(kBlack);
    m_sidebar->SetForegroundColour(kPink);
    // This tree carries its own colours, so ask only for the dark selection
    // bar rather than the full UpdateDarkUI treatment.
    wxGetApp().UpdateDarkUIWin(m_sidebar);
    side->Add(m_sidebar, 1, wxEXPAND | wxLEFT | wxRIGHT, int(0.5 * em));

    auto *side_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_refresh = new wxButton(side_panel, wxID_ANY, _L("Refresh"));
    m_add_manual = new wxButton(side_panel, wxID_ANY, _L("Add manually"));
    wxGetApp().UpdateDarkUI(m_refresh);
    wxGetApp().UpdateDarkUI(m_add_manual);
    side_buttons->Add(m_refresh, 1, wxRIGHT, int(0.3 * em));
    side_buttons->Add(m_add_manual, 1);
    side->Add(side_buttons, 0, wxEXPAND | wxALL, int(0.5 * em));

    side_panel->SetSizer(side);
    side_panel->SetMinSize(wxSize(22 * em, -1));
    root->Add(side_panel, 0, wxEXPAND);

    // ---- main pane -----------------------------------------------------
    m_main_pane = new wxSimplebook(this, wxID_ANY);
    m_main_pane->SetBackgroundColour(kBlack);

    auto make_page = [this, em](wxStaticText *&title, wxStaticText *&body) {
        auto *page = new wxPanel(m_main_pane);
        page->SetBackgroundColour(kBlack);
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        title = new wxStaticText(page, wxID_ANY, wxEmptyString);
        title->SetForegroundColour(kPink);
        title->SetFont(wxGetApp().bold_font());
        body = new wxStaticText(page, wxID_ANY, wxEmptyString);
        body->SetForegroundColour(kDimmed);
        sizer->Add(title, 0, wxALL, em);
        sizer->Add(body, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);
        page->SetSizer(sizer);
        return page;
    };

    wxPanel *empty_page = make_page(m_empty_title, m_empty_body);
    m_main_pane->AddPage(empty_page, wxEmptyString);

    wxStaticText *discovered_title = nullptr;
    wxStaticText *discovered_body = nullptr;
    wxPanel *discovered_page = make_page(discovered_title, discovered_body);
    m_detail_title = discovered_title;
    m_detail_body = discovered_body;
    // A newly discovered device gets identity details and a guided Connect;
    // it must not impersonate a configured printer interface.
    m_connect = new wxButton(discovered_page, wxID_ANY, _L("Connect"));
    discovered_page->GetSizer()->Add(m_connect, 0, wxLEFT | wxBOTTOM, em);
    // N5: lifecycle actions for a saved connection, rebuilt per selection so
    // only the actions its current state offers ever exist as buttons.
    m_actions = new wxBoxSizer(wxHORIZONTAL);
    discovered_page->GetSizer()->Add(m_actions, 0, wxLEFT | wxBOTTOM, em);
    m_main_pane->AddPage(discovered_page, wxEmptyString);

    root->Add(m_main_pane, 1, wxEXPAND);
    SetSizer(root);

    m_empty_title->SetLabel(_L("No printer connections yet"));
    m_empty_body->SetLabel(_L(
        "TigerSlicer can look for printers that advertise themselves on your local network.\n"
        "You do not need a physical printer profile first.\n\n"
        "Use Refresh to scan, or Add manually if your printer does not advertise itself."));

    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { start_discovery(); });
    m_add_manual->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_manual_printer(); });
    // N4: Connect enters the guided flow. It never sends a command here.
    m_connect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const auto found = std::find_if(m_entries.begin(), m_entries.end(),
            [this](const ConnectionEntry &entry) { return entry.key == m_selected_key; });
        if (found != m_entries.end())
            run_connect_flow(*found);
    });
    m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent &event) {
        m_filter = into_u8(m_search->GetValue());
        apply_filter();
        event.Skip();
    });
    m_sidebar->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent &event) {
        // Selection is navigation only - no printer command is issued here.
        m_selected_key = m_sidebar->selected_key();
        update_main_pane();
        event.Skip();
    });
    // On Windows at high DPI, native tree selection notifications may be
    // swallowed by the themed tree before its caret changes. Resolve the
    // clicked item explicitly from tree-local coordinates as well. This is
    // navigation only and therefore never contacts or controls the printer.
    m_sidebar->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &event) {
        const std::string key = m_sidebar->key_at(event.GetPosition());
        if (!key.empty()) {
            m_selected_key = key;
            update_main_pane();
        }
        event.Skip();
    });
}

wxWindow *PrinterConnectionsPanel::regular_interface_parent() const
{
    return m_main_pane;
}

void PrinterConnectionsPanel::attach_regular_interface(wxWindow *regular_page)
{
    wxASSERT(m_main_pane != nullptr);
    wxASSERT(regular_page != nullptr);
    wxASSERT(regular_page->GetParent() == m_main_pane);
    wxASSERT(m_regular_interface == nullptr);
    m_regular_interface = regular_page;
    m_main_pane->AddPage(regular_page, wxEmptyString);
}

void PrinterConnectionsPanel::show_regular_interface()
{
    if (m_main_pane == nullptr || m_regular_interface == nullptr)
        return;
    m_regular_interface_live = true;
    m_main_pane->ChangeSelection(PageSaved);
}

void PrinterConnectionsPanel::show_discovery_interface()
{
    if (m_main_pane == nullptr)
        return;
    // The web interface was torn down. Fall back to whatever this workspace's
    // own selection describes rather than pinning the empty state.
    m_regular_interface_live = false;
    update_main_pane();
}

bool PrinterConnectionsPanel::regular_interface_shown() const
{
    return m_main_pane != nullptr && m_regular_interface != nullptr &&
           m_main_pane->GetCurrentPage() == m_regular_interface;
}

std::vector<ConnectionEntry> PrinterConnectionsPanel::collect_entries() const
{
    std::vector<ConnectionEntry> entries;

    // Saved physical-printer profiles first, so a configured printer is
    // reachable even with no network at all.
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        for (const PhysicalPrinter &printer : bundle->physical_printers) {
            ConnectionEntry entry;
            entry.section  = ConnectionEntry::Section::Saved;
            entry.key      = "saved:" + printer.name;
            entry.title    = printer.name;
            entry.saved    = true;
            entry.technology_known = true;
            entry.technology = printer.printer_technology();
            if (printer.config.has("print_host"))
                entry.subtitle = printer.config.opt_string("print_host");
            entry.status = _u8L("Saved");
            entries.push_back(std::move(entry));
        }
    }

    for (const DiscoveredPrinter &device : m_discovery->devices()) {
        // Fold a discovered device into its saved profile rather than listing
        // the same printer twice.
        auto match = std::find_if(entries.begin(), entries.end(),
            [&device](const ConnectionEntry &entry) {
                return entry.saved && !entry.subtitle.empty() &&
                       (entry.subtitle.find(device.hostname) != std::string::npos ||
                        (!device.address.empty() &&
                         entry.subtitle.find(device.address) != std::string::npos));
            });
        if (match != entries.end()) {
            match->device = device;
            match->status = device.online ? _u8L("Saved, seen on LAN") : _u8L("Saved, offline");
            match->section = device.online ? ConnectionEntry::Section::Saved
                                           : ConnectionEntry::Section::Offline;
            continue;
        }

        ConnectionEntry entry;
        entry.section  = device.online ? ConnectionEntry::Section::Discovered
                                       : ConnectionEntry::Section::Offline;
        entry.key      = "lan:" + (device.identity.empty() ? device.address : device.identity);
        entry.title    = device.name.empty() ? device.identity : device.name;
        entry.subtitle = device.address;
        entry.device   = device;
        entry.technology_known = device.technology_known;
        entry.technology = device.technology;
        entry.status   = device.online ? (device.requires_authentication
                                            ? _u8L("Authentication required")
                                            : _u8L("Discovered"))
                                       : _u8L("Not seen recently");
        entries.push_back(std::move(entry));
    }

    return entries;
}

PrinterConnectionsPanel::OverviewCounts PrinterConnectionsPanel::overview_counts() const
{
    OverviewCounts counts;
    for (const ConnectionEntry &entry : m_entries) {
        switch (entry.section) {
        case ConnectionEntry::Section::Connected:  ++counts.connected; break;
        case ConnectionEntry::Section::Saved:      ++counts.saved; break;
        case ConnectionEntry::Section::Discovered: ++counts.discovered; break;
        case ConnectionEntry::Section::Offline:    ++counts.offline; break;
        }
        // Anything needing the user's attention before it can be used.
        if (entry.device.requires_authentication ||
            entry.section == ConnectionEntry::Section::Offline)
            ++counts.needs_attention;
    }
    return counts;
}

ConnectionRow PrinterConnectionsPanel::row_from(const ConnectionEntry &entry) const
{
    ConnectionRow row;
    row.id      = entry.key;
    row.title   = entry.title;
    row.saved   = entry.saved;
    row.online  = entry.device.online;
    row.requires_authentication = entry.device.requires_authentication;
    row.technology_known        = entry.technology_known;
    row.technology_is_resin     = entry.technology == ptSLA;
    // A live web session belongs to the selected physical printer only.
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    row.session_open = m_regular_interface_live && entry.saved &&
        bundle != nullptr && bundle->physical_printers.has_selection() &&
        bundle->physical_printers.get_selected_printer().name == entry.title;
    // A saved profile with a configured host can take uploads; the same web
    // interface is what control means here.
    row.supports_upload  = entry.saved && !entry.subtitle.empty();
    row.supports_control = row.supports_upload;
    if (!entry.device.identity.empty() || entry.device.online) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - entry.device.last_seen);
        row.last_seen_age_s = int64_t(age.count());
    }
    return row;
}

void PrinterConnectionsPanel::update_overview()
{
    if (m_overview == nullptr)
        return;

    // N19: the tested aggregation partitions the fleet - every printer in
    // exactly one category, reachable-but-silent counted Offline rather than
    // optimistically Ready, nothing hidden.
    const int64_t now_s = int64_t(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    std::vector<PrinterReport> reports;
    reports.reserve(m_entries.size());
    for (const ConnectionEntry &entry : m_entries)
        reports.push_back(printer_report(row_from(entry), now_s));
    const PrinterOverview overview =
        build_overview(reports, {}, OverviewPolicy{}, now_s);

    wxString summary;
    auto append = [&summary](const wxString &label, size_t value) {
        if (value == 0)
            return;
        if (!summary.IsEmpty())
            summary += "   ";
        summary += wxString::Format("%s %zu", label, value);
    };
    append(_L("Printing"), overview.counts.printing);
    append(_L("Ready"), overview.counts.ready);
    append(_L("Paused"), overview.counts.paused);
    append(_L("Offline"), overview.counts.offline);
    append(_L("Needs attention"), overview.counts.needs_attention);
    append(_L("Maintenance due"), overview.counts.maintenance_due);
    append(_L("Queued"), overview.counts.queued_jobs);
    // Authentication is a per-connection property the activity model does not
    // carry; reported alongside, not folded into a category.
    size_t needs_auth = 0;
    for (const ConnectionEntry &entry : m_entries)
        if (entry.device.requires_authentication)
            ++needs_auth;
    append(_L("Needs sign-in"), needs_auth);
    if (overview.has_stale_data) {
        if (!summary.IsEmpty())
            summary += "   ";
        summary += _L("(some data is stale)");
    }
    m_overview->SetLabel(summary.IsEmpty() ? _L("No printers yet") : summary);
}

wxString PrinterConnectionsPanel::describe_profile_match(const DiscoveredPrinter &device) const
{
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return wxEmptyString;

    PrinterDeviceCapabilities capabilities;
    capabilities.vendor           = device.vendor;
    capabilities.model            = device.model;
    capabilities.firmware         = device.firmware;
    capabilities.technology_known = device.technology_known;
    capabilities.technology       = device.technology;

    std::vector<PrinterProfileSummary> profiles;
    for (const Preset &preset : bundle->printers) {
        if (!preset.is_visible)
            continue;
        PrinterProfileSummary summary;
        summary.name       = preset.name;
        summary.technology = preset.printer_technology();
        if (preset.config.has("printer_model"))
            summary.model = preset.config.opt_string("printer_model");
        if (preset.config.has("printer_vendor"))
            summary.vendor = preset.config.opt_string("printer_vendor");
        profiles.push_back(std::move(summary));
    }

    const ProfileMatch match = match_printer_profile(capabilities, profiles);
    switch (match.level) {
    case ProfileMatchLevel::Exact:
        return wxString::Format(_L("Matches your profile \"%s\"."), from_u8(match.best_profile));
    case ProfileMatchLevel::CompatibleWithWarnings:
        return wxString::Format(_L("Looks compatible with \"%s\", with points to review."),
                                from_u8(match.best_profile));
    case ProfileMatchLevel::Ambiguous:
        // Deliberately does not pick one; the user chooses.
        return wxString::Format(_L("Several profiles fit equally well (%d). Choose one when connecting."),
                                int(match.tied_profiles.size()));
    case ProfileMatchLevel::NoCompatibleProfile:
        return _L("No installed profile matches this printer yet.");
    }
    return wxEmptyString;
}

void PrinterConnectionsPanel::reload_entries()
{
    m_entries = collect_entries();
    if (m_selected_key.empty() && m_entries.size() == 1)
        m_selected_key = m_entries.front().key;
    apply_filter();
    update_overview();
    update_main_pane();
}

void PrinterConnectionsPanel::apply_filter()
{
    std::vector<ConnectionEntry> visible;
    const std::string needle = boost::algorithm::to_lower_copy(m_filter);
    for (const ConnectionEntry &entry : m_entries) {
        if (needle.empty()) {
            visible.push_back(entry);
            continue;
        }
        const std::string haystack = boost::algorithm::to_lower_copy(entry.title + " " + entry.subtitle);
        if (haystack.find(needle) != std::string::npos)
            visible.push_back(entry);
    }
    if (m_sidebar != nullptr)
        m_sidebar->rebuild(visible, m_selected_key);
}

void PrinterConnectionsPanel::update_main_pane()
{
    if (m_main_pane == nullptr)
        return;

    const auto found = std::find_if(m_entries.begin(), m_entries.end(),
        [this](const ConnectionEntry &entry) { return entry.key == m_selected_key; });

    if (found == m_entries.end()) {
        // N7: exactly one state, chosen by the tested resolver from what the
        // environment reports, most blocking first - so "no printers found"
        // can never mask a blocked, offline, or never-run scan.
        size_t online_results = 0;
        for (const ConnectionEntry &entry : m_entries)
            if (entry.device.online)
                ++online_results;
        switch (resolve_discovery_state(discovery_conditions(m_status, online_results))) {
        case DiscoveryTabState::Discovering:
            m_empty_title->SetLabel(_L("Looking for printers on your network"));
            break;
        case DiscoveryTabState::PermissionDenied:
            m_empty_title->SetLabel(_L("Local network permission is required to discover printers"));
            break;
        case DiscoveryTabState::NetworkOffline:
            m_empty_title->SetLabel(_L("No usable network interface was found"));
            break;
        case DiscoveryTabState::Unsupported:
            m_empty_title->SetLabel(_L("Discovery could not be started"));
            break;
        case DiscoveryTabState::AuthenticationRequired:
            m_empty_title->SetLabel(_L("A printer requires authentication before it can be listed"));
            break;
        case DiscoveryTabState::NoPrintersFound:
            m_empty_title->SetLabel(_L("No printer connections yet"));
            break;
        case DiscoveryTabState::Results:
            m_empty_title->SetLabel(_L("Select a printer"));
            break;
        }
        m_main_pane->ChangeSelection(PageEmpty);
        return;
    }

    wxString details;
    auto add_line = [&details](const wxString &label, const std::string &value) {
        if (!value.empty())
            details += label + ": " + from_u8(value) + "\n";
    };
    add_line(_L("Hostname"), found->device.hostname);
    add_line(_L("Address"), found->device.address);
    // A saved profile carries its configured host even when nothing has been
    // discovered on the network, so the detail page is never blank.
    if (found->saved && found->device.hostname.empty() && found->device.address.empty())
        add_line(_L("Configured host"), found->subtitle);
    add_line(_L("Manufacturer"), found->device.vendor);
    add_line(_L("Model"), found->device.model);
    add_line(_L("Firmware"), found->device.firmware);
    if (!found->device.protocols.empty())
        details += _L("Connection") + ": " + from_u8(boost::algorithm::join(found->device.protocols, ", ")) + "\n";
    details += _L("Technology") + ": " +
        (found->technology_known ? (found->technology == ptSLA ? _L("Resin (MSLA)") : _L("FDM"))
                                 : _L("Not advertised")) + "\n";
    if (found->device.requires_authentication)
        details += _L("This printer requires authentication before it can be used.") + "\n";

    // N10: the tested card model decides freshness. A reading older than the
    // staleness window is Stale with its age, never drawn as current.
    {
        const int64_t now_s = int64_t(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const PrinterCard card = build_printer_card(
            printer_report(row_from(*found), now_s), CardDensity::Compact,
            OverviewPolicy{}, now_s);
        wxString status_line = _L("Status") + ": " + _(printer_activity_name(card.activity));
        if (card.activity_freshness == Freshness::Stale) {
            const ConnectionRow row = row_from(*found);
            if (row.last_seen_age_s.has_value())
                status_line += " " + wxString::Format(_L("(last seen %d s ago - stale)"),
                                                      int(*row.last_seen_age_s));
            else
                status_line += " " + _L("(stale)");
        } else if (card.activity_freshness == Freshness::Absent) {
            status_line += " " + _L("(never reported)");
        }
        details += status_line + "\n";
        if (card.attention)
            details += _L("Needs attention") + ": " + from_u8(card.attention_reason) + "\n";
    }

    // N8: report the profile match as evidence, never as a selection.
    if (const wxString match = describe_profile_match(found->device); !match.IsEmpty())
        details += "\n" + match + "\n";

    if (found->saved && m_regular_interface_live) {
        m_main_pane->ChangeSelection(PageSaved);
    } else {
        m_detail_title->SetLabel(from_u8(found->title));
        if (found->saved) {
            // Without a created and addressed web interface, PageSaved is an
            // empty browser showing a foreign loading screen. Report what this
            // profile actually is instead of implying a live connection.
            details += "\n" + _L(
                "This printer has no live web interface open in TigerSlicer.");
            m_connect->Hide();
            // N5: only the actions this connection's state offers.
            rebuild_actions(&*found);
        } else {
            m_connect->Show();
            rebuild_actions(nullptr);
        }
        m_detail_body->SetLabel(details);
        m_connect->GetParent()->Layout();
        // Compatibility is not claimed: only what the device advertised.
        m_main_pane->ChangeSelection(PageDiscovered);
    }
    Layout();
}

void PrinterConnectionsPanel::rebuild_actions(const ConnectionEntry *entry)
{
    if (m_actions == nullptr)
        return;
    m_actions->Clear(true);
    if (entry == nullptr)
        return;

    const ConnectionView view = view_connection(connection_status(row_from(*entry)));
    wxWindow *page = m_connect->GetParent();
    const int em = wxGetApp().em_unit();
    const ConnectionEntry captured = *entry;
    for (const ConnectionAction action : view.actions) {
        auto *button = new wxButton(page, wxID_ANY, action_label(action));
        wxGetApp().UpdateDarkUI(button);
        button->Bind(wxEVT_BUTTON, [this, action, captured](wxCommandEvent &) {
            run_action(action, captured);
        });
        m_actions->Add(button, 0, wxRIGHT, int(0.4 * em));
    }
    // A control specific to the other technology is withheld, and the view
    // says which - surface that instead of silently missing a button.
    if (!view.withheld_controls.empty()) {
        auto *note = new wxStaticText(page, wxID_ANY,
            from_u8(boost::algorithm::join(view.withheld_controls, "  ")));
        note->SetForegroundColour(kDimmed);
        note->SetFont(wxGetApp().small_font());
        m_actions->Add(note, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, int(0.4 * em));
    }
}

void PrinterConnectionsPanel::run_action(ConnectionAction action, const ConnectionEntry &entry)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    switch (action) {
    case ConnectionAction::Connect:
        // The established, explicit path: selecting the physical printer
        // opens its interface. Explicitly user-initiated, never silent.
        if (bundle != nullptr) {
            bundle->physical_printers.select_printer(entry.title);
            wxGetApp().mainframe->load_printer_url();
            show_regular_interface();
        }
        break;
    case ConnectionAction::Disconnect:
        // Ends the session; keeps the profile. Distinct from Forget.
        if (bundle != nullptr && bundle->physical_printers.has_selection() &&
            bundle->physical_printers.get_selected_printer().name == entry.title)
            bundle->physical_printers.unselect_printer();
        show_discovery_interface();
        break;
    case ConnectionAction::Forget: {
        // Deletes the saved profile. Asks first; deletion is not undoable.
        MessageDialog dialog(this,
            format_wxstr(_L("Forget the printer connection \"%1%\"?\n\n"
                            "The saved connection profile is deleted. The printer itself "
                            "and your slicing presets are not touched."), from_u8(entry.title)),
            _L("Forget printer"), wxYES_NO | wxNO_DEFAULT);
        if (dialog.ShowModal() != wxID_YES || bundle == nullptr)
            break;
        bundle->physical_printers.select_printer(entry.title);
        bundle->physical_printers.delete_selected_printer();
        reload_entries();
        break;
    }
    case ConnectionAction::EditCredentials:
    case ConnectionAction::TestConnection: {
        // Credentials and the connection test live in the connection dialog;
        // secrets never pass through this panel.
        PhysicalPrinterDialog dialog(this);
        dialog.ShowModal();
        reload_entries();
        break;
    }
    case ConnectionAction::Rediscover:
        start_discovery();
        break;
    }
}

void PrinterConnectionsPanel::run_connect_flow(const ConnectionEntry &entry)
{
    // N4: the guided flow sends nothing. It reports what would happen and
    // what is still needed; the connection dialog does the actual setup.
    ConnectRequest request;
    request.device.vendor           = entry.device.vendor;
    request.device.model            = entry.device.model;
    request.device.firmware         = entry.device.firmware;
    request.device.technology_known = entry.technology_known;
    request.device.technology       = entry.technology;
    if (entry.device.protocols.empty() && !entry.device.address.empty())
        request.manual_address = ManualAddress{ entry.device.address };
    request.known_providers = known_providers();

    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        for (const Preset &preset : bundle->printers) {
            if (!preset.is_visible)
                continue;
            PrinterProfileSummary summary;
            summary.name       = preset.name;
            summary.technology = preset.printer_technology();
            if (preset.config.has("printer_model"))
                summary.model = preset.config.opt_string("printer_model");
            if (preset.config.has("printer_vendor"))
                summary.vendor = preset.config.opt_string("printer_vendor");
            request.profiles.push_back(std::move(summary));
        }
        request.active_slicing_preset = bundle->printers.get_selected_preset_name();
    }

    const ConnectPlan plan = plan_connect(request);

    wxString text;
    if (!plan.message.empty())
        text += from_u8(plan.message) + "\n\n";
    if (plan.provider.has_value()) {
        text += format_wxstr(_L("Provider: %1%"), from_u8(plan.provider->display_name)) + "\n";
        if (plan.required_credentials != CredentialKind::None)
            text += format_wxstr(
                _L("This provider authenticates with: %1%. You will enter it in the "
                   "connection dialog; it is never stored here."),
                _(credential_kind_name(plan.required_credentials))) + "\n";
    }
    if (!plan.match.best_profile.empty())
        text += format_wxstr(_L("Profile match: %1%"), from_u8(plan.match.best_profile)) + "\n";
    if (plan.would_change_active_preset)
        // The failure N4 forbids is doing this silently.
        text += _L("Saving this connection would associate a different slicing preset. "
                   "Your active preset stays unchanged unless you choose to switch.") +
                "\n" + from_u8(plan.preset_change_note) + "\n";
    for (const std::string &warning : plan.mismatch_warnings)
        text += from_u8(warning) + "\n";
    if (!plan.remaining_steps.empty()) {
        text += "\n" + _L("Remaining steps:") + "\n";
        for (const ConnectStep step : plan.remaining_steps)
            text += wxString(" - ") + _(connect_step_name(step)) + "\n";
    }
    text += "\n" + _L("Open the connection dialog to continue?");

    MessageDialog dialog(this, text,
        format_wxstr(_L("Connect to %1%"), from_u8(entry.title)), wxYES_NO);
    if (dialog.ShowModal() == wxID_YES) {
        // A new connection profile named after the device; the dialog owns
        // hosts, credentials and the test.
        PhysicalPrinterDialog setup(this);
        setup.ShowModal();
        reload_entries();
    }
}

void PrinterConnectionsPanel::on_discovery_update(DiscoveredPrinters devices, DiscoveryStatus status)
{
    m_status = status;
    reload_entries();
}

void PrinterConnectionsPanel::add_manual_printer()
{
    wxTextEntryDialog dialog(this,
        _L("Enter the printer's hostname or IP address, optionally with a port."),
        _L("Add printer manually"));
    if (dialog.ShowModal() != wxID_OK)
        return;
    const std::string host = into_u8(dialog.GetValue());
    if (boost::algorithm::trim_copy(host).empty())
        return;

    DiscoveredPrinter manual;
    manual.identity = host;
    manual.name     = host;
    manual.address  = host;
    manual.addresses.push_back(host);
    manual.last_seen = std::chrono::steady_clock::now();
    // A manually added host is listed but never contacted here; connecting is
    // a separate, explicit action.
    m_discovery->add_manual(manual);
}

void PrinterConnectionsPanel::start_discovery()
{
    m_discovery->start();
}

void PrinterConnectionsPanel::stop_discovery()
{
    m_discovery->stop();
}

void PrinterConnectionsPanel::on_activate()
{
    m_active = true;
    reload_entries();
    start_discovery();
}

void PrinterConnectionsPanel::on_deactivate()
{
    m_active = false;
    // Leaving the workspace cancels scanning rather than polling in the
    // background; saved rows remain listed when the user returns.
    stop_discovery();
}

void PrinterConnectionsPanel::msw_rescale()
{
    const int em = wxGetApp().em_unit();
    if (m_sidebar != nullptr)
        m_sidebar->GetParent()->SetMinSize(wxSize(22 * em, -1));
    Layout();
}

void PrinterConnectionsPanel::sys_color_changed()
{
    SetBackgroundColour(kBlack);
    if (m_sidebar != nullptr) {
        m_sidebar->SetBackgroundColour(kBlack);
        m_sidebar->SetForegroundColour(kPink);
        // WM_THEMECHANGED resets the window's visual style, so reassert it.
        wxGetApp().UpdateDarkUIWin(m_sidebar);
    }
    if (m_search != nullptr)
        wxGetApp().UpdateDarkUI(m_search);
    if (m_refresh != nullptr)
        wxGetApp().UpdateDarkUI(m_refresh);
    if (m_add_manual != nullptr)
        wxGetApp().UpdateDarkUI(m_add_manual);
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
