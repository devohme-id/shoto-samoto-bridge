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
        for (const char* cid2 : {"60", "61", "62", "63", "42", "44", "47", "4F", "51", "92"}) {
            size_t rlen = build_ydt1363_frame("20", "02", "46", cid2, "", req, sizeof(req));
            size_t olen = handle_pylontech_request(req, rlen, bank, out, sizeof(out));
            assert(olen > 0 && verify_ydt_checksum(out, olen));
            if (std::strcmp(cid2, "63") == 0) {
                // Nominal 63H payload ends with status byte C0 (bit7=chg, bit6=disg) and CCL=01F4 (50.0A)
                assert(std::strstr(out, "D480B3B001F40320C0") != nullptr);
            }
        }

        // Exact Samoto IHF6200/ILS6300 live captured V3.5 60H polls (ADR=02 & ADR=12)
        for (const char* raw60 : {"~200246600000FDAC\r", "~201246600000FDAB\r"}) {
            size_t olen60 = handle_pylontech_request(raw60, std::strlen(raw60), bank, out, sizeof(out));
            assert(olen60 > 0 && verify_ydt_checksum(out, olen60));
            assert(std::strstr(out, "466F7263655F4C000000") != nullptr);
            assert(std::strstr(out, "50796C6F6E") != nullptr);
        }
        assert(handle_pylontech_request("~200246600000FDAC\r", 18, bank, out, sizeof(out)) > 0 &&
               std::strncmp(out, "~200246006082", 13) == 0);
        assert(handle_pylontech_request("~201246600000FDAB\r", 18, bank, out, sizeof(out)) > 0 &&
               std::strncmp(out, "~201246006082", 13) == 0);

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
        assert(handle_pylontech_request("~200246600000FDAC\r", 18, boot_bank, out, sizeof(out)) == 0);
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
        assert(p.ext.min_cell_mv == 3333 && p.ext.min_cell_idx == 3);
        assert(p.ext.max_cell_mv == 3336 && p.ext.max_cell_idx == 1);
        BankExt bank{};
        assert(aggregate_bank(&p, 1, 1500, pol, bank));
        assert(bank.min_cell_pack == 1 && bank.min_cell_idx == 3);
        assert(bank.max_cell_pack == 1 && bank.max_cell_idx == 1);
        char req[64], out[256];
        size_t rlen = build_ydt1363_frame("20", "02", "46", "63", "", req, sizeof(req));
        size_t olen = handle_pylontech_request(req, rlen, bank, out, sizeof(out));
        assert(olen > 0 && verify_ydt_checksum(out, olen));
        size_t olen61 = handle_pylontech_request("~200246610000FDAB\r", 18, bank, out, sizeof(out));
        assert(olen61 > 18 && verify_ydt_checksum(out, olen61));
        char hex61[128];
        size_t hlen61 = olen61 - 18;
        assert(hlen61 < sizeof(hex61));
        std::memcpy(hex61, out + 13, hlen61);
        hex61[hlen61] = '\0';
        auto info61 = from_hex(hex61);
        assert(info61.size() == 49);
        assert(((info61[13] << 8) | info61[14]) == 101); // Max Cell: Pack 1 Cell 1 -> ID:1(1)
        assert(((info61[17] << 8) | info61[18]) == 103); // Min Cell: Pack 1 Cell 3 -> ID:1(3)
        assert(((info61[23] << 8) | info61[24]) == 101); // Temp pos: Pack 1 Cell 1 -> ID:1(1)

        // 6a. Leading TX->RX turnaround glitch bytes prepended to 140-byte TLV frame still parse cleanly
        uint8_t glitch_tlv[2 + sizeof(tlv_140b)];
        glitch_tlv[0] = 0x00;
        glitch_tlv[1] = 0xFF;
        std::memcpy(glitch_tlv + 2, tlv_140b, sizeof(tlv_140b));
        PackState pg{};
        assert(parse_tbd_binary_resp(glitch_tlv, sizeof(glitch_tlv), 1, 1000, lim, pg));
        assert(std::fabs(pg.core.soc - 84.22f) < 0.02f);
        assert(std::fabs(pg.core.bus_v - 50.03f) < 0.02f);

        // 6b. Leading turnaround glitch bytes prepended to 87-byte Modbus V1.2 frame still parse cleanly
        uint8_t glitch_mb[2 + sizeof(modbus_87b)];
        glitch_mb[0] = 0x00;
        glitch_mb[1] = 0xFF;
        std::memcpy(glitch_mb + 2, modbus_87b, sizeof(modbus_87b));
        PackState pmg{};
        assert(parse_shoto_modbus_v12_resp(glitch_mb, sizeof(glitch_mb), 214, 1000, lim, pmg));
        assert(std::fabs(pmg.core.soc - 84.22f) < 0.02f);
        assert(std::fabs(pmg.core.bus_v - 49.98f) < 0.02f);

        // 6c. Corrupted TLV frame where pack bus_v disagrees with sum of cells by >3V is rejected even with valid checksum
        uint8_t bad_tlv[sizeof(tlv_140b)];
        std::memcpy(bad_tlv, tlv_140b, sizeof(tlv_140b));
        // Tag 0x08 (bus_v) sits at bytes [84..85]: 0x13 0x8B (50.03V) -> overwrite to 0x17 0x70 (60.00V)
        bad_tlv[84] = 0x17;
        bad_tlv[85] = 0x70;
        bad_tlv[sizeof(bad_tlv) - 2] = tbd_checksum(bad_tlv, sizeof(bad_tlv) - 2);
        PackState p_bad{};
        assert(!parse_tbd_binary_resp(bad_tlv, sizeof(bad_tlv), 1, 1000, lim, p_bad));

        // 6d. Unapproved policy preserves raw_soc diagnostic while failing closed on safety core
        Policy unapproved_pol = pol;
        unapproved_pol.approved = false;
        BankExt unapproved_bank{};
        assert(!aggregate_bank(&p, 1, 1500, unapproved_pol, unapproved_bank));
        assert(!unapproved_bank.core.valid);
        assert(unapproved_bank.core.ccl == 0.0f);
        assert(unapproved_bank.core.dcl == 0.0f);
        assert(unapproved_bank.core.soc == 0.0f);
        assert(std::fabs(unapproved_bank.raw_soc - 84.22f) < 0.02f);

        // 6e. Corrupted/mismatched TLV header frame with valid outer checksum must NOT fall through into Format A
        uint8_t bad_tlv_plen[sizeof(tlv_140b)];
        std::memcpy(bad_tlv_plen, tlv_140b, sizeof(tlv_140b));
        bad_tlv_plen[3] = 0x84; // triggers Format B mismatch while keeping outer EOI 0x0D & valid tbd_checksum
        bad_tlv_plen[sizeof(bad_tlv_plen) - 2] = tbd_checksum(bad_tlv_plen, sizeof(bad_tlv_plen) - 2);
        PackState p_fallthrough{};
        assert(!parse_tbd_binary_resp(bad_tlv_plen, sizeof(bad_tlv_plen), 1, 1000, lim, p_fallthrough));

        // 6f. 1-pack Samoto V3.5 60H response (~201246600000FDAB -> LENID 8062 / 98 hex chars)
        size_t olen60_1 = handle_pylontech_request("~201246600000FDAB\r", 18, bank, out, sizeof(out));
        assert(olen60_1 > 0 && verify_ydt_checksum(out, olen60_1));
        assert(std::strncmp(out, "~201246008062", 13) == 0);

        // 6g. Normal -10A discharging (SD-BA430 tag 0x06 word 1 = 0x0002) must NOT trigger Pylontech 62H Cell UV alarm
        uint8_t dis_tlv[sizeof(tlv_140b)];
        std::memcpy(dis_tlv, tlv_140b, sizeof(tlv_140b));
        dis_tlv[38] = 0x79; dis_tlv[39] = 0x18; // -10.00A -> 31000
        dis_tlv[70] = 0x00; dis_tlv[71] = 0x02; // live tag 0x06 word 1 = 0x0002 (discharging)
        dis_tlv[sizeof(dis_tlv) - 2] = tbd_checksum(dis_tlv, sizeof(dis_tlv) - 2);
        PackState p_dis{};
        assert(parse_tbd_binary_resp(dis_tlv, sizeof(dis_tlv), 1, 1600, lim, p_dis));
        assert(p_dis.ext.warn_flags == 0u && p_dis.ext.prot_flags == 0u && p_dis.ext.status_flags == 0x02u);
        BankExt dis_bank{};
        assert(aggregate_bank(&p_dis, 1, 1600, pol, dis_bank));
        size_t olen62_ok = handle_pylontech_request("~200246620000FDAA\r", 18, dis_bank, out, sizeof(out));
        assert(olen62_ok > 0 && verify_ydt_checksum(out, olen62_ok));
        assert(std::strstr(out, "00000000") != nullptr && std::strstr(out, "10000000") == nullptr);

        // Genuine low cell (2750mV <= 2800mV) MUST still raise Pylontech 62H Cell UV alarm (a1 = 0x10)
        uint8_t uv_tlv[sizeof(dis_tlv)];
        std::memcpy(uv_tlv, dis_tlv, sizeof(dis_tlv));
        uv_tlv[6] = 0x0A; uv_tlv[7] = 0xBE; // cell 0 = 2750 mV
        uv_tlv[sizeof(uv_tlv) - 2] = tbd_checksum(uv_tlv, sizeof(uv_tlv) - 2);
        PackState p_uv{};
        assert(parse_tbd_binary_resp(uv_tlv, sizeof(uv_tlv), 1, 1600, lim, p_uv));
        assert((p_uv.ext.warn_flags & 0x0002u) != 0 && p_uv.ext.prot_flags == 0u);
        BankExt uv_bank{};
        assert(aggregate_bank(&p_uv, 1, 1600, pol, uv_bank));
        size_t olen62_uv = handle_pylontech_request("~200246620000FDAA\r", 18, uv_bank, out, sizeof(out));
        assert(olen62_uv > 0 && verify_ydt_checksum(out, olen62_uv));
        assert(std::strstr(out, "10000000") != nullptr);
    }

    // 7. 3-pack IHF6200 aggregation (300Ah, CCL=90A, DCL=150A) + Pylontech V3.5 60H 3-barcode response
    {
        PackState packs[3];
        for (int i = 0; i < 3; ++i) {
            packs[i].addr = static_cast<uint8_t>(i + 1);
            packs[i].core.complete = true;
            packs[i].core.identity_verified = true;
            packs[i].core.limits_verified = true;
            packs[i].core.allow_charge = true;
            packs[i].core.allow_discharge = true;
            packs[i].core.critical = false;
            packs[i].core.sampled_ms = 5000;
            packs[i].core.bus_v = 50.00f;
            packs[i].core.capacity_ah = 100.0f;
            packs[i].core.soc = 80.0f + 5.0f * i;       // 80, 85, 90% -> avg 85%
            packs[i].core.bus_a = -40.0f - 5.0f * i;    // -40, -45, -50A -> sum -135A
            packs[i].core.ccl = 90.0f;
            packs[i].core.dcl = 150.0f;
            packs[i].core.cvl = 53.6f;
            packs[i].core.dvl = 46.0f;
            packs[i].ext.cell_count = 15;
            packs[i].ext.min_cell_mv = static_cast<uint16_t>(3325 + i);
            packs[i].ext.min_cell_idx = static_cast<uint8_t>(4 + i);
            packs[i].ext.max_cell_mv = static_cast<uint16_t>(3335 + i);
            packs[i].ext.max_cell_idx = static_cast<uint8_t>(15 - i);
            packs[i].ext.soh = 100.0f;
            packs[i].ext.rem_cap_ah = packs[i].core.soc;
        }
        Policy pol{};
        pol.approved = true;
        pol.freshness_ms = 6000;
        pol.max_delta_v = 1.0f;
        pol.charge_cap = 90.0f;
        pol.discharge_cap = 150.0f;

        BankExt bank2{};
        assert(aggregate_bank(packs, 2, 6000, pol, bank2));
        assert(bank2.min_cell_pack == 1 && bank2.min_cell_idx == 4);
        assert(bank2.max_cell_pack == 2 && bank2.max_cell_idx == 14);

        BankExt bank3{};
        assert(aggregate_bank(packs, 3, 6000, pol, bank3));
        assert(bank3.pack_count == 3);
        assert(std::fabs(bank3.core.capacity_ah - 300.0f) < 0.01f);
        assert(std::fabs(bank3.core.soc - 85.0f) < 0.01f);
        assert(std::fabs(bank3.core.bus_a + 135.0f) < 0.01f);
        assert(std::fabs(bank3.core.ccl - 90.0f) < 0.01f);
        assert(std::fabs(bank3.core.dcl - 150.0f) < 0.01f);
        assert(bank3.min_cell_mv == 3325 && bank3.max_cell_mv == 3337);
        assert(bank3.min_cell_pack == 1 && bank3.min_cell_idx == 4);
        assert(bank3.max_cell_pack == 3 && bank3.max_cell_idx == 13);

        char out[400];
        size_t olen60 = handle_pylontech_request("~200246600000FDAC\r", 18, bank3, out, sizeof(out));
        assert(olen60 > 0 && verify_ydt_checksum(out, olen60));
        assert(std::strncmp(out, "~2002460040A2", 13) == 0);
        assert(std::strstr(out, "42343831303042323130544537303033") != nullptr);

        size_t olen61 = handle_pylontech_request("~200246610000FDAB\r", 18, bank3, out, sizeof(out));
        assert(olen61 > 18 && verify_ydt_checksum(out, olen61));
        char hex61_3[128];
        size_t hlen61_3 = olen61 - 18;
        assert(hlen61_3 < sizeof(hex61_3));
        std::memcpy(hex61_3, out + 13, hlen61_3);
        hex61_3[hlen61_3] = '\0';
        auto info61_3 = from_hex(hex61_3);
        assert(info61_3.size() == 49);
        assert(((info61_3[13] << 8) | info61_3[14]) == 313); // Max Cell: Pack 3 Cell 13 -> ID:3(13)
        assert(((info61_3[17] << 8) | info61_3[18]) == 104); // Min Cell: Pack 1 Cell 4  -> ID:1(4)

        // Fail-closed: Pack #3 delta-V > 1.0V must invalidate whole bank
        packs[2].core.bus_v = 51.5f;
        BankExt bad_v{};
        assert(!aggregate_bank(packs, 3, 6000, pol, bad_v));
        assert(!bad_v.core.valid);

        // Fail-closed: Pack #3 stale > 6000ms must invalidate whole bank
        packs[2].core.bus_v = 50.0f;
        BankExt stale3{};
        assert(!aggregate_bank(packs, 3, 12000, pol, stale3));
        assert(!stale3.core.valid);
    }

    // 8) Runtime dynamic pack scaling & hot-plug / hot-unplug transitions (0..4 packs)
    {
        auto approx = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        DynamicLimits dl;
        dl.ccl_1pack_a = 45.0f;
        dl.dcl_1pack_a = 80.0f;
        dl.ccl_per_pack_a = 30.0f;
        dl.dcl_per_pack_a = 50.0f;

        float ccl = -1.0f, dcl = -1.0f;
        compute_dynamic_caps(0, dl, ccl, dcl);
        assert(approx(ccl, 0.0f) && approx(dcl, 0.0f));
        compute_dynamic_caps(1, dl, ccl, dcl);
        assert(approx(ccl, 45.0f) && approx(dcl, 80.0f));
        compute_dynamic_caps(2, dl, ccl, dcl);
        assert(approx(ccl, 60.0f) && approx(dcl, 100.0f));
        compute_dynamic_caps(3, dl, ccl, dcl);
        assert(approx(ccl, 90.0f) && approx(dcl, 150.0f));
        compute_dynamic_caps(4, dl, ccl, dcl);
        assert(approx(ccl, 120.0f) && approx(dcl, 200.0f));

        DynamicLimits bad_dl;
        bad_dl.ccl_1pack_a = -5.0f;
        bad_dl.dcl_1pack_a = std::numeric_limits<float>::quiet_NaN();
        compute_dynamic_caps(1, bad_dl, ccl, dcl);
        assert(approx(ccl, 0.0f) && approx(dcl, 0.0f));

        PackState packs[4]{};
        for (int i = 0; i < 4; ++i) {
            packs[i].addr = static_cast<uint8_t>(i + 1);
            packs[i].core.complete = true;
            packs[i].core.identity_verified = true;
            packs[i].core.limits_verified = true;
            packs[i].core.critical = false;
            packs[i].core.sampled_ms = 5000;
            packs[i].core.bus_v = 51.20f + 0.02f * static_cast<float>(i);
            packs[i].core.bus_a = 10.0f;
            packs[i].core.soc = 80.0f;
            packs[i].core.capacity_ah = 100.0f;
            packs[i].core.ccl = 120.0f;
            packs[i].core.dcl = 200.0f;
            packs[i].core.cvl = 53.6f;
            packs[i].core.dvl = 46.0f;
            packs[i].core.allow_charge = true;
            packs[i].core.allow_discharge = true;
            packs[i].ext.rem_cap_ah = 80.0f;
            packs[i].ext.soh = 100.0f;
            packs[i].ext.cell_count = 15;
            packs[i].ext.min_cell_mv = 3412;
            packs[i].ext.max_cell_mv = 3415;
            packs[i].ext.avg_temp_c = 30.0f;
        }

        Policy pol;
        pol.approved = true;
        pol.freshness_ms = 6000;
        pol.max_delta_v = 1.0f;

        auto verify_step = [&](const PackState* active, size_t n, float exp_cap, float exp_ccl, float exp_dcl) {
            compute_dynamic_caps(n, dl, pol.charge_cap, pol.discharge_cap);
            BankExt bank{};
            assert(aggregate_bank(active, n, 6000, pol, bank));
            assert(bank.core.valid);
            assert(bank.pack_count == static_cast<uint8_t>(n));
            assert(approx(bank.core.capacity_ah, exp_cap));
            assert(approx(bank.core.ccl, exp_ccl));
            assert(approx(bank.core.dcl, exp_dcl));

            char out[512];
            size_t olen60 = handle_pylontech_request("~200246600000FDAC\r", 18, bank, out, sizeof(out));
            assert(olen60 > 18 && verify_ydt_checksum(out, olen60));
            assert(out[olen60 - 1] == '\r');
            // Barcodes count matches runtime pack_count (33B device header = 66 hex + n * 16B barcode = 32 hex)
            size_t data_hex_len = olen60 - 1 - 13 - 4;
            assert(data_hex_len == 66u + n * 32u);
            if (n == 4) {
                assert(std::strstr(out, "42343831303042323130544537303034") != nullptr);
            }
        };

        // 1 -> 2 (ILS3600) -> 3 (IHF6200) -> 4 packs -> hot-unplug to 1 pack (slot #2)
        verify_step(packs, 1, 100.0f, 45.0f, 80.0f);
        verify_step(packs, 2, 200.0f, 60.0f, 100.0f);
        verify_step(packs, 3, 300.0f, 90.0f, 150.0f);
        verify_step(packs, 4, 400.0f, 120.0f, 200.0f);
        verify_step(&packs[1], 1, 100.0f, 45.0f, 80.0f);

        // Hot-unplug all -> 0 packs -> fail-closed silent boot state
        compute_dynamic_caps(0, dl, pol.charge_cap, pol.discharge_cap);
        BankExt zero_bank{};
        assert(!aggregate_bank(packs, 0, 6000, pol, zero_bank));
        assert(!zero_bank.core.valid);
        assert(!zero_bank.has_telemetry);
        assert(zero_bank.pack_count == 0);
        assert(approx(zero_bank.core.ccl, 0.0f) && approx(zero_bank.core.dcl, 0.0f));
        char out0[256];
        assert(handle_pylontech_request("~200246600000FDAC\r", 18, zero_bank, out0, sizeof(out0)) == 0);
        assert(handle_pylontech_request("~200246630000FDA9\r", 18, zero_bank, out0, sizeof(out0)) == 0);

        // Hot-plug voltage mismatch > 1.0V -> fail-closed 0A inhibit
        PackState mismatch[2] = {packs[0], packs[1]};
        mismatch[0].core.bus_v = 50.00f;
        mismatch[1].core.bus_v = 51.60f;
        compute_dynamic_caps(2, dl, pol.charge_cap, pol.discharge_cap);
        BankExt mm_bank{};
        assert(!aggregate_bank(mismatch, 2, 6000, pol, mm_bank));
        assert(!mm_bank.core.valid);
        assert(approx(mm_bank.core.ccl, 0.0f) && approx(mm_bank.core.dcl, 0.0f));
        char out_mm[256];
        size_t olen63 = handle_pylontech_request("~200246630000FDA9\r", 18, mm_bank, out_mm, sizeof(out_mm));
        assert(olen63 > 18 && verify_ydt_checksum(out_mm, olen63));
        char info_hex[64];
        size_t hlen = olen63 - 18;
        assert(hlen < sizeof(info_hex));
        std::memcpy(info_hex, out_mm + 13, hlen);
        info_hex[hlen] = '\0';
        auto info_mm = from_hex(info_hex);
        assert(info_mm.size() == 9 && info_mm[4] == 0x00 && info_mm[5] == 0x00 &&
               info_mm[6] == 0x00 && info_mm[7] == 0x00 && info_mm[8] == 0x00); // Charge & Discharge inhibit (9B 63H payload)
    }

    // 9) Read-only TBD protection & alarm parameter decoders (TianBangda baohucs / gaojcs_read)
    {
        auto build_param_resp = [](uint8_t addr, uint8_t cmd, const std::vector<uint8_t>& payload) {
            std::vector<uint8_t> f;
            f.push_back(0x7E);
            f.push_back(addr);
            f.push_back(cmd);
            f.push_back(static_cast<uint8_t>(payload.size()));
            f.insert(f.end(), payload.begin(), payload.end());
            f.push_back(tbd_checksum(f.data(), f.size()));
            f.push_back(0x0D);
            return f;
        };

        // 10-byte (plen=4): [0x2E, 0x01, hi, lo] for cmd=0x2E (43.20V=4320), cmd=0x2F (48.00V=4800), cmd=0x35 (56.00V=5600)
        auto f_cut = build_param_resp(1, 0x2E, {0x2E, 0x01, 0x10, 0xE0});
        auto f_rec = build_param_resp(1, 0x2F, {0x2F, 0x01, 0x12, 0xC0});
        auto f_chg = build_param_resp(1, 0x35, {0x35, 0x01, 0x15, 0xE0});
        auto f_short = build_param_resp(1, 0x32, {0x0C, 0x4E}); // 8-byte (plen=2): 3150 mV
        uint16_t v_cut = 0, v_rec = 0, v_chg = 0, v_short = 0;
        assert(parse_tbd_param_u16(f_cut.data(), f_cut.size(), 1, 0x2E, v_cut) && v_cut == 4320u);
        assert(parse_tbd_param_u16(f_rec.data(), f_rec.size(), 1, 0x2F, v_rec) && v_rec == 4800u);
        assert(parse_tbd_param_u16(f_chg.data(), f_chg.size(), 1, 0x35, v_chg) && v_chg == 5600u);
        assert(parse_tbd_param_u16(f_short.data(), f_short.size(), 1, 0x32, v_short) && v_short == 3150u);

        auto f_bad = f_cut;
        f_bad[f_bad.size() - 2] ^= 0x55u;
        assert(!parse_tbd_param_u16(f_bad.data(), f_bad.size(), 1, 0x2E, v_cut));

        // cmd=0x43 alarm parameter table (4 bytes per record: tag, pad, hi, lo)
        std::vector<uint8_t> p43;
        auto add_tag = [&](uint8_t tag, uint16_t val) {
            p43.push_back(tag);
            p43.push_back(0x01);
            p43.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
            p43.push_back(static_cast<uint8_t>(val & 0xFFu));
        };
        add_tag(0x00, 3650u);
        add_tag(0x02, 3350u);
        add_tag(0x03, 2650u);
        add_tag(0x05, 2900u);
        add_tag(0x06, 5700u);
        add_tag(0x08, 5460u);
        add_tag(0x09, 4600u);
        add_tag(0x0B, 5120u);
        add_tag(0x21, 30u);
        add_tag(0x22, 40u);
        add_tag(0x23, 800u);
        add_tag(0x24, 500u);
        auto f43 = build_param_resp(1, 0x43, p43);
        TbdAlarmParams ap{};
        assert(parse_tbd_alarm_params_43(f43.data(), f43.size(), 1, ap));
        assert(ap.valid && ap.tag_count == 12u);
        assert(ap.cell_ov_alarm_mv == 3650u && ap.cell_ov_alarm_rec_mv == 3350u);
        assert(ap.cell_uv_alarm_mv == 2650u && ap.cell_uv_alarm_rec_mv == 2900u);
        assert(ap.pack_ov_alarm_cv == 5700u && ap.pack_ov_alarm_rec_cv == 5460u);
        assert(ap.pack_uv_alarm_cv == 4600u && ap.pack_uv_alarm_rec_cv == 5120u);
        assert(ap.low_soc_alarm_pct == 30u && ap.low_soc_alarm_rec_pct == 40u);
        assert(ap.cell_diff_alarm_mv == 800u && ap.cell_diff_alarm_rec_mv == 500u);

        auto f43_bad = f43;
        f43_bad[f43_bad.size() - 2] ^= 0xFFu;
        TbdAlarmParams ap_bad{};
        assert(!parse_tbd_alarm_params_43(f43_bad.data(), f43_bad.size(), 1, ap_bad) && !ap_bad.valid);
    }

    // 10) Bank-level CCL/DCL caps (IHF6200 90A/120A) + 63H frame + InhibitReason classifier
    {
        auto approx = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        DynamicLimits dl_cap;
        dl_cap.ccl_1pack_a = 45.0f;
        dl_cap.dcl_1pack_a = 80.0f;
        dl_cap.ccl_per_pack_a = 30.0f;
        dl_cap.dcl_per_pack_a = 50.0f;
        dl_cap.ccl_max_a = 90.0f;
        dl_cap.dcl_max_a = 120.0f;

        float ccl = -1.0f, dcl = -1.0f;
        compute_dynamic_caps(0, dl_cap, ccl, dcl);
        assert(approx(ccl, 0.0f) && approx(dcl, 0.0f));
        compute_dynamic_caps(1, dl_cap, ccl, dcl);
        assert(approx(ccl, 45.0f) && approx(dcl, 80.0f));
        compute_dynamic_caps(2, dl_cap, ccl, dcl);
        assert(approx(ccl, 60.0f) && approx(dcl, 100.0f));
        compute_dynamic_caps(3, dl_cap, ccl, dcl);
        assert(approx(ccl, 90.0f) && approx(dcl, 120.0f));
        compute_dynamic_caps(4, dl_cap, ccl, dcl);
        assert(approx(ccl, 90.0f) && approx(dcl, 120.0f));

        DynamicLimits dl_uncapped;
        dl_uncapped.ccl_1pack_a = 45.0f;
        dl_uncapped.dcl_1pack_a = 80.0f;
        dl_uncapped.ccl_per_pack_a = 30.0f;
        dl_uncapped.dcl_per_pack_a = 50.0f;
        compute_dynamic_caps(3, dl_uncapped, ccl, dcl);
        assert(approx(ccl, 90.0f) && approx(dcl, 150.0f));
        compute_dynamic_caps(4, dl_uncapped, ccl, dcl);
        assert(approx(ccl, 120.0f) && approx(dcl, 200.0f));

        PackState packs[3]{};
        for (int i = 0; i < 3; ++i) {
            packs[i].addr = static_cast<uint8_t>(i + 1);
            packs[i].core.complete = true;
            packs[i].core.identity_verified = true;
            packs[i].core.limits_verified = true;
            packs[i].core.allow_charge = true;
            packs[i].core.allow_discharge = true;
            packs[i].core.critical = false;
            packs[i].core.sampled_ms = 5000;
            packs[i].core.bus_v = 49.80f + 0.02f * static_cast<float>(i);
            packs[i].core.capacity_ah = 100.0f;
            packs[i].core.soc = 80.0f;
            packs[i].core.bus_a = -3.0f;
            packs[i].core.ccl = 120.0f;
            packs[i].core.dcl = 200.0f;
            packs[i].core.cvl = 53.6f;
            packs[i].core.dvl = 46.0f;
            packs[i].ext.cell_count = 15;
            packs[i].ext.min_cell_mv = 3315;
            packs[i].ext.max_cell_mv = 3325;
            packs[i].ext.soh = 99.0f;
        }
        Policy pol;
        pol.approved = true;
        pol.freshness_ms = 9000;
        pol.max_delta_v = 1.0f;
        compute_dynamic_caps(3, dl_cap, pol.charge_cap, pol.discharge_cap);

        BankExt b3{};
        float dv = -1.0f;
        uint32_t age = 999999u;
        assert(aggregate_bank(packs, 3, 6000, pol, b3));
        assert(classify_bank_inhibit(packs, 3, 6000, pol, &dv, &age) == INHIBIT_NONE);
        assert(approx(dv, 0.04f) && age == 1000u);
        assert(approx(b3.core.ccl, 90.0f) && approx(b3.core.dcl, 120.0f));

        char out63[256];
        size_t o63 = handle_pylontech_request("~200246630000FDA9\r", 18, b3, out63, sizeof(out63));
        assert(o63 > 18 && verify_ydt_checksum(out63, o63));
        assert(std::strstr(out63, "D160B3B0038404B0C0") != nullptr); // 53.6V, 46.0V, 90.0A, 120.0A, 0xC0

        // Classify all 8 InhibitReason cases + 1:1 agreement with aggregate_bank()
        assert(std::strcmp(inhibit_reason_name(INHIBIT_NONE), "NONE") == 0);
        assert(classify_bank_inhibit(packs, 0, 6000, pol, &dv, &age) == INHIBIT_NO_PACKS);
        assert(approx(dv, 0.0f) && age == 0u);
        assert(std::strcmp(inhibit_reason_name(INHIBIT_NO_PACKS), "NO_PACKS") == 0);

        Policy pol_bad = pol;
        pol_bad.approved = false;
        assert(classify_bank_inhibit(packs, 3, 6000, pol_bad) == INHIBIT_POLICY);
        assert(!aggregate_bank(packs, 3, 6000, pol_bad, b3));
        assert(std::strcmp(inhibit_reason_name(INHIBIT_POLICY), "POLICY") == 0);

        PackState p_crit[3] = {packs[0], packs[1], packs[2]};
        p_crit[1].core.critical = true;
        assert(classify_bank_inhibit(p_crit, 3, 6000, pol) == INHIBIT_CRITICAL);
        assert(!aggregate_bank(p_crit, 3, 6000, pol, b3));
        assert(std::strcmp(inhibit_reason_name(INHIBIT_CRITICAL), "CRITICAL") == 0);

        assert(classify_bank_inhibit(packs, 3, 15001, pol, &dv, &age) == INHIBIT_STALE);
        assert(age == 10001u);
        assert(!aggregate_bank(packs, 3, 15001, pol, b3));
        assert(std::strcmp(inhibit_reason_name(INHIBIT_STALE), "STALE") == 0);

        PackState p_inc[3] = {packs[0], packs[1], packs[2]};
        p_inc[2].core.complete = false;
        assert(classify_bank_inhibit(p_inc, 3, 6000, pol) == INHIBIT_INCOMPLETE);
        assert(!aggregate_bank(p_inc, 3, 6000, pol, b3));
        assert(std::strcmp(inhibit_reason_name(INHIBIT_INCOMPLETE), "INCOMPLETE") == 0);

        PackState p_cross[2] = {packs[0], packs[1]};
        p_cross[0].core.cvl = 50.0f; p_cross[0].core.dvl = 46.0f;
        p_cross[1].core.cvl = 53.6f; p_cross[1].core.dvl = 51.0f;
        assert(classify_bank_inhibit(p_cross, 2, 6000, pol) == INHIBIT_LIMITS);
        assert(!aggregate_bank(p_cross, 2, 6000, pol, b3));
        assert(std::strcmp(inhibit_reason_name(INHIBIT_LIMITS), "LIMITS") == 0);

        PackState p_dv[3] = {packs[0], packs[1], packs[2]};
        p_dv[2].core.bus_v = 51.20f; // delta_v = 51.20 - 49.80 = 1.40V > 1.0V
        assert(classify_bank_inhibit(p_dv, 3, 6000, pol, &dv, &age) == INHIBIT_DELTA_V);
        assert(approx(dv, 1.40f));
        assert(!aggregate_bank(p_dv, 3, 6000, pol, b3));
        assert(std::strcmp(inhibit_reason_name(INHIBIT_DELTA_V), "DELTA_V") == 0);
    }

    std::cout << "ALL 10 C++17 bridge protocol & safety suites PASS\n";
    return 0;
}
