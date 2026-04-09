#pragma once

#include <Concurrent.hpp>
#include <Log.hpp>
#include <utility>

namespace cornucopia::ugly_duckling::kernel {

#define UD_LOG_COLOR_BLACK "30"
#define UD_LOG_COLOR_RED "31"
#define UD_LOG_COLOR_GREEN "32"
#define UD_LOG_COLOR_BROWN "33"
#define UD_LOG_COLOR_BLUE "34"
#define UD_LOG_COLOR_PURPLE "35"
#define UD_LOG_COLOR_CYAN "36"
#define UD_LOG_COLOR(COLOR) "\033[0;" COLOR "m"
#define UD_LOG_BOLD(COLOR) "\033[1;" COLOR "m"
#define UD_LOG_RESET_COLOR "\033[0m"

class ConsoleProvider {
public:
    static void init(std::shared_ptr<Queue<LogRecord>> logRecords, Level recordedLevel) {
        ConsoleProvider::logRecords = std::move(logRecords);
        ConsoleProvider::recordedLevel = recordedLevel;
        ConsoleProvider::originalVprintf = esp_log_set_vprintf(ConsoleProvider::processLogFunc);
    }

private:
    static int processLogFunc(const char* format, va_list args) {
        std::string message = renderMessage(format, args);
        return processLog(message);
    }

    static int processLog(const std::string& message) {
        if (message.empty()) {
            return 0;
        }

        std::string assembledMessage;
        {
            std::lock_guard<std::mutex> lock(partialMessageMutex);
            if (message[message.length() - 1] != '\n') {
                partialMessage += message;
                return 0;
            }
            if (!partialMessage.empty()) {
                assembledMessage = partialMessage + message;
                partialMessage.clear();
            }
        }
        if (assembledMessage.empty()) {
            return processLogLine(message);
        }
        return processLogLine(assembledMessage);
    }

    static int processLogLine(const std::string& message) {
        Level level = getLevel(message);
        if (level <= recordedLevel) {
            logRecords->offer(level, message);
        }

        int count = 0;
#ifdef UD_DEBUG
        // Erase the current line
        count += printf("\033[1G\033[0K");
        switch (level) {
            case Level::Error:
                count += printf(UD_LOG_COLOR(UD_LOG_COLOR_RED));
                break;
            case Level::Warning:
                count += printf(UD_LOG_COLOR(UD_LOG_COLOR_BROWN));
                break;
            case Level::Info:
                count += printf(UD_LOG_COLOR(UD_LOG_COLOR_GREEN));
                break;
            case Level::Debug:
                count += printf(UD_LOG_COLOR(UD_LOG_COLOR_CYAN));
                break;
            case Level::Verbose:
                count += printf(UD_LOG_COLOR(UD_LOG_COLOR_BLUE));
                break;
            default:
                break;
        }
#endif

        count += printf("%s", message.c_str());

#ifdef UD_DEBUG
        switch (level) {
            case Level::Error:
            case Level::Warning:
            case Level::Info:
                count += printf(UD_LOG_RESET_COLOR);
                break;
            default:
                break;
        }
#endif
        return count;
    }

    static std::string renderMessage(const char* format, va_list args) {
        int length;
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            length = vsnprintf(buffer, BUFFER_SIZE, format, args);
            if (length < 0) {
                return "<Encoding error>";
            }
            if (length < BUFFER_SIZE) {
                return { buffer, static_cast<size_t>(length) };
            }
        }

        // The buffer was too small, try again with a heap-allocated buffer instead, but still limit length
        length = std::min(length, 2048);
        char* heapBuffer = new char[length + 1];
        (void) vsnprintf(heapBuffer, length + 1, format, args);
        std::string result(heapBuffer, length);
        delete[] heapBuffer;
        return result;
    }

    static Level getLevel(const std::string& message) {
        // Anything that doesn't look like 'X ...' is a debug message
        if (message.length() < 2 || message[1] != ' ') {
            return Level::Debug;
        }
        switch (message[0]) {
            case 'E':
                return Level::Error;
            case 'W':
                return Level::Warning;
            case 'I':
                return Level::Info;
            case 'D':
                return Level::Debug;
            case 'V':
                return Level::Verbose;
            default:
                // Anything with an unknown level is a debug message
                return Level::Debug;
        }
    }

    static vprintf_like_t originalVprintf;
    static std::shared_ptr<Queue<LogRecord>> logRecords;
    static Level recordedLevel;
    static std::mutex bufferMutex;
    static constexpr size_t BUFFER_SIZE = 128;
    static char buffer[];

    static std::mutex partialMessageMutex;
    static std::string partialMessage;
};

vprintf_like_t ConsoleProvider::originalVprintf;
std::shared_ptr<Queue<LogRecord>> ConsoleProvider::logRecords;
Level ConsoleProvider::recordedLevel;
std::mutex ConsoleProvider::bufferMutex;
char ConsoleProvider::buffer[BUFFER_SIZE];
std::mutex ConsoleProvider::partialMessageMutex;
std::string ConsoleProvider::partialMessage;

}    // namespace cornucopia::ugly_duckling::kernel
