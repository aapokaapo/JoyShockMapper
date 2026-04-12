#include <algorithm>
#include <string>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>

#include "InputHelpers.h"

// Minimal sd_notify implementation that does not require libsystemd.
// Sends a status string to the socket specified by the NOTIFY_SOCKET
// environment variable (as described in sd_notify(3)).
static void sd_notify_msg(const char *msg)
{
	const char *socket_path = ::getenv("NOTIFY_SOCKET");
	if (socket_path == nullptr || socket_path[0] == '\0')
		return;

	int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;

	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	// NOTIFY_SOCKET may be an abstract socket (starting with '@')
	if (socket_path[0] == '@')
	{
		addr.sun_path[0] = '\0';
		::strncpy(addr.sun_path + 1, socket_path + 1, sizeof(addr.sun_path) - 2);
		addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	}
	else
	{
		::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
		addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	}

	::sendto(fd, msg, ::strlen(msg), MSG_NOSIGNAL,
	         reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
	::close(fd);
}

static std::atomic<bool> g_watchdog_running{ false };

// Start a background thread that sends systemd watchdog keepalives.
// The interval is half of WATCHDOG_USEC to guarantee at least one ping
// per watchdog period.
static void start_watchdog()
{
	const char *watchdog_usec = ::getenv("WATCHDOG_USEC");
	if (watchdog_usec == nullptr || watchdog_usec[0] == '\0')
		return;

	unsigned long usec = 0;
	try { usec = std::stoul(watchdog_usec); }
	catch (const std::invalid_argument &) { return; }
	catch (const std::out_of_range &) { return; }
	if (usec == 0)
		return;

	g_watchdog_running = true;
	auto interval = std::chrono::microseconds(usec / 2);
	// The thread is detached because joining it from a signal handler is not
	// async-signal-safe.  The thread exits naturally once g_watchdog_running
	// is cleared by terminateHandler, and any in-progress sd_notify_msg call
	// holds no resources that require explicit cleanup.
	std::thread([interval]() {
		while (g_watchdog_running.load())
		{
			sd_notify_msg("WATCHDOG=1");
			std::this_thread::sleep_for(interval);
		}
	}).detach();
}

static void terminateHandler(int signo)
{
	if (signo != SIGTERM && signo != SIGINT)
	{
		std::fprintf(stderr, "Caught unexpected signal %d\n", signo);
		return;
	}

	sd_notify_msg("STOPPING=1");
	g_watchdog_running = false;
	WriteToConsole("QUIT");
}

static void reloadHandler(int /*signo*/)
{
	// SIGHUP: reconnect all physical controllers and recreate virtual devices.
	// Note: WriteToConsole is not strictly async-signal-safe (it uses a mutex),
	// but this follows the same pattern as the existing terminateHandler above
	// and matches the overall signal-handling approach in this codebase.
	WriteToConsole("RECONNECT_CONTROLLERS");
}

static const int initialize = [] {
	std::string appRootDir{};

	// Check if we're running as an AppImage, and if so set up the root directory
	const auto APPDIR = ::getenv("APPDIR");
	if (APPDIR != nullptr)
	{
		const auto userId = ::getuid();
		std::printf("\n\033[1;33mRunning as AppImage, make sure user %d has RW permissions to /dev/uinput and /dev/hidraw*\n\033[0m", userId);
		appRootDir = APPDIR;
	}

	// Create the configuration directories for JSM
	std::string configDirectory;

	// Get the default config path, or if not defined set the default to $HOME/.config
	const auto XDG_CONFIG_HOME = getenv("XDG_CONFIG_HOME");
	if (XDG_CONFIG_HOME == nullptr)
	{
		const auto home = getenv("HOME");
		// HOME may be unset when running as a systemd system service; fall back to /var/lib/joyshockmapper
		configDirectory = (home != nullptr ? std::string{ home } : std::string{ "/var/lib/joyshockmapper" }) + "/.config";
		::mkdir(configDirectory.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	}
	else
	{
		configDirectory = XDG_CONFIG_HOME;
	}

	// Create the base configuration directory if it doesn't already exist
	::mkdir(configDirectory.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	configDirectory = configDirectory + "/JoyShockMapper/";
	::mkdir(configDirectory.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

	// Create the AutoLoad directory
	::mkdir((configDirectory + "AutoLoad/").c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

	// Create the GyroConfigs directory
	configDirectory += "GyroConfigs/";
	::mkdir(configDirectory.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

	const auto globalConfigDirectory = appRootDir + "/etc/JoyShockMapper/GyroConfigs/";

	// Copy the configuration files from etc to the local configuration, without overwritting
	const auto globalConfigFiles = ListDirectory(globalConfigDirectory);
	const auto localConfigFiles = ListDirectory(configDirectory);

	for (const auto &gFile : globalConfigFiles)
	{
		const auto it = std::find(localConfigFiles.begin(), localConfigFiles.end(), gFile);
		if (it == localConfigFiles.end())
		{
			auto source = ::open((globalConfigDirectory + gFile).c_str(), O_RDONLY, 0);
			auto dest = ::open((configDirectory + gFile).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

			struct stat stat_source;
			::fstat(source, &stat_source);
			::sendfile(dest, source, 0, stat_source.st_size);

			::close(source);
			::close(dest);
		}
	}

	signal(SIGTERM, &terminateHandler);
	signal(SIGINT, &terminateHandler);
	signal(SIGHUP, &reloadHandler);

	start_watchdog();
	sd_notify_msg("READY=1");

	return 0;
}();
