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

    for (;;) {
        esp_task_wdt_reset();
        if (g_active_proto == PROTO_AUTO) {
            if (!run_autodetect(lim)) {
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
                vTaskDelay(pdMS_TO_TICKS(80));
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
                    vTaskDelay(pdMS_TO_TICKS(80));
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

        size_t active_count = 0;
        bool scale_changed = false;
        BankExt new_bank{};
        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            PackState active_packs[MAX_PACKS]{};
            if (!g_dynamic_discovery) {
                uint8_t fixed_n = (FIXED_PACK_COUNT < 1) ? 1
                    : ((FIXED_PACK_COUNT > MAX_PACKS) ? MAX_PACKS : static_cast<uint8_t>(FIXED_PACK_COUNT));
                for (uint8_t i = 0; i < fixed_n; ++i) {
                    active_packs[active_count++] = g_packs[i];
                }
            } else {
                for (uint8_t i = 0; i < MAX_PACKS; ++i) {
                    if (g_pack_online[i]) {
                        active_packs[active_count++] = g_packs[i];
                    }
                }
            }
            compute_dynamic_caps(active_count, dyn_lim, pol.charge_cap, pol.discharge_cap);
            aggregate_bank(active_packs, active_count, millis(), pol, g_bank);
            new_bank = g_bank;
            scale_changed = (active_count != prev_active_count);
            prev_active_count = active_count;
            xSemaphoreGive(g_snap_mutex);
        }
        if (scale_changed) {
            Serial.printf("[SCALE] active=%u cap_target=%.0fAh CCL_lim=%.1fA DCL_lim=%.1fA bank=%s (CCL=%.1fA DCL=%.1fA)\n",
                          static_cast<unsigned>(active_count),
                          static_cast<float>(active_count) * 100.0f,
                          pol.charge_cap, pol.discharge_cap,
                          new_bank.core.valid ? "YES" : "INHIBIT",
                          new_bank.core.ccl, new_bank.core.dcl);
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
                    }
#if BRIDGE_MODE == MODE_BRIDGE_ACTIVE
                    char resp[360];
                    size_t resp_len = handle_pylontech_request(rx_buf, rx_pos, snap, resp, sizeof(resp));
                    if (resp_len > 0) {
                        InvSerial.write(reinterpret_cast<const uint8_t*>(resp), resp_len);
                        InvSerial.flush();
                        g_inv_resp_count++;
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
            if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                snap = g_bank;
                for (uint8_t i = 0; i < MAX_PACKS; ++i) {
                    ps_snap[i] = g_packs[i];
                    online_snap[i] = g_pack_online[i];
                    miss_snap[i] = g_pack_miss[i];
                }
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
            } else {
                Serial.println("Perintah: status | packs | rescan | cells | celllog | raw | batraw | scan | help");
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
