#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
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
    // A URC that arrives while the command is in flight lands here too.
    std::vector<std::string> lines;

    // The error text of a +CME ERROR (with AT+CMEE=2 a readable message, otherwise a number)
    std::string error;

    bool ok() const {
        return result == AtResult::Ok;
    }

    /**
     * @brief The first information line starting with the given prefix, with the prefix and any
     * following spaces removed -- e.g. "+QCCID:" on "+QCCID: 8988..." gives "8988...".
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
 * @brief Parses the response to an AT command, as accumulated so far.
 *
 * Returns nullopt until a final result code (OK, ERROR or +CME ERROR) has arrived on a complete
 * line, so it can be called on every chunk the UART delivers. Only complete lines count: a
 * response cut off in the middle of "OK" is not mistaken for a finished one, and an information
 * line merely containing "OK" or "ERROR" is not a final result code.
 *
 * @param command the command as sent, without the trailing CR; its echo is dropped, in case the
 * module still has echo on (ATE1 is the power-on default)
 * @param awaitAfterOk for commands whose actual outcome follows the OK on a line of its own, such
 * as "SEND OK" after AT+QISEND or "+QIOPEN: 0,0" after AT+QIOPEN: the prefix of that line. The
 * response is then only complete once such a line has arrived; it ends up among the lines.
 * @param consumed set to how much of the buffer the response took up, once it's complete. What
 * follows arrived after the final result code, such as a URC, and isn't part of the response.
 */
inline std::optional<AtResponse> parseAtResponse(std::string_view buffer, std::string_view command, std::string_view awaitAfterOk = {}, size_t* consumed = nullptr) {
    AtResponse response { .result = AtResult::Timeout, .lines = {}, .error = {} };
    const auto* start = buffer.data();    // NOLINT(bugprone-suspicious-stringview-data-usage)
    auto complete = [&]() {
        if (consumed != nullptr) {
            *consumed = static_cast<size_t>(buffer.data() - start);
        }
        return response;
    };
    bool gotOk = false;
    while (true) {
        auto newline = buffer.find('\n');
        if (newline == std::string_view::npos) {
            // Incomplete line, if any: wait for more
            return std::nullopt;
        }
        std::string_view line = buffer.substr(0, newline);
        buffer.remove_prefix(newline + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        // With echo on, the module repeats the command, terminated by the CR we sent
        while (!line.empty() && line.front() == '\r') {
            line.remove_prefix(1);
        }

        if (line.empty() || line == command) {
            continue;
        }
        if (gotOk) {
            response.lines.emplace_back(line);
            if (line.starts_with(awaitAfterOk)) {
                return complete();
            }
            continue;
        }
        if (line == "OK") {
            response.result = AtResult::Ok;
            if (awaitAfterOk.empty()) {
                return complete();
            }
            gotOk = true;
            continue;
        }
        if (line == "ERROR") {
            response.result = AtResult::Error;
            return complete();
        }
        static constexpr std::string_view CME_ERROR = "+CME ERROR:";
        if (line.starts_with(CME_ERROR)) {
            line.remove_prefix(CME_ERROR.size());
            while (line.starts_with(' ')) {
                line.remove_prefix(1);
            }
            response.result = AtResult::CmeError;
            response.error = line;
            return complete();
        }
        response.lines.emplace_back(line);
    }
}

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
