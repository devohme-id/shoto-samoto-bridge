#include "../firmware/shoto_samoto_bridge/bridge_protocols.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

static std::vector<uint8_t> from_hex(const char* s) {
    std::vector<uint8_t> v;
    while (s[0] && s[1]) {
        if (s[0] == ' ') { ++s; continue; }
        int hi = hex_val(s[0]), lo = hex_val(s[1]);
        assert(hi >= 0 && lo >= 0);
        v.push_back(static_cast<uint8_t>((hi << 4) | lo));
        s += 2;
    }
    return v;
}

static bool verify_ydt_checksum(const char* frame, size_t len) {
    char ver[3], adr[3], cid1[3], rtn[3];
    const char* info = nullptr;
    size_t info_len = 0;
    return validate_ydt1363_frame(frame, len, ver, adr, cid1, rtn, info, info_len);
}

int main() {
    PolicyLimits lim{};

    // 1. Shoto BMS Modbus V1.2 official PDF p.10 golden TX + RX
    {
        uint8_t tx[8];
        build_shoto_modbus_v12_req(0x27, tx);
        auto exp_tx = from_hex("27040FFF00278432");
        assert(std::memcmp(tx, exp_tx.data(), 8) == 0);

        auto rx = from_hex(
            "27044E"
            "13EB271001F402B202BC0000000008001388271003E80000000F"
            "0D4E0D480D3D0D4A0D400D480D460D470D3E0D400D4F0D540D470D4C0D4DFFFF"
            "000402BC028A02BC02BC0078041A012C14C816082820");
        PackState st{};
        assert(parse_shoto_modbus_v12_resp(rx.data(), rx.size(), 0x27, 1000, lim, st));
        assert(std::fabs(st.core.bus_v - 50.99f) < 0.01f);
        assert(std::fabs(st.core.bus_a - 0.0f) < 0.01f);
        assert(std::fabs(st.core.soc - 50.0f) < 0.01f);
        assert(std::fabs(st.ext.soh - 100.0f) < 0.01f);
        assert(std::fabs(st.core.capacity_ah - 100.0f) < 0.01f);
        assert(st.ext.cell_count == 15);
        assert(st.ext.cell_mv[0] == 3406);
        assert(st.ext.cell_mv[14] == 3405);
        assert(st.ext.min_cell_mv == 3389); // 0x0D3D
        assert(st.ext.max_cell_mv == 3412); // 0x0D54
        assert(st.ext.fault_flags == 0x00);
        assert(st.ext.status_flags == 0x08);
        assert(!st.core.allow_charge);      // bit2 = 0 in 0x0800
        assert(st.core.allow_discharge);    // bit3 = 1 in 0x0800
        assert(std::fabs(st.core.cvl - 54.4f) < 0.01f); // capped from 56.4V to safe 54.4V
    }

    // 2. TianBangda Binary (TBD) golden TX + RX decode
    {
        uint8_t tx[6];
        build_tbd_binary_req(1, 0x7C, 0x01, tx);
        auto exp_tx = from_hex("7C010100020D");
        assert(std::memcmp(tx, exp_tx.data(), 6) == 0);

        uint8_t rx[86] = {0};
        rx[0] = 0x7C; rx[1] = 0x01; rx[5] = 15;
        for (int i = 0; i < 15; ++i) {
            uint16_t mv = static_cast<uint16_t>(3300 + i);
            rx[6 + 2 * i] = static_cast<uint8_t>((mv >> 8) & 0x1F);
            rx[7 + 2 * i] = static_cast<uint8_t>(mv & 0xFF);
        }
        size_t c = 30;
        // Current +12.50 A charging -> 30000 - 1250 = 28750 (0x704E)
        rx[8 + c] = 0x70; rx[9 + c] = 0x4E;
        // SOC 75.00% -> 7500 (0x1D4C)
        rx[12 + c] = 0x1D; rx[13 + c] = 0x4C;
        // Cap 100.00 Ah -> 10000 (0x2710)
        rx[16 + c] = 0x27; rx[17 + c] = 0x10;
        rx[19 + c] = 5;
        for (int k = 0; k < 5; ++k) rx[21 + c + 2 * k] = static_cast<uint8_t>(75 + k);
        size_t b = 40;
        rx[34 + b] = 0x00; rx[35 + b] = 0x0A; // 10 cycles
        rx[38 + b] = 0x14; rx[39 + b] = 0xA0; // 52.80 V
        rx[42 + b] = 0x27; rx[43 + b] = 0x10; // 100.00% SOH
        rx[84] = tbd_checksum(rx, 84);
        rx[85] = 0x0D;

        PackState st{};
        assert(parse_tbd_binary_resp(rx, sizeof(rx), 1, 2000, lim, st));
        assert(std::fabs(st.core.bus_a - 12.5f) < 0.01f);
        assert(std::fabs(st.core.soc - 75.0f) < 0.01f);
        assert(std::fabs(st.core.bus_v - 52.80f) < 0.01f);
        assert(st.ext.cycles == 10);
        assert(std::fabs(st.ext.min_temp_c - 25.0f) < 0.01f);
        assert(std::fabs(st.ext.max_temp_c - 29.0f) < 0.01f);
    }

    // 3. TianBangda YD/T 1363.3 DOC V2.8 golden TX + RX decode
    {
        char tx[64];
        size_t n = build_tbd_1363_req(1, 0x42, tx, sizeof(tx));
        assert(n > 0 && std::strcmp(tx, "~26004642E00201FD30\r") == 0);
        n = build_tbd_1363_req(1, 0x44, tx, sizeof(tx));
        assert(n > 0 && std::strcmp(tx, "~26004644E00201FD2E\r") == 0);
        n = build_tbd_1363_req(1, 0x4F, tx, sizeof(tx));
        assert(n > 0 && std::strcmp(tx, "~2600464F0000FD94\r") == 0);

        char rx[256];
        // Build valid CID2=42 response: 15 cells = 3.300V, 2 temps = 25.0C, I = +5.00A, V = 49.50V, Rem = 80Ah, Full = 100Ah, Cyc = 5
        const char* payload =
            "00010F"
            "0CE40CE40CE40CE40CE40CE40CE40CE40CE40CE40CE40CE40CE40CE40CE4"
            "020BA50BA5"
            "01F413561F400227100005";
        size_t rlen = build_ydt1363_frame("26", "00", "46", "00", payload, rx, sizeof(rx));
        PackState st{};
        assert(parse_tbd_1363_42_resp(rx, rlen, 1, 2000, lim, st));
        assert(std::fabs(st.core.bus_v - 49.50f) < 0.01f);
        assert(std::fabs(st.core.bus_a - 5.00f) < 0.01f);
        assert(std::fabs(st.core.soc - 80.00f) < 0.01f);
    }

    // 4. SmartLi DC/DC Modbus block parse
    {
        uint8_t tx[8];
        build_smartli_dcdc_req(1, 0x0000, 7, tx);
        assert(tx[0] == 1 && tx[1] == 4);

        auto build_mb_resp = [](uint8_t addr, const uint16_t* regs, size_t cnt, std::vector<uint8_t>& out) {
            out.clear();
            out.push_back(addr);
            out.push_back(0x04);
            out.push_back(static_cast<uint8_t>(cnt * 2));
            for (size_t i = 0; i < cnt; ++i) {
                out.push_back(static_cast<uint8_t>(regs[i] >> 8));
                out.push_back(static_cast<uint8_t>(regs[i] & 0xFF));
            }
            uint16_t crc = modbus_crc16(out.data(), out.size());
            out.push_back(static_cast<uint8_t>(crc & 0xFF));
            out.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        };
        uint16_t r0[7] = {5280, 5275, 0, 0, 0, 2850, 2650};
        uint16_t r1[16] = {1500, 1500, 100, 0, 88, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
        std::vector<uint8_t> b0, b1;
        build_mb_resp(1, r0, 7, b0);
        build_mb_resp(1, r1, 16, b1);
        PackState st{};
        assert(parse_smartli_dcdc_block(b0.data(), b0.size(), b1.data(), b1.size(), 1, 2000, lim, st));
        assert(std::fabs(st.core.bus_v - 52.80f) < 0.01f);
        assert(std::fabs(st.core.bus_a - 15.0f) < 0.01f);
        assert(std::fabs(st.core.soc - 88.0f) < 0.01f);
    }

    // 5. Dual-pack aggregation + Pylontech LV RS485 emulator (61/62/63/42/44/47/4F/51/92) + fail-closed safety
    {
        PackState packs[2];
        for (int i = 0; i < 2; ++i) {
            packs[i].core.complete = true;
            packs[i].core.identity_verified = true;
            packs[i].core.limits_verified = true;
            packs[i].core.allow_charge = true;
            packs[i].core.allow_discharge = true;
            packs[i].core.critical = false;
            packs[i].core.sampled_ms = 5000;
            packs[i].core.bus_v = 52.80f;
            packs[i].core.bus_a = 10.0f;
            packs[i].core.capacity_ah = 100.0f;
            packs[i].core.soc = (i == 0) ? 80.0f : 90.0f;
            packs[i].core.ccl = 60.0f;
            packs[i].core.dcl = 80.0f;
            packs[i].core.cvl = 54.4f;
            packs[i].core.dvl = 46.0f;
            packs[i].ext.cell_count = 15;
            packs[i].ext.min_cell_mv = 3310;
            packs[i].ext.max_cell_mv = 3325;
            packs[i].ext.min_temp_c = 26.0f;
            packs[i].ext.max_temp_c = 28.0f;
            packs[i].ext.avg_temp_c = 27.0f;
            packs[i].ext.soh = 100.0f;
            packs[i].ext.rem_cap_ah = (i == 0) ? 80.0f : 90.0f;
        }
        Policy pol{};
        pol.approved = true;
        pol.freshness_ms = 5000;
        pol.max_delta_v = 0.5f;
        pol.charge_cap = 50.0f;
        pol.discharge_cap = 80.0f;

        BankExt bank{};
        assert(aggregate_bank(packs, 2, 6000, pol, bank));
        assert(std::fabs(bank.core.soc - 85.0f) < 0.01f);
        assert(std::fabs(bank.core.bus_a - 20.0f) < 0.01f);
        assert(std::fabs(bank.core.ccl - 50.0f) < 0.01f); // capped conservatively

        char req[64], out[400];
        for (const char* cid2 : {"61", "62", "63", "42", "44", "47", "4F", "51", "92"}) {
            size_t rlen = build_ydt1363_frame("20", "02", "46", cid2, "", req, sizeof(req));
            size_t olen = handle_pylontech_request(req, rlen, bank, out, sizeof(out));
            assert(olen > 0 && verify_ydt_checksum(out, olen));
            if (std::strcmp(cid2, "63") == 0) {
                // Nominal 63H payload ends with status byte C0 (bit7=chg, bit6=disg) and CCL=01F4 (50.0A)
                assert(std::strstr(out, "D480B3B001F40320C0") != nullptr);
            }
        }

        // Fail-closed test 1: stale data (now_ms = 12000 > 5000 + 5000) -> 63H must inhibit (0000 0000 00)
        BankExt stale_bank{};
        assert(!aggregate_bank(packs, 2, 12000, pol, stale_bank));
        size_t rlen = build_ydt1363_frame("20", "02", "46", "63", "", req, sizeof(req));
        size_t olen = handle_pylontech_request(req, rlen, stale_bank, out, sizeof(out));
        assert(olen > 0 && verify_ydt_checksum(out, olen));
        assert(std::strstr(out, "0000000000") != nullptr);

        // Fail-closed test 2: boot (has_telemetry == false) -> silent (olen == 0)
        BankExt boot_bank{};
        assert(handle_pylontech_request(req, rlen, boot_bank, out, sizeof(out)) == 0);
    }

    // 6. Live SD-BA430-15S-V23-V22 hardware captures (140B TBD TLV @ id=1 & shifted FC04 @ id=214)
    {
        // Real 140-byte TLV response from /dev/cu.usbserial-B002C8GU on 7C 01 01 00 02 0D
        const uint8_t tlv_140b[] = {
            0x7e,0x01,0x01,0x86,
            0x01,0x0f,0x0d,0x08,0x0d,0x06,0x0d,0x05,0x0d,0x06,0x0d,0x07,0x0d,0x05,0x0d,0x07,
            0x0d,0x07,0x0d,0x06,0x0d,0x06,0x0d,0x07,0x0d,0x08,0x0d,0x08,0x0d,0x07,0x0d,0x06,
            0x02,0x01,0x75,0x30,0x03,0x01,0x20,0xe6,0x04,0x01,0x27,0x10,0x05,0x08,0x00,0x54,
            0x00,0x54,0x00,0x54,0x00,0x54,0x20,0x57,0x20,0x55,0x20,0x54,0x20,0x54,0x06,0x05,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x01,0x00,0x05,0x08,0x01,
            0x13,0x8b,0x09,0x01,0x27,0x10,0x0a,0x01,0x00,0x00,0x0b,0x01,0x00,0x00,0x01,0xe9,
            0x0c,0x01,0x00,0x00,0x01,0xd4,0x0d,0x01,0x00,0x02,0xf6,0x70,0x0e,0x01,0x00,0x02,
            0xb2,0x00,0x0f,0x01,0x00,0x00,0x61,0xcd,0x10,0x01,0x00,0x00,0x5c,0xca,0x11,0x01,
            0x13,0x8d,0x12,0x01,0x00,0x00,0xec,0x0d
        };
        static_assert(sizeof(tlv_140b) == 140, "expected 140-byte SD-BA430 TLV capture");

        PolicyLimits lim{};
        PackState p{};
        assert(parse_tbd_binary_resp(tlv_140b, sizeof(tlv_140b), 1, 1000, lim, p));
        assert(p.detected_proto == PROTO_TBD_BINARY);
        assert(p.ext.cell_count == 15);
        assert(p.ext.cell_mv[0] == 3336 && p.ext.cell_mv[14] == 3334);
        assert(std::fabs(p.core.bus_v - 50.03f) < 0.02f);
        assert(std::fabs(p.core.bus_a) < 0.01f);
        assert(std::fabs(p.core.soc - 84.22f) < 0.02f);
        assert(std::fabs(p.ext.soh - 100.0f) < 0.01f);
        assert(std::fabs(p.core.capacity_ah - 100.0f) < 0.01f);
        assert(p.ext.cycles == 5);
        assert(std::fabs(p.ext.temps_c[0] - 34.0f) < 0.1f);
        assert(p.core.allow_charge && p.core.allow_discharge && !p.core.critical);

        // Real 87-byte FC04 0x0FFF qty=41 response at addr=214 (0xD6)
        const uint8_t modbus_87b[] = {
            0xd6,0x04,0x52,
            0x13,0x86,0x27,0x10,0x03,0x48,0x02,0xe4,0x03,0x02,0x00,0x00,0x00,0x00,0x1c,0x00,
            0x20,0xe6,0x00,0x05,0x27,0x10,0x03,0x02,0x00,0x2e,0x00,0x0f,0x0d,0x08,0x0d,0x06,
            0x0d,0x04,0x0d,0x06,0x0d,0x07,0x0d,0x05,0x0d,0x08,0x0d,0x08,0x0d,0x07,0x0d,0x07,
            0x0d,0x08,0x0d,0x07,0x0d,0x08,0x0d,0x07,0x0d,0x06,0x00,0x00,0x00,0x04,0x00,0x22,
            0x00,0x22,0x00,0x22,0x00,0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0xed,0x41
        };
        static_assert(sizeof(modbus_87b) == 87, "expected 87-byte SD-BA430 FC04 capture");

        PackState pm{};
        assert(parse_shoto_modbus_v12_resp(modbus_87b, sizeof(modbus_87b), 214, 1000, lim, pm));
        assert(pm.detected_proto == PROTO_MODBUS_V12);
        assert(pm.ext.cell_count == 15);
        assert(pm.ext.cell_mv[0] == 3336 && pm.ext.cell_mv[14] == 3334);
        assert(std::fabs(pm.core.bus_v - 49.98f) < 0.02f);
        assert(std::fabs(pm.core.soc - 84.22f) < 0.02f);
        assert(std::fabs(pm.ext.soh - 100.0f) < 0.01f);
        assert(pm.ext.cycles == 5);
        assert(pm.ext.temp_count == 4 && std::fabs(pm.ext.temps_c[0] - 34.0f) < 0.1f);
        assert(pm.core.allow_charge && pm.core.allow_discharge);

        // Also verify standard 39-reg (83-byte) slice works with valid CRC
        uint8_t modbus_83b[83];
        std::memcpy(modbus_83b, modbus_87b, 81);
        modbus_83b[2] = 78;
        uint16_t crc39 = modbus_crc16(modbus_83b, 81);
        modbus_83b[81] = static_cast<uint8_t>(crc39 & 0xFFu);
        modbus_83b[82] = static_cast<uint8_t>((crc39 >> 8) & 0xFFu);
        PackState pm39{};
        assert(parse_shoto_modbus_v12_resp(modbus_83b, sizeof(modbus_83b), 214, 1000, lim, pm39));
        assert(pm39.ext.cell_count == 15 && pm39.ext.cycles == 5);
        assert(pm39.ext.temp_count == 4 && std::fabs(pm39.ext.temps_c[3] - 34.0f) < 0.1f);

        // Verify 1-pack aggregation + Pylontech 63H frame generation
        Policy pol{};
        pol.approved = true;
        pol.freshness_ms = 5000;
        pol.max_delta_v = 0.5f;
        pol.charge_cap = 50.0f;
        pol.discharge_cap = 80.0f;
        BankExt bank{};
        assert(aggregate_bank(&p, 1, 1500, pol, bank));
        char req[64], out[256];
        size_t rlen = build_ydt1363_frame("20", "02", "46", "63", "", req, sizeof(req));
        size_t olen = handle_pylontech_request(req, rlen, bank, out, sizeof(out));
        assert(olen > 0 && verify_ydt_checksum(out, olen));
    }

    std::cout << "ALL 6 C++17 bridge protocol & safety suites PASS\n";
    return 0;
}
