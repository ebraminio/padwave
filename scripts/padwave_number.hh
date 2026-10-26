#pragma once
// Similar to HarfBuzz's hb-open-type.hh: wire integer types with implicit
// conversion to arithmetic types, but little-endian instead of HarfBuzz's
// big-endian.
//
// The value is *stored as little-endian bytes*, not as a host integer, so a
// struct made of these types is byte-identical to the wire on any machine —
// big-endian hosts included. get()/set() assemble and split the bytes one at a
// time; there is no memcpy of an integer and no host byte-order assumption.

#include <stddef.h>
#include <stdint.h>

namespace padwave_number {

template <typename Type, typename UnsignedType = Type>
struct NumType {
  static constexpr unsigned int static_size = sizeof(Type);

  NumType() = default;
  explicit NumType(Type value) { set(value); }

  NumType &operator=(Type o) {
    set(o);
    return *this;
  }

  operator Type() const { return get(); }

  // bytes[0] is the least significant byte, on every host.
  Type get() const {
    UnsignedType v = 0;
    for (unsigned int i = 0; i < sizeof(Type); i++)
      v = (UnsignedType)((v << 8) | bytes[sizeof(Type) - 1 - i]);
    return (Type)v;
  }

  void set(Type value) {
    UnsignedType v = (UnsignedType)value;
    for (unsigned int i = 0; i < sizeof(Type); i++) {
      bytes[i] = (unsigned char)(v & 0xFF);
      v = (UnsignedType)(v >> 8);
    }
  }

  bool operator==(const NumType &o) const { return get() == o.get(); }
  bool operator!=(const NumType &o) const { return get() != o.get(); }
  bool operator==(Type o) const { return get() == o; }
  bool operator!=(Type o) const { return get() != o; }

  NumType &operator+=(Type n) {
    set((Type)(get() + n));
    return *this;
  }
  NumType &operator-=(Type n) {
    set((Type)(get() - n));
    return *this;
  }
  NumType &operator++() {
    set((Type)(get() + 1));
    return *this;
  }
  NumType &operator--() {
    set((Type)(get() - 1));
    return *this;
  }

  unsigned char bytes[sizeof(Type)];
};

} // namespace padwave_number

typedef padwave_number::NumType<uint8_t> PWUINT8;
typedef padwave_number::NumType<uint16_t> PWUINT16;
typedef padwave_number::NumType<uint32_t> PWUINT32;
typedef padwave_number::NumType<int8_t, uint8_t> PWINT8;
typedef padwave_number::NumType<int16_t, uint16_t> PWINT16;
typedef padwave_number::NumType<int32_t, uint32_t> PWINT32;

static_assert(sizeof(PWUINT8) == 1, "PWUINT8 is one byte");
static_assert(sizeof(PWUINT16) == 2, "PWUINT16 is two bytes");
static_assert(sizeof(PWUINT32) == 4, "PWUINT32 is four bytes");
static_assert(sizeof(PWINT8) == 1, "PWINT8 is one byte");
static_assert(sizeof(PWINT16) == 2, "PWINT16 is two bytes");
static_assert(sizeof(PWINT32) == 4, "PWINT32 is four bytes");
