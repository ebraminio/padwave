#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <hidapi.h>
#include <Xinput.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "dualsense.hh"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

// XINPUT_GAMEPAD_GUIDE is not defined in the official Xinput.h
#define XINPUT_GAMEPAD_GUIDE 0x0400

// ---------------------------------------------------------------------------
// Additional types not in the official Xinput.h
// ---------------------------------------------------------------------------

typedef struct _XINPUT_BASE_BUS_INFORMATION {
  WORD vendorId;
  WORD productId;
  WORD productVersion;
  WORD field_6;
  DWORD field_8;
  BYTE field_C;
  BYTE field_D;
  BYTE field_E;
  BYTE field_F;
} XINPUT_BASE_BUS_INFORMATION;

typedef struct _XINPUT_SYSTEM_BUTTONS {
  DWORD StandardSystemButtons;
  DWORD ExtraSystemButtons;
  DWORD MoreSystemButtons1;
  DWORD MoreSystemButtons2;
  DWORD MoreSystemButtons3;
  DWORD MoreSystemButtons4;
  DWORD MoreSystemButtons5;
  DWORD MoreSystemButtons6;
} XINPUT_SYSTEM_BUTTONS;

#if defined(__WINE_XINPUT_H)
using DsCapabilitiesEx = XINPUT_CAPABILITIES_EX;
#define DS2XI_CAPS_VERSION VersionNumber
#else
struct DsCapabilitiesEx {
  XINPUT_CAPABILITIES Capabilities;
  WORD VendorId;
  WORD ProductId;
  WORD ProductVersion;
  WORD unk1;
  DWORD unk2;
};
#define DS2XI_CAPS_VERSION ProductVersion
#endif

#ifndef DS2XI_SYSTEM_XINPUT
#error "DS2XI_SYSTEM_XINPUT must name the matching system XInput DLL"
#endif
/* Each variant selects the APIs it exports. The matching .def file is the
   final filter; this keeps unrelated wrappers out of the translation unit too. */
#if !defined(DS2XI_EXPORTS_XINPUT1_1) && !defined(DS2XI_EXPORTS_XINPUT1_2) && \
    !defined(DS2XI_EXPORTS_XINPUT1_3) && !defined(DS2XI_EXPORTS_XINPUT1_4) && \
    !defined(DS2XI_EXPORTS_XINPUT9_1_0) && !defined(DS2XI_EXPORTS_XINPUTUAP)
#error "Select the XInput export set"
#endif

#if defined(DS2XI_EXPORTS_XINPUT1_1) || defined(DS2XI_EXPORTS_XINPUT1_2) || \
    defined(DS2XI_EXPORTS_XINPUT1_3) || defined(DS2XI_EXPORTS_XINPUT1_4) || \
    defined(DS2XI_EXPORTS_XINPUTUAP)
#define DS2XI_EXPORTS_ENABLE
#endif
#if defined(DS2XI_EXPORTS_XINPUT1_1) || defined(DS2XI_EXPORTS_XINPUT1_2) || \
    defined(DS2XI_EXPORTS_XINPUT1_3) || defined(DS2XI_EXPORTS_XINPUT9_1_0)
#define DS2XI_EXPORTS_DSOUND
#endif
#if defined(DS2XI_EXPORTS_XINPUT1_3) || defined(DS2XI_EXPORTS_XINPUT1_4) || \
    defined(DS2XI_EXPORTS_XINPUTUAP)
#define DS2XI_EXPORTS_BATTERY
#define DS2XI_EXPORTS_KEYSTROKE
#endif
#if defined(DS2XI_EXPORTS_XINPUT1_4) || defined(DS2XI_EXPORTS_XINPUTUAP)
#define DS2XI_EXPORTS_AUDIO
#endif
#if defined(DS2XI_EXPORTS_XINPUT1_3) || defined(DS2XI_EXPORTS_XINPUT1_4)
#define DS2XI_EXPORTS_GUIDE
#endif
#if defined(DS2XI_EXPORTS_XINPUT1_4)
#define DS2XI_EXPORTS_BASE_BUS
#define DS2XI_EXPORTS_CAPS_EX
#define DS2XI_EXPORTS_SYSTEM_BUTTONS
#endif


// ---------------------------------------------------------------------------
// System XInput function pointers
// ---------------------------------------------------------------------------

typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *PFN_XInputSetState)(DWORD, XINPUT_VIBRATION *);
typedef DWORD(WINAPI *PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef void(WINAPI *PFN_XInputEnable)(BOOL);
typedef DWORD(WINAPI *PFN_XInputGetAudioDeviceIds)(DWORD, LPWSTR, UINT *, LPWSTR, UINT *);
typedef DWORD(WINAPI *PFN_XInputGetBatteryInformation)(DWORD, BYTE,
                                                       XINPUT_BATTERY_INFORMATION *);
typedef DWORD(WINAPI *PFN_XInputGetKeystroke)(DWORD, DWORD, PXINPUT_KEYSTROKE);
typedef DWORD(WINAPI *PFN_XInputGetDSoundAudioDeviceGuids)(DWORD, GUID *, GUID *);
typedef DWORD(WINAPI *PFN_XInputGetStateEx)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *PFN_XInputWaitForGuideButton)(DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI *PFN_XInputCancelGuideButtonWait)(DWORD);
typedef DWORD(WINAPI *PFN_XInputPowerOffController)(DWORD);
typedef DWORD(WINAPI *PFN_XInputGetBaseBusInformation)(DWORD, XINPUT_BASE_BUS_INFORMATION *);
typedef DWORD(WINAPI *PFN_XInputGetCapabilitiesEx)(DWORD, DWORD, DWORD, DsCapabilitiesEx *);
typedef DWORD(WINAPI *PFN_XInputGetSystemButtons)(DWORD, XINPUT_SYSTEM_BUTTONS *, PVOID);

static HMODULE g_SystemXInput                                     = nullptr;
[[maybe_unused]] static PFN_XInputGetState g_FpnGetState                           = nullptr;
[[maybe_unused]] static PFN_XInputSetState g_FpnSetState                           = nullptr;
[[maybe_unused]] static PFN_XInputGetCapabilities g_FpnGetCaps                     = nullptr;
[[maybe_unused]] static PFN_XInputEnable g_FpnEnable                               = nullptr;
[[maybe_unused]] static PFN_XInputGetAudioDeviceIds g_FpnGetAudioDeviceIds         = nullptr;
[[maybe_unused]] static PFN_XInputGetBatteryInformation g_FpnGetBattery            = nullptr;
[[maybe_unused]] static PFN_XInputGetKeystroke g_FpnGetKeystroke                   = nullptr;
[[maybe_unused]] static PFN_XInputGetDSoundAudioDeviceGuids g_FpnGetDSoundAudioDeviceGuids = nullptr;
[[maybe_unused]] static PFN_XInputGetStateEx g_FpnGetStateEx                       = nullptr;
[[maybe_unused]] static PFN_XInputWaitForGuideButton g_FpnWaitForGuideButton       = nullptr;
[[maybe_unused]] static PFN_XInputCancelGuideButtonWait g_FpnCancelGuideButtonWait = nullptr;
[[maybe_unused]] static PFN_XInputPowerOffController g_FpnPowerOffController       = nullptr;
[[maybe_unused]] static PFN_XInputGetBaseBusInformation g_FpnGetBaseBusInformation = nullptr;
[[maybe_unused]] static PFN_XInputGetCapabilitiesEx g_FpnGetCapabilitiesEx         = nullptr;
[[maybe_unused]] static PFN_XInputGetSystemButtons g_FpnGetSystemButtons           = nullptr;

// ---------------------------------------------------------------------------
// DualSense state
// ---------------------------------------------------------------------------

static constexpr DWORD kMaxControllers = 4;

struct DsControllerState {
  hid_device *device = nullptr;
  bool connected     = false;
  bool isBluetooth   = false;
  WORD vendorId      = 0;
  WORD productId     = 0;
  WORD releaseNumber = 0;
  DWORD packetNumber = 0;
  uint8_t smallMotor = 0;
  uint8_t largeMotor = 0;
  uint8_t batteryLevel = 0;
  dualsense::OutputReport::Sequence sequence;
  char path[1024] = {};
};

static DsControllerState g_Controllers[kMaxControllers];
[[maybe_unused]] static volatile LONG g_CancelGuideButtonWait[kMaxControllers] = {};

struct GuideWaitContext {
  DWORD user;
  DWORD *result;
};
[[maybe_unused]] static GuideWaitContext g_GuideWaitContexts[kMaxControllers];

// LED / color state
static uint8_t g_LedNumber = 0;
static DWORD g_Color       = 0xFF0000; // Default blue

// DualSense constants
static constexpr int SONY_VID    = 0x054c;
static constexpr int DS_PID      = 0x0ce6;
static constexpr int DS_EDGE_PID = 0x0df2;

