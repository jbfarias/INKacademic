#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
struct HalFile {
  std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;
  size_t maxRead = static_cast<size_t>(-1);
  explicit operator bool() const { return bytes != nullptr; }
  size_t fileSize() const { return bytes->size(); }
  int read(void* out, size_t n) {
    if (!bytes || offset > bytes->size()) return -1;
    n = std::min({n, bytes->size() - offset, maxRead});
    std::memcpy(out, bytes->data() + offset, n);
    offset += n;
    return static_cast<int>(n);
  }
  size_t write(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes->insert(bytes->end(), p, p + n);
    return n;
  }
  bool sync() { return true; }
  void close() { bytes = nullptr; }
};
struct TestStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  unsigned opens = 0;
  size_t readLimit = static_cast<size_t>(-1);
  unsigned mutateOnOpen = 0;
  bool openFileForRead(const char*, const char* path, HalFile& f) {
    auto i = files.find(path);
    if (i == files.end()) return false;
    if (++opens == mutateOnOpen && i->second.size() > 1024) i->second[1024] ^= 1;
    f.maxRead = readLimit;
    f.bytes = &i->second;
    f.offset = 0;
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& f) {
    files[path].clear();
    f.bytes = &files[path];
    f.offset = 0;
    return true;
  }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool exists(const char* path) const { return files.count(path); }
};
extern TestStorage Storage;
