# ds2xi

A Windows XInput wrapper for DualSense, built on `dualsense.hh`.

It exports `XInput1_4.dll` and maps player 0 to the first DualSense found over
USB or Bluetooth. Rumble, the lightbar, player LEDs, battery level, sticks,
triggers, buttons, and the d-pad are supported. Other XInput users fall through
to the system `XInput1_4.dll`.

Build from this directory with xmake:

```bat
xmake f -c -y
xmake -y
```

HIDAPI comes from `../dualsense-demo/third_party/hidapi`; build the demo
submodules first if needed.

Copy `build/windows/x64/release/XInput1_4.dll` next to the game or into its
loader directory. The supplied `xinput.def` keeps the standard XInput export
ordinals, including the undocumented ones.
