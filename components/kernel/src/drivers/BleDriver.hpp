#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <time.h>

#include <esp_random.h>
#include <host/ble_hs.h>    // NOLINT(misc-header-include-cycle) -- ble_hs.h and ble_gap.h include each other; cycle is in ESP-IDF, not our code
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/bas/ble_svc_bas.h>
#include <services/cts/ble_svc_cts.h>
#include <services/dis/ble_svc_dis.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include <Log.hpp>
#include <NvsStore.hpp>

namespace cornucopia::ugly_duckling::kernel::drivers {

LOGGING_TAG(BLE, "ble")

enum class BleStatus : std::uint8_t {
    Idle,
    Resetting,
    Error,
    Advertising,
    Connected
};

// Starts NimBLE, advertises the device, and hosts standard GATT services readable with any BLE
// scanner app (nRF Connect, LightBlue, etc.) without a custom client:
//  - Device Information Service (DIS, 0x180A): model, firmware version, serial number
//  - Battery Service     (BAS, 0x180F): battery level, updated via setBatteryLevel()
//  - Current Time Service (CTS, 0x1805): current time; a central can push the time on connect
class BleDriver final {
public:
    BleDriver(
        const std::string& deviceName,
        const std::string& modelName,
        const std::string& firmwareVersion,
        const std::string& serialNumber,
        const std::shared_ptr<NvsStore>& nvs)
        : deviceName(deviceName)
        , modelName(modelName)
        , firmwareVersion(firmwareVersion)
        , serialNumber(serialNumber)
        , bleAddr(loadOrGenerateAddress(*nvs)) {
        instance = this;

        nimble_port_init();

        ble_hs_cfg.reset_cb = onReset;
        ble_hs_cfg.sync_cb = onSync;

        // Order matters: GAP and GATT must be initialized before other services
        ble_svc_gap_init();
        ble_svc_gatt_init();
        ble_svc_dis_init();
        ble_svc_bas_init();
        ble_svc_cts_init({
            .fetch_time_cb = ctsGetTime,
            .local_time_info_cb = ctsGetLocalTimeInfo,
            .ref_time_info_cb = ctsGetRefTimeInfo,
            .set_time_cb = ctsSetTime,
            .set_local_time_info_cb = ctsSetLocalTimeInfo,
        });

        // NimBLE DIS stores these pointers directly (no copy), so the strings must remain alive
        // for the device lifetime. They are stored as members below.
        ble_svc_gap_device_name_set(this->deviceName.c_str());
        ble_svc_dis_manufacturer_name_set("Cornucopia Machines");
        ble_svc_dis_model_number_set(this->modelName.c_str());
        ble_svc_dis_firmware_revision_set(this->firmwareVersion.c_str());
        ble_svc_dis_serial_number_set(this->serialNumber.c_str());

        nimble_port_freertos_init(hostTask);

        LOGTD(BLE, "Initialized, will advertise as '%s'", this->deviceName.c_str());
    }

    BleStatus getStatus() {
        return status;
    }

    static void setBatteryLevel(uint8_t percent) {
        ble_svc_bas_battery_level_set(percent);
    }

    void setOnTimeReceived(std::function<void(time_t)> callback) {
        onTimeReceived = std::move(callback);
    }

private:
    static std::array<uint8_t, 6> loadOrGenerateAddress(NvsStore& nvs) {
        std::array<uint8_t, 6> addr { };
        JsonDocument doc;
        if (nvs.getJson("addr", doc) && doc.is<JsonArray>() && doc.as<JsonArray>().size() == 6) {
            auto arr = doc.as<JsonArray>();
            for (size_t i = 0; i < 6; i++) {
                addr[i] = arr[i].as<uint8_t>();
            }
            LOGTD(BLE, "Loaded BLE address from NVS");
            return addr;
        }
        // Generate a static random address: fill with random bytes, then set the top 2 bits of
        // the MSB to 11 as required by the Bluetooth spec for static random addresses.
        esp_fill_random(addr.data(), addr.size());
        addr[5] |= 0xC0;
        JsonDocument newDoc;
        auto arr = newDoc.to<JsonArray>();
        for (auto byte : addr) {
            arr.add(byte);
        }
        nvs.setJson("addr", newDoc.as<JsonVariantConst>());
        LOGTD(BLE, "Generated and stored new BLE address");
        return addr;
    }

    static void onSync() {
        instance->handleSync();
    }

    static void onReset(int reason) {
        instance->handleReset(reason);
    }

    static void hostTask(void* /* param */) {
        nimble_port_run();
        nimble_port_freertos_deinit();
    }

    void handleSync() {
        ble_hs_id_set_rnd(bleAddr.data());
        startAdvertising();
    }

    void handleReset(int reason) {
        status = BleStatus::Resetting;
        LOGTE(BLE, "Resetting state; reason: 0x%02x", reason);
    }

