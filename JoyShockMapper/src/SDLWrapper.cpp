#include "JSMVariable.hpp"
#include "JslWrapper.h"
#include "JSMVariable.hpp"
 #include "TriggerEffectGenerator.h"
#include "SettingsManager.h"
#include "SDL3/SDL.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#define _USE_MATH_DEFINES
#include <math.h> // M_PI
#include <algorithm>
#include <memory>
#include <iostream>
#include <cstring>
#include <span>

typedef struct
{
	Uint8 ucEnableBits1;              /* 0 */
	Uint8 ucEnableBits2;              /* 1 */
	Uint8 ucRumbleRight;              /* 2 */
	Uint8 ucRumbleLeft;               /* 3 */
	Uint8 ucHeadphoneVolume;          /* 4 */
	Uint8 ucSpeakerVolume;            /* 5 */
	Uint8 ucMicrophoneVolume;         /* 6 */
	Uint8 ucAudioEnableBits;          /* 7 */
	Uint8 ucMicLightMode;             /* 8 */
	Uint8 ucAudioMuteBits;            /* 9 */
	Uint8 rgucRightTriggerEffect[11]; /* 10 */
	Uint8 rgucLeftTriggerEffect[11];  /* 21 */
	Uint8 rgucUnknown1[6];            /* 32 */
	Uint8 ucLedFlags;                 /* 38 */
	Uint8 rgucUnknown2[2];            /* 39 */
	Uint8 ucLedAnim;                  /* 41 */
	Uint8 ucLedBrightness;            /* 42 */
	Uint8 ucPadLights;                /* 43 */
	Uint8 ucLedRed;                   /* 44 */
	Uint8 ucLedGreen;                 /* 45 */
	Uint8 ucLedBlue;                  /* 46 */
} DS5EffectsState_t;

struct ControllerDevice
{
	ControllerDevice(int id)
	  : _has_accel(false)
	  , _has_gyro(false)
	{
		_prevTouchState.t0Down = false;
		_prevTouchState.t1Down = false;
		if (SDL_IsGamepad(id))
		{
			_sdlController = nullptr;
			for (int retry = 3; retry > 0 && _sdlController == nullptr; --retry)
			{
				_sdlController = SDL_OpenGamepad(id);

				if (_sdlController == nullptr)
				{
					CERR << SDL_GetError() << ". Trying again!\n";
					SDL_Delay(1000);
				}
				else
				{
					_has_gyro = SDL_GamepadHasSensor(_sdlController, SDL_SENSOR_GYRO);
					_has_accel = SDL_GamepadHasSensor(_sdlController, SDL_SENSOR_ACCEL);

					if (_has_gyro)
					{
						SDL_SetGamepadSensorEnabled(_sdlController, SDL_SENSOR_GYRO, true);
					}
					if (_has_accel)
					{
						SDL_SetGamepadSensorEnabled(_sdlController, SDL_SENSOR_ACCEL, true);
					}

					int vid = SDL_GetGamepadVendor(_sdlController);
					int pid = SDL_GetGamepadProduct(_sdlController);

					auto sdl_ctrlr_type = SDL_GetGamepadType(_sdlController);
					switch (sdl_ctrlr_type)
					{
					case SDL_GamepadType::SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
						_ctrlr_type = JS_TYPE_JOYCON_LEFT;
						_split_type = JS_SPLIT_TYPE_LEFT;
						break;
					case SDL_GamepadType::SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
						_ctrlr_type = JS_TYPE_JOYCON_RIGHT;
						_split_type = JS_SPLIT_TYPE_RIGHT;
						break;
					case SDL_GamepadType::SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
					case SDL_GamepadType::SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
						_ctrlr_type = JS_TYPE_PRO_CONTROLLER;
						break;
					case SDL_GamepadType::SDL_GAMEPAD_TYPE_PS4:
						_ctrlr_type = JS_TYPE_DS4;
						break;
					case SDL_GamepadType::SDL_GAMEPAD_TYPE_PS5:
						_ctrlr_type = JS_TYPE_DS;
						break;
					case SDL_GamepadType::SDL_GAMEPAD_TYPE_XBOXONE:
						_ctrlr_type = JS_TYPE_XBOXONE;
						if (vid == 0x0e6f) // PDP Vendor ID
						{
							_ctrlr_type = JS_TYPE_XBOX_SERIES;
						}
						if (vid == 0x24c6) // PowerA
						{
							_ctrlr_type = JS_TYPE_XBOX_SERIES;
						}
						if (vid == 0x045e) // Microsoft Vendor ID
						{
							switch (pid)
							{
							case(0x02e3): // Xbox Elite Series 1
								// Intentional fall through to the next case
							case(0x0b05): //Xbox Elite Series 2 - Bluetooth
								// Intentional fall through to the next case
							case(0x0b00): //Xbox Elite Series 2
							case (0x02ff): //XBOXGIP driver software PID - not sure what this is, might be from Valve's driver for using Elite paddles
								// in any case, this is what my ELite Series 2 is showing as currently, so adding it here for now    
								_ctrlr_type = JS_TYPE_XBOXONE_ELITE;
								break;
							case(0x0b12): //Xbox Series controller
								// Intentional fall through to the next case
							case(0x0b13): // Xbox Series controller - bluetooth
								_ctrlr_type = JS_TYPE_XBOX_SERIES;
								break;
							}
						}
						break;
					}
				}// next attempt?
			}
		}
	}

