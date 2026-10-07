#include <catch2/catch_test_macros.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/AtSocket.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/Edrx.hpp>
#include <drivers/cellular/NetworkTime.hpp>
#include <drivers/cellular/RadioStatus.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace cornucopia::ugly_duckling::kernel::drivers::cellular;
using namespace std::chrono_literals;

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

TEST_CASE("parseAtResponse reports where the response ends, before a URC in the same read") {
    std::string_view buffer = "\r\n+QIRD: 512,0,\"00\"\r\n\r\nOK\r\n\r\n+QIURC: \"recv\",0,512\r\n";
    size_t consumed = 0;

    auto response = parseAtResponse(buffer, "AT+QIRD=0,512", {}, &consumed);

    REQUIRE(response.has_value());
    REQUIRE(response->lines == std::vector<std::string> { "+QIRD: 512,0,\"00\"" });
    REQUIRE(buffer.substr(consumed) == "\r\n+QIURC: \"recv\",0,512\r\n");
}

TEST_CASE("parseAtResponse consumes all of a response with nothing after it") {
    std::string_view buffer = "\r\nOK\r\n";
    size_t consumed = 0;

    REQUIRE(parseAtResponse(buffer, "AT", {}, &consumed).has_value());
    REQUIRE(consumed == buffer.size());
}

TEST_CASE("parseAtResponse with awaitAfterOk waits for the line after OK") {
    REQUIRE_FALSE(parseAtResponse("\r\nOK\r\n", "AT+QISEND=0,2,\"0102\"", "SEND ").has_value());

    auto response = parseAtResponse("\r\nOK\r\n\r\nSEND OK\r\n", "AT+QISEND=0,2,\"0102\"", "SEND ");

    REQUIRE(response.has_value());
    REQUIRE(response->result == AtResult::Ok);
    REQUIRE(response->lines == std::vector<std::string> { "SEND OK" });
}

TEST_CASE("parseAtResponse with awaitAfterOk keeps URCs that arrive in between") {
    auto response = parseAtResponse("\r\nOK\r\n\r\n+CEREG: 5\r\n\r\n+QIOPEN: 0,0\r\n", "AT+QIOPEN=0,0,\"TCP\",\"example.com\",8883,0,0", "+QIOPEN:");

    REQUIRE(response.has_value());
    REQUIRE(response->lines == std::vector<std::string> { "+CEREG: 5", "+QIOPEN: 0,0" });
}

