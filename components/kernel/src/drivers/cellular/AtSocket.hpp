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
 * @brief Encodes bytes as uppercase hex, the way the socket commands take them in hex string mode
 * (AT#XSEND=<handle>,1,...).
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
 * @brief Parses the handle out of "#XSOCKET: <handle>,<type>,<protocol>", the response to
 * AT#XSOCKET=<family>,<type>,<role>.
 *
 * @return nullopt if the line isn't one, or the handle is negative (the socket didn't open)
 */
inline std::optional<int> parseXsocket(std::string_view line) {
    auto fields = parseAtFields(line, "#XSOCKET:");
    if (!fields || fields->size() != 3) {
        return std::nullopt;
    }
    auto handle = (*fields)[0].asInt();
    return handle && *handle >= 0 ? handle : std::nullopt;
}

/**
 * @brief Whether "#XCONNECT: <handle>,<status>" reports the socket connected.
 */
inline bool parseXconnect(std::string_view line, int handle) {
    auto fields = parseAtFields(line, "#XCONNECT:");
    return fields && fields->size() == 2 && (*fields)[0].asInt() == handle && (*fields)[1].asInt() == 1;
}

/**
 * @brief The byte count of "#XSEND: <handle>,<result_type>,<size>" (or "#XSENDTO: ..."): how much
 * of a send the modem took.
 */
inline std::optional<size_t> parseXsend(std::string_view line, std::string_view prefix, int handle) {
    auto fields = parseAtFields(line, prefix);
    if (!fields || fields->size() != 3 || (*fields)[0].asInt() != handle) {
        return std::nullopt;
    }
    auto size = (*fields)[2].asInt();
    return size && *size >= 0 ? std::optional<size_t>(*size) : std::nullopt;
}

/**
 * @brief The byte count in the header line of a hex mode read, "#XRECV: <handle>,1,<size>" or
 * "#XRECVFROM: <handle>,1,<size>,"<ip>",<port>"; the data follows on a line of its own.
 */
inline std::optional<size_t> parseXrecvHeader(std::string_view line, std::string_view prefix, int handle) {
    auto fields = parseAtFields(line, prefix);
    if (!fields || fields->size() < 3 || (*fields)[0].asInt() != handle || (*fields)[1].asInt() != 1) {
        return std::nullopt;
    }
    auto size = (*fields)[2].asInt();
    return size && *size >= 0 ? std::optional<size_t>(*size) : std::nullopt;
}

/**
 * @brief The poll events in "#XAPOLL: <handle>,<revents>", as AT#XAPOLL reports them.
 */
struct PollEvents {
    int handle;
    int events;

    // POLLIN: data to read. Only reported for the first data after a read
    bool readable() const {
        return (events & 1) != 0;
    }

    // POLLERR, POLLHUP or POLLNVAL: the connection is gone, once what it received has been read
    bool closed() const {
        return (events & (8 | 16 | 32)) != 0;
    }
};

inline std::optional<PollEvents> parseXapoll(std::string_view line) {
    auto fields = parseAtFields(line, "#XAPOLL:");
    if (!fields || fields->size() != 2) {
        return std::nullopt;
    }
    auto handle = (*fields)[0].asInt();
    auto events = (*fields)[1].asInt();
    if (!handle || !events || *handle < 0 || *events < 0) {
        return std::nullopt;
    }
    return PollEvents { .handle = *handle, .events = *events };
}

enum class SocketEventType : uint8_t {
    // Data is waiting in the modem; announced once, until a read has found it drained
    DataAvailable,
    // The peer or the network closed the connection, or the modem restarted
    Closed,
};

struct SocketEvent {
    SocketEventType type;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
