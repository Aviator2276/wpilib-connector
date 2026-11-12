// Wire protocol between halsim_webbridge (in the robot process) and the
// mirror HAL (in the browser). See PROTOCOL.md for the prose description.
//
// A WebSocket binary frame is one MessagePack array of messages. A message
// is a MessagePack array whose first element is an Op. Values use the
// narrowest MessagePack type: booleans as bool, HAL int/long/enum as
// integers, doubles as float 64.
//
// This header carries its own small MessagePack encoder and decoder so the
// two ends, built by different toolchains, share one implementation and need
// nothing from each other's libraries.

#pragma once

#include <stdint.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace websim::proto {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kDefaultPort = 3301;

enum Op : uint8_t {
  // ---- bridge -> mirror
  kHello = 0,       // [op, protocolVersion, halTableHash, wpilibVersion]
  kSimDevNew = 2,   // [op, devId, name]
  kSimDevFree = 3,  // [op, devId]
  // [op, devId, valId, name, direction, halType, value, [options], [optionValues]]
  kSimValNew = 4,
  kEncoderCh = 6,     // [op, index, channelA, channelB]
  kLedData = 7,       // [op, index, bin(r,g,b,...)]
  kJoyOutputs = 8,    // [op, stick, outputs, leftRumble, rightRumble]
  // [op, fpgaTimeUs, paused, nextNotifierTimeoutUs, [[name, timeoutUs, valid]...]]
  kTiming = 9,
  kSnapshotEnd = 12,  // [op]   everything before this was current state

  // ---- both directions
  kField = 1,     // [op, fieldIndex, deviceIndex, value]
  kSimVal = 5,    // [op, valId, value]
  kGameData = 10, // [op, str]

  // ---- mirror -> bridge
  kJoystick = 20,  // [op, stick, [axes], [povs], buttons, buttonCount]
  kNewData = 21,   // [op]   latch DS + joystick data in the robot program
  kPause = 22,     // [op, bool]
  kStep = 23,      // [op, deltaUs]
};

// ---------------------------------------------------------------- encoder --

class Writer {
 public:
  std::vector<uint8_t>& buf() { return m_buf; }
  const std::vector<uint8_t>& buf() const { return m_buf; }
  bool empty() const { return m_count == 0; }
  uint32_t count() const { return m_count; }

  // Start a frame: an array whose length is patched in by Finish().
  void Begin() {
    m_buf.clear();
    m_count = 0;
    Put(0xdd);  // array 32
    Put32(0);
  }
  void Finish() {
    m_buf[1] = m_count >> 24;
    m_buf[2] = m_count >> 16;
    m_buf[3] = m_count >> 8;
    m_buf[4] = m_count;
  }
  // Start one message of `n` elements (including the op).
  void Message(Op op, uint32_t n) {
    ++m_count;
    Array(n);
    UInt(op);
  }

  void Array(uint32_t n) {
    if (n < 16) {
      Put(0x90 | n);
    } else if (n < 65536) {
      Put(0xdc);
      Put16(n);
    } else {
      Put(0xdd);
      Put32(n);
    }
  }
  void Nil() { Put(0xc0); }
  void Bool(bool v) { Put(v ? 0xc3 : 0xc2); }
  void UInt(uint64_t v) {
    if (v < 128) {
      Put(v);
    } else if (v <= 0xff) {
      Put(0xcc);
      Put(v);
    } else if (v <= 0xffff) {
      Put(0xcd);
      Put16(v);
    } else if (v <= 0xffffffffu) {
      Put(0xce);
      Put32(v);
    } else {
      Put(0xcf);
      Put64(v);
    }
  }
  void Int(int64_t v) {
    if (v >= 0) {
      UInt(v);
    } else if (v >= -32) {
      Put(static_cast<uint8_t>(v));
    } else if (v >= INT8_MIN) {
      Put(0xd0);
      Put(static_cast<uint8_t>(v));
    } else if (v >= INT16_MIN) {
      Put(0xd1);
      Put16(static_cast<uint16_t>(v));
    } else if (v >= INT32_MIN) {
      Put(0xd2);
      Put32(static_cast<uint32_t>(v));
    } else {
      Put(0xd3);
      Put64(static_cast<uint64_t>(v));
    }
  }
  void Double(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    Put(0xcb);
    Put64(bits);
  }
  void Str(std::string_view s) {
    if (s.size() < 32) {
      Put(0xa0 | s.size());
    } else if (s.size() < 256) {
      Put(0xd9);
      Put(s.size());
    } else if (s.size() < 65536) {
      Put(0xda);
      Put16(s.size());
    } else {
      Put(0xdb);
      Put32(s.size());
    }
    m_buf.insert(m_buf.end(), s.begin(), s.end());
  }
  void Bin(const uint8_t* data, size_t size) {
    if (size < 256) {
      Put(0xc4);
      Put(size);
    } else if (size < 65536) {
      Put(0xc5);
      Put16(size);
    } else {
      Put(0xc6);
      Put32(size);
    }
    m_buf.insert(m_buf.end(), data, data + size);
  }

