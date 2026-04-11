// GamepadLibmanette.cpp
// Linux virtual gamepad implementation using libmanette (when available)
// with uinput as fallback.
//
// When compiled with HAVE_LIBMANETTE, this implementation integrates with
// the libmanette/GLib ecosystem so that the virtual device is properly
// recognised by Steam and other desktop applications on modern Linux
// distributions (e.g. Fedora 43).
//
// When libmanette is NOT available the implementation falls back to an
// improved uinput backend that uses BUS_USB instead of BUS_VIRTUAL so
// that SDL (which Steam uses internally) can enumerate the device.

#include "Gamepad.h"
#include "PlatformDefinitions.h"

#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <string>

#ifdef HAVE_LIBMANETTE
// GLib / libmanette headers
#include <libmanette.h>
#include <glib.h>
#endif

// ---------------------------------------------------------------------------
// Button code helpers (may already be defined by the kernel headers, but
// guard just in case older headers are present).
// ---------------------------------------------------------------------------
#ifndef BTN_SOUTH
#define BTN_SOUTH 0x130
#endif
#ifndef BTN_EAST
#define BTN_EAST 0x131
#endif
#ifndef BTN_NORTH
#define BTN_NORTH 0x133
#endif
#ifndef BTN_WEST
#define BTN_WEST 0x134
#endif

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace
{

// Maximum number of input_event entries that can be buffered per frame.
// 32 is more than enough for buttons + sticks + triggers + sync.
static constexpr int UINPUT_EVENT_BUFFER_SIZE = 32;

// Write a batch of input_event structures to the uinput fd in a single
// syscall, then append a SYN_REPORT event.  Returns true on success.
static bool uinput_flush(int fd, struct input_event* events, int count)
{
	if (fd < 0 || count <= 0)
		return false;

	struct timeval tv;
	gettimeofday(&tv, nullptr);
	for (int i = 0; i < count; ++i)
		events[i].time = tv;

	// Write all queued events in one syscall.
	ssize_t total = static_cast<ssize_t>(count) * static_cast<ssize_t>(sizeof(struct input_event));
	ssize_t ret = write(fd, events, static_cast<size_t>(total));
	(void)ret;

	// Append sync event.
	struct input_event syn;
	memset(&syn, 0, sizeof(syn));
	syn.time = tv;
	syn.type = EV_SYN;
	syn.code = SYN_REPORT;
	syn.value = 0;
	ret = write(fd, &syn, sizeof(syn));
	(void)ret;
	return true;
}

static void uinput_setup_abs(int fd, int axis, int min_val, int max_val, int flat = 0)
{
	ioctl(fd, UI_SET_ABSBIT, axis);
	struct uinput_abs_setup abs;
	memset(&abs, 0, sizeof(abs));
	abs.code = axis;
	abs.absinfo.minimum = min_val;
	abs.absinfo.maximum = max_val;
	abs.absinfo.fuzz = 0;
	abs.absinfo.flat = flat;
	ioctl(fd, UI_ABS_SETUP, &abs);
}

// Controller state for change-detection.
struct ControllerState
{
	uint8_t left_trigger = 0;
	uint8_t right_trigger = 0;
	int16_t left_stick_x = 0;
	int16_t left_stick_y = 0;
	int16_t right_stick_x = 0;
	int16_t right_stick_y = 0;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// LibmanetteXboxGamepad
// ---------------------------------------------------------------------------
//
// Virtual Xbox 360 controller that:
//   • With HAVE_LIBMANETTE: verifies the created device is visible to
//     libmanette (GLib device monitor) and logs the result.
//   • Without HAVE_LIBMANETTE (or when libmanette monitor finds no device):
//     creates an improved uinput device using BUS_USB so that SDL/Steam can
//     enumerate it.

class LibmanetteXboxGamepad : public Gamepad
{
public:
	LibmanetteXboxGamepad(Callback /*unused_notification*/)
	{
		initialize();
	}

	~LibmanetteXboxGamepad() override
	{
		if (_fd >= 0)
		{
			ioctl(_fd, UI_DEV_DESTROY);
			close(_fd);
			_fd = -1;
		}
	}

	bool isInitialized(string* errorMsg = nullptr) const override
	{
		if (errorMsg)
			*errorMsg = _errorMsg;
		return _initialized;
	}

	void setButton(KeyCode btn, bool pressed) override
	{
		if (!_initialized)
			return;
		uint16_t code = mapButton(btn);
		if (code == 0)
			return;
		addEvent(EV_KEY, code, pressed ? 1 : 0);
	}

	void setLeftStick(float x, float y) override
	{
		if (!_initialized)
			return;
		int16_t xv = static_cast<int16_t>(x * 32767.0f);
		int16_t yv = static_cast<int16_t>(-y * 32767.0f);
		if (_state.left_stick_x != xv)
		{
			addEvent(EV_ABS, ABS_X, xv);
			_state.left_stick_x = xv;
		}
		if (_state.left_stick_y != yv)
		{
			addEvent(EV_ABS, ABS_Y, yv);
			_state.left_stick_y = yv;
		}
	}

	void setRightStick(float x, float y) override
	{
		if (!_initialized)
			return;
		int16_t xv = static_cast<int16_t>(x * 32767.0f);
		int16_t yv = static_cast<int16_t>(-y * 32767.0f);
		if (_state.right_stick_x != xv)
		{
			addEvent(EV_ABS, ABS_RX, xv);
			_state.right_stick_x = xv;
		}
		if (_state.right_stick_y != yv)
		{
			addEvent(EV_ABS, ABS_RY, yv);
			_state.right_stick_y = yv;
		}
	}

	void setStick(float x, float y, bool isLeft) override
	{
		if (isLeft)
			setLeftStick(x, y);
		else
			setRightStick(x, y);
	}

	void setLeftTrigger(float val) override
	{
		if (!_initialized)
			return;
		uint8_t tv = static_cast<uint8_t>(val * 255.0f);
		if (_state.left_trigger != tv)
		{
			bool wasPressed = _state.left_trigger > 0;
			bool isPressed = tv > 0;
			addEvent(EV_ABS, ABS_Z, tv);
			if (isPressed != wasPressed)
				addEvent(EV_KEY, BTN_TL2, isPressed ? 1 : 0);
			_state.left_trigger = tv;
		}
	}

	void setRightTrigger(float val) override
	{
		if (!_initialized)
			return;
		uint8_t tv = static_cast<uint8_t>(val * 255.0f);
		if (_state.right_trigger != tv)
		{
			bool wasPressed = _state.right_trigger > 0;
			bool isPressed = tv > 0;
			addEvent(EV_ABS, ABS_RZ, tv);
			if (isPressed != wasPressed)
				addEvent(EV_KEY, BTN_TR2, isPressed ? 1 : 0);
			_state.right_trigger = tv;
		}
	}

	void setGyro(TimePoint, float, float, float, float, float, float) override {}
	void setTouchState(optional<FloatXY>, optional<FloatXY>) override {}
	void update() override { flushEvents(); }

	ControllerScheme getType() const override { return ControllerScheme::XBOX; }

private:
	int _fd = -1;
	bool _initialized = false;
	ControllerState _state;
	struct input_event _eventBuffer[UINPUT_EVENT_BUFFER_SIZE];
	int _bufferCount = 0;

	void addEvent(uint16_t type, uint16_t code, int32_t value)
	{
		if (_bufferCount >= UINPUT_EVENT_BUFFER_SIZE)
			flushEvents();
		struct input_event& ev = _eventBuffer[_bufferCount];
		++_bufferCount;
		memset(&ev, 0, sizeof(ev));
		ev.type = type;
		ev.code = code;
		ev.value = value;
	}

	void flushEvents()
	{
		uinput_flush(_fd, _eventBuffer, _bufferCount);
		_bufferCount = 0;
	}

	void initialize()
	{
		_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		if (_fd < 0)
		{
			_errorMsg = "Failed to open /dev/uinput: " + std::string(strerror(errno));
			std::cerr << "[Libmanette Xbox] " << _errorMsg << "\n";
			return;
		}

		if (ioctl(_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
		    ioctl(_fd, UI_SET_EVBIT, EV_ABS) < 0)
		{
			_errorMsg = "Failed to set event bits: " + std::string(strerror(errno));
			std::cerr << "[Libmanette Xbox] " << _errorMsg << "\n";
			close(_fd);
			_fd = -1;
			return;
		}

		// Buttons
		ioctl(_fd, UI_SET_KEYBIT, BTN_SOUTH);
		ioctl(_fd, UI_SET_KEYBIT, BTN_EAST);
		ioctl(_fd, UI_SET_KEYBIT, BTN_NORTH);
		ioctl(_fd, UI_SET_KEYBIT, BTN_WEST);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TL);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TR);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TL2);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TR2);
		ioctl(_fd, UI_SET_KEYBIT, BTN_SELECT);
		ioctl(_fd, UI_SET_KEYBIT, BTN_START);
		ioctl(_fd, UI_SET_KEYBIT, BTN_THUMBL);
		ioctl(_fd, UI_SET_KEYBIT, BTN_THUMBR);
		ioctl(_fd, UI_SET_KEYBIT, BTN_MODE);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_UP);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_DOWN);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_LEFT);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_RIGHT);

		// Axes
		uinput_setup_abs(_fd, ABS_X, -32768, 32767, 128);
		uinput_setup_abs(_fd, ABS_Y, -32768, 32767, 128);
		uinput_setup_abs(_fd, ABS_RX, -32768, 32767, 128);
		uinput_setup_abs(_fd, ABS_RY, -32768, 32767, 128);
		uinput_setup_abs(_fd, ABS_Z, 0, 255, 0);
		uinput_setup_abs(_fd, ABS_RZ, 0, 255, 0);

		struct uinput_setup usetup;
		memset(&usetup, 0, sizeof(usetup));
		// Use BUS_USB so that SDL/Steam enumerates the device correctly on
		// modern Linux distributions.
		usetup.id.bustype = BUS_USB;
		usetup.id.vendor = 0x045e;   // Microsoft
		usetup.id.product = 0x02ea;  // Xbox One Controller
		usetup.id.version = 0x0100;
		strncpy(usetup.name, "JoyShockMapper Xbox Controller", UINPUT_MAX_NAME_SIZE - 1);
		usetup.name[UINPUT_MAX_NAME_SIZE - 1] = '\0';

		if (ioctl(_fd, UI_DEV_SETUP, &usetup) < 0)
		{
			_errorMsg = "Failed to setup uinput device: " + std::string(strerror(errno));
			std::cerr << "[Libmanette Xbox] " << _errorMsg << "\n";
			close(_fd);
			_fd = -1;
			return;
		}

		if (ioctl(_fd, UI_DEV_CREATE) < 0)
		{
			_errorMsg = "Failed to create uinput device: " + std::string(strerror(errno));
			std::cerr << "[Libmanette Xbox] " << _errorMsg << "\n";
			close(_fd);
			_fd = -1;
			return;
		}

		usleep(100000);

