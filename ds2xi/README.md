# ds2xi

A Windows XInput wrapper for DualSense, built on `dualsense.hh`.

It exports matching shims for all common XInput variants:

```text
xinput1_1.dll
xinput1_2.dll
xinput1_3.dll
XInput1_4.dll
XInput9_1_0.dll
XInputUap.dll
```

Each shim maps up to four DualSense controllers to XInput user slots 0-3.
Rumble, the lightbar, player LEDs, battery level, sticks, triggers, buttons,
and the d-pad are supported. Calls for unconnected slots fall through to the
matching system DLL. The latest version also exports the undocumented XInput
ordinals.

## Requirements

- Windows with a recent Visual Studio installation, or clang-cl, or MinGW-w64
- [xmake](https://xmake.io/)
- For MinGW, MSYS2 with both toolchains:

  ```bat
  winget install MSYS2.MSYS2
  ```

  In an MSYS2 shell:

  ```bash
  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-i686-gcc
  ```

Do not hardcode a toolchain path in scripts. Select it with `PATH` and let
xmake discover the toolchain.

## Build

Run from this directory. All project targets are compiled with warnings as
errors: `/W4 /WX` for MSVC/clang-cl and `-Wall -Wextra -Werror` for MinGW.

### MSVC

```bat
xmake f -c -y -p windows -a x64 --toolchain=msvc
xmake -y
```

For x86:

```bat
xmake f -c -y -p windows -a x86 --toolchain=msvc
xmake -y
```

### clang-cl

```bat
set PATH=C:\Program Files\LLVM\bin;%PATH%
xmake f -c -y -p windows -a x64 --toolchain=clang-cl
xmake -y
```

Use `-a x86` for the 32-bit build.

### MinGW

For x64:

```bat
set PATH=C:\msys64\mingw64\bin;%PATH%
xmake f -c -y -p mingw -a x64 --toolchain=mingw
xmake -y
```

For x86:

```bat
set PATH=C:\msys64\mingw32\bin;%PATH%
xmake f -c -y -p mingw -a x86 --toolchain=mingw
xmake -y
```

If MSYS2 is installed elsewhere, adjust `PATH` accordingly. `w64devkit` is an
x64-only portable alternative; use MSYS2 for both architectures.

## Outputs

For the Windows/MSVC and clang-cl builds:

```text
build\windows\<arch>\release\
```

For MinGW:

```text
build\mingw\<arch>\release\
```

Each directory contains the six DLLs and six matching test executables:

```text
xinput1_1.dll    xinput1_1_test.exe
xinput1_2.dll    xinput1_2_test.exe
xinput1_3.dll    xinput1_3_test.exe
XInput1_4.dll    XInput1_4_test.exe
XInput9_1_0.dll  XInput9_1_0_test.exe
XInputUap.dll    XInputUap_test.exe
```

The testers take no arguments. Each loads the same-named DLL from its own
directory, so either run it from that directory or copy the pair together:

```bat
cd build\windows\x64\release
XInput1_4_test.exe
```

A connected controller should print `GetState=0x00000000`; without one it
prints `0x0000048F` (`ERROR_DEVICE_NOT_CONNECTED`). The tester also prints
capabilities, battery information where exported, undocumented
`XInputGetStateEx` where exported, and briefly exercises rumble.

## Deploy

Copy only the desired XInput DLL from the build output into the game or its
loader directory. The test executables are only for validation and should not
be deployed.

The export tables live next to the implementation:

- `xinput1_1.def`, `xinput1_2.def`, `xinput1_3.def`
- `xinput1_4.def`, `xinput9_1_0.def`, `xinputuap.def`

`main.cc` also compiles only the wrappers selected for each variant, so the
source and export table stay aligned.
