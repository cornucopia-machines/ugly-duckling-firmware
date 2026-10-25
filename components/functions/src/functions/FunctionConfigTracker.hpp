#pragma once

#include <Result.hpp>
#include <config/ConfigManifestEntry.hpp>

#include <ArduinoJson.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace cornucopia::ugly_duckling::functions {

using cornucopia::ugly_duckling::kernel::failure;
using cornucopia::ugly_duckling::kernel::Result;
using cornucopia::ugly_duckling::kernel::config::ConfigManifestEntry;

// Applies a verbatim config body (already parsed out of a ConfigEnvelope) to a live function
// instance. Empty for functions that don't implement HasConfig<TConfig>.
using ConfigureFn = std::function<Result<>(JsonObjectConst)>;

/**
 * @brief In-memory name -> {configureFn, fingerprint} bookkeeping at the heart of FunctionRegistry
 * (docs/Configuration.md). Deliberately has no NVS/MQTT dependency of its own, so the
 * apply-and-track-fingerprint logic is unit-testable with a fake configureFn, independent of
 * FunctionRegistry's real persistence (StoredConfig, which needs real NVS).
 */
class FunctionConfigTracker {
public:
    void record(const std::string& name, ConfigureFn configureFn, const std::string& fingerprint, const std::string& requestedAt) {
        entries[name] = Entry { .configureFn = std::move(configureFn), .fingerprint = fingerprint, .requestedAt = requestedAt };
    }

    // Applies data via the named entry's configureFn and records the fingerprint/requestedAt only
    // once configureFn succeeds (proof-of-apply, not proof-of-receipt). Fails if the name is
    // unknown or was recorded without a configureFn -- a protocol violation, handled in Phase 1
    // exactly like any other faulty configuration.
    Result<> apply(const std::string& name, JsonObjectConst data, const std::string& fingerprint, const std::string& requestedAt) {
        auto it = entries.find(name);
        if (it == entries.end()) {
            return failure("Cannot reconfigure unknown function '" + name + "'");
        }
        Entry& entry = it->second;
        if (!entry.configureFn) {
            return failure("Function '" + name + "' does not support configuration");
        }
        TRY(entry.configureFn(data));
        entry.fingerprint = fingerprint;
        entry.requestedAt = requestedAt;
        return {};
    }

    // name -> {fingerprint, requestedAt} for every recorded entry, straight from in-memory state.
    std::unordered_map<std::string, ConfigManifestEntry> manifest() const {
        std::unordered_map<std::string, ConfigManifestEntry> result;
        for (const auto& [name, entry] : entries) {
            result.emplace(name, ConfigManifestEntry { .fingerprint = entry.fingerprint, .requestedAt = entry.requestedAt });
        }
        return result;
    }

private:
    struct Entry {
        ConfigureFn configureFn;
        std::string fingerprint;
        std::string requestedAt;
    };

    std::unordered_map<std::string, Entry> entries;
};

// Writes the SYNC payload's `configurations` entries from a function manifest -- name ->
// {fingerprint, requestedAt} (docs/Configuration.md, "BOOT, SYNC, UPDATE"). Pure JSON construction
// with no NVS/MQTT dependency, so it's unit-testable independent of FunctionRegistry; Device.hpp's
// publishSync() is the sole caller.
inline void writeSyncManifest(JsonObject& configurations, const std::unordered_map<std::string, ConfigManifestEntry>& manifest) {
    for (const auto& [name, entry] : manifest) {
        auto configurationEntry = configurations[name].to<JsonObject>();
        configurationEntry["fingerprint"] = entry.fingerprint;
        configurationEntry["requestedAt"] = entry.requestedAt;
    }
}

}    // namespace cornucopia::ugly_duckling::functions
