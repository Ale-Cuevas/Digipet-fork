#include "ota_updater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <memory>
#include <vector>

#ifndef DIGIPET_VERSION
#define DIGIPET_VERSION "0.0.0-dev"
#endif

// GitHub "owner/repo" whose releases this firmware updates from. Must match
// the repo holding the private key for kPublicKey below.
#ifndef DIGIPET_OTA_REPO
#define DIGIPET_OTA_REPO "Ale-Cuevas/Digipet-fork"
#endif

namespace {
constexpr char kManifestUrl[] =
    "https://github.com/" DIGIPET_OTA_REPO "/releases/latest/download/"
    "digipet-manifest.json";
constexpr char kSignatureUrl[] =
    "https://github.com/" DIGIPET_OTA_REPO "/releases/latest/download/"
    "digipet-manifest.sig";
constexpr char kTarget[] = "waveshare-esp32-s3-touch-amoled-1.8-v2";
// Must stay byte-identical to release/firmware-signing-public.pem; CI checks.
constexpr char kPublicKey[] = R"KEY(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEb80x2OBRpIPT/thjjfM/3yGiGaeS
177bpRfqVjgTRMkU2mRW5y3ImyyZI0s0r0IlFRDNARiyPiJv8j+ecIhFoA==
-----END PUBLIC KEY-----
)KEY";

bool connectWifi(const char *ssid, const char *password,
                 const OtaStatusCallback &status) {
  if (!ssid || !ssid[0] || !password || !password[0]) return false;
  status("CONNECTING TO WIFI", 3);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

void stopWifi() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

bool getAsset(const char *url, std::vector<uint8_t> &output, size_t maximum) {
  NetworkClientSecure client;
  // Authenticity does not depend on the transport certificate: the manifest
  // must pass the embedded ECDSA key before any metadata is trusted.
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const int declared = http.getSize();
  if (declared > 0 && static_cast<size_t>(declared) > maximum) {
    http.end();
    return false;
  }
  NetworkClient *stream = http.getStreamPtr();
  output.clear();
  if (declared > 0) output.reserve(declared);
  uint8_t buffer[512];
  uint32_t lastData = millis();
  while (http.connected() && (declared < 0 || output.size() < static_cast<size_t>(declared))) {
    const size_t available = stream->available();
    if (available) {
      const size_t count = stream->readBytes(buffer, min(available, sizeof(buffer)));
      if (!count || output.size() + count > maximum) {
        http.end();
        return false;
      }
      output.insert(output.end(), buffer, buffer + count);
      lastData = millis();
    } else if (millis() - lastData > 10000) {
      http.end();
      return false;
    } else {
      delay(10);
    }
  }
  http.end();
  return !output.empty() && (declared < 0 || output.size() == static_cast<size_t>(declared));
}

bool verifyManifest(const std::vector<uint8_t> &manifest,
                    const std::vector<uint8_t> &signature) {
  uint8_t digest[32];
  mbedtls_sha256(manifest.data(), manifest.size(), digest, 0);
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parsed = mbedtls_pk_parse_public_key(
      &key, reinterpret_cast<const uint8_t *>(kPublicKey), sizeof(kPublicKey));
  const int verified = parsed == 0
      ? mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                          signature.data(), signature.size())
      : parsed;
  mbedtls_pk_free(&key);
  return verified == 0;
}

int compareVersions(const char *left, const char *right) {
  for (uint8_t part = 0; part < 3; ++part) {
    char *leftEnd = nullptr;
    char *rightEnd = nullptr;
    const long a = strtol(left, &leftEnd, 10);
    const long b = strtol(right, &rightEnd, 10);
    left = leftEnd;
    right = rightEnd;
    if (a != b) return a < b ? -1 : 1;
    if (*left == '.') ++left;
    if (*right == '.') ++right;
  }
  return 0;
}

bool hexDigest(const char *hex, uint8_t output[32]) {
  if (!hex || strlen(hex) != 64) return false;
  for (uint8_t i = 0; i < 32; ++i) {
    char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    char *end = nullptr;
    output[i] = strtoul(pair, &end, 16);
    if (!end || *end) return false;
  }
  return true;
}