static void zeroMemory(void *ptr, size_t size) {
  volatile unsigned char *p = static_cast<volatile unsigned char *>(ptr);
  while (size--) *p++ = 0;
}

// ---------------------------------------------------------------------------
// Load system XInput
// ---------------------------------------------------------------------------

static bool loadSystemXInput() {
  if (g_SystemXInput) return true;

  char sysDir[MAX_PATH];
  zeroMemory(sysDir, sizeof(sysDir));
  if (GetSystemDirectoryA(sysDir, MAX_PATH) == 0) return false;

  char fullPath[MAX_PATH];
  zeroMemory(fullPath, sizeof(fullPath));
  if (!PathCombineA(fullPath, sysDir, DS2XI_SYSTEM_XINPUT)) return false;

  g_SystemXInput = LoadLibraryA(fullPath);
  if (!g_SystemXInput) return false;

  g_FpnGetState =
      reinterpret_cast<PFN_XInputGetState>(GetProcAddress(g_SystemXInput, "XInputGetState"));
  g_FpnSetState =
      reinterpret_cast<PFN_XInputSetState>(GetProcAddress(g_SystemXInput, "XInputSetState"));
  g_FpnGetCaps = reinterpret_cast<PFN_XInputGetCapabilities>(
      GetProcAddress(g_SystemXInput, "XInputGetCapabilities"));
  g_FpnEnable = reinterpret_cast<PFN_XInputEnable>(GetProcAddress(g_SystemXInput, "XInputEnable"));
  g_FpnGetAudioDeviceIds = reinterpret_cast<PFN_XInputGetAudioDeviceIds>(
      GetProcAddress(g_SystemXInput, "XInputGetAudioDeviceIds"));
  g_FpnGetBattery = reinterpret_cast<PFN_XInputGetBatteryInformation>(
      GetProcAddress(g_SystemXInput, "XInputGetBatteryInformation"));
  g_FpnGetKeystroke = reinterpret_cast<PFN_XInputGetKeystroke>(
      GetProcAddress(g_SystemXInput, "XInputGetKeystroke"));
  g_FpnGetDSoundAudioDeviceGuids = reinterpret_cast<PFN_XInputGetDSoundAudioDeviceGuids>(
      GetProcAddress(g_SystemXInput, "XInputGetDSoundAudioDeviceGuids"));
  g_FpnGetStateEx = reinterpret_cast<PFN_XInputGetStateEx>(
      GetProcAddress(g_SystemXInput, reinterpret_cast<LPCSTR>(100)));
  g_FpnWaitForGuideButton = reinterpret_cast<PFN_XInputWaitForGuideButton>(
      GetProcAddress(g_SystemXInput, reinterpret_cast<LPCSTR>(101)));
  g_FpnCancelGuideButtonWait = reinterpret_cast<PFN_XInputCancelGuideButtonWait>(
      GetProcAddress(g_SystemXInput, reinterpret_cast<LPCSTR>(102)));
  g_FpnPowerOffController = reinterpret_cast<PFN_XInputPowerOffController>(
      GetProcAddress(g_SystemXInput, reinterpret_cast<LPCSTR>(103)));
  g_FpnGetBaseBusInformation = reinterpret_cast<PFN_XInputGetBaseBusInformation>(
      GetProcAddress(g_SystemXInput, reinterpret_cast<LPCSTR>(104)));
  g_FpnGetCapabilitiesEx = reinterpret_cast<PFN_XInputGetCapabilitiesEx>(
      GetProcAddress(g_SystemXInput, reinterpret_cast<LPCSTR>(108)));
  g_FpnGetSystemButtons = reinterpret_cast<PFN_XInputGetSystemButtons>(
      GetProcAddress(g_SystemXInput, reinterpret_cast<LPCSTR>(109)));

  return true;
}

// ---------------------------------------------------------------------------
// DualSense output report (rumble + LED)
// ---------------------------------------------------------------------------

static uint8_t playerLedMask(const DsControllerState &controller) {
  if (g_LedNumber != 0) return g_LedNumber;

  if (controller.batteryLevel == 0) return 0;
  if (controller.batteryLevel >= 100) return 0x1F;
  return static_cast<uint8_t>((1u << ((controller.batteryLevel * 5) / 100)) - 1);
}

