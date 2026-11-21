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

static SemaphoreHandle_t g_snap_mutex = nullptr;
static PackState g_packs[2];
static BankExt g_bank;
static ShotoProtoKind g_active_proto = static_cast<ShotoProtoKind>(SHOTO_PROTOCOL);
static uint8_t g_pack_addrs[2] = {PACK1_ADDR, PACK2_ADDR};
static bool g_raw_log = false;
static uint32_t g_inv_req_count = 0;
static uint32_t g_inv_resp_count = 0;
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
            if (n > 0 && (millis() - last_byte) > 65) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
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
    ShotoProtoKind proto, uint8_t addr, const PolicyLimits& lim, PackState& out)
{
    uint8_t rx[256];
    if (proto == PROTO_MODBUS_V12) {
        uint8_t tx[8];
        build_shoto_modbus_v12_req(addr, tx);
        size_t n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), 350, true);
        return parse_shoto_modbus_v12_resp(rx, n, addr, millis(), lim, out);
    }
    if (proto == PROTO_TBD_BINARY) {
        // SD-BA430 / BMS PLUS V1.634 responds immediately WITHOUT 0xAA prefix (0xAA breaks its parser!).
        for (uint8_t hdr : {uint8_t(0x7C), uint8_t(0x7E)}) {
            uint8_t tx[6];
            build_tbd_binary_req(addr, hdr, 0x01, tx);
            size_t n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), 450, true);
            if (parse_tbd_binary_resp(rx, n, addr, millis(), lim, out)) return true;
        }
        // Fallback for older TianBangda V1.1 boards needing 16x 0xAA UART wakeup
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
            size_t n = uart_transact(BatSerial, tx, sizeof(tx), rx, sizeof(rx), 450, true);
            if (parse_tbd_binary_resp(rx, n, addr, millis(), lim, out)) return true;
        }
        return false;
    }
    if (proto == PROTO_TBD_1363) {
        char tx[64];
        size_t tx_len = build_tbd_1363_req(addr, 0x42, tx, sizeof(tx));
        size_t n = uart_transact(BatSerial, reinterpret_cast<const uint8_t*>(tx), tx_len, rx, sizeof(rx) - 1, 400, true);
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
        size_t n0 = uart_transact(BatSerial, tx0, 8, rx0, sizeof(rx0), 300, true);
        vTaskDelay(pdMS_TO_TICKS(50));
        build_smartli_dcdc_req(addr, 0x1030, 16, tx1);
        size_t n1 = uart_transact(BatSerial, tx1, 8, rx1, sizeof(rx1), 350, true);
        return parse_smartli_dcdc_block(rx0, n0, rx1, n1, addr, millis(), lim, out);
    }
    return false;
}

