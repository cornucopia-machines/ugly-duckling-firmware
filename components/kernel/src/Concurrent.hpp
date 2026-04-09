#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include <freertos/FreeRTOS.h>    // NOLINT(misc-header-include-cycle)

#include <Time.hpp>
#include <utility>

using namespace std::chrono;

namespace cornucopia::ugly_duckling::kernel {

class BaseQueue {
protected:
    BaseQueue(const std::string& name, size_t messageSize, size_t capacity)
        : name(name)
        , queue(xQueueCreate(capacity, messageSize)) {
    }

public:
    virtual ~BaseQueue() {
        vQueueDelete(queue);
    }

    virtual void clear() = 0;

    UBaseType_t size() const {
        return uxQueueMessagesWaiting(queue);
    }

protected:
    const std::string name;

    constexpr IRAM_ATTR QueueHandle_t getQueueHandle() const {
        return queue;
    }

private:
    QueueHandle_t queue;
};

template <typename TMessage>
class Queue : public BaseQueue {
public:
    Queue(const std::string& name, size_t capacity = 16)
        : BaseQueue(name, sizeof(TMessage*), capacity) {
    }

    template <typename... Args>
        requires std::constructible_from<TMessage, Args...>
    void put(Args&&... args) {
        auto innerArgs = std::make_tuple(std::forward<Args>(args)...);
        while (!std::apply([this](auto&&... unpackedArgs) {
            // Note: without 'this->' Clang Tidy complains about unnecessarily captured 'this'
            return this->offerIn(ticks::max(), std::forward<decltype(unpackedArgs)>(unpackedArgs)...);
        }, innerArgs)) { }
    }

