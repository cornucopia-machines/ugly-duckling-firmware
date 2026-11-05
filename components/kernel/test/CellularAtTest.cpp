#include <catch2/catch_test_macros.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/AtSocket.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/Edrx.hpp>
#include <drivers/cellular/NetworkTime.hpp>
#include <drivers/cellular/RadioStatus.hpp>
#include <drivers/cellular/Rat.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace cornucopia::ugly_duckling::kernel::drivers::cellular;
using namespace std::chrono_literals;

// Lines captured from an nRF9151 on Desert Lark MK2 (Serial Modem 2.0.0, modem firmware 2.0.4),
// on LTE-M with an Onomondo SIM roaming on One HU, unless noted otherwise

namespace {

AtResponse collect(std::string_view command, std::initializer_list<std::string_view> lines) {
    AtResponseCollector collector(command);
    for (auto line : lines) {
        if (collector.feed(line)) {
            break;
        }
    }
    return collector.take();
}

std::vector<std::string> feedLines(AtLineAssembler& assembler, std::initializer_list<std::string_view> chunks) {
    std::vector<std::string> lines;
    for (auto chunk : chunks) {
        assembler.feed(chunk, [&](std::string_view line) {
            lines.emplace_back(line);
        });
    }
    return lines;
}

}    // namespace

TEST_CASE("AtResponseCollector collects information lines up to OK") {
    AtResponseCollector collector("AT#XSMVER");

    REQUIRE_FALSE(collector.feed(R"(#XSMVER: "v2.0.0","3.4.1")"));
    REQUIRE(collector.feed("OK"));
    auto response = collector.take();
    REQUIRE(response.result == AtResult::Ok);
    REQUIRE(response.lines == std::vector<std::string> { R"(#XSMVER: "v2.0.0","3.4.1")" });
}

TEST_CASE("AtResponseCollector drops the echo of the command and empty lines") {
    auto response = collect("AT+CFUN?", { "AT+CFUN?", "", "+CFUN: 1", "OK" });

    REQUIRE(response.ok());
    REQUIRE(response.lines == std::vector<std::string> { "+CFUN: 1" });
}

TEST_CASE("AtResponseCollector stays incomplete without a final result code") {
    AtResponseCollector collector("AT+CGPADDR=0");

    REQUIRE_FALSE(collector.feed(R"(+CGPADDR: 0,"240.5.254.234")"));
    REQUIRE(collector.take().result == AtResult::Timeout);
}

TEST_CASE("AtResponseCollector does not take OK inside an information line for the result") {
    AtResponseCollector collector("AT#XRECV=1,1,64,0,2048");

    REQUIRE_FALSE(collector.feed("4F4B"));
    REQUIRE_FALSE(collector.feed("OK then"));
}

TEST_CASE("AtResponseCollector reports ERROR and +CME ERROR with its code") {
    REQUIRE(collect("AT#XRECV=1,1,64,0,100", { "ERROR" }).result == AtResult::Error);

    auto response = collect("AT+CTZR?", { "+CME ERROR: 0" });
    REQUIRE(response.result == AtResult::CmeError);
    REQUIRE(response.error == "0");
}

TEST_CASE("AtResponseCollector keeps a hex read's data line after its header") {
    auto response = collect("AT#XRECV=1,1,64,0,100", { "#XRECV: 1,1,5", "48545450AA", "OK" });

    REQUIRE(response.ok());
    REQUIRE(response.lines == std::vector<std::string> { "#XRECV: 1,1,5", "48545450AA" });
}

TEST_CASE("isFinalResultCode recognizes only whole result lines") {
    REQUIRE(isFinalResultCode("OK"));
    REQUIRE(isFinalResultCode("ERROR"));
    REQUIRE(isFinalResultCode("+CME ERROR: 0"));
    REQUIRE_FALSE(isFinalResultCode("OK then"));
    REQUIRE_FALSE(isFinalResultCode("#XSEND: 1,0,56"));
}

TEST_CASE("AtResponse::find returns the value after a prefix") {
    auto response = collect("AT%XSYSTEMMODE?", { "%XSYSTEMMODE: 1,1,0,1", "OK" });

    REQUIRE(response.find("%XSYSTEMMODE:") == "1,1,0,1");
    REQUIRE_FALSE(response.find("+CFUN:").has_value());
}

