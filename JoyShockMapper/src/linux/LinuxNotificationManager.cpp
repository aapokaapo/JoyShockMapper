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
// Call this from the GTK thread (before gtk_main()) so that signal delivery
// is dispatched by the running GLib main loop.
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
// sendNotification
// ---------------------------------------------------------------------------
bool sendNotification(
  const std::string &summary,
  const std::string &body,
  Urgency urgency,
  int /*expireTimeoutMs*/) // portal controls timeout; parameter kept for ABI
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

	g_object_unref(conn);

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

} // namespace LinuxNotifications

#endif // HAVE_XDG_NOTIFICATIONS
