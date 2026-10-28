#pragma once

// Only the one-shot form generatePlayerId() uses; real SHA-256 in sim_hal.cpp.

#include <cstddef>

int mbedtls_sha256(const unsigned char *input, size_t length, unsigned char output[32],
                   int is224);