static bool run_autodetect(const PolicyLimits& lim) {
    Serial.println("[SCAN] Memulai deteksi otomatis 4 protokol Shoto (read-only)...");
    const uint8_t dcdc_addrs[] = {61, 62, 214, 215, 1, 2, 3, 4};
    const uint8_t mb_addrs[] = {38, 39, 40, 41, 61, 62, 214, 215, 1, 2, 3, 4};
    PackState tmp{};
    for (uint8_t a : {uint8_t(1), uint8_t(2)}) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_TBD_BINARY, a, lim, tmp)) {
            g_active_proto = PROTO_TBD_BINARY;
            g_pack_addrs[0] = a;
            g_pack_addrs[1] = static_cast<uint8_t>(a + 1);
            Serial.printf("[SCAN] COCOK! TBD_BINARY @ addr=%u (V=%.2fV SOC=%.1f%%)\n",
                          a, tmp.core.bus_v, tmp.core.soc);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    for (uint8_t a : mb_addrs) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_MODBUS_V12, a, lim, tmp)) {
            g_active_proto = PROTO_MODBUS_V12;
            g_pack_addrs[0] = a;
            g_pack_addrs[1] = static_cast<uint8_t>(a + 1);
            Serial.printf("[SCAN] COCOK! SHOTO_MODBUS_V12 @ addr=%u (V=%.2fV SOC=%.1f%%)\n",
                          a, tmp.core.bus_v, tmp.core.soc);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    for (uint8_t a : dcdc_addrs) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_SMARTLI_DCDC, a, lim, tmp)) {
            g_active_proto = PROTO_SMARTLI_DCDC;
            g_pack_addrs[0] = a;
            g_pack_addrs[1] = static_cast<uint8_t>(a + 1);
            Serial.printf("[SCAN] COCOK! SMARTLI_DCDC @ addr=%u (V=%.2fV SOC=%.1f%%)\n",
                          a, tmp.core.bus_v, tmp.core.soc);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    for (uint8_t a : {uint8_t(1), uint8_t(2)}) {
        esp_task_wdt_reset();
        if (poll_single_pack(PROTO_TBD_1363, a, lim, tmp)) {
            g_active_proto = PROTO_TBD_1363;
            g_pack_addrs[0] = a;
            g_pack_addrs[1] = static_cast<uint8_t>(a + 1);
            Serial.printf("[SCAN] COCOK! TBD_1363 @ group=%u (V=%.2fV SOC=%.1f%%)\n",
                          a, tmp.core.bus_v, tmp.core.soc);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    Serial.println("[SCAN] Belum ada balasan dari Shoto. Cek kabel RJ45 Pin 1/2 & polaritas A/B.");
    return false;
}

static void bat_poll_task(void* arg) {
    (void)arg;
    esp_task_wdt_add(nullptr);
    PolicyLimits lim;
    lim.cvl_v = POLICY_CVL_MAX_V;
    lim.dvl_v = POLICY_DVL_MIN_V;
    lim.ccl_a = POLICY_CCL_CAP_A;
    lim.dcl_a = POLICY_DCL_CAP_A;

    Policy pol;
    pol.approved = POLICY_USER_APPROVED;
    pol.freshness_ms = POLICY_FRESHNESS_MS;
    pol.max_delta_v = POLICY_MAX_DELTA_V;
    pol.charge_cap = POLICY_CCL_CAP_A;
    pol.discharge_cap = POLICY_DCL_CAP_A;

    uint32_t last_log = 0;

    for (;;) {
        esp_task_wdt_reset();
        if (g_active_proto == PROTO_AUTO) {
            if (!run_autodetect(lim)) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        for (uint8_t idx = 0; idx < PACK_COUNT; ++idx) {
            PackState fresh{};
            if (poll_single_pack(g_active_proto, g_pack_addrs[idx], lim, fresh)) {
                if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    g_packs[idx] = fresh;
                    xSemaphoreGive(g_snap_mutex);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        }

        if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            aggregate_bank(g_packs, PACK_COUNT, millis(), pol, g_bank);
            xSemaphoreGive(g_snap_mutex);
        }

        if (millis() - last_log >= 5000) {
            last_log = millis();
            BankExt snap{};
            if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                snap = g_bank;
                xSemaphoreGive(g_snap_mutex);
            }
            Serial.printf("[BAT %s] packs=%u valid=%s V=%.2fV I=%.2fA SOC=%.1f%% T=%.1fC CCL=%.1fA DCL=%.1fA | INV rx=%lu tx=%lu\n",
                          proto_name(g_active_proto), snap.pack_count,
                          snap.core.valid ? "YES" : "INHIBIT",
                          snap.bus_v, snap.core.bus_a, snap.core.soc, snap.avg_temp_c,
                          snap.core.ccl, snap.core.dcl,
                          static_cast<unsigned long>(g_inv_req_count),
                          static_cast<unsigned long>(g_inv_resp_count));
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

static void inv_bridge_task(void* arg) {
    (void)arg;
    esp_task_wdt_add(nullptr);
    char rx_buf[256];
    size_t rx_pos = 0;

    for (;;) {
        esp_task_wdt_reset();
        while (InvSerial.available()) {
            int b = InvSerial.read();
            if (b < 0) break;
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
            PackState p0{}, p1{};
            if (xSemaphoreTake(g_snap_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                snap = g_bank; p0 = g_packs[0]; p1 = g_packs[1];
                xSemaphoreGive(g_snap_mutex);
            }

            if (std::strcmp(cmd_buf, "status") == 0) {
                Serial.printf("Proto=%s | Bridge=%s | PolicyApproved=%s | BankValid=%s\n",
                              proto_name(g_active_proto),
                              BRIDGE_MODE == MODE_BRIDGE_ACTIVE ? "ACTIVE" : "MONITOR_ONLY",
                              POLICY_USER_APPROVED ? "YES" : "NO",
                              snap.core.valid ? "YES" : "INHIBIT");
                Serial.printf("Bank: V=%.2fV I=%.2fA SOC=%.1f%% SOH=%.1f%% CVL=%.2fV DVL=%.2fV CCL=%.1fA DCL=%.1fA\n",
                              snap.bus_v, snap.core.bus_a, snap.core.soc, snap.soh,
                              snap.core.cvl, snap.core.dvl, snap.core.ccl, snap.core.dcl);
                for (int i = 0; i < PACK_COUNT; ++i) {
                    const PackState& ps = (i == 0) ? p0 : p1;
                    Serial.printf("  Pack#%d (addr=%u): age=%lums V=%.2fV I=%.2fA SOC=%.1f%% prot=%#06x fault=%#04x\n",
                                  i + 1, ps.addr, static_cast<unsigned long>(millis() - ps.core.sampled_ms),
                                  ps.core.bus_v, ps.core.bus_a, ps.core.soc,
                                  ps.ext.prot_flags, ps.ext.fault_flags);
                }
            } else if (std::strcmp(cmd_buf, "cells") == 0) {
                for (int i = 0; i < PACK_COUNT; ++i) {
                    const PackState& ps = (i == 0) ? p0 : p1;
                    Serial.printf("Pack#%d (%u cells): ", i + 1, ps.ext.cell_count);
                    for (uint8_t k = 0; k < ps.ext.cell_count; ++k) {
                        Serial.printf("C%d=%u ", k + 1, ps.ext.cell_mv[k]);
                    }
                    Serial.println();
                }
            } else if (std::strcmp(cmd_buf, "raw") == 0) {
                g_raw_log = !g_raw_log;
                Serial.printf("Raw Inverter frame logging: %s\n", g_raw_log ? "ON" : "OFF");
            } else if (std::strcmp(cmd_buf, "scan") == 0) {
                g_active_proto = PROTO_AUTO;
                Serial.println("Memicu ulang auto-detect protokol Shoto...");
            } else {
                Serial.println("Perintah: status | cells | raw | scan | help");
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
    Serial.printf("Mode=%s | PolicyApproved=%s | Proto=%s | Packs=%d\n",
                  BRIDGE_MODE == MODE_BRIDGE_ACTIVE ? "ACTIVE" : "MONITOR_ONLY",
                  POLICY_USER_APPROVED ? "YES" : "NO",
                  proto_name(g_active_proto), PACK_COUNT);

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
