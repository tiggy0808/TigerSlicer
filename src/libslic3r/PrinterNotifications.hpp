///|/ TigerSlicer Phase N: local-first printer notification centre.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterNotifications_hpp_
#define slic3r_PrinterNotifications_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class NotificationEvent
{
    PrintCompleted,
    InterventionRequired,
    MaterialRunout,
    ToolchangeFailure,
    PrinterFault,
    ConnectionLost,
    ConnectionRestored,
    MaintenanceDue,
    QueueBlocked,
    UploadCompleted,
    UploadFailed,
    PrinterDiscovered,
    SavedPrinterChanged,
};

// Events the user must not be able to silence into a dangerous silence.
// Faults and interventions still surface during quiet hours.
bool is_critical(NotificationEvent event);

struct Notification
{
    std::string       id;
    NotificationEvent event { NotificationEvent::PrintCompleted };
    std::string       printer;
    std::string       title;
    std::string       body;
    int64_t           at { 0 };      // caller-supplied timestamp (seconds)
    bool              acknowledged { false };
};

// Per-printer, per-event delivery preferences.
struct NotificationPreferences
{
    // Events the user has muted. Critical events are still delivered.
    std::vector<NotificationEvent> muted_events;
    // Printers the user has muted entirely.
    std::vector<std::string> muted_printers;

    // Quiet hours as seconds-since-midnight, local. When start > end the
    // window wraps past midnight.
    std::optional<int> quiet_from_s;
    std::optional<int> quiet_to_s;

    // Suppress a repeat of the same event for the same printer within this
    // many seconds.
    int dedup_window_s { 300 };

    bool os_notifications_enabled { false };
};

enum class DeliveryDecision
{
    Deliver,
    // Recorded in the centre but not surfaced as an alert.
    RecordOnly,
    // Identical to something already delivered very recently.
    Duplicate,
};

class NotificationCenter
{
public:
    void set_preferences(NotificationPreferences preferences);
    const NotificationPreferences &preferences() const { return m_preferences; }

    // Submit an event. `seconds_into_day` lets quiet hours be evaluated
    // without this unit depending on a clock, keeping it deterministic.
    DeliveryDecision submit(const Notification &notification, int seconds_into_day);

    bool acknowledge(const std::string &id);
    void acknowledge_all();

    // Everything recorded, newest last.
    const std::vector<Notification> &all() const { return m_notifications; }
    std::vector<Notification> unacknowledged() const;
    size_t unacknowledged_count() const;

    void clear();

private:
    bool muted(const Notification &notification) const;
    bool in_quiet_hours(int seconds_into_day) const;
    const Notification *recent_duplicate(const Notification &notification) const;

    NotificationPreferences   m_preferences;
    std::vector<Notification> m_notifications;
};

const char *to_string(NotificationEvent event);

} // namespace Slic3r

#endif // slic3r_PrinterNotifications_hpp_
