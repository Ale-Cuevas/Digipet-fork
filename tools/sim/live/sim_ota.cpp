// OTA in the simulator: the sim has no network, so an update attempt walks
// the progress UI briefly and then reports a network failure -- the same
// screen a board out of Wi-Fi range shows.

#include "ota_updater.h"

OtaResult performSignedOta(const char *, const char *, OtaStatusCallback status) {
  if (status) {
    status("CONNECTING TO WIFI", 3);
    delay(1200);
  }
  return OtaResult::NetworkFailed;
}

// Same strings as src/ota_updater.cpp, so the result screen reads the same.
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
