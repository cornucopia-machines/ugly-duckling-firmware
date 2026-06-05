#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <time.h>

#include <esp_random.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>    // NOLINT(misc-header-include-cycle) -- ble_hs.h and ble_gap.h include each other; cycle is in ESP-IDF, not our code
#include <host/ble_hs_mbuf.h>
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/bas/ble_svc_bas.h>
#include <services/cts/ble_svc_cts.h>
#include <services/dis/ble_svc_dis.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include "WifiApRecord.hpp"
#include <ArduinoJson.h>
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
//  - Ugly Duckling Service (custom, 128-bit): WiFi provisioning (scan, credentials, status, control)
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

        // Register the Ugly Duckling custom GATT service (must be before nimble_port_freertos_init)
        int rc = ble_gatts_count_cfg(gattSvcs);
        if (rc != 0) {
            LOGTE(BLE, "ble_gatts_count_cfg failed: 0x%02x", rc);
        }
        rc = ble_gatts_add_svcs(gattSvcs);
        if (rc != 0) {
            LOGTE(BLE, "ble_gatts_add_svcs failed: 0x%02x", rc);
        }

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

    void setOnWifiScanRequested(std::function<void()> callback) {
        onWifiScanRequested = std::move(callback);
    }

    void setOnWifiCredentialsReceived(std::function<void(std::string, std::string)> callback) {
        onWifiCredentialsReceived = std::move(callback);
    }

    void setOnWifiControlReceived(std::function<void(std::string)> callback) {
        onWifiControlReceived = std::move(callback);
    }

    // Called by the WiFi driver whenever the WiFi status string changes.
    // Updates the cached value and notifies the connected client (if any).
    // Schedules work on the NimBLE host task so all BLE state is single-threaded.
    void setWifiStatus(std::string newStatus) {
        LOGTD(BLE, "Publishing new WiFi status: %s", newStatus.c_str());
        postToHostTask([this, s = std::move(newStatus)]() mutable {
            wifiStatus = std::move(s);
            if (connHandle < 0) {
                return;
            }
            struct os_mbuf* om = ble_hs_mbuf_from_flat(wifiStatus.data(), wifiStatus.size());
            if (om == nullptr) {
                LOGTE(BLE, "Failed to allocate mbuf for WiFi status notification");
                return;
            }
            int rc = ble_gatts_notify_custom(static_cast<uint16_t>(connHandle), wifiStatusValHandle, om);
            if (rc != 0) {
                LOGTD(BLE, "Failed to notify WiFi status: 0x%02x", rc);
            }
        });
    }

    // Called by the WiFi driver when a BLE-triggered scan completes.
    // Schedules work on the NimBLE host task so all BLE state is single-threaded.
    //
    // Protocol: one Notify per AP (self-contained JSON object), followed by one empty
    // Notify as an end-of-stream sentinel. The client appends each AP to a list
    // and finalises on the empty sentinel — no string assembly or re-parsing needed.
    void setScanResults(std::vector<WifiApRecord> results) {
        LOGTD(BLE, "Publishing WiFi scan results (%u APs)", static_cast<unsigned>(results.size()));
        postToHostTask([this, aps = std::move(results)]() mutable {
            scanInProgress = false;
            if (connHandle < 0) {
                return;
            }
            for (const WifiApRecord& ap : aps) {
                JsonDocument doc;
                doc["ssid"] = ap.ssid;
                doc["rssi"] = ap.rssi;
                doc["authMode"] = ap.authMode;
                doc["wifiGen"] = ap.wifiGen;
                std::string apJson;
                serializeJson(doc, apJson);
                struct os_mbuf* om = ble_hs_mbuf_from_flat(apJson.data(), static_cast<uint16_t>(apJson.size()));
                if (om == nullptr) {
                    LOGTE(BLE, "Failed to allocate mbuf for AP notification");
                    return;
                }
                int rc = ble_gatts_notify_custom(static_cast<uint16_t>(connHandle), scanResultsValHandle, om);
                if (rc != 0) {
                    LOGTD(BLE, "Failed to notify AP: 0x%02x", rc);
                    return;
                }
            }
            struct os_mbuf* sentinel = ble_hs_mbuf_from_flat(nullptr, 0);
            if (sentinel != nullptr) {
                ble_gatts_notify_custom(static_cast<uint16_t>(connHandle), scanResultsValHandle, sentinel);
            }
            LOGTD(BLE, "Sent %u AP notification(s) + sentinel", static_cast<unsigned>(aps.size()));
        });
    }

