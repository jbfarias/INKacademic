#pragma once

#include <cstddef>
#include <cstring>

#include "FirmwareVersion.h"

namespace firmware_identity {
// Bounded streaming parser: no growing string, memmove or per-byte allocation.
// Invalid candidates (including the parser's own prefix literal) are skipped.
class Scanner {
 public:
  bool feed(char c) {
    if (found_) return true;
    if (state_ == 0) {
      if (c == prefix_[position_]) {
        if (++position_ == sizeof(prefix_) - 1) {
          state_ = 1;
          position_ = 0;
        }
      } else {
        position_ = c == prefix_[0] ? 1 : 0;
      }
    } else if (state_ == 1) {
      if (c == '|' && position_ > 0) {
        device_[position_] = '\0';
        state_ = 2;
        position_ = 0;
      } else if (((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') && position_ < 31) {
        device_[position_++] = c;
      } else
        reset(c);
    } else if (state_ == 2) {
      if (c != delimiter_[position_])
        reset(c);
      else if (++position_ == sizeof(delimiter_) - 1) {
        state_ = 3;
        position_ = 0;
      }
    } else {
      if (c == '|' && position_ > 0) {
        version_[position_] = '\0';
        if (firmware_version::parse(version_).valid)
          found_ = true;
        else
          reset(c);
      } else if (c > ' ' && c <= '~' && position_ < 63)
        version_[position_++] = c;
      else
        reset(c);
    }
    return found_;
  }
  bool found() const { return found_; }
  const char* device() const { return device_; }
  const char* version() const { return version_; }

 private:
  void reset(char c) {
    state_ = 0;
    position_ = c == prefix_[0] ? 1 : 0;
  }
  static constexpr char prefix_[] = "INKADEMIC_FW_ID|device=";
  static constexpr char delimiter_[] = "version=";
  char device_[32] = {};
  char version_[64] = {};
  size_t position_ = 0;
  unsigned state_ = 0;
  bool found_ = false;
};
}  // namespace firmware_identity
