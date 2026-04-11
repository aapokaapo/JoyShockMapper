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
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>

// Define standard gamepad button codes if not already defined
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

size_t Gamepad::_count = 0;

Gamepad::Gamepad()
{
	++_count;
}

Gamepad::~Gamepad()
{
	--_count;
}

// Namespace for filtering virtual gamepad devices
namespace DeviceFilterNamespace
{
	static constexpr const char* VIRTUAL_DEVICE_NAME = "JoyShockMapper";
	
	bool isVirtualDevice(const std::string& deviceName)
	{
		return deviceName.find(VIRTUAL_DEVICE_NAME) != std::string::npos ||
		       deviceName.find("Xbox") != std::string::npos ||
		       deviceName.find("DualShock") != std::string::npos;
	}
	
	std::string getDeviceName(int eventNum)
	{
		std::string path = "/sys/class/input/event" + std::to_string(eventNum) + "/device/name";
		std::ifstream file(path);
		std::string name;
		if (file.is_open())
		{
			std::getline(file, name);
			// Remove trailing whitespace
			name.erase(name.find_last_not_of(" \n\r\t") + 1);
			file.close();
		}
		return name;
	}
}

// Base implementation class for uinput virtual devices
class UinputGamepad : public Gamepad
{
protected:
	int uinput_fd = -1;
	bool initialized = false;

	struct ControllerState {
		uint16_t buttons = 0;
		uint8_t left_trigger = 0;
		uint8_t right_trigger = 0;
		int16_t left_stick_x = 0;
		int16_t left_stick_y = 0;
		int16_t right_stick_x = 0;
		int16_t right_stick_y = 0;
	} state;

	void sendEvent(uint16_t type, uint16_t code, int32_t value)
	{
		if (uinput_fd < 0) return;

		struct input_event event;
		memset(&event, 0, sizeof(event));
		event.type = type;
		event.code = code;
		event.value = value;
		gettimeofday(&event.time, nullptr);

		ssize_t ret = write(uinput_fd, &event, sizeof(event));
		(void)ret; // Suppress unused variable warning
	}

	void sendSync()
	{
		sendEvent(EV_SYN, SYN_REPORT, 0);
	}

	void setupAbsAxis(int axis, int min_val, int max_val, int flat = 0)
	{
		ioctl(uinput_fd, UI_SET_ABSBIT, axis);
		struct uinput_abs_setup abs_setup;
		memset(&abs_setup, 0, sizeof(abs_setup));
		abs_setup.code = axis;
		abs_setup.absinfo.minimum = min_val;
		abs_setup.absinfo.maximum = max_val;
		abs_setup.absinfo.fuzz = 0;
		abs_setup.absinfo.flat = flat;
		ioctl(uinput_fd, UI_ABS_SETUP, &abs_setup);
	}

public:
	virtual ~UinputGamepad()
	{
		if (uinput_fd >= 0) {
			ioctl(uinput_fd, UI_DEV_DESTROY);
			close(uinput_fd);
		}
	}

	virtual bool isInitialized(string* errorMsg = nullptr) const override
	{
		if (errorMsg) {
			*errorMsg = _errorMsg;
		}
		return initialized;
	}

	virtual void setStick(float x, float y, bool isLeft) override
	{
		if (isLeft) {
			setLeftStick(x, y);
		} else {
			setRightStick(x, y);
		}
	}

	virtual void setGyro(TimePoint now, float accelX, float accelY, float accelZ,
	                     float gyroX, float gyroY, float gyroZ) override
	{
		// Gyro data is not supported in standard Linux gamepad emulation
		// Would require custom kernel driver
	}

	virtual void setTouchState(optional<FloatXY> press1, optional<FloatXY> press2) override
	{
		// Touch input is not supported in standard gamepad emulation
	}

	virtual void update() override
	{
		sendSync();
	}
};