TEST_CASE("parseCeregUrc reads a registration with location") {
    auto registration = parseCeregUrc(R"(+CEREG: 2,"13F9","00177A01",7)");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Searching);
    REQUIRE(registration->trackingAreaCode == "13F9");
    REQUIRE(registration->cellId == "00177A01");
    REQUIRE(registration->accessTechnology == 7);
    REQUIRE_FALSE(registration->rejectCause.has_value());
}

TEST_CASE("parseCeregUrc reads mode 5 with empty cause fields and the PSM timers") {
    auto registration = parseCeregUrc(R"(+CEREG: 5,"13F9","00177A01",7,,,"11100000","11100000")");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::RegisteredRoaming);
    REQUIRE(registration->isRegistered());
    REQUIRE(registration->accessTechnology == 7);
    REQUIRE_FALSE(registration->causeType.has_value());
    REQUIRE_FALSE(registration->rejectCause.has_value());
}

TEST_CASE("parseCeregUrc reads the reject cause of a refused registration") {
    // A 1NCE SIM refused on NB-IoT by Yettel HU
    auto registration = parseCeregUrc(R"(+CEREG: 2,"6216","0014B307",9,0,15)");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Searching);
    REQUIRE(registration->causeType == 0);
    REQUIRE(registration->rejectCause == 15);
}

TEST_CASE("parseCeregUrc handles a bare status") {
    auto registration = parseCeregUrc("+CEREG: 2");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Searching);
    REQUIRE_FALSE(registration->trackingAreaCode.has_value());
}

TEST_CASE("parseCeregRead skips the URC mode in front of the status") {
    // URC mode 5, unknown -- as a URC, "+CEREG: 5,4" would mean "roaming, TAC 4"
    auto registration = parseCeregRead("+CEREG: 5,4");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Unknown);
}

TEST_CASE("parseCeregRead reads location after the mode") {
    auto registration = parseCeregRead(R"(+CEREG: 5,5,"13F9","00177A01",7,,,"11100000","11100000")");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::RegisteredRoaming);
    REQUIRE(registration->cellId == "00177A01");
}

TEST_CASE("describe summarizes a registration for the log") {
    REQUIRE(describe(*parseCeregUrc(R"(+CEREG: 5,"13F9","00177A01",7)")) == "registered (roaming), TAC 13F9, cell 00177A01");
    REQUIRE(describe(*parseCeregUrc("+CEREG: 3,,,,0,15")) == "registration denied, EMM cause 15");
    REQUIRE(describe(*parseCeregUrc("+CEREG: 3,,,,1,42")) == "registration denied, manufacturer cause 42");
    REQUIRE(describe(*parseCeregUrc("+CEREG: 2")) == "searching");
}

TEST_CASE("CEREG parsers reject malformed input") {
    REQUIRE_FALSE(parseCeregUrc("+CSCON: 1").has_value());
    REQUIRE_FALSE(parseCeregUrc("+CEREG: ").has_value());
    REQUIRE_FALSE(parseCeregUrc("+CEREG: 9").has_value());
    REQUIRE_FALSE(parseCeregRead("+CEREG: 5").has_value());
    REQUIRE_FALSE(parseCeregRead("+CEREG: x,1").has_value());
}

TEST_CASE("splitAtFields separates numbers, quoted strings and empty fields") {
    auto fields = splitAtFields(" 3,\"6216\",,9");

    REQUIRE(fields.size() == 4);
    REQUIRE(fields[0].asInt() == 3);
    REQUIRE(fields[1].asString() == "6216");
    REQUIRE_FALSE(fields[1].asInt().has_value());
    REQUIRE(fields[1].asHex() == 0x6216);
    REQUIRE(fields[2].empty());
    REQUIRE_FALSE(fields[2].asInt().has_value());
    REQUIRE_FALSE(fields[2].asString().has_value());
    REQUIRE(fields[3].asInt() == 9);
}

