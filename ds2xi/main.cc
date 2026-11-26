#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <hidapi.h>
#include <Xinput.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "dualsense.hh"

// XINPUT_GAMEPAD_GUIDE is not defined in the official Xinput.h
#define XINPUT_GAMEPAD_GUIDE 0x0400

// ---------------------------------------------------------------------------
// Additional types not in the official Xinput.h
// ---------------------------------------------------------------------------

typedef struct _XINPUT_BASE_BUS_INFORMATION {
  DWORD dwBusType;
  DWORD dwVendorId;
  DWORD dwProductId;
  DWORD dwVersionNumber;
  DWORD dwSerialNumber;
} XINPUT_BASE_BUS_INFORMATION;

#ifndef __WINE_XINPUT_H
typedef struct _XINPUT_CAPABILITIES_EX {
  XINPUT_CAPABILITIES Capabilities;
  WORD VendorId;
  WORD ProductId;
  WORD VersionNumber;
  WORD unk1;
  DWORD unk2;
} XINPUT_CAPABILITIES_EX;
#endif

// ---------------------------------------------------------------------------
// System XInput function pointers
// ---------------------------------------------------------------------------

typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE *) noexcept;
typedef DWORD(WINAPI *PFN_XInputSetState)(DWORD, XINPUT_VIBRATION *) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *) noexcept;
typedef void(WINAPI *PFN_XInputEnable)(BOOL) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetAudioDeviceIds)(DWORD, LPWSTR, UINT *, LPWSTR, UINT *) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetBatteryInformation)(DWORD, BYTE,
                                                       XINPUT_BATTERY_INFORMATION *) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetKeystroke)(DWORD, DWORD, PXINPUT_KEYSTROKE) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetStateEx)(DWORD, XINPUT_STATE *) noexcept;
typedef DWORD(WINAPI *PFN_XInputWaitForGuideButton)(DWORD, DWORD, LPVOID) noexcept;
typedef DWORD(WINAPI *PFN_XInputCancelGuideButtonWait)(DWORD) noexcept;
typedef DWORD(WINAPI *PFN_XInputPowerOffController)(DWORD) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetBaseBusInformation)(DWORD, DWORD,
                                                       XINPUT_BASE_BUS_INFORMATION *) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetCapabilitiesEx)(DWORD, DWORD, XINPUT_CAPABILITIES_EX *) noexcept;
typedef DWORD(WINAPI *PFN_XInputGetSystemButtons)(DWORD, XINPUT_KEYSTROKE *) noexcept;

static HMODULE g_SystemXInput                                     = nullptr;
static PFN_XInputGetState g_FpnGetState                           = nullptr;
static PFN_XInputSetState g_FpnSetState                           = nullptr;
static PFN_XInputGetCapabilities g_FpnGetCaps                     = nullptr;
static PFN_XInputEnable g_FpnEnable                               = nullptr;
static PFN_XInputGetAudioDeviceIds g_FpnGetAudioDeviceIds         = nullptr;
static PFN_XInputGetBatteryInformation g_FpnGetBattery            = nullptr;
static PFN_XInputGetKeystroke g_FpnGetKeystroke                   = nullptr;
static PFN_XInputGetStateEx g_FpnGetStateEx                       = nullptr;
static PFN_XInputWaitForGuideButton g_FpnWaitForGuideButton       = nullptr;
static PFN_XInputCancelGuideButtonWait g_FpnCancelGuideButtonWait = nullptr;
static PFN_XInputPowerOffController g_FpnPowerOffController       = nullptr;
static PFN_XInputGetBaseBusInformation g_FpnGetBaseBusInformation = nullptr;
static PFN_XInputGetCapabilitiesEx g_FpnGetCapabilitiesEx         = nullptr;
static PFN_XInputGetSystemButtons g_FpnGetSystemButtons           = nullptr;

// ---------------------------------------------------------------------------
// DualSense state
// ---------------------------------------------------------------------------

static hid_device *g_DsDevice = nullptr;
static bool g_DsConnected     = false;
static bool g_DsIsBluetooth   = false;
static DWORD g_PacketNumber   = 0;
static dualsense::OutputReport::Sequence g_Sequence;

// Rumble state
static uint8_t g_SmallMotor = 0;
static uint8_t g_LargeMotor = 0;

// LED / color state
static uint8_t g_LedNumber    = 0;
static DWORD g_Color          = 0xFF0000; // Default blue
static uint8_t g_BatteryLevel = 0;

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
  if (!PathCombineA(fullPath, sysDir, "XInput1_4.dll")) return false;

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

static uint8_t playerLedMask() {
  if (g_LedNumber != 0) return g_LedNumber;

  if (g_BatteryLevel == 0) return 0;
  if (g_BatteryLevel >= 100) return 0x1F;
  return static_cast<uint8_t>((1u << ((g_BatteryLevel * 5) / 100)) - 1);
}