template <typename Fill>
static bool sendOutput(DsControllerState &controller, Fill fill) {
  if (!controller.device) return false;

  if (controller.isBluetooth) {
    dualsense::OutputReport report(controller.sequence);
    fill(report.getBody());
    report.sign();
    return hid_write(controller.device, reinterpret_cast<const unsigned char *>(&report),
                     sizeof(report)) > 0;
  } else {
    dualsense::UsbOutputReport report{};
    fill(report.getBody());
    return hid_write(controller.device, reinterpret_cast<const unsigned char *>(&report),
                     sizeof(report)) > 0;
  }
}

static void sendDualSenseOutput(DsControllerState &controller) {
  sendOutput(controller, [&controller](dualsense::OutputBody &body) {
    const uint8_t leds = playerLedMask(controller);
    body.setCompatibleVibration(controller.largeMotor, controller.smallMotor);
    body.setPlayerLedBrightness(dualsense::OutputBody::PlayerLedBrightness::Low);
    body.setPlayerLeds((leds & 0x01) != 0, (leds & 0x02) != 0, (leds & 0x04) != 0,
                       (leds & 0x08) != 0, (leds & 0x10) != 0, false);
    body.setLightbar(static_cast<uint8_t>(g_Color & 0xFF),
                     static_cast<uint8_t>((g_Color >> 8) & 0xFF),
                     static_cast<uint8_t>((g_Color >> 16) & 0xFF));
  });
}

static void cleanControllerState(DsControllerState &controller) {
  sendOutput(controller, [](dualsense::OutputBody &body) {
    body.setCompatibleVibration(0, 0);
    body.setPlayerLeds(false, false, false, false, false, false);
    body.setLightbar(0, 0, 0);
  });
}

// ---------------------------------------------------------------------------
// DualSense enumeration
// ---------------------------------------------------------------------------

static bool isDualSenseDevice(hid_device_info *info) {
  return info->vendor_id == SONY_VID &&
         (info->product_id == DS_PID || info->product_id == DS_EDGE_PID);
}