OtaResult downloadAndInstall(const char *url, size_t expectedSize,
                             const uint8_t expectedSha[32],
                             const OtaStatusCallback &status) {
  Serial.printf("OTA: download start, size=%u, free heap=%u, stack=%u\n",
                static_cast<unsigned>(expectedSize), ESP.getFreeHeap(),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    Serial.println("OTA: HTTP begin failed");
    return OtaResult::DownloadFailed;
  }
  const int responseCode = http.GET();
  Serial.printf("OTA: firmware HTTP=%d, declared=%d, free heap=%u, stack=%u\n",
                responseCode, http.getSize(), ESP.getFreeHeap(),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  if (responseCode != HTTP_CODE_OK) {
    http.end();
    return OtaResult::DownloadFailed;
  }
  if (expectedSize == 0 || http.getSize() != static_cast<int>(expectedSize)) {
    Serial.println("OTA: invalid firmware size");
    http.end();
    return OtaResult::InstallFailed;
  }
  if (!Update.begin(expectedSize, U_FLASH)) {
    Serial.printf("OTA: Update.begin failed, error=%u (%s)\n",
                  Update.getError(), Update.errorString());
    http.end();
    return OtaResult::InstallFailed;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  NetworkClient *stream = http.getStreamPtr();
  // Keep the transfer buffer off the loop task's small stack. TLS, HTTP,
  // hashing, and UI rendering are all active in this call chain.
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[4096]);
  if (!buffer) {
    Serial.println("OTA: transfer buffer allocation failed");
    Update.abort();
    mbedtls_sha256_free(&sha);
    http.end();
    return OtaResult::InstallFailed;
  }
  size_t received = 0;
  int lastProgress = -1;
  uint32_t lastData = millis();
  while (received < expectedSize && http.connected()) {
    const size_t available = stream->available();
    if (available) {
      const size_t wanted = std::min(available,
          std::min(static_cast<size_t>(4096), expectedSize - received));
      const size_t count = stream->readBytes(buffer.get(), wanted);
      if (!count || Update.write(buffer.get(), count) != count) {
        Serial.printf("OTA: write failed at %u, error=%u (%s)\n",
                      static_cast<unsigned>(received), Update.getError(),
                      Update.errorString());
        Update.abort();
        mbedtls_sha256_free(&sha);
        http.end();
        return OtaResult::InstallFailed;
      }
      mbedtls_sha256_update(&sha, buffer.get(), count);
      received += count;
      lastData = millis();
      const int progress = 20 + static_cast<int>(received * 75 / expectedSize);
      if (progress >= lastProgress + 5) {
        lastProgress = progress;
        status("INSTALLING SIGNED UPDATE", progress);
      }
    } else if (millis() - lastData > 15000) {
      Update.abort();
      mbedtls_sha256_free(&sha);
      http.end();
      return OtaResult::DownloadFailed;
    } else {
      delay(10);
    }
  }
  uint8_t actualSha[32];
  mbedtls_sha256_finish(&sha, actualSha);
  mbedtls_sha256_free(&sha);
  http.end();
  if (received != expectedSize || memcmp(actualSha, expectedSha, 32) != 0) {
    Serial.printf("OTA: hash/length rejected, received=%u\n",
                  static_cast<unsigned>(received));
    Update.abort();
    return OtaResult::ImageInvalid;
  }
  if (!Update.end(false) || !Update.isFinished()) {
    Serial.printf("OTA: Update.end failed, error=%u (%s)\n",
                  Update.getError(), Update.errorString());
    Update.abort();
    return OtaResult::InstallFailed;
  }
  Serial.println("OTA: image verified and activated");
  status("UPDATE VERIFIED // REBOOTING", 100);
  delay(1200);
  ESP.restart();
  return OtaResult::Updated;
}
}  // namespace

OtaResult performSignedOta(const char *ssid, const char *password,
                           OtaStatusCallback status) {
  if (!ssid || !ssid[0] || !password || !password[0]) {
    return OtaResult::NoConfiguration;
  }
  if (!connectWifi(ssid, password, status)) {
    stopWifi();
    return OtaResult::NetworkFailed;
  }
  status("FETCHING RELEASE MANIFEST", 8);
  std::vector<uint8_t> manifest;
  std::vector<uint8_t> signature;
  if (!getAsset(kManifestUrl, manifest, 8192) ||
      !getAsset(kSignatureUrl, signature, 256)) {
    stopWifi();
    return OtaResult::ManifestFailed;
  }
  status("VERIFYING RELEASE SIGNATURE", 14);
  if (!verifyManifest(manifest, signature)) {
    stopWifi();
    return OtaResult::SignatureFailed;
  }

  JsonDocument document;
  const DeserializationError jsonError =
      deserializeJson(document, manifest.data(), manifest.size());
  const char *product = document["product"] | "";
  const char *target = document["target"] | "";
  const char *version = document["version"] | "";
  if (jsonError || document["schema"].as<int>() != 1 ||
      strcmp(product, "digipet") || strcmp(target, kTarget) || !version[0]) {
    stopWifi();
    return OtaResult::Incompatible;
  }
  if (compareVersions(DIGIPET_VERSION, version) >= 0) {
    stopWifi();
    return OtaResult::UpToDate;
  }

  const char *url = document["firmware"]["url"] | "";
  const char *shaHex = document["firmware"]["sha256"] | "";
  const size_t size = document["firmware"]["size"] | 0;
  uint8_t expectedSha[32];
  if (!url[0] || !hexDigest(shaHex, expectedSha)) {
    stopWifi();
    return OtaResult::Incompatible;
  }
  status((String("UPDATE ") + version + " AVAILABLE").c_str(), 18);
  const OtaResult result = downloadAndInstall(url, size, expectedSha, status);
  stopWifi();
  return result;
}

const char *otaResultMessage(OtaResult result) {
  switch (result) {
    case OtaResult::Updated: return "UPDATE INSTALLED";
    case OtaResult::UpToDate: return "ALREADY UP TO DATE";
    case OtaResult::NoConfiguration: return "NO WIFI CONFIGURATION";
    case OtaResult::NetworkFailed: return "WIFI CONNECTION FAILED";
    case OtaResult::ManifestFailed: return "NO SIGNED RELEASE FOUND";
    case OtaResult::SignatureFailed: return "SIGNATURE REJECTED";
    case OtaResult::Incompatible: return "RELEASE INCOMPATIBLE";
    case OtaResult::DownloadFailed: return "DOWNLOAD FAILED";
    case OtaResult::ImageInvalid: return "IMAGE HASH REJECTED";
    case OtaResult::InstallFailed: return "INSTALLATION FAILED";
  }
  return "UPDATE FAILED";
}
