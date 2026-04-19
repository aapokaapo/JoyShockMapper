#pragma once

#ifdef __linux__

#include <string>

namespace LinuxNotifications
{

enum class Urgency
{
	Low = 0,
	Normal = 1,
	Critical = 2,
};

// Default notification timeout in milliseconds.
// Note: the XDG Desktop Portal does not expose a per-notification timeout;
// this value is kept for API compatibility and is currently unused by the
// portal back-end.
static constexpr int kDefaultExpireMs = 7000;

#ifdef HAVE_XDG_NOTIFICATIONS
// Send a desktop notification via the XDG Desktop Portal
// (org.freedesktop.portal.Notification – the modern standard on GNOME 45+
// and Fedora 43).  Returns true on success, false if the portal is
// unavailable or an error occurs (failure is non-fatal; the caller should
// not abort on false).
bool sendNotification(
  const std::string &summary,
  const std::string &body = "",
  Urgency urgency = Urgency::Normal,
  int expireTimeoutMs = kDefaultExpireMs);

// Subscribe to org.freedesktop.portal.Notification.ActionInvoked on the
// session bus so that GNOME 49+ does not show a startup-notification spinner
// when the user clicks a desktop notification.
//
// Called early in main() before connectDevices() so that the subscription is
// active before any notification is sent.  Also called from the GTK thread
// (before gtk_main()) when a tray icon is active so that a running GLib main
// loop can dispatch the signal.  In headless (--headless / systemd service)
// mode the GTK thread never starts, so the early call from main() is the
// only invocation.  std::call_once ensures the subscription is created once.
void setupActionHandler();
#else
// XDG Desktop Portal unavailable at build time – notifications silently disabled.
inline bool sendNotification(
  const std::string & /*summary*/,
  const std::string & /*body*/ = "",
  Urgency /*urgency*/ = Urgency::Normal,
  int /*expireTimeoutMs*/ = kDefaultExpireMs)
{
	return false;
}
inline void setupActionHandler() {}
#endif // HAVE_XDG_NOTIFICATIONS

} // namespace LinuxNotifications

#endif // __linux__