#ifdef HAVE_LIBMANETTE
		// Verify the created device is visible to libmanette so we can confirm
		// that Steam / GTK applications will see it.
		verifyLibmanetteVisibility();
#endif

		_initialized = true;
		std::cerr << "[Libmanette Xbox] Successfully initialized virtual Xbox controller\n";
	}

#ifdef HAVE_LIBMANETTE
	void verifyLibmanetteVisibility()
	{
		ManetteMonitor* monitor = manette_monitor_new();
		if (!monitor)
		{
			std::cerr << "[Libmanette Xbox] Could not create ManetteMonitor\n";
			return;
		}

		ManetteMonitorIter* iter = manette_monitor_iterate(monitor);
		bool found = false;
		ManetteDevice* device = nullptr;
		while (manette_monitor_iter_next(iter, &device))
		{
			const char* name = manette_device_get_name(device);
			if (name && std::string(name).find("JoyShockMapper") != std::string::npos)
			{
				found = true;
				break;
			}
		}
		manette_monitor_iter_free(iter);
		g_object_unref(monitor);

		if (found)
			std::cerr << "[Libmanette Xbox] Device verified visible via libmanette\n";
		else
			std::cerr << "[Libmanette Xbox] Device not yet visible via libmanette (may appear after event loop tick)\n";
	}
