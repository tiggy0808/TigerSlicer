// Phase N: notification centre. The risk with notification settings is that a
// user mutes something and then misses a fault, so these tests pin down that
// critical events survive muting and quiet hours, while everything else
// respects preferences and deduplicates.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrinterNotifications.hpp"

using namespace Slic3r;

namespace {

Notification event(const std::string &id, NotificationEvent type, int64_t at,
                   const std::string &printer = "printer1")
{
    Notification notification;
    notification.id      = id;
    notification.event   = type;
    notification.printer = printer;
    notification.title   = to_string(type);
    notification.at      = at;
    return notification;
}

constexpr int hour(int h) { return h * 3600; }

} // namespace

TEST_CASE("An ordinary event is delivered", "[notifications]")
{
    NotificationCenter centre;
    CHECK(centre.submit(event("a", NotificationEvent::PrintCompleted, 0), hour(12)) ==
          DeliveryDecision::Deliver);
    CHECK(centre.unacknowledged_count() == 1);
}

TEST_CASE("Muted events are recorded but not surfaced", "[notifications]")
{
    NotificationPreferences preferences;
    preferences.muted_events = { NotificationEvent::PrintCompleted };

    NotificationCenter centre;
    centre.set_preferences(preferences);

    CHECK(centre.submit(event("a", NotificationEvent::PrintCompleted, 0), hour(12)) ==
          DeliveryDecision::RecordOnly);
    // Still visible in the centre, just not alerted.
    CHECK(centre.all().size() == 1);
}

TEST_CASE("Muting a printer does not mute its faults", "[notifications]")
{
    NotificationPreferences preferences;
    preferences.muted_printers = { "printer1" };

    NotificationCenter centre;
    centre.set_preferences(preferences);

    CHECK(centre.submit(event("a", NotificationEvent::PrintCompleted, 0), hour(12)) ==
          DeliveryDecision::RecordOnly);
    // A fault is exactly what the user needs to hear about regardless.
    CHECK(centre.submit(event("b", NotificationEvent::PrinterFault, 10), hour(12)) ==
          DeliveryDecision::Deliver);
}

TEST_CASE("Quiet hours suppress ordinary alerts only", "[notifications]")
{
    NotificationPreferences preferences;
    preferences.quiet_from_s = hour(22);
    preferences.quiet_to_s   = hour(7);
    // Isolate quiet-hours behaviour from deduplication, which is covered
    // separately below.
    preferences.dedup_window_s = 0;

    NotificationCenter centre;
    centre.set_preferences(preferences);

    CHECK(centre.submit(event("a", NotificationEvent::PrintCompleted, 0), hour(23)) ==
          DeliveryDecision::RecordOnly);
    CHECK(centre.submit(event("b", NotificationEvent::PrintCompleted, 10), hour(12)) ==
          DeliveryDecision::Deliver);
    // Intervention still wakes you up.
    CHECK(centre.submit(event("c", NotificationEvent::InterventionRequired, 20), hour(23)) ==
          DeliveryDecision::Deliver);
}

TEST_CASE("Quiet hours that wrap midnight are handled", "[notifications]")
{
    NotificationPreferences preferences;
    preferences.quiet_from_s = hour(22);
    preferences.quiet_to_s   = hour(7);
    preferences.dedup_window_s = 0;

    NotificationCenter centre;
    centre.set_preferences(preferences);

    // Both sides of midnight are inside the window.
    CHECK(centre.submit(event("a", NotificationEvent::PrintCompleted, 0), hour(23)) ==
          DeliveryDecision::RecordOnly);
    CHECK(centre.submit(event("b", NotificationEvent::PrintCompleted, 1), hour(3)) ==
          DeliveryDecision::RecordOnly);
    // ...and the middle of the day is outside it.
    CHECK(centre.submit(event("c", NotificationEvent::PrintCompleted, 2), hour(14)) ==
          DeliveryDecision::Deliver);
}

TEST_CASE("Repeated identical events are deduplicated", "[notifications]")
{
    NotificationPreferences preferences;
    preferences.dedup_window_s = 300;

    NotificationCenter centre;
    centre.set_preferences(preferences);

    CHECK(centre.submit(event("a", NotificationEvent::ConnectionLost, 1000), hour(12)) ==
          DeliveryDecision::Deliver);
    CHECK(centre.submit(event("b", NotificationEvent::ConnectionLost, 1100), hour(12)) ==
          DeliveryDecision::Duplicate);
    // A duplicate must not pile up in the list either.
    CHECK(centre.all().size() == 1);

    // Outside the window it is news again.
    CHECK(centre.submit(event("c", NotificationEvent::ConnectionLost, 2000), hour(12)) ==
          DeliveryDecision::Deliver);
}

TEST_CASE("Deduplication is per printer", "[notifications]")
{
    NotificationCenter centre;
    CHECK(centre.submit(event("a", NotificationEvent::ConnectionLost, 1000, "printer1"), hour(12)) ==
          DeliveryDecision::Deliver);
    // A different machine losing connection is a separate problem.
    CHECK(centre.submit(event("b", NotificationEvent::ConnectionLost, 1010, "printer2"), hour(12)) ==
          DeliveryDecision::Deliver);
}

TEST_CASE("Acknowledgement clears the attention count", "[notifications]")
{
    NotificationCenter centre;
    centre.submit(event("a", NotificationEvent::PrintCompleted, 0), hour(12));
    centre.submit(event("b", NotificationEvent::UploadFailed, 10), hour(12));
    REQUIRE(centre.unacknowledged_count() == 2);

    CHECK(centre.acknowledge("a"));
    CHECK(centre.unacknowledged_count() == 1);
    CHECK_FALSE(centre.acknowledge("missing"));

    centre.acknowledge_all();
    CHECK(centre.unacknowledged_count() == 0);
    CHECK(centre.unacknowledged().empty());
}

TEST_CASE("Operating-system notifications are off by default", "[notifications]")
{
    const NotificationCenter centre;
    // Local-first: nothing leaves the application unless asked for.
    CHECK_FALSE(centre.preferences().os_notifications_enabled);
}
