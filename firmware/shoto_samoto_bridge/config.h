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
#define SHOTO_PROTOCOL   PROTO_AUTO
#define SHOTO_BAUD       9600

// 3. JUMLAH & ALAMAT BATERAI SHOTO:
//    - Mulai dari PACK_COUNT = 1 untuk uji 1 baterai terlebih dahulu.
//    - Ubah ke 2 setelah kedua Shoto di-cascade (Fase A02) dan terbaca unik (Fase A03).
#define PACK_COUNT       1
#define PACK1_ADDR       1    // SD-BA430 TBD_BINARY = 1 (untuk PROTO_AUTO memindai 1, 2, 38, 39, 61, 214 otomatis)
#define PACK2_ADDR       2    // Pack #2 default = 2 (atau 39 untuk Modbus V1.2)

// 4. SISI INVERTER SAMOTO IHF6200 / ILS6300:
//    - MODE_MONITOR_ONLY  = ESP32 HANYA membaca baterai & MENGINTIP request Samoto (tidak membalas)
//    - MODE_BRIDGE_ACTIVE = ESP32 aktif menjawab Samoto memakai protokol Pylontech LV RS485
#define MODE_MONITOR_ONLY  0
#define MODE_BRIDGE_ACTIVE 1
#define BRIDGE_MODE        MODE_MONITOR_ONLY
#define INVERTER_BAUD      9600

// 5. BATAS SAFETY KONSERVATIF (WAJIB SESUAI SPESIFIKASI BATERAI & INVERTER 48V):
//    - POLICY_USER_APPROVED = false -> saat mode aktif tetap kirim limit 0A (inhibit) demi keamanan bench.
//    - Ubah ke true HANYA setelah Fase A03-A05 lulus dan siap uji sinkronisasi ke Samoto (Fase A06).
#define POLICY_USER_APPROVED false
#define POLICY_CVL_MAX_V     54.4f   // Batas atas tegangan charge (Shoto 15S default float 53.2V / boost 56.4V -> cap aman 54.4V)
#define POLICY_DVL_MIN_V     46.0f   // Batas bawah tegangan discharge (cutoff aman 46.0V)
#define POLICY_CCL_CAP_A     50.0f   // Batas arus charge aman tahap commissioning (A)
#define POLICY_DCL_CAP_A     80.0f   // Batas arus discharge aman tahap commissioning (A)
#define POLICY_MAX_DELTA_V   1.0f    // Selisih tegangan maks antar 2 baterai paralel (V)
#define POLICY_FRESHNESS_MS  5000UL  // Jika baterai tidak merespons > 5 detik -> otomatis inhibit (0A)
#define POLL_INTERVAL_MS     1500UL  // Interval baca Shoto setiap 1.5 detik

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