    static int gapEventCallback(struct ble_gap_event* event, void* driverp) {
        auto* driver = static_cast<BleDriver*>(driverp);
        switch (event->type) {
            case BLE_GAP_EVENT_CONNECT:
                if (event->connect.status == 0) {
                    driver->status = BleStatus::Connected;
                    LOGTD(BLE, "Client connected, handle: %d", event->connect.conn_handle);
                } else {
                    LOGTD(BLE, "Connection failed, error: 0x%02x, restarting advertising", event->connect.status);
                    driver->startAdvertising();
                }
                break;
            case BLE_GAP_EVENT_DISCONNECT:
                LOGTD(BLE, "Client disconnected, reason: 0x%02x, restarting advertising",
                    event->disconnect.reason);
                driver->startAdvertising();
                break;
            default:
                break;
        }
        return 0;
    }

    static int ctsGetTime(struct ble_svc_cts_curr_time* ct) {
        time_t now = system_clock::to_time_t(system_clock::now());
        struct tm t {};
        gmtime_r(&now, &t);
        ct->et_256.d_d_t.d_t = {
            .year = static_cast<uint16_t>(t.tm_year + 1900),
            .month = static_cast<uint8_t>(t.tm_mon + 1),
            .day = static_cast<uint8_t>(t.tm_mday),
            .hours = static_cast<uint8_t>(t.tm_hour),
            .minutes = static_cast<uint8_t>(t.tm_min),
            .seconds = static_cast<uint8_t>(t.tm_sec),
        };
        // CTS day-of-week: 1=Monday … 7=Sunday; tm_wday: 0=Sunday … 6=Saturday
        ct->et_256.d_d_t.day_of_week = (t.tm_wday == 0) ? 7 : static_cast<uint8_t>(t.tm_wday);
        ct->et_256.fractions_256 = 0;
        ct->adjust_reason = 0;
        return 0;
    }

    static int ctsSetTime(struct ble_svc_cts_curr_time ct) {
        if (instance->onTimeReceived) {
            const auto& d = ct.et_256.d_d_t.d_t;
            struct tm t = {
                .tm_sec = d.seconds,
                .tm_min = d.minutes,
                .tm_hour = d.hours,
                .tm_mday = d.day,
                .tm_mon = d.month - 1,
                .tm_year = d.year - 1900,
                .tm_isdst = 0,
            };
            // mktime treats tm as local time; on ESP-IDF TZ defaults to UTC so this is correct
            instance->onTimeReceived(mktime(&t));
        }
        return 0;
    }

    static int ctsGetLocalTimeInfo(struct ble_svc_cts_local_time_info* lti) {
        lti->timezone = 0;                  // UTC (units of 15 min)
        lti->dst_offset = TIME_STANDARD;    // no DST
        return 0;
    }

    static int ctsGetRefTimeInfo(struct ble_svc_cts_reference_time_info* rti) {
        rti->time_source = TIME_SOURCE_UNKNOWN;
        rti->time_accuracy = 254;           // unknown
        rti->days_since_update = 0;
        rti->hours_since_update = 0;
        return 0;
    }

    static int ctsSetLocalTimeInfo(struct ble_svc_cts_local_time_info /* lti */) {
        return 0;    // timezone management not supported
    }

    void startAdvertising() {
        static const std::array<ble_uuid16_t, 3> serviceUuids = { {
            { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x180A },
            { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x180F },
            { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x1805 },
        } };

        // Primary ad: flags + name (26 bytes available after flags' 3B + name header 2B).
        const char* name = ble_svc_gap_device_name();
        struct ble_hs_adv_fields adFields = { };
        adFields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        adFields.name = reinterpret_cast<const uint8_t*>(name);
        adFields.name_len = static_cast<uint8_t>(strnlen(name, 26));
        adFields.name_is_complete = (strnlen(name, 27) <= 26) ? 1 : 0;

        int rc = ble_gap_adv_set_fields(&adFields);
        if (rc != 0) {
            LOGTE(BLE, "Failed to set advertising fields: 0x%02x", rc);
            status = BleStatus::Error;
            return;
        }

        // Scan response: service UUIDs (so they don't compete with the name for PDU space).
        struct ble_hs_adv_fields rspFields = { };
        rspFields.uuids16 = serviceUuids.data();
        rspFields.num_uuids16 = serviceUuids.size();
        rspFields.uuids16_is_complete = 1;

        rc = ble_gap_adv_rsp_set_fields(&rspFields);
        if (rc != 0) {
            LOGTE(BLE, "Failed to set scan response fields: 0x%02x", rc);
            status = BleStatus::Error;
            return;
        }

        struct ble_gap_adv_params params = { };
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;

        rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER, &params, gapEventCallback, this);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            LOGTE(BLE, "Failed to start advertising: 0x%02x", rc);
            status = BleStatus::Error;
            return;
        }
        status = BleStatus::Advertising;
        LOGTD(BLE, "Advertising as '%s'", name);
    }

    const std::string deviceName;
    const std::string modelName;
    const std::string firmwareVersion;
    const std::string serialNumber;
    const std::array<uint8_t, 6> bleAddr;

    std::function<void(time_t)> onTimeReceived;

    BleStatus status { BleStatus::Idle };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static BleDriver* instance = nullptr;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
