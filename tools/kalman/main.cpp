#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "utils/scheduling/MoistureKalmanFilter.hpp"

using cornucopia::ugly_duckling::utils::scheduling::MoistureKalmanFilter;

struct DataPoint {
    std::string time;
    double volume {};
    double moisture {};
    double temperature {};
};

static double toDouble(const std::string& s) {
    if (s.empty())
        return 0.0;
    size_t idx = 0;
    double v = std::stod(s, &idx);
    (void) idx;
    return v;
}

// Callback style parser: calls `onRow(dp)` for each parsed line
static void parseCsvStream(std::istream& in, const std::function<void(const DataPoint&)>& onRow) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("time,", 0) == 0)
            continue;    // skip header
        if (line.empty())
            continue;

        std::stringstream ss(line);
        std::string field;
        DataPoint dp;

        if (!std::getline(ss, dp.time, ','))
            continue;

        if (std::getline(ss, field, ',')) {
            if (!field.empty() && field.front() == '"' && field.back() == '"' && field.size() >= 2)
                field = field.substr(1, field.size() - 2);
            dp.volume = toDouble(field);
        }
        if (std::getline(ss, field, ','))
            dp.moisture = toDouble(field);
        if (std::getline(ss, field, ','))
            dp.temperature = toDouble(field);

        onRow(dp);
    }
}

struct Args {
    std::string dataPath;
    double initMoistReal = 50.0;
    double initBeta = 0.0;
    double tempRef = 20.0;
    double qMoist = 1e-6;
    double qBeta = 1e-6;
    double R = 1e-3;
};

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --data input.csv [options]\n"
              << "Options:\n"
              << "  --initMoistReal <double>   (default 50.0)\n"
              << "  --initBeta <double>        (default 0.0)\n"
              << "  --tempRef <double>         (default 20.0)\n"
              << "  --qMoist <double>          (default 1e-6)\n"
              << "  --qBeta <double>           (default 1e-6)\n"
              << "  --R <double>               (default 1.0)\n"
              << std::endl;
}

static bool parseArgs(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        auto needVal = [&](int& i) -> const char* {
            if (i + 1 >= argc)
                return nullptr;
            return argv[++i];
        };
        if (a == "--data") {
            if (const char* v = needVal(i))
                out.dataPath = v;
            else
                return false;
        } else if (a == "--initMoistReal") {
            if (const char* v = needVal(i))
                out.initMoistReal = std::atof(v);
            else
                return false;
        } else if (a == "--initBeta") {
            if (const char* v = needVal(i))
                out.initBeta = std::atof(v);
            else
                return false;
        } else if (a == "--tempRef") {
            if (const char* v = needVal(i))
                out.tempRef = std::atof(v);
            else
                return false;
        } else if (a == "--qMoist") {
            if (const char* v = needVal(i))
                out.qMoist = std::atof(v);
            else
                return false;
        } else if (a == "--qBeta") {
            if (const char* v = needVal(i))
                out.qBeta = std::atof(v);
            else
                return false;
        } else if (a == "--R") {
            if (const char* v = needVal(i))
                out.R = std::atof(v);
            else
                return false;
        } else if (a == "-h" || a == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            return false;
        }
    }
    return !out.dataPath.empty();
}

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        printUsage(argv[0]);
        return 2;
    }

    try {
        std::istream* inPtr = nullptr;
        std::ifstream fileIn;
        if (args.dataPath == "-") {
            inPtr = &std::cin;
        } else {
            fileIn.open(args.dataPath);
            if (!fileIn) {
                std::cerr << "Failed to open input: " << args.dataPath << "\n";
                return 1;
            }
            inPtr = &fileIn;
        }

        std::cerr << "Reading: " << (args.dataPath == "-" ? "<stdin>" : args.dataPath) << "\n";

        MoistureKalmanFilter filter(args.initMoistReal, args.initBeta, args.tempRef);

        std::cout << "time,moisture,temperature,real_moisture,beta,,qMoist,qBeta,R\n";

        size_t rows = 0;
        parseCsvStream(*inPtr, [&](const DataPoint& dp) {
            filter.update(dp.moisture, dp.temperature, args.qMoist, args.qBeta, args.R);
            std::cout << dp.time << ','
                      << std::fixed
                      << std::setprecision(3)
                      << dp.moisture << ','
                      << dp.temperature << ','
                      << filter.getMoistReal() << ','
                      << filter.getBeta();
            if (rows == 0) {
                std::cout << ",,"
                          << std::scientific
                          << args.qMoist << ','
                          << args.qBeta << ','
                          << args.R;
            }
            std::cout << '\n';
            ++rows;
        });

        std::cerr << "Processed rows: " << rows << "\n";
        // Print final state to stderr for convenience
        std::cerr << "Final beta=" << filter.getBeta() << ", moistReal=" << filter.getMoistReal() << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unknown error\n";
        return 1;
    }
}