// Xbox controller virtual device
class XboxGamepadImpl : public UinputGamepad
{
public:
	XboxGamepadImpl(Callback notification)
	{
		initializeUinput();
		if (!initialized) {
			std::cerr << "[Xbox Gamepad] Failed to initialize: " << _errorMsg << std::endl;
		} else {
			std::cerr << "[Xbox Gamepad] Successfully initialized" << std::endl;
		}
	}

private:
	void initializeUinput()
	{
		uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		if (uinput_fd < 0) {
			_errorMsg = "Failed to open /dev/uinput: " + std::string(strerror(errno));
			std::cerr << "[Xbox Gamepad] " << _errorMsg << std::endl;
			return;
		}

		// Set up device capabilities
		if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
			_errorMsg = "Failed to set EV_KEY: " + std::string(strerror(errno));
			std::cerr << "[Xbox Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		if (ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0) {
			_errorMsg = "Failed to set EV_ABS: " + std::string(strerror(errno));
			std::cerr << "[Xbox Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		// Set button capabilities
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_SOUTH);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_EAST);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_NORTH);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_WEST);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TL);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TR);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TL2);     // Left trigger (ZL)
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TR2);     // Right trigger (ZT)
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_SELECT);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_START);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_THUMBL);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_THUMBR);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_MODE);
		// Add D-Pad buttons
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_UP);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_DOWN);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_LEFT);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_RIGHT);

		// Set analog axis capabilities
		setupAbsAxis(ABS_X, -32768, 32767, 128);
		setupAbsAxis(ABS_Y, -32768, 32767, 128);
		setupAbsAxis(ABS_RX, -32768, 32767, 128);
		setupAbsAxis(ABS_RY, -32768, 32767, 128);
		setupAbsAxis(ABS_Z, 0, 255, 0);
		setupAbsAxis(ABS_RZ, 0, 255, 0);

		struct uinput_setup usetup;
		memset(&usetup, 0, sizeof(usetup));
		usetup.id.bustype = BUS_VIRTUAL;
		usetup.id.vendor = 0x045e;  // Microsoft
		usetup.id.product = 0x02ea; // Xbox 360 Controller
		strncpy(usetup.name, "JoyShockMapper Xbox", UINPUT_MAX_NAME_SIZE - 1);
		usetup.name[UINPUT_MAX_NAME_SIZE - 1] = '\0';

		if (ioctl(uinput_fd, UI_DEV_SETUP, &usetup) < 0) {
			_errorMsg = "Failed to setup uinput device: " + std::string(strerror(errno));
			std::cerr << "[Xbox Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
			_errorMsg = "Failed to create uinput device: " + std::string(strerror(errno));
			std::cerr << "[Xbox Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		usleep(100000);
		initialized = true;
	}

	uint16_t mapButtonToEvdev(KeyCode btn)
	{
		// Map JSM button codes to evdev button codes
		// btn.code contains the button code constants from PlatformDefinitions.h
		if (btn.code == X_A) return BTN_SOUTH;
		if (btn.code == X_B) return BTN_EAST;
		if (btn.code == X_X) return BTN_NORTH;
		if (btn.code == X_Y) return BTN_WEST;
		if (btn.code == X_LB) return BTN_TL;
		if (btn.code == X_RB) return BTN_TR;
		if (btn.code == X_LT) return BTN_TL2;         // Left trigger
		if (btn.code == X_RT) return BTN_TR2;         // Right trigger
		if (btn.code == X_BACK) return BTN_SELECT;
		if (btn.code == X_START) return BTN_START;
		if (btn.code == X_LS) return BTN_THUMBL;
		if (btn.code == X_RS) return BTN_THUMBR;
		if (btn.code == X_GUIDE) return BTN_MODE;
		// D-Pad mappings
		if (btn.code == X_UP) return BTN_DPAD_UP;
		if (btn.code == X_DOWN) return BTN_DPAD_DOWN;
		if (btn.code == X_LEFT) return BTN_DPAD_LEFT;
		if (btn.code == X_RIGHT) return BTN_DPAD_RIGHT;
		return 0;
	}