template <typename Fill>
static bool sendOutput(Fill fill) {
  if (!g_DsDevice) return false;

  if (g_DsIsBluetooth) {
    dualsense::OutputReport report(g_Sequence);
    fill(report.getBody());
    report.sign();
    return hid_write(g_DsDevice, reinterpret_cast<const unsigned char *>(&report), sizeof(report)) >
           0;
  } else {
    dualsense::UsbOutputReport report{};
    fill(report.getBody());
    return hid_write(g_DsDevice, reinterpret_cast<const unsigned char *>(&report), sizeof(report)) >
           0;
  }
}

static void sendDualSenseOutput() {
  sendOutput([](dualsense::OutputBody &body) {
    const uint8_t leds = playerLedMask();
    body.setCompatibleVibration(g_LargeMotor, g_SmallMotor);
    body.setPlayerLedBrightness(dualsense::OutputBody::PlayerLedBrightness::Low);
    body.setPlayerLeds((leds & 0x01) != 0, (leds & 0x02) != 0, (leds & 0x04) != 0,
                       (leds & 0x08) != 0, (leds & 0x10) != 0, false);
    body.setLightbar(static_cast<uint8_t>(g_Color & 0xFF),
                     static_cast<uint8_t>((g_Color >> 8) & 0xFF),
                     static_cast<uint8_t>((g_Color >> 16) & 0xFF));
  });
}

