#include <windows.h>
#include <Xinput.h>

#include <cstdio>
#include <cstring>

typedef DWORD(WINAPI *GetStateFn)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *SetStateFn)(DWORD, XINPUT_VIBRATION *);
typedef DWORD(WINAPI *GetCapsFn)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef DWORD(WINAPI *GetBatteryFn)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
typedef DWORD(WINAPI *GetStateExFn)(DWORD, XINPUT_STATE *);

template <typename T>
static T resolve(HMODULE module, const char *name) {
  return reinterpret_cast<T>(GetProcAddress(module, name));
}

#ifndef DS2XI_TEST_XINPUT
#error "DS2XI_TEST_XINPUT must name the variant DLL next to the tester"
#endif

int main() {
  char exePath[MAX_PATH];
  char dllPath[MAX_PATH];
  DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    std::printf("GetModuleFileName failed\n");
    return 2;
  }
  char *separator = std::strrchr(exePath, '\\');
  if (!separator) {
    std::printf("Cannot find tester directory\n");
    return 2;
  }
  *separator = '\0';
  std::snprintf(dllPath, sizeof(dllPath), "%s\\%s", exePath, DS2XI_TEST_XINPUT);

  std::printf("Loading %s\n", dllPath);
  HMODULE module = LoadLibraryA(dllPath);
  if (!module) {
    std::printf("LoadLibrary failed: %lu\n", GetLastError());
    return 2;
  }

  auto getState = resolve<GetStateFn>(module, "XInputGetState");
  auto setState = resolve<SetStateFn>(module, "XInputSetState");
  auto getCaps = resolve<GetCapsFn>(module, "XInputGetCapabilities");
  auto getBattery = resolve<GetBatteryFn>(module, "XInputGetBatteryInformation");
  auto getStateEx = resolve<GetStateExFn>(module, "XInputGetStateEx");

  if (!getState) {
    std::printf("XInputGetState missing\n");
    FreeLibrary(module);
    return 2;
  }

  XINPUT_STATE state{};
  DWORD result = getState(0, &state);
  std::printf("GetState=0x%08lX packet=%lu buttons=0x%04X LX=%d LY=%d RX=%d RY=%d LT=%u RT=%u\n",
              result, static_cast<unsigned long>(state.dwPacketNumber),
              static_cast<unsigned>(state.Gamepad.wButtons), state.Gamepad.sThumbLX,
              state.Gamepad.sThumbLY, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY,
              static_cast<unsigned>(state.Gamepad.bLeftTrigger),
              static_cast<unsigned>(state.Gamepad.bRightTrigger));

  if (getCaps) {
    XINPUT_CAPABILITIES caps{};
    DWORD capsResult = getCaps(0, 0, &caps);
    std::printf("GetCaps=0x%08lX type=%u subtype=%u flags=0x%04X\n", capsResult,
                static_cast<unsigned>(caps.Type), static_cast<unsigned>(caps.SubType),
                static_cast<unsigned>(caps.Flags));
  }

  if (getBattery) {
    XINPUT_BATTERY_INFORMATION battery{};
    DWORD batteryResult = getBattery(0, BATTERY_DEVTYPE_GAMEPAD, &battery);
    std::printf("GetBattery=0x%08lX type=%u level=%u\n", batteryResult,
                static_cast<unsigned>(battery.BatteryType),
                static_cast<unsigned>(battery.BatteryLevel));
  }

  if (getStateEx) {
    XINPUT_STATE stateEx{};
    DWORD exResult = getStateEx(0, &stateEx);
    std::printf("GetStateEx=0x%08lX packet=%lu\n", exResult,
                static_cast<unsigned long>(stateEx.dwPacketNumber));
  }

  if (result == ERROR_SUCCESS && setState) {
    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = 20000;
    vibration.wRightMotorSpeed = 10000;
    setState(0, &vibration);
    Sleep(200);
    vibration.wLeftMotorSpeed = 0;
    vibration.wRightMotorSpeed = 0;
    setState(0, &vibration);
  }

  FreeLibrary(module);
  return result == ERROR_SUCCESS ? 0 : 1;
}