	virtual ~ControllerDevice()
	{
		_micLight = 0;
		memset(&_leftTriggerEffect, 0, sizeof(_leftTriggerEffect));
		memset(&_rightTriggerEffect, 0, sizeof(_rightTriggerEffect));
		_big_rumble = 0;
		_small_rumble = 0;
		SendEffect();
		SDL_CloseGamepad(_sdlController);
	}

	inline bool isValid()
	{
		return _sdlController != nullptr;
	}

private:
	void LoadTriggerEffect(uint8_t *rgucTriggerEffect, const AdaptiveTriggerSetting *trigger_effect)
	{
		using namespace ExtendInput::DataTools::DualSense;
		rgucTriggerEffect[0] = (uint8_t)trigger_effect->mode;
		switch (trigger_effect->mode)
		{
		case AdaptiveTriggerMode::RESISTANCE_RAW:
		{
			TriggerEffectGenerator::Simple_Feedback(rgucTriggerEffect, 0, trigger_effect->start, trigger_effect->force);
		}
		break;
		case AdaptiveTriggerMode::SEGMENT:
			rgucTriggerEffect[1] = trigger_effect->start;
			rgucTriggerEffect[2] = trigger_effect->end;
			rgucTriggerEffect[3] = trigger_effect->force;
			break;
		case AdaptiveTriggerMode::RESISTANCE:
			TriggerEffectGenerator::Feedback(rgucTriggerEffect, 0, trigger_effect->start, trigger_effect->force);
			break;
		case AdaptiveTriggerMode::BOW:
			TriggerEffectGenerator::Bow(rgucTriggerEffect, 0, trigger_effect->start, trigger_effect->end, trigger_effect->force, trigger_effect->forceExtra);
			break;
		case AdaptiveTriggerMode::GALLOPING:
			TriggerEffectGenerator::Galloping(rgucTriggerEffect, 0, trigger_effect->start, trigger_effect->end, trigger_effect->force, trigger_effect->forceExtra, trigger_effect->frequency);
			break;
	    case AdaptiveTriggerMode::SEMI_AUTOMATIC:
			TriggerEffectGenerator::Simple_Weapon(rgucTriggerEffect, 0, trigger_effect->start, trigger_effect->end, trigger_effect->force);
			break;
		case AdaptiveTriggerMode::AUTOMATIC:
			TriggerEffectGenerator::Simple_Vibration(rgucTriggerEffect, 0, trigger_effect->start, trigger_effect->force, trigger_effect->frequency);
			break;
		case AdaptiveTriggerMode::MACHINE:
			TriggerEffectGenerator::Machine(rgucTriggerEffect, 0, trigger_effect->start, trigger_effect->end, trigger_effect->force, trigger_effect->forceExtra, trigger_effect->frequency, trigger_effect->frequencyExtra);
			break;
		default:
			rgucTriggerEffect[0] = 0x05; // no effect
		}
	}

public:
	void SendEffect()
	{
		if (_ctrlr_type == JS_TYPE_DS)
		{
			if (!_effectDirty)
				return;

			DS5EffectsState_t effectPacket;
			memset(&effectPacket, 0, sizeof(effectPacket));

			// Add adaptive trigger data
			effectPacket.ucEnableBits1 |= 0x08 | 0x04; // Enable left and right trigger effect respectively
			LoadTriggerEffect(effectPacket.rgucLeftTriggerEffect, &_leftTriggerEffect);
			LoadTriggerEffect(effectPacket.rgucRightTriggerEffect, &_rightTriggerEffect);

			// Add current rumbling data
			effectPacket.ucEnableBits1 |= 0x01 | 0x02;
			effectPacket.ucRumbleLeft = _big_rumble >> 8;
			effectPacket.ucRumbleRight = _small_rumble >> 8;

			// Add current mic light
			effectPacket.ucEnableBits2 |= 0x01;      /* Enable microphone light */
			effectPacket.ucMicLightMode = _micLight; /* Bitmask, 0x00 = off, 0x01 = solid, 0x02 = pulse */

			// Only send if packet content has actually changed
			if (memcmp(&effectPacket, &_cachedEffectPacket, sizeof(effectPacket)) != 0)
			{
				_cachedEffectPacket = effectPacket;
				SDL_SendGamepadEffect(_sdlController, &effectPacket, sizeof(effectPacket));
			}
			_effectDirty = false;
		}
	}

