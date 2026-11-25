/**
 * SHOTO SMART-LI (SDA10-48100/48150/4850) <-> SAMOTO (IHF6200/ILS6300) BRIDGE
 * ============================================================================
 * Firmware siap flash (Arduino IDE 2.x / PlatformIO) berbasis FreeRTOS dual-UART.
 *
 * CARA PAKAI CEPAT:
 *  1. Edit config.h sesuai hasil scan/autodetect (default aman: PROTO_AUTO + MODE_MONITOR_ONLY).
 *  2. Pilih Board: "ESP32 Dev Module" (untuk Opsi A) atau "ESP32S3 Dev Module" (Opsi B).
 *  3. Upload -> Buka Serial Monitor pada baud 115200.
 *  4. Ketik "help" di Serial Monitor untuk perintah diagnosa live (status, cells, scan, raw).
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "config.h"
#include "safety_core.hpp"
#include "bridge_protocols.hpp"

HardwareSerial BatSerial(BAT_UART_NUM);
HardwareSerial InvSerial(INV_UART_NUM);

static constexpr uint8_t MAX_PACKS = MAX_SUPPORTED_PACKS;
static_assert(
    POLICY_FRESHNESS_MS >= (PACK_OFFLINE_MISSES > 1 ? (PACK_OFFLINE_MISSES - 1) : 1) * (POLL_INTERVAL_MS + 2500UL),
    "POLICY_FRESHNESS_MS terlalu mepet untuk hot-unplug bebas glitch");
static SemaphoreHandle_t g_snap_mutex = nullptr;
static PackState g_packs[MAX_PACKS];
static bool g_pack_online[MAX_PACKS] = {false, false, false, false};
static uint8_t g_pack_miss[MAX_PACKS] = {0, 0, 0, 0};
static BankExt g_bank;
static ShotoProtoKind g_active_proto = static_cast<ShotoProtoKind>(SHOTO_PROTOCOL);
static uint8_t g_pack_addrs[MAX_PACKS] = {PACK1_ADDR, PACK2_ADDR, PACK3_ADDR, PACK4_ADDR};
static bool g_dynamic_discovery = DYNAMIC_PACK_DISCOVERY;
static volatile bool g_force_rescan_packs = true;
static volatile bool g_dump_params = false;
static bool g_raw_log = false;
static bool g_bat_raw_log = false;
static bool g_auto_cell_log = AUTO_LOG_CELLS;
static bool g_tbd_locked_no_wake = false;
static uint32_t g_bat_poll_ok = 0;
static uint32_t g_bat_poll_fail = 0;
static uint32_t g_inv_req_count = 0;
static uint32_t g_inv_resp_count = 0;
static uint32_t g_inv_raw_bytes = 0;
static uint32_t g_last_inv_rx_ms = 0;

static uint32_t g_inhibit_count = 0;
static uint32_t g_delta_v_retry_ok = 0;
static uint32_t g_delta_v_retry_fail = 0;
static InhibitReason g_last_inhibit_reason = INHIBIT_NONE;
static uint32_t g_last_inhibit_ms = 0;
static float g_last_inhibit_dv = 0.0f;
static uint32_t g_last_inhibit_age_ms = 0;
static float g_peak_delta_v = 0.0f;
static uint32_t g_peak_age_ms = 0;

static uint32_t g_inv_60_cnt = 0;
static uint32_t g_inv_61_cnt = 0;
static uint32_t g_inv_62_cnt = 0;
static uint32_t g_inv_63_cnt = 0;
static uint32_t g_inv_other_cnt = 0;
static uint32_t g_inv_mutex_miss = 0;
static char g_inv_last_tx_61[144] = "";
static char g_inv_last_tx_62[40] = "";
static char g_inv_last_tx_63[48] = "";
static uint32_t g_inv_last_61_ms = 0;
static uint32_t g_inv_last_62_ms = 0;
static uint32_t g_inv_last_63_ms = 0;

static void copy_tx_stripped(char* dst, size_t dst_cap, const char* src, size_t src_len) {
    if (dst_cap == 0u) return;
    while (src_len > 0u && (src[src_len - 1u] == '\r' || src[src_len - 1u] == '\n')) {
        src_len--;
    }
    size_t n = (src_len < dst_cap - 1u) ? src_len : (dst_cap - 1u);
    if (n > 0u) std::memcpy(dst, src, n);
    dst[n] = '\0';
}

static void bat_set_tx(bool tx_en) {
#if BAT_DE_PIN >= 0
    digitalWrite(BAT_DE_PIN, tx_en ? HIGH : LOW);
    if (tx_en) delayMicroseconds(100);
#else
    (void)tx_en;
#endif
}

static size_t uart_transact(
    HardwareSerial& port, const uint8_t* tx, size_t tx_len,
    uint8_t* rx, size_t rx_cap, uint32_t wait_ms, bool is_bat)
{
    while (port.available()) port.read();
    if (is_bat) bat_set_tx(true);
    port.write(tx, tx_len);
    port.flush();
    if (is_bat) {
        delayMicroseconds(200);
        bat_set_tx(false);
    }

    uint32_t start = millis();
    uint32_t last_byte = start;
    size_t n = 0;
    while ((millis() - start) < wait_ms && n < rx_cap) {
        if (port.available()) {
            int b = port.read();
            if (b >= 0) {
                rx[n++] = static_cast<uint8_t>(b);
                last_byte = millis();
            }
        } else {
            if (n >= 6 && (millis() - last_byte) > 65) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    if (is_bat && g_bat_raw_log) {
        Serial.printf("[BAT TX %uB]", static_cast<unsigned>(tx_len));
        for (size_t i = 0; i < tx_len && i < 24; ++i) Serial.printf(" %02X", tx[i]);
        Serial.println();
        Serial.printf("[BAT RX %uB]", static_cast<unsigned>(n));
        if (n <= 40) {
            for (size_t i = 0; i < n; ++i) Serial.printf(" %02X", rx[i]);
        } else {
            for (size_t i = 0; i < 32; ++i) Serial.printf(" %02X", rx[i]);
            Serial.print(" ...");
            for (size_t i = n - 8; i < n; ++i) Serial.printf(" %02X", rx[i]);
        }
        Serial.println();
    }
    return n;
}

static const char* proto_name(ShotoProtoKind p) {
    switch (p) {
        case PROTO_MODBUS_V12: return "SHOTO_MODBUS_V12";
        case PROTO_TBD_BINARY: return "TBD_BINARY";
        case PROTO_TBD_1363:   return "TBD_1363";
        case PROTO_SMARTLI_DCDC: return "SMARTLI_DCDC";
        default: return "AUTO";
    }
}

static bool poll_single_pack(
    ShotoProtoKind proto, uint8_t addr, const PolicyLimits& lim, PackState& out,
    uint32_t wait_override_ms = 0)
{
    uint8_t rx[256];
    if (proto == PROTO_MODBUS_V12) {
        uint8_t tx[8];
        build_shoto_modbus_v12_req(addr, tx);
        uint32_t w = (wait_override_ms > 0u) ? wait_override_ms : 500u;
        size_t n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), w, true);
        return parse_shoto_modbus_v12_resp(rx, n, addr, millis(), lim, out);
    }
    if (proto == PROTO_TBD_BINARY) {
        // SD-BA430 / BMS PLUS V1.634 responds immediately WITHOUT 0xAA prefix (0xAA breaks its parser!).
        uint32_t w = (wait_override_ms > 0u) ? wait_override_ms : 500u;
        for (uint8_t hdr : {uint8_t(0x7C), uint8_t(0x7E)}) {
            uint8_t tx[6];
            build_tbd_binary_req(addr, hdr, 0x01, tx);
            size_t n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), w, true);
            if (parse_tbd_binary_resp(rx, n, addr, millis(), lim, out)) {
                g_tbd_locked_no_wake = true;
                return true;
            }
        }
        if (g_tbd_locked_no_wake || g_active_proto != PROTO_AUTO) {
            return false;
        }
        // Fallback only during PROTO_AUTO scan for older TianBangda V1.1 boards needing 16x 0xAA UART wakeup
        static const uint8_t wake[16] = {
            0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,
            0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA
        };
        bat_set_tx(true);
        BatSerial.write(wake, sizeof(wake));
        BatSerial.flush();
        bat_set_tx(false);
        vTaskDelay(pdMS_TO_TICKS(25));
        for (uint8_t hdr : {uint8_t(0x7C), uint8_t(0x7E)}) {
            uint8_t tx[6];
            build_tbd_binary_req(addr, hdr, 0x01, tx);
            size_t n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), 650, true);
            if (parse_tbd_binary_resp(rx, n, addr, millis(), lim, out)) return true;
        }
        return false;
    }
    if (proto == PROTO_TBD_1363) {
        char tx[64];
        size_t tx_len = build_tbd_1363_req(addr, 0x42, tx, sizeof(tx));
        uint32_t w = (wait_override_ms > 0u) ? wait_override_ms : 400u;
        size_t n = uart_transact(BatSerial, reinterpret_cast<const uint8_t*>(tx), tx_len, rx, sizeof(rx) - 1, w, true);
        rx[n] = 0;
        const char* s = reinterpret_cast<const char*>(rx);
        const char* start = std::strchr(s, '~');
        const char* end = start ? std::strchr(start, '\r') : nullptr;
        if (!start || !end) return false;
        return parse_tbd_1363_42_resp(start, static_cast<size_t>(end - start + 1), addr, millis(), lim, out);
    }
    if (proto == PROTO_SMARTLI_DCDC) {
        uint8_t tx0[8], tx1[8], rx0[64], rx1[96];
        build_smartli_dcdc_req(addr, 0x0000, 7, tx0);
        uint32_t w0 = (wait_override_ms > 0u) ? wait_override_ms : 300u;
        size_t n0 = uart_transact(BatSerial, tx0, 8, rx0, sizeof(rx0), w0, true);
        vTaskDelay(pdMS_TO_TICKS(50));
        build_smartli_dcdc_req(addr, 0x1030, 16, tx1);
        uint32_t w1 = (wait_override_ms > 0u) ? wait_override_ms : 350u;
        size_t n1 = uart_transact(BatSerial, tx1, 8, rx1, sizeof(rx1), w1, true);
        return parse_smartli_dcdc_block(rx0, n0, rx1, n1, addr, millis(), lim, out);
    }
    return false;
}

static void print_pack_cells(uint8_t idx, const PackState& ps) {
    uint16_t dv = (ps.ext.max_cell_mv >= ps.ext.min_cell_mv)
        ? static_cast<uint16_t>(ps.ext.max_cell_mv - ps.ext.min_cell_mv)
        : 0u;
    Serial.printf("[CELLS P%u addr=%u n=%u min=%umV max=%umV dV=%umV]",
                  idx + 1, ps.addr, ps.ext.cell_count,
                  ps.ext.min_cell_mv, ps.ext.max_cell_mv, dv);
    for (uint8_t k = 0; k < ps.ext.cell_count; ++k) {
        Serial.printf(" C%02u=%u", k + 1, ps.ext.cell_mv[k]);
    }
    Serial.println();
}

static void print_hex_line(const char* prefix, const uint8_t* buf, size_t n) {
    Serial.print(prefix);
    size_t shown = (n > 200u) ? 200u : n;
    for (size_t i = 0; i < shown; ++i) {
        Serial.printf(" %02X", static_cast<unsigned>(buf[i]));
    }
    if (n > shown) Serial.print(" ...");
    Serial.println();
}

static void run_shoto_param_audit(uint8_t addr, const BankExt& snap) {
    uint8_t rx[260];
    Serial.printf("[PARAM START addr=%u proto=%s]\n",
                  static_cast<unsigned>(addr), proto_name(g_active_proto));

    struct TbdProtProbeSpec {
        uint8_t cmd;
        const char* name;
        float scale;
        const char* unit;
    };
    static const TbdProtProbeSpec specs[] = {
        {0x06, "cell_ov_prot_v",    0.001f, "V"},
        {0x07, "cell_ov_rec_v",     0.001f, "V"},
        {0x09, "cell_uv_prot_v",    0.001f, "V"},
        {0x0A, "cell_uv_rec_v",     0.001f, "V"},
        {0x2A, "pack_ov_prot_v",    0.01f,  "V"},
        {0x2B, "pack_ov_rec_v",     0.01f,  "V"},
        {0x2E, "pack_uv_prot_v",    0.01f,  "V"},
        {0x2F, "pack_uv_rec_v",     0.01f,  "V"},
        {0x35, "const_chg_v",       0.01f,  "V"},
        {0x2D, "const_chg_i",       0.01f,  "A_or_raw"},
        {0x32, "cell_sleep_v",      0.001f, "V"},
        {0x3E, "uv_off_interval",   1.0f,   "raw"},
        {0x50, "sleep_time",        1.0f,   "min"},
    };

    bool has_cell_uv_prot = false;
    float cell_uv_prot_v = 0.0f;
    bool has_pack_uv_prot = false;
    float pack_uv_prot_v = 0.0f;
    bool has_pack_uv_rec = false;
    float pack_uv_rec_v = 0.0f;
    bool has_const_chg_v = false;
    float const_chg_v = 0.0f;

    // A. TBD_BINARY read-only protection parameter probes (never send 0xAA)
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i) {
        esp_task_wdt_reset();
        uint8_t tx[6];
        build_tbd_binary_req(addr, 0x7C, specs[i].cmd, tx);
        size_t n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), 220u, true);
        if (n == 0u) {
            build_tbd_binary_req(addr, 0x7E, specs[i].cmd, tx);
            n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), 220u, true);
        }
        uint16_t raw = 0u;
        bool ok = parse_tbd_param_u16(rx, n, addr, specs[i].cmd, raw);
        float val = ok ? (static_cast<float>(raw) * specs[i].scale) : 0.0f;
        if (ok) {
            if (specs[i].cmd == 0x09) { has_cell_uv_prot = true; cell_uv_prot_v = val; }
            else if (specs[i].cmd == 0x2E) { has_pack_uv_prot = true; pack_uv_prot_v = val; }
            else if (specs[i].cmd == 0x2F) { has_pack_uv_rec = true; pack_uv_rec_v = val; }
            else if (specs[i].cmd == 0x35) { has_const_chg_v = true; const_chg_v = val; }
        }
        Serial.printf("[PARAM TBD_PROT addr=%u cmd=0x%02X name=%s ok=%s raw=%u val=%.3f%s rx=%uB hex=",
                      static_cast<unsigned>(addr),
                      static_cast<unsigned>(specs[i].cmd),
                      specs[i].name,
                      ok ? "YES" : "NO",
                      static_cast<unsigned>(raw),
                      val,
                      specs[i].unit,
                      static_cast<unsigned>(n));
        for (size_t k = 0; k < n && k < 24u; ++k) {
            Serial.printf("%s%02X", (k == 0u) ? "" : " ", static_cast<unsigned>(rx[k]));
        }
        Serial.println("]");
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    // B. TBD_BINARY read-only alarm parameter table (cmd 0x43)
    esp_task_wdt_reset();
    uint8_t tx43[6];
    build_tbd_binary_req(addr, 0x7C, 0x43, tx43);
    size_t n43 = uart_transact(BatSerial, tx43, sizeof(tx43), rx, sizeof(rx), 550u, true);
    if (n43 == 0u) {
        build_tbd_binary_req(addr, 0x7E, 0x43, tx43);
        n43 = uart_transact(BatSerial, tx43, sizeof(tx43), rx, sizeof(rx), 550u, true);
    }
    char pfx43[48];
    std::snprintf(pfx43, sizeof(pfx43), "[PARAM TBD_ALARM_43_RAW rx=%uB]", static_cast<unsigned>(n43));
    print_hex_line(pfx43, rx, n43);
    TbdAlarmParams ap{};
    bool has_ap = parse_tbd_alarm_params_43(rx, n43, addr, ap);
    if (has_ap) {
        Serial.printf("[PARAM TBD_ALARM_43 tags=%u cell_ov=%.3fV/%.3fV cell_uv=%.3fV/%.3fV pack_ov=%.2fV/%.2fV pack_uv_low_alarm=%.2fV/%.2fV low_soc_alarm=%u%%/%u%% cell_dv=%umV/%umV]\n",
                      static_cast<unsigned>(ap.tag_count),
                      static_cast<float>(ap.cell_ov_alarm_mv) * 0.001f,
                      static_cast<float>(ap.cell_ov_alarm_rec_mv) * 0.001f,
                      static_cast<float>(ap.cell_uv_alarm_mv) * 0.001f,
                      static_cast<float>(ap.cell_uv_alarm_rec_mv) * 0.001f,
                      static_cast<float>(ap.pack_ov_alarm_cv) * 0.01f,
                      static_cast<float>(ap.pack_ov_alarm_rec_cv) * 0.01f,
                      static_cast<float>(ap.pack_uv_alarm_cv) * 0.01f,
                      static_cast<float>(ap.pack_uv_alarm_rec_cv) * 0.01f,
                      static_cast<unsigned>(ap.low_soc_alarm_pct),
                      static_cast<unsigned>(ap.low_soc_alarm_rec_pct),
                      static_cast<unsigned>(ap.cell_diff_alarm_mv),
                      static_cast<unsigned>(ap.cell_diff_alarm_rec_mv));
    }

    // C. Shoto Modbus V1.2 FC04 (0x0FFF..0x1025) @ addr=214 & active addr
    bool has_mb_float = false;
    float mb_float_v = 0.0f;
    float mb_boost_v = 0.0f;
    uint8_t mb_probes[2] = {214u, addr};
    size_t mb_probe_cnt = (addr == 214u) ? 1u : 2u;
    for (size_t m = 0; m < mb_probe_cnt; ++m) {
        esp_task_wdt_reset();
        uint8_t paddr = mb_probes[m];
        uint8_t tx_mb[8];
        build_shoto_modbus_v12_req(paddr, tx_mb);
        size_t nmb = uart_transact(BatSerial, tx_mb, sizeof(tx_mb), rx, sizeof(rx), 350u, true);
        bool mb_ok = false, shifted = false;
        size_t reg_cnt = 0;
        uint16_t r34 = 0;
        float r37_v = 0.0f, r38_v = 0.0f;
        for (size_t s = 0; s + 5u <= nmb; ++s) {
            if (rx[s] != paddr || rx[s + 1] != 0x04 || rx[s + 2] < 6u) continue;
            size_t flen = static_cast<size_t>(rx[s + 2]) + 5u;
            if (s + flen > nmb) continue;
            uint16_t rx_crc = static_cast<uint16_t>(rx[s + flen - 2] | (rx[s + flen - 1] << 8));
            if (modbus_crc16(rx + s, flen - 2u) != rx_crc) continue;
            mb_ok = true;
            reg_cnt = static_cast<size_t>(rx[s + 2]) / 2u;
            auto reg = [&](size_t idx) -> uint16_t {
                return (idx < reg_cnt) ? be_u16(rx + s + 3u + 2u * idx) : 0u;
            };
            shifted = (reg_cnt > 13u && reg(12) > 16u && reg(13) >= 1u && reg(13) <= 16u);
            if (!shifted) {
                if (reg_cnt > 34u) r34 = reg(34);
                if (reg_cnt > 37u) r37_v = static_cast<float>(reg(37)) * 0.01f;
                if (reg_cnt > 38u) r38_v = static_cast<float>(reg(38)) * 0.01f;
                if (r37_v > 0.0f) {
                    has_mb_float = true;
                    mb_float_v = r37_v;
                    mb_boost_v = r38_v;
                }
            }
            break;
        }
        Serial.printf("[PARAM MODBUS_V12 addr=%u ok=%s rx=%uB regs=%u shifted=%s r34_max_i=%u r37_float_v=%.2fV r38_boost_v=%.2fV hex=",
                      static_cast<unsigned>(paddr),
                      mb_ok ? "YES" : "NO",
                      static_cast<unsigned>(nmb),
                      static_cast<unsigned>(reg_cnt),
                      shifted ? "YES" : "NO",
                      static_cast<unsigned>(r34),
                      r37_v,
                      r38_v);
        for (size_t k = 0; k < nmb && k < 100u; ++k) {
            Serial.printf("%s%02X", (k == 0u) ? "" : " ", static_cast<unsigned>(rx[k]));
        }
        Serial.println("]");
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    // D. YD/T 1363 CID2=47H read-only probe
    esp_task_wdt_reset();
    char info47[4], tx47[64];
    std::snprintf(info47, sizeof(info47), "%02X", static_cast<unsigned>(addr));
    size_t tx47_len = build_ydt1363_frame("26", "00", "46", "47", info47, tx47, sizeof(tx47));
    size_t n47 = uart_transact(BatSerial, reinterpret_cast<const uint8_t*>(tx47), tx47_len, rx, sizeof(rx), 350u, true);
    char pfx47[48];
    std::snprintf(pfx47, sizeof(pfx47), "[PARAM YDT1363_47H rx=%uB]", static_cast<unsigned>(n47));
    print_hex_line(pfx47, rx, n47);

    // E. 7-Parameter Audit Summary
    float pol_cvl = (snap.core.cvl > 0.0f) ? snap.core.cvl : POLICY_CVL_MAX_V;
    float pol_dvl = (snap.core.dvl > 0.0f) ? snap.core.dvl : POLICY_DVL_MIN_V;
    Serial.println("[PARAM SUMMARY]");
    if (has_ap) {
        Serial.printf("[PARAM MAP 1 Low_battery_alarm] Shoto_EEPROM=%.2fV(rec %.2fV)/SOC=%u%%(rec %u%%) | Bridge_DVL=%.2fV | Bridge_62H_PackUV=43.00V_CellUV=2.800V\n",
                      static_cast<float>(ap.pack_uv_alarm_cv) * 0.01f,
                      static_cast<float>(ap.pack_uv_alarm_rec_cv) * 0.01f,
                      static_cast<unsigned>(ap.low_soc_alarm_pct),
                      static_cast<unsigned>(ap.low_soc_alarm_rec_pct),
                      pol_dvl);
    } else {
        Serial.printf("[PARAM MAP 1 Low_battery_alarm] Shoto_EEPROM=N/A | Bridge_DVL=%.2fV | Bridge_62H_PackUV=43.00V_CellUV=2.800V\n",
                      pol_dvl);
    }
    Serial.println("[PARAM MAP 2 BMS_Lock_machine_battery_capacity] Shoto_EEPROM=N/A(inverter_side) | Inverter_ILS3600=Menu26_BSU | Inverter_IHF6200=BMS_Low_Power_SOC");
    if (has_pack_uv_rec) {
        Serial.printf("[PARAM MAP 3 battery_recharge_voltage] Shoto_EEPROM_UV_recovery=%.2fV | Inverter_side=LBU_BackToGrid\n", pack_uv_rec_v);
    } else {
        Serial.println("[PARAM MAP 3 battery_recharge_voltage] Shoto_EEPROM=N/A | Inverter_side=LBU_BackToGrid");
    }
    Serial.println("[PARAM MAP 4 paralel_shutdown_battery] Shoto_EEPROM=N/A(per_pack_MOS_antitheft_only) | Inverter_side=Parallel_Mode_Turn_Off_Voltage_SOC");
    if (has_pack_uv_prot || has_cell_uv_prot) {
        Serial.printf("[PARAM MAP 5 battery_cut_off_voltage] Shoto_EEPROM_PackUV=%.2fV CellUV=%.3fV(x15) | Bridge_DVL_63H=%.2fV\n",
                      pack_uv_prot_v, cell_uv_prot_v, pol_dvl);
    } else {
        Serial.printf("[PARAM MAP 5 battery_cut_off_voltage] Shoto_EEPROM=N/A | Bridge_DVL_63H=%.2fV\n", pol_dvl);
    }
    if (has_const_chg_v) {
        Serial.printf("[PARAM MAP 6 battery_constant_charging_voltage] Shoto_EEPROM_cmd35=%.2fV | Bridge_CVL_63H=%.2fV\n",
                      const_chg_v, pol_cvl);
    } else {
        Serial.printf("[PARAM MAP 6 battery_constant_charging_voltage] Shoto_EEPROM=N/A | Bridge_CVL_63H=%.2fV\n", pol_cvl);
    }
    if (has_mb_float) {
        Serial.printf("[PARAM MAP 7 battery_float_charging_voltage] Shoto_Modbus_r1024=%.2fV(boost %.2fV) | Bridge_CVL_63H=%.2fV\n",
                      mb_float_v, mb_boost_v, pol_cvl);
    } else {
        Serial.printf("[PARAM MAP 7 battery_float_charging_voltage] Shoto_EEPROM=N/A(TBD_only_has_cmd35;Modbus_shifted_no_r1024) | Bridge_CVL_63H=%.2fV\n",
                      pol_cvl);
    }
    Serial.println("[PARAM DONE]");
}

static bool run_autodetect(const PolicyLimits& lim) {
    Serial.println("[SCAN] Memulai deteksi otomatis 4 protokol Shoto (read-only)...");
    const uint8_t dcdc_addrs[] = {61, 62, 214, 215, 1, 2, 3, 4};
    const uint8_t mb_addrs[] = {38, 39, 40, 41, 61, 62, 214, 215, 1, 2, 3, 4};
    PackState tmp{};
    auto finish_scan = [&](ShotoProtoKind proto, uint8_t base_addr, const PackState& p, const char* lbl) {
        g_active_proto = proto;
        for (uint8_t i = 0; i < MAX_PACKS; ++i) {
            g_pack_addrs[i] = static_cast<uint8_t>(base_addr + i);
        }
        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            for (uint8_t i = 0; i < MAX_PACKS; ++i) {
                g_pack_online[i] = false;
                g_pack_miss[i] = 0;
            }
            g_packs[0] = p;
            g_pack_online[0] = true;
            xSemaphoreGive(g_snap_mutex);
        }
        g_force_rescan_packs = true;
        g_bat_poll_ok++;
        Serial.printf("[SCAN] COCOK! %s @ addr=%u (V=%.2fV SOC=%.1f%%)\n",
                      lbl, base_addr, p.core.bus_v, p.core.soc);
    };
    for (uint8_t a : {uint8_t(1), uint8_t(2)}) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_TBD_BINARY, a, lim, tmp)) {
            finish_scan(PROTO_TBD_BINARY, a, tmp, "TBD_BINARY");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    for (uint8_t a : mb_addrs) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_MODBUS_V12, a, lim, tmp)) {
            finish_scan(PROTO_MODBUS_V12, a, tmp, "SHOTO_MODBUS_V12");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    for (uint8_t a : dcdc_addrs) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_SMARTLI_DCDC, a, lim, tmp)) {
            finish_scan(PROTO_SMARTLI_DCDC, a, tmp, "SMARTLI_DCDC");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    for (uint8_t a : {uint8_t(1), uint8_t(2)}) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_TBD_1363, a, lim, tmp)) {
            finish_scan(PROTO_TBD_1363, a, tmp, "TBD_1363");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    Serial.println("[SCAN] Belum ada balasan dari Shoto. Cek kabel RJ45 Pin 1/2, polaritas A/B, atau ketik 'batraw' untuk lihat RX mentah.");
    return false;
}

static void bat_poll_task(void* arg) {
    (void)arg;
    esp_task_wdt_add(nullptr);
    PolicyLimits lim;
    lim.cvl_v = POLICY_CVL_MAX_V;
    lim.dvl_v = POLICY_DVL_MIN_V;
    float max_ccl_a = POLICY_CCL_PER_PACK_A * static_cast<float>(MAX_PACKS);
    float max_dcl_a = POLICY_DCL_PER_PACK_A * static_cast<float>(MAX_PACKS);
    lim.ccl_a = (POLICY_CCL_1PACK_A > max_ccl_a) ? POLICY_CCL_1PACK_A : max_ccl_a;
    lim.dcl_a = (POLICY_DCL_1PACK_A > max_dcl_a) ? POLICY_DCL_1PACK_A : max_dcl_a;

    DynamicLimits dyn_lim;
    dyn_lim.ccl_1pack_a = POLICY_CCL_1PACK_A;
    dyn_lim.dcl_1pack_a = POLICY_DCL_1PACK_A;
    dyn_lim.ccl_per_pack_a = POLICY_CCL_PER_PACK_A;
    dyn_lim.dcl_per_pack_a = POLICY_DCL_PER_PACK_A;
    dyn_lim.ccl_max_a = POLICY_CCL_BANK_MAX_A;
    dyn_lim.dcl_max_a = POLICY_DCL_BANK_MAX_A;

    Policy pol;
    pol.approved = POLICY_USER_APPROVED;
    pol.freshness_ms = POLICY_FRESHNESS_MS;
    pol.max_delta_v = POLICY_MAX_DELTA_V;
    pol.charge_cap = 0.0f;
    pol.discharge_cap = 0.0f;

    uint32_t last_log = 0;
    uint32_t last_discovery_ms = 0;
    uint8_t probe_cursor = 0;
    size_t prev_active_count = 0;
    bool prev_bank_valid = false;

    for (;;) {
        esp_task_wdt_reset();
        if (g_active_proto == PROTO_AUTO) {
            if (!run_autodetect(lim)) {
                if (g_dump_params) {
                    g_dump_params = false;
                    BankExt empty_bank{};
                    run_shoto_param_audit(g_pack_addrs[0], empty_bank);
                }
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        bool was_online[MAX_PACKS]{};
        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            for (uint8_t i = 0; i < MAX_PACKS; ++i) was_online[i] = g_pack_online[i];
            xSemaphoreGive(g_snap_mutex);
        }

        if (!g_dynamic_discovery) {
            g_force_rescan_packs = false;
            uint8_t fixed_n = (FIXED_PACK_COUNT < 1) ? 1
                : ((FIXED_PACK_COUNT > MAX_PACKS) ? MAX_PACKS : static_cast<uint8_t>(FIXED_PACK_COUNT));
            for (uint8_t idx = 0; idx < fixed_n; ++idx) {
                esp_task_wdt_reset();
                PackState fresh{};
                if (poll_single_pack(g_active_proto, g_pack_addrs[idx], lim, fresh)) {
                    g_bat_poll_ok++;
                    if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_packs[idx] = fresh;
                        g_pack_online[idx] = true;
                        g_pack_miss[idx] = 0;
                        xSemaphoreGive(g_snap_mutex);
                    }
                } else {
                    g_bat_poll_fail++;
                }
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            last_discovery_ms = millis();
        } else {
            bool do_full_scan = g_force_rescan_packs;
            if (do_full_scan) g_force_rescan_packs = false;
            bool has_online = false;

            if (do_full_scan) {
                for (uint8_t idx = 0; idx < MAX_PACKS; ++idx) {
                    esp_task_wdt_reset();
                    uint32_t w = was_online[idx] ? 0u : 320u;
                    PackState fresh{};
                    if (poll_single_pack(g_active_proto, g_pack_addrs[idx], lim, fresh, w)) {
                        g_bat_poll_ok++;
                        bool newly_online = false;
                        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            newly_online = !g_pack_online[idx];
                            g_packs[idx] = fresh;
                            g_pack_online[idx] = true;
                            g_pack_miss[idx] = 0;
                            xSemaphoreGive(g_snap_mutex);
                        }
                        if (newly_online) {
                            Serial.printf("[PACK+] Pack#%u (addr=%u) ONLINE V=%.2fV SOC=%.1f%%\n",
                                          idx + 1, g_pack_addrs[idx], fresh.core.bus_v, fresh.core.soc);
                        }
                        has_online = true;
                    } else if (was_online[idx]) {
                        g_bat_poll_fail++;
                        bool went_offline = false;
                        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            g_pack_miss[idx]++;
                            if (g_pack_miss[idx] >= PACK_OFFLINE_MISSES) {
                                g_pack_online[idx] = false;
                                g_pack_miss[idx] = 0;
                                went_offline = true;
                            }
                            xSemaphoreGive(g_snap_mutex);
                        }
                        if (went_offline) {
                            Serial.printf("[PACK-] Pack#%u (addr=%u) OFFLINE (%ux miss) -> auto-rescale\n",
                                          idx + 1, g_pack_addrs[idx], static_cast<unsigned>(PACK_OFFLINE_MISSES));
                        } else {
                            has_online = true;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(40));
                }
                last_discovery_ms = millis();
            } else {
                for (uint8_t idx = 0; idx < MAX_PACKS; ++idx) {
                    if (!was_online[idx]) continue;
                    esp_task_wdt_reset();
                    PackState fresh{};
                    if (poll_single_pack(g_active_proto, g_pack_addrs[idx], lim, fresh, 0)) {
                        g_bat_poll_ok++;
                        has_online = true;
                        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            g_packs[idx] = fresh;
                            g_pack_online[idx] = true;
                            g_pack_miss[idx] = 0;
                            xSemaphoreGive(g_snap_mutex);
                        }
                    } else {
                        g_bat_poll_fail++;
                        bool went_offline = false;
                        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            g_pack_miss[idx]++;
                            if (g_pack_miss[idx] >= PACK_OFFLINE_MISSES) {
                                g_pack_online[idx] = false;
                                g_pack_miss[idx] = 0;
                                went_offline = true;
                            }
                            xSemaphoreGive(g_snap_mutex);
                        }
                        if (went_offline) {
                            Serial.printf("[PACK-] Pack#%u (addr=%u) OFFLINE (%ux miss) -> auto-rescale\n",
                                          idx + 1, g_pack_addrs[idx], static_cast<unsigned>(PACK_OFFLINE_MISSES));
                        } else {
                            has_online = true;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(25));
                }

                if (!has_online || (millis() - last_discovery_ms >= DISCOVERY_INTERVAL_MS)) {
                    uint8_t target = MAX_PACKS;
                    for (uint8_t step = 0; step < MAX_PACKS; ++step) {
                        uint8_t cand = static_cast<uint8_t>((probe_cursor + step) % MAX_PACKS);
                        if (!was_online[cand]) {
                            target = cand;
                            break;
                        }
                    }
                    if (target < MAX_PACKS) {
                        probe_cursor = static_cast<uint8_t>((target + 1u) % MAX_PACKS);
                        last_discovery_ms = millis();
                        esp_task_wdt_reset();
                        PackState fresh{};
                        if (poll_single_pack(g_active_proto, g_pack_addrs[target], lim, fresh, 320)) {
                            g_bat_poll_ok++;
                            if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                                g_packs[target] = fresh;
                                g_pack_online[target] = true;
                                g_pack_miss[target] = 0;
                                xSemaphoreGive(g_snap_mutex);
                            }
                            Serial.printf("[PACK+] Pack#%u (addr=%u) HOT-PLUG ONLINE V=%.2fV SOC=%.1f%% -> auto-rescale\n",
                                          target + 1, g_pack_addrs[target], fresh.core.bus_v, fresh.core.soc);
                        }
                    }
                }
            }
        }

        auto collect_active_packs = [](PackState* active_out, uint8_t* slots_out) -> size_t {
            size_t cnt = 0;
            if (!g_dynamic_discovery) {
                uint8_t fixed_n = (FIXED_PACK_COUNT < 1) ? 1
                    : ((FIXED_PACK_COUNT > MAX_PACKS) ? MAX_PACKS : static_cast<uint8_t>(FIXED_PACK_COUNT));
                for (uint8_t i = 0; i < fixed_n; ++i) {
                    active_out[cnt] = g_packs[i];
                    slots_out[cnt] = i;
                    cnt++;
                }
            } else {
                for (uint8_t i = 0; i < MAX_PACKS; ++i) {
                    if (g_pack_online[i]) {
                        active_out[cnt] = g_packs[i];
                        slots_out[cnt] = i;
                        cnt++;
                    }
                }
            }
            return cnt;
        };

        size_t active_count = 0;
        bool got_snap = false;
        bool scale_changed = false;
        bool do_param_audit = false;
        uint8_t param_addr = g_pack_addrs[0];
        PackState active_packs[MAX_PACKS]{};
        uint8_t active_slots[MAX_PACKS] = {};
        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            got_snap = true;
            active_count = collect_active_packs(active_packs, active_slots);
            if (g_dump_params) {
                g_dump_params = false;
                do_param_audit = true;
                for (uint8_t i = 0; i < MAX_PACKS; ++i) {
                    if (g_pack_online[i] && g_pack_addrs[i] > 0u) {
                        param_addr = g_pack_addrs[i];
                        break;
                    }
                }
            }
            xSemaphoreGive(g_snap_mutex);
        }

        BankExt new_bank{};
        if (got_snap) {
            compute_dynamic_caps(active_count, dyn_lim, pol.charge_cap, pol.discharge_cap);
            uint32_t now_agg = millis();
            BankExt cand_bank{};
            aggregate_bank(active_packs, active_count, now_agg, pol, cand_bank);
            float cur_dv = 0.0f;
            uint32_t cur_age = 0u;
            InhibitReason reason = classify_bank_inhibit(active_packs, active_count, now_agg, pol, &cur_dv, &cur_age);

            bool did_dv_retry = false;
            bool dv_retry_recovered = false;
            if (!cand_bank.core.valid && reason == INHIBIT_DELTA_V && active_count >= 2u) {
                Serial.printf("[RETRY] Transient deltaV=%.2fV > %.2fV -> re-polling %u packs...\n",
                              cur_dv, pol.max_delta_v, static_cast<unsigned>(active_count));
                for (size_t i = 0; i < active_count; ++i) {
                    uint8_t slot = active_slots[i];
                    esp_task_wdt_reset();
                    PackState fresh{};
                    if (poll_single_pack(g_active_proto, g_pack_addrs[slot], lim, fresh, 0)) {
                        g_bat_poll_ok++;
                        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            g_packs[slot] = fresh;
                            g_pack_online[slot] = true;
                            g_pack_miss[slot] = 0;
                            xSemaphoreGive(g_snap_mutex);
                        }
                    } else {
                        g_bat_poll_fail++;
                    }
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    active_count = collect_active_packs(active_packs, active_slots);
                    xSemaphoreGive(g_snap_mutex);
                }
                compute_dynamic_caps(active_count, dyn_lim, pol.charge_cap, pol.discharge_cap);
                now_agg = millis();
                cand_bank = BankExt{};
                aggregate_bank(active_packs, active_count, now_agg, pol, cand_bank);
                reason = classify_bank_inhibit(active_packs, active_count, now_agg, pol, &cur_dv, &cur_age);
                did_dv_retry = true;
                dv_retry_recovered = cand_bank.core.valid;
            }

            bool log_inhibit = false;
            uint32_t inh_num = 0;
            if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_bank = cand_bank;
                if (did_dv_retry) {
                    if (dv_retry_recovered) g_delta_v_retry_ok++;
                    else g_delta_v_retry_fail++;
                }
                if (active_count >= 2u && cur_dv > g_peak_delta_v) g_peak_delta_v = cur_dv;
                if (active_count >= 1u && cur_age > g_peak_age_ms) g_peak_age_ms = cur_age;
                if (!cand_bank.core.valid && active_count > 0u && (prev_bank_valid || g_inhibit_count == 0u)) {
                    g_inhibit_count++;
                    g_last_inhibit_reason = reason;
                    g_last_inhibit_ms = now_agg;
                    g_last_inhibit_dv = cur_dv;
                    g_last_inhibit_age_ms = cur_age;
                    inh_num = g_inhibit_count;
                    log_inhibit = true;
                }
                prev_bank_valid = cand_bank.core.valid;
                new_bank = g_bank;
                scale_changed = (active_count != prev_active_count);
                prev_active_count = active_count;
                xSemaphoreGive(g_snap_mutex);
            }
            if (log_inhibit) {
                Serial.printf("[INHIBIT #%lu] reason=%s dV=%.2fV maxAge=%lums packs=%u\n",
                              static_cast<unsigned long>(inh_num),
                              inhibit_reason_name(reason),
                              cur_dv,
                              static_cast<unsigned long>(cur_age),
                              static_cast<unsigned>(active_count));
            }
        }
        if (scale_changed) {
            Serial.printf("[SCALE] active=%u cap_target=%.0fAh CCL_lim=%.1fA DCL_lim=%.1fA bank=%s (CCL=%.1fA DCL=%.1fA)\n",
                          static_cast<unsigned>(active_count),
                          static_cast<float>(active_count) * 100.0f,
                          pol.charge_cap, pol.discharge_cap,
                          new_bank.core.valid ? "YES" : "INHIBIT",
                          new_bank.core.ccl, new_bank.core.dcl);
        }
        if (do_param_audit) {
            run_shoto_param_audit(param_addr, new_bank);
        }

        if (millis() - last_log >= 5000) {
            last_log = millis();
            BankExt snap{};
            PackState packs_snap[MAX_PACKS]{};
            bool online_snap[MAX_PACKS]{};
            uint32_t max_age = 0;
            uint32_t now_log = millis();
            if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                snap = g_bank;
                for (uint8_t i = 0; i < MAX_PACKS; ++i) {
                    packs_snap[i] = g_packs[i];
                    online_snap[i] = g_pack_online[i];
                    if (g_pack_online[i] && packs_snap[i].core.complete) {
                        uint32_t a = now_log - packs_snap[i].core.sampled_ms;
                        if (a > max_age) max_age = a;
                    }
                }
                xSemaphoreGive(g_snap_mutex);
            }
            float disp_soc = snap.core.valid ? snap.core.soc : snap.raw_soc;
            float disp_i = snap.core.valid ? snap.core.bus_a : snap.raw_bus_a;
            Serial.printf("[BAT %s] packs=%u valid=%s V=%.2fV I=%.2fA SOC=%.1f%% T=%.1fC CCL=%.1fA DCL=%.1fA ok=%lu err=%lu age=%lums | INV rx=%lu tx=%lu bytes=%lu\n",
                          proto_name(g_active_proto), snap.pack_count,
                          snap.core.valid ? "YES" : "INHIBIT",
                          snap.bus_v, disp_i, disp_soc, snap.avg_temp_c,
                          snap.core.ccl, snap.core.dcl,
                          static_cast<unsigned long>(g_bat_poll_ok),
                          static_cast<unsigned long>(g_bat_poll_fail),
                          static_cast<unsigned long>(max_age),
                          static_cast<unsigned long>(g_inv_req_count),
                          static_cast<unsigned long>(g_inv_resp_count),
                          static_cast<unsigned long>(g_inv_raw_bytes));
            if (g_auto_cell_log) {
                for (uint8_t idx = 0; idx < MAX_PACKS; ++idx) {
                    if (online_snap[idx] && packs_snap[idx].ext.cell_count > 0) {
                        print_pack_cells(idx, packs_snap[idx]);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

static void inv_bridge_task(void* arg) {
    (void)arg;
    esp_task_wdt_add(nullptr);
    char rx_buf[256];
    size_t rx_pos = 0;
    uint8_t raw_chunk[32];
    size_t raw_n = 0;
    uint32_t raw_last_ms = 0;
    auto flush_raw = [&]() {
        if (g_raw_log && raw_n > 0) {
            Serial.printf("[INV RAW %uB]", static_cast<unsigned>(raw_n));
            for (size_t i = 0; i < raw_n; ++i) Serial.printf(" %02X", raw_chunk[i]);
            Serial.println();
        }
        raw_n = 0;
    };

    for (;;) {
        esp_task_wdt_reset();
        while (InvSerial.available()) {
            int b = InvSerial.read();
            if (b < 0) break;
            g_inv_raw_bytes++;
            if (g_raw_log) {
                raw_chunk[raw_n++] = static_cast<uint8_t>(b);
                raw_last_ms = millis();
                if (raw_n >= sizeof(raw_chunk)) flush_raw();
            }
            char c = static_cast<char>(b);
            if (c == '~') {
                rx_pos = 0;
                rx_buf[rx_pos++] = c;
            } else if (rx_pos > 0) {
                if (rx_pos < sizeof(rx_buf) - 1) {
                    rx_buf[rx_pos++] = c;
                } else {
                    rx_pos = 0; // overflow protection
                }
                if (c == '\r') {
                    rx_buf[rx_pos] = '\0';
                    g_inv_req_count++;
                    g_last_inv_rx_ms = millis();
                    if (g_raw_log) {
                        Serial.printf("[INV RX] %s\n", rx_buf);
                    }
                    BankExt snap{};
                    if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        snap = g_bank;
                        xSemaphoreGive(g_snap_mutex);
                    } else {
                        g_inv_mutex_miss++;
                    }
#if BRIDGE_MODE == MODE_BRIDGE_ACTIVE
                    char resp[360];
                    size_t resp_len = handle_pylontech_request(rx_buf, rx_pos, snap, resp, sizeof(resp));
                    if (resp_len > 0) {
                        InvSerial.write(reinterpret_cast<const uint8_t*>(resp), resp_len);
                        InvSerial.flush();
                        g_inv_resp_count++;
                        uint32_t now_tx = millis();
                        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                            if (rx_pos >= 18u) {
                                if (rx_buf[7] == '6' && rx_buf[8] == '0') {
                                    g_inv_60_cnt++;
                                } else if (rx_buf[7] == '6' && rx_buf[8] == '1') {
                                    g_inv_61_cnt++;
                                    g_inv_last_61_ms = now_tx;
                                    copy_tx_stripped(g_inv_last_tx_61, sizeof(g_inv_last_tx_61), resp, resp_len);
                                } else if (rx_buf[7] == '6' && rx_buf[8] == '2') {
                                    g_inv_62_cnt++;
                                    g_inv_last_62_ms = now_tx;
                                    copy_tx_stripped(g_inv_last_tx_62, sizeof(g_inv_last_tx_62), resp, resp_len);
                                } else if (rx_buf[7] == '6' && rx_buf[8] == '3') {
                                    g_inv_63_cnt++;
                                    g_inv_last_63_ms = now_tx;
                                    copy_tx_stripped(g_inv_last_tx_63, sizeof(g_inv_last_tx_63), resp, resp_len);
                                } else {
                                    g_inv_other_cnt++;
                                }
                            } else {
                                g_inv_other_cnt++;
                            }
                            xSemaphoreGive(g_snap_mutex);
                        } else {
                            g_inv_mutex_miss++;
                        }
                        if (g_raw_log) {
                            Serial.printf("[INV TX] %s\n", resp);
                        }
                    }
#endif
                    rx_pos = 0;
                }
            }
        }
        if (raw_n > 0 && (millis() - raw_last_ms >= 40)) {
            flush_raw();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void handle_serial_cli() {
    static char cmd_buf[48];
    static size_t pos = 0;
    while (Serial.available()) {
        char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (pos == 0) continue;
            cmd_buf[pos] = '\0';
            pos = 0;

            BankExt snap{};
            PackState ps_snap[MAX_PACKS]{};
            bool online_snap[MAX_PACKS]{};
            uint8_t miss_snap[MAX_PACKS]{};
            struct DiagSnap {
                uint32_t inhibit_count;
                uint32_t dv_retry_ok;
                uint32_t dv_retry_fail;
                InhibitReason last_inhibit_reason;
                uint32_t last_inhibit_ms;
                float last_inhibit_dv;
                uint32_t last_inhibit_age_ms;
                float peak_dv;
                uint32_t peak_age_ms;
                uint32_t inv_60;
                uint32_t inv_61;
                uint32_t inv_62;
                uint32_t inv_63;
                uint32_t inv_other;
                uint32_t inv_mtx_miss;
                char tx61[144];
                char tx62[40];
                char tx63[48];
                uint32_t tx61_ms;
                uint32_t tx62_ms;
                uint32_t tx63_ms;
            };
            DiagSnap dsnap = {};
            if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                snap = g_bank;
                for (uint8_t i = 0; i < MAX_PACKS; ++i) {
                    ps_snap[i] = g_packs[i];
                    online_snap[i] = g_pack_online[i];
                    miss_snap[i] = g_pack_miss[i];
                }
                dsnap.inhibit_count = g_inhibit_count;
                dsnap.dv_retry_ok = g_delta_v_retry_ok;
                dsnap.dv_retry_fail = g_delta_v_retry_fail;
                dsnap.last_inhibit_reason = g_last_inhibit_reason;
                dsnap.last_inhibit_ms = g_last_inhibit_ms;
                dsnap.last_inhibit_dv = g_last_inhibit_dv;
                dsnap.last_inhibit_age_ms = g_last_inhibit_age_ms;
                dsnap.peak_dv = g_peak_delta_v;
                dsnap.peak_age_ms = g_peak_age_ms;
                dsnap.inv_60 = g_inv_60_cnt;
                dsnap.inv_61 = g_inv_61_cnt;
                dsnap.inv_62 = g_inv_62_cnt;
                dsnap.inv_63 = g_inv_63_cnt;
                dsnap.inv_other = g_inv_other_cnt;
                dsnap.inv_mtx_miss = g_inv_mutex_miss;
                std::memcpy(dsnap.tx61, g_inv_last_tx_61, sizeof(dsnap.tx61));
                std::memcpy(dsnap.tx62, g_inv_last_tx_62, sizeof(dsnap.tx62));
                std::memcpy(dsnap.tx63, g_inv_last_tx_63, sizeof(dsnap.tx63));
                dsnap.tx61_ms = g_inv_last_61_ms;
                dsnap.tx62_ms = g_inv_last_62_ms;
                dsnap.tx63_ms = g_inv_last_63_ms;
                xSemaphoreGive(g_snap_mutex);
            }

            if (std::strcmp(cmd_buf, "status") == 0) {
                Serial.printf("Proto=%s | Bridge=%s | PolicyApproved=%s | DynDiscovery=%s | Packs=%u/%u | BankValid=%s\n",
                              proto_name(g_active_proto),
                              BRIDGE_MODE == MODE_BRIDGE_ACTIVE ? "ACTIVE" : "MONITOR_ONLY",
                              POLICY_USER_APPROVED ? "YES" : "NO",
                              g_dynamic_discovery ? "ON" : "FIXED",
                              snap.pack_count, static_cast<unsigned>(MAX_PACKS),
                              snap.core.valid ? "YES" : "INHIBIT");
                Serial.printf("Bank: V=%.2fV I=%.2fA SOC=%.1f%% (rawSOC=%.1f%%) SOH=%.1f%% CVL=%.2fV DVL=%.2fV CCL=%.1fA DCL=%.1fA | ok=%lu err=%lu minC=%umV maxC=%umV\n",
                              snap.bus_v,
                              snap.core.valid ? snap.core.bus_a : snap.raw_bus_a,
                              snap.core.valid ? snap.core.soc : snap.raw_soc,
                              snap.raw_soc, snap.soh,
                              snap.core.cvl, snap.core.dvl, snap.core.ccl, snap.core.dcl,
                              static_cast<unsigned long>(g_bat_poll_ok),
                              static_cast<unsigned long>(g_bat_poll_fail),
                              snap.min_cell_mv, snap.max_cell_mv);
                Serial.printf("Diag: inhibit=%lu (last=%s dV=%.2fV age=%lums) dvRetry=%lu/%lu peakDV=%.2fV peakAge=%lums | INV 60=%lu 61=%lu 62=%lu 63=%lu oth=%lu mtxMiss=%lu\n",
                              static_cast<unsigned long>(dsnap.inhibit_count),
                              inhibit_reason_name(dsnap.last_inhibit_reason),
                              dsnap.last_inhibit_dv,
                              static_cast<unsigned long>(dsnap.last_inhibit_age_ms),
                              static_cast<unsigned long>(dsnap.dv_retry_ok),
                              static_cast<unsigned long>(dsnap.dv_retry_fail),
                              dsnap.peak_dv,
                              static_cast<unsigned long>(dsnap.peak_age_ms),
                              static_cast<unsigned long>(dsnap.inv_60),
                              static_cast<unsigned long>(dsnap.inv_61),
                              static_cast<unsigned long>(dsnap.inv_62),
                              static_cast<unsigned long>(dsnap.inv_63),
                              static_cast<unsigned long>(dsnap.inv_other),
                              static_cast<unsigned long>(dsnap.inv_mtx_miss));
                for (int i = 0; i < MAX_PACKS; ++i) {
                    const PackState& ps = ps_snap[i];
                    if (online_snap[i]) {
                        Serial.printf("  Pack#%d (addr=%u) [ONLINE]: age=%lums V=%.2fV I=%.2fA SOC=%.1f%% prot=%#06x fault=%#04x\n",
                                      i + 1, g_pack_addrs[i], static_cast<unsigned long>(millis() - ps.core.sampled_ms),
                                      ps.core.bus_v, ps.core.bus_a, ps.core.soc,
                                      ps.ext.prot_flags, ps.ext.fault_flags);
                    } else {
                        Serial.printf("  Pack#%d (addr=%u) [OFFLINE]\n", i + 1, g_pack_addrs[i]);
                    }
                }
            } else if (std::strcmp(cmd_buf, "diag") == 0) {
                const char* inv_mode = (dsnap.inv_63 > 0u && dsnap.inv_62 > 0u)
                    ? "PYL_FULL (63H+61H+62H)"
                    : ((dsnap.inv_61 > 0u && dsnap.inv_63 == 0u) ? "SCAN_61H_ONLY (USE/LIA)" : "IDLE/OTHER");
                uint32_t now_cli = millis();
                Serial.printf("[DIAG BANK] valid=%s packs=%u/%u inhibit=%lu (last=%s @-%lums dV=%.2fV age=%lums) dvRetry=%lu/%lu peakDV=%.2fV peakAge=%lums bat_ok=%lu bat_err=%lu\n",
                              snap.core.valid ? "YES" : "INHIBIT",
                              snap.pack_count, static_cast<unsigned>(MAX_PACKS),
                              static_cast<unsigned long>(dsnap.inhibit_count),
                              inhibit_reason_name(dsnap.last_inhibit_reason),
                              static_cast<unsigned long>(dsnap.last_inhibit_ms > 0u ? (now_cli - dsnap.last_inhibit_ms) : 0u),
                              dsnap.last_inhibit_dv,
                              static_cast<unsigned long>(dsnap.last_inhibit_age_ms),
                              static_cast<unsigned long>(dsnap.dv_retry_ok),
                              static_cast<unsigned long>(dsnap.dv_retry_fail),
                              dsnap.peak_dv,
                              static_cast<unsigned long>(dsnap.peak_age_ms),
                              static_cast<unsigned long>(g_bat_poll_ok),
                              static_cast<unsigned long>(g_bat_poll_fail));
                Serial.printf("[DIAG INV] mode=%s rx=%lu tx=%lu | 60H=%lu 61H=%lu 62H=%lu 63H=%lu other=%lu mtxMiss=%lu\n",
                              inv_mode,
                              static_cast<unsigned long>(g_inv_req_count),
                              static_cast<unsigned long>(g_inv_resp_count),
                              static_cast<unsigned long>(dsnap.inv_60),
                              static_cast<unsigned long>(dsnap.inv_61),
                              static_cast<unsigned long>(dsnap.inv_62),
                              static_cast<unsigned long>(dsnap.inv_63),
                              static_cast<unsigned long>(dsnap.inv_other),
                              static_cast<unsigned long>(dsnap.inv_mtx_miss));
                Serial.printf("[DIAG TX63] age=%lums %s\n",
                              static_cast<unsigned long>(dsnap.tx63_ms > 0u ? (now_cli - dsnap.tx63_ms) : 0u),
                              dsnap.tx63[0] ? dsnap.tx63 : "<none>");
                Serial.printf("[DIAG TX62] age=%lums %s\n",
                              static_cast<unsigned long>(dsnap.tx62_ms > 0u ? (now_cli - dsnap.tx62_ms) : 0u),
                              dsnap.tx62[0] ? dsnap.tx62 : "<none>");
                Serial.printf("[DIAG TX61] age=%lums %s\n",
                              static_cast<unsigned long>(dsnap.tx61_ms > 0u ? (now_cli - dsnap.tx61_ms) : 0u),
                              dsnap.tx61[0] ? dsnap.tx61 : "<none>");
            } else if (std::strcmp(cmd_buf, "packs") == 0) {
                Serial.printf("DynDiscovery=%s | ActivePacks=%u/%u | BankValid=%s | Cap=%.0fAh CCL=%.1fA DCL=%.1fA\n",
                              g_dynamic_discovery ? "ON" : "FIXED",
                              snap.pack_count, static_cast<unsigned>(MAX_PACKS),
                              snap.core.valid ? "YES" : "INHIBIT",
                              snap.core.capacity_ah, snap.core.ccl, snap.core.dcl);
                for (int i = 0; i < MAX_PACKS; ++i) {
                    const PackState& ps = ps_snap[i];
                    if (online_snap[i]) {
                        Serial.printf("  Slot#%d (addr=%u): ONLINE  V=%.2fV I=%.2fA SOC=%.1f%% miss=%u age=%lums\n",
                                      i + 1, g_pack_addrs[i], ps.core.bus_v, ps.core.bus_a, ps.core.soc,
                                      miss_snap[i], static_cast<unsigned long>(millis() - ps.core.sampled_ms));
                    } else {
                        Serial.printf("  Slot#%d (addr=%u): OFFLINE\n", i + 1, g_pack_addrs[i]);
                    }
                }
            } else if (std::strcmp(cmd_buf, "rescan") == 0) {
                g_force_rescan_packs = true;
                Serial.println("Memicu rescan seluruh slot Pack #1..#4 (addr 1..4)...");
            } else if (std::strcmp(cmd_buf, "cells") == 0) {
                bool printed = false;
                for (int i = 0; i < MAX_PACKS; ++i) {
                    if (online_snap[i] && ps_snap[i].ext.cell_count > 0) {
                        print_pack_cells(static_cast<uint8_t>(i), ps_snap[i]);
                        printed = true;
                    }
                }
                if (!printed) {
                    Serial.println("[CELLS] Belum ada pack online / data sel.");
                }
            } else if (std::strcmp(cmd_buf, "celllog") == 0) {
                g_auto_cell_log = !g_auto_cell_log;
                Serial.printf("Auto per-cell logging: %s\n", g_auto_cell_log ? "ON" : "OFF");
            } else if (std::strcmp(cmd_buf, "raw") == 0) {
                g_raw_log = !g_raw_log;
                Serial.printf("Raw Inverter frame logging: %s\n", g_raw_log ? "ON" : "OFF");
            } else if (std::strcmp(cmd_buf, "batraw") == 0) {
                g_bat_raw_log = !g_bat_raw_log;
                Serial.printf("Raw Battery RS485 frame logging: %s\n", g_bat_raw_log ? "ON" : "OFF");
            } else if (std::strcmp(cmd_buf, "scan") == 0) {
                g_tbd_locked_no_wake = false;
                g_active_proto = PROTO_AUTO;
                Serial.println("Memicu ulang auto-detect protokol Shoto...");
            } else if (std::strcmp(cmd_buf, "params") == 0) {
                g_dump_params = true;
                Serial.println("[PARAM] Audit parameter read-only Shoto dijadwalkan pada siklus Core 0 berikutnya...");
            } else {
                Serial.println("Perintah: status | diag | packs | rescan | cells | celllog | raw | batraw | scan | params | help");
            }
        } else if (pos < sizeof(cmd_buf) - 1) {
            cmd_buf[pos++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\n=== SHOTO <-> SAMOTO BMS BRIDGE v3.0 ===");
    Serial.printf("Mode=%s | PolicyApproved=%s | Proto=%s | DynDiscovery=%s | MaxPacks=%d | FixedPacks=%d\n",
                  BRIDGE_MODE == MODE_BRIDGE_ACTIVE ? "ACTIVE" : "MONITOR_ONLY",
                  POLICY_USER_APPROVED ? "YES" : "NO",
                  proto_name(g_active_proto),
                  g_dynamic_discovery ? "ON" : "FIXED",
                  static_cast<int>(MAX_PACKS),
                  FIXED_PACK_COUNT);

#if BAT_DE_PIN >= 0
    pinMode(BAT_DE_PIN, OUTPUT);
    digitalWrite(BAT_DE_PIN, LOW);
#endif
    BatSerial.begin(SHOTO_BAUD, SERIAL_8N1, BAT_RX_PIN, BAT_TX_PIN);
    InvSerial.begin(INVERTER_BAUD, SERIAL_8N1, INV_RX_PIN, INV_TX_PIN);

    g_snap_mutex = xSemaphoreCreateMutex();
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdt_cfg = {};
    wdt_cfg.timeout_ms = 10000;
    wdt_cfg.idle_core_mask = 0;
    wdt_cfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdt_cfg) == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_init(&wdt_cfg);
    }
#else
    esp_task_wdt_init(10, true);
#endif

    xTaskCreatePinnedToCore(bat_poll_task, "bat_poll", 6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(inv_bridge_task, "inv_bridge", 6144, nullptr, 3, nullptr, 1);
    Serial.println("Ketik 'help' untuk perintah diagnosa.");
}

void loop() {
    handle_serial_cli();
    delay(25);
}
