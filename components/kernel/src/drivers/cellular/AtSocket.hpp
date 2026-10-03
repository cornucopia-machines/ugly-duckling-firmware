#pragma once

#include <drivers/cellular/AtResponse.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief Encodes bytes as uppercase hex, the way the socket commands take them in hex mode
 * (AT+QICFG="dataformat",1,1).
 */
inline std::string toHex(const uint8_t* data, size_t length) {
    static constexpr std::string_view DIGITS = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        hex += DIGITS[data[i] >> 4];
        hex += DIGITS[data[i] & 0x0F];
    }
    return hex;
}

/**
 * @brief Decodes hex digits (either case) into bytes.
 *
 * @param out room for hex.size() / 2 bytes
 * @return false if the input has an odd length or anything but hex digits
 */
inline bool fromHex(std::string_view hex, uint8_t* out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    // from_chars takes an end pointer, so the view needn't be NUL-terminated
    const auto* digits = hex.data();    // NOLINT(bugprone-suspicious-stringview-data-usage)
    for (size_t i = 0; i < hex.size(); i += 2) {
        // Unsigned, so a sign is rejected too
        auto [ptr, ec] = std::from_chars(digits + i, digits + i + 2, out[i / 2], 16);
        if (ec != std::errc {} || ptr != digits + i + 2) {
            return false;
        }
    }
    return true;
}

/**
 * @brief One read from the module's receive buffer: the response line of AT+QIRD.
 */
struct SocketRead {
    // Bytes in this read; 0 when the buffer was empty
    size_t length;
    // Bytes still waiting in the module's buffer; only reported with AT+QICFG="showlength",1
    std::optional<size_t> remaining;
    // The data as hex digits, pointing into the parsed line
    std::string_view hex;
};

/**
 * @brief Parses the data line of AT+QIRD in hex mode.
 *
 * +QIRD: <actual_read_length>[,<remaining_length>],"<data>", or just "+QIRD: 0" when there is
 * nothing to read. Rejects a line whose hex doesn't match the length it claims.
 */
inline std::optional<SocketRead> parseQird(std::string_view line) {
    auto fields = parseAtFields(line, "+QIRD:");
    if (!fields || fields->empty()) {
        return std::nullopt;
    }
    auto length = (*fields)[0].asInt();
    if (!length || *length < 0) {
        return std::nullopt;
    }
    if (*length == 0) {
        auto remaining = fields->size() > 1 ? (*fields)[1].asInt() : std::nullopt;
        return SocketRead { .length = 0, .remaining = remaining ? std::optional<size_t>(*remaining) : std::nullopt, .hex = {} };
    }
    // The data is always the last field: with showlength the remaining length sits in between
    if (fields->size() < 2 || fields->size() > 3) {
        return std::nullopt;
    }
    auto hex = fields->back().asString();
    if (!hex || hex->size() != static_cast<size_t>(*length) * 2) {
        return std::nullopt;
    }
    std::optional<size_t> remaining;
    if (fields->size() == 3) {
        auto value = (*fields)[1].asInt();
        if (!value || *value < 0) {
            return std::nullopt;
        }
        remaining = static_cast<size_t>(*value);
    }
    return SocketRead { .length = static_cast<size_t>(*length), .remaining = remaining, .hex = *hex };
}

/**
 * @brief Parses the "+QIOPEN: <connectID>,<result>" that follows AT+QIOPEN; the result is 0 on
 * success, otherwise an error code from chapter 3 of the TCP/IP application note.
 */
inline std::optional<int> parseQiopen(std::string_view line, int connectId) {
    auto fields = parseAtFields(line, "+QIOPEN:");
    if (!fields || fields->size() != 2 || (*fields)[0].asInt() != connectId) {
        return std::nullopt;
    }
    return (*fields)[1].asInt();
}

enum class SocketEventType : uint8_t {
    // Data is waiting in the module's buffer; reported once until the buffer has been drained
    DataAvailable,
    // The module's 2 KB receive buffer is full: read it before the peer is throttled
    BufferFull,
    // The peer, or the network, closed the connection
    Closed,
};

struct SocketEvent {
    SocketEventType type;
    int connectId;
};

/**
 * @brief Parses the socket URCs of buffer access mode:
 * +QIURC: "recv",<connectID>[,<current_recv_length>], +QIURC: "recv",<connectID>,"buff full"
 * and +QIURC: "closed",<connectID>.
 */
inline std::optional<SocketEvent> parseQiurc(std::string_view line) {
    auto fields = parseAtFields(line, "+QIURC:");
    if (!fields || fields->size() < 2) {
        return std::nullopt;
    }
    auto type = (*fields)[0].asString();
    auto connectId = (*fields)[1].asInt();
    if (!type || !connectId) {
        return std::nullopt;
    }
    if (*type == "recv") {
        bool full = fields->size() > 2 && (*fields)[2].asString() == "buff full";
        return SocketEvent { .type = full ? SocketEventType::BufferFull : SocketEventType::DataAvailable, .connectId = *connectId };
    }
    if (*type == "closed") {
        return SocketEvent { .type = SocketEventType::Closed, .connectId = *connectId };
    }
    return std::nullopt;
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
