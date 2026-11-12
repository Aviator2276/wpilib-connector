// HAL_Value <-> wire value, shared by both ends of the bridge protocol.
#pragma once

#include <hal/Value.h>

#include "bridge_protocol.h"

namespace websim::proto {

inline void WriteHalValue(Writer& w, const HAL_Value& v) {
  switch (v.type) {
    case HAL_BOOLEAN:
      w.Bool(v.data.v_boolean != 0);
      break;
    case HAL_DOUBLE:
      w.Double(v.data.v_double);
      break;
    case HAL_INT:
      w.Int(v.data.v_int);
      break;
    case HAL_LONG:
      w.Int(v.data.v_long);
      break;
    case HAL_ENUM:
      w.Int(v.data.v_enum);
      break;
    default:
      w.Nil();
      break;
  }
}

// Convert a decoded wire value to the HAL type the receiver expects.
inline bool ToHalValue(const Value& in, HAL_Type type, HAL_Value* out) {
  if (!in.IsNumber()) {
    return false;
  }
  switch (type) {
    case HAL_BOOLEAN:
      *out = HAL_MakeBoolean(in.AsBool());
      return true;
    case HAL_DOUBLE:
      *out = HAL_MakeDouble(in.AsDouble());
      return true;
    case HAL_INT:
      *out = HAL_MakeInt(static_cast<int32_t>(in.AsInt()));
      return true;
    case HAL_LONG:
      *out = HAL_MakeLong(in.AsInt());
      return true;
    case HAL_ENUM:
      *out = HAL_MakeEnum(static_cast<int32_t>(in.AsInt()));
      return true;
    default:
      return false;
  }
}

}  // namespace websim::proto