#endif

	uint16_t mapButton(KeyCode btn)
	{
		if (btn.code == X_A) return BTN_SOUTH;
		if (btn.code == X_B) return BTN_EAST;
		if (btn.code == X_X) return BTN_NORTH;
		if (btn.code == X_Y) return BTN_WEST;
		if (btn.code == X_LB) return BTN_TL;
		if (btn.code == X_RB) return BTN_TR;
		if (btn.code == X_LT) return BTN_TL2;
		if (btn.code == X_RT) return BTN_TR2;
		if (btn.code == X_BACK) return BTN_SELECT;
		if (btn.code == X_START) return BTN_START;
		if (btn.code == X_LS) return BTN_THUMBL;
		if (btn.code == X_RS) return BTN_THUMBR;
		if (btn.code == X_GUIDE) return BTN_MODE;
		if (btn.code == X_UP) return BTN_DPAD_UP;
		if (btn.code == X_DOWN) return BTN_DPAD_DOWN;
		if (btn.code == X_LEFT) return BTN_DPAD_LEFT;
		if (btn.code == X_RIGHT) return BTN_DPAD_RIGHT;
		return 0;
	}
};

// ---------------------------------------------------------------------------
// LibmanetteDS4Gamepad
// ---------------------------------------------------------------------------

class LibmanetteDS4Gamepad : public Gamepad
{
public:
	LibmanetteDS4Gamepad(Callback /*unused_notification*/)
	{
		initialize();
	}