    template <typename... Args>
        requires std::constructible_from<TMessage, Args...>
    bool offer(Args&&... args) {
        return offerIn(ticks::zero(), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires std::constructible_from<TMessage, Args...>
    bool offerIn(ticks timeout, Args&&... args) {
        auto copy = new TMessage(std::forward<Args>(args)...);
        bool sentWithoutDropping = xQueueSend(getQueueHandle(), reinterpret_cast<const void*>(&copy), timeout.count()) == pdTRUE;
        if (!sentWithoutDropping) {
            printf("Overflow in queue '%s', dropping message\n",
                this->name.c_str());
            delete copy;
        }
        return sentWithoutDropping;
    }

    using MessageHandler = std::function<void(TMessage&)>;

    size_t drain(const MessageHandler& handler) {
        return drain(SIZE_MAX, std::move(handler));
    }

    size_t drain(size_t maxItems, const MessageHandler& handler) {
        size_t count = 0;
        while (count < maxItems) {
            if (!poll(handler)) {
                break;
            }
            count++;
        }
        return count;
    }

    /**
     * @brief Wait for the first item to appear within the given timeout,
     * then drain any items remaining in the queue.
     */
    size_t drainIn(ticks timeout, const MessageHandler& handler) {
        return drainIn(SIZE_MAX, timeout, std::move(handler));
    }

    /**
     * @brief Wait for the first item to appear within the given timeout,
     * then drain no more than `maxItems` items remaining in the queue.
     */
    size_t drainIn(size_t maxItems, ticks timeout, const MessageHandler& handler) {
        size_t count = 0;
        ticks nextTimeout = timeout;
        while (true) {
            if (count >= maxItems) {
                break;
            }
            if (!pollIn(nextTimeout, handler)) {
                break;
            }
            count++;
            nextTimeout = ticks::zero();
        }
        return count;
    }

    void take() {
        take([](const TMessage& message) { });
    }

    void take(const MessageHandler& handler) {
        while (!pollIn(ticks::max(), handler)) { }
    }

    bool poll() {
        return poll([](const TMessage& message) { });
    }

    bool poll(const MessageHandler& handler) {
        return pollIn(ticks::zero(), std::move(handler));
    }

    bool pollIn(ticks timeout) {
        return pollIn(timeout, [](const TMessage& message) { });
    }

    bool pollIn(ticks timeout, const MessageHandler& handler) {
        TMessage* message;
        if (!xQueueReceive(getQueueHandle(), reinterpret_cast<void*>(&message), timeout.count())) {
            return false;
        }
        handler(*message);
        delete message;
        return true;
    }

    void clear() override {
        this->drain([](const TMessage& message) { });
    }
};

template <typename TMessage>
class CopyQueue : public BaseQueue {
public:
    CopyQueue(const std::string& name, size_t capacity = 16)
        : BaseQueue(name, sizeof(TMessage), capacity) {
    }

    void put(const TMessage message) {
        while (!offerIn(ticks::max(), message)) { }
    }

    bool offer(const TMessage message) {
        return offerIn(ticks::zero(), message);
    }

    bool offerIn(ticks timeout, const TMessage message) {
        bool sentWithoutDropping = xQueueSend(getQueueHandle(), &message, timeout.count()) == pdTRUE;
        if (!sentWithoutDropping) {
            printf("Overflow in queue '%s', dropping message",
                this->name.c_str());
        }
        return sentWithoutDropping;
    }

    bool IRAM_ATTR offerFromISR(const TMessage& message) {
        BaseType_t xHigherPriorityTaskWoken;
        bool sentWithoutDropping = xQueueSendFromISR(getQueueHandle(), &message, &xHigherPriorityTaskWoken) == pdTRUE;
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        return sentWithoutDropping;
    }

    void overwrite(const TMessage message) {
        xQueueOverwrite(getQueueHandle(), &message);
    }

    void IRAM_ATTR overwriteFromISR(const TMessage& message) {
        BaseType_t xHigherPriorityTaskWoken;
        xQueueOverwriteFromISR(getQueueHandle(), &message, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }

    TMessage take() {
        while (true) {
            auto message = pollIn(ticks::max());
            if (message.has_value()) {
                return message.value();
            }
        }
    }

    std::optional<TMessage> poll() {
        return pollIn(ticks::zero());
    }

    std::optional<TMessage> pollIn(ticks timeout) {
        TMessage message {};
        if (xQueueReceive(getQueueHandle(), &message, timeout.count())) {
            return message;
        }
        return std::nullopt;
    }

    void clear() override {
        xQueueReset(getQueueHandle());
    }
};

class MutexBase {
public:
    virtual ~MutexBase() = default;

    void lock() {
        while (!lockIn(ticks::max())) { }
    }

    bool tryLock() {
        return lockIn(ticks::zero());
    }

    virtual bool lockIn(ticks timeout) = 0;

    virtual void unlock() = 0;
};

class Mutex : public MutexBase {
public:
    Mutex()
        : mutex(xSemaphoreCreateMutex()) {
    }

    ~Mutex() override {
        vSemaphoreDelete(mutex);
    }

    bool lockIn(ticks timeout) override {
        return xSemaphoreTake(mutex, timeout.count());
    }

    void unlock() override {
        xSemaphoreGive(mutex);
    }

private:
    const SemaphoreHandle_t mutex;
};

class RecursiveMutex : public MutexBase {
public:
    RecursiveMutex()
        : mutex(xSemaphoreCreateRecursiveMutex()) {
    }

    ~RecursiveMutex() override {
        vSemaphoreDelete(mutex);
    }

    bool lockIn(ticks timeout) override {
        return xSemaphoreTakeRecursive(mutex, timeout.count());
    }

    void unlock() override {
        xSemaphoreGiveRecursive(mutex);
    }

private:
    const SemaphoreHandle_t mutex;
};

class Lock {
public:
    Lock(MutexBase& mutex)
        : mutex(mutex) {
        mutex.lock();
    }

    ~Lock() {
        mutex.unlock();
    }

    // Delete copy constructor and assignment operator to prevent copying
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    MutexBase& mutex;
};

}    // namespace cornucopia::ugly_duckling::kernel
