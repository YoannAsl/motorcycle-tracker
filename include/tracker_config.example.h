#pragma once

// Copy this file to include/tracker_config.h and replace every placeholder.
// tracker_config.h is ignored.
#define HOTSPOT_NAME "replace-with-phone-hotspot-name"
#define HOTSPOT_PASSWORD "replace-with-phone-hotspot-password"
#define UPLOAD_URL "https://uploads.example/v1/track-point-batches"
#define TRACKER_ID "replace-with-stable-tracker-id"
#define TRACKER_TOKEN "replace-with-bearer-token"

// Paste the PEM-encoded root CA that validates UPLOAD_URL. The firmware does
// not have an insecure TLS mode.
#define UPLOAD_ROOT_CA_CERTIFICATE                                      \
  "-----BEGIN CERTIFICATE-----\n"                                    \
  "replace-with-root-ca-certificate\n"                               \
  "-----END CERTIFICATE-----\n"
