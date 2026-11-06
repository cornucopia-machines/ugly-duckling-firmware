#pragma once

/*
 * Hands out one MoleCricket per board on the I²C bus. Kept apart from MoleCricket.hpp, which is pure
 * C++ and compiles in native unit tests.
 */

#include "MoleCricket.hpp"
#include "Sc16is752I2CBus.hpp"
#include <I2CManager.hpp>
#include <Result.hpp>
#include <Task.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

namespace cornucopia::ugly_duckling::peripherals::serial {

// Peripherals on terminals A and B of the same Mole Cricket must share one bridge instance: it holds
// the GPIO shadow and the lock that keeps the two probes from measuring at once.
class MoleCricketRegistry {
public:
    Result<std::shared_ptr<MoleCricket>> getOrCreate(const std::shared_ptr<kernel::I2CManager>& i2c, const std::shared_ptr<kernel::drivers::SwitchedRail>& rail, const kernel::I2CConfig& config) {
        std::scoped_lock lock(mutex);
        auto key = std::make_tuple(config.sda->getGpio(), config.scl->getGpio(), config.address);
        if (auto it = boards.find(key); it != boards.end()) {
            if (auto board = it->second.lock()) {
                return board;
            }
        }
        auto device = TRY(i2c->createDevice("mole-cricket", config));
        auto board = TRY(MoleCricket::create(
            std::make_unique<Sc16is752I2CBus>(device),
            rail,
            [](std::chrono::milliseconds duration) { kernel::Task::delay(duration); },
            config.toString()));
        boards[key] = board;
        return board;
    }

private:
    std::mutex mutex;
    std::map<std::tuple<gpio_num_t, gpio_num_t, uint8_t>, std::weak_ptr<MoleCricket>> boards;
};

}    // namespace cornucopia::ugly_duckling::peripherals::serial
