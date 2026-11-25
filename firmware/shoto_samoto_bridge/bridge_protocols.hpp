#pragma once
/**
 * Portable C++17 protocol codecs, safety aggregator, and Pylontech LV RS485 emulator.
 * Facts verified from:
 *  - Shoto BMS Modbus Communication Protocol V1.2_EN-20190110.pdf (Product: SDA10-48100)
 *  - TianBangda LFP MS-ShotoV1.1.634-26.exe (REA managed analysis ev_f1b14058...)
 *  - BMS Communication Protocol_V2_8_180807.doc (YD/T 1363.3 ASCII)
 *  - hpeyerl/esphome-shoto-smartli (SmartLi DC/DC candidate)
 *  - Fahmula/esphome-pylontech-rs485 (CID2 0x61/0x62/0x63) + Pylontech V3.3 (0x42/0x44/0x47/0x4F/0x51)
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "safety_core.hpp"

enum ShotoProtoKind : uint8_t {
    PROTO_AUTO = 0,
    PROTO_MODBUS_V12 = 1,
    PROTO_TBD_BINARY = 2,
    PROTO_TBD_1363 = 3,
    PROTO_SMARTLI_DCDC = 4,
};

struct PolicyLimits {
    float cvl_v = 54.4f;
    float dvl_v = 46.0f;
    float ccl_a = 50.0f;
    float dcl_a = 80.0f;
    float default_capacity_ah = 100.0f;
};

struct DynamicLimits {
    float ccl_1pack_a = 45.0f;
    float dcl_1pack_a = 80.0f;
    float ccl_per_pack_a = 30.0f;
    float dcl_per_pack_a = 50.0f;
    float ccl_max_a = 0.0f; // 0 = uncapped
    float dcl_max_a = 0.0f; // 0 = uncapped
};

inline void compute_dynamic_caps(
    size_t online_packs, const DynamicLimits& dl, float& ccl_out, float& dcl_out)
{
    auto non_neg = [](float v) -> float {
        return (finite(v) && v > 0.0f) ? v : 0.0f;
    };
    if (online_packs == 0u) {
        ccl_out = 0.0f;
        dcl_out = 0.0f;
        return;
    }
    if (online_packs == 1u) {
        ccl_out = non_neg(dl.ccl_1pack_a);
        dcl_out = non_neg(dl.dcl_1pack_a);
    } else {
        float n = static_cast<float>(online_packs);
        ccl_out = non_neg(dl.ccl_per_pack_a) * n;
        dcl_out = non_neg(dl.dcl_per_pack_a) * n;
    }
    if (finite(dl.ccl_max_a) && dl.ccl_max_a > 0.0f) ccl_out = std::min(ccl_out, dl.ccl_max_a);
    if (finite(dl.dcl_max_a) && dl.dcl_max_a > 0.0f) dcl_out = std::min(dcl_out, dl.dcl_max_a);
}

struct TelemetryExt {
    uint8_t cell_count = 0;
    uint16_t cell_mv[16] = {0};
    uint16_t min_cell_mv = 0;
    uint16_t max_cell_mv = 0;
    uint8_t min_cell_idx = 1;
    uint8_t max_cell_idx = 1;
    uint8_t temp_count = 0;
    float temps_c[5] = {0};
    float min_temp_c = 25.0f;
    float max_temp_c = 25.0f;
    float avg_temp_c = 25.0f;
    float soh = 100.0f;
    float rem_cap_ah = 0.0f;
    uint16_t cycles = 0;
    uint16_t warn_flags = 0;
    uint16_t prot_flags = 0;
    uint8_t fault_flags = 0;
    uint8_t status_flags = 0;
    uint8_t online_count = 1;
};

struct PackState {
    Pack core;
    TelemetryExt ext;
    ShotoProtoKind detected_proto = PROTO_AUTO;
    uint8_t addr = 0;
};

struct BankExt {
    Bank core;
    bool has_telemetry = false;
    uint8_t pack_count = 0;
    float bus_v = 0.0f;
    float raw_soc = 0.0f;
    float raw_bus_a = 0.0f;
    float rem_cap_ah = 0.0f;
    float soh = 0.0f;
    float min_temp_c = 25.0f;
    float max_temp_c = 25.0f;
    float avg_temp_c = 25.0f;
    uint16_t min_cell_mv = 0;
    uint16_t max_cell_mv = 0;
    uint8_t min_cell_pack = 1;
    uint8_t min_cell_idx = 1;
    uint8_t max_cell_pack = 1;
    uint8_t max_cell_idx = 1;
    uint16_t cycles = 0;
    uint16_t warn_flags = 0;
    uint16_t prot_flags = 0;
    uint8_t fault_flags = 0;
    uint8_t cell_count = 0;
    uint16_t cell_mv[16] = {0};
};

// -------------------- Checksums --------------------

inline uint16_t modbus_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                            : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

inline uint8_t tbd_checksum(const uint8_t* data, size_t len) {
    uint8_t x = 0;
    uint32_t s = 0;
    for (size_t i = 0; i < len; ++i) {
        x ^= data[i];
        s += data[i];
    }
    return static_cast<uint8_t>((x ^ s) & 0xFFu);
}

inline uint16_t ydt_chksum(const char* body, size_t len) {
    uint32_t s = 0;
    for (size_t i = 0; i < len; ++i) {
        s += static_cast<uint8_t>(body[i]);
    }
    return static_cast<uint16_t>((~s + 1u) & 0xFFFFu);
}

inline uint16_t ydt_length_field(uint16_t lenid_ascii) {
    uint16_t n = lenid_ascii & 0x0FFFu;
    uint16_t sum = ((n >> 8) & 0xFu) + ((n >> 4) & 0xFu) + (n & 0xFu);
    uint16_t lchk = ((~sum) + 1u) & 0xFu;
    return static_cast<uint16_t>((lchk << 12) | n);
}

inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

inline bool hex_to_bytes(const char* hex, size_t hex_len, uint8_t* out, size_t out_cap) {
    if ((hex_len % 2) != 0 || (hex_len / 2) > out_cap) return false;
    for (size_t i = 0; i < hex_len; i += 2) {
        int hi = hex_val(hex[i]);
        int lo = hex_val(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

inline uint16_t be_u16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline int16_t be_s16(const uint8_t* p) {
    return static_cast<int16_t>(be_u16(p));
}

// -------------------- 1. Shoto BMS Modbus V1.2 (PDF) --------------------

inline void build_shoto_modbus_v12_req(uint8_t addr, uint8_t out[8]) {
    out[0] = addr;
    out[1] = 0x04;
    out[2] = 0x0F;
    out[3] = 0xFF;
    out[4] = 0x00;
    out[5] = 0x27;
    uint16_t crc = modbus_crc16(out, 6);
    out[6] = static_cast<uint8_t>(crc & 0xFF);
    out[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

inline bool parse_shoto_modbus_v12_resp(
    const uint8_t* rx, size_t len, uint8_t expected_addr,
    uint32_t now_ms, const PolicyLimits& lim, PackState& out)
{
    const uint8_t* f = nullptr;
    for (size_t start = 0; start + 83u <= len; ++start) {
        if (rx[start] != expected_addr || rx[start + 1] != 0x04 || rx[start + 2] < 78) continue;
        size_t frame_len = static_cast<size_t>(rx[start + 2]) + 5u;
        if (start + frame_len > len) continue;
        uint16_t rx_crc = static_cast<uint16_t>(rx[start + frame_len - 2] | (rx[start + frame_len - 1] << 8));
        if (modbus_crc16(rx + start, frame_len - 2) == rx_crc) {
            f = rx + start;
            break;
        }
    }
    if (!f) return false;

    size_t reg_cnt = static_cast<size_t>(f[2]) / 2u;
    auto reg = [&](size_t idx) -> uint16_t {
        return be_u16(f + 3 + 2 * idx);
    };

    PackState st{};
    st.detected_proto = PROTO_MODBUS_V12;
    st.addr = expected_addr;
    st.core.sampled_ms = now_ms;
    st.core.bus_v = reg(0) * 0.01f;
    // PDF Note 3: charge is negative, discharge is positive -> negate to canonical (+ charge, - discharge)
    st.core.bus_a = (10000.0f - static_cast<float>(reg(1))) * 0.1f;
    st.ext.rem_cap_ah = reg(2) * 0.1f;
    float avg_t = (static_cast<float>(reg(3)) - 400.0f) * 0.1f;
    st.ext.warn_flags = reg(5);
    st.ext.prot_flags = reg(6);
    uint16_t r1006 = reg(7);
    // Verified from PDF p.10 golden capture 0x0800 (Status=0x08 DisgMOSConnect, Fault=0x00)
    st.ext.fault_flags = static_cast<uint8_t>(r1006 & 0xFFu);
    st.ext.status_flags = static_cast<uint8_t>((r1006 >> 8) & 0xFFu);
    st.core.soc = reg(8) * 0.01f;

    bool ba430_shifted = (reg(12) > 16u && reg(13) >= 1u && reg(13) <= 16u);
    if (ba430_shifted) {
        // Verified on live SD-BA430-15S @ addr=214 FC04 0x0FFF..0x1021
        st.ext.cycles = reg(9);
        st.ext.soh = reg(10) * 0.01f;
        st.core.capacity_ah = (st.core.soc > 1.0f && st.ext.rem_cap_ah > 0.0f)
            ? (st.ext.rem_cap_ah * 100.0f / st.core.soc)
            : lim.default_capacity_ah;
        uint16_t cnum = std::min<uint16_t>(reg(13), 16u);
        st.ext.cell_count = static_cast<uint8_t>(cnum);
        uint16_t cmin = 65535u, cmax = 0u;
        uint8_t cmin_idx = 1u, cmax_idx = 1u;
        for (uint16_t i = 0; i < cnum && (14u + i) < reg_cnt; ++i) {
            uint16_t mv = reg(14u + i);
            if (mv != 0xFFFFu && mv > 0u) {
                st.ext.cell_mv[i] = mv;
                if (mv < cmin) { cmin = mv; cmin_idx = static_cast<uint8_t>(i + 1u); }
                if (mv > cmax) { cmax = mv; cmax_idx = static_cast<uint8_t>(i + 1u); }
            }
        }
        st.ext.min_cell_mv = (cmin == 65535u) ? 0 : cmin;
        st.ext.max_cell_mv = cmax;
        st.ext.min_cell_idx = cmin_idx;
        st.ext.max_cell_idx = cmax_idx;

        uint16_t tnum = (reg_cnt > 30u) ? std::min<uint16_t>(reg(30), 4u) : 0u;
        st.ext.temp_count = static_cast<uint8_t>(tnum);
        float tmin = 999.0f, tmax = -999.0f, tsum = 0.0f;
        for (uint16_t k = 0; k < tnum && (31u + k) < reg_cnt; ++k) {
            uint16_t tr = reg(31u + k);
            float tc = (tr < 200u) ? static_cast<float>(tr) : (static_cast<float>(tr) - 400.0f) * 0.1f;
            st.ext.temps_c[k] = tc;
            tmin = std::min(tmin, tc);
            tmax = std::max(tmax, tc);
            tsum += tc;
        }
        st.ext.min_temp_c = (tnum > 0) ? tmin : avg_t;
        st.ext.max_temp_c = (tnum > 0) ? tmax : avg_t;
        st.ext.avg_temp_c = (tnum > 0) ? (tsum / tnum) : avg_t;
        st.core.ccl = lim.ccl_a;
        st.core.dcl = lim.dcl_a;
        st.core.cvl = lim.cvl_v;
        st.core.dvl = lim.dvl_v;
    } else {
        st.ext.soh = reg(9) * 0.01f;
        st.core.capacity_ah = reg(10) * 0.1f;
        st.ext.cycles = reg(11);

        uint16_t cnum = std::min<uint16_t>(reg(12), 16u);
        st.ext.cell_count = static_cast<uint8_t>(cnum);
        uint16_t cmin = 65535u, cmax = 0u;
        uint8_t cmin_idx = 1u, cmax_idx = 1u;
        for (uint16_t i = 0; i < cnum; ++i) {
            uint16_t mv = reg(13 + i);
            if (mv != 0xFFFFu) {
                st.ext.cell_mv[i] = mv;
                if (mv < cmin) { cmin = mv; cmin_idx = static_cast<uint8_t>(i + 1u); }
                if (mv > cmax) { cmax = mv; cmax_idx = static_cast<uint8_t>(i + 1u); }
            }
        }
        st.ext.min_cell_mv = (cmin == 65535u) ? 0 : cmin;
        st.ext.max_cell_mv = cmax;
        st.ext.min_cell_idx = cmin_idx;
        st.ext.max_cell_idx = cmax_idx;

        uint16_t tnum = std::min<uint16_t>(reg(29), 4u);
        st.ext.temp_count = static_cast<uint8_t>(tnum);
        float tmin = 999.0f, tmax = -999.0f, tsum = 0.0f;
        for (uint16_t k = 0; k < tnum; ++k) {
            float tc = (static_cast<float>(reg(30 + k)) - 400.0f) * 0.1f;
            st.ext.temps_c[k] = tc;
            tmin = std::min(tmin, tc);
            tmax = std::max(tmax, tc);
            tsum += tc;
        }
        st.ext.min_temp_c = (tnum > 0) ? tmin : avg_t;
        st.ext.max_temp_c = (tnum > 0) ? tmax : avg_t;
        st.ext.avg_temp_c = (tnum > 0) ? (tsum / tnum) : avg_t;

        float bms_max_chg_cur = static_cast<float>(reg(34));
        float bms_boost_v = reg(38) * 0.01f;
        st.core.ccl = std::min(bms_max_chg_cur, lim.ccl_a);
        st.core.dcl = lim.dcl_a;
        st.core.cvl = (bms_boost_v > lim.dvl_v) ? std::min(bms_boost_v, lim.cvl_v) : lim.cvl_v;
        st.core.dvl = lim.dvl_v;
    }

    st.core.critical = (st.ext.prot_flags != 0) || (st.ext.fault_flags != 0);
    st.core.allow_charge = !st.core.critical && ((st.ext.status_flags & 0x04u) != 0);
    st.core.allow_discharge = !st.core.critical && ((st.ext.status_flags & 0x08u) != 0);
    st.core.identity_verified = true;
    st.core.limits_verified = true;
    st.core.complete = (st.core.bus_v > 0.0f && st.core.capacity_ah > 0.0f);

    out = st;
    return true;
}

// -------------------- 2. TianBangda Binary (TBD, EXE CIL) --------------------

inline void build_tbd_binary_req(uint8_t addr, uint8_t hdr, uint8_t cmd, uint8_t out[6]) {
    out[0] = hdr;
    out[1] = addr;
    out[2] = cmd;
    out[3] = 0x00;
    out[4] = tbd_checksum(out, 4);
    out[5] = 0x0D;
}

inline bool parse_tbd_binary_resp(
    const uint8_t* rx, size_t len, uint8_t expected_addr,
    uint32_t now_ms, const PolicyLimits& lim, PackState& out)
{
    for (size_t start = 0; start + 48u <= len; ++start) {
        if ((rx[start] != 0x7E && rx[start] != 0x7C) || rx[start + 1] != expected_addr) continue;
        const uint8_t* f = rx + start;

        // Format B: Live SD-BA430 / BMS PLUS V1.634 TLV frame (e.g. 140 bytes: 7E <addr> 01 <plen> <tags...> <chk> 0D)
        uint8_t plen = f[3];
        size_t flen_b = static_cast<size_t>(4u + plen + 2u);
        if (f[0] == 0x7E && f[2] == 0x01 && plen >= 40 &&
            start + flen_b <= len && f[4] == 0x01 &&
            f[flen_b - 1] == 0x0D && f[flen_b - 2] == tbd_checksum(f, flen_b - 2)) {
            PackState st{};
            st.detected_proto = PROTO_TBD_BINARY;
            st.addr = expected_addr;
            st.core.sampled_ms = now_ms;
            st.core.capacity_ah = lim.default_capacity_ah;

            size_t pos = 4u;
            size_t end = 4u + plen;
            uint32_t cell_sum_mv = 0u;
            while (pos + 2u <= end) {
                uint8_t tag = f[pos];
                uint8_t cnt = f[pos + 1u];
                pos += 2u;
                size_t elem_sz = (tag >= 0x0Bu && tag <= 0x10u) ? 4u : 2u;
                size_t bytes_needed = static_cast<size_t>(cnt) * elem_sz;
                if (pos + bytes_needed > end) break;

                if (tag == 0x01 && cnt >= 1) {
                    uint8_t nc = std::min<uint8_t>(cnt, 16u);
                    st.ext.cell_count = nc;
                    uint16_t cmin = 65535u, cmax = 0u;
                    uint8_t cmin_idx = 1u, cmax_idx = 1u;
                    for (uint8_t i = 0; i < nc; ++i) {
                        uint16_t mv = static_cast<uint16_t>(be_u16(f + pos + 2u * i) & 0x1FFFu);
                        st.ext.cell_mv[i] = mv;
                        cell_sum_mv += mv;
                        if (mv > 0u && mv < cmin) { cmin = mv; cmin_idx = static_cast<uint8_t>(i + 1u); }
                        if (mv > cmax) { cmax = mv; cmax_idx = static_cast<uint8_t>(i + 1u); }
                    }
                    st.ext.min_cell_mv = (cmin == 65535u) ? 0u : cmin;
                    st.ext.max_cell_mv = cmax;
                    st.ext.min_cell_idx = cmin_idx;
                    st.ext.max_cell_idx = cmax_idx;
                } else if (tag == 0x02 && cnt >= 1) {
                    st.core.bus_a = (30000.0f - static_cast<float>(be_u16(f + pos))) * 0.01f;
                } else if (tag == 0x03 && cnt >= 1) {
                    st.core.soc = static_cast<float>(be_u16(f + pos)) * 0.01f;
                } else if (tag == 0x04 && cnt >= 1) {
                    st.ext.soh = static_cast<float>(be_u16(f + pos)) * 0.01f;
                } else if (tag == 0x05 && cnt >= 1) {
                    uint8_t tn = std::min<uint8_t>(cnt, 5u);
                    st.ext.temp_count = tn;
                    float tmin = 999.0f, tmax = -999.0f, tsum = 0.0f;
                    for (uint8_t k = 0; k < tn; ++k) {
                        float tc = static_cast<float>(f[pos + 2u * k + 1u]) - 50.0f;
                        st.ext.temps_c[k] = tc;
                        tmin = std::min(tmin, tc);
                        tmax = std::max(tmax, tc);
                        tsum += tc;
                    }
                    st.ext.min_temp_c = tmin;
                    st.ext.max_temp_c = tmax;
                    st.ext.avg_temp_c = tsum / tn;
                } else if (tag == 0x06) {
                    // Preserve SD-BA430 word 1 low byte as diagnostic status (e.g. 0x02 = discharging);
                    // SD-BA430 tag 0x06 bit positions differ from Shoto Modbus V1.2 warn/prot registers.
                    if (cnt >= 2) st.ext.status_flags = static_cast<uint8_t>(be_u16(f + pos + 2u) & 0xFFu);
                } else if (tag == 0x07 && cnt >= 1) {
                    st.ext.cycles = be_u16(f + pos);
                } else if (tag == 0x08 && cnt >= 1) {
                    st.core.bus_v = static_cast<float>(be_u16(f + pos)) * 0.01f;
                } else if (tag == 0x09 && cnt >= 1) {
                    uint16_t cap_cah = be_u16(f + pos);
                    if (cap_cah > 0u) st.core.capacity_ah = static_cast<float>(cap_cah) * 0.01f;
                } else if (tag == 0x11 && cnt >= 1 && st.core.bus_v <= 0.0f) {
                    st.core.bus_v = static_cast<float>(be_u16(f + pos)) * 0.01f;
                }
                pos += bytes_needed;
            }
            bool struct_ok = (pos == end) &&
                (st.core.soc >= 0.0f && st.core.soc <= 100.0f) &&
                (st.ext.soh >= 0.0f && st.ext.soh <= 100.0f);
            if (struct_ok && st.ext.cell_count > 0 && st.core.bus_v > 0.0f) {
                float cell_sum_v = static_cast<float>(cell_sum_mv) * 0.001f;
                if (std::fabs(cell_sum_v - st.core.bus_v) >= 3.0f) {
                    struct_ok = false;
                }
            }
            st.ext.rem_cap_ah = st.core.capacity_ah * (st.core.soc * 0.01f);
            st.core.ccl = lim.ccl_a;
            st.core.dcl = lim.dcl_a;
            st.core.cvl = lim.cvl_v;
            st.core.dvl = lim.dvl_v;

            // Derive Modbus-V1.2/Pylontech-compatible warn & protection flags from measured 15S LFP values
            if (st.ext.max_cell_mv >= 3650u) st.ext.warn_flags |= 0x0001u; // Cell OV
            if (st.ext.min_cell_mv > 0u && st.ext.min_cell_mv <= 2800u) st.ext.warn_flags |= 0x0002u; // Cell UV
            if (st.core.bus_v >= 55.2f) st.ext.warn_flags |= 0x0004u; // Pack OV
            if (st.core.bus_v > 0.0f && st.core.bus_v <= 43.0f) st.ext.warn_flags |= 0x0008u; // Pack UV
            if (st.ext.temp_count > 0 && st.ext.max_temp_c >= 55.0f) st.ext.warn_flags |= 0x0100u; // Temp High
            if (st.ext.temp_count > 0 && st.ext.min_temp_c <= 2.0f) st.ext.warn_flags |= 0x0200u; // Temp Low

            if (st.ext.max_cell_mv >= 3750u) st.ext.prot_flags |= 0x0001u; // Cell OV prot
            if (st.core.bus_v >= 56.5f) st.ext.prot_flags |= 0x0002u; // Pack OV prot
            if (st.ext.min_cell_mv > 0u && st.ext.min_cell_mv <= 2550u) st.ext.prot_flags |= 0x0004u; // Cell UV prot
            if (st.core.bus_v > 0.0f && st.core.bus_v <= 40.5f) st.ext.prot_flags |= 0x0008u; // Pack UV prot
            if (st.ext.temp_count > 0 && st.ext.max_temp_c >= 65.0f) st.ext.prot_flags |= 0x0200u; // OT prot
            if (st.ext.temp_count > 0 && st.ext.min_temp_c <= -10.0f) st.ext.prot_flags |= 0x0400u; // UT prot

            st.core.critical = (st.ext.prot_flags != 0);
            st.core.allow_charge = !st.core.critical;
            st.core.allow_discharge = !st.core.critical;
            st.core.identity_verified = (st.ext.cell_count > 0);
            st.core.limits_verified = true;
            st.core.complete = (st.core.bus_v > 0.0f && st.ext.cell_count > 0);
            if (!struct_ok || !st.core.complete) continue;
            out = st;
            return true;
        }
        if (f[0] == 0x7E && f[2] == 0x01 && plen >= 40) {
            continue;
        }

        // Format A: Legacy fixed 86/88-byte frame (TianBangda MS-ShotoV1.1.exe timer_data_Tick)
        size_t flen_a = len - start;
        if (flen_a < 48 || f[flen_a - 1] != 0x0D || f[flen_a - 2] != tbd_checksum(f, flen_a - 2)) {
            continue;
        }
        uint8_t n_cells = f[5];
        if ((n_cells != 15 && n_cells != 16) || flen_a < static_cast<size_t>(56 + 2 * n_cells)) {
            continue;
        }

        size_t c = 2u * n_cells;
        PackState st{};
        st.detected_proto = PROTO_TBD_BINARY;
        st.addr = expected_addr;
        st.core.sampled_ms = now_ms;
        st.ext.cell_count = n_cells;

        uint16_t cmin = 65535u, cmax = 0u;
        uint8_t cmin_idx = 1u, cmax_idx = 1u;
        uint32_t cell_sum_mv = 0u;
        for (uint8_t i = 0; i < n_cells; ++i) {
            uint16_t mv = static_cast<uint16_t>(((f[6 + 2 * i] & 0x1Fu) << 8) | f[7 + 2 * i]);
            st.ext.cell_mv[i] = mv;
            cell_sum_mv += mv;
            if (mv > 0u && mv < cmin) { cmin = mv; cmin_idx = static_cast<uint8_t>(i + 1u); }
            if (mv > cmax) { cmax = mv; cmax_idx = static_cast<uint8_t>(i + 1u); }
        }
        st.ext.min_cell_mv = (cmin == 65535u) ? 0u : cmin;
        st.ext.max_cell_mv = cmax;
        st.ext.min_cell_idx = cmin_idx;
        st.ext.max_cell_idx = cmax_idx;

        uint16_t raw_i = be_u16(f + 8 + c);
        st.core.bus_a = (30000.0f - static_cast<float>(raw_i)) * 0.01f;
        st.core.soc = static_cast<float>(be_u16(f + 12 + c)) * 0.01f;
        st.core.capacity_ah = static_cast<float>(be_u16(f + 16 + c)) * 0.01f;
        st.ext.rem_cap_ah = st.core.capacity_ah * (st.core.soc * 0.01f);

        uint8_t tnum = 5;
        st.ext.temp_count = tnum;
        float tmin = 999.0f, tmax = -999.0f, tsum = 0.0f;
        for (uint8_t k = 0; k < tnum; ++k) {
            float tc = static_cast<float>(f[21 + c + 2 * k]) - 50.0f;
            st.ext.temps_c[k] = tc;
            tmin = std::min(tmin, tc);
            tmax = std::max(tmax, tc);
            tsum += tc;
        }
        st.ext.min_temp_c = tmin;
        st.ext.max_temp_c = tmax;
        st.ext.avg_temp_c = tsum / tnum;

        size_t b = 2u * n_cells + 2u * tnum;
        if (flen_a < 46 + b) continue;
        // Capture protection & status bytes from flogbuff (f[22+b .. 33+b])
        st.ext.prot_flags = static_cast<uint16_t>(f[22 + b] | (f[23 + b] << 8));
        st.ext.warn_flags = static_cast<uint16_t>(f[30 + b] | (f[31 + b] << 8));
        uint8_t fet_status = f[29 + b];
        st.ext.status_flags = fet_status;

        st.ext.cycles = be_u16(f + 34 + b);
        st.core.bus_v = static_cast<float>(be_u16(f + 38 + b)) * 0.01f;
        st.ext.soh = static_cast<float>(be_u16(f + 42 + b)) * 0.01f;

        st.core.ccl = lim.ccl_a;
        st.core.dcl = lim.dcl_a;
        st.core.cvl = lim.cvl_v;
        st.core.dvl = lim.dvl_v;

        // Bit 3 of f[22+b]..f[23+b] are prot flags; check critical conservatively
        st.core.critical = (f[22 + b] & 0xF8u) != 0 || (f[23 + b] & 0x1Fu) != 0;
        st.core.allow_charge = !st.core.critical;
        st.core.allow_discharge = !st.core.critical;
        st.core.identity_verified = true;
        st.core.limits_verified = true;
        st.core.complete = (st.core.bus_v > 0.0f && st.core.capacity_ah > 0.0f);
        if (!st.core.complete ||
            st.core.soc < 0.0f || st.core.soc > 100.0f ||
            st.ext.soh < 0.0f || st.ext.soh > 100.0f ||
            std::fabs((static_cast<float>(cell_sum_mv) * 0.001f) - st.core.bus_v) >= 5.0f) {
            continue;
        }

        out = st;
        return true;
    }
    return false;
}

// -------------------- 2b. TianBangda Read-Only Parameter Decoders (EXE RE) --------------------

struct TbdAlarmParams {
    bool valid = false;
    uint8_t tag_count = 0;
    uint16_t cell_ov_alarm_mv = 0;
    uint16_t cell_ov_alarm_rec_mv = 0;
    uint16_t cell_uv_alarm_mv = 0;
    uint16_t cell_uv_alarm_rec_mv = 0;
    uint16_t pack_ov_alarm_cv = 0;
    uint16_t pack_ov_alarm_rec_cv = 0;
    uint16_t pack_uv_alarm_cv = 0;
    uint16_t pack_uv_alarm_rec_cv = 0;
    uint16_t low_soc_alarm_pct = 0;
    uint16_t low_soc_alarm_rec_pct = 0;
    uint16_t cell_diff_alarm_mv = 0;
    uint16_t cell_diff_alarm_rec_mv = 0;
};

// Verified from TianBangda V1.1.634-26.exe Form1::baohucs: payload uint16 value at rx[6..7] (or rx[4..5] if plen==2).
inline bool parse_tbd_param_u16(
    const uint8_t* rx, size_t len, uint8_t expected_addr, uint8_t expected_cmd, uint16_t& val_out)
{
    for (size_t start = 0; start + 8u <= len; ++start) {
        if ((rx[start] != 0x7E && rx[start] != 0x7C) ||
            rx[start + 1] != expected_addr ||
            rx[start + 2] != expected_cmd) {
            continue;
        }
        const uint8_t* f = rx + start;
        uint8_t plen = f[3];
        if (plen < 2u) continue;
        size_t flen = static_cast<size_t>(4u + plen + 2u);
        if (start + flen > len || f[flen - 1] != 0x0D) continue;
        if (f[flen - 2] != tbd_checksum(f, flen - 2)) continue;
        val_out = (plen >= 4u) ? be_u16(f + 6u) : be_u16(f + 4u);
        return true;
    }
    return false;
}

// Verified from TianBangda V1.1.634-26.exe Form1::gaojcs_read (cmd 0x43): 4-byte records [tag, len/pad, hi, lo].
inline bool parse_tbd_alarm_params_43(
    const uint8_t* rx, size_t len, uint8_t expected_addr, TbdAlarmParams& out)
{
    out = TbdAlarmParams{};
    for (size_t start = 0; start + 10u <= len; ++start) {
        if ((rx[start] != 0x7E && rx[start] != 0x7C) ||
            rx[start + 1] != expected_addr ||
            rx[start + 2] != 0x43) {
            continue;
        }
        const uint8_t* f = rx + start;
        uint8_t plen = f[3];
        if (plen < 4u) continue;
        size_t flen = static_cast<size_t>(4u + plen + 2u);
        if (start + flen > len || f[flen - 1] != 0x0D) continue;
        if (f[flen - 2] != tbd_checksum(f, flen - 2)) continue;

        size_t end = static_cast<size_t>(4u + plen);
        for (size_t pos = 4u; pos + 4u <= end; pos += 4u) {
            uint8_t tag = f[pos];
            uint16_t v = be_u16(f + pos + 2u);
            out.tag_count++;
            switch (tag) {
                case 0x00: out.cell_ov_alarm_mv = v; break;
                case 0x02: out.cell_ov_alarm_rec_mv = v; break;
                case 0x03: out.cell_uv_alarm_mv = v; break;
                case 0x05: out.cell_uv_alarm_rec_mv = v; break;
                case 0x06: out.pack_ov_alarm_cv = v; break;
                case 0x08: out.pack_ov_alarm_rec_cv = v; break;
                case 0x09: out.pack_uv_alarm_cv = v; break;
                case 0x0B: out.pack_uv_alarm_rec_cv = v; break;
                case 0x21: out.low_soc_alarm_pct = v; break;
                case 0x22: out.low_soc_alarm_rec_pct = v; break;
                case 0x23: out.cell_diff_alarm_mv = v; break;
                case 0x24: out.cell_diff_alarm_rec_mv = v; break;
                default: break;
            }
        }
        out.valid = (out.tag_count > 0u);
        return out.valid;
    }
    return false;
}

// -------------------- 3. TianBangda YD/T 1363.3 ASCII (DOC V2.8) --------------------

inline size_t build_ydt1363_frame(
    const char* ver, const char* adr, const char* cid1,
    const char* cid2_or_rtn, const char* info_hex, char* out, size_t out_cap)
{
    size_t info_len = info_hex ? std::strlen(info_hex) : 0u;
    uint16_t len_field = ydt_length_field(static_cast<uint16_t>(info_len));
    char body[512];
    int n = std::snprintf(body, sizeof(body), "%s%s%s%s%04X%s",
                          ver, adr, cid1, cid2_or_rtn, len_field, info_hex ? info_hex : "");
    if (n < 12 || static_cast<size_t>(n) >= sizeof(body)) return 0;
    uint16_t chk = ydt_chksum(body, static_cast<size_t>(n));
    int total = std::snprintf(out, out_cap, "~%s%04X\r", body, chk);
    if (total < 0 || static_cast<size_t>(total) >= out_cap) return 0;
    return static_cast<size_t>(total);
}

inline size_t build_tbd_1363_req(uint8_t pack_group, uint8_t cid2, char* out, size_t out_cap) {
    char cid2_s[4], info_s[4];
    std::snprintf(cid2_s, sizeof(cid2_s), "%02X", cid2);
    if (cid2 == 0x42 || cid2 == 0x44) {
        std::snprintf(info_s, sizeof(info_s), "%02X", pack_group);
    } else {
        info_s[0] = '\0';
    }
    return build_ydt1363_frame("26", "00", "46", cid2_s, info_s, out, out_cap);
}

inline bool validate_ydt1363_frame(
    const char* frame, size_t len,
    char ver_out[3], char adr_out[3], char cid1_out[3], char cid2_out[3],
    const char*& info_ptr, size_t& info_len)
{
    if (len < 18 || frame[0] != '~' || frame[len - 1] != '\r') return false;
    size_t body_len = len - 6; // exclude '~', 4 hex chksum, '\r'
    if (body_len < 12 || (body_len % 2) != 0) return false;
    const char* body = frame + 1;
    const char* chk_str = frame + 1 + body_len;

    for (size_t i = 0; i < body_len + 4; ++i) {
        if (hex_val(body[i]) < 0) return false;
    }
    uint8_t chk_bytes[2];
    if (!hex_to_bytes(chk_str, 4, chk_bytes, 2)) return false;
    uint16_t rx_chk = be_u16(chk_bytes);
    if (ydt_chksum(body, body_len) != rx_chk) return false;

    uint8_t len_bytes[2];
    if (!hex_to_bytes(body + 8, 4, len_bytes, 2)) return false;
    uint16_t len_field = be_u16(len_bytes);
    uint16_t lenid = len_field & 0x0FFFu;
    if (lenid != body_len - 12 || len_field != ydt_length_field(lenid)) return false;

    std::memcpy(ver_out, body + 0, 2); ver_out[2] = '\0';
    std::memcpy(adr_out, body + 2, 2); adr_out[2] = '\0';
    std::memcpy(cid1_out, body + 4, 2); cid1_out[2] = '\0';
    std::memcpy(cid2_out, body + 6, 2); cid2_out[2] = '\0';
    info_ptr = body + 12;
    info_len = lenid;
    return true;
}

inline bool parse_tbd_1363_42_resp(
    const char* frame, size_t len, uint8_t expected_group,
    uint32_t now_ms, const PolicyLimits& lim, PackState& out)
{
    char ver[3], adr[3], cid1[3], rtn[3];
    const char* info = nullptr;
    size_t info_len = 0;
    if (!validate_ydt1363_frame(frame, len, ver, adr, cid1, rtn, info, info_len)) return false;
    if (std::strcmp(rtn, "00") != 0 || info_len < 32) return false;

    uint8_t raw[256];
    if (!hex_to_bytes(info, info_len, raw, sizeof(raw))) return false;
    size_t rlen = info_len / 2;
    uint8_t m = raw[2];
    if (m == 0 || m > 16 || 3 + 2 * m + 1 > rlen) return false;

    PackState st{};
    st.detected_proto = PROTO_TBD_1363;
    st.addr = expected_group;
    st.core.sampled_ms = now_ms;
    st.ext.cell_count = m;

    size_t idx = 3;
    uint16_t cmin = 65535u, cmax = 0u;
    uint8_t cmin_idx = 1u, cmax_idx = 1u;
    for (uint8_t i = 0; i < m; ++i) {
        uint16_t mv = be_u16(raw + idx);
        st.ext.cell_mv[i] = mv;
        if (mv > 0u && mv < cmin) { cmin = mv; cmin_idx = static_cast<uint8_t>(i + 1u); }
        if (mv > cmax) { cmax = mv; cmax_idx = static_cast<uint8_t>(i + 1u); }
        idx += 2;
    }
    st.ext.min_cell_mv = (cmin == 65535u) ? 0u : cmin;
    st.ext.max_cell_mv = cmax;
    st.ext.min_cell_idx = cmin_idx;
    st.ext.max_cell_idx = cmax_idx;

    uint8_t n = raw[idx++];
    if (idx + 2 * n + 11 > rlen) return false;
    st.ext.temp_count = std::min<uint8_t>(n, 5);
    float tmin = 999.0f, tmax = -999.0f, tsum = 0.0f;
    for (uint8_t k = 0; k < n; ++k) {
        float tc = (static_cast<float>(be_u16(raw + idx)) - 2731.0f) * 0.1f;
        if (k < 5) st.ext.temps_c[k] = tc;
        tmin = std::min(tmin, tc);
        tmax = std::max(tmax, tc);
        tsum += tc;
        idx += 2;
    }
    st.ext.min_temp_c = (n > 0) ? tmin : 25.0f;
    st.ext.max_temp_c = (n > 0) ? tmax : 25.0f;
    st.ext.avg_temp_c = (n > 0) ? (tsum / n) : 25.0f;

    st.core.bus_a = static_cast<float>(be_s16(raw + idx)) * 0.01f; idx += 2;
    st.core.bus_v = static_cast<float>(be_u16(raw + idx)) * 0.01f; idx += 2;
    st.ext.rem_cap_ah = static_cast<float>(be_u16(raw + idx)) * 0.01f; idx += 2;
    idx += 1; // self-define count
    st.core.capacity_ah = static_cast<float>(be_u16(raw + idx)) * 0.01f; idx += 2;
    st.ext.cycles = be_u16(raw + idx);
    st.core.soc = (st.core.capacity_ah > 0.0f)
        ? std::min(100.0f, 100.0f * st.ext.rem_cap_ah / st.core.capacity_ah)
        : 0.0f;
    st.ext.soh = 100.0f;

    st.core.ccl = lim.ccl_a;
    st.core.dcl = lim.dcl_a;
    st.core.cvl = lim.cvl_v;
    st.core.dvl = lim.dvl_v;
    st.core.critical = false;
    st.core.allow_charge = true;
    st.core.allow_discharge = true;
    st.core.identity_verified = true;
    st.core.limits_verified = true;
    st.core.complete = (st.core.bus_v > 0.0f && st.core.capacity_ah > 0.0f);

    out = st;
    return true;
}

// -------------------- 4. SmartLi DC/DC Modbus (hpeyerl candidate) --------------------

inline void build_smartli_dcdc_req(uint8_t addr, uint16_t start_reg, uint16_t count, uint8_t out[8]) {
    out[0] = addr;
    out[1] = 0x04;
    out[2] = static_cast<uint8_t>((start_reg >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(start_reg & 0xFF);
    out[4] = static_cast<uint8_t>((count >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(count & 0xFF);
    uint16_t crc = modbus_crc16(out, 6);
    out[6] = static_cast<uint8_t>(crc & 0xFF);
    out[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

inline bool parse_smartli_dcdc_block(
    const uint8_t* blk0_rx, size_t blk0_len,
    const uint8_t* blk1_rx, size_t blk1_len,
    uint8_t expected_addr, uint32_t now_ms, const PolicyLimits& lim, PackState& out)
{
    // blk0: 0x0000..0x0006 (7 regs -> 14B + 5B = 19B)
    // blk1: 0x1030..0x103F (16 regs -> 32B + 5B = 37B)
    if (blk0_len < 19 || blk1_len < 37) return false;
    if (blk0_rx[0] != expected_addr || blk0_rx[1] != 0x04 || blk0_rx[2] < 14) return false;
    if (blk1_rx[0] != expected_addr || blk1_rx[1] != 0x04 || blk1_rx[2] < 32) return false;
    size_t l0 = blk0_rx[2] + 5u, l1 = blk1_rx[2] + 5u;
    if (blk0_len < l0 || blk1_len < l1) return false;
    if (modbus_crc16(blk0_rx, l0 - 2) != static_cast<uint16_t>(blk0_rx[l0 - 2] | (blk0_rx[l0 - 1] << 8))) return false;
    if (modbus_crc16(blk1_rx, l1 - 2) != static_cast<uint16_t>(blk1_rx[l1 - 2] | (blk1_rx[l1 - 1] << 8))) return false;

    auto r0 = [&](size_t idx) { return be_u16(blk0_rx + 3 + 2 * idx); };
    auto r1 = [&](size_t idx) { return be_u16(blk1_rx + 3 + 2 * idx); };

    PackState st{};
    st.detected_proto = PROTO_SMARTLI_DCDC;
    st.addr = expected_addr;
    st.core.sampled_ms = now_ms;
    st.core.bus_v = r0(0) * 0.01f;
    float tmax = static_cast<float>(static_cast<int16_t>(r0(5))) * 0.01f;
    float tmin = static_cast<float>(static_cast<int16_t>(r0(6))) * 0.01f;
    st.ext.max_temp_c = tmax;
    st.ext.min_temp_c = tmin;
    st.ext.avg_temp_c = 0.5f * (tmax + tmin);

    st.core.bus_a = static_cast<float>(static_cast<int16_t>(r1(0))) * 0.01f;
    uint16_t cap_raw = r1(2);
    st.core.capacity_ah = (cap_raw > 0) ? static_cast<float>(cap_raw) : lim.default_capacity_ah;
    st.core.soc = static_cast<float>(r1(4));
    st.ext.soh = static_cast<float>(r1(5));
    st.ext.rem_cap_ah = st.core.capacity_ah * (st.core.soc * 0.01f);
    st.ext.prot_flags = r1(12);
    st.ext.status_flags = static_cast<uint8_t>(r1(13) & 0xFFu);
    st.ext.online_count = static_cast<uint8_t>(r1(15) & 0xFFu);

    st.core.ccl = lim.ccl_a;
    st.core.dcl = lim.dcl_a;
    st.core.cvl = lim.cvl_v;
    st.core.dvl = lim.dvl_v;
    st.core.critical = (st.ext.prot_flags != 0);
    st.core.allow_charge = !st.core.critical;
    st.core.allow_discharge = !st.core.critical;
    st.core.identity_verified = true;
    st.core.limits_verified = true;
    st.core.complete = (st.core.bus_v > 0.0f && st.core.capacity_ah > 0.0f);

    out = st;
    return true;
}

// -------------------- Bank Aggregation + Safety Gate --------------------

enum InhibitReason : uint8_t {
    INHIBIT_NONE = 0,
    INHIBIT_NO_PACKS = 1,
    INHIBIT_POLICY = 2,
    INHIBIT_STALE = 3,
    INHIBIT_CRITICAL = 4,
    INHIBIT_INCOMPLETE = 5,
    INHIBIT_LIMITS = 6,
    INHIBIT_DELTA_V = 7
};

inline const char* inhibit_reason_name(InhibitReason r) {
    switch (r) {
        case INHIBIT_NONE:       return "NONE";
        case INHIBIT_NO_PACKS:   return "NO_PACKS";
        case INHIBIT_POLICY:     return "POLICY";
        case INHIBIT_STALE:      return "STALE";
        case INHIBIT_CRITICAL:   return "CRITICAL";
        case INHIBIT_INCOMPLETE: return "INCOMPLETE";
        case INHIBIT_LIMITS:     return "LIMITS";
        case INHIBIT_DELTA_V:    return "DELTA_V";
        default:                 return "UNKNOWN";
    }
}

inline InhibitReason classify_bank_inhibit(
    const PackState* packs, size_t count, uint32_t now_ms, const Policy& pol,
    float* delta_v_out = nullptr, uint32_t* max_age_out = nullptr)
{
    if (delta_v_out) *delta_v_out = 0.0f;
    if (max_age_out) *max_age_out = 0u;
    size_t n = std::min<size_t>(count, 4u);
    if (n == 0u) return INHIBIT_NO_PACKS;

    float min_v = 999.0f, max_v = -999.0f;
    float cvl_lim = 1e6f, dvl_lim = 0.0f;
    uint32_t max_age = 0u;
    bool any_critical = false, any_stale = false, any_unusable = false;

    for (size_t i = 0; i < n; ++i) {
        const PackState& p = packs[i];
        uint32_t age = uint32_t(now_ms - p.core.sampled_ms);
        if (age > max_age) max_age = age;
        if (p.core.critical) any_critical = true;
        if (age > pol.freshness_ms) any_stale = true;
        if (!usable(p.core, now_ms, pol)) any_unusable = true;
        min_v = std::min(min_v, p.core.bus_v);
        max_v = std::max(max_v, p.core.bus_v);
        cvl_lim = std::min(cvl_lim, p.core.cvl);
        dvl_lim = std::max(dvl_lim, p.core.dvl);
    }

    if (delta_v_out) *delta_v_out = (n >= 2u && finite(max_v) && finite(min_v)) ? (max_v - min_v) : 0.0f;
    if (max_age_out) *max_age_out = max_age;

    if (!pol.approved ||
        !finite(pol.charge_cap) || pol.charge_cap < 0.0f ||
        !finite(pol.discharge_cap) || pol.discharge_cap < 0.0f ||
        (n >= 2u && (!finite(pol.max_delta_v) || pol.max_delta_v <= 0.0f))) {
        return INHIBIT_POLICY;
    }
    if (any_critical) return INHIBIT_CRITICAL;
    if (any_stale) return INHIBIT_STALE;
    if (any_unusable) return INHIBIT_INCOMPLETE;
    if (!(cvl_lim > dvl_lim)) return INHIBIT_LIMITS;
    if (n >= 2u && (max_v - min_v) > pol.max_delta_v) return INHIBIT_DELTA_V;
    return INHIBIT_NONE;
}

inline bool aggregate_bank(
    const PackState* packs, size_t count, uint32_t now_ms, const Policy& pol, BankExt& out)
{
    BankExt b{};
    if (count == 0) {
        out = b;
        return false;
    }

    if (count == 1) {
        const PackState& p = packs[0];
        b.has_telemetry = p.core.complete;
        b.pack_count = 1;
        b.bus_v = p.core.bus_v;
        b.raw_soc = p.core.soc;
        b.raw_bus_a = p.core.bus_a;
        b.rem_cap_ah = p.ext.rem_cap_ah;
        b.soh = p.ext.soh;
        b.min_temp_c = p.ext.min_temp_c;
        b.max_temp_c = p.ext.max_temp_c;
        b.avg_temp_c = p.ext.avg_temp_c;
        b.min_cell_mv = p.ext.min_cell_mv;
        b.max_cell_mv = p.ext.max_cell_mv;
        b.min_cell_pack = (p.addr > 0u) ? p.addr : 1u;
        b.min_cell_idx = (p.ext.min_cell_idx > 0u) ? p.ext.min_cell_idx : 1u;
        b.max_cell_pack = b.min_cell_pack;
        b.max_cell_idx = (p.ext.max_cell_idx > 0u) ? p.ext.max_cell_idx : 1u;
        b.cycles = p.ext.cycles;
        b.warn_flags = p.ext.warn_flags;
        b.prot_flags = p.ext.prot_flags;
        b.fault_flags = p.ext.fault_flags;
        b.cell_count = p.ext.cell_count;
        std::memcpy(b.cell_mv, p.ext.cell_mv, sizeof(b.cell_mv));

        if (pol.approved && usable(p.core, now_ms, pol) && pol.charge_cap >= 0.0f && pol.discharge_cap >= 0.0f) {
            b.core.cvl = p.core.cvl;
            b.core.dvl = p.core.dvl;
            if (b.core.cvl > b.core.dvl) {
                b.core.capacity_ah = p.core.capacity_ah;
                b.core.soc = p.core.soc;
                b.core.bus_a = p.core.bus_a;
                b.core.ccl = p.core.allow_charge ? std::min(p.core.ccl, pol.charge_cap) : 0.0f;
                b.core.dcl = p.core.allow_discharge ? std::min(p.core.dcl, pol.discharge_cap) : 0.0f;
                b.core.charge = b.core.ccl > 0.0f;
                b.core.discharge = b.core.dcl > 0.0f;
                b.core.valid = true;
            }
        }
        out = b;
        return b.core.valid;
    }

    if (count == 2) {
        // 2 packs: delegate core safety and weighted SOC/current to audited safety_core.hpp
        b.has_telemetry = packs[0].core.complete || packs[1].core.complete;
        b.pack_count = 2;
        b.core = aggregate(packs[0].core, packs[1].core, now_ms, pol);
        b.bus_v = 0.5f * (packs[0].core.bus_v + packs[1].core.bus_v);
        float cap_sum = packs[0].core.capacity_ah + packs[1].core.capacity_ah;
        b.raw_soc = (cap_sum > 0.0f)
            ? ((packs[0].core.soc * packs[0].core.capacity_ah + packs[1].core.soc * packs[1].core.capacity_ah) / cap_sum)
            : 0.5f * (packs[0].core.soc + packs[1].core.soc);
        b.raw_bus_a = packs[0].core.bus_a + packs[1].core.bus_a;
        b.rem_cap_ah = packs[0].ext.rem_cap_ah + packs[1].ext.rem_cap_ah;
        b.soh = std::min(packs[0].ext.soh, packs[1].ext.soh);
        b.min_temp_c = std::min(packs[0].ext.min_temp_c, packs[1].ext.min_temp_c);
        b.max_temp_c = std::max(packs[0].ext.max_temp_c, packs[1].ext.max_temp_c);
        b.avg_temp_c = 0.5f * (packs[0].ext.avg_temp_c + packs[1].ext.avg_temp_c);
        uint16_t min_c0 = packs[0].ext.min_cell_mv, min_c1 = packs[1].ext.min_cell_mv;
        uint8_t p0_id = (packs[0].addr > 0u) ? packs[0].addr : 1u;
        uint8_t p1_id = (packs[1].addr > 0u) ? packs[1].addr : 2u;
        uint8_t i0_min = (packs[0].ext.min_cell_idx > 0u) ? packs[0].ext.min_cell_idx : 1u;
        uint8_t i1_min = (packs[1].ext.min_cell_idx > 0u) ? packs[1].ext.min_cell_idx : 1u;
        uint8_t i0_max = (packs[0].ext.max_cell_idx > 0u) ? packs[0].ext.max_cell_idx : 1u;
        uint8_t i1_max = (packs[1].ext.max_cell_idx > 0u) ? packs[1].ext.max_cell_idx : 1u;
        if (min_c1 == 0u || (min_c0 > 0u && min_c0 <= min_c1)) {
            b.min_cell_mv = min_c0;
            b.min_cell_pack = p0_id;
            b.min_cell_idx = i0_min;
        } else {
            b.min_cell_mv = min_c1;
            b.min_cell_pack = p1_id;
            b.min_cell_idx = i1_min;
        }
        if (packs[0].ext.max_cell_mv >= packs[1].ext.max_cell_mv) {
            b.max_cell_mv = packs[0].ext.max_cell_mv;
            b.max_cell_pack = p0_id;
            b.max_cell_idx = i0_max;
        } else {
            b.max_cell_mv = packs[1].ext.max_cell_mv;
            b.max_cell_pack = p1_id;
            b.max_cell_idx = i1_max;
        }
        b.cycles = std::max(packs[0].ext.cycles, packs[1].ext.cycles);
        b.warn_flags = packs[0].ext.warn_flags | packs[1].ext.warn_flags;
        b.prot_flags = packs[0].ext.prot_flags | packs[1].ext.prot_flags;
        b.fault_flags = packs[0].ext.fault_flags | packs[1].ext.fault_flags;
        b.cell_count = packs[0].ext.cell_count;
        std::memcpy(b.cell_mv, packs[0].ext.cell_mv, sizeof(b.cell_mv));
        out = b;
        return b.core.valid;
    }

    // 3..4 packs (e.g. IHF6200 3-battery bank): same fail-closed safety gate across all packs
    size_t n = std::min<size_t>(count, 4u);
    b.pack_count = static_cast<uint8_t>(n);
    float sum_v = 0.0f, cap_sum = 0.0f, weighted_soc_sum = 0.0f, sum_i = 0.0f, sum_rem = 0.0f;
    float min_soh = 100.0f, min_t = 999.0f, max_t = -999.0f, sum_avg_t = 0.0f;
    uint16_t min_c = 65535u, max_c = 0u, max_cyc = 0u;
    uint8_t min_cp = 1u, min_ci = 1u, max_cp = 1u, max_ci = 1u;
    float min_v = 999.0f, max_v = -999.0f;
    float min_ccl = 1e6f, min_dcl = 1e6f;
    float cvl_lim = 1e6f, dvl_lim = 0.0f;
    bool all_usable = true, all_chg = true, all_dis = true;

    for (size_t i = 0; i < n; ++i) {
        const PackState& p = packs[i];
        if (p.core.complete) b.has_telemetry = true;
        sum_v += p.core.bus_v;
        cap_sum += p.core.capacity_ah;
        weighted_soc_sum += p.core.soc * p.core.capacity_ah;
        sum_i += p.core.bus_a;
        sum_rem += p.ext.rem_cap_ah;
        min_soh = std::min(min_soh, p.ext.soh);
        min_t = std::min(min_t, p.ext.min_temp_c);
        max_t = std::max(max_t, p.ext.max_temp_c);
        sum_avg_t += p.ext.avg_temp_c;
        uint8_t pid = (p.addr > 0u) ? p.addr : static_cast<uint8_t>(i + 1u);
        uint8_t pmin_i = (p.ext.min_cell_idx > 0u) ? p.ext.min_cell_idx : 1u;
        uint8_t pmax_i = (p.ext.max_cell_idx > 0u) ? p.ext.max_cell_idx : 1u;
        if (p.ext.min_cell_mv > 0u && p.ext.min_cell_mv < min_c) {
            min_c = p.ext.min_cell_mv;
            min_cp = pid;
            min_ci = pmin_i;
        }
        if (p.ext.max_cell_mv > max_c) {
            max_c = p.ext.max_cell_mv;
            max_cp = pid;
            max_ci = pmax_i;
        }
        max_cyc = std::max(max_cyc, p.ext.cycles);
        b.warn_flags |= p.ext.warn_flags;
        b.prot_flags |= p.ext.prot_flags;
        b.fault_flags |= p.ext.fault_flags;

        min_v = std::min(min_v, p.core.bus_v);
        max_v = std::max(max_v, p.core.bus_v);
        if (!usable(p.core, now_ms, pol)) all_usable = false;
        if (!p.core.allow_charge) all_chg = false;
        if (!p.core.allow_discharge) all_dis = false;
        min_ccl = std::min(min_ccl, p.core.ccl);
        min_dcl = std::min(min_dcl, p.core.dcl);
        cvl_lim = std::min(cvl_lim, p.core.cvl);
        dvl_lim = std::max(dvl_lim, p.core.dvl);
    }

    b.bus_v = sum_v / static_cast<float>(n);
    b.raw_soc = (cap_sum > 0.0f) ? (weighted_soc_sum / cap_sum) : 0.0f;
    b.raw_bus_a = sum_i;
    b.rem_cap_ah = sum_rem;
    b.soh = min_soh;
    b.min_temp_c = min_t;
    b.max_temp_c = max_t;
    b.avg_temp_c = sum_avg_t / static_cast<float>(n);
    b.min_cell_mv = (min_c == 65535u) ? 0u : min_c;
    b.max_cell_mv = max_c;
    b.min_cell_pack = min_cp;
    b.min_cell_idx = min_ci;
    b.max_cell_pack = max_cp;
    b.max_cell_idx = max_ci;
    b.cycles = max_cyc;
    b.cell_count = packs[0].ext.cell_count;
    std::memcpy(b.cell_mv, packs[0].ext.cell_mv, sizeof(b.cell_mv));

    if (pol.approved && finite(pol.max_delta_v) && pol.max_delta_v > 0.0f &&
        finite(pol.charge_cap) && pol.charge_cap >= 0.0f &&
        finite(pol.discharge_cap) && pol.discharge_cap >= 0.0f &&
        all_usable && (max_v - min_v) <= pol.max_delta_v && cvl_lim > dvl_lim) {
        b.core.cvl = cvl_lim;
        b.core.dvl = dvl_lim;
        b.core.capacity_ah = cap_sum;
        b.core.soc = b.raw_soc;
        b.core.bus_a = sum_i;
        b.core.ccl = all_chg ? std::min(min_ccl, pol.charge_cap) : 0.0f;
        b.core.dcl = all_dis ? std::min(min_dcl, pol.discharge_cap) : 0.0f;
        b.core.charge = b.core.ccl > 0.0f;
        b.core.discharge = b.core.dcl > 0.0f;
        b.core.valid = true;
    }
    out = b;
    return b.core.valid;
}

// -------------------- Pylontech LV RS485 Inverter Emulator --------------------

inline uint16_t temp_c_to_deci_k(float temp_c) {
    float dk = (temp_c + 273.1f) * 10.0f;
    if (dk < 0.0f) dk = 0.0f;
    if (dk > 65535.0f) dk = 65535.0f;
    return static_cast<uint16_t>(std::lround(dk));
}

template <typename T>
inline T clamp_val(T v, T lo, T hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

inline size_t handle_pylontech_request(
    const char* frame, size_t len, const BankExt& bank, char* out, size_t out_cap)
{
    char ver[3], adr[3], cid1[3], cid2[3];
    const char* req_info = nullptr;
    size_t req_info_len = 0;
    if (!validate_ydt1363_frame(frame, len, ver, adr, cid1, cid2, req_info, req_info_len)) {
        return 0;
    }
    if (!bank.has_telemetry) {
        // BOOT / DISCOVERY: silent fail-closed until first real battery telemetry
        return 0;
    }

    const char* resp_ver = (std::strcmp(ver, "00") == 0) ? "20" : ver;
    const char* resp_adr = (std::strcmp(adr, "00") == 0) ? "02" : adr;
    char info[320] = {0};

    float soc_f = bank.core.valid ? bank.core.soc : 0.0f;
    uint8_t soc_u8 = static_cast<uint8_t>(clamp_val<int>(static_cast<int>(std::lround(soc_f)), 0, 100));
    uint8_t soh_u8 = static_cast<uint8_t>(clamp_val<int>(static_cast<int>(std::lround(bank.soh)), 0, 100));
    uint16_t volt_mv = static_cast<uint16_t>(clamp_val<long>(std::lround(bank.bus_v * 1000.0f), 0L, 65535L));
    uint16_t volt_cv = static_cast<uint16_t>(clamp_val<long>(std::lround(bank.bus_v * 100.0f), 0L, 65535L));
    int16_t cur_ca = static_cast<int16_t>(clamp_val<long>(std::lround((bank.core.valid ? bank.core.bus_a : 0.0f) * 100.0f), -32768L, 32767L));
    uint16_t avg_tk = temp_c_to_deci_k(bank.avg_temp_c);
    uint16_t min_tk = temp_c_to_deci_k(bank.min_temp_c);
    uint16_t max_tk = temp_c_to_deci_k(bank.max_temp_c);
    uint16_t min_cell = (bank.min_cell_mv > 0) ? bank.min_cell_mv : 3200u;
    uint16_t max_cell = (bank.max_cell_mv > 0) ? bank.max_cell_mv : 3200u;

    uint16_t cvl_mv = static_cast<uint16_t>(clamp_val<long>(std::lround(bank.core.cvl * 1000.0f), 0L, 65535L));
    uint16_t dvl_mv = static_cast<uint16_t>(clamp_val<long>(std::lround(bank.core.dvl * 1000.0f), 0L, 65535L));
    uint16_t ccl_da = bank.core.valid ? static_cast<uint16_t>(clamp_val<long>(std::lround(bank.core.ccl * 10.0f), 0L, 65535L)) : 0u;
    uint16_t dcl_da = bank.core.valid ? static_cast<uint16_t>(clamp_val<long>(std::lround(bank.core.dcl * 10.0f), 0L, 65535L)) : 0u;
    uint8_t chg_dis_status = 0;
    if (bank.core.valid) {
        if (bank.core.charge && ccl_da > 0) chg_dis_status |= (1u << 7);
        if (bank.core.discharge && dcl_da > 0) chg_dis_status |= (1u << 6);
    }

    if (std::strcmp(cid2, "60") == 0) {
        // Pylontech RS485 V3.5 Section 2.1 (p.12) System Basic Info:
        // 10B BatteryName ("Force_L\0\0\0") + 20B Manufacturer ("Pylon\0...") + 2B Ver (0009) + 1B Count + N*16B Barcodes
        uint8_t n_packs = clamp_val<uint8_t>(bank.pack_count > 0 ? bank.pack_count : 1u, 1u, 4u);
        int pos = std::snprintf(
            info, sizeof(info),
            "466F7263655F4C000000"
            "50796C6F6E000000000000000000000000000000"
            "0009%02X",
            n_packs);
        for (uint8_t i = 0; i < n_packs && pos > 0 && static_cast<size_t>(pos) + 32u < sizeof(info); ++i) {
            pos += std::snprintf(
                info + pos, sizeof(info) - static_cast<size_t>(pos),
                "423438313030423231305445373030%02X",
                static_cast<unsigned>(0x31u + i));
        }
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    if (std::strcmp(cid2, "61") == 0) {
        // 49 bytes (98 ASCII hex chars) matching Fahmula/Pylontech V3.5 system analog
        // Samoto/Siseli decodes cell position uint16 as decimal (pack_id * 100 + cell_num)
        uint16_t max_cell_pos = static_cast<uint16_t>(
            clamp_val<uint16_t>(bank.max_cell_pack > 0u ? bank.max_cell_pack : 1u, 1u, 99u) * 100u +
            clamp_val<uint16_t>(bank.max_cell_idx > 0u ? bank.max_cell_idx : 1u, 1u, 99u));
        uint16_t min_cell_pos = static_cast<uint16_t>(
            clamp_val<uint16_t>(bank.min_cell_pack > 0u ? bank.min_cell_pack : 1u, 1u, 99u) * 100u +
            clamp_val<uint16_t>(bank.min_cell_idx > 0u ? bank.min_cell_idx : 1u, 1u, 99u));
        uint16_t temp_pos = 101u;
        std::snprintf(
            info, sizeof(info),
            "%04X%04X%02X%04X%04X%02X%02X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X%04X",
            volt_mv, static_cast<uint16_t>(cur_ca), soc_u8,
            bank.cycles, bank.cycles, soh_u8, soh_u8,
            max_cell, max_cell_pos, min_cell, min_cell_pos,
            avg_tk, max_tk, temp_pos, min_tk, temp_pos,
            avg_tk, max_tk, temp_pos, min_tk, temp_pos,
            avg_tk, max_tk, temp_pos, min_tk, temp_pos);
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    if (std::strcmp(cid2, "62") == 0) {
        uint8_t a1 = 0, a2 = 0, p1 = 0, p2 = 0;
        if (bank.warn_flags & 0x0004u) a1 |= (1u << 7); // Pack OV
        if (bank.warn_flags & 0x0008u) a1 |= (1u << 6); // Pack UV
        if (bank.warn_flags & 0x0001u) a1 |= (1u << 5); // Cell OV
        if (bank.warn_flags & 0x0002u) a1 |= (1u << 4); // Cell UV
        if (bank.warn_flags & 0x0500u) a1 |= (1u << 3); // Temp High
        if (bank.warn_flags & 0x0A00u) a1 |= (1u << 2); // Temp Low
        if (bank.warn_flags & 0x0010u) a2 |= (1u << 6); // Chg OC
        if (bank.warn_flags & 0x0020u) a2 |= (1u << 5); // Disg OC
        if (bank.prot_flags & 0x0002u) p1 |= (1u << 7); // Mod OV
        if (bank.prot_flags & 0x0008u) p1 |= (1u << 6); // Mod UV
        if (bank.prot_flags & 0x0001u) p1 |= (1u << 5); // Cell OV
        if (bank.prot_flags & 0x0004u) p1 |= (1u << 4); // Cell UV
        if (bank.prot_flags & 0x0A00u) p1 |= (1u << 3); // OT
        if (bank.prot_flags & 0x1400u) p1 |= (1u << 2); // UT
        if (bank.prot_flags & 0x0030u) p2 |= (1u << 6); // Chg OC
        if (bank.prot_flags & 0x00C0u) p2 |= (1u << 5); // Disg OC
        if (!bank.core.valid || bank.fault_flags != 0) p2 |= (1u << 3); // System fault / inhibit
        std::snprintf(info, sizeof(info), "%02X%02X%02X%02X", a1, a2, p1, p2);
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    if (std::strcmp(cid2, "63") == 0) {
        std::snprintf(info, sizeof(info), "%04X%04X%04X%04X%02X",
                      cvl_mv, dvl_mv, ccl_da, dcl_da, chg_dis_status);
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    if (std::strcmp(cid2, "47") == 0) {
        // Pylontech V3.3 per-pack management info (10 bytes: ADR + same 9 limit/status bytes)
        std::snprintf(info, sizeof(info), "%s%04X%04X%04X%04X%02X",
                      resp_adr, cvl_mv, dvl_mv, ccl_da, dcl_da, chg_dis_status);
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    if (std::strcmp(cid2, "42") == 0) {
        uint8_t m = (bank.cell_count > 0 && bank.cell_count <= 16) ? bank.cell_count : 15;
        char* p = info;
        size_t rem = sizeof(info);
        int w = std::snprintf(p, rem, "00%02X%02X", 1, m);
        p += w; rem -= static_cast<size_t>(w);
        for (uint8_t i = 0; i < m; ++i) {
            uint16_t mv = (bank.cell_mv[i] > 0) ? bank.cell_mv[i] : static_cast<uint16_t>(volt_mv / m);
            w = std::snprintf(p, rem, "%04X", mv);
            p += w; rem -= static_cast<size_t>(w);
        }
        uint16_t rem_cah = static_cast<uint16_t>(clamp_val<long>(std::lround(bank.rem_cap_ah * 100.0f), 0L, 65535L));
        uint16_t full_cah = static_cast<uint16_t>(clamp_val<long>(std::lround(bank.core.capacity_ah * 100.0f), 0L, 65535L));
        std::snprintf(p, rem, "04%04X%04X%04X%04X%04X%04X%04X02%04X%04X",
                      avg_tk, avg_tk, min_tk, max_tk,
                      static_cast<uint16_t>(cur_ca), volt_cv, rem_cah, full_cah, bank.cycles);
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    if (std::strcmp(cid2, "44") == 0) {
        uint8_t m = (bank.cell_count > 0 && bank.cell_count <= 16) ? bank.cell_count : 15;
        char* p = info;
        size_t rem = sizeof(info);
        int w = std::snprintf(p, rem, "00%02X%02X", 1, m);
        p += w; rem -= static_cast<size_t>(w);
        for (uint8_t i = 0; i < m; ++i) {
            w = std::snprintf(p, rem, "00");
            p += w; rem -= static_cast<size_t>(w);
        }
        uint8_t fet = 0;
        if (bank.core.valid) {
            if (bank.core.discharge) fet |= 0x01u;
            if (bank.core.charge) fet |= 0x02u;
        }
        uint8_t sys = bank.core.valid ? 0x10u : 0x20u; // Standby vs TurnOff/Fault
        std::snprintf(p, rem, "0400000000000007000000000000%02X0000%02X", fet, sys);
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    if (std::strcmp(cid2, "4F") == 0) {
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", "", out, out_cap);
    }

    if (std::strcmp(cid2, "51") == 0) {
        // 10B DeviceName ("SHOTO_BRDG") + 2B Ver (01 00) + 20B Manufacturer ("PYLON               ")
        const char* ascii_hex =
            "53484F544F5F42524447"
            "0100"
            "50594C4F4E202020202020202020202020202020";
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", ascii_hex, out, out_cap);
    }

    if (std::strcmp(cid2, "92") == 0) {
        std::snprintf(info, sizeof(info), "%02X", bank.pack_count > 0 ? bank.pack_count : 1);
        return build_ydt1363_frame(resp_ver, resp_adr, cid1, "00", info, out, out_cap);
    }

    // Unknown CID2 -> protocol-compliant invalid CID2 return code (0x04)
    return build_ydt1363_frame(resp_ver, resp_adr, cid1, "04", "", out, out_cap);
}
