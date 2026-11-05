#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

enum class AtResult : uint8_t {
    Ok,
    Error,
    // "+CME ERROR: <err>"; the text after the colon is in AtResponse::error
    CmeError,
    // No final result code arrived in time
    Timeout,
};

struct AtResponse {
    AtResult result;

    // Information lines between the command and the final result code, without line endings.
    // URCs the channel recognizes as such go to the URC handler instead (see AtChannel).
    std::vector<std::string> lines;

    // The error text of a +CME ERROR (a number with AT+CMEE=1)
    std::string error;

    bool ok() const {
        return result == AtResult::Ok;
    }

    /**
     * @brief The first information line starting with the given prefix, with the prefix and any
     * following spaces removed -- e.g. "%XICCID:" on "%XICCID: 8988..." gives "8988...".
     */
    std::optional<std::string_view> find(std::string_view prefix) const {
        for (const auto& line : lines) {
            if (line.starts_with(prefix)) {
                std::string_view value(line);
                value.remove_prefix(prefix.size());
                while (value.starts_with(' ')) {
                    value.remove_prefix(1);
                }
                return value;
            }
        }
        return std::nullopt;
    }
};

/**
 * @brief One comma-separated field of an information line such as "+CEREG: 3,\"6216\",,9".
 *
 * 3GPP TS 27.007 responses carry three kinds of field: numbers, quoted strings, and empty
 * (omitted optional) fields. The accessors return nullopt for an empty field and for a field
 * that is not of the requested kind.
 */
class AtField {
public:
    AtField(std::string_view text, bool quoted)
        : text(text)
        , quoted(quoted) {
    }

    bool empty() const {
        return text.empty() && !quoted;
    }

    bool isQuoted() const {
        return quoted;
    }

    std::optional<int> asInt() const {
        return quoted ? std::nullopt : parseNumber(text, 10);
    }

    /**
     * @brief A hex number, quoted or not, e.g. the "0014B308" of a cell ID.
     */
    std::optional<int> asHex() const {
        return parseNumber(text, 16);
    }

    /**
     * @brief A binary number, quoted or not, e.g. the "0011" of an eDRX cycle.
     */
    std::optional<int> asBinary() const {
        return parseNumber(text, 2);
    }

    /**
     * @brief The text of the field, without the quotes if it was quoted.
     */
    std::optional<std::string_view> asString() const {
        if (empty()) {
            return std::nullopt;
        }
        return text;
    }

private:
    static std::optional<int> parseNumber(std::string_view text, int base) {
        if (text.empty()) {
            return std::nullopt;
        }
        int value = 0;
        // from_chars takes an end pointer, so the view needn't be NUL-terminated
        const auto* begin = text.data();    // NOLINT(bugprone-suspicious-stringview-data-usage)
        const auto* end = begin + text.size();
        auto [ptr, ec] = std::from_chars(begin, end, value, base);
        if (ec != std::errc {} || ptr != end) {
            return std::nullopt;
        }
        return value;
    }

    std::string_view text;
    bool quoted;
};

/**
 * @brief Splits the parameters of an information line into fields.
 *
 * Commas inside quoted strings don't split ("+COPS: 0,0,\"Vodafone, DE\",9" has four fields),
 * and spaces around fields are dropped.
 */
inline std::vector<AtField> splitAtFields(std::string_view text) {
    std::vector<AtField> fields;
    size_t position = 0;
    while (true) {
        while (position < text.size() && text[position] == ' ') {
            position++;
        }
        bool quoted = position < text.size() && text[position] == '"';
        size_t start;
        size_t end;
        if (quoted) {
            start = position + 1;
            auto closingQuote = text.find('"', start);
            end = closingQuote == std::string_view::npos ? text.size() : closingQuote;
            position = closingQuote == std::string_view::npos ? text.size() : closingQuote + 1;
            // Skip anything between the closing quote and the next comma
            position = std::min(text.find(',', position), text.size());
        } else {
            start = position;
            position = std::min(text.find(',', position), text.size());
            end = position;
            while (end > start && text[end - 1] == ' ') {
                end--;
            }
        }
        fields.emplace_back(text.substr(start, end - start), quoted);
        if (position >= text.size()) {
            return fields;
        }
        // Step over the comma
        position++;
    }
}

/**
 * @brief The fields of an information line with the given prefix, e.g. "+CSQ:" on
 * "+CSQ: 21,99"; nullopt if the line has a different prefix.
 */
