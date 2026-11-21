# Panduan Induk Bridge Shoto SmartLi ↔ Samoto IHF6200 / ILS6300 (v3.0)

> **Tujuan Utama Proyek:** Menghubungkan bank **2× Shoto SmartLi `SDA10-48100 / SDA10-48150 / SDA10-4850` (tanpa DIP switch)** ke inverter **Samoto `IHF6200 / ILS6300` (48 VDC)** melalui bridge ESP32 dual-UART agar seluruh **parameter BMS baterai** (`Tegangan Pack`, `Arus +/-`, `SOC %`, `SOH %`, `Suhu`, serta batas pengaman `CVL/DVL/CCL/DCL`) tersinkron dan terbaca akurat oleh inverter.  
> **Standar Kebenaran Dokumen:** Seluruh protokol & codec telah di-reverse-engineer langsung dari paket referensi [`reference/`](reference/) (`TianBangda LFP MS-ShotoV1.1.exe`, `Shoto BMS Modbus V1.2 PDF`, `BMS Protocol V2.8 DOC`) dan diverifikasi lewat unit test Python & C++17. Fakta pinout/protokol unit fisik Anda di lapangan **tetap wajib dibuktikan pada Fase A03 / B03** menggunakan **Autonics SCM-US48I** sebelum menutup rangkaian ke Samoto.

---

## 1. Pilih Jalur Panduan Anda (Opsi A vs Opsi B)

Ikuti **HANYA SATU jalur panduan** dari Phase `00` hingga `09` agar langkah perakitan, pinout, dan konfigurasi firmware tetap konsisten.

| Kriteria | **Opsi A — DIY `ESP32 WROOM-32` + `2× XY-K485`** *(BOM Anda Saat Ini)* | **Opsi B — Panel Permanen `Waveshare ESP32-S3-RS485-CAN`** |
|---|---|---|
| **Panduan Langkah-demi-Langkah** | **[GUIDE-A-DIY.md](GUIDE-A-DIY.md)** *(Phase `A00`–`A09`)* · [Versi Siap Print 3 Lembar A4](GUIDE-A-DIY-PRINT.pdf) | **[GUIDE-B-WAVESHARE.md](GUIDE-B-WAVESHARE.md)** *(Phase `B00`–`B09`)* |
| **Kesesuaian BOM Anda** | **100% cocok dengan 5 item BOM yang sudah Anda siapkan:** `ESP32 DevKit WROOM-32`, `2× XY-K485`, `Patch Cord Cat5e/6`, `Autonics SCM-US48I`, `Adaptor USB 5 V 2 A + kabel data`. | Memerlukan pembelian `Waveshare ESP32-S3-RS485-CAN (SKU 32154/34781)` + `1× Modul TTL-RS485 3.3 V terisolasi (SH1.0)`. |
| **Pin UART Bridge (`config.h`)** | `#define BOARD_DIY_XY_K485` (Default):<br>• `BAT`: `GPIO17 (TX)`, `GPIO16 (RX)`<br>• `INV`: `GPIO26 (TX)`, `GPIO27 (RX)` | `#define BOARD_WAVESHARE_S3`:<br>• `BAT` (onboard isolated): `GPIO17 (TX)`, `GPIO18 (RX)`, `GPIO21 (DE/RE)`<br>• `INV` (`SH1.0`): `GPIO1 (TX)`, `GPIO2 (RX)` |
| **Isolasi RS485** | `XY-K485` **non-isolated** → cocok untuk meja kerja dengan adaptor 5 V floating + uji GND `< 2 V` (`A05`). Untuk panel 24/7 disarankan upgrade drop-in ke `2× Waveshare TTL TO RS485 (C)` tanpa ubah pin/firmware. | **Terisolasi penuh dua sisi** (onboard RS485 terisolasi + modul eksternal terisolasi di `SH1.0`), mendukung PSU panel `7–36 V DC`. |
| **Target Compile Firmware** | Arduino `ESP32 Dev Module` / PlatformIO `diy_esp32_wroom32` | Arduino `ESP32S3 Dev Module` (`USB CDC On Boot: Enabled`) / PlatformIO `waveshare_esp32s3` |

