#pragma once

#include <Log.hpp>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <soc/soc_caps.h>

#include <cstdint>
#include <cstring>

// The certificate bundle embedded by the mbedtls component
extern const uint8_t x509CrtBundleStart[] asm("_binary_x509_crt_bundle_start");    // NOLINT(hicpp-no-assembler)
extern const uint8_t x509CrtBundleEnd[] asm("_binary_x509_crt_bundle_end");        // NOLINT(hicpp-no-assembler)

namespace cornucopia::ugly_duckling::kernel {

/**
 * @brief Installs a copy of the embedded CA bundle in internal RAM for the lifetime of this object.
 *
 * Works around ESP32-C6 erratum CPU-863 (DIG-694): a word-crossing misaligned load followed
 * within two instructions by a store to a region with different PMP permissions raises a
 * spurious "Load access fault". IDF v6.1's esp_crt_bundle.c reads the byte-packed bundle via
 * `uint16_t*` casts, and the embedded bundle is not aligned in flash, so a CA lookup during
 * the TLS handshake can crash (see #643). With the bundle in RW DRAM, the load and the
 * subsequent store share permissions and the erratum cannot trigger.
 *
 * Fixed upstream in esp-idf 9c3a553 ("read crt bundle byte-wise to avoid misaligned flash
 * access"), which is not in the v6.1 tag. TODO: remove once we are on IDF v6.1.1.
 *
 * The HTTP updater is the only user of the bundle, so the copy only needs to live for the
 * duration of an OTA. Falls back to the flash-resident bundle if the copy cannot be made.
 */
class RamCertBundle {
public:
    RamCertBundle() {
#if SOC_CPU_MISALIGNED_ACCESS_ON_PMP_MISMATCH_ISSUE
        size_t size = x509CrtBundleEnd - x509CrtBundleStart;
        buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (buffer == nullptr) {
            LOGW("Failed to allocate %zu bytes for CA bundle in RAM, using bundle in flash", size);
            return;
        }
        std::memcpy(buffer, x509CrtBundleStart, size);
        esp_err_t err = esp_crt_bundle_set(buffer, size);
        if (err != ESP_OK) {
            LOGW("Failed to install CA bundle from RAM (%s), using bundle in flash", esp_err_to_name(err));
            heap_caps_free(buffer);
            buffer = nullptr;
        }
#endif
    }

    ~RamCertBundle() {
        if (buffer != nullptr) {
            // Clears the active bundle so the next attach falls back to the one in flash
            esp_crt_bundle_detach(nullptr);
            heap_caps_free(buffer);
        }
    }

    RamCertBundle(const RamCertBundle&) = delete;
    RamCertBundle& operator=(const RamCertBundle&) = delete;

private:
    uint8_t* buffer = nullptr;
};

}    // namespace cornucopia::ugly_duckling::kernel