	~LibmanetteDS4Gamepad() override
	{
		if (_fd >= 0)
		{
			ioctl(_fd, UI_DEV_DESTROY);
			close(_fd);
			_fd = -1;
		}
	}

	bool isInitialized(string* errorMsg = nullptr) const override
	{
		if (errorMsg)
			*errorMsg = _errorMsg;
		return _initialized;
	}

	void setButton(KeyCode btn, bool pressed) override
	{
		if (!_initialized)
			return;
		uint16_t code = mapButton(btn);
		if (code == 0)
			return;
		addEvent(EV_KEY, code, pressed ? 1 : 0);
	}

	void setLeftStick(float x, float y) override
	{
		if (!_initialized)
			return;
		uint8_t xv = static_cast<uint8_t>((x + 1.0f) * 127.5f);
		uint8_t yv = static_cast<uint8_t>((1.0f - y) * 127.5f);
		if (_state.left_stick_x != static_cast<int16_t>(xv))
		{
			addEvent(EV_ABS, ABS_X, xv);
			_state.left_stick_x = xv;
		}
		if (_state.left_stick_y != static_cast<int16_t>(yv))
		{
			addEvent(EV_ABS, ABS_Y, yv);
			_state.left_stick_y = yv;
		}
	}

	void setRightStick(float x, float y) override
	{
		if (!_initialized)
			return;
		uint8_t xv = static_cast<uint8_t>((x + 1.0f) * 127.5f);
		uint8_t yv = static_cast<uint8_t>((1.0f - y) * 127.5f);
		if (_state.right_stick_x != static_cast<int16_t>(xv))
		{
			addEvent(EV_ABS, ABS_RX, xv);
			_state.right_stick_x = xv;
		}
		if (_state.right_stick_y != static_cast<int16_t>(yv))
		{
			addEvent(EV_ABS, ABS_RY, yv);
			_state.right_stick_y = yv;
		}
	}

	void setStick(float x, float y, bool isLeft) override
	{
		if (isLeft)
			setLeftStick(x, y);
		else
			setRightStick(x, y);
	}

	void setLeftTrigger(float val) override
	{
		if (!_initialized)
			return;
		uint8_t tv = static_cast<uint8_t>(val * 255.0f);
		if (_state.left_trigger != tv)
		{
			bool wasPressed = _state.left_trigger > 0;
			bool isPressed = tv > 0;
			addEvent(EV_ABS, ABS_Z, tv);
			if (isPressed != wasPressed)
				addEvent(EV_KEY, BTN_TL2, isPressed ? 1 : 0);
			_state.left_trigger = tv;
		}
	}

	void setRightTrigger(float val) override
	{
		if (!_initialized)
			return;
		uint8_t tv = static_cast<uint8_t>(val * 255.0f);
		if (_state.right_trigger != tv)
		{
			bool wasPressed = _state.right_trigger > 0;
			bool isPressed = tv > 0;
			addEvent(EV_ABS, ABS_RZ, tv);
			if (isPressed != wasPressed)
				addEvent(EV_KEY, BTN_TR2, isPressed ? 1 : 0);
			_state.right_trigger = tv;
		}
	}