---

## 2. Ikhtisar Rangkaian Utuh & Diagram Proyek

Sistem selalu terbagi menjadi **3 segmen yang terpisah secara elektrikal pada jalur datanya**:
1. **Bank Baterai Shoto 48 V (`#1` + `#2` + Terminator `120 Ω`):** Daya `+48 V / -48 V` dirangkai **PARALEL** ke Samoto lewat DC Breaker/Fuse. Data antar-pack memakai **Patch Cord Straight 8-Pin** (`#1 RS485A ↔ #2 RS485B`) dengan **Terminator `120 Ω` pada `Pin 7 ↔ 8`** di ujung `#2 RS485A`.
2. **Module Bridge ESP32 (Core 0: Baca Shoto · Core 1: Emulasi Pylontech ke Samoto):** Terhubung ke `Shoto #1 RS485B` (`Pin 1 Putih-Oranye` & `Pin 2 Oranye`) di sisi `BAT`, dan ke port `BMS / RS485` Samoto di sisi `INV`.
3. **Alat Diagnosa Terisolasi `Autonics SCM-US48I`:** Dipakai bergantian di **Fase `A03`/`B03`** untuk memindai Shoto (`autodetect`) dan menangkap pinout/frame asli Samoto (`capture`) sebelum kabel sisi `INV` dipasang.