TEST_CASE("parseAtResponse with awaitAfterOk ends on ERROR before OK") {
    auto response = parseAtResponse("\r\nERROR\r\n", "AT+QICLOSE=0", "CLOSE OK");

    REQUIRE(response.has_value());
    REQUIRE(response->result == AtResult::Error);
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

TEST_CASE("toHex and fromHex round-trip arbitrary bytes") {
    const std::array<uint8_t, 6> bytes { 0x00, 0x0D, 0x0A, 0x7F, 0xAB, 0xFF };

    auto hex = toHex(bytes.data(), bytes.size());
    REQUIRE(hex == "000D0A7FABFF");

    std::array<uint8_t, 6> decoded {};
    REQUIRE(fromHex(hex, decoded.data()));
    REQUIRE(decoded == bytes);
    // Lowercase works too
    REQUIRE(fromHex("abff", decoded.data()));
    REQUIRE(decoded[0] == 0xAB);
}

TEST_CASE("fromHex rejects odd lengths and non-hex digits") {
    std::array<uint8_t, 2> out {};
    REQUIRE_FALSE(fromHex("ABC", out.data()));
    REQUIRE_FALSE(fromHex("AG", out.data()));
}

TEST_CASE("parseQird reads data with the remaining length") {
    auto read = parseQird("+QIRD: 3,17,\"16030A\"");

    REQUIRE(read.has_value());
    REQUIRE(read->length == 3);
    REQUIRE(read->remaining == 17);
    REQUIRE(read->hex == "16030A");
}

TEST_CASE("parseQird reads data without the remaining length") {
    auto read = parseQird("+QIRD: 2,\"3132\"");

    REQUIRE(read.has_value());
    REQUIRE(read->length == 2);
    REQUIRE_FALSE(read->remaining.has_value());
    REQUIRE(read->hex == "3132");
}

TEST_CASE("parseQird reads an empty buffer") {
    auto read = parseQird("+QIRD: 0");

    REQUIRE(read.has_value());
    REQUIRE(read->length == 0);
}

TEST_CASE("parseQird rejects data that doesn't match its length") {
    REQUIRE_FALSE(parseQird("+QIRD: 3,0,\"3132\"").has_value());
    REQUIRE_FALSE(parseQird("+QIURC: \"recv\",0").has_value());
}

TEST_CASE("parseQiopen reads the result for the connection") {
    REQUIRE(parseQiopen("+QIOPEN: 0,0", 0) == 0);
    REQUIRE(parseQiopen("+QIOPEN: 0,566", 0) == 566);
    REQUIRE_FALSE(parseQiopen("+QIOPEN: 1,0", 0).has_value());
}

TEST_CASE("parseQiurc recognizes the socket URCs") {
    auto recv = parseQiurc("+QIURC: \"recv\",0,123");
    REQUIRE(recv.has_value());
    REQUIRE(recv->type == SocketEventType::DataAvailable);
    REQUIRE(recv->connectId == 0);

    auto full = parseQiurc("+QIURC: \"recv\",0,\"buff full\"");
    REQUIRE(full.has_value());
    REQUIRE(full->type == SocketEventType::BufferFull);

    auto closed = parseQiurc("+QIURC: \"closed\",1");
    REQUIRE(closed.has_value());
    REQUIRE(closed->type == SocketEventType::Closed);
    REQUIRE(closed->connectId == 1);

    REQUIRE_FALSE(parseQiurc("+QIURC: \"incoming\",1,0").has_value());
    REQUIRE_FALSE(parseQiurc("+CEREG: 1").has_value());
}

TEST_CASE("parseModemTimestamp reads UTC timestamps") {
    // 2026-10-03T12:34:56Z
    REQUIRE(parseModemTimestamp("2026/10/03,12:34:56") == 1791030896);
    REQUIRE(parseModemTimestamp("26/10/03,12:34:56") == 1791030896);
    REQUIRE(parseModemTimestamp("1970/01/01,00:00:00") == 0);
    // Leap day
    REQUIRE(parseModemTimestamp("2024/02/29,00:00:00") == 1709164800);
}

TEST_CASE("parseModemTimestamp converts local time with a zone to UTC") {
    // The AT manual's example: 22:10:00 at GMT+2 is 20:10:00 UTC
    REQUIRE(parseModemTimestamp("14/05/06,22:10:00+08") == parseModemTimestamp("14/05/06,20:10:00"));
    REQUIRE(parseModemTimestamp("14/05/06,18:10:00-08") == parseModemTimestamp("14/05/06,20:10:00"));
}

TEST_CASE("parseModemTimestamp rejects malformed input") {
    REQUIRE_FALSE(parseModemTimestamp("").has_value());
    REQUIRE_FALSE(parseModemTimestamp("2026/10/03").has_value());
    REQUIRE_FALSE(parseModemTimestamp("2026/13/03,12:34:56").has_value());
    REQUIRE_FALSE(parseModemTimestamp("2026/10/03,12-34-56").has_value());
    REQUIRE_FALSE(parseModemTimestamp("2026/10/03,12:34:56*08").has_value());
}

TEST_CASE("parseCtzeu reads the universal time") {
    REQUIRE(parseCtzeu("+CTZEU: \"+08\",1,\"2026/10/03,12:34:56\"") == 1791030896);
    // Time zone only: the network doesn't have to send the time
    REQUIRE_FALSE(parseCtzeu("+CTZEU: \"+08\",1").has_value());
}

TEST_CASE("parseQntp reads the time of a successful sync") {
    REQUIRE(parseQntp("+QNTP: 0,\"2026/10/03,12:34:56\"") == 1791030896);
    REQUIRE_FALSE(parseQntp("+QNTP: 565").has_value());
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

TEST_CASE("encodeEdrxCycle gives the 4-bit code of NB-IoT cycles") {
    REQUIRE(encodeEdrxCycle(20480ms) == "0010");
    REQUIRE(encodeEdrxCycle(40960ms) == "0011");
    REQUIRE(encodeEdrxCycle(163840ms) == "1001");
    REQUIRE(encodeEdrxCycle(10485760ms) == "1111");
}

TEST_CASE("encodeEdrxCycle rejects other lengths") {
    REQUIRE_FALSE(encodeEdrxCycle(41000ms).has_value());
    // LTE-M only
    REQUIRE_FALSE(encodeEdrxCycle(61440ms).has_value());
    REQUIRE_FALSE(encodeEdrxCycle(0ms).has_value());
}

TEST_CASE("parseCedrxrdp reads what the network granted") {
    auto edrx = parseCedrxrdp(R"(+CEDRXRDP: 5,"0011","0101","0011")");

    REQUIRE(edrx.has_value());
    REQUIRE(edrx->active);
    REQUIRE(edrx->requested == 40960ms);
    REQUIRE(edrx->granted == 81920ms);
    REQUIRE(edrx->pagingTimeWindow == 10240ms);
    REQUIRE(describe(*edrx) == "81920 ms (requested 40960 ms), paging window 10240 ms");
}

TEST_CASE("parseCedrxrdp reports a cell without eDRX") {
    auto edrx = parseCedrxrdp("+CEDRXRDP: 0");

    REQUIRE(edrx.has_value());
    REQUIRE_FALSE(edrx->active);
    REQUIRE(describe(*edrx) == "not used on this cell");
}

TEST_CASE("parseCedrxrdp leaves out codes it doesn't know") {
    auto edrx = parseCedrxrdp(R"(+CEDRXRDP: 5,"0011","0100")");

    REQUIRE(edrx.has_value());
    REQUIRE(edrx->requested == 40960ms);
    REQUIRE_FALSE(edrx->granted.has_value());
    REQUIRE_FALSE(edrx->pagingTimeWindow.has_value());
}

TEST_CASE("parseCedrxp reads the URC") {
    auto edrx = parseCedrxp(R"(+CEDRXP: 5,"0011","0011","1111")");

    REQUIRE(edrx.has_value());
    REQUIRE(edrx->granted == 40960ms);
    REQUIRE(describe(*edrx) == "40960 ms, paging window 40960 ms");
}

TEST_CASE("parseQdrxIdleCycle reads the paging cycle while idle") {
    REQUIRE(parseQdrxIdleCycle("+QDRX: 1,1280") == 1280ms);
    REQUIRE(parseQdrxIdleCycle("+QDRX: 1,10240") == 10240ms);
}

TEST_CASE("parseQdrxIdleCycle ignores connected mode and missing cycles") {
    REQUIRE_FALSE(parseQdrxIdleCycle("+QDRX: 2,8,4,10,4,2560,8").has_value());
    REQUIRE_FALSE(parseQdrxIdleCycle("+QDRX: 1").has_value());
    REQUIRE_FALSE(parseQdrxIdleCycle("+QDRX: 0").has_value());
}
