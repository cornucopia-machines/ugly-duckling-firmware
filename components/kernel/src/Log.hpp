#pragma once

#include <esp_log.h>
#include <string.h>

#include <string>

namespace cornucopia::ugly_duckling::kernel {

enum class Level : uint8_t {
    None = 0,
    // Fatal = 1,
    Error = 2,
    Warning = 3,
    Info = 4,
    Debug = 5,
    Verbose = 6,
};

struct LogRecord {
    const Level level;
    const std::string message;
};

#ifndef UD_LOG_LEVEL
#ifdef UD_DEBUG
#define UD_LOG_LEVEL ESP_LOG_DEBUG
#else
#define UD_LOG_LEVEL ESP_LOG_INFO
#endif
#endif

// Compile-time ceiling for our own logs, deliberately separate from CONFIG_LOG_MAXIMUM_LEVEL
// (LOG_LOCAL_LEVEL), which gates IDF and third-party code. Release builds compile those at WARN
// but keep our INFO logs, which LOGGING_TAG enables at runtime and publishLogs forwards over MQTT.
// esp_log_level_set() doesn't clamp to the Kconfig maximum, so the runtime override still works.
#ifdef UD_DEBUG
#define UD_LOG_MAXIMUM_LEVEL CONFIG_LOG_MAXIMUM_LEVEL
#else
#define UD_LOG_MAXIMUM_LEVEL UD_LOG_LEVEL
#endif

#ifndef UD_LOG_VERBOSE
#define UD_LOG_VERBOSE ""
#endif

// helper: check if substring is in comma-separated list
inline bool loggingTagInList(const char* tag, const char* list) {
    if (list == nullptr) {
        return false;
    }
    const char* p = strstr(list, tag);
    while (p != nullptr) {
        const char* after = p + strlen(tag);
        if ((p == list || p[-1] == ',') && (*after == '\0' || *after == ',')) {
            return true;
        }
        p = strstr(after, tag);
    }
    return false;
}

// LOGGING_TAG(varName, "tagname")
#define LOGGING_TAG(varName, name)                        \
    inline constexpr const char* varName = "ud:" name;    \
    struct varName##_LoggerInit {                         \
        varName##_LoggerInit() {                          \
            esp_log_level_t lvl = UD_LOG_LEVEL;           \
            if (loggingTagInList(name, UD_LOG_VERBOSE)) { \
                lvl = ESP_LOG_VERBOSE;                    \
            }                                             \
            esp_log_level_set(varName, lvl);              \
        }                                                 \
    };                                                    \
    inline const varName##_LoggerInit varName##_logger_init;

LOGGING_TAG(GLOBAL, "global")

#define UD_LOG_LEVEL_LOCAL(level, tag, format, ...)           \
    do {                                                      \
        if (UD_LOG_MAXIMUM_LEVEL >= (level)) {                \
            ESP_LOG_LEVEL(level, tag, format, ##__VA_ARGS__); \
        }                                                     \
    } while (0)

#define LOGTE(tag, format, ...) UD_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define LOGTW(tag, format, ...) UD_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, format, ##__VA_ARGS__)
#define LOGTI(tag, format, ...) UD_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, format, ##__VA_ARGS__)
#define LOGTD(tag, format, ...) UD_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define LOGTV(tag, format, ...) UD_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, tag, format, ##__VA_ARGS__)

#define LOGE(format, ...) LOGTE(GLOBAL, format, ##__VA_ARGS__)
#define LOGW(format, ...) LOGTW(GLOBAL, format, ##__VA_ARGS__)
#define LOGI(format, ...) LOGTI(GLOBAL, format, ##__VA_ARGS__)
#define LOGD(format, ...) LOGTD(GLOBAL, format, ##__VA_ARGS__)
#define LOGV(format, ...) LOGTV(GLOBAL, format, ##__VA_ARGS__)

}    // namespace cornucopia::ugly_duckling::kernel
