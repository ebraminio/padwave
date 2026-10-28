// padwave_app_stub.hh — App-less Input emit hooks for tests and fuzzers.
//
// The real definitions live in padwave.cc and call App. A test that exercises
// Input::parse() without an App links these no-ops instead.
#pragma once

#include "padwave_app_fwd.hh"
#include "padwave_input.hh"

inline void Input::emitFrameToApp(const input_frame *) {}

inline void Input::emitAction(const char *, int) {}

inline void Input::emitEvent(const char *) {}

inline void Input::emitMic(const uint8_t *, size_t) {}