| Diagram | Cakupan | File |
|---|---|---|
| **Rangkaian Utuh Opsi A** | Shoto #1/#2 + Terminator + 2× XY-K485 + ESP32 + 5V 2A + Samoto IHF6200/ILS6300 + Jalur 48V + SCM-US48I | [PNG](diagrams/diy-xy-k485.png) · [SVG](diagrams/diy-xy-k485.svg) |
| **Patch Cord Realistis: `XY-K485 #1 ↔ Shoto RS485B`** | Panduan visual kupas T568B (Pin 1 Putih-Oranye, Pin 2 Oranye) ke sekrup `XY-K485 #1` & Varian A/B | [PNG](diagrams/patchcord-xy-k485-shoto.png) · [SVG](diagrams/patchcord-xy-k485-shoto.svg) |
| **Patch Cord Realistis: `XY-K485 #2 ↔ Samoto BMS`** | Panduan visual kupas T568B (Kandidat #1: Pin 5 Putih-Biru `A+`, Pin 3 Putih-Hijau `B−`) ke sekrup `XY-K485 #2` | [PNG](diagrams/patchcord-xy-k485-samoto.png) · [SVG](diagrams/patchcord-xy-k485-samoto.svg) |
| **Wiring Dua Shoto & Terminator** | Langkah 1–4 Cascade + Varian Pinout RJ45 A vs B + Pembuatan & Uji Terminator `120 Ω` | [PNG](diagrams/shoto-cascade.png) · [SVG](diagrams/shoto-cascade.svg) |
| **Layout Opsi B (Waveshare S3)** | Waveshare ESP32-S3-RS485-CAN (`GPIO17/18/21`) + `SH1.0` (`GPIO1/2`) | [PNG](diagrams/waveshare.png) · [SVG](diagrams/waveshare.svg) |

---

## 3. Matriks Fakta Protokol Shoto (Hasil Deep Reverse Engineering)

Seluruh 4 protokol berikut **didukung otomatis** oleh `python3 tools/serial_lab.py autodetect` maupun firmware `PROTO_AUTO` pada [`firmware/shoto_samoto_bridge/bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp):

| ID Firmware (`config.h`) | Sumber Referensi Terbukti | Baud & Framing | Alamat / Query | Konvensi Arus Asli → Normalisasi Bridge (`+ = Charge, - = Discharge`) |
|---|---|---|---|---|
| `PROTO_MODBUS_V12` | `Shoto BMS Modbus Communication Protocol V1.2_EN-20190110.pdf` *(menyebut **`SDA10-48100`**)* | `9600 8N1` Modbus RTU | Addr `38+DIP` (`38,39,1,2`), FC `0x04` reg `0x0FFF..0x1025` *(golden TX: `27 04 0F FF 00 27 84 32`)* | **PDF Note 3:** reg `0x1000` negatif saat charge `(raw-10000)×0.1` → dibalik bridge menjadi `+(10000-raw)×0.1` |
| `PROTO_TBD_BINARY` | Disassembly biner `.NET` `TianBangda LFP MS-ShotoV1.1.634-26.exe` *(SHA-256 `75d3bdf7...`)* | `9600 8N1` Biner + `16×0xAA` wake | Addr `1..2`, TX `7C <addr> 01 00 <chk> 0D` (`chk = XOR^SUM`), RX `56 + 2×N_cells` byte (`86 B` 15S / `88 B` 16S) | `(30000 - raw) / 100.0` (dikonfirmasi dari log `data/*.xls`) |
| `PROTO_TBD_1363` | `BMS Communication Protocol_V2_8_180807.doc` & `Form1.getcrc16checkcode` | `9600 8N1` ASCII YD/T 1363.3 | `VER=26, ADR=00, CID1=46`, Pack 1 `~26004642E00201FD30\r` | `S16 / 100.0` A, Suhu `(raw - 2731) / 10.0` °C |
| `PROTO_SMARTLI_DCDC` | Referensi komunitas SmartLi DC/DC (`[S4]`) | `9600 / 19200 8N1` Modbus RTU | ID `1..4`, FC `0x04` reg `0x0000..0x0006` & `0x1030..0x103F` | Arus bus signed `0x1030` |

### Pinout RJ45 Shoto SmartLi / SDA10 (`T568B`) — Varian A vs Varian B

| Pin (`T568B`) | Warna Kabel | **Varian A — SmartLi V2.2 Tanpa DIP** *(Helios Table 6, p.11 `[S3]`)* | **Varian B — SDA10 TianBangda BMS** *(Manual SDA10)* |
|---|---|---|---|
| **Pin 1** | **Putih-Oranye** | **`RS485 A+`** *(utama)* | **`RS485 B-`** *(paralel dgn Pin 8)* |
| **Pin 2** | **Oranye** | **`RS485 B-`** *(utama)* | **`RS485 A+`** *(paralel dgn Pin 7)* |
| **Pin 3 / 4** | Putih-Hijau / Biru | Duplikat `RS485 A+` / `B-` | `GND` / `NC` |
| **Pin 5 / 6** | Putih-Biru / Hijau | `RS485 GND` / `NC` | `NC` / `GND` |
| **Pin 7 / 8** | Putih-Coklat / Coklat | **`CAN H` / `CAN L`** *(cascade & **Terminator 120 Ω**)* | `RS485 A+` / `RS485 B-` |

> Pada kedua varian, pasangan `Pin 1 (Putih-Oranye)` dan `Pin 2 (Oranye)` selalu membawa RS485 — tinggal uji polaritas via `Autonics SCM-US48I` di Fase `A03`/`B03`.

---

## 4. Peta File Tools & Firmware di Repository Ini

### 4.1 Tool Diagnosa Lapangan (`Autonics SCM-US48I`)
- **[`tools/serial_lab.py`](tools/serial_lab.py)**:
  - `python3 tools/serial_lab.py autodetect --port <PORT>` → memindai 4 protokol Shoto secara *read-only* di `9600` & `19200` baud.
  - `python3 tools/serial_lab.py probe --port <PORT> --baud <BAUD> --ids 38,39,1,2 --detail` → menguji pembacaan 1 vs 2 baterai.
  - `python3 tools/serial_lab.py capture --port <PORT> --baud 9600 --seconds 45 --out data/samoto_capture.log` → menangkap frame asli port BMS Samoto.
  - `python3 tools/serial_lab.py decode <FILE>` → men-decode file capture (YD/T 1363.3 ASCII, TianBangda binary, Modbus RTU).
- **[`tests/test_serial_lab.py`](tests/test_serial_lab.py)**: unit test Python dengan golden frames dari `.exe` dan PDF V1.2.

### 4.2 Firmware ESP32 Siap Flash (Dual-Core FreeRTOS Bridge)
Folder *self-contained* **[`firmware/shoto_samoto_bridge/`](firmware/shoto_samoto_bridge/)** siap dibuka di **Arduino IDE 2.x** maupun **PlatformIO**:
- **[`shoto_samoto_bridge.ino`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino)**: Task baca Shoto (`Core 0`), task respon emulasi Pylontech LV RS485 (`Core 1`, `0x61/0x62/0x63` + `0x42/0x44/0x47/0x4F/0x51/0x92`), serta Serial CLI `115200` (`status`, `cells`, `raw`, `scan`, `help`).
- **[`config.h`](firmware/shoto_samoto_bridge/config.h)**: Pemilihan board (`BOARD_DIY_XY_K485` / `BOARD_WAVESHARE_S3`), protokol (`PROTO_AUTO` default), `PACK_COUNT` (`1` default), mode keamanan (`BRIDGE_MODE = MODE_MONITOR_ONLY`, `POLICY_USER_APPROVED = false` default), dan batas `CVL/DVL/CCL/DCL/Vdelta/Freshness`.
- **[`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp)** & **[`safety_core.hpp`](firmware/shoto_samoto_bridge/safety_core.hpp)**: Codec 4 protokol Shoto, agregasi 1–2 baterai, fail-closed zero-limit inhibit (`5000 ms` stale / delta `> 1.0 V` → `CCL=0, DCL=0`), dan encoder Pylontech.
- **[`platformio.ini`](firmware/shoto_samoto_bridge/platformio.ini)**: Target `diy_esp32_wroom32` dan `waveshare_esp32s3`.
- **Unit test C++17 host:** [`tests/safety_test.cpp`](tests/safety_test.cpp) dan [`tests/bridge_test.cpp`](tests/bridge_test.cpp).

### 4.3 Template ESPHome (Khusus Monitoring Wi-Fi / Home Assistant Opsional)
- **Modbus V1.2 (`SDA10-48100`, addr `38`/`39`):** [`firmware/diy-reader-modbus-v12.yaml`](firmware/diy-reader-modbus-v12.yaml) & [`firmware/waveshare-reader-modbus-v12.yaml`](firmware/waveshare-reader-modbus-v12.yaml).
- **SmartLi DC/DC (`0x0000/0x1030`):** [`firmware/diy-reader.yaml`](firmware/diy-reader.yaml) & [`firmware/waveshare-reader.yaml`](firmware/waveshare-reader.yaml).
- **Secrets template:** [`firmware/secrets.example.yaml`](firmware/secrets.example.yaml).

---

## 5. Peta Jalan 10 Phase & Gate Keselamatan (Ringkasan)

1. **`00` — Identifikasi & BOM:** Pastikan bank 48 V paralel (bukan seri), siapkan BOM jalur A (`GUIDE-A-DIY.md`) atau jalur B (`GUIDE-B-WAVESHARE.md`).
2. **`01` — Rakit Meja Kerja:** Rakit MCU + transceiver RS485, uji `VCC/RXD <= 3.35 V` dengan multimeter (**tanpa kabel baterai & tanpa kabel Samoto**).
3. **`02` — Cascade Dua Shoto + Terminator:** Rangkai `#1 RS485A ↔ #2 RS485B` straight 8-pin + terminator `120 Ω` (`Pin 7 ↔ 8`) di `#2 RS485A` (ukur ohm saat lepas).
4. **`03` — Uji Faktual via `Autonics SCM-US48I`:** `autodetect` Shoto & `capture`/`decode` port BMS Samoto (`Menu 05 = PYL/LIB`). **Gate wajib sebelum kabel inverter dipasang!**
5. **`04` — Flash Firmware Siap Pakai (`MODE_MONITOR_ONLY`):** Upload `shoto_samoto_bridge.ino` dan verifikasi pembacaan live via Serial CLI `status` & `cells`.
6. **`05` — Uji Keselamatan Elektrikal & Kebijakan:** Untuk `XY-K485` non-isolated, ukur beda potensial GND `< 2 V DC / < 5 V AC`; verifikasi `15S/16S` & limit `CVL/DVL/CCL/DCL`.
7. **`06` — Aktivasi Sinkronisasi (`MODE_BRIDGE_ACTIVE` + `POLICY_USER_APPROVED = true`):** Sambung sisi `INV` setelah uji `05` lulus, cek `Error 61` hilang & LCD Samoto menyinkronkan `SOC`, `Vpack`, dan arus.
8. **`07` — Home Assistant (Opsional):** Jaga agar monitoring Wi-Fi tidak mengganggu kestabilan tugas bridge inverter.
9. **`08` — Commissioning `T01`–`T15`:** Verifikasi akurasi, arah arus `+`/`-`, uji cabut kabel fail-closed `< 5 dtk` (`CCL=0/DCL=0`), dan auto-recovery.
10. **`09` — Panel Permanen & Pemeliharaan:** Untuk Opsi A 24/7, upgrade `2× XY-K485` ke `2× Waveshare TTL TO RS485 (C)` (pin GPIO tetap sama) atau migrasi ke Opsi B.

---

## 6. Referensi Dokumen & Kode Sumber

[S1]: https://samoto.co.id/products/ihf-3000/
[S2]: https://samoto.co.id/products/ihf-6200/
[S3]: https://heliosps.com/wp-content/uploads/2024/12/Helios-Manual-Smart-lithium-battery-V2.2.pdf
[S4]: https://github.com/hpeyerl/esphome-shoto-smartli
[S5]: https://github.com/Fahmula/esphome-pylontech-rs485
[S6]: https://docs.waveshare.com/ESP32-S3-RS485-CAN
[S7]: https://files.waveshare.com/wiki/ESP32-S3-RS485-CAN/ESP32-S3-RS485-CAN-Schematic.pdf
[S8]: https://docs.waveshare.com/ESP32-S3-RS485-CAN/Resources-And-Documents
[S9]: https://github.com/G414ng/Techfine-Samoto-MPPT
[S10]: https://github.com/Qu33zZ/esphome-yambms
[S11]: https://github.com/Qu33zZ/esphome-yambms/blob/main/documents/README/Board_Waveshare_ESP32-S3-RS485-CAN.md
[S12]: https://github.com/ai-republic/bms-to-inverter
[S13]: https://esphome.io/components/uart/
[S14]: https://esphome.io/components/modbus_controller/

* Dokumen teknis utama: `[S1]` [Samoto IHF-3000][S1] · `[S2]` [Samoto IHF-6200][S2] · `[S3]` [Shoto Helios SmartLi V2.2 (Table 6)][S3] · `[S4]` [esphome-shoto-smartli][S4] · `[S5]` [esphome-pylontech-rs485][S5] · `[S6]` [Waveshare Docs][S6] · `[S7]` [Waveshare Schematic][S7] · `[S8]` [Waveshare Resources][S8] · `[S9]` [Techfine-Samoto-MPPT][S9] · `[S10]` [esphome-yambms][S10] · `[S11]` [YamBMS Waveshare Profile][S11] · `[S12]` [bms-to-inverter][S12] · `[S13]` [ESPHome UART][S13] · `[S14]` [ESPHome Modbus Controller][S14].