public:
	virtual void setButton(KeyCode btn, bool pressed) override
	{
		if (!initialized) return;

		uint16_t evdev_btn = mapButtonToEvdev(btn);
		if (evdev_btn == 0) return;

		sendEvent(EV_KEY, evdev_btn, pressed ? 1 : 0);
		sendSync();
	}

	virtual void setLeftStick(float x, float y) override
	{
		if (!initialized) return;

		int16_t x_val = static_cast<int16_t>(x * 32767.0f);
		int16_t y_val = static_cast<int16_t>(-y * 32767.0f);

		if (state.left_stick_x != x_val) {
			sendEvent(EV_ABS, ABS_X, x_val);
			state.left_stick_x = x_val;
		}

		if (state.left_stick_y != y_val) {
			sendEvent(EV_ABS, ABS_Y, y_val);
			state.left_stick_y = y_val;
		}

		sendSync();
	}

	virtual void setRightStick(float x, float y) override
	{
		if (!initialized) return;

		int16_t x_val = static_cast<int16_t>(x * 32767.0f);
		int16_t y_val = static_cast<int16_t>(-y * 32767.0f);

		if (state.right_stick_x != x_val) {
			sendEvent(EV_ABS, ABS_RX, x_val);
			state.right_stick_x = x_val;
		}

		if (state.right_stick_y != y_val) {
			sendEvent(EV_ABS, ABS_RY, y_val);
			state.right_stick_y = y_val;
		}

		sendSync();
	}

	virtual void setLeftTrigger(float val) override
	{
		if (!initialized) return;

		uint8_t trigger_val = static_cast<uint8_t>(val * 255.0f);

		if (state.left_trigger != trigger_val) {
			bool wasPressed = state.left_trigger > 0;
			bool isPressed = trigger_val > 0;

			sendEvent(EV_ABS, ABS_Z, trigger_val);
			if (isPressed != wasPressed)
				sendEvent(EV_KEY, BTN_TL2, isPressed ? 1 : 0);

			state.left_trigger = trigger_val;
			sendSync();
		}
	}

	virtual void setRightTrigger(float val) override
	{
		if (!initialized) return;

		uint8_t trigger_val = static_cast<uint8_t>(val * 255.0f);

		if (state.right_trigger != trigger_val) {
			bool wasPressed = state.right_trigger > 0;
			bool isPressed = trigger_val > 0;

			sendEvent(EV_ABS, ABS_RZ, trigger_val);
			if (isPressed != wasPressed)
				sendEvent(EV_KEY, BTN_TR2, isPressed ? 1 : 0);

			state.right_trigger = trigger_val;
			sendSync();
		}
	}

	virtual ControllerScheme getType() const override
	{
		return ControllerScheme::XBOX;
	}
};

