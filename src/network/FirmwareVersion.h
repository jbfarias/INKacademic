#pragma once

#include <climits>
#include <cstddef>
#include <cstring>

namespace firmware_version {
struct Version {
  int part[4] = {};
  int rank = 0;  // development < RC < stable
  int rc = 0;
  bool valid = false;
};
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool number(const char*& p, int& value) {
  if (!digit(*p)) return false;
  while (digit(*p)) {
    const int d = *p++ - '0';
    if (value > (INT_MAX - d) / 10) return false;
    value = value * 10 + d;
  }
  return true;
}
inline Version parse(const char* text) {
  Version v;
  if (!text) return v;
  size_t length = 0;
  while (length <= 63 && text[length]) ++length;
  if (length == 0 || length > 63) return v;
  const char* p = text;
  if (*p == 'v' || *p == 'V') ++p;
  for (size_t i = 0; i < 4; ++i) {
    if (!number(p, v.part[i])) return v;
    if (*p != '.') break;
    if (i == 3) return v;
    ++p;
  }
  v.rank = 2;
  if (*p == '-') {
    ++p;
    v.rank = 0;
    if ((p[0] == 'r' || p[0] == 'R') && (p[1] == 'c' || p[1] == 'C')) {
      p += 2;
      v.rank = 1;
      if (*p == '.' || *p == '-') {
        ++p;
        if (!digit(*p)) return v;
      }
      if (digit(*p) && !number(p, v.rc)) return v;
      if (*p && *p != '+') return v;
    } else {
      if (!*p) return v;
      while (*p && *p != '+') {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || digit(*p) || *p == '-' || *p == '.')) return v;
        ++p;
      }
    }
  }
  if (*p == '+') {
    if (std::strstr(p, "+dev") == p) v.rank = 0;
    ++p;
    if (!*p) return v;
    while (*p) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || digit(*p) || *p == '-' || *p == '.')) return v;
      ++p;
    }
  }
  v.valid = *p == '\0';
  return v;
}
inline int compare(const char* left, const char* right) {
  const auto a = parse(left), b = parse(right);
  if (!a.valid || !b.valid) return 0;
  for (size_t i = 0; i < 4; ++i) {
    if (a.part[i] != b.part[i]) return a.part[i] > b.part[i] ? 1 : -1;
  }
  if (a.rank != b.rank) return a.rank > b.rank ? 1 : -1;
  if (a.rank == 1 && a.rc != b.rc) return a.rc > b.rc ? 1 : -1;
  return 0;
}

// Releases published before the version-line correction used numbers from
// 1.6.1 through 1.8.0. Map those identifiers onto their corrected 1.5.x
// chronology when deciding whether an update is newer. This lets an installed
// legacy-numbered build migrate to the official 1.6.0 without weakening the
// normal version ordering for any future release.
inline const char* correctedLegacyVersion(const char* text) {
  if (text == nullptr) return text;
  if (std::strcmp(text, "1.6.1") == 0) return "1.5.3";
  if (std::strcmp(text, "1.7.0-rc") == 0) return "1.5.4-rc.1";
  if (std::strcmp(text, "1.7.0-rc.2") == 0) return "1.5.4-rc.2";
  if (std::strcmp(text, "1.7.1-rc.1") == 0) return "1.5.4-rc.3";
  if (std::strcmp(text, "1.7.1-rc.2") == 0) return "1.5.4-rc.4";
  if (std::strcmp(text, "1.7.2") == 0) return "1.5.4";
  if (std::strcmp(text, "1.8.0-rc") == 0) return "1.5.5-rc.1";
  if (std::strcmp(text, "1.8.0-rc-2") == 0 || std::strcmp(text, "1.8.0-rc.2") == 0 || std::strcmp(text, "1.8.0") == 0 ||
      std::strncmp(text, "1.8.0-dev+", 10) == 0)
    return "1.5.5-rc.2";
  return text;
}

inline int compareForUpdate(const char* candidate, const char* current) {
  return compare(correctedLegacyVersion(candidate), correctedLegacyVersion(current));
}
}  // namespace firmware_version
