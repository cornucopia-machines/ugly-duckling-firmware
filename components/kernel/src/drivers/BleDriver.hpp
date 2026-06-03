#pragma once

#include <string>

#include <host/ble_hs.h>    // NOLINT(misc-header-include-cycle) -- ble_hs.h and ble_gap.h include each other; cycle is in ESP-IDF, not our code
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/dis/ble_svc_dis.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include <Log.hpp>

namespace cornucopia::ugly_duckling::kernel::drivers {

LOGGING_TAG(BLE, "ble")

enum class BleStatus {
    Off,
    Resetting,
    Error,
    Idle,
    Advertising,
    Connected
};

// Starts NimBLE, advertises the device, and hosts the standard Device Information Service (DIS,
// UUID 0x180A). Readable with any BLE scanner app (nRF Connect, LightBlue, etc.) without a
// custom client.
class BleDriver final {
public:
    BleDriver(
        const std::string& deviceName,
        const std::string& modelName,
        const std::string& firmwareVersion,
        const std::string& serialNumber)
        : deviceName(deviceName)
        , modelName(modelName)
        , firmwareVersion(firmwareVersion)
        , serialNumber(serialNumber) {
        instance = this;

        nimble_port_init();
        status = BleStatus::Idle;

        ble_hs_cfg.reset_cb = onReset;
        ble_hs_cfg.sync_cb = onSync;

        // Order matters: GAP and GATT must be initialized before DIS
        ble_svc_gap_init();
        ble_svc_gatt_init();
        ble_svc_dis_init();

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

private:
    static void onSync() {
        ble_addr_t addr;
        int rc = ble_hs_id_gen_rnd(1, &addr);
        if (rc == 0) {
            ble_hs_id_set_rnd(addr.val);
        }
        instance->startAdvertising();
    }

    static void onReset(int reason) {
        LOGTW(BLE, "Host reset, reason: %d", reason);
        instance->status = BleStatus::Resetting;
    }

    static void hostTask(void* /* param */) {
        nimble_port_run();
        nimble_port_freertos_deinit();
    }

    static int gapEventCallback(struct ble_gap_event* event, void* driverp) {
        auto driver = static_cast<BleDriver*>(driverp);
        switch (event->type) {
            case BLE_GAP_EVENT_CONNECT:
                if (event->connect.status == 0) {
                    driver->status = BleStatus::Connected;
                    LOGTD(BLE, "Client connected (handle=%d)", event->connect.conn_handle);
                } else {
                    LOGTD(BLE, "Connection failed (error=%d), restarting advertising", event->connect.status);
                    driver->startAdvertising();
                }
                break;
            case BLE_GAP_EVENT_DISCONNECT:
                LOGTD(BLE, "Client disconnected (reason=%d), restarting advertising",
                    event->disconnect.reason);
                driver->startAdvertising();
                break;
            default:
                break;
        }
        return 0;
    }

    void startAdvertising() {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        static ble_uuid16_t disUuid = BLE_UUID16_INIT(0x180A);

        const char* name = ble_svc_gap_device_name();
        auto nameLen = static_cast<uint8_t>(strnlen(name, UINT8_MAX));

        struct ble_hs_adv_fields fields = { };
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.uuids16 = &disUuid;
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;

        // BLE advertising PDU is 31 bytes. With flags (3B) + UUID16 (4B) + name header (2B) = 9B
        // overhead, 22 bytes remain for the name. Skip name in adv if it doesn't fit; it is still
        // readable via the GAP Device Name characteristic after connecting.
        if (nameLen <= 22) {
            fields.name = reinterpret_cast<const uint8_t*>(name);
            fields.name_len = nameLen;
            fields.name_is_complete = 1;
        }

        int rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            LOGTE(BLE, "Failed to set advertising fields: %d", rc);
            status = BleStatus::Error;
            return;
        }

        struct ble_gap_adv_params params = { };
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;

        rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER, &params, gapEventCallback, this);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            LOGTE(BLE, "Failed to start advertising: %d", rc);
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
    BleStatus status { BleStatus::Off };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline static BleDriver* instance = nullptr;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
