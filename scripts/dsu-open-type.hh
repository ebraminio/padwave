// dsu-open-type.hh — the wire integers, after HarfBuzz's hb-open-type.hh.
//
// HarfBuzz's idea, applied to the DualSense: a field type that *is* the bytes
// on the wire, with an implicit conversion to the arithmetic type so reading
// one is transparent, but a distinct type so the code says "this is a wire
// field" rather than "this is a number". HBUINT8/16/32 become DSUINT8/16/32;
// the signed counterparts follow the same DS prefix.
//
// The controller is little-endian and so is the host (asserted by the schemas
// header), so the value can be stored natively and the struct stays a memcpy
// away from bytes.
#ifndef DSU_OPEN_TYPE_HH
#define DSU_OPEN_TYPE_HH

#include <stddef.h>
#include <stdint.h>

namespace dsu_detail {

template <typename Type>
struct NumType {
  typedef Type type;
  static constexpr unsigned int static_size = sizeof(Type);

  NumType() = default;
  explicit constexpr NumType(Type v) : v(v) {}

  NumType &operator=(Type o) {
    v = o;
    return *this;
  }

  operator Type() const { return v; }

  Type get() const { return v; }
  void set(Type o) { v = o; }

  bool operator==(const NumType &o) const { return v == o.v; }
  bool operator!=(const NumType &o) const { return v != o.v; }
  bool operator==(Type o) const { return v == o; }
  bool operator!=(Type o) const { return v != o; }

  NumType &operator+=(Type n) {
    v = (Type)(v + n);
    return *this;
  }
  NumType &operator-=(Type n) {
    v = (Type)(v - n);
    return *this;
  }
  NumType &operator++() {
    v = (Type)(v + 1);
    return *this;
  }
  NumType &operator--() {
    v = (Type)(v - 1);
    return *this;
  }

  Type v;
} __attribute__((packed));

} // namespace dsu_detail

typedef dsu_detail::NumType<uint8_t> DSUINT8;
typedef dsu_detail::NumType<uint16_t> DSUINT16;
typedef dsu_detail::NumType<uint32_t> DSUINT32;
typedef dsu_detail::NumType<int8_t> DSINT8;
typedef dsu_detail::NumType<int16_t> DSINT16;
typedef dsu_detail::NumType<int32_t> DSINT32;

static_assert(sizeof(DSUINT8) == 1, "DSUINT8 is one byte");
static_assert(sizeof(DSUINT16) == 2, "DSUINT16 is two bytes");
static_assert(sizeof(DSUINT32) == 4, "DSUINT32 is four bytes");
static_assert(sizeof(DSINT16) == 2, "DSINT16 is two bytes");

#endif // DSU_OPEN_TYPE_HH
