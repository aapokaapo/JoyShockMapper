#include "linux/LinuxNotificationManager.h"

#ifdef HAVE_XDG_NOTIFICATIONS

#include <gio/gio.h>
#include <iostream>
#include <atomic>
#include <mutex>

namespace LinuxNotifications
{

namespace
{
// Monotonically-increasing counter used to give each notification a unique ID.
std::atomic<unsigned int> g_notif_counter{ 0 };
} // anonymous namespace

// ---------------------------------------------------------------------------
// setupActionHandler
// ---------------------------------------------------------------------------
// Subscribes to org.freedesktop.portal.Notification.ActionInvoked on the
// session bus.  The empty handler tells GNOME that the action was handled
// locally, which prevents the startup-notification spinner that would
// otherwise appear while GNOME waits for an acknowledgement.
//
// This is called early in main() (before connectDevices()) so that the
// signal subscription is in place before any notifications are sent.  It is
// also called from the GTK thread (before gtk_main()) when a tray icon is
// present, so that a running GLib main loop can dispatch the signal.
// std::call_once ensures the subscription is created only once regardless
// of how many times this function is called.
//
// In headless mode (--headless / systemd service) there is no GTK thread,
// so the early call from main() is the only invocation.  sendNotification()
// uses fully synchronous GLib/GIO calls and does not require a running GLib
// main loop, so notifications work correctly in that environment too.
void setupActionHandler()
{
	static std::once_flag flag;
	std::call_once(flag, []() {
		GError *error = nullptr;
		GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
		if (!conn)
		{
			if (error)
				g_error_free(error);
			return; // nothing to unref – g_bus_get_sync returned null on failure
		}

		g_dbus_connection_signal_subscribe(
		  conn,
		  nullptr,                               // sender: any (portal may use different name)
		  "org.freedesktop.portal.Notification", // interface
		  "ActionInvoked",                       // signal name
		  "/org/freedesktop/portal/desktop",     // object path
		  nullptr,                               // arg0 filter
		  G_DBUS_SIGNAL_FLAGS_NONE,
		  [](GDBusConnection *, const gchar *, const gchar *, const gchar *,
		     const gchar *, GVariant *, gpointer) {
			  // Intentionally empty – acknowledging the signal is sufficient
			  // to tell GNOME 49+ that the action was handled locally and
			  // that no spinner is needed.
		  },
		  nullptr,  // user_data
		  nullptr); // user_data_free_func

		// Release our reference; GLib keeps the shared connection alive so
		// the signal subscription remains active for the process lifetime.
		g_object_unref(conn);
	});
}

// ---------------------------------------------------------------------------
// tryClassicNotification
// ---------------------------------------------------------------------------
// Sends a notification via the classic org.freedesktop.Notifications D-Bus
// interface (the libnotify/notify-send protocol).  This interface has no
// app-id requirement and works from any process, including systemd user
// services that are not launched via D-Bus activation.  Returns true on
// success, false if the notification daemon is not available or an error
// occurs.
static bool tryClassicNotification(
  GDBusConnection *conn,
  const std::string &summary,
  const std::string &body,
  Urgency urgency,
  int expireTimeoutMs)
{
	// Map Urgency to the hint value expected by the classic spec (0=low,1=normal,2=critical).
	guchar urgencyByte;
	switch (urgency)
	{
	case Urgency::Low:      urgencyByte = 0; break;
	case Urgency::Critical: urgencyByte = 2; break;
	default:                urgencyByte = 1; break;
	}

	// Build the hints dict: {"urgency": byte}.
	GVariantBuilder hints_builder;
	g_variant_builder_init(&hints_builder, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&hints_builder, "{sv}", "urgency",
	  g_variant_new_byte(urgencyByte));

	// Build the actions array (empty – we don't register any actions).
	GVariantBuilder actions_builder;
	g_variant_builder_init(&actions_builder, G_VARIANT_TYPE("as"));

	// org.freedesktop.Notifications.Notify signature:
	// (STRING app_name, UINT32 replaces_id, STRING app_icon,
	//  STRING summary, STRING body,
	//  ARRAY<STRING> actions, DICT<STRING,VARIANT> hints,
	//  INT32 expire_timeout)
	// → (UINT32 notification_id)
	GError *error = nullptr;
	GVariant *result = g_dbus_connection_call_sync(
	  conn,
	  "org.freedesktop.Notifications",
	  "/org/freedesktop/Notifications",
	  "org.freedesktop.Notifications",
	  "Notify",
	  g_variant_new("(susssasa{sv}i)",
	    "JoyShockMapper",         // app_name
	    static_cast<guint32>(0),  // replaces_id (0 = new notification)
	    "JoyShockMapper",         // app_icon
	    summary.c_str(),          // summary
	    body.c_str(),             // body
	    &actions_builder,
	    &hints_builder,
	    expireTimeoutMs),
	  G_VARIANT_TYPE("(u)"),
	  G_DBUS_CALL_FLAGS_NONE,
	  -1,
	  nullptr,
	  &error);

	if (!result)
	{
		if (error)
		{
			std::cerr << "[Notifications] org.freedesktop.Notifications.Notify failed: "
			          << error->message << '\n';
			g_error_free(error);
		}
		return false;
	}

	g_variant_unref(result);
	return true;
}

// ---------------------------------------------------------------------------
// tryPortalNotification
// ---------------------------------------------------------------------------
// Sends a notification via the XDG Desktop Portal
// (org.freedesktop.portal.Notification).  This is the preferred path for
// sandboxed apps (Flatpak/Snap), but on GNOME 45+ it silently discards
// notifications from processes that have no registered app-id (e.g. a
// systemd user service that was not launched via D-Bus activation).
// Returns true if the D-Bus call itself did not return an error; note that
// a true return does not guarantee the notification was actually displayed.
static bool tryPortalNotification(
  GDBusConnection *conn,
  const std::string &summary,
  const std::string &body,
  Urgency urgency)
{
	// Map our Urgency enum to the portal priority string.
	const char *priority;
	switch (urgency)
	{
	case Urgency::Low:      priority = "low";    break;
	case Urgency::Critical: priority = "urgent"; break;
	default:                priority = "normal"; break;
	}

	// Build the notification dictionary required by the portal spec.
	GVariantBuilder notif_builder;
	g_variant_builder_init(&notif_builder, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&notif_builder, "{sv}", "title",
	  g_variant_new_string(summary.c_str()));
	if (!body.empty())
	{
		g_variant_builder_add(&notif_builder, "{sv}", "body",
		  g_variant_new_string(body.c_str()));
	}
	g_variant_builder_add(&notif_builder, "{sv}", "priority",
	  g_variant_new_string(priority));

	// Each notification gets a unique ID so the portal can replace or remove
	// it individually if needed.
	std::string id = "jsm-" + std::to_string(g_notif_counter.fetch_add(1));

	// org.freedesktop.portal.Notification.AddNotification signature:
	// (STRING id, DICT<STRING,VARIANT> notification) → ()
	GError *error = nullptr;
	GVariant *result = g_dbus_connection_call_sync(
	  conn,
	  "org.freedesktop.portal.Desktop",
	  "/org/freedesktop/portal/desktop",
	  "org.freedesktop.portal.Notification",
	  "AddNotification",
	  g_variant_new("(sa{sv})", id.c_str(), &notif_builder),
	  G_VARIANT_TYPE("()"),
	  G_DBUS_CALL_FLAGS_NONE,
	  -1,
	  nullptr,
	  &error);

	if (!result)
	{
		if (error)
		{
			std::cerr << "[Notifications] XDG portal AddNotification failed: "
			          << error->message << '\n';
			g_error_free(error);
		}
		return false;
	}

	g_variant_unref(result);
	return true;
}

// ---------------------------------------------------------------------------
// sendNotification
// ---------------------------------------------------------------------------
bool sendNotification(
  const std::string &summary,
  const std::string &body,
  Urgency urgency,
  int expireTimeoutMs)
{
	GError *error = nullptr;
	GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
	if (!conn)
	{
		if (error)
		{
			std::cerr << "[Notifications] Cannot connect to session bus: " << error->message << '\n';
			g_error_free(error);
		}
		return false;
	}

	// Try the classic org.freedesktop.Notifications interface first.  It
	// works from any process (including systemd user services) without
	// requiring an app-id and is universally supported on all desktops.
	// Fall back to the XDG Desktop Portal only if the classic daemon is
	// absent – the portal silently drops notifications on GNOME 45+ when
	// it cannot associate the caller with a registered .desktop app-id.
	bool sent = tryClassicNotification(conn, summary, body, urgency, expireTimeoutMs);
	if (!sent)
		sent = tryPortalNotification(conn, summary, body, urgency);

	g_object_unref(conn);

	if (!sent)
		std::cerr << "[Notifications] All notification backends failed\n";

	return sent;
}

} // namespace LinuxNotifications

#endif // HAVE_XDG_NOTIFICATIONS
