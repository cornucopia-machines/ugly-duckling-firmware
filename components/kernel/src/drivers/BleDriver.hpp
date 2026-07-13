#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <time.h>

#include <esp_random.h>
#include <esp_timer.h>
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
#include <EspException.hpp>
#include <Log.hpp>
#include <NvsStore.hpp>

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel::drivers {

LOGGING_TAG(BLE, "ble")

enum class BleStatus : std::uint8_t {
    Disabled,
    Idle,
    Resetting,
    Error,
    Advertising,
    Connected
};

// No-op BLE driver used when BLE is disabled at runtime (settings->bleEnabled = false).
// All methods are no-ops; getStatus() returns Disabled. Subclassed by NimBleDriver.
class BleDriver {
public:
    virtual ~BleDriver() = default;
    virtual BleStatus getStatus() { return BleStatus::Disabled; }
    virtual void setBatteryLevel(uint8_t /*unused*/) {}
    virtual void setOnTimeReceived(const std::function<void(time_t)>& /*unused*/) {}
    virtual void setOnWifiScanRequested(const std::function<void()>& /*unused*/) {}
    virtual void setOnWifiCredentialsReceived(const std::function<void(std::string, std::string)>& /*unused*/) {}
    virtual void setOnWifiControlReceived(const std::function<void(std::string)>& /*unused*/) {}
    virtual void setWifiStatus(const std::string& /*unused*/) {}
    virtual void setScanResults(const std::vector<WifiApRecord>& /*unused*/) {}
};

// Starts NimBLE, advertises the device, and hosts standard GATT services readable with any BLE
// scanner app (nRF Connect, LightBlue, etc.) without a custom client:
//  - Device Information Service (DIS, 0x180A): model, firmware version, serial number
//  - Battery Service     (BAS, 0x180F): battery level, updated via setBatteryLevel()
//  - Current Time Service (CTS, 0x1805): current time; a central can push the time on connect
//  - Ugly Duckling Service (custom, 128-bit): WiFi provisioning (scan, credentials, status, control)
class NimBleDriver final : public BleDriver {
public:
    NimBleDriver(
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

        // Errors from here through the end of the constructor throw (see throwOnBleError):
        // a device that can't stand up its GATT server or advertise isn't usable, so fail
        // fast at boot instead of continuing in a silently broken state.
        ESP_ERROR_THROW(nimble_port_init());

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
        throwOnBleError(ble_gatts_count_cfg(gattSvcs), "ble_gatts_count_cfg");
        throwOnBleError(ble_gatts_add_svcs(gattSvcs), "ble_gatts_add_svcs");

        // NimBLE DIS stores these pointers directly (no copy), so the strings must remain alive
        // for the device lifetime. They are stored as members below.
        throwOnBleError(ble_svc_gap_device_name_set(this->deviceName.c_str()), "ble_svc_gap_device_name_set");
        throwOnBleError(ble_svc_dis_manufacturer_name_set("Cornucopia Machines"), "ble_svc_dis_manufacturer_name_set");
        throwOnBleError(ble_svc_dis_model_number_set(this->modelName.c_str()), "ble_svc_dis_model_number_set");
        throwOnBleError(ble_svc_dis_firmware_revision_set(this->firmwareVersion.c_str()), "ble_svc_dis_firmware_revision_set");
        throwOnBleError(ble_svc_dis_serial_number_set(this->serialNumber.c_str()), "ble_svc_dis_serial_number_set");

        // Burst-mode advertising timer — see startAdvertising() and the BLE_GAP_EVENT_ADV_COMPLETE
        // case in gapEventCallback() for why this exists.
        esp_timer_create_args_t timerArgs = {
            .callback = advRestartTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ble-adv-restart",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_THROW(esp_timer_create(&timerArgs, &advRestartTimer));

        nimble_port_freertos_init(hostTask);

        LOGTD(BLE, "Initialized, will advertise as '%s'", this->deviceName.c_str());
    }

    BleStatus getStatus() override {
        return status;
    }

    void setBatteryLevel(uint8_t percent) override {
        ble_svc_bas_battery_level_set(percent);
    }

    void setOnTimeReceived(const std::function<void(time_t)>& callback) override {
        onTimeReceived = callback;
    }

    void setOnWifiScanRequested(const std::function<void()>& callback) override {
        onWifiScanRequested = callback;
    }

    void setOnWifiCredentialsReceived(const std::function<void(std::string, std::string)>& callback) override {
        onWifiCredentialsReceived = callback;
    }

    void setOnWifiControlReceived(const std::function<void(std::string)>& callback) override {
        onWifiControlReceived = callback;
    }

    // Called by the WiFi driver whenever the WiFi status string changes.
    // Updates the cached value and notifies the connected client (if any).
    // Schedules work on the NimBLE host task so all BLE state is single-threaded.
    void setWifiStatus(const std::string& newStatus) override {
        LOGTD(BLE, "Publishing new WiFi status: %s", newStatus.c_str());
        postToHostTask([this, s = newStatus]() mutable {
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
    void setScanResults(const std::vector<WifiApRecord>& results) override {
        LOGTD(BLE, "Publishing WiFi scan results (%u APs)", static_cast<unsigned>(results.size()));
        postToHostTask([this, aps = results]() mutable {
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
    // NimBLE host functions return their own int error codes (BLE_HS_*), not esp_err_t, so
    // ESP_ERROR_THROW (which calls esp_err_to_name()) doesn't apply. Used only for one-time
    // initialization in the constructor: a failure there means the device can't function as
    // designed (no GATT server, no advertising, etc.), so fail fast at boot rather than limp
    // along silently — unlike runtime paths (notify, restart advertising after disconnect),
    // which log-and-continue since a transient failure there is expected and recoverable.
    static void throwOnBleError(int rc, const char* what) {
        if (rc != 0) {
            char message[80];
            snprintf(message, sizeof(message), "%s failed: 0x%02x", what, rc);
            throw std::runtime_error(message);
        }
    }

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

    // Fires advBurstInterval after each advertising burst completes (see
    // BLE_GAP_EVENT_ADV_COMPLETE). Runs on the esp_timer task, not the NimBLE host task, so
    // marshal the actual restart through postToHostTask like every other cross-task entry point.
    static void advRestartTimerCallback(void* arg) {
        postToHostTask([driver = static_cast<NimBleDriver*>(arg)]() {
            driver->startAdvertising();
        });
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
        auto* driver = static_cast<NimBleDriver*>(driverp);
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
            case BLE_GAP_EVENT_ADV_COMPLETE: {
                // The single burst event (max_events=1 in startAdvertising) just finished.
                // Re-arm after advBurstInterval instead of restarting immediately — see
                // startAdvertising() for why.
                esp_timer_stop(driver->advRestartTimer);    // harmless if not currently armed
                int rc = esp_timer_start_once(driver->advRestartTimer, duration_cast<microseconds>(advBurstInterval).count());
                if (rc != ESP_OK) {
                    LOGTE(BLE, "Failed to arm advertising restart timer: 0x%02x", rc);
                }
                break;
            }
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

    // Extended advertising (ble_gap_ext_adv_*, BLE 5.0) instead of legacy (ble_gap_adv_*).
    // Matches Espressif's power_save reference example, which uses this by default on
    // ESP32-C6. A single extended AD payload has room for up to 1650 bytes, so unlike the
    // old legacy-advertising implementation there's no need to split fields between a
    // primary ad and a separate scan response — flags, name, and all service UUIDs fit
    // in one ble_hs_adv_fields / ble_gap_ext_adv_set_data call.
    //
    // Burst mode: fires a single advertising event (max_events=1 below) instead of advertising
    // continuously, then re-arms via advRestartTimer after advBurstInterval (see
    // BLE_GAP_EVENT_ADV_COMPLETE in gapEventCallback). Net over-the-air behavior is the same
    // as continuous 2s-interval advertising — one event every ~2s, still connectable — but the
    // WiFi/BLE coexistence scheduler on ESP32-C6 selects its scheme based on the BLE controller's
    // *advertising state*, not actual radio activity: while advertising is enabled at all (even
    // between 2s-spaced events), coex wakes the chip from light sleep every ~2.8ms to service RF
    // time slices. Idle-between-bursts avoids that entirely. Measured: ~13 mA continuous vs
    // ~3 mA bursty with WiFi station + BLE both active.
    void startAdvertising() {
        constexpr uint8_t instance = 0;
        if (ble_gap_ext_adv_active(instance)) {
            return;
        }

        static const std::array<ble_uuid16_t, 3> serviceUuids16 = { {
            { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x180A },
            { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x180F },
            { .u = { .type = BLE_UUID_TYPE_16 }, .value = 0x1805 },
        } };

        const char* name = ble_svc_gap_device_name();
        struct ble_hs_adv_fields adFields = { };
        adFields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        adFields.name = reinterpret_cast<const uint8_t*>(name);
        adFields.name_len = static_cast<uint8_t>(strlen(name));
        adFields.name_is_complete = 1;
        adFields.uuids16 = serviceUuids16.data();
        adFields.num_uuids16 = serviceUuids16.size();
        adFields.uuids16_is_complete = 1;
        adFields.uuids128 = &uglyDucklingServiceUuid;
        adFields.num_uuids128 = 1;
        adFields.uuids128_is_complete = 1;

        struct ble_gap_ext_adv_params params = { };
        params.connectable = 1;    // must accept CONNECT_IND for phone-based WiFi provisioning
        params.own_addr_type = BLE_OWN_ADDR_RANDOM;
        params.primary_phy = BLE_HCI_LE_PHY_1M;
        params.secondary_phy = BLE_HCI_LE_PHY_1M;
        params.tx_power = 127;    // let the stack pick
        params.sid = 0;
        // Standard "fast advertising interval 1" (30-60ms) so the single burst event
        // (max_events=1 below) fires promptly; the ~2s spacing between bursts comes from
        // advRestartTimer instead.
        params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
        params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

        int rc = ble_gap_ext_adv_configure(instance, &params, nullptr, gapEventCallback, this);
        if (rc != 0) {
            LOGTE(BLE, "Failed to configure extended advertising: 0x%02x", rc);
            status = BleStatus::Error;
            return;
        }

        struct os_mbuf* data = os_msys_get_pkthdr(0, 0);
        if (data == nullptr) {
            LOGTE(BLE, "Failed to allocate mbuf for advertising data");
            status = BleStatus::Error;
            return;
        }
        rc = ble_hs_adv_set_fields_mbuf(&adFields, data);
        if (rc != 0) {
            LOGTE(BLE, "Failed to encode advertising fields: 0x%02x", rc);
            status = BleStatus::Error;
            return;
        }
        rc = ble_gap_ext_adv_set_data(instance, data);
        if (rc != 0) {
            LOGTE(BLE, "Failed to set advertising data: 0x%02x", rc);
            status = BleStatus::Error;
            return;
        }

        rc = ble_gap_ext_adv_start(instance, 0, 1);    // max_events=1: stop after this one burst
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

    // Gap between advertising bursts; see startAdvertising().
    static constexpr auto advBurstInterval = 2s;

    // All fields below are only accessed from the NimBLE host task (GAP/GATT callbacks
    // and postToHostTask closures), so no synchronisation is needed.
    int connHandle { -1 };
    bool scanInProgress { false };
    std::string wifiStatus;
    esp_timer_handle_t advRestartTimer { nullptr };

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
    inline static NimBleDriver* instance = nullptr;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
