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
static constexpr int kDefaultExpireMs = 7000;

#ifdef HAVE_GIO_NOTIFICATIONS
// Send a desktop notification via the org.freedesktop.Notifications D-Bus interface.
// Returns true on success, false if the notification service is unavailable or an
// error occurs (failure is non-fatal; the caller should not abort on false).
// Clicking the notification dismisses it without triggering any further action.
bool sendNotification(
  const std::string &summary,
  const std::string &body = "",
  Urgency urgency = Urgency::Normal,
  int expireTimeoutMs = kDefaultExpireMs);
#else
// GIO/D-Bus unavailable at build time – notifications silently disabled.
inline bool sendNotification(
  const std::string & /*summary*/,
  const std::string & /*body*/ = "",
  Urgency /*urgency*/ = Urgency::Normal,
  int /*expireTimeoutMs*/ = kDefaultExpireMs)
{
	return false;
}
#endif // HAVE_GIO_NOTIFICATIONS

} // namespace LinuxNotifications

#endif // __linux__