// DS4 controller virtual device
class DS4GamepadImpl : public UinputGamepad
{
public:
	DS4GamepadImpl(Callback notification)
	{
		initializeUinput();
		if (!initialized) {
			std::cerr << "[DS4 Gamepad] Failed to initialize: " << _errorMsg << std::endl;
		} else {
			std::cerr << "[DS4 Gamepad] Successfully initialized" << std::endl;
		}
	}

private:
	void initializeUinput()
	{
		uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		if (uinput_fd < 0) {
			_errorMsg = "Failed to open /dev/uinput: " + std::string(strerror(errno));
			std::cerr << "[DS4 Gamepad] " << _errorMsg << std::endl;
			return;
		}

		// Set up device capabilities
		if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
			_errorMsg = "Failed to set EV_KEY: " + std::string(strerror(errno));
			std::cerr << "[DS4 Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		if (ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0) {
			_errorMsg = "Failed to set EV_ABS: " + std::string(strerror(errno));
			std::cerr << "[DS4 Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		// Set button capabilities for DS4
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_WEST);    // Square
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_NORTH);   // Triangle
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_EAST);    // Circle
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_SOUTH);   // Cross
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TL);      // L1
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TR);      // R1
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TL2);     // L2 trigger (ZL)
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TR2);     // R2 trigger (ZT)
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_SELECT);  // Share
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_START);   // Options
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_THUMBL);  // L3
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_THUMBR);  // R3
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_MODE);    // PS button
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOUCH);   // Touchpad click
		// Add D-Pad buttons for DS4
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_UP);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_DOWN);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_LEFT);
		ioctl(uinput_fd, UI_SET_KEYBIT, BTN_DPAD_RIGHT);

		// Set analog axis capabilities - DS4 uses 0-255 range
		setupAbsAxis(ABS_X, 0, 255, 15);
		setupAbsAxis(ABS_Y, 0, 255, 15);
		setupAbsAxis(ABS_RX, 0, 255, 15);
		setupAbsAxis(ABS_RY, 0, 255, 15);
		setupAbsAxis(ABS_Z, 0, 255, 0);
		setupAbsAxis(ABS_RZ, 0, 255, 0);

		struct uinput_setup usetup;
		memset(&usetup, 0, sizeof(usetup));
		usetup.id.bustype = BUS_VIRTUAL;
		usetup.id.vendor = 0x054c;  // Sony
		usetup.id.product = 0x05c4; // DualShock 4
		strncpy(usetup.name, "JoyShockMapper DualShock 4", UINPUT_MAX_NAME_SIZE - 1);
		usetup.name[UINPUT_MAX_NAME_SIZE - 1] = '\0';

		if (ioctl(uinput_fd, UI_DEV_SETUP, &usetup) < 0) {
			_errorMsg = "Failed to setup uinput device: " + std::string(strerror(errno));
			std::cerr << "[DS4 Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
			_errorMsg = "Failed to create uinput device: " + std::string(strerror(errno));
			std::cerr << "[DS4 Gamepad] " << _errorMsg << std::endl;
			close(uinput_fd);
			uinput_fd = -1;
			return;
		}

		usleep(100000);
		initialized = true;
	}

	uint16_t mapButtonToEvdev(KeyCode btn)
	{
		// Map JSM PS button codes to evdev codes
		// btn.code contains the button code constants from PlatformDefinitions.h
		if (btn.code == PS_SQUARE) return BTN_WEST;
		if (btn.code == PS_TRIANGLE) return BTN_NORTH;
		if (btn.code == PS_CIRCLE) return BTN_EAST;
		if (btn.code == PS_CROSS) return BTN_SOUTH;
		if (btn.code == PS_L1) return BTN_TL;
		if (btn.code == PS_R1) return BTN_TR;
		if (btn.code == PS_L2) return BTN_TL2;        // Left trigger
		if (btn.code == PS_R2) return BTN_TR2;        // Right trigger
		if (btn.code == PS_SHARE) return BTN_SELECT;
		if (btn.code == PS_OPTIONS) return BTN_START;
		if (btn.code == PS_L3) return BTN_THUMBL;
		if (btn.code == PS_R3) return BTN_THUMBR;
		if (btn.code == PS_HOME) return BTN_MODE;
		if (btn.code == PS_PAD_CLICK) return BTN_TOUCH; // Touchpad click
		// D-Pad mappings
		if (btn.code == PS_UP) return BTN_DPAD_UP;
		if (btn.code == PS_DOWN) return BTN_DPAD_DOWN;
		if (btn.code == PS_LEFT) return BTN_DPAD_LEFT;
		if (btn.code == PS_RIGHT) return BTN_DPAD_RIGHT;
		return 0;
	}

