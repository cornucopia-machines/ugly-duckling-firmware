#include "State.hpp"

#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

namespace cornucopia::ugly_duckling::kernel {

bool StateSource::setFromISR() const {
    return setBitsFromISR(eventBits | STATE_CHANGE_BIT_MASK) == pdPASS;
}

bool StateSource::clearFromISR() const {
    bool queued = xEventGroupClearBitsFromISR(eventGroup, eventBits) == pdPASS;
    return setBitsFromISR(STATE_CHANGE_BIT_MASK) == pdPASS && queued;
}

}    // namespace cornucopia::ugly_duckling::kernel