TEST_CASE("splitAtFields keeps commas inside quoted strings") {
    auto fields = splitAtFields("0,0,\"Vodafone, DE\",9");

    REQUIRE(fields.size() == 4);
    REQUIRE(fields[2].asString() == "Vodafone, DE");
    REQUIRE(fields[3].asInt() == 9);
}

TEST_CASE("splitAtFields handles negative numbers, a trailing empty field and an empty quoted string") {
    auto fields = splitAtFields("-157,\"\",");

    REQUIRE(fields.size() == 3);
    REQUIRE(fields[0].asInt() == -157);
    REQUIRE_FALSE(fields[1].empty());
    REQUIRE(fields[1].asString() == "");
    REQUIRE(fields[2].empty());
}

TEST_CASE("parseAtFields only accepts the expected prefix") {
    REQUIRE(parseAtFields("+CSCON: 1,0", "+CSCON:")->size() == 2);
    REQUIRE_FALSE(parseAtFields("+CEREG: 1", "+CSCON:").has_value());
}

TEST_CASE("AtLineAssembler joins lines split across chunks") {
    AtLineAssembler assembler(64);

    auto lines = feedLines(assembler, { "\r\n#XAPOLL: 1", ",1\r\n\r\n+CSCON: 0\r", "\n" });

    REQUIRE(lines == std::vector<std::string> { "#XAPOLL: 1,1", "+CSCON: 0" });
    REQUIRE_FALSE(assembler.hasPending());
}

TEST_CASE("AtLineAssembler keeps an unfinished line pending") {
    AtLineAssembler assembler(64);

    auto lines = feedLines(assembler, { "\r\n+CEREG: 1\r\n#XAPO" });

    REQUIRE(lines == std::vector<std::string> { "+CEREG: 1" });
    REQUIRE(assembler.hasPending());
}

TEST_CASE("AtLineAssembler cuts off an overlong line and drops the rest of it") {
    AtLineAssembler assembler(8);

    auto lines = feedLines(assembler, { "0123456789", "ABC\r\nnext\r\n" });

    REQUIRE(lines == std::vector<std::string> { "01234567", "next" });
    REQUIRE(assembler.getOverlongCount() == 1);
}

TEST_CASE("AtLineAssembler drops the line it has started on reset") {
    AtLineAssembler assembler(64);

    // Garbage from a probe at the wrong baud rate, with no line ending
    auto lines = feedLines(assembler, { "\x80\xfe" });
    assembler.reset();
    lines = feedLines(assembler, { "\r\nOK\r\n" });

    REQUIRE(lines == std::vector<std::string> { "OK" });
}

TEST_CASE("toHex and fromHex round-trip arbitrary bytes") {
    const std::array<uint8_t, 6> bytes { 0x00, 0x0D, 0x0A, 0x7F, 0xAB, 0xFF };

    auto hex = toHex(bytes.data(), bytes.size());
    REQUIRE(hex == "000D0A7FABFF");

    std::array<uint8_t, 6> decoded {};
    REQUIRE(fromHex(hex, decoded.data()));
    REQUIRE(decoded == bytes);
    // Serial Modem answers in lowercase
    REQUIRE(fromHex("abff", decoded.data()));
    REQUIRE(decoded[0] == 0xAB);
}

TEST_CASE("fromHex rejects odd lengths and non-hex digits") {
    std::array<uint8_t, 2> out {};
    REQUIRE_FALSE(fromHex("ABC", out.data()));
    REQUIRE_FALSE(fromHex("AG", out.data()));
}

TEST_CASE("parseXsocket reads the handle of a new socket") {
    // Handles start at 1, even with nothing else open
    REQUIRE(parseXsocket("#XSOCKET: 1,1,6") == 1);
    REQUIRE(parseXsocket("#XSOCKET: 2,2,17") == 2);
    REQUIRE_FALSE(parseXsocket("#XSOCKET: -1,1,6").has_value());
    // The read command's lines have five fields
    REQUIRE_FALSE(parseXsocket("#XSOCKET: 1,1,0,1,0").has_value());
}

TEST_CASE("parseXconnect only accepts a connected status for the socket") {
    REQUIRE(parseXconnect("#XCONNECT: 1,1", 1));
    REQUIRE_FALSE(parseXconnect("#XCONNECT: 1,0", 1));
    REQUIRE_FALSE(parseXconnect("#XCONNECT: 2,1", 1));
}

TEST_CASE("parseXsend reads how much the modem took") {
    REQUIRE(parseXsend("#XSEND: 1,0,56", "#XSEND:", 1) == 56);
    REQUIRE(parseXsend("#XSENDTO: 1,0,48", "#XSENDTO:", 1) == 48);
    REQUIRE_FALSE(parseXsend("#XSEND: 2,0,56", "#XSEND:", 1).has_value());
    REQUIRE_FALSE(parseXsend("#XSENDTO: 1,0,48", "#XSEND:", 1).has_value());
}

TEST_CASE("parseXrecvHeader reads the size of a hex mode read") {
    REQUIRE(parseXrecvHeader("#XRECV: 1,1,828", "#XRECV:", 1) == 828);
    REQUIRE(parseXrecvHeader(R"(#XRECVFROM: 1,1,48,"94.130.184.193",123)", "#XRECVFROM:", 1) == 48);
    // Binary mode, or another socket
    REQUIRE_FALSE(parseXrecvHeader("#XRECV: 1,0,828", "#XRECV:", 1).has_value());
    REQUIRE_FALSE(parseXrecvHeader("#XRECV: 2,1,828", "#XRECV:", 1).has_value());
}

TEST_CASE("parseXapoll reads the poll events") {
    auto readable = parseXapoll("#XAPOLL: 1,1");
    REQUIRE(readable.has_value());
    REQUIRE(readable->handle == 1);
    REQUIRE(readable->readable());
    REQUIRE_FALSE(readable->closed());

    // POLLHUP: the peer closed the connection
    auto hangUp = parseXapoll("#XAPOLL: 1,16");
    REQUIRE(hangUp.has_value());
    REQUIRE_FALSE(hangUp->readable());
    REQUIRE(hangUp->closed());

    // POLLIN and POLLHUP together
    REQUIRE(parseXapoll("#XAPOLL: 0,17")->readable());
    REQUIRE(parseXapoll("#XAPOLL: 0,17")->closed());
    // The test command's answer isn't an event
    REQUIRE_FALSE(parseXapoll("#XAPOLL: <handle>,(0,1),(0,1,4,5)").has_value());
}

TEST_CASE("parseCsconUrc reads the RRC state") {
    REQUIRE(parseCsconUrc("+CSCON: 1") == true);
    REQUIRE(parseCsconUrc("+CSCON: 0") == false);
}

TEST_CASE("parseCsconUrc rejects the read response and other values") {
    REQUIRE_FALSE(parseCsconUrc("+CSCON: 1,0").has_value());
    REQUIRE_FALSE(parseCsconUrc("+CSCON: 2").has_value());
    REQUIRE_FALSE(parseCsconUrc("+CEREG: 1").has_value());
}

TEST_CASE("parseCsconRead reads the mode after <n>") {
    REQUIRE(parseCsconRead("+CSCON: 1,0") == false);
    REQUIRE(parseCsconRead("+CSCON: 0,1") == true);
    REQUIRE_FALSE(parseCsconRead("+CSCON: 1").has_value());
}

TEST_CASE("parseXmonitor reads the serving cell") {
    auto cell = parseXmonitor(R"(%XMONITOR: 5,"One HU","One HU","21670","13F9",7,20,"00177A01",478,6200,42,29,"","11100000","11100000","01001001")");

    REQUIRE(cell.has_value());
    REQUIRE(cell->isCamped());
    REQUIRE(cell->rat == Rat::LteM);
    REQUIRE(cell->operatorName == "One HU");
    REQUIRE(cell->plmn == "21670");
    REQUIRE(cell->trackingAreaCode == "13F9");
    REQUIRE(cell->band == 20);
    REQUIRE(cell->cellId == "00177A01");
    REQUIRE(cell->pci == 478);
    REQUIRE(cell->earfcn == 6200);
    REQUIRE(cell->rsrp == -99);
    REQUIRE(cell->sinr == 4);
    REQUIRE_FALSE(cell->rsrq.has_value());
}

TEST_CASE("parseXmonitor recognizes a modem without a cell") {
    auto cell = parseXmonitor("%XMONITOR: 4");

    REQUIRE(cell.has_value());
    REQUIRE_FALSE(cell->isCamped());
    REQUIRE(describe(*cell) == "no serving cell");
    REQUIRE_FALSE(parseXmonitor("%XMONITOR: ").has_value());
    REQUIRE_FALSE(parseXmonitor("+CEREG: 4").has_value());
}

TEST_CASE("parseXmonitor leaves unknown signal values out") {
    auto cell = parseXmonitor(R"(%XMONITOR: 1,"","","21630","6216",9,20,"0014B307",184,6449,255,127,"","","","")");

    REQUIRE(cell.has_value());
    REQUIRE(cell->rat == Rat::NbIot);
    REQUIRE_FALSE(cell->rsrp.has_value());
    REQUIRE_FALSE(cell->sinr.has_value());
}

TEST_CASE("addConeval adds RSRQ, coverage enhancement level and transmit power") {
    auto cell = *parseXmonitor(R"(%XMONITOR: 5,"One HU","One HU","21670","13F9",7,20,"00177A01",478,6200,42,29,"","11100000","11100000","01001001")");

    addConeval(cell, R"(%CONEVAL: 0,0,7,41,19,29,"00177A01","21670",478,6200,20,0,0,13,1,8,117)");

    // Index 19 is -10.5 to -10 dB
    REQUIRE(cell.rsrq == -11);
    REQUIRE(cell.ecl == 0);
    REQUIRE(cell.txPower == 13);
    REQUIRE(describe(cell) == "LTE-M on One HU (21670), band 20, EARFCN 6200, PCI 478, cell 00177A01, RSRP -99 dBm, RSRQ -11 dB, SINR 4 dB, ECL 0, TX power 13 dBm");
}

TEST_CASE("addConeval ignores a failed evaluation and another cell") {
    auto cell = *parseXmonitor(R"(%XMONITOR: 5,"One HU","One HU","21670","13F9",7,20,"00177A01",478,6200,42,29,"","11100000","11100000","01001001")");

    addConeval(cell, "%CONEVAL: 1");
    addConeval(cell, R"(%CONEVAL: 0,0,7,41,19,29,"00177A02","21670",478,6200,20,0,0,13,1,8,117)");

    REQUIRE_FALSE(cell.rsrq.has_value());
    REQUIRE_FALSE(cell.ecl.has_value());
}

TEST_CASE("parseRat reads network-config's names") {
    REQUIRE(parseRat("lte-m") == Rat::LteM);
    REQUIRE(parseRat("nb-iot") == Rat::NbIot);
    REQUIRE_FALSE(parseRat("LTE-M").has_value());
    REQUIRE(toConfigName(Rat::NbIot) == std::string_view("nb-iot"));
}

TEST_CASE("parseRats keeps the order, and defaults to both with LTE-M first") {
    REQUIRE(parseRats({}) == std::vector<Rat> { Rat::LteM, Rat::NbIot });
    REQUIRE(parseRats({ "nb-iot", "lte-m" }) == std::vector<Rat> { Rat::NbIot, Rat::LteM });
    REQUIRE(parseRats({ "nb-iot", "nb-iot", "5g" }) == std::vector<Rat> { Rat::NbIot });
    REQUIRE(parseRats({ "5g" }) == std::vector<Rat> { Rat::LteM, Rat::NbIot });
}

TEST_CASE("toSystemMode enables the RATs with the preference in front, GNSS off") {
    REQUIRE(toSystemMode({ Rat::LteM, Rat::NbIot }) == "1,1,0,1");
    REQUIRE(toSystemMode({ Rat::NbIot, Rat::LteM }) == "1,1,0,2");
    REQUIRE(toSystemMode({ Rat::LteM }) == "1,0,0,0");
    REQUIRE(toSystemMode({ Rat::NbIot }) == "0,1,0,0");
    REQUIRE_FALSE(toSystemMode({}).has_value());
}

TEST_CASE("fromAccessTechnology maps <AcT> to the RAT") {
    REQUIRE(fromAccessTechnology(7) == Rat::LteM);
    REQUIRE(fromAccessTechnology(9) == Rat::NbIot);
    REQUIRE_FALSE(fromAccessTechnology(4).has_value());
    REQUIRE_FALSE(fromAccessTechnology(std::nullopt).has_value());
}

TEST_CASE("encodeEdrxCycle gives the 4-bit code of each RAT's cycles") {
    REQUIRE(encodeEdrxCycle(Rat::LteM, 5120ms) == "0000");
    REQUIRE(encodeEdrxCycle(Rat::LteM, 81920ms) == "0101");
    REQUIRE(encodeEdrxCycle(Rat::NbIot, 20480ms) == "0010");
    REQUIRE(encodeEdrxCycle(Rat::NbIot, 81920ms) == "0101");
    REQUIRE(encodeEdrxCycle(Rat::NbIot, 10485760ms) == "1111");
}

TEST_CASE("encodeEdrxCycle rejects lengths the RAT doesn't have") {
    REQUIRE_FALSE(encodeEdrxCycle(Rat::LteM, 41000ms).has_value());
    // LTE-M only
    REQUIRE_FALSE(encodeEdrxCycle(Rat::NbIot, 61440ms).has_value());
    REQUIRE_FALSE(encodeEdrxCycle(Rat::NbIot, 5120ms).has_value());
    REQUIRE_FALSE(encodeEdrxCycle(Rat::LteM, 0ms).has_value());
}

TEST_CASE("parseCedrxp reads what the network granted on LTE-M") {
    auto edrx = parseCedrxp(R"(+CEDRXP: 4,"0101","0101","0001")");

    REQUIRE(edrx.has_value());
    REQUIRE(edrx->active);
    REQUIRE(edrx->rat == Rat::LteM);
    REQUIRE(edrx->granted == 81920ms);
    // 1.28 s steps on LTE-M
    REQUIRE(edrx->pagingTimeWindow == 2560ms);
    REQUIRE(describe(*edrx) == "81920 ms on LTE-M, paging window 2560 ms");
}

TEST_CASE("parseCedrxrdp reads what the network granted on NB-IoT") {
    auto edrx = parseCedrxrdp(R"(+CEDRXRDP: 5,"0011","0101","0011")");

    REQUIRE(edrx.has_value());
    REQUIRE(edrx->active);
    REQUIRE(edrx->requested == 40960ms);
    REQUIRE(edrx->granted == 81920ms);
    // 2.56 s steps on NB-IoT
    REQUIRE(edrx->pagingTimeWindow == 10240ms);
    REQUIRE(describe(*edrx) == "81920 ms on NB-IoT (requested 40960 ms), paging window 10240 ms");
}

TEST_CASE("parseCedrxrdp reports eDRX not granted") {
    // LTE-M, nothing requested or granted
    auto notGranted = parseCedrxrdp(R"(+CEDRXRDP: 4,"","","")");
    REQUIRE(notGranted.has_value());
    REQUIRE_FALSE(notGranted->active);
    REQUIRE(notGranted->rat == Rat::LteM);
    REQUIRE(describe(*notGranted) == "not used on this cell");

    // A cell without eDRX
    auto unsupported = parseCedrxrdp("+CEDRXRDP: 0");
    REQUIRE(unsupported.has_value());
    REQUIRE_FALSE(unsupported->active);
    REQUIRE_FALSE(unsupported->rat.has_value());
}

TEST_CASE("parseXtime reads the universal time") {
    // 2026-11-03T23:47:38Z, local time zone +1 h ("40": four quarters, swapped)
    REQUIRE(parseXtime(R"(%XTIME: "40","62113032748340","00")") == 1793749658);
    // 2026-11-03T23:52:49Z: every byte swapped, the seconds too
    REQUIRE(parseXtime(R"(%XTIME: "40","62113032259440","00")") == 1793749969);
}

TEST_CASE("parseXtime rejects a URC without the time, and malformed times") {
    REQUIRE_FALSE(parseXtime(R"(%XTIME: "40",,"00")").has_value());
    REQUIRE_FALSE(parseXtime(R"(%XTIME: "40","6211303274","00")").has_value());
    // Month 31
    REQUIRE_FALSE(parseXtime(R"(%XTIME: "40","62133032748340","00")").has_value());
    REQUIRE_FALSE(parseXtime(R"(%XTIME: "40","6211303A748340","00")").has_value());
}

TEST_CASE("parseCclk reads the local time, and takes the time zone off") {
    // 2026-11-04T12:23:54Z, from a network at +1 h (four quarters)
    REQUIRE(parseCclk(R"(+CCLK: "26/11/04,13:23:54+04")") == 1793795034);
    REQUIRE(parseCclk(R"(+CCLK: "26/11/04,08:23:54-16")") == 1793795034);
    REQUIRE(parseCclk(R"(+CCLK: "26/11/04,12:23:54+00")") == 1793795034);
    // 00:30 local on New Year's Day at +1 h is still the old year in UTC
    REQUIRE(parseCclk(R"(+CCLK: "26/01/01,00:30:00+04")") == 1767223800);
}

TEST_CASE("parseCclk rejects other lines and malformed times") {
    REQUIRE_FALSE(parseCclk(R"(%CCLK: "26/11/04,13:23:54+04",0)").has_value());
    // Month 13
    REQUIRE_FALSE(parseCclk(R"(+CCLK: "26/13/04,13:23:54+04")").has_value());
    REQUIRE_FALSE(parseCclk(R"(+CCLK: "26/11/04 13:23:54+04")").has_value());
    // No time zone, and one beyond ±12 h
    REQUIRE_FALSE(parseCclk(R"(+CCLK: "26/11/04,13:23:54")").has_value());
    REQUIRE_FALSE(parseCclk(R"(+CCLK: "26/11/04,13:23:54+49")").has_value());
    REQUIRE_FALSE(parseCclk(R"(+CCLK: "26/11/04,13:23:54*04")").has_value());
}

TEST_CASE("formatCclk writes UTC for AT+CCLK=") {
    REQUIRE(formatCclk(1793795034) == "26/11/04,12:23:54+00");
    REQUIRE(parseCclk(R"(+CCLK: ")" + *formatCclk(1767232800) + "\"") == 1767232800);
    // The two-digit year only holds 2000–2099
    REQUIRE_FALSE(formatCclk(0).has_value());
}

TEST_CASE("makeSntpRequest asks as an SNTP version 4 client") {
    auto request = makeSntpRequest();

    REQUIRE(request.size() == 48);
    REQUIRE(request[0] == 0x23);
    REQUIRE(toHex(request.data() + 1, request.size() - 1) == std::string(94, '0'));
}

TEST_CASE("parseSntpResponse reads the server's transmit time") {
    // pool.ntp.org through Serial Modem (AT#XRECVFROM in hex mode)
    std::array<uint8_t, SNTP_PACKET_SIZE> reply {};
    REQUIRE(fromHex("240200e400000249000000008145fd01ee94e9e0f500e7ff0000000000000000ee94f238314b37ffee94f238314b57ff", reply.data()));

    // 2026-11-03T23:52:24Z
    REQUIRE(parseSntpResponse(reply.data(), reply.size()) == 1793749944);
}

TEST_CASE("parseSntpResponse rejects short packets, client packets and kiss-o'-death") {
    std::array<uint8_t, SNTP_PACKET_SIZE> reply {};
    REQUIRE(fromHex("240200e400000249000000008145fd01ee94e9e0f500e7ff0000000000000000ee94f238314b37ffee94f238314b57ff", reply.data()));
    REQUIRE_FALSE(parseSntpResponse(reply.data(), 47).has_value());

    auto client = reply;
    client[0] = 0x23;
    REQUIRE_FALSE(parseSntpResponse(client.data(), client.size()).has_value());

    auto kissOfDeath = reply;
    kissOfDeath[1] = 0;
    REQUIRE_FALSE(parseSntpResponse(kissOfDeath.data(), kissOfDeath.size()).has_value());
}

TEST_CASE("parseSntpResponse handles NTP era 1, from 2036") {
    std::array<uint8_t, SNTP_PACKET_SIZE> reply {};
    reply[0] = 0x24;
    reply[1] = 2;
    // 1 second into era 1: 2036-02-07T06:28:17Z
    reply[43] = 1;
    REQUIRE(parseSntpResponse(reply.data(), reply.size()) == 2085978497);
}
