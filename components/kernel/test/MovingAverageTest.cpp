#include <catch2/catch_test_macros.hpp>

#include <MovingAverage.hpp>

using namespace cornucopia::ugly_duckling::kernel;

TEST_CASE("empty instance returns zero") {
    MovingAverage<double> ma(3);
    REQUIRE(ma.getAverage() == 0);
}

TEST_CASE("single measurement is returned") {
    MovingAverage<double> ma(3);
    ma.record(1);
    REQUIRE(ma.getAverage() == 1);
}

TEST_CASE("two measurements are averaged") {
    MovingAverage<double> ma(3);
    ma.record(1);
    ma.record(2);
    REQUIRE(ma.getAverage() == 1.5);
}

TEST_CASE("at capacity measurements are averaged") {
    MovingAverage<double> ma(3);
    ma.record(1);
    ma.record(2);
    ma.record(3);
    REQUIRE(ma.getAverage() == 2);
}

TEST_CASE("over capacity measurements are discarded") {
    MovingAverage<double> ma(3);
    ma.record(1);
    ma.record(2);
    ma.record(3);
    ma.record(4);
    ma.record(5);
    REQUIRE(ma.getAverage() == 4);
}

TEST_CASE("second measurements is returned for single-cell window") {
    MovingAverage<double> ma(1);
    ma.record(1);
    ma.record(2);
    REQUIRE(ma.getAverage() == 2);
}