private:
    // Posts a callable onto the NimBLE host task's event queue. All BLE state
    // (wifiStatus, wifiScanResults, connHandle, scanInProgress) is owned by the
    // host task, so this is the only safe way to mutate it from another task.
    static void postToHostTask(std::function<void()> fn) {
        struct Payload {
            ble_npl_event ev;
            std::function<void()> fn;
        };
        auto* payload = new Payload { .ev = {}, .fn = std::move(fn) };    // NOLINT(cppcoreguidelines-owning-memory)
        ble_npl_event_init(
            &payload->ev,
            [](ble_npl_event* ev) {
                auto* p = static_cast<Payload*>(ble_npl_event_get_arg(ev));
                p->fn();
                delete p;    // NOLINT(cppcoreguidelines-owning-memory)
            },
            payload);
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &payload->ev);
    }

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
                    driver->connHandle = static_cast<int>(event->connect.conn_handle);
                    driver->status = BleStatus::Connected;
                    LOGTD(BLE, "Client connected, handle: %d", event->connect.conn_handle);
                } else {
                    LOGTD(BLE, "Connection failed, error: 0x%02x, restarting advertising", event->connect.status);
                    driver->startAdvertising();
                }
                break;
            case BLE_GAP_EVENT_DISCONNECT:
                driver->connHandle = -1;
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
        (void) gmtime_r(&now, &t);
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
                .tm_wday = 0,    // ignored by mktime
                .tm_yday = 0,    // ignored by mktime
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

    static int scanResultsAccessCallback(uint16_t /* conn_handle */, uint16_t /* attr_handle */, struct ble_gatt_access_ctxt* /* ctxt */, void* /* arg */) {
        // Read is not supported; results are delivered exclusively via Notify.
        // A scan is triggered by writing "scan" to the WiFi Control characteristic.
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    static int wifiStatusAccessCallback(uint16_t /* conn_handle */, uint16_t /* attr_handle */, struct ble_gatt_access_ctxt* ctxt, void* /* arg */) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        int rc = os_mbuf_append(ctxt->om, instance->wifiStatus.data(), instance->wifiStatus.size());
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    static int wifiCredentialsAccessCallback(uint16_t /* conn_handle */, uint16_t /* attr_handle */, struct ble_gatt_access_ctxt* ctxt, void* /* arg */) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        // NOLINTNEXTLINE(bugprone-casting-through-void, cppcoreguidelines-pro-type-cstyle-cast, performance-no-int-to-ptr)
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        std::string json(len, '\0');
        if (ble_hs_mbuf_to_flat(ctxt->om, json.data(), len, nullptr) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        JsonDocument doc;
        if (deserializeJson(doc, json) != DeserializationError::Ok
            || !doc["ssid"].is<const char*>()
            || !doc["password"].is<const char*>()) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (instance->onWifiCredentialsReceived) {
            instance->onWifiCredentialsReceived(
                doc["ssid"].as<std::string>(),
                doc["password"].as<std::string>());
        }
        return 0;
    }

    static int wifiControlAccessCallback(uint16_t /* conn_handle */, uint16_t /* attr_handle */, struct ble_gatt_access_ctxt* ctxt, void* /* arg */) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        // NOLINTNEXTLINE(bugprone-casting-through-void, cppcoreguidelines-pro-type-cstyle-cast, performance-no-int-to-ptr)
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        std::string cmd(len, '\0');
        if (ble_hs_mbuf_to_flat(ctxt->om, cmd.data(), len, nullptr) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (cmd == "scan") {
            if (instance->onWifiScanRequested && !instance->scanInProgress) {
                instance->scanInProgress = true;
                instance->onWifiScanRequested();
            }
        } else if (instance->onWifiControlReceived) {
            instance->onWifiControlReceived(std::move(cmd));
        }
        return 0;
    }

    void startAdvertising() {
        static const std::array<ble_uuid16_t, 3> serviceUuids16 = { {
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

        // Scan response: 3 × 16-bit UUIDs (8 B) + 1 × 128-bit UUID (18 B) = 26 B of 31 B available.
        struct ble_hs_adv_fields rspFields = { };
        rspFields.uuids16 = serviceUuids16.data();
        rspFields.num_uuids16 = serviceUuids16.size();
        rspFields.uuids16_is_complete = 1;
        rspFields.uuids128 = &uglyDucklingServiceUuid;
        rspFields.num_uuids128 = 1;
        rspFields.uuids128_is_complete = 1;

        rc = ble_gap_adv_rsp_set_fields(&rspFields);
        if (rc != 0) {
            LOGTE(BLE, "Failed to set scan response fields: 0x%02x", rc);
            status = BleStatus::Error;
            return;
        }

        struct ble_gap_adv_params params = { };
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        // 2 s interval (3200 × 0.625 ms) — long enough for the CPU to enter light sleep between events
        params.itvl_min = 3200;
        params.itvl_max = 3200;

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
    std::function<void()> onWifiScanRequested;
    std::function<void(std::string, std::string)> onWifiCredentialsReceived;
    std::function<void(std::string)> onWifiControlReceived;

    BleStatus status { BleStatus::Idle };

    // All fields below are only accessed from the NimBLE host task (GAP/GATT callbacks
    // and postToHostTask closures), so no synchronisation is needed.
    int connHandle { -1 };
    bool scanInProgress { false };
    std::string wifiStatus;

    // Ugly Duckling Service — UUID: 100D32C7-A4E6-4F72-8D7A-A61871CE4FD6
    // Bytes stored little-endian (LSB first) as required by NimBLE.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_uuid128_t uglyDucklingServiceUuid = BLE_UUID128_INIT(
        0xd6, 0x4f, 0xce, 0x71, 0x18, 0xa6, 0x7a, 0x8d,
        0x72, 0x4f, 0xe6, 0xa4, 0xc7, 0x32, 0x0d, 0x10);

    // WiFi Scan Results characteristic — 16-bit UUID 0x0001 (custom, within the Ugly Duckling Service)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_uuid16_t scanResultsChrUuid = { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x0001 };
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static uint16_t scanResultsValHandle = 0;

    // WiFi Status characteristic — 16-bit UUID 0x0002 (custom, within the Ugly Duckling Service)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_uuid16_t wifiStatusChrUuid = { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x0002 };
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static uint16_t wifiStatusValHandle = 0;

    // WiFi Credentials characteristic — 16-bit UUID 0x0003 (custom, within the Ugly Duckling Service)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_uuid16_t wifiCredentialsChrUuid = { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x0003 };

    // WiFi Control characteristic — 16-bit UUID 0x0004 (custom, within the Ugly Duckling Service)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_uuid16_t wifiControlChrUuid = { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x0004 };

    // Characteristic User Description descriptor — standard UUID 0x2901; read by LightBlue/nRF Connect
    // to display a human-readable label next to each custom characteristic UUID.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_uuid16_t userDescUuid = { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x2901 };

    static int userDescAccessCallback(uint16_t /* conn_handle */, uint16_t /* attr_handle */, struct ble_gatt_access_ctxt* ctxt, void* arg) {
        const char* label = static_cast<const char*>(arg);
        int rc = os_mbuf_append(ctxt->om, label, strlen(label));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    // Non-const storage required because ble_gatt_dsc_def::arg is void* (no const overload).
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static char scanResultsLabel[] = "WiFi Scan Results";
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static char wifiStatusLabel[] = "WiFi Status";
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static char wifiCredentialsLabel[] = "WiFi Credentials";
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static char wifiControlLabel[] = "WiFi Control";

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_gatt_dsc_def scanResultsDscs[] = {
        { .uuid = &userDescUuid.u, .att_flags = BLE_ATT_F_READ, .min_key_size = 0, .access_cb = userDescAccessCallback, .arg = scanResultsLabel },
        { },
    };
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_gatt_dsc_def wifiStatusDscs[] = {
        { .uuid = &userDescUuid.u, .att_flags = BLE_ATT_F_READ, .min_key_size = 0, .access_cb = userDescAccessCallback, .arg = wifiStatusLabel },
        { },
    };
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_gatt_dsc_def wifiCredentialsDscs[] = {
        { .uuid = &userDescUuid.u, .att_flags = BLE_ATT_F_READ, .min_key_size = 0, .access_cb = userDescAccessCallback, .arg = wifiCredentialsLabel },
        { },
    };
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_gatt_dsc_def wifiControlDscs[] = {
        { .uuid = &userDescUuid.u, .att_flags = BLE_ATT_F_READ, .min_key_size = 0, .access_cb = userDescAccessCallback, .arg = wifiControlLabel },
        { },
    };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_gatt_chr_def uglyDucklingChrs[] = {
        {
            .uuid = &scanResultsChrUuid.u,
            .access_cb = scanResultsAccessCallback,
            .arg = nullptr,
            .descriptors = scanResultsDscs,
            .flags = BLE_GATT_CHR_F_NOTIFY,
            .min_key_size = 0,
            .val_handle = &scanResultsValHandle,
            .cpfd = nullptr,
        },
        {
            .uuid = &wifiStatusChrUuid.u,
            .access_cb = wifiStatusAccessCallback,
            .arg = nullptr,
            .descriptors = wifiStatusDscs,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .min_key_size = 0,
            .val_handle = &wifiStatusValHandle,
            .cpfd = nullptr,
        },
        {
            .uuid = &wifiCredentialsChrUuid.u,
            .access_cb = wifiCredentialsAccessCallback,
            .arg = nullptr,
            .descriptors = wifiCredentialsDscs,
            .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            .min_key_size = 0,
            .val_handle = nullptr,
            .cpfd = nullptr,
        },
        {
            .uuid = &wifiControlChrUuid.u,
            .access_cb = wifiControlAccessCallback,
            .arg = nullptr,
            .descriptors = wifiControlDscs,
            .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            .min_key_size = 0,
            .val_handle = nullptr,
            .cpfd = nullptr,
        },
        { },    // terminator
    };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static ble_gatt_svc_def gattSvcs[] = {
        {
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &uglyDucklingServiceUuid.u,
            .includes = nullptr,
            .characteristics = uglyDucklingChrs,
        },
        { },    // terminator
    };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static BleDriver* instance = nullptr;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
