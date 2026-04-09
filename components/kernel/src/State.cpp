#include "State.hpp"
#include "freertos/idf_additions.h"

namespace cornucopia::ugly_duckling::kernel {

bool StateSource::setFromISR() const {
    return hasAllBits(setBitsFromISR(eventBits | STATE_CHANGE_BIT_MASK));
}

bool StateSource::clearFromISR() const {
    bool cleared = hasAllBits(xEventGroupClearBitsFromISR(eventGroup, eventBits));
    setBitsFromISR(STATE_CHANGE_BIT_MASK);
    return cleared;
}

}    // namespace cornucopia::ugly_duckling::kernel