public:
	virtual void setButton(KeyCode btn, bool pressed) override
	{
		if (!initialized) return;

		uint16_t evdev_btn = mapButtonToEvdev(btn);
		if (evdev_btn == 0) return;

		sendEvent(EV_KEY, evdev_btn, pressed ? 1 : 0);
		sendSync();
	}

	virtual void setLeftStick(float x, float y) override
	{
		if (!initialized) return;

		// DS4 uses 0-255 range centered at 128
		uint8_t x_val = static_cast<uint8_t>((x + 1.0f) * 127.5f);
		uint8_t y_val = static_cast<uint8_t>((1.0f - y) * 127.5f);

		if (state.left_stick_x != static_cast<int16_t>(x_val)) {
			sendEvent(EV_ABS, ABS_X, x_val);
			state.left_stick_x = x_val;
		}

		if (state.left_stick_y != static_cast<int16_t>(y_val)) {
			sendEvent(EV_ABS, ABS_Y, y_val);
			state.left_stick_y = y_val;
		}

		sendSync();
	}

	virtual void setRightStick(float x, float y) override
	{
		if (!initialized) return;

		uint8_t x_val = static_cast<uint8_t>((x + 1.0f) * 127.5f);
		uint8_t y_val = static_cast<uint8_t>((1.0f - y) * 127.5f);

		if (state.right_stick_x != static_cast<int16_t>(x_val)) {
			sendEvent(EV_ABS, ABS_RX, x_val);
			state.right_stick_x = x_val;
		}

		if (state.right_stick_y != static_cast<int16_t>(y_val)) {
			sendEvent(EV_ABS, ABS_RY, y_val);
			state.right_stick_y = y_val;
		}

		sendSync();
	}

	virtual void setLeftTrigger(float val) override
	{
		if (!initialized) return;

		uint8_t trigger_val = static_cast<uint8_t>(val * 255.0f);

		if (state.left_trigger != trigger_val) {
			bool wasPressed = state.left_trigger > 0;
			bool isPressed = trigger_val > 0;

			sendEvent(EV_ABS, ABS_Z, trigger_val);
			if (isPressed != wasPressed)
				sendEvent(EV_KEY, BTN_TL2, isPressed ? 1 : 0);

			state.left_trigger = trigger_val;
			sendSync();
		}
	}

	virtual void setRightTrigger(float val) override
	{
		if (!initialized) return;

		uint8_t trigger_val = static_cast<uint8_t>(val * 255.0f);

		if (state.right_trigger != trigger_val) {
			bool wasPressed = state.right_trigger > 0;
			bool isPressed = trigger_val > 0;

			sendEvent(EV_ABS, ABS_RZ, trigger_val);
			if (isPressed != wasPressed)
				sendEvent(EV_KEY, BTN_TR2, isPressed ? 1 : 0);

			state.right_trigger = trigger_val;
			sendSync();
		}
	}

	virtual ControllerScheme getType() const override
	{
		return ControllerScheme::DS4;
	}
};

// Forward declarations for the libmanette-backed factory functions defined in
// GamepadLibmanette.cpp.  These are tried first; if they return nullptr the
// code falls back to the plain uinput implementations below.
Gamepad* createLibmanetteXboxGamepad(Gamepad::Callback notification);
Gamepad* createLibmanetteDS4Gamepad(Gamepad::Callback notification);

Gamepad* Gamepad::getNew(ControllerScheme scheme, Callback notification)
{
	switch (scheme) {
		case ControllerScheme::XBOX:
		{
			// Try the libmanette / improved-uinput backend first.
			Gamepad* pad = createLibmanetteXboxGamepad(notification);
			if (pad)
				return pad;
			// Fall back to the original uinput implementation.
			std::cerr << "[Gamepad] libmanette Xbox backend unavailable, falling back to uinput\n";
			return new XboxGamepadImpl(notification);
		}
		case ControllerScheme::DS4:
		{
			Gamepad* pad = createLibmanetteDS4Gamepad(notification);
			if (pad)
				return pad;
			std::cerr << "[Gamepad] libmanette DS4 backend unavailable, falling back to uinput\n";
			return new DS4GamepadImpl(notification);
		}
		default:
			return nullptr;
	}
}