	bool _has_gyro;
	bool _has_accel;
	int _split_type = JS_SPLIT_TYPE_FULL;
	int _ctrlr_type = 0;
	uint16_t _small_rumble = 0;
	uint16_t _big_rumble = 0;
	bool _rumble_dirty = false; // Only send rumble when values change
	AdaptiveTriggerSetting _leftTriggerEffect;
	AdaptiveTriggerSetting _rightTriggerEffect;
	uint8_t _micLight = 0;
	SDL_Gamepad *_sdlController = nullptr;
	TOUCH_STATE _prevTouchState;
	DS5EffectsState_t _cachedEffectPacket{}; // Cached DS5 packet – rebuilt only when effect parameters change
	bool _effectDirty = true;               // Force a send on first tick
};

struct SdlInstance : public JslWrapper
{
public:
	SdlInstance()
	{
		SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "0");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOYCON_HOME_LED, "0");
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_HOME_LED, "0");
		SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
		SDL_Init(SDL_INIT_GAMEPAD);
	}

	virtual ~SdlInstance()
	{
		SDL_Quit();
	}

	int pollDevices()
	{
		// Cache the pointer to TICK_TIME setting once – avoids map lookup + dynamic_cast every tick.
		auto *tickTimeSetting = SettingsManager::get<float>(SettingID::TICK_TIME);

		while (keep_polling)
		{
			auto tick_time = tickTimeSetting->value();
			SDL_Delay(Uint32(tick_time));

			// Snapshot SDL state under lock, then release before firing callbacks.
			controller_lock.lock();
			SDL_UpdateGamepads();
			// Collect device IDs to process so callbacks run outside the lock.
			vector<int> deviceIds;
			deviceIds.reserve(_controllerMap.size());
			for (const auto& [id, device] : _controllerMap)
				deviceIds.push_back(id);
			controller_lock.unlock();

			for (int deviceId : deviceIds)
			{
				if (g_callback)
				{
					g_callback(deviceId, JOY_SHOCK_STATE{}, JOY_SHOCK_STATE{}, IMU_STATE{}, IMU_STATE{}, tick_time);
				}
				if (g_touch_callback)
				{
					lock_guard guard(controller_lock);
					auto it = _controllerMap.find(deviceId);
					if (it == _controllerMap.end())
						continue;
					TOUCH_STATE touch = GetTouchState(deviceId, false);
					g_touch_callback(deviceId, touch, it->second->_prevTouchState, tick_time);
					it->second->_prevTouchState = touch;
				}
				// Perform rumble only when the rumble values have changed.
				{
					lock_guard guard(controller_lock);
					auto it = _controllerMap.find(deviceId);
					if (it != _controllerMap.end() && it->second->_rumble_dirty)
					{
						SDL_RumbleGamepad(it->second->_sdlController, it->second->_big_rumble, it->second->_small_rumble, Uint32(tick_time + 5));
						it->second->_rumble_dirty = false;
					}
				}
			}
		}

		return 1;
	}

	SDL_JoystickID * _joysticksArray = nullptr;
	unordered_map<int, ControllerDevice *> _controllerMap;
	void (*g_callback)(int, JOY_SHOCK_STATE, JOY_SHOCK_STATE, IMU_STATE, IMU_STATE, float) = nullptr;
	void (*g_touch_callback)(int, TOUCH_STATE, TOUCH_STATE, float) = nullptr;
	atomic_bool keep_polling = false;
	mutex controller_lock;
	SDL_Thread *_pollingThread = nullptr;

	int ConnectDevices() override
	{
		bool isFalse = false;
		if (keep_polling.compare_exchange_strong(isFalse, true))
		{
			// keep polling was false! It is set to true now.
			_pollingThread = SDL_CreateThread([] (void *obj)
			{
				  auto this_ = static_cast<SdlInstance *>(obj);
				  return this_->pollDevices();
			  },
			  "Poll Devices", this);
			// Do NOT detach – we join in DisconnectAndDisposeAll() for clean teardown.
		}
		SDL_UpdateGamepads(); // Refresh driver listing
		SDL_free(_joysticksArray);
		int count = 0;
		_joysticksArray = SDL_GetJoysticks(&count);
		return count;
	}

	int GetDeviceCount() override
	{
		std::lock_guard guard(controller_lock);
		SDL_free(_joysticksArray);
		int count = 0;
		_joysticksArray = SDL_GetJoysticks(&count);
		return count;
	}

	int GetConnectedDeviceHandles(int *deviceHandleArray, int size) override
	{
		lock_guard guard(controller_lock);
		auto iter = _controllerMap.begin();
		while (iter != _controllerMap.end())
		{
			delete iter->second;
			iter = _controllerMap.erase(iter);
		}
		for (int i = 0; i < size; i++)
		{
			ControllerDevice *device = new ControllerDevice(_joysticksArray[i]);
			if (device->isValid())
			{
				deviceHandleArray[i] = i + 1;
				_controllerMap[deviceHandleArray[i]] = device;
			}
			else
			{
                deviceHandleArray[i] = -1;
				delete device;
			}
		}
		return int(_controllerMap.size());
	}

	void DisconnectAndDisposeAll() override
	{
		// Signal the polling thread to stop, then wait for it to exit cleanly.
		keep_polling = false;
		g_callback = nullptr;
		g_touch_callback = nullptr;
		if (_pollingThread)
		{
			SDL_WaitThread(_pollingThread, nullptr);
			_pollingThread = nullptr;
		}
		lock_guard guard(controller_lock);
		auto iter = _controllerMap.begin();
		while (iter != _controllerMap.end())
		{
			delete iter->second;
			iter = _controllerMap.erase(iter);
		}
		SDL_free(_joysticksArray);
		_joysticksArray = nullptr;
	}

	JOY_SHOCK_STATE GetSimpleState(int deviceId) override
	{
		return JOY_SHOCK_STATE();
	}

	IMU_STATE GetIMUState(int deviceId) override
	{
		IMU_STATE imuState;
		memset(&imuState, 0, sizeof(imuState));
		if (_controllerMap[deviceId]->_has_gyro)
		{
			array<float, 3> gyro;
			SDL_GetGamepadSensorData(_controllerMap[deviceId]->_sdlController, SDL_SENSOR_GYRO, &gyro[0], 3);
			static constexpr float toDegPerSec = float(180. / M_PI);
			imuState.gyroX = gyro[0] * toDegPerSec;
			imuState.gyroY = gyro[1] * toDegPerSec;
			imuState.gyroZ = gyro[2] * toDegPerSec;
		}
		if (_controllerMap[deviceId]->_has_accel)
		{
			array<float, 3> accel;
			SDL_GetGamepadSensorData(_controllerMap[deviceId]->_sdlController, SDL_SENSOR_ACCEL, &accel[0], 3);
			static constexpr float toGs = 1.f / 9.8f;
			imuState.accelX = accel[0] * toGs;
			imuState.accelY = accel[1] * toGs;
			imuState.accelZ = accel[2] * toGs;
		}
		return imuState;
	}

	MOTION_STATE GetMotionState(int deviceId) override
	{
		return MOTION_STATE();
	}

	TOUCH_STATE GetTouchState(int deviceId, bool previous) override
        {
            TOUCH_STATE state;
            memset(&state, 0, sizeof(TOUCH_STATE));
            
            // Get the controller pointer
            if (_controllerMap[deviceId] == nullptr)
            {
                return state;
            }
            
            auto controller = _controllerMap[deviceId]->_sdlController;
            
            // Only try to read touchpad if the controller has touchpads
            if (controller && SDL_GetNumGamepadTouchpads(controller) > 0)
            {
                if (!SDL_GetGamepadTouchpadFinger(controller, 0, 0, &state.t0Down, &state.t0X, &state.t0Y, nullptr) || 
                    !SDL_GetGamepadTouchpadFinger(controller, 0, 1, &state.t1Down, &state.t1X, &state.t1Y, nullptr))
                {
                    // Only log once or at debug level to avoid spam
                    static bool touched_error = false;
                    if (!touched_error)
                    {
                        CERR << "Cannot get finger state: " << SDL_GetError() << '\n';
                        touched_error = true;
                    }
                }
            }
            // If no touchpad, just return empty state silently
            
            return state;
        }

	bool GetTouchpadDimension(int deviceId, int &sizeX, int &sizeY) override
	{
		// I am assuming a single touchpad (or all _touchpads are the same dimension)?
		auto *jc = _controllerMap[deviceId];
		if (jc != nullptr)
		{
			switch (_controllerMap[deviceId]->_ctrlr_type)
			{
			case JS_TYPE_DS4:
			case JS_TYPE_DS:
				// Matching SDL resolution
				sizeX = 1920;
				sizeY = 920;
				break;
			default:
				sizeX = 0;
				sizeY = 0;
				break;
			}
			return true;
		}
		return false;
	}

	int GetButtons(int deviceId) override
	{
		static const map<int, int> sdl2jsl = {
			{ SDL_GAMEPAD_BUTTON_SOUTH, JSOFFSET_S },
			{ SDL_GAMEPAD_BUTTON_EAST, JSOFFSET_E },
			{ SDL_GAMEPAD_BUTTON_WEST, JSOFFSET_W },
			{ SDL_GAMEPAD_BUTTON_NORTH, JSOFFSET_N },
			{ SDL_GAMEPAD_BUTTON_BACK, JSOFFSET_MINUS },
			{ SDL_GAMEPAD_BUTTON_GUIDE, JSOFFSET_HOME },
			{ SDL_GAMEPAD_BUTTON_START, JSOFFSET_PLUS },
			{ SDL_GAMEPAD_BUTTON_LEFT_STICK, JSOFFSET_LCLICK },
			{ SDL_GAMEPAD_BUTTON_RIGHT_STICK, JSOFFSET_RCLICK },
			{ SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, JSOFFSET_L },
			{ SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, JSOFFSET_R },
			{ SDL_GAMEPAD_BUTTON_DPAD_UP, JSOFFSET_UP },
			{ SDL_GAMEPAD_BUTTON_DPAD_DOWN, JSOFFSET_DOWN },
			{ SDL_GAMEPAD_BUTTON_DPAD_LEFT, JSOFFSET_LEFT },
			{ SDL_GAMEPAD_BUTTON_DPAD_RIGHT, JSOFFSET_RIGHT }
		};

		int buttons = 0;
		for (const auto& [sdlBtn, jslOffset] : sdl2jsl)
		{
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GamepadButton(sdlBtn)) ? 1 << jslOffset : 0;

		}
		switch (_controllerMap[deviceId]->_ctrlr_type)
		{
		case JS_TYPE_JOYCON_LEFT:
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_MISC1) ? 1 << JSOFFSET_CAPTURE : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) ? 1 << JSOFFSET_SL : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2) ? 1 << JSOFFSET_SR : 0;
			break;
		case JS_TYPE_JOYCON_RIGHT:
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) ? 1 << JSOFFSET_SL : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) ? 1 << JSOFFSET_SR : 0;
			break;
		case JS_TYPE_DS:
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_MISC1) ? 1 << JSOFFSET_MIC : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) ? 1 << JSOFFSET_SR : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) ? 1 << JSOFFSET_SL : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) ? 1 << JSOFFSET_FNR : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2) ? 1 << JSOFFSET_FNL : 0;
			// Intentional fall through to the next case
		case JS_TYPE_DS4:
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_TOUCHPAD) ? 1 << JSOFFSET_CAPTURE : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) ? 1 << JSOFFSET_SL : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) ? 1 << JSOFFSET_SR : 0;
			break;
		case JS_TYPE_PRO_CONTROLLER:
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_MISC1) ? 1 << JSOFFSET_CAPTURE : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) ? 1 << JSOFFSET_SR : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) ? 1 << JSOFFSET_SL : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) ? 1 << JSOFFSET_FNR : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2) ? 1 << JSOFFSET_FNL : 0;
			break;
		default:
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_MISC1) ? 1 << JSOFFSET_CAPTURE : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) ? 1 << JSOFFSET_FNL : 0;
			buttons |= SDL_GetGamepadButton(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) ? 1 << JSOFFSET_FNR : 0;
			break;
		}
		return buttons;
	}

	float GetLeftX(int deviceId) override
	{
		return SDL_GetGamepadAxis(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_AXIS_LEFTX) / (float)SDL_JOYSTICK_AXIS_MAX;
	}

	float GetLeftY(int deviceId) override
	{
		return -SDL_GetGamepadAxis(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_AXIS_LEFTY) / (float)SDL_JOYSTICK_AXIS_MAX;
	}

	float GetRightX(int deviceId) override
	{
		return SDL_GetGamepadAxis(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_AXIS_RIGHTX) / (float)SDL_JOYSTICK_AXIS_MAX;
	}

	float GetRightY(int deviceId) override
	{
		return -SDL_GetGamepadAxis(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_AXIS_RIGHTY) / (float)SDL_JOYSTICK_AXIS_MAX;
	}

	float GetLeftTrigger(int deviceId) override
	{
		return (SDL_GetGamepadAxis(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) / (float)(SDL_JOYSTICK_AXIS_MAX);
	}

	float GetRightTrigger(int deviceId) override
	{
		return (SDL_GetGamepadAxis(_controllerMap[deviceId]->_sdlController, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) / (float)(SDL_JOYSTICK_AXIS_MAX);
	}

	float GetGyroX(int deviceId) override
	{
		if (_controllerMap[deviceId]->_has_gyro)
		{
			float rawGyro[3];
			SDL_GetGamepadSensorData(_controllerMap[deviceId]->_sdlController, SDL_SENSOR_GYRO, rawGyro, 3);
		}
		return float();
	}

	float GetGyroY(int deviceId) override
	{
		if (_controllerMap[deviceId]->_has_gyro)
		{
			float rawGyro[3];
			SDL_GetGamepadSensorData(_controllerMap[deviceId]->_sdlController, SDL_SENSOR_GYRO, rawGyro, 3);
		}
		return float();
	}

	float GetGyroZ(int deviceId) override
	{
		if (_controllerMap[deviceId]->_has_gyro)
		{
			float rawGyro[3];
			SDL_GetGamepadSensorData(_controllerMap[deviceId]->_sdlController, SDL_SENSOR_GYRO, rawGyro, 3);
		}
		return float();
	}

	float GetAccelX(int deviceId) override
	{
		return float();
	}

	float GetAccelY(int deviceId) override
	{
		return float();
	}

	float GetAccelZ(int deviceId) override
	{
		return float();
	}

	int GetTouchId(int deviceId, bool secondTouch = false) override
	{
		return int();
	}

	bool GetTouchDown(int deviceId, bool secondTouch)
	{
		bool touchState = 0;
		return SDL_GetGamepadTouchpadFinger(_controllerMap[deviceId]->_sdlController, 0, secondTouch ? 1 : 0, &touchState, nullptr, nullptr, nullptr) ? touchState : false;
	}

	float GetTouchX(int deviceId, bool secondTouch = false) override
	{
		float x = 0;
		if (SDL_GetGamepadTouchpadFinger(_controllerMap[deviceId]->_sdlController, 0, secondTouch ? 1 : 0, nullptr, nullptr, &x, nullptr))
		{
			return x;
		}
		return x;
	}

	float GetTouchY(int deviceId, bool secondTouch = false) override
	{
		float y = 0;
		if (SDL_GetGamepadTouchpadFinger(_controllerMap[deviceId]->_sdlController, 0, secondTouch ? 1 : 0, nullptr, nullptr, &y, nullptr))
		{
			return y;
		}
		return y;
	}

	float GetStickStep(int deviceId) override
	{
		return float();
	}

	float GetTriggerStep(int deviceId) override
	{
		return float();
	}

	float GetPollRate(int deviceId) override
	{
		return float();
	}

	void ResetContinuousCalibration(int deviceId) override
	{
	}

	void StartContinuousCalibration(int deviceId) override
	{
	}

	void PauseContinuousCalibration(int deviceId) override
	{
	}

	void GetCalibrationOffset(int deviceId, float &xOffset, float &yOffset, float &zOffset) override
	{
	}

	void SetCalibrationOffset(int deviceId, float xOffset, float yOffset, float zOffset) override
	{
	}

	void SetCallback(void (*callback)(int, JOY_SHOCK_STATE, JOY_SHOCK_STATE, IMU_STATE, IMU_STATE, float)) override
	{
		lock_guard guard(controller_lock);
		g_callback = callback;
	}

	void SetTouchCallback(void (*callback)(int, TOUCH_STATE, TOUCH_STATE, float)) override
	{
		lock_guard guard(controller_lock);
		g_touch_callback = callback;
	}

	int GetControllerType(int deviceId) override
	{
		return _controllerMap[deviceId]->_ctrlr_type;
	}

	int GetControllerSplitType(int deviceId) override
	{
		return _controllerMap[deviceId]->_split_type;
	}

	int GetControllerColour(int deviceId) override
	{
		return int();
	}

	void SetLightColour(int deviceId, int colour) override
	{
		auto prop = SDL_GetGamepadProperties(_controllerMap[deviceId]->_sdlController);
		
		if (SDL_GetStringProperty(prop, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, nullptr) != nullptr)
		{
			union
			{
				uint32_t raw;
				uint8_t argb[4];
			} uColour;
			uColour.raw = colour;
			SDL_SetGamepadLED(_controllerMap[deviceId]->_sdlController, uColour.argb[2], uColour.argb[1], uColour.argb[0]);
		}
	}

	void SetRumble(int deviceId, int smallRumble, int bigRumble) override
	{
		auto *dev = _controllerMap[deviceId];
		uint16_t newSmall = clamp(smallRumble, 0, int(UINT16_MAX));
		uint16_t newBig   = clamp(bigRumble,   0, int(UINT16_MAX));
		if (dev->_small_rumble != newSmall || dev->_big_rumble != newBig)
		{
			dev->_small_rumble = newSmall;
			dev->_big_rumble   = newBig;
			dev->_rumble_dirty = true;
			dev->_effectDirty  = true; // DS5 effect packet also embeds rumble
		}
	}

	void SetPlayerNumber(int deviceId, int number) override
	{
		SDL_SetGamepadPlayerIndex(_controllerMap[deviceId]->_sdlController, number);
	}

	void SetTriggerEffect(int deviceId, const AdaptiveTriggerSetting &_leftTriggerEffect, const AdaptiveTriggerSetting &_rightTriggerEffect) override
	{
		if (_leftTriggerEffect != _controllerMap[deviceId]->_leftTriggerEffect || _rightTriggerEffect != _controllerMap[deviceId]->_rightTriggerEffect)
		{
			// Update active trigger effect and mark dirty so SendEffect() rebuilds the packet.
			_controllerMap[deviceId]->_leftTriggerEffect = _leftTriggerEffect;
			_controllerMap[deviceId]->_rightTriggerEffect = _rightTriggerEffect;
			_controllerMap[deviceId]->_effectDirty = true;
		}
		_controllerMap[deviceId]->SendEffect();
	}

	virtual void SetMicLight(int deviceId, uint8_t mode) override
	{
		if (mode != _controllerMap[deviceId]->_micLight)
		{
			_controllerMap[deviceId]->_micLight = mode;
			_controllerMap[deviceId]->_effectDirty = true;
			_controllerMap[deviceId]->SendEffect();
		}
	}
};

JslWrapper *JslWrapper::getNew()
{
	return new SdlInstance();
}
