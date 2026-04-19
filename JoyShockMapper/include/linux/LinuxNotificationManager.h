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

// Send a desktop notification via the org.freedesktop.Notifications D-Bus interface.
// Returns true on success, false if the notification service is unavailable or an
// error occurs (failure is non-fatal; the caller should not abort on false).
bool sendNotification(
  const std::string &summary,
  const std::string &body = "",
  Urgency urgency = Urgency::Normal,
  int expireTimeoutMs = 7000);

} // namespace LinuxNotifications

#endif // __linux__