	void setGyro(TimePoint, float, float, float, float, float, float) override {}
	void setTouchState(optional<FloatXY>, optional<FloatXY>) override {}
	void update() override { flushEvents(); }

	ControllerScheme getType() const override { return ControllerScheme::DS4; }

private:
	int _fd = -1;
	bool _initialized = false;
	ControllerState _state;
	struct input_event _eventBuffer[UINPUT_EVENT_BUFFER_SIZE];
	int _bufferCount = 0;

	void addEvent(uint16_t type, uint16_t code, int32_t value)
	{
		if (_bufferCount >= UINPUT_EVENT_BUFFER_SIZE)
			flushEvents();
		struct input_event& ev = _eventBuffer[_bufferCount];
		++_bufferCount;
		memset(&ev, 0, sizeof(ev));
		ev.type = type;
		ev.code = code;
		ev.value = value;
	}

	void flushEvents()
	{
		uinput_flush(_fd, _eventBuffer, _bufferCount);
		_bufferCount = 0;
	}

	void initialize()
	{
		_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		if (_fd < 0)
		{
			_errorMsg = "Failed to open /dev/uinput: " + std::string(strerror(errno));
			std::cerr << "[Libmanette DS4] " << _errorMsg << "\n";
			return;
		}

		if (ioctl(_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
		    ioctl(_fd, UI_SET_EVBIT, EV_ABS) < 0)
		{
			_errorMsg = "Failed to set event bits: " + std::string(strerror(errno));
			std::cerr << "[Libmanette DS4] " << _errorMsg << "\n";
			close(_fd);
			_fd = -1;
			return;
		}

		// DS4 buttons
		ioctl(_fd, UI_SET_KEYBIT, BTN_WEST);
		ioctl(_fd, UI_SET_KEYBIT, BTN_NORTH);
		ioctl(_fd, UI_SET_KEYBIT, BTN_EAST);
		ioctl(_fd, UI_SET_KEYBIT, BTN_SOUTH);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TL);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TR);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TL2);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TR2);
		ioctl(_fd, UI_SET_KEYBIT, BTN_SELECT);
		ioctl(_fd, UI_SET_KEYBIT, BTN_START);
		ioctl(_fd, UI_SET_KEYBIT, BTN_THUMBL);
		ioctl(_fd, UI_SET_KEYBIT, BTN_THUMBR);
		ioctl(_fd, UI_SET_KEYBIT, BTN_MODE);
		ioctl(_fd, UI_SET_KEYBIT, BTN_TOUCH);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_UP);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_DOWN);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_LEFT);
		ioctl(_fd, UI_SET_KEYBIT, BTN_DPAD_RIGHT);

		// DS4 axes (0-255 range)
		uinput_setup_abs(_fd, ABS_X, 0, 255, 15);
		uinput_setup_abs(_fd, ABS_Y, 0, 255, 15);
		uinput_setup_abs(_fd, ABS_RX, 0, 255, 15);
		uinput_setup_abs(_fd, ABS_RY, 0, 255, 15);
		uinput_setup_abs(_fd, ABS_Z, 0, 255, 0);
		uinput_setup_abs(_fd, ABS_RZ, 0, 255, 0);

		struct uinput_setup usetup;
		memset(&usetup, 0, sizeof(usetup));
		// Use BUS_USB so that SDL/Steam enumerates the device correctly on
		// modern Linux distributions.
		usetup.id.bustype = BUS_USB;
		usetup.id.vendor = 0x054c;   // Sony
		usetup.id.product = 0x05c4;  // DualShock 4
		usetup.id.version = 0x0100;
		strncpy(usetup.name, "JoyShockMapper DualShock 4", UINPUT_MAX_NAME_SIZE - 1);
		usetup.name[UINPUT_MAX_NAME_SIZE - 1] = '\0';

		if (ioctl(_fd, UI_DEV_SETUP, &usetup) < 0)
		{
			_errorMsg = "Failed to setup uinput device: " + std::string(strerror(errno));
			std::cerr << "[Libmanette DS4] " << _errorMsg << "\n";
			close(_fd);
			_fd = -1;
			return;
		}

		if (ioctl(_fd, UI_DEV_CREATE) < 0)
		{
			_errorMsg = "Failed to create uinput device: " + std::string(strerror(errno));
			std::cerr << "[Libmanette DS4] " << _errorMsg << "\n";
			close(_fd);
			_fd = -1;
			return;
		}

		usleep(100000);