static void cleanControllerState() {
  sendOutput([](dualsense::OutputBody &body) {
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

static void tryConnectDualSense() {
  if (g_DsConnected) return;

  hid_device_info *devices = hid_enumerate(SONY_VID, 0);
  if (!devices) return;

  const hid_device_info *chosen = nullptr;
  for (hid_device_info *cur = devices; cur; cur = cur->next) {
    if (!isDualSenseDevice(cur)) continue;
    if (!chosen) chosen = cur;
    if (cur->usage_page == 1 && cur->usage == 5) {
      chosen = cur;
      break;
    }
  }
  if (!chosen) {
    hid_free_enumeration(devices);
    return;
  }

  hid_device *dev = hid_open_path(chosen->path);
  if (!dev) {
    hid_free_enumeration(devices);
    return;
  }

  g_DsDevice      = dev;
  g_DsIsBluetooth = chosen->bus_type == HID_API_BUS_BLUETOOTH;
  g_DsConnected   = true;
  g_BatteryLevel  = 0;

  hid_set_nonblocking(dev, true);
  sendDualSenseOutput();
  hid_free_enumeration(devices);
}

// ---------------------------------------------------------------------------
// DualSense input -> XInput mapping (called directly from XInputGetState)
// ---------------------------------------------------------------------------

static bool readDualSenseState(XINPUT_STATE &state) {
  if (!g_DsDevice) return false;

  uint8_t buffer[512];
  int n = hid_read(g_DsDevice, buffer, sizeof(buffer));
  if (n <= 0) return false;
  size_t bytesRead = static_cast<size_t>(n);

  const dualsense::InputBody *input = nullptr;
  if (auto *report = dualsense::InputReport::asFrame(buffer, bytesRead)) {
    input = &report->getBody();
  } else if (auto *report = dualsense::UsbInputReport::asFrame(buffer, bytesRead)) {
    input = &report->getBody();
  } else
    return false;

  const unsigned battery = input->getBatteryPercentage();
  if (battery != g_BatteryLevel) {
    g_BatteryLevel = battery;
    sendDualSenseOutput();
  }

  zeroMemory(&state, sizeof(state));
  state.dwPacketNumber = ++g_PacketNumber;

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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  (void)hModule;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    loadSystemXInput();
    hid_init();
    break;

  case DLL_PROCESS_DETACH:
    if (g_DsDevice) {
      cleanControllerState();
      hid_close(g_DsDevice);
      g_DsDevice = nullptr;
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

// Ordinal 2
DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState) noexcept {
  if (dwUserIndex == 0) {
    if (!pState) return ERROR_INVALID_PARAMETER;

    // Try to connect if not already connected
    if (!g_DsConnected) tryConnectDualSense();

    if (g_DsConnected && pState && readDualSenseState(*pState)) return ERROR_SUCCESS;
  }

  if (g_FpnGetState) return g_FpnGetState(dwUserIndex, pState);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 3
DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration) noexcept {
  if (dwUserIndex == 0) {
    if (!pVibration) return ERROR_INVALID_PARAMETER;

    if (!g_DsConnected) tryConnectDualSense();

    if (g_DsConnected) {
      uint32_t sm  = pVibration->wRightMotorSpeed / 257;
      g_SmallMotor = pVibration->wRightMotorSpeed ? static_cast<uint8_t>(sm < 64 ? 64 : sm) : 0;
      uint32_t lm  = pVibration->wLeftMotorSpeed / 257;
      g_LargeMotor = pVibration->wLeftMotorSpeed ? static_cast<uint8_t>(lm < 40 ? 40 : lm) : 0;

      g_LedNumber = 0;
      sendDualSenseOutput();
      return ERROR_SUCCESS;
    }
  }

  if (g_FpnSetState) return g_FpnSetState(dwUserIndex, pVibration);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 4
DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
                                   XINPUT_CAPABILITIES *pCapabilities) noexcept {
  if (dwUserIndex == 0) {
    if (!pCapabilities) return ERROR_INVALID_PARAMETER;
    if (dwFlags != 0 && dwFlags != XINPUT_FLAG_GAMEPAD) return ERROR_BAD_ARGUMENTS;

    if (!g_DsConnected) tryConnectDualSense();

    if (g_DsConnected) {
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

// Ordinal 5
void WINAPI XInputEnable(BOOL enable) noexcept {
  if (g_FpnEnable) g_FpnEnable(enable);
}

// Ordinal 6
extern "C" DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId, UINT *pRenderCount,
                                     LPWSTR pCaptureDeviceId, UINT *pCaptureCount) noexcept {
  if (g_FpnGetAudioDeviceIds)
    return g_FpnGetAudioDeviceIds(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId,
                                  pCaptureCount);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 7
DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
                                         XINPUT_BATTERY_INFORMATION *pBatteryInformation) noexcept {
  if (dwUserIndex == 0) {
    if (pBatteryInformation) {
      pBatteryInformation->BatteryType =
          BATTERY_TYPE_NIMH; // It's actually litium polymer but this is best matching
      if (g_BatteryLevel <= 10)
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_EMPTY;
      else if (g_BatteryLevel <= 35)
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_LOW;
      else if (g_BatteryLevel <= 75)
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_MEDIUM;
      else
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_FULL;
      return ERROR_SUCCESS;
    }
  }

  if (g_FpnGetBattery) return g_FpnGetBattery(dwUserIndex, devType, pBatteryInformation);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 8
DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
                                PXINPUT_KEYSTROKE pKeystroke) noexcept {
  if (g_FpnGetKeystroke) return g_FpnGetKeystroke(dwUserIndex, dwReserved, pKeystroke);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 100 — XInputGetStateEx (undocumented)
DWORD WINAPI XInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE *pState) noexcept {
  return XInputGetState(dwUserIndex, pState);
}

// Ordinal 101 — XInputWaitForGuideButton (undocumented)
extern "C" DWORD WINAPI XInputWaitForGuideButton(DWORD dwUserIndex, DWORD dwFlag, LPVOID pVoid) noexcept {
  if (g_FpnWaitForGuideButton) return g_FpnWaitForGuideButton(dwUserIndex, dwFlag, pVoid);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 102 — XInputCancelGuideButtonWait (undocumented)
extern "C" DWORD WINAPI XInputCancelGuideButtonWait(DWORD dwUserIndex) noexcept {
  if (g_FpnCancelGuideButtonWait) return g_FpnCancelGuideButtonWait(dwUserIndex);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 103 — XInputPowerOffController (undocumented)
extern "C" DWORD WINAPI XInputPowerOffController(DWORD dwUserIndex) noexcept {
  if (g_FpnPowerOffController) return g_FpnPowerOffController(dwUserIndex);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 104 — XInputGetBaseBusInformation (undocumented)
extern "C" DWORD WINAPI XInputGetBaseBusInformation(
    DWORD dwUserIndex, DWORD dwFlags, XINPUT_BASE_BUS_INFORMATION *pBaseBusInformation) noexcept {
  if (g_FpnGetBaseBusInformation)
    return g_FpnGetBaseBusInformation(dwUserIndex, dwFlags, pBaseBusInformation);
  return ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinal 108 — XInputGetCapabilitiesEx (undocumented)
#ifndef XINPUT_MINGW_NO_GET_CAPABILITIES_EX
extern "C" DWORD WINAPI XInputGetCapabilitiesEx(DWORD dwUserIndex, DWORD dwFlags,
                                     XINPUT_CAPABILITIES_EX *pCapabilitiesEx) noexcept {
  if (g_FpnGetCapabilitiesEx) return g_FpnGetCapabilitiesEx(dwUserIndex, dwFlags, pCapabilitiesEx);
  return ERROR_DEVICE_NOT_CONNECTED;
}
#endif

// Ordinal 109 — XInputGetSystemButtons (undocumented)
extern "C" DWORD WINAPI XInputGetSystemButtons(DWORD dwUserIndex, XINPUT_KEYSTROKE *pKeystroke) noexcept {
  if (g_FpnGetSystemButtons) return g_FpnGetSystemButtons(dwUserIndex, pKeystroke);
  return ERROR_DEVICE_NOT_CONNECTED;
}