 private:
  void Put(uint64_t b) { m_buf.push_back(static_cast<uint8_t>(b)); }
  void Put16(uint64_t v) {
    Put(v >> 8);
    Put(v);
  }
  void Put32(uint64_t v) {
    Put16(v >> 16);
    Put16(v);
  }
  void Put64(uint64_t v) {
    Put32(v >> 32);
    Put32(v);
  }

  std::vector<uint8_t> m_buf;
  uint32_t m_count = 0;
};

// ---------------------------------------------------------------- decoder --

struct Value {
  enum Kind { kNil, kBool, kInt, kDouble, kStr, kBin, kArray, kError };
  Kind kind = kError;
  int64_t i = 0;         // kBool, kInt; element count for kArray
  double d = 0;          // kDouble
  std::string_view s;    // kStr, kBin (points into the frame)

  bool IsNumber() const { return kind == kInt || kind == kDouble || kind == kBool; }
  double AsDouble() const { return kind == kDouble ? d : static_cast<double>(i); }
  int64_t AsInt() const { return kind == kDouble ? static_cast<int64_t>(d) : i; }
  bool AsBool() const { return kind == kDouble ? d != 0 : i != 0; }
};

// Pull parser over one frame. After an error every read returns kError, so
// callers can check ok() once per message instead of after every element.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : m_p{data}, m_end{data + size} {}

  bool ok() const { return !m_error; }
  bool AtEnd() const { return m_p >= m_end; }

  Value Next() {
    Value v;
    if (m_error || m_p >= m_end) {
      m_error = true;
      return v;
    }
    uint8_t b = *m_p++;
    if (b < 0x80) {
      return Int(b);
    }
    if (b >= 0xe0) {
      return Int(static_cast<int8_t>(b));
    }
    if ((b & 0xf0) == 0x90) {
      return Arr(b & 0x0f);
    }
    if ((b & 0xe0) == 0xa0) {
      return Bytes(Value::kStr, b & 0x1f);
    }
    switch (b) {
      case 0xc0:
        v.kind = Value::kNil;
        return v;
      case 0xc2:
      case 0xc3:
        v.kind = Value::kBool;
        v.i = b == 0xc3;
        return v;
      case 0xc4:
        return Bytes(Value::kBin, Get(1));
      case 0xc5:
        return Bytes(Value::kBin, Get(2));
      case 0xc6:
        return Bytes(Value::kBin, Get(4));
      case 0xca: {
        uint32_t bits = static_cast<uint32_t>(Get(4));
        float f;
        std::memcpy(&f, &bits, 4);
        v.kind = Value::kDouble;
        v.d = f;
        return Check(v);
      }
      case 0xcb: {
        uint64_t bits = Get(8);
        v.kind = Value::kDouble;
        std::memcpy(&v.d, &bits, 8);
        return Check(v);
      }
      case 0xcc:
        return Int(Get(1));
      case 0xcd:
        return Int(Get(2));
      case 0xce:
        return Int(Get(4));
      case 0xcf:
        return Int(static_cast<int64_t>(Get(8)));
      case 0xd0:
        return Int(static_cast<int8_t>(Get(1)));
      case 0xd1:
        return Int(static_cast<int16_t>(Get(2)));
      case 0xd2:
        return Int(static_cast<int32_t>(Get(4)));
      case 0xd3:
        return Int(static_cast<int64_t>(Get(8)));
      case 0xd9:
        return Bytes(Value::kStr, Get(1));
      case 0xda:
        return Bytes(Value::kStr, Get(2));
      case 0xdb:
        return Bytes(Value::kStr, Get(4));
      case 0xdc:
        return Arr(Get(2));
      case 0xdd:
        return Arr(Get(4));
      default:
        m_error = true;  // maps and extensions are not part of the protocol
        return v;
    }
  }

  // Skip `n` more values (descending into arrays).
  void Skip(int64_t n) {
    while (n-- > 0 && !m_error) {
      Value v = Next();
      if (v.kind == Value::kArray) {
        Skip(v.i);
      }
    }
  }

 private:
  uint64_t Get(int bytes) {
    if (m_end - m_p < bytes) {
      m_error = true;
      return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
      v = (v << 8) | *m_p++;
    }
    return v;
  }
  Value Check(Value v) {
    if (m_error) {
      v.kind = Value::kError;
    }
    return v;
  }
  Value Int(int64_t i) {
    Value v;
    v.kind = Value::kInt;
    v.i = i;
    return Check(v);
  }
  Value Arr(uint64_t n) {
    Value v;
    v.kind = Value::kArray;
    v.i = static_cast<int64_t>(n);
    // each element takes at least one byte; reject impossible counts early
    if (n > static_cast<uint64_t>(m_end - m_p)) {
      m_error = true;
    }
    return Check(v);
  }
  Value Bytes(Value::Kind kind, uint64_t n) {
    Value v;
    if (m_error || n > static_cast<uint64_t>(m_end - m_p)) {
      m_error = true;
      return v;
    }
    v.kind = kind;
    v.s = {reinterpret_cast<const char*>(m_p), static_cast<size_t>(n)};
    m_p += n;
    return v;
  }

  const uint8_t* m_p;
  const uint8_t* m_end;
  bool m_error = false;
};

}  // namespace websim::proto