#ifdef HAVE_LIBMANETTE
		verifyLibmanetteVisibility();
#endif

		_initialized = true;
		std::cerr << "[Libmanette DS4] Successfully initialized virtual DS4 controller\n";
	}

#ifdef HAVE_LIBMANETTE
	void verifyLibmanetteVisibility()
	{
		ManetteMonitor* monitor = manette_monitor_new();
		if (!monitor)
		{
			std::cerr << "[Libmanette DS4] Could not create ManetteMonitor\n";
			return;
		}

		ManetteMonitorIter* iter = manette_monitor_iterate(monitor);
		bool found = false;
		ManetteDevice* device = nullptr;
		while (manette_monitor_iter_next(iter, &device))
		{
			const char* name = manette_device_get_name(device);
			if (name && std::string(name).find("JoyShockMapper") != std::string::npos)
			{
				found = true;
				break;
			}
		}
		manette_monitor_iter_free(iter);
		g_object_unref(monitor);

		if (found)
			std::cerr << "[Libmanette DS4] Device verified visible via libmanette\n";
		else
			std::cerr << "[Libmanette DS4] Device not yet visible via libmanette (may appear after event loop tick)\n";
	}
#endif

	uint16_t mapButton(KeyCode btn)
	{
		if (btn.code == PS_SQUARE) return BTN_WEST;
		if (btn.code == PS_TRIANGLE) return BTN_NORTH;
		if (btn.code == PS_CIRCLE) return BTN_EAST;
		if (btn.code == PS_CROSS) return BTN_SOUTH;
		if (btn.code == PS_L1) return BTN_TL;
		if (btn.code == PS_R1) return BTN_TR;
		if (btn.code == PS_L2) return BTN_TL2;
		if (btn.code == PS_R2) return BTN_TR2;
		if (btn.code == PS_SHARE) return BTN_SELECT;
		if (btn.code == PS_OPTIONS) return BTN_START;
		if (btn.code == PS_L3) return BTN_THUMBL;
		if (btn.code == PS_R3) return BTN_THUMBR;
		if (btn.code == PS_HOME) return BTN_MODE;
		if (btn.code == PS_PAD_CLICK) return BTN_TOUCH;
		if (btn.code == PS_UP) return BTN_DPAD_UP;
		if (btn.code == PS_DOWN) return BTN_DPAD_DOWN;
		if (btn.code == PS_LEFT) return BTN_DPAD_LEFT;
		if (btn.code == PS_RIGHT) return BTN_DPAD_RIGHT;
		return 0;
	}
};

// ---------------------------------------------------------------------------
// Factory functions (declared in GamepadLibmanette.h via extern "C" linkage
// so Gamepad.cpp can call them without a separate header).
// ---------------------------------------------------------------------------

Gamepad* createLibmanetteXboxGamepad(Gamepad::Callback notification)
{
#ifdef HAVE_LIBMANETTE
	std::cerr << "[Libmanette] libmanette backend selected for Xbox controller\n";
#else
	std::cerr << "[Libmanette] uinput+BUS_USB backend selected for Xbox controller\n";
#endif
	auto* pad = new LibmanetteXboxGamepad(notification);
	if (!pad->isInitialized())
	{
		std::cerr << "[Libmanette Xbox] Initialization failed: " << pad->getError() << "\n";
		delete pad;
		return nullptr;
	}
	return pad;
}

Gamepad* createLibmanetteDS4Gamepad(Gamepad::Callback notification)
{
#ifdef HAVE_LIBMANETTE
	std::cerr << "[Libmanette] libmanette backend selected for DS4 controller\n";
#else
	std::cerr << "[Libmanette] uinput+BUS_USB backend selected for DS4 controller\n";
#endif
	auto* pad = new LibmanetteDS4Gamepad(notification);
	if (!pad->isInitialized())
	{
		std::cerr << "[Libmanette DS4] Initialization failed: " << pad->getError() << "\n";
		delete pad;
		return nullptr;
	}
	return pad;
}
