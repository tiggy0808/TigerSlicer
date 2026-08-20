///|/ TigerSlicer Phase N: local-first printer notification centre.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterNotifications.hpp"

#include <algorithm>

namespace Slic3r {

const char *to_string(NotificationEvent event)
{
    switch (event) {
    case NotificationEvent::PrintCompleted:       return "print completed";
    case NotificationEvent::InterventionRequired: return "intervention required";
    case NotificationEvent::MaterialRunout:       return "material runout";
    case NotificationEvent::ToolchangeFailure:    return "toolchange failure";
    case NotificationEvent::PrinterFault:         return "printer fault";
    case NotificationEvent::ConnectionLost:       return "connection lost";
    case NotificationEvent::ConnectionRestored:   return "connection restored";
    case NotificationEvent::MaintenanceDue:       return "maintenance due";
    case NotificationEvent::QueueBlocked:         return "queue blocked";
    case NotificationEvent::UploadCompleted:      return "upload completed";
    case NotificationEvent::UploadFailed:         return "upload failed";
    case NotificationEvent::PrinterDiscovered:    return "printer discovered";
    case NotificationEvent::SavedPrinterChanged:  return "saved printer changed";
    }
    return "notification";
}

bool is_critical(NotificationEvent event)
{
    switch (event) {
    case NotificationEvent::InterventionRequired:
    case NotificationEvent::MaterialRunout:
    case NotificationEvent::ToolchangeFailure:
    case NotificationEvent::PrinterFault:
        // A print that needs a human, or hardware in trouble, must reach the
        // user even if they have muted the noisy events.
        return true;
    default:
        return false;
    }
}

void NotificationCenter::set_preferences(NotificationPreferences preferences)
{
    m_preferences = std::move(preferences);
}

bool NotificationCenter::muted(const Notification &notification) const
{
    if (is_critical(notification.event))
        return false;
    if (std::find(m_preferences.muted_events.begin(), m_preferences.muted_events.end(),
                  notification.event) != m_preferences.muted_events.end())
        return true;
    return std::find(m_preferences.muted_printers.begin(), m_preferences.muted_printers.end(),
                     notification.printer) != m_preferences.muted_printers.end();
}

bool NotificationCenter::in_quiet_hours(int seconds_into_day) const
{
    if (!m_preferences.quiet_from_s.has_value() || !m_preferences.quiet_to_s.has_value())
        return false;
    const int from = *m_preferences.quiet_from_s;
    const int to   = *m_preferences.quiet_to_s;
    if (from == to)
        return false;
    if (from < to)
        return seconds_into_day >= from && seconds_into_day < to;
    // Window wraps past midnight (e.g. 22:00 to 07:00).
    return seconds_into_day >= from || seconds_into_day < to;
}

const Notification *NotificationCenter::recent_duplicate(const Notification &notification) const
{
    if (m_preferences.dedup_window_s <= 0)
        return nullptr;
    for (auto it = m_notifications.rbegin(); it != m_notifications.rend(); ++it) {
        if (it->event != notification.event || it->printer != notification.printer)
            continue;
        if (notification.at - it->at <= m_preferences.dedup_window_s)
            return &*it;
        // Entries are appended in time order, so once one is outside the
        // window every earlier one is too.
        break;
    }
    return nullptr;
}

DeliveryDecision NotificationCenter::submit(const Notification &notification, int seconds_into_day)
{
    if (recent_duplicate(notification) != nullptr)
        return DeliveryDecision::Duplicate;   // not recorded twice

    m_notifications.push_back(notification);

    if (muted(notification))
        return DeliveryDecision::RecordOnly;

    // Quiet hours suppress the alert but never the record, and never apply to
    // events the user cannot afford to miss.
    if (in_quiet_hours(seconds_into_day) && !is_critical(notification.event))
        return DeliveryDecision::RecordOnly;

    return DeliveryDecision::Deliver;
}

bool NotificationCenter::acknowledge(const std::string &id)
{
    const auto it = std::find_if(m_notifications.begin(), m_notifications.end(),
        [&id](const Notification &notification) { return notification.id == id; });
    if (it == m_notifications.end())
        return false;
    it->acknowledged = true;
    return true;
}

void NotificationCenter::acknowledge_all()
{
    for (Notification &notification : m_notifications)
        notification.acknowledged = true;
}

std::vector<Notification> NotificationCenter::unacknowledged() const
{
    std::vector<Notification> out;
    for (const Notification &notification : m_notifications)
        if (!notification.acknowledged)
            out.push_back(notification);
    return out;
}

size_t NotificationCenter::unacknowledged_count() const
{
    return size_t(std::count_if(m_notifications.begin(), m_notifications.end(),
        [](const Notification &notification) { return !notification.acknowledged; }));
}

void NotificationCenter::clear()
{
    m_notifications.clear();
}

} // namespace Slic3r