static bool samePath(const DsControllerState &controller, const char *path) {
  if (!controller.connected || !path) return false;
  const char *a = controller.path;
  const char *b = path;
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

static void copyPath(char *destination, size_t capacity, const char *source) {
  if (!capacity) return;
  size_t i = 0;
  for (; i + 1 < capacity && source[i]; ++i) destination[i] = source[i];
  destination[i] = '\0';
}

static void ensureControllers() {
  bool haveEmpty = false;
  for (const auto &controller : g_Controllers)
    if (!controller.connected) haveEmpty = true;
  if (!haveEmpty) return;

  hid_device_info *devices = hid_enumerate(SONY_VID, 0);
  if (!devices) return;

  for (hid_device_info *cur = devices; cur; cur = cur->next) {
    if (!isDualSenseDevice(cur)) continue;
    if (cur->usage_page != 1 || cur->usage != 5) continue;

    bool alreadyOpen = false;
    for (const auto &controller : g_Controllers)
      if (samePath(controller, cur->path)) alreadyOpen = true;
    if (alreadyOpen) continue;

    DsControllerState *slot = nullptr;
    for (auto &controller : g_Controllers) {
      if (!controller.connected) {
        slot = &controller;
        break;
      }
    }
    if (!slot) break;

    hid_device *dev = hid_open_path(cur->path);
    if (!dev) continue;

    slot->device      = dev;
    slot->connected   = true;
    slot->isBluetooth = cur->bus_type == HID_API_BUS_BLUETOOTH;
    slot->vendorId    = cur->vendor_id;
    slot->productId   = cur->product_id;
    slot->releaseNumber = cur->release_number;
    slot->packetNumber  = 0;
    slot->smallMotor    = 0;
    slot->largeMotor    = 0;
    slot->batteryLevel  = 0;
    copyPath(slot->path, sizeof(slot->path), cur->path);

    hid_set_nonblocking(dev, true);
    sendDualSenseOutput(*slot);
  }

  hid_free_enumeration(devices);
}

// ---------------------------------------------------------------------------
// DualSense input -> XInput mapping (called directly from XInputGetState)
// ---------------------------------------------------------------------------

static bool readDualSenseState(DWORD userIndex, XINPUT_STATE &state) {
  if (userIndex >= kMaxControllers) return false;
  DsControllerState &controller = g_Controllers[userIndex];
  if (!controller.device) return false;

  uint8_t buffer[512];
  int n = hid_read(controller.device, buffer, sizeof(buffer));
  if (n <= 0) return false;
  size_t bytesRead = static_cast<size_t>(n);

  const dualsense::InputBody *input = nullptr;
  if (auto *bluetoothReport = dualsense::InputReport::asFrame(buffer, bytesRead)) {
    input = &bluetoothReport->getBody();
  } else if (auto *usbReport = dualsense::UsbInputReport::asFrame(buffer, bytesRead)) {
    input = &usbReport->getBody();
  } else
    return false;

  const unsigned battery = input->getBatteryPercentage();
  if (battery != controller.batteryLevel) {
    controller.batteryLevel = static_cast<uint8_t>(battery);
    sendDualSenseOutput(controller);
  }

  zeroMemory(&state, sizeof(state));
  state.dwPacketNumber = ++controller.packetNumber;

  state.Gamepad.bLeftTrigger  = static_cast<BYTE>(input->getLeftTrigger());
  state.Gamepad.bRightTrigger = static_cast<BYTE>(input->getRightTrigger());
  const auto leftStick        = input->getLeftStick();
  const auto rightStick       = input->getRightStick();
  state.Gamepad.sThumbLX      = static_cast<SHORT>((leftStick.x * 257) - 32768);
  state.Gamepad.sThumbLY      = static_cast<SHORT>(32767 - (leftStick.y * 257));
  state.Gamepad.sThumbRX      = static_cast<SHORT>((rightStick.x * 257) - 32768);
  state.Gamepad.sThumbRY      = static_cast<SHORT>(32767 - (rightStick.y * 257));

  using Button       = dualsense::InputBody::Button;
  const auto buttons = input->getButtons();
  if (buttons.isPressed(Button::ButtonSquare)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_X;
  if (buttons.isPressed(Button::ButtonCross)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_A;
  if (buttons.isPressed(Button::ButtonCircle)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_B;
  if (buttons.isPressed(Button::ButtonTriangle)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
  if (buttons.isPressed(Button::L1)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
  if (buttons.isPressed(Button::R1)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
  if (buttons.isPressed(Button::Create)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
  if (buttons.isPressed(Button::Options)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_START;
  if (buttons.isPressed(Button::L3)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
  if (buttons.isPressed(Button::R3)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
  if (buttons.isPressed(Button::PsHome)) state.Gamepad.wButtons |= XINPUT_GAMEPAD_GUIDE;

  using Dpad = dualsense::InputBody::Dpad;
  switch (input->getDpad()) {
  case Dpad::Up: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP; break;
  case Dpad::UpRight:
    state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT;
    break;
  case Dpad::Right: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
  case Dpad::DownRight:
    state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT;
    break;
  case Dpad::Down: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
  case Dpad::DownLeft:
    state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT;
    break;
  case Dpad::Left: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
  case Dpad::UpLeft:
    state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT;
    break;
  case Dpad::None: break;
  }

  return true;
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------

// DllMain is @1 in every variant.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  (void)hModule;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    loadSystemXInput();
    hid_init();
    break;

  case DLL_PROCESS_DETACH:
    for (auto &controller : g_Controllers) {
      if (!controller.device) continue;
      cleanControllerState(controller);
      hid_close(controller.device);
      controller.device = nullptr;
      controller.connected = false;
      controller.isBluetooth = false;
      controller.vendorId = 0;
      controller.productId = 0;
      controller.releaseNumber = 0;
      controller.packetNumber = 0;
      controller.smallMotor = 0;
      controller.largeMotor = 0;
      controller.batteryLevel = 0;
      zeroMemory(&controller.sequence, sizeof(controller.sequence));
      controller.path[0] = '\0';
    }
    hid_exit();
    if (g_SystemXInput) {
      FreeLibrary(g_SystemXInput);
      g_SystemXInput = nullptr;
    }
    break;
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// Exported XInput functions
//
// Note: We do NOT wrap these in extern "C" because <Xinput.h> already
// declares them with C linkage. We also avoid __declspec(dllexport)
// and use a .def file instead to prevent linkage conflicts.
// ---------------------------------------------------------------------------

// 1.1/1.2 @5, 1.3/1.4 @2, 9.1.0 @4, UAP @7
DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState) noexcept {
  if (dwUserIndex < kMaxControllers) {
    if (!pState) return ERROR_INVALID_PARAMETER;

    DsControllerState &controller = g_Controllers[dwUserIndex];
    if (!controller.connected) ensureControllers();

    if (controller.connected && readDualSenseState(dwUserIndex, *pState)) return ERROR_SUCCESS;
  }

  if (g_FpnGetState) return g_FpnGetState(dwUserIndex, pState);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// 1.1/1.2 @6, 1.3/1.4 @3, 9.1.0 @5, UAP @8
DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration) noexcept {
  if (dwUserIndex < kMaxControllers) {
    if (!pVibration) return ERROR_INVALID_PARAMETER;

    DsControllerState &controller = g_Controllers[dwUserIndex];
    if (!controller.connected) ensureControllers();

    if (controller.connected) {
      uint32_t sm = pVibration->wRightMotorSpeed / 257;
      controller.smallMotor =
          pVibration->wRightMotorSpeed ? static_cast<uint8_t>(sm < 64 ? 64 : sm) : 0;
      uint32_t lm = pVibration->wLeftMotorSpeed / 257;
      controller.largeMotor =
          pVibration->wLeftMotorSpeed ? static_cast<uint8_t>(lm < 40 ? 40 : lm) : 0;

      g_LedNumber = 0;
      sendDualSenseOutput(controller);
      return ERROR_SUCCESS;
    }
  }

  if (g_FpnSetState) return g_FpnSetState(dwUserIndex, pVibration);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// 1.1/1.2 @3, 1.3/1.4 @4, 9.1.0 @2, UAP @5
DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
                                   XINPUT_CAPABILITIES *pCapabilities) noexcept {
  if (dwUserIndex < kMaxControllers) {
    if (!pCapabilities) return ERROR_INVALID_PARAMETER;
    if (dwFlags != 0 && dwFlags != XINPUT_FLAG_GAMEPAD) return ERROR_BAD_ARGUMENTS;

    DsControllerState &controller = g_Controllers[dwUserIndex];
    if (!controller.connected) ensureControllers();

    if (controller.connected) {
      zeroMemory(pCapabilities, sizeof(XINPUT_CAPABILITIES));
      pCapabilities->Type    = XINPUT_DEVTYPE_GAMEPAD;
      pCapabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
      pCapabilities->Flags   = XINPUT_CAPS_FFB_SUPPORTED;
      pCapabilities->Gamepad.wButtons =
          (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
           XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
           XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_LEFT_SHOULDER |
           XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X |
           XINPUT_GAMEPAD_Y);
      pCapabilities->Gamepad.bLeftTrigger       = 0xFF;
      pCapabilities->Gamepad.bRightTrigger      = 0xFF;
      pCapabilities->Gamepad.sThumbLX           = -0x40;
      pCapabilities->Gamepad.sThumbLY           = -0x40;
      pCapabilities->Gamepad.sThumbRX           = -0x40;
      pCapabilities->Gamepad.sThumbRY           = -0x40;
      pCapabilities->Vibration.wLeftMotorSpeed  = 0xFFFF;
      pCapabilities->Vibration.wRightMotorSpeed = 0xFFFF;
      return ERROR_SUCCESS;
    }
  }

  if (g_FpnGetCaps) return g_FpnGetCaps(dwUserIndex, dwFlags, pCapabilities);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// 1.1/1.2 @2, 1.3/1.4 @5, UAP @2; absent in 9.1.0
#ifdef DS2XI_EXPORTS_ENABLE
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4995)
#endif
void WINAPI XInputEnable(BOOL enable) noexcept {
  if (g_FpnEnable) g_FpnEnable(enable);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

// 1.4 @10, UAP @3
#ifdef DS2XI_EXPORTS_AUDIO
extern "C" DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId, UINT *pRenderCount,
                                     LPWSTR pCaptureDeviceId, UINT *pCaptureCount) noexcept {
  if (g_FpnGetAudioDeviceIds)
    return g_FpnGetAudioDeviceIds(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId,
                                  pCaptureCount);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif

// 1.3/1.4 @7, UAP @4
#ifdef DS2XI_EXPORTS_BATTERY
DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
                                         XINPUT_BATTERY_INFORMATION *pBatteryInformation) noexcept {
  if (dwUserIndex < kMaxControllers) {
    DsControllerState &controller = g_Controllers[dwUserIndex];
    if (!controller.connected) ensureControllers();

    if (controller.connected && pBatteryInformation) {
      pBatteryInformation->BatteryType =
          BATTERY_TYPE_NIMH; // It's actually litium polymer but this is best matching
      if (controller.batteryLevel <= 10)
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_EMPTY;
      else if (controller.batteryLevel <= 35)
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_LOW;
      else if (controller.batteryLevel <= 75)
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_MEDIUM;
      else
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_FULL;
      return ERROR_SUCCESS;
    }
  }

  if (g_FpnGetBattery) return g_FpnGetBattery(dwUserIndex, devType, pBatteryInformation);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif

// 1.3/1.4 @8, UAP @6
#ifdef DS2XI_EXPORTS_KEYSTROKE
DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
                                PXINPUT_KEYSTROKE pKeystroke) noexcept {
  if (g_FpnGetKeystroke) return g_FpnGetKeystroke(dwUserIndex, dwReserved, pKeystroke);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif

// 1.1/1.2 @4, 1.3 @6, 9.1.0 @3; absent in 1.4/UAP.
#ifdef DS2XI_EXPORTS_DSOUND
extern "C" DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID *pDSoundRenderGuid,
                                                        GUID *pDSoundCaptureGuid) noexcept {
  if (g_FpnGetDSoundAudioDeviceGuids)
    return g_FpnGetDSoundAudioDeviceGuids(dwUserIndex, pDSoundRenderGuid, pDSoundCaptureGuid);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif

// Undocumented: 1.3 and 1.4 @100
#ifdef DS2XI_EXPORTS_GUIDE
DWORD WINAPI XInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE *pState) noexcept {
  return XInputGetState(dwUserIndex, pState);
}
#endif

// Undocumented: 1.3 and 1.4 @101
#ifdef DS2XI_EXPORTS_GUIDE
static DWORD WINAPI guideWaitThreadProc(LPVOID parameter) {
  GuideWaitContext *context = static_cast<GuideWaitContext *>(parameter);
  for (;;) {
    if (InterlockedExchange(&g_CancelGuideButtonWait[context->user], FALSE) != FALSE) {
      *context->result = ERROR_CANCELLED;
      return 0;
    }
    XINPUT_STATE state{};
    if (XInputGetState(context->user, &state) == ERROR_SUCCESS &&
        (state.Gamepad.wButtons & XINPUT_GAMEPAD_GUIDE)) {
      *context->result = ERROR_SUCCESS;
      return 0;
    }
    Sleep(10);
  }
}

extern "C" DWORD WINAPI XInputWaitForGuideButton(DWORD dwUserIndex, DWORD dwFlag, LPVOID pVoid) noexcept {
  if (dwUserIndex >= kMaxControllers || !pVoid) {
    if (g_FpnWaitForGuideButton) return g_FpnWaitForGuideButton(dwUserIndex, dwFlag, pVoid);
    return ERROR_BAD_ARGUMENTS;
  }

  DsControllerState &controller = g_Controllers[dwUserIndex];
  if (!controller.connected) ensureControllers();
  if (!controller.connected) {
    if (g_FpnWaitForGuideButton) return g_FpnWaitForGuideButton(dwUserIndex, dwFlag, pVoid);
    return ERROR_DEVICE_NOT_CONNECTED;
  }

  DWORD *result = static_cast<DWORD *>(pVoid);
  InterlockedExchange(&g_CancelGuideButtonWait[dwUserIndex], FALSE);

  if (dwFlag != 0 && dwFlag != static_cast<DWORD>(-1)) {
    *result = ERROR_IO_PENDING;
    GuideWaitContext &context = g_GuideWaitContexts[dwUserIndex];
    context.user   = dwUserIndex;
    context.result = result;
    HANDLE thread = CreateThread(nullptr, 0, guideWaitThreadProc, &context, 0, nullptr);
    if (!thread) {
      *result = GetLastError();
      return *result;
    }
    CloseHandle(thread);
    return ERROR_SUCCESS;
  }

  /* dwFlag 0 (and the system's -1 alias) blocks for one Guide press. */
  for (;;) {
    if (InterlockedExchange(&g_CancelGuideButtonWait[dwUserIndex], FALSE) != FALSE) {
      *result = ERROR_CANCELLED;
      return *result;
    }
    XINPUT_STATE state{};
    if (XInputGetState(dwUserIndex, &state) == ERROR_SUCCESS &&
        (state.Gamepad.wButtons & XINPUT_GAMEPAD_GUIDE)) {
      *result = ERROR_SUCCESS;
      return *result;
    }
    Sleep(10);
  }
}
#endif

// Undocumented: 1.3 and 1.4 @102
#ifdef DS2XI_EXPORTS_GUIDE
extern "C" DWORD WINAPI XInputCancelGuideButtonWait(DWORD dwUserIndex) noexcept {
  if (dwUserIndex < kMaxControllers && g_Controllers[dwUserIndex].connected) {
    InterlockedExchange(&g_CancelGuideButtonWait[dwUserIndex], TRUE);
    return ERROR_SUCCESS;
  }
  if (g_FpnCancelGuideButtonWait) return g_FpnCancelGuideButtonWait(dwUserIndex);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif
// Undocumented: 1.3 and 1.4 @103
#ifdef DS2XI_EXPORTS_GUIDE
extern "C" DWORD WINAPI XInputPowerOffController(DWORD dwUserIndex) noexcept {
  /* Windows exposes no reliable HID/Bluetooth power-off for DualSense.
     Use the system implementation for actual Xbox controllers. */
  if (g_FpnPowerOffController) return g_FpnPowerOffController(dwUserIndex);
  return ERROR_CALL_NOT_IMPLEMENTED;
}
#endif

// Undocumented: 1.4 @104
#ifdef DS2XI_EXPORTS_BASE_BUS
extern "C" DWORD WINAPI XInputGetBaseBusInformation(
    DWORD dwBusIndex, XINPUT_BASE_BUS_INFORMATION *pBaseBusInformation) noexcept {
  if (dwBusIndex < kMaxControllers && pBaseBusInformation) {
    DsControllerState &controller = g_Controllers[dwBusIndex];
    if (!controller.connected) ensureControllers();
    if (controller.connected) {
      zeroMemory(pBaseBusInformation, sizeof(*pBaseBusInformation));
      pBaseBusInformation->vendorId = controller.vendorId;
      pBaseBusInformation->productId = controller.productId;
      pBaseBusInformation->productVersion = controller.releaseNumber;
      return ERROR_SUCCESS;
    }
  }
  if (g_FpnGetBaseBusInformation)
    return g_FpnGetBaseBusInformation(dwBusIndex, pBaseBusInformation);
  return ERROR_BAD_ARGUMENTS;
}
#endif

// Undocumented: 1.4 @108
// The first argument is an unknown/reserved value; Wine and MinGW both use
// this four-argument signature.
#ifdef DS2XI_EXPORTS_CAPS_EX
extern "C" DWORD WINAPI XInputGetCapabilitiesEx(DWORD unknown, DWORD dwUserIndex, DWORD dwFlags,
                                     DsCapabilitiesEx *pCapabilitiesEx) noexcept {
  if (unknown != 1 || dwUserIndex >= kMaxControllers ||
      (dwFlags != 0 && dwFlags != XINPUT_CAPS_FFB_SUPPORTED) || !pCapabilitiesEx)
    return ERROR_INVALID_PARAMETER;

  DsControllerState &controller = g_Controllers[dwUserIndex];
  if (!controller.connected) ensureControllers();
  if (controller.connected) {
    XINPUT_CAPABILITIES capabilities{};
    if (XInputGetCapabilities(dwUserIndex, 0, &capabilities) == ERROR_SUCCESS) {
      zeroMemory(pCapabilitiesEx, sizeof(*pCapabilitiesEx));
      pCapabilitiesEx->Capabilities = capabilities;
      pCapabilitiesEx->VendorId = controller.vendorId;
      pCapabilitiesEx->ProductId = controller.productId;
      pCapabilitiesEx->DS2XI_CAPS_VERSION = controller.releaseNumber;
      return ERROR_SUCCESS;
    }
  }

  if (g_FpnGetCapabilitiesEx)
    return g_FpnGetCapabilitiesEx(unknown, dwUserIndex, dwFlags, pCapabilitiesEx);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif

// Undocumented: 1.4 @109
#ifdef DS2XI_EXPORTS_SYSTEM_BUTTONS
extern "C" DWORD WINAPI XInputGetSystemButtons(DWORD dwUserIndex,
                                               XINPUT_SYSTEM_BUTTONS *pSystemButtons,
                                               PVOID reserved) noexcept {
  if (dwUserIndex < kMaxControllers && pSystemButtons) {
    DsControllerState &controller = g_Controllers[dwUserIndex];
    if (!controller.connected) ensureControllers();
    if (controller.connected) {
      zeroMemory(pSystemButtons, sizeof(*pSystemButtons));
      XINPUT_STATE state{};
      if (XInputGetState(dwUserIndex, &state) == ERROR_SUCCESS &&
          (state.Gamepad.wButtons & XINPUT_GAMEPAD_GUIDE)) {
        pSystemButtons->StandardSystemButtons = XINPUT_GAMEPAD_GUIDE;
      }
      (void)reserved;
      return ERROR_SUCCESS;
    }
  }
  if (g_FpnGetSystemButtons)
    return g_FpnGetSystemButtons(dwUserIndex, pSystemButtons, reserved);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif
