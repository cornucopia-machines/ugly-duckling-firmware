#include <catch2/catch_test_macros.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/RadioStatus.hpp>

using namespace cornucopia::ugly_duckling::kernel::drivers::cellular;

// Response samples follow the BC660K-GL AT Commands Manual v1.3 and the bring-up notes in
// https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/641

TEST_CASE("parseAtResponse collects information lines up to OK") {
    auto response = parseAtResponse("\r\nQuectel_Ltd\r\nQuectel_BC660K-GL\r\nRevision: BC660KGLAAR01A01\r\n\r\nOK\r\n", "ATI");

    REQUIRE(response.has_value());
    REQUIRE(response->result == AtResult::Ok);
    REQUIRE(response->lines == std::vector<std::string> { "Quectel_Ltd", "Quectel_BC660K-GL", "Revision: BC660KGLAAR01A01" });
}

TEST_CASE("parseAtResponse drops the echo of the command") {
    auto response = parseAtResponse("AT+QCCID\r\r\n+QCCID: 89860446091891372008\r\n\r\nOK\r\n", "AT+QCCID");

    REQUIRE(response.has_value());
    REQUIRE(response->lines == std::vector<std::string> { "+QCCID: 89860446091891372008" });
}

TEST_CASE("parseAtResponse waits for the final result code") {
    REQUIRE_FALSE(parseAtResponse("", "AT").has_value());
    REQUIRE_FALSE(parseAtResponse("\r\n+CSQ: 21,99\r\n", "AT+CSQ").has_value());
    // "OK" without its line ending yet
    REQUIRE_FALSE(parseAtResponse("\r\n+CSQ: 21,99\r\n\r\nOK", "AT+CSQ").has_value());
}

TEST_CASE("parseAtResponse does not take OK inside an information line for the result") {
    REQUIRE_FALSE(parseAtResponse("\r\n+QNBIOTEVENT: \"ENTER PSM OK\"\r\n", "AT").has_value());
}

TEST_CASE("parseAtResponse reports ERROR") {
    auto response = parseAtResponse("\r\nERROR\r\n", "AT+QENG=\"servingcell\"");

    REQUIRE(response.has_value());
    REQUIRE(response->result == AtResult::Error);
    REQUIRE(response->lines.empty());
}

TEST_CASE("parseAtResponse reports +CME ERROR with its text") {
    auto response = parseAtResponse("\r\n+CME ERROR: SIM not inserted\r\n", "AT+CIMI");

    REQUIRE(response.has_value());
    REQUIRE(response->result == AtResult::CmeError);
    REQUIRE(response->error == "SIM not inserted");
}

TEST_CASE("AtResponse::find returns the value after a prefix") {
    auto response = parseAtResponse("\r\n+CEREG: 3,1\r\n\r\nOK\r\n", "AT+CEREG?");

    REQUIRE(response.has_value());
    REQUIRE(response->find("+CEREG:") == "3,1");
    REQUIRE_FALSE(response->find("+CSQ:").has_value());
}

TEST_CASE("parseCeregUrc reads a registration with location") {
    auto registration = parseCeregUrc("+CEREG: 1,\"6216\",\"0B2C4A01\",9");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::RegisteredHome);
    REQUIRE(registration->isRegistered());
    REQUIRE(registration->trackingAreaCode == "6216");
    REQUIRE(registration->cellId == "0B2C4A01");
    REQUIRE(registration->accessTechnology == 9);
    REQUIRE_FALSE(registration->rejectCause.has_value());
}

TEST_CASE("parseCeregUrc reads the reject cause of a denied registration") {
    auto registration = parseCeregUrc("+CEREG: 3,\"6216\",\"0B2C4A01\",9,0,15");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Denied);
    REQUIRE_FALSE(registration->isRegistered());
    REQUIRE(registration->causeType == 0);
    REQUIRE(registration->rejectCause == 15);
}

TEST_CASE("parseCeregUrc handles a bare status") {
    auto registration = parseCeregUrc("+CEREG: 2");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Searching);
    REQUIRE_FALSE(registration->trackingAreaCode.has_value());
}

TEST_CASE("parseCeregUrc handles empty optional fields") {
    auto registration = parseCeregUrc("+CEREG: 3,,,,0,15");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Denied);
    REQUIRE_FALSE(registration->trackingAreaCode.has_value());
    REQUIRE_FALSE(registration->cellId.has_value());
    REQUIRE(registration->rejectCause == 15);
}

TEST_CASE("parseCeregRead skips the URC mode in front of the status") {
    // URC mode 3, currently searching -- as a URC, the same text would mean "denied"
    auto registration = parseCeregRead("+CEREG: 3,2");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::Searching);
}

TEST_CASE("parseCeregRead reads location and reject cause after the mode") {
    auto registration = parseCeregRead("+CEREG: 3,5,\"6216\",\"0B2C4A01\",9");

    REQUIRE(registration.has_value());
    REQUIRE(registration->status == RegistrationStatus::RegisteredRoaming);
    REQUIRE(registration->isRegistered());
    REQUIRE(registration->cellId == "0B2C4A01");
}

TEST_CASE("describe summarizes a registration for the log") {
    REQUIRE(describe(*parseCeregUrc("+CEREG: 1,\"6216\",\"0B2C4A01\",9")) == "registered (home), TAC 6216, cell 0B2C4A01");
    REQUIRE(describe(*parseCeregUrc("+CEREG: 3,,,,0,15")) == "registration denied, EMM cause 15");
    REQUIRE(describe(*parseCeregUrc("+CEREG: 3,,,,1,42")) == "registration denied, manufacturer cause 42");
    REQUIRE(describe(*parseCeregUrc("+CEREG: 2")) == "searching");
}

TEST_CASE("CEREG parsers reject malformed input") {
    REQUIRE_FALSE(parseCeregUrc("+CSQ: 21,99").has_value());
    REQUIRE_FALSE(parseCeregUrc("+CEREG: ").has_value());
    REQUIRE_FALSE(parseCeregUrc("+CEREG: 9").has_value());
    REQUIRE_FALSE(parseCeregRead("+CEREG: 3").has_value());
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
    REQUIRE(parseAtFields("+CSQ: 21,99", "+CSQ:")->size() == 2);
    REQUIRE_FALSE(parseAtFields("+CEREG: 1", "+CSQ:").has_value());
}

TEST_CASE("parseCsq converts the level to dBm") {
    REQUIRE(parseCsq("+CSQ: 12,99")->rssiDbm == -89);
    REQUIRE(parseCsq("+CSQ: 0,99")->rssiDbm == -113);
    REQUIRE(parseCsq("+CSQ: 31,0")->rssiDbm == -51);
}

TEST_CASE("parseCsq reports an unknown signal as nullopt") {
    // Captured on an MK13 right after boot, before the modem found a cell
    auto quality = parseCsq("+CSQ: 99,99");

    REQUIRE(quality.has_value());
    REQUIRE_FALSE(quality->rssiDbm.has_value());
}

TEST_CASE("parseCsq rejects malformed input") {
    REQUIRE_FALSE(parseCsq("+CSQ: 50,99").has_value());
    REQUIRE_FALSE(parseCsq("+CSQ: ").has_value());
    REQUIRE_FALSE(parseCsq("+CEREG: 1").has_value());
}

TEST_CASE("parseQengServingCell recognizes a module that is still searching") {
    // Captured on an MK13 right after boot
    auto cell = parseQengServingCell("+QENG: 0,0,0,0,\"00000000\",-157,-20,-137,-30,0,\"0000\",255,-128,255");

    REQUIRE(cell.has_value());
    REQUIRE_FALSE(cell->isCamped());
    REQUIRE_FALSE(cell->ecl.has_value());
    REQUIRE_FALSE(cell->txPower.has_value());
    REQUIRE_FALSE(cell->operationMode.has_value());
    REQUIRE(describe(*cell) == "no serving cell");
}

TEST_CASE("parseQengServingCell reads a serving cell") {
    // Values from the Desert Lark bring-up report (EARFCN 6449 in band 20, guard band);
    // the transmit power is made up, the report doesn't give it
    auto cell = parseQengServingCell("+QENG: 0,6449,0,184,\"0014B308\",-100,-12,-88,-3,20,\"6216\",0,10,2");

    REQUIRE(cell.has_value());
    REQUIRE(cell->isCamped());
    REQUIRE(cell->earfcn == 6449);
    REQUIRE(cell->pci == 184);
    REQUIRE(cell->cellId == "0014B308");
    REQUIRE(cell->rsrp == -100);
    REQUIRE(cell->rsrq == -12);
    REQUIRE(cell->rssi == -88);
    REQUIRE(cell->sinr == -3);
    REQUIRE(cell->band == 20);
    REQUIRE(cell->trackingAreaCode == "6216");
    REQUIRE(cell->ecl == 0);
    REQUIRE(cell->txPower == 10);
    REQUIRE(cell->operationMode == 2);
    REQUIRE(describe(*cell) == "band 20, EARFCN 6449, PCI 184, cell 0014B308, RSRP -100 dBm, RSRQ -12 dB, SINR -3 dB, ECL 0");
}

TEST_CASE("parseQengServingCell ignores neighbor cell lines and short lines") {
    REQUIRE_FALSE(parseQengServingCell("+QENG: 1,6449,185,-110,-15").has_value());
    REQUIRE_FALSE(parseQengServingCell("+QENG: 0,6449,0,184").has_value());
}