inline std::optional<std::vector<AtField>> parseAtFields(std::string_view line, std::string_view prefix) {
    if (!line.starts_with(prefix)) {
        return std::nullopt;
    }
    line.remove_prefix(prefix.size());
    return splitAtFields(line);
}

inline const char* toString(AtResult result) {
    switch (result) {
        case AtResult::Ok:
            return "OK";
        case AtResult::Error:
            return "ERROR";
        case AtResult::CmeError:
            return "CME ERROR";
        case AtResult::Timeout:
            return "timeout";
    }
    return "?";
}

/**
 * @brief Collects the response to an AT command from the lines the modem sends, one at a time.
 *
 * Information lines are kept until a final result code (OK, ERROR, +CME ERROR or +CMS ERROR)
 * completes the response. An information line merely containing "OK" or "ERROR" is not a final
 * result code: only a whole line counts.
 */
class AtResponseCollector {
public:
    /**
     * @param command the command as sent, without the trailing CR; its echo is dropped, in case
     * the modem has echo on
     */
    explicit AtResponseCollector(std::string_view command)
        : command(command) {
    }

    /**
     * @brief Takes the next line, without its line ending.
     *
     * @return whether the response is complete with it
     */
    bool feed(std::string_view line) {
        if (line.empty() || line == command) {
            return false;
        }
        if (line == "OK") {
            response.result = AtResult::Ok;
            return true;
        }
        if (line == "ERROR") {
            response.result = AtResult::Error;
            return true;
        }
        for (std::string_view prefix : { std::string_view("+CME ERROR:"), std::string_view("+CMS ERROR:") }) {
            if (line.starts_with(prefix)) {
                line.remove_prefix(prefix.size());
                while (line.starts_with(' ')) {
                    line.remove_prefix(1);
                }
                response.result = AtResult::CmeError;
                response.error = line;
                return true;
            }
        }
        response.lines.emplace_back(line);
        return false;
    }

    /**
     * @brief The response so far; its result stays Timeout until the response is complete.
     */
    AtResponse take() {
        return std::move(response);
    }

private:
    const std::string command;
    AtResponse response { .result = AtResult::Timeout, .lines = {}, .error = {} };
};

/**
 * @brief Whether a line is a final result code, the end of some command's response.
 */
inline bool isFinalResultCode(std::string_view line) {
    return line == "OK" || line == "ERROR" || line.starts_with("+CME ERROR:") || line.starts_with("+CMS ERROR:");
}

/**
 * @brief Splits what the modem sends into lines, as it arrives from the UART in chunks.
 *
 * A line cut off at the end of a chunk is kept here until the rest arrives. Socket data comes as
 * a single hex line of up to 4 KB, far more than one UART read.
 */
class AtLineAssembler {
public:
    explicit AtLineAssembler(size_t maxLine)
        : maxLine(maxLine) {
    }

    /**
     * @brief Calls handle with every line completed by data, without its line ending; empty
     * lines are skipped.
     *
     * A line longer than maxLine is handed over cut off at that length, and the rest of it is
     * dropped: the modem never sends one, so it can only be garbled.
     */
    template <typename Handler>
    void feed(std::string_view data, Handler handle) {
        while (!data.empty()) {
            auto newline = data.find('\n');
            auto segment = data.substr(0, newline);
            data.remove_prefix(newline == std::string_view::npos ? data.size() : newline + 1);
            if (!discarding) {
                auto room = maxLine - pending.size();
                pending.append(segment.substr(0, room));
                if (segment.size() > room) {
                    overlong++;
                    emit(handle);
                    discarding = true;
                }
            }
            if (newline == std::string_view::npos) {
                return;
            }
            if (!discarding) {
                emit(handle);
            }
            discarding = false;
        }
    }

    /**
     * @brief Drops the line started so far.
     */
    void reset() {
        pending.clear();
        discarding = false;
    }

    /**
     * @brief Whether a line has been started but not finished yet.
     */
    bool hasPending() const {
        return !pending.empty() || discarding;
    }

    /**
     * @brief How many lines were longer than maxLine.
     */
    uint32_t getOverlongCount() const {
        return overlong;
    }

private:
    template <typename Handler>
    void emit(Handler& handle) {
        std::string_view line = pending;
        while (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            handle(line);
        }
        pending.clear();
    }

    const size_t maxLine;
    std::string pending;
    // Dropping the rest of an overlong line, until its line ending
    bool discarding = false;
    uint32_t overlong = 0;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
