#pragma once

#include <string>
#include <iostream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include <FakeLog.hpp>

#include <scheduling/IScheduler.hpp>

using namespace cornucopia::ugly_duckling::utils::scheduling;

namespace Catch {

template <>
struct StringMaker<ScheduleResult> {
    static std::string convert(ScheduleResult const& r) {
        using cornucopia::ugly_duckling::peripherals::api::toString;
        std::ostringstream oss;
        oss << "ScheduleResult{";
        oss << "target=" << toString(r.targetState) << ", ";
        oss << "next=";
        if (r.nextDeadline.has_value()) {
            oss << r.nextDeadline->count() << "ms";
        } else {
            oss << "None";
        }
        oss << ", publish=" << (r.shouldPublishTelemetry ? "true" : "false") << "}";
        return oss.str();
    }
};

}    // namespace Catch
