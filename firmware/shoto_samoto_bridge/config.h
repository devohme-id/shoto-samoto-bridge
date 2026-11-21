#pragma once
/**
 * KONFIGURASI UTAMA BRIDGE SHOTO <-> SAMOTO IHF6200 / ILS6300
 * ============================================================
 * Ubah parameter di bawah ini sesuai hasil Fase A03 / B03 sebelum klik Upload.
 */

// 1. PILIH HARDWARE BOARD (aktifkan SALAH SATU saja):
#ifndef BOARD_WAVESHARE_S3
  #define BOARD_DIY_XY_K485    // Opsi A: ESP32 DevKit WROOM-32 + 2x XY-K485 (atau 2x Waveshare TTL-RS485 C)
#endif
// #define BOARD_WAVESHARE_S3  // Opsi B: aktifkan baris ini di Arduino IDE (PlatformIO set otomatis via -DBOARD_WAVESHARE_S3)

// 2. PROTOKOL SHOTO (hasil `python3 tools/serial_lab.py autodetect`):
//    Pilihan:
//      PROTO_AUTO         = Deteksi otomatis saat boot (aman, hanya perintah READ)
//      PROTO_MODBUS_V12   = Resmi Shoto BMS Modbus V1.2 (reg 0x0FFF..0x1025, default ID 38/39)
//      PROTO_TBD_BINARY   = TianBangda Binary 0x7C/0x7E (hasil RE TianBangda .exe, default ID 1/2)
//      PROTO_TBD_1363     = TianBangda YD/T 1363.3 ASCII (~26004642..., default Pack 1/2)
//      PROTO_SMARTLI_DCDC = Shoto SmartLi DC/DC Modbus (reg 0x0000/0x1030, default ID 1/2)
//    Terverifikasi live pada SD-BA430-15S: PROTO_TBD_BINARY @ 9600, PACK1_ADDR 1.
//    Untuk baterai tipe lain, ubah ke PROTO_AUTO atau ketik "scan" di Serial Monitor.
#define SHOTO_PROTOCOL   PROTO_TBD_BINARY
#define SHOTO_BAUD       9600

// 3. MODE MULTI-BATERAI DINAMIS 24/7 (Common config untuk bench 1 pack, ILS3600 2 pack, IHF6200 3-4 pack):
//    - DYNAMIC_PACK_DISCOVERY = true  -> ESP32 otomatis mendeteksi baterai online (addr 1..4) saat boot
//      dan terus memantau hot-plug / hot-unplug di RS485, menyesuaikan kapasitas, CCL, DCL, serta 60H runtime
//      tanpa perlu upload ulang firmware.
//    - Untuk mengunci jumlah pack tetap (opsional), ubah ke false dan atur FIXED_PACK_COUNT (1..4).
#ifndef DYNAMIC_PACK_DISCOVERY
  #define DYNAMIC_PACK_DISCOVERY true
#endif
#ifndef FIXED_PACK_COUNT
  #define FIXED_PACK_COUNT       1
#endif
#define MAX_SUPPORTED_PACKS      4     // Slot alamat RS485 paralel maksimum (addr 1..4)
#define PACK_OFFLINE_MISSES      3     // Unplug / offline terdeteksi setelah 3x gagal berturut-turut
#define DISCOVERY_INTERVAL_MS    5000UL // Interval probe 1 slot offline (round-robin) untuk hot-plug

#define PACK1_ADDR       1    // SD-BA430 TBD_BINARY = 1 (untuk PROTO_AUTO memindai 1, 2, 38, 39, 61, 214 otomatis)
#define PACK2_ADDR       2    // Pack #2 default = 2 (atau 39 untuk Modbus V1.2)
#define PACK3_ADDR       3    // Pack #3 default = 3
#define PACK4_ADDR       4    // Cadangan Pack #4 = 4

// 4. SISI INVERTER SAMOTO ILS3600 / IHF6200:
//    - MODE_MONITOR_ONLY  = ESP32 HANYA membaca baterai & MENGINTIP request Samoto (tidak membalas)
//    - MODE_BRIDGE_ACTIVE = ESP32 aktif menjawab Samoto memakai protokol Pylontech LV RS485
#define MODE_MONITOR_ONLY  0
#define MODE_BRIDGE_ACTIVE 1
#define BRIDGE_MODE        MODE_BRIDGE_ACTIVE
#define INVERTER_BAUD      9600

// 5. BATAS SAFETY 24/7 AWET (Otomatis menskalakan CCL/DCL mengikuti jumlah baterai online):
//    0 pack -> INHIBIT (0A / 0A)
//    1 pack -> CCL 45A (0.45C), DCL 80A (0.80C)  [bench / uji meja]
//    2 pack -> CCL 60A (30A/pack), DCL 100A (50A/pack) [ILS3600 24/7]
//    3 pack -> CCL 90A (30A/pack), DCL 150A (50A/pack) [IHF6200 24/7]
//    4 pack -> CCL 120A (30A/pack), DCL 200A (50A/pack) [ekspansi 4 pack]
#define POLICY_USER_APPROVED   true
#define POLICY_CVL_MAX_V       53.6f   // 15S LFP sweet spot 24/7 (3.573 V/sel) -> awet & minim stress
#define POLICY_DVL_MIN_V       46.0f   // 3.067 V/sel -> batas cut-off discharge aman 15S LFP
#define POLICY_CCL_1PACK_A     45.0f   // 0.45C saat hanya 1x 100Ah online
#define POLICY_DCL_1PACK_A     80.0f   // 0.80C saat hanya 1x 100Ah online
#define POLICY_CCL_PER_PACK_A  30.0f   // 0.30C/pack saat >= 2 baterai paralel online -> dingin & awet
#define POLICY_DCL_PER_PACK_A  50.0f   // 0.50C/pack saat >= 2 baterai paralel online
#define POLICY_MAX_DELTA_V     1.0f    // Selisih tegangan maks antar baterai paralel (V)
#define POLICY_FRESHNESS_MS    6000UL  // Jika baterai online tidak merespons > 6 detik -> INHIBIT (0A)
#define POLL_INTERVAL_MS       1500UL  // Interval baca Shoto online setiap 1.5 detik
#define AUTO_LOG_CELLS         true    // Tampilkan rincian tegangan 15 sel + dV tiap 5 detik di Serial Monitor

// -------------------- Pemetaan Pin Hardware (otomatis) --------------------
#ifdef BOARD_WAVESHARE_S3
  #define BAT_UART_NUM       1
  #define BAT_TX_PIN         17
  #define BAT_RX_PIN         18
  #define BAT_DE_PIN         21   // Hardware direction control RS485 onboard Waveshare S3
  #define INV_UART_NUM       2
  #define INV_TX_PIN         1    // SH1.0 eksternal (auto-direction, tanpa DE/RE)
  #define INV_RX_PIN         2
  #define INV_DE_PIN         -1
#else
  // Default: ESP32 DevKit WROOM-32 + 2x XY-K485 (atau 2x Waveshare TTL-RS485 (C))
  #define BAT_UART_NUM       2
  #define BAT_TX_PIN         17
  #define BAT_RX_PIN         16
  #define BAT_DE_PIN         -1   // XY-K485 auto-direction, tidak pakai pin DIR
  #define INV_UART_NUM       1
  #define INV_TX_PIN         26
  #define INV_RX_PIN         27
  #define INV_DE_PIN         -1
#endif
