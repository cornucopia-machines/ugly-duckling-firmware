#include "Pin.hpp"

namespace farmhub::kernel {

std::map<std::string, PinPtr> Pin::BY_NAME;

std::map<std::string, InternalPinPtr> InternalPin::INTERNAL_BY_NAME;
std::map<gpio_num_t, InternalPinPtr> InternalPin::INTERNAL_BY_GPIO;
std::vector<adc_oneshot_unit_handle_t> AnalogPin::ANALOG_UNITS { 2 };

}    // namespace farmhub::kernel
