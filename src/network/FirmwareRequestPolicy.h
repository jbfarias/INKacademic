#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

namespace firmware_request {
// Firmware controls are served by the reader itself. Browser clients must use
// its origin; command-line clients without Origin must still opt in explicitly.
inline bool allowed(std::string_view origin, std::string_view host, std::string_view intent) {
  if (intent != "1" || host.empty()) return false;
  if (origin.empty()) return true;
  constexpr std::string_view scheme = "http://";
  return origin.substr(0, scheme.size()) == scheme && origin.substr(scheme.size()) == host;
}

inline bool validStateValue(std::string_view value) {
  if (value.empty() || value.size() > 255) return false;
  return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < ' ' || c == ';' || c == '='; });
}

inline bool validManualUrl(std::string_view url) {
  if (url.empty() || url.size() > 1024) return false;
  size_t start = 0;
  if (url.substr(0, 8) == "https://")
    start = 8;
  else if (url.substr(0, 7) == "http://")
    start = 7;
  else
    return false;
  const size_t end = url.find_first_of("/?#", start);
  const auto host = url.substr(start, end == std::string_view::npos ? end : end - start);
  if (host.empty() || host.find('@') != std::string_view::npos) return false;
  return std::none_of(url.begin(), url.end(), [](char c) { return c <= ' ' || c > '~' || c == '#' || c == '\\'; });
}

inline bool parseSize(std::string_view text, size_t& result) {
  result = 0;
  if (text.empty()) return false;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    const size_t digit = static_cast<size_t>(c - '0');
    if (result > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
    result = result * 10 + digit;
  }
  return true;
}

// Each multipart request owns its terminal events only after START is accepted.
// A rejected request must not close/remove or change another transaction.
class UploadRequest {
 public:
  bool start() {
    if (started_) {
      reject("Send one file per firmware request.");
      return false;
    }
    started_ = true;
    return true;
  }
  void finish() {
    started_ = false;
    accepted_ = false;
    error_ = nullptr;
  }
  void accept() { accepted_ = true; }
  void reject(const char* reason) {
    accepted_ = false;
    error_ = reason;
  }
  bool accepted() const { return accepted_; }
  const char* error() const { return error_; }

 private:
  bool started_ = false;
  bool accepted_ = false;
  const char* error_ = nullptr;  // static error messages only
};
}  // namespace firmware_request
