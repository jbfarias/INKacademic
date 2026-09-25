#include <openssl/sha.h>

#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "FirmwareFlasher.h"
#include "FirmwareIdentityScanner.h"
#include "FirmwareRequestPolicy.h"
#include "FirmwareVersion.h"
#include "HalStorage.h"
#include "HttpDownloader.h"
#include "OtaSignature.h"
#include "OtaUpdater.h"
#include "ReleaseSignatures.h"

TestStorage Storage;
static esp_partition_t destination{8 * 1024 * 1024, "ota_1", 0x810000}, running{8 * 1024 * 1024, "ota_0", 0x10000};
static std::vector<uint8_t> flash(destination.size, 0xff);
static bool bootChanged = false, corruptWrite = false, readFailure = false, eraseFailure = false, writeFailure = false;
static size_t resets = 0, yields = 0, erases = 0;
void esp_task_wdt_reset() { ++resets; }
void yield() { ++yields; }
void delay(unsigned) { ++yields; }
const esp_partition_t* esp_ota_get_running_partition() { return &running; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return &destination; }
esp_err_t esp_partition_read(const esp_partition_t* p, size_t pos, void* out, size_t n) {
  if (p == &running) {
    assert(pos == 12 && n == 2);
    uint16_t chip = 9;
    std::memcpy(out, &chip, 2);
    return ESP_OK;
  }
  if (readFailure || pos + n > flash.size()) return -1;
  std::memcpy(out, flash.data() + pos, n);
  return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t*, size_t pos, const void* data, size_t n) {
  assert(pos + n <= flash.size());
  assert(n <= 4096);
  if (writeFailure) return -1;
  std::memcpy(flash.data() + pos, data, n);
  if (corruptWrite && pos == 0) flash[1024] ^= 1;
  return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t pos, size_t n) {
  assert(pos % 4096 == 0 && n == 4096);
  ++erases;
  if (eraseFailure) return -1;
  std::memset(flash.data() + pos, 0xff, n);
  return ESP_OK;
}
namespace ota_boot {
bool switchTo(const esp_partition_t*) {
  bootChanged = true;
  return true;
}
}  // namespace ota_boot
static void reset() {
  Storage = {};
  bootChanged = corruptWrite = readFailure = eraseFailure = writeFailure = false;
  erases = resets = yields = 0;
}
static std::array<uint8_t, 32> digest(const std::vector<uint8_t>& data) {
  std::array<uint8_t, 32> d{};
  SHA256(data.data(), data.size(), d.data());
  return d;
}
static std::vector<uint8_t> image(bool trailer = true, bool inkademic = true) {
  std::vector<uint8_t> v(24 + 8 + 65536, 0x5a);
  std::fill(v.begin(), v.begin() + 32, 0);
  v[0] = 0xe9;
  v[1] = 1;
  v[12] = 9;
  v[23] = trailer;
  v[28] = 0;
  v[29] = 0;
  v[30] = 1;
  v[31] = 0;
  const char marker[] = "INKADEMIC_FW_ID|device=x4-pro|version=1.9.0|";
  if (inkademic) std::memcpy(v.data() + 100, marker, sizeof(marker));
  uint8_t checksum = 0xef;
  for (size_t i = 32; i < v.size(); ++i) checksum ^= v[i];
  v.resize((v.size() + 16) & ~size_t(15), 0);
  v.back() = checksum;
  if (trailer) {
    auto d = digest(v);
    v.insert(v.end(), d.begin(), d.end());
  }
  return v;
}
static void versions() {
  using firmware_version::compare;
  for (const char* bad :
       {"", "1.8.0-rc-", "1.8.0-rc.", "2147483648.0.0", "1.8.0-rc.2147483648", "1.8.0.0.1", "1.8.0+", "1.8.0|x"})
    assert(!firmware_version::parse(bad).valid);
  assert(!firmware_version::parse(nullptr).valid);
  assert(compare("1.8.0-rc-2", "1.8.0-rc.1") > 0);
  assert(compare("1.8.0", "1.8.0-rc.99") > 0);
  assert(compare("1.8.0-rc+abc", "1.8.0-dev+x4-pro") > 0);
  assert(compare("1.8.0-x4-pro", "1.8.0") < 0);
  assert(compare("1.8.0+dev.branch", "1.8.0-rc.1") < 0);
  assert(compare("1.8.0+release", "1.8.0") == 0);
  assert(compare("1.9.0", "2147483648.0.0") == 0);  // invalid fails closed
  assert(firmware_version::parse("2147483647.0.0").valid);
  assert(firmware_version::compareForUpdate("1.6.0", "1.6.1") > 0);
  assert(firmware_version::compareForUpdate("1.6.0", "1.7.2") > 0);
  assert(firmware_version::compareForUpdate("1.6.0", "1.8.0-rc-2") > 0);
  assert(firmware_version::compareForUpdate("1.6.0", "1.8.0-dev+x4-pro") > 0);
  assert(firmware_version::compareForUpdate("1.6.1", "1.5.2") > 0);
}
static void identity() {
  const std::string valid = "INKADEMIC_FW_ID|device=x4-pro|version=1.8.0-rc-2|";
  const std::vector<std::string> noise = {std::string(7000, 'x'),
                                          "INKADEMIC_FW_ID|device=",
                                          "INKADEMIC_FW_ID|device=|version=1.8.0|",
                                          "INKADEMIC_FW_ID|device=x4-pro|version=2147483648.0.0|",
                                          "INKADEMIC_FW_ID|device=" + std::string(256, 'a') + "|version=1.0.0|",
                                          "INKADEMIC_FW_ID|device=x4-pro|version=" + std::string(400, 'x') + "|"};
  for (const auto& n : noise)
    for (size_t chunk : {1u, 14u, 4096u}) {
      firmware_identity::Scanner scan;
      const auto bytes = n + std::string(1, '\0') + valid;
      for (size_t p = 0; p < bytes.size(); p += chunk)
        for (size_t i = p; i < std::min(p + chunk, bytes.size()); ++i) scan.feed(bytes[i]);
      assert(scan.found());
      assert(std::strcmp(scan.device(), "x4-pro") == 0);
      assert(std::strcmp(scan.version(), "1.8.0-rc-2") == 0);
    }
  firmware_identity::Scanner partial;
  for (char c : valid.substr(0, valid.size() - 1)) partial.feed(c);
  assert(!partial.found());
}
static void requests() {
  using namespace firmware_request;
  assert(allowed("http://reader.local", "reader.local", "1"));
  assert(allowed("http://10.0.0.1:80", "10.0.0.1:80", "1"));
  assert(allowed("", "reader.local", "1"));
  for (const auto origin : {"https://evil.test", "http://reader.local.evil.test", "null", "http://reader.local/"})
    assert(!allowed(origin, "reader.local", "1"));
  assert(!allowed("", "reader.local", ""));
  assert(!allowed("http://reader.local", "reader.local", ""));
  size_t size = 0;
  assert(parseSize("65536", size) && size == 65536);
  for (const char* bad : {"", "-1", "+1", " 1", "oops", "1.0", "18446744073709551616"}) assert(!parseSize(bad, size));
  UploadRequest request;
  assert(!request.accepted());
  assert(request.start());
  request.reject("busy");
  assert(!request.accepted());
  assert(request.error());  // WRITE/END/ABORT cannot own staging
  assert(!request.start());
  assert(!request.accepted());  // second multipart cannot clear rejection
  request.finish();
  assert(!request.accepted());
  assert(!request.error());
  assert(request.start());
  request.accept();
  assert(request.accepted());
  auto completed = request;
  request.finish();
  assert(completed.accepted());
  assert(!request.accepted());
  assert(request.start());
  request.accept();
  assert(!request.start());
  assert(!request.accepted());
}
static void signatures() {
  for (const auto& sample : kReleaseSignatures) {
    assert(ota_signature::verify(sample.digest, sample.signature, 64));
    auto changed = sample;
    changed.digest[7] ^= 1;
    assert(!ota_signature::verify(changed.digest, changed.signature, 64));
    changed = sample;
    changed.signature[3] ^= 1;
    assert(!ota_signature::verify(changed.digest, changed.signature, 64));
    for (size_t n : {0u, 1u, 63u, 65u}) assert(!ota_signature::verify(sample.digest, sample.signature, n));
  }
  assert(!ota_signature::verify(nullptr, kReleaseSignatures[0].signature, 64));
}
static void flashing() {
  using namespace firmware_flash;
  for (bool trailer : {false, true}) {
    reset();
    Storage.files["fw"] = image(trailer);
    auto d = digest(Storage.files["fw"]);
    uint8_t actual[32];
    assert(validateImageFile("fw", destination.size, actual) == Result::OK);
    assert(std::memcmp(actual, d.data(), 32) == 0);
    assert(flashFromSdPath("fw", nullptr, nullptr) == Result::OK);
    assert(bootChanged && erases > 16 && resets > erases && yields > erases);
    assert(std::equal(Storage.files["fw"].begin(), Storage.files["fw"].end(), flash.begin()));
  }
  reset();
  Storage.files["fw"] = image();
  Storage.readLimit = 64;  // header succeeds; a short body read must abort before erasing.
  assert(flashFromSdPath("fw", nullptr, nullptr) == Result::READ_FAIL);
  assert(!bootChanged && erases == 0);
  // Preserve user-controlled SD migration: no INKademic identity or signature required.
  reset();
  Storage.files["foreign-fw"] = image(true, false);
  assert(flashFromSdPath("foreign-fw", nullptr, nullptr) == Result::OK);
  assert(bootChanged);
  reset();
  Storage.files["fw"] = image();
  Storage.mutateOnOpen = 2;
  assert(flashFromSdPath("fw", nullptr, nullptr) == Result::BAD_SHA);
  assert(!bootChanged);
  reset();
  Storage.files["fw"] = image();
  auto d = digest(Storage.files["fw"]);
  Storage.files["fw"][1024] ^= 1;
  assert(flashFromSdPath("fw", nullptr, nullptr, true, d.data()) == Result::BAD_SHA);
  assert(!bootChanged);
  for (int failure = 0; failure < 4; ++failure) {
    reset();
    Storage.files["fw"] = image();
    corruptWrite = failure == 0;
    readFailure = failure == 1;
    eraseFailure = failure == 2;
    writeFailure = failure == 3;
    assert(flashFromSdPath("fw", nullptr, nullptr) != Result::OK);
    assert(!bootChanged);
  }
  reset();
  Storage.files["fw"] = image();
  Storage.files["fw"][0] = 0;
  assert(flashFromSdPath("fw", nullptr, nullptr) == Result::BAD_MAGIC);
  assert(!bootChanged && erases == 0);
  reset();
  Storage.files["fw"] = image();
  Storage.files["fw"].back() ^= 1;
  assert(flashFromSdPath("fw", nullptr, nullptr) == Result::BAD_SHA);
  assert(!bootChanged && erases == 0);
  reset();
  Storage.files["fw"] = image();
  char dev[32], ver[64];
  assert(validateBrowserImageFile("fw", destination.size, "x4-pro", "1.8.0", "sig", dev, 32, ver, 64) ==
         Result::SIGNATURE_MISSING);
  Storage.files["sig"] = std::vector<uint8_t>(64, 0);
  assert(validateBrowserImageFile("fw", destination.size, "x4-pro", "1.8.0", "sig", dev, 32, ver, 64) ==
         Result::SIGNATURE_INVALID);
  assert(validateBrowserImageFile("fw", destination.size, "sticky", "1.8.0", "sig", dev, 32, ver, 64) ==
         Result::BAD_TARGET);
  assert(validateBrowserImageFile("fw", destination.size, "x4-pro", "1.9.0", "sig", dev, 32, ver, 64) ==
         Result::BAD_VERSION);
}
static std::vector<uint8_t> networkBody;
static HttpDownloader::DownloadError networkResult = HttpDownloader::OK;
HttpDownloader::DownloadError HttpDownloader::streamUrl(const std::string& url, const DataCallback& sink,
                                                        ProgressCallback progress, const std::string&,
                                                        const std::string&, DownloadOptions options) {
  assert(firmware_request::validManualUrl(url));
  assert(options.transport == Transport::ESP_HTTP);  // Never bypass CA checks for unpinned manual HTTPS.
  for (size_t offset = 0; offset < networkBody.size();) {
    if (options.shouldCancel && options.shouldCancel()) return ABORTED;
    const size_t count = std::min(size_t(4096), networkBody.size() - offset);
    if (!sink(networkBody.data() + offset, count)) return ABORTED;
    offset += count;
    if (progress) progress(offset, networkBody.size());
  }
  return networkResult;
}
static void manualOta() {
  reset();
  networkBody = image(true, false);
  networkResult = HttpDownloader::OK;
  OtaUpdater updater;
  assert(updater.loadSavedSource() == OtaUpdater::OK && !updater.isManualSource());
  for (const char* bad : {"file:///firmware.bin", "https://", "https://user:pass@example.com/fw",
                          "https://example.com/\nfw", "https://example.com/fw#part", "javascript:x"})
    assert(!updater.setManualSource(bad));
  const std::string url = "https://example.com/other-firmware.bin";
  Storage.files[OtaUpdater::MANUAL_SOURCE_PATH] = {url.begin(), url.end()};
  assert(updater.loadSavedSource() == OtaUpdater::OK && updater.isManualSource());
  assert(updater.getLatestUrl() == url);
  assert(updater.installManualUpdate(nullptr, nullptr, nullptr) == OtaUpdater::OK);
  assert(bootChanged && !Storage.exists("/.inkademic-ota-manual.bin"));
  assert(updater.getProcessedSize() == networkBody.size());
  assert(std::equal(networkBody.begin(), networkBody.end(), flash.begin()));
  reset();
  networkResult = HttpDownloader::HTTP_ERROR;
  assert(updater.installManualUpdate(nullptr, nullptr, nullptr) == OtaUpdater::HTTP_ERROR);
  assert(!bootChanged && erases == 0 && !Storage.exists("/.inkademic-ota-manual.bin"));
  reset();
  networkResult = HttpDownloader::OK;
  networkBody.back() ^= 1;
  assert(updater.installManualUpdate(nullptr, nullptr, nullptr) == OtaUpdater::HASH_MISMATCH_ERROR);
  assert(!bootChanged && erases == 0);
  reset();
  networkBody = image(true, false);
  networkBody[12] = 5;
  assert(updater.installManualUpdate(nullptr, nullptr, nullptr) == OtaUpdater::WRONG_DEVICE_ERROR);
  assert(!bootChanged && erases == 0);
  reset();
  networkBody.assign(destination.size + 1, 0);
  assert(updater.installManualUpdate(nullptr, nullptr, nullptr) == OtaUpdater::HTTP_ERROR);
  assert(!bootChanged && erases == 0 && !Storage.exists("/.inkademic-ota-manual.bin"));
  reset();
  networkBody = image(true, false);
  std::atomic<bool> cancel{true};
  assert(updater.installManualUpdate(nullptr, nullptr, &cancel) == OtaUpdater::CANCELLED_ERROR);
  assert(!bootChanged && erases == 0);
  assert(updater.loadSavedSource() == OtaUpdater::OK && !updater.isManualSource());
  Storage.files[OtaUpdater::MANUAL_SOURCE_PATH] = std::vector<uint8_t>(1025, 'x');
  assert(updater.loadSavedSource() == OtaUpdater::JSON_PARSE_ERROR);
  reset();
  Storage.files["fw"] = image(true, false);
  char dev[32], version[64];
  uint8_t d[32];
  assert(firmware_flash::validateBrowserImageFile(
             "fw", destination.size, "x4-pro", "1.8.0", nullptr, dev, sizeof(dev), version, sizeof(version), d,
             firmware_flash::ValidationPolicy::Manual) == firmware_flash::Result::OK);
  assert(dev[0] == '\0' && version[0] == '\0');
  assert(firmware_flash::flashFromSdPath("fw", nullptr, nullptr, true, d) == firmware_flash::Result::OK && bootChanged);
  bootChanged = false;
  Storage.files["fw"][1024] ^= 1;
  assert(firmware_flash::flashFromSdPath("fw", nullptr, nullptr, true, d) == firmware_flash::Result::BAD_SHA &&
         !bootChanged);
}
static std::vector<uint8_t> read(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  assert(f.good());
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void published(const char* dir) {
  using namespace firmware_flash;
  reset();
  Storage.files["fw"] = read(std::string(dir) + "/rc2-x4-pro.bin");
  Storage.files["sig"] = read(std::string(dir) + "/rc2-x4-pro.bin.sig");
  char dev[32], ver[64];
  uint8_t signedDigest[32];
  assert(validateBrowserImageFile("fw", destination.size, "x4-pro", "1.7.0", "sig", dev, 32, ver, 64, signedDigest) ==
         Result::OK);
  assert(flashFromSdPath("fw", nullptr, nullptr, true, signedDigest) == Result::OK);
  assert(bootChanged);
  bootChanged = false;
  Storage.files["fw"][1024] ^= 1;
  assert(flashFromSdPath("fw", nullptr, nullptr, true, signedDigest) == Result::BAD_SHA);
  assert(!bootChanged);
  std::cout << "Published signed X4 Pro image: browser validation, flash readback, SD substitution rejection passed\n";
}
int main(int argc, char** argv) {
  versions();
  identity();
  requests();
  signatures();
  flashing();
  manualOta();
  if (argc > 1) published(argv[1]);
  std::cout << "Firmware security regressions passed; verifier key stack size=" << sizeof(ed25519_key) << " bytes\n";
}
