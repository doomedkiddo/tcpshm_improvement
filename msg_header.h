/*
MIT License

Copyright (c) 2018 Meng Rao <raomeng1@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once
#include <cstdint>
#include <cstring>
#include <concepts>
#include <bit>

namespace tcpshm {

// Detect system endianness using C++20 <bit> header
static constexpr bool is_little_endian = std::endian::native == std::endian::little;

// Endianness handling class
template<bool ToLittle>
class Endian {
public:
  static constexpr bool IsLittle = is_little_endian;

  // Unsigned integer conversions with concepts
  template<std::unsigned_integral T>
  static T Convert(T v) {
    if constexpr (ToLittle == IsLittle) 
      return v;
    else if constexpr (sizeof(T) == 2)
      return bswap16(v);
    else if constexpr (sizeof(T) == 4)
      return bswap32(v);
    else if constexpr (sizeof(T) == 8)
      return bswap64(v);
    else
      return v; // For other sizes, no conversion
  }
  
  // Signed integer conversions with concepts
  template<std::signed_integral T>
  static T Convert(T v) {
    return static_cast<T>(Convert(static_cast<std::make_unsigned_t<T>>(v)));
  }
  
  // Floating point conversions with concepts
  template<std::floating_point T>
  static T Convert(T v) {
    if constexpr (sizeof(T) == 4) {
      union { float f; uint32_t i; } u{.f = v};
      u.i = Convert(u.i);
      return u.f;
    }
    else if constexpr (sizeof(T) == 8) {
      union { double d; uint64_t i; } u{.d = v};
      u.i = Convert(u.i);
      return u.d;
    }
    else {
      return v; // For other sizes, no conversion
    }
  }
  
  // In-place conversion template
  template<typename T>
  static void ConvertInPlace(T& t) {
    t = Convert(t);
  }

private:
  // Byte swap functions
  static constexpr uint16_t bswap16(uint16_t x) {
    return ((x & 0x00FFU) << 8) | ((x & 0xFF00U) >> 8);
  }

  static constexpr uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000FFU) << 24) | 
           ((x & 0x0000FF00U) << 8)  | 
           ((x & 0x00FF0000U) >> 8)  | 
           ((x & 0xFF000000U) >> 24);
  }

  static constexpr uint64_t bswap64(uint64_t x) {
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) << 8)  |
           ((x & 0x000000FF00000000ULL) >> 8)  |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
  }
};

struct MsgHeader
{
    // size of this msg, including header itself
    // auto set by lib, can be read by user
    uint16_t size;
    // msg type of app msg is set by user and must not be 0
    uint16_t msg_type;
    // internally used for ptcp, must not be modified by user
    uint32_t ack_seq;

    template<bool ToLittle>
    void ConvertByteOrder() {
        Endian<ToLittle> ed;
        ed.ConvertInPlace(size);
        ed.ConvertInPlace(msg_type);
        ed.ConvertInPlace(ack_seq);
    }
};
} // namespace tcpshm

