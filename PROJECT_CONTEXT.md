# PROJECT CONTEXT: Shoto SmartLi / SD-BA430 ↔ Samoto ILS3600 / IHF6200 BMS Bridge

> **Jenis Dokumen:** Konteks Global & *Single Source of Truth* Proyek (untuk pengembangan jangka panjang & sinkronisasi antar-sesi).  
> **Baseline Codebase:** Git Commit [`c06ed15`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino) (`main`) + Branch `feat/home-assistant-integration` (`v3.1.0`, baseline `4fae5e6` + update `PROJECT_CONTEXT.md`)  
> **Tanggal Pembaruan Konteks:** `2026-11-25`  
> **Status Proyek:** **3-Pack Cascade (`addr=1,2,3`, `300 Ah`) + Samoto `IHF6200` (`Menu 05 = PYL`, `PYL_FULL`) Terverifikasi Live (`49,78 V / 79,0% SOC`, `err=0`, `inhibit=0`)** · **Cloud Monitoring `solar.siseli.com` Terverifikasi 1:1** · **Integrasi Home Assistant + MQTT (`212` entitas) HOST-VERIFIED (C++17/11, 26 Python tests, 9 stub build `.ino`), PENDING LIVE DEPLOYMENT di Mini PC & ESP32** · **Rujukan: [`ISSUE_REPORT_2026-11-24.md`](ISSUE_REPORT_2026-11-24.md), [`GLOSSARY.md`](GLOSSARY.md), [`GUIDE-HOME-ASSISTANT.md`](GUIDE-HOME-ASSISTANT.md)**.

---

## Protokol Pemeliharaan Dokumen Ini (*Living Context Rules*)

**Aturan Wajib pada Setiap Sesi Pengembangan:**
1. **Baca sebelum bekerja:** Setiap kali memulai sesi baru atau melakukan perubahan codebase, jadikan dokumen ini rujukan utama agar tidak mengulang kesalahan desain, pinout, atau protokol yang sudah dibuktikan di lapangan.
2. **Update setelah setiap perubahan:** Setiap kali ada perubahan kode di `firmware/`, `tools/`, `tests/`, perubahan diagram/panduan, atau temuan uji fisik baru (mis. hasil 2-pack, IHF6200, Waveshare S3), **wajib memperbarui** bagian terkait di dokumen ini:
   - **Bagian 3** untuk fakta hardware & hasil pengukuran lapangan baru.
   - **Bagian 4** untuk perubahan konfigurasi, API, CLI, atau file baru.
   - **Bagian 5** untuk memindahkan item yang selesai dari *Backlog* atau menambah isu baru.
   - **Bagian 6** untuk menambah entri *Changelog* (commit & tanggal).
3. **Standar Fakta:** Bedakan dengan tegas antara **Terverifikasi Live di Hardware Fisik**, **Terverifikasi di Unit Test / Referensi RE**, dan **Belum Diuji Fisik**. Jangan pernah menuliskan asumsi sebagai fakta.

---

## 1. Konsep & Arsitektur Sistem

### 1.1 Gambaran Umum
Proyek ini adalah **jembatan protokol BMS dual-UART berbasis ESP32 FreeRTOS** (*hardware-in-the-loop BMS translator & safety gate*) yang menghubungkan baterai lithium **Shoto SmartLi / `01-SD-BA430` / `15SBA430` (15S LFP, 48 V / 100 Ah per pack, tanpa DIP switch)** ke inverter hybrid **Samoto `ILS3600` dan `IHF6200`** menggunakan protokol **Pylontech LV RS485 (`V3.5` + `V3.3`)**.

```text
┌─────────────────────────────┐      RS485 (9600 8N1)       ┌──────────────────────────────────┐      RS485 (9600 8N1)       ┌───────────────────────────────┐
│  Bank Baterai Shoto 1..4×   │ ◄════ Read-Only Poll ══════ │       ESP32 Dual-Core Bridge     │ ═════► Pylontech V3.5/V3.3 ════► │  Inverter Samoto              │
│  SD-BA430 / 15SBA430 (15S)  │ ───── Telemetry 140B ─────► │ Core 0: bat_poll_task + Safety   │ ◄════ Polling 60/61/62/63 ═════ │  ILS3600 / IHF6200            │
│  addr 1..4 (auto-addressing)│      (TBD_BINARY / dll)     │ Core 1: inv_bridge_task + CLI    │        (+ toleran 01 03..)      │  (Menu 24=USER, 25=BnS On)    │
└─────────────────────────────┘                             └──────────────────────────────────┘                               └───────────────────────────────┘
          ▲                                                                  │
          │ Cascade RJ45 straight 8-pin + Endloop 120 Ω (Pin 7↔8)            ▼ USB Serial 115200
          └──────────────────────────────────────────────────────── [Log Live + CLI + CSV Per-Sel]
```

### 1.2 Arah Data & Kontrol (1-Arah *Read-Only* ke BMS Baterai)
* **ESP32 → BMS Baterai Shoto (`READ-ONLY`):** ESP32 **tidak pernah melakukan write** ke EEPROM, alamat, atau register proteksi internal BMS Shoto. Pengubahan memori internal Shoto hanya dimungkinkan via PC menggunakan kabel USB-RS485 dan software resmi *TianBangda / BMS PLUS Diagnose UI* (password menu `CONFIG`: `888888`, lihat MOP hal. 15).
* **ESP32 ↔ Inverter Samoto (Tanya-Jawab Protokol Pylontech, Kontrol dari Bridge → Inverter):**
  * Protokol Pylontech RS485 **tidak memiliki perintah write dari inverter ke BMS**. Sebaliknya, bridge-lah yang mengirimkan plafon aman (`CVL`, `DVL`, `CCL`, `DCL` dan flag izinkan charge/discharge) melalui frame **`63H`** serta telemetri asli (`V`, `I`, `SOC`, `SOH`, `suhu`, `sel min/max`, `warning/protection`) melalui **`60H/61H/62H`**.
  * **Pengaturan via Menu Inverter:** Pengguna tetap bisa mengatur logika kerja baterai pada firmware Samoto (Menu `23 BRU`, `26 BSU`, `27 STG`, `28 STB`, `29 BSR`, `30 LOU`, prioritas SBU/SUB/UTL, serta batas arus charge inverter selama `<= CCL` bridge). Pengaturan ini disimpan di inverter, tidak di-write ke BMS Shoto.
* **Cara Menyelaraskan Tegangan & SOC Sebelum Paralel Multi-Pack:** Charge penuh setiap baterai **satu per satu secara terpisah** (mode 1 pack otomatis: `CVL 53.6 V`, `CCL 45 A`) hingga arus turun mendekati `0 A` (memicu kalibrasi SOC 100% dan *top-balancing* internal BMS), ukur & pastikan selisih tegangan antarbaterai **`< 0.5 V`** (ideal `< 0.2 V`), baru paralelkan kabel daya DC + busbar + cascade komunikasi.

### 1.3 Arsitektur FreeRTOS & Concurrency (`shoto_samoto_bridge.ino`)
* **Core 0 (`bat_poll_task`, stack `6144`, priority `2`):**
  * Mengelola deteksi protokol (`PROTO_AUTO` / `run_autodetect()`), pembacaan pack `1..4`, mekanisme hot-plug/hot-unplug, penskalaan dinamis (`compute_dynamic_caps()`), agregasi & gating safety (`aggregate_bank()`), audit EEPROM *read-only* per-pack (`run_shoto_param_audit()` 1 pack/siklus pada `params all`), serta pencatatan log periodik 5 detik (`[BAT ...]` & `[CELLS P# ...]`).
* **Core 0 (`ha_mqtt_task`, stack `8192`, priority `1`, aktif bila `#define ENABLE_HA_MQTT 1`):**
  * Tugas jaringan **non-blocking** terpisah untuk Wi-Fi (`WIFI_STA`, backoff `5s → 30s`), Eclipse Mosquitto MQTT (`PubSubClient`, buffer `1600 B`, LWT `online`/`offline` di `<MQTT_TOPIC_PREFIX>/status`, backoff `5s → 30s`), serta `ArduinoOTA` LAN-only.
  * Hanya menyalin snapshot `g_bank`, `g_packs[]`, `g_pack_eeprom[]`, dan counter diagnostik dari `g_snap_mutex` secara singkat tanpa pernah menahan mutex saat I/O jaringan atau penulisan NVS (`Preferences`).
* **Core 1 (`inv_bridge_task`, stack `6144`, priority `3`):**
  * Memantau `InvSerial` tiap 2 ms, menyaring frame Pylontech valid mulai dari `'~'` (`0x7E`) sampai `'\r'` (`0x0D`) sambil mengabaikan byte Modbus RTU campuran (`01 03 00 01 00 03 54 0B`), menyalin snapshot `g_bank` lewat `g_snap_mutex`, dan membalas lewat `handle_pylontech_request()`.
* **`loop()` (Core 1):** Menjalankan `handle_serial_cli()` non-blocking pada USB Serial `115200 baud` dengan delay `25 ms`.
* **Mutex & I/O Hygiene:** Seluruh salinan `g_packs[]`, `g_pack_online[]`, `g_pack_miss[]`, `g_pack_eeprom[]`, dan `g_bank` dilindungi `g_snap_mutex` (`SemaphoreHandle_t`), sementara `Serial.printf`, publish MQTT, dan akses NVS dijalankan *di luar* mutex agar task inverter tidak pernah tertahan oleh serial monitor maupun Wi-Fi/MQTT.
* **Watchdog & Portabilitas Compiler:** Hardware Task WDT (`10 detik`, `trigger_panic = true`) mendukung **ESP-IDF 4 (`-std=gnu++11`)** dan **ESP-IDF 5 (`-std=gnu++17`)** via `#if ESP_IDF_VERSION`, disertai `#undef min` / `#undef max` untuk mencegah bentrokan makro Arduino, dan `static_assert` waktu kompilasi untuk konsistensi timing hot-unplug.

### 1.4 Prinsip *Fail-Closed Safety* (`safety_core.hpp` & `bridge_protocols.hpp`)
Bank **wajib masuk kondisi aman (`INHIBIT`: `CCL = 0.0 A`, `DCL = 0.0 A`)** apabila salah satu kondisi berikut terjadi:
1. **Boot / Belum Ada Telemetri Valid (`!has_telemetry`):** `handle_pylontech_request()` **diam (`return 0`)** agar inverter tidak menganggap BMS hadir sebelum data nyata terbaca.
2. **Policy / Bridge Nonaktif:** `POLICY_USER_APPROVED == false` atau `BRIDGE_MODE == MODE_MONITOR_ONLY` (`!g_bridge_active`).
3. **0 Pack Online:** `aggregate_bank(..., count=0, ...)` menghasilkan `valid = false`, `ccl = 0.0f`, `dcl = 0.0f`, `!has_telemetry`.
4. **Data Kedaluwarsa (*Stale*):** Umur sampel salah satu pack aktif `> POLICY_FRESHNESS_MS` (`9000 ms`).
5. **Selisih Tegangan Antar-Pack Terlalu Lebar:** `max_v - min_v > POLICY_MAX_DELTA_V` (`1.0 V` default; dapat di-clamp runtime `0.5–1.5 V`) saat 2–4 baterai online → `63H` mengirim `CCL = 0 A`, `DCL = 0 A`, status inhibit `0x00`, dan `62H` menaikkan bit System Fault (`p2 |= 1<<3`).
6. **Proteksi Kritis / Integritas Frame Gagal:** Cell OV/UV, Pack OV/UV, OT/UT, jumlah tegangan sel berbeda `>= 3.0 V` terhadap tegangan pack (TLV Format B) atau `>= 5.0 V` (Format A), checksum salah, atau `CVL <= DVL`.

### 1.5 Arsitektur Integrasi Home Assistant, Keamanan Aksi & Reverse Proxy (`ha_mqtt_codec.hpp` + `homeassistant/`)
* **MQTT Auto-Discovery (`212` Entitas untuk `MAX_SUPPORTED_PACKS = 4`):**
  * Menghasilkan `64` entitas global (bank telemetry, diagnosa inverter/ESP32, tombol/switch/select/number aksi) + `4 × 37 = 148` entitas per-pack (`17` status/sel min-max-dV/EEPROM valid + atribut/tombol audit + `5` suhu + `15` tegangan sel individual) dengan isolasi topik multi-site lewat `HA_DEVICE_ID` & `MQTT_TOPIC_PREFIX`.
  * Seluruh payload JSON dibentuk lewat serializer statis `snprintf` (`ha_mqtt_codec.hpp`, batas `HA_MQTT_MAX_PAYLOAD = 1400 B`, tanpa alokasi heap `String`).
* **Kontrol Aksi 2-Langkah (*Maintenance Unlock* `300 s`) & Persistensi NVS:**
  * Aksi baca/diagnostik aman (`rescan`, `audit_eeprom`, `reset_diag`, `republish_discovery`, `cell_log`) dapat dijalankan kapan saja.
  * Aksi kritikal (`restart`, `bridge_mode`, `dyn_discovery`, `policy_cvl`, `policy_dvl`, `policy_ccl`, `policy_dcl`, `policy_max_dv`, `policy_reset`) **wajib ditolak di ESP32 (`HA_ACT_DENIED_LOCKED`)** jika `Maintenance Unlock` tidak aktif; switch ini otomatis mengunci kembali setelah `HA_MAINT_UNLOCK_TIMEOUT_MS` (`300000 ms` / 5 menit).
  * Perubahan kebijakan & mode disimpan persisten di NVS (`Preferences` namespace `"shoto_ha"`) dengan *hard clamp* 15S LFP (`CVL 51.0–54.4 V`, `DVL 44.0–48.0 V`, `CCL 10.0–90.0 A`, `DCL 10.0–120.0 A`, `MaxΔV 0.5–1.5 V`, sanitasi `NaN`/`Inf`).
* **Strict Read-Only ke EEPROM Shoto:** Pembacaan parameter proteksi (`cmd 0x06..0x50`, `13` register) dan seluruh `43` tag alarm (`cmd 0x43`, `0x00..0x2A`) melalui Home Assistant tetap **100% read-only** tanpa perintah tulis EEPROM dan tanpa `16× 0xAA`.
* **Keamanan Mini PC & Akses Jarak Jauh:**
  * Di Mini PC, Mosquitto (`1883`, autentikasi wajib `allow_anonymous false`) dan Home Assistant (`8123`, default loopback `127.0.0.1` pada Docker Compose + `recorder` 30 hari & `commit_interval: 15`) hanya untuk LAN/internal.
  * Akses internet/IP Publik hanya melalui **HTTPS Reverse Proxy port `443`** (Caddy / Nginx Proxy Manager) dengan WebSocket (`/api/websocket`), `use_x_forwarded_for: true`, `trusted_proxies`, `ip_ban_enabled: true` (`login_attempts_threshold: 5`), dan **2FA TOTP**; port `8123`, `1883`, `22`, dan OTA tidak pernah diekspos ke WAN.

---

## 2. Tujuan Proyek & Kriteria Keberhasilan

| Aspek | Target Desain | Status Saat Ini |
|---|---|---|
| **Common 1-Binary Firmware** | 1 firmware untuk Bench `1×`, `ILS3600` `2×`, `IHF6200` `3×`, dan ekspansi `4×` tanpa edit kode/upload ulang | **Tercapai di Firmware (`DYNAMIC_PACK_DISCOVERY = true`)** |
| **Dinamis Hot-Plug / Hot-Unplug** | Tambah/lepas pack terdeteksi otomatis & men-skala `Ah/CCL/DCL/60H` tanpa glitch `SOC=0%` atau fault sesaat | **Tercapai + Fixed via `94f4dbd`** *(perlu uji fisik 2-pack)* |
| **Bebas Fault Samoto** | Bebas `ALA 245` (BMS Comm Fault) & `ALA 235` (Low Voltage Fault) | **Terverifikasi Live di `ILS3600` & `IHF6200` (`Menu 05 = PYL`, 1-pack)** |
| **Operasi 24/7 Awet (15S LFP)** | `CVL 53.6 V` (3,573 V/sel), `DVL 46.0 V` (3,067 V/sel), `CCL 30 A/pack`, `DCL 50 A/pack` | **Aktif di `config.h`** |
| **Observabilitas & Audit** | Log 15 sel + `dV` per-pack di Serial Monitor & ekspor `.log` + `.csv` via PC | **Aktif (`cells`, `celllog`, `esp-log --csv`)** |
| **Instalasi Panel Jangka Panjang** | Transceiver RS485 terisolasi + commissioning 2/3-pack penuh di `ILS3600` & `IHF6200` | **Masuk Backlog (Tahap Lanjut)** |

---

## 3. Fakta Hardware & Temuan Lapangan Terverifikasi (*Ground Truth*)

> **Peringatan Penting:** Bagian ini berisi fakta yang terbukti langsung pada unit fisik milik user (`SD-BA430` + `Samoto ILS3600`) dan MOP resmi di `reference/`. Gunakan ini sebagai acuan utama bila ada konflik dengan dokumen panduan awal!

### 3.1 Baterai Fisik: Shoto SmartLi `SDA10-48100` / `01-SD-BA430` (`15SBA430`)
* **Spesifikasi Terbukti:** `15S LFP` (15 sel seri), kapasitas nominal **`100 Ah`** (`100.00 Ah`), tanpa saklar DIP fisik, kontrol ON/OFF/Sleep lewat tombol `RESET` (`3–5 detik` untuk menghidupkan, `5–10 detik` untuk mematikan — lihat `reference/instalasi-baterai-lithium-shoto-sda10-48100_compress.pdf` hal. 6).
* **Protokol Live Terverifikasi:** **`PROTO_TBD_BINARY` @ `9600 8N1`**, **`addr = 1`**.
  * **Temuan Kritis 1 (Tanpa `0xAA` Wakeup!):** Berbeda dari `TianBangda MS-ShotoV1.1.exe` legacy (Format A 86/88B), `SD-BA430` menggunakan **Format B: TLV Biner 140-byte** (`7E 01 01 86 01 0F ... <chk> 0D`, memakai *BMS PLUS Diagnose UI V1.634*). Mengirim `16× 0xAA` ke unit ini **justru merusak parsernya / memicu balasan corrupt** (`V=15.37V, SOC=2.0%`). Firmware mengunci `g_tbd_locked_no_wake = true` begitu Frame B terbaca dan memblokir fallback `0xAA` ketika `g_active_proto != PROTO_AUTO`.
  * **Temuan Kritis 2 (Arus Polaritas & Status Tag `0x06`):**
    * `Tag 0x02` menggunakan rumus `(30000 - raw) * 0.01f` (`+A = charging`, `-A = discharging`), diverifikasi live hingga `-11.59 A` dan `0.00 A`.
    * **`Tag 0x06` word 1 = `0x0002`** pada `SD-BA430` adalah indikator normal **discharging** (bukan bitmask Cell-UV milik Modbus V1.2!). Mapping langsung sebelumnya menyebabkan false alarm `62H = 10000000` yang memicu **`ALA 235` palsu**. Sekarang `warn_flags`/`prot_flags` pada TLV Format B diturunkan dari nilai terukur sel/pack/suhu fisik ([`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp#L429-L443)).
  * **Temuan Sekunder (`addr=214`):** Pada pemindaian Modbus `FC04` `addr=214`, `SD-BA430` juga merespons format `SHOTO_MODBUS_V12` dengan offset register tergeser (`ba430_shifted`, [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp#L231-L270)).
  * **Parameter EEPROM & Batas Internal Shoto (RE `TianBangda V1.1.634-26.exe` + `tbdset.tbd` + `Modbus V1.2`):**
    * Pembacaan proteksi (`Form1::baohucs` / `timer_readbhcs_Tick`, **100% read-only**) memetakan `cmd 0x06/0x07` (*Cell OV Prot/Rec*, referensi `3.75 V / 3.34 V`), `0x09/0x0A` (*Cell UV Prot/Rec*, `2.50 V / 2.90 V`), `0x2A/0x2B` (*Pack OV Prot/Rec*, `57.60 V / 54.60 V`), `0x2E/0x2F` (*Pack UV Prot / Cut-off & Recovery*, `43.20 V / 48.00 V`), `0x35` (*Constant Charge Voltage / 恒压值*, `56.00 V` di `tbdset.tbd`), dan `0x32/0x3E/0x50` (*Cell Sleep* `3.15 V`, *UV OFF Interval*, *Sleep Time*).
    * Pembacaan alarm (`Form1::gaojcs_read`, `cmd 0x43`) memuat tag `0x09/0x0B` (*Pack UV / Low Battery Alarm & Rec*, `46.00 V / 51.20 V`), `0x03/0x05` (*Cell UV Alarm*, `2.65 V / 2.90 V`), dan `0x21/0x22` (*Low Capacity Alarm*, `30% / 40%`). `Modbus V1.2` mencatat default `0x1024 Float Charge V = 53.2 V` & `0x1025 Boost Charge V = 56.4 V` (pada `SD-BA430` `addr=214` format `ba430_shifted` tidak mengekspos reg 37/38).
    * Parameter **`BMS Lock machine battery capacity`** (`ILS3600 Menu 26 BSU` / `IHF6200 BMS Low Power SOC`) dan **`paralel shutdown battery`** (`Parallel Mode Turn Off Voltage/SOC`) **bukan register EEPROM BMS Shoto**, melainkan milik sisi **inverter Samoto**.
    * Pembuktian live EEPROM unit fisik tersedia lewat perintah CLI ESP32 **`params`** (`run_shoto_param_audit()` tanpa `0xAA` & tanpa write) serta perekaman file PC `tools/serial_lab.py esp-log --params --out data/shoto_params.log`.

### 3.2 Pinout RJ45 Shoto `15SBA430` & Koreksi Kritis Cascade (`RS485A` vs `RS485B`)
Menurut **MOP Resmi Westindo (`reference/instalasi-baterai-lithium-shoto-sda10-48100_compress.pdf`, hal. 5, 7, 11)** dengan kabel standar **T568B**:

| Pin RJ45 (T568B) | Warna Kabel | Fungsi pada Shoto `15SBA430` (MOP Hal. 7) | Hubungan ke `XY-K485 #1` / Sistem |
|---|---|---|---|
| **Pin 1** | **Putih-Oranye** | **`RS485 A+`** | Hubungkan ke **`XY-K485 #1 A+`** |
| **Pin 2** | **Oranye** | **`RS485 B-`** | Hubungkan ke **`XY-K485 #1 B-`** |
| **Pin 3 / 4** | Putih-Hijau / Biru | Duplikat internal `RS485 A+` / `B-` | Tidak dipakai (isolasi) |
| **Pin 5** | Putih-Biru | `RS485 GND` | Tidak disambung untuk kabel pendek |
| **Pin 6** | Hijau | `NC` | Kosong |
| **Pin 7** | **Putih-Coklat** | **`CAN H`** | Jalur cascade antar-pack & **Endloop `120 Ω`** |
| **Pin 8** | **Coklat** | **`CAN L`** | Jalur cascade antar-pack & **Endloop `120 Ω`** |

> **KOREKSI KRITIS ARAH CASCADE (Temuan `2026-11-22`):**
> * **Dokumentasi lama yang SALAH (`diagrams/shoto-cascade.svg` & `GUIDE-A-DIY.md`):** sempat menuliskan reader ke `#1 RS485B`, cascade `#1 RS485A ↔ #2 RS485B`, dan terminator di `#2 RS485A`.
> * **Arah RESMI yang BENAR (MOP Hal. 5 & Hal. 11):**
>   ```text
>   [XY-K485 #1 BAT] ──► [Pack #1 RS485A]
>                        [Pack #1 RS485B] ──(Straight 8-pin penuh)──► [Pack #2 RS485A]
>                                                                     [Pack #2 RS485B] ──(jika 3 pack: ke Pack #3 RS485A)
>                                                                                      └─► [Pack Terakhir RS485B] ◄── [Plug Endloop 120 Ω (Pin 7↔8)]
>   ```
> * **Status Lapangan Cascade Pack #2 Saat Ini (Per `2026-11-22`):**
>   * Setelah port ditukar mengikuti MOP (`XY-K485 #1 → #1 RS485A`, `#1 RS485B → #2 RS485A`), **Pack #1 (`addr=1`) stabil ONLINE**, namun **Pack #2 (`addr=2`) tetap `[OFFLINE]`** karena pada saat pengujian fisik **Plug Endloop `120 Ω` belum terpasang** dan **kedua BMS belum di-reboot** setelah penyambungan kabel cascade.
>   * Gejala teramati di log `batraw`: kedua unit sama-sama masih beralamat `addr=1` sehingga terjadi *ping-pong bus collision* (SOC bergantian `67.0%` vs `67.3%`, RX banjir `256B` saat polling `addr=4`, dan `ALA 245` muncul-hilang).
>   * **Workaround Aman Sementara:** Cabut kabel RJ45 cascade antar-pack hingga Endloop `120 Ω` tersedia & kedua BMS di-reboot lewat tombol `RESET`. Kabel daya DC paralel 48 V tetap aman aktif karena tegangan kedua pack sudah seimbang (`~49.15 V`).

### 3.3 Inverter Samoto: `ILS3600` & `IHF6200` (Keduanya Terverifikasi Live 1-Pack)
* **Pinout Port BMS Samoto (`ILS3600` & `IHF6200`, Terbukti via `data/samoto_pin12.log` & `data/ihf6200_raw.log`):**
  * **Pin 2 (`Oranye`) → `XY-K485 #2 A(+)`**
  * **Pin 1 (`Putih-Oranye`) → `XY-K485 #2 B(-)`**
  * *(Kandidat awal Pin 5/3 pada `GUIDE-A-DIY.md` menghasilkan 0 byte pada `data/samoto_capture.log`.)*
* **Parameter Serial & Pola Polling (`9600 8N1`):**
  * **ILS3600:** Saat idle (`data/samoto_pin12.log`) bergantian mengirim **Pylontech V3.5 `60H`** (`~200246600000FDAC\r`, `~201246600000FDAB\r`) dan **Modbus RTU FC03** (`01 03 00 01 00 03 54 0B`); saat aktif ([`data/esp32_debug.log`](data/esp32_debug.log)) menjalankan loop **`62H` → `63H` → `61H`**.
  * **IHF6200 (`data/ihf6200_raw.log`):**
    * Saat **Menu `05 = LIA`** (sebelum dipindah ke `PYL`): mengulang scan `~200246610000FDAB`, `~201246610000FDAA`, `01 03 00 13 00 09 74 09`, `01 03 00 01 00 03 54 0B`; tidak pernah meminta `62H`/`63H`, dan di `solar.siseli.com` tampil `"Do Not Display BMS Details"`.
    * Saat **Menu `05 = PYL`**: langsung mengunci penuh protokol Pylontech V3.5 dengan loop **`63H` → `61H` → `62H`** (~200 ms interval, `err=0`, bebas fault).
* **Pengaturan Menu Inverter Samoto (Dari Foto Manual Produk Resmi User):**
  * **Seri `ILS3600`:** `BTT (24)` = `USER`, `BnS (25)` = `On`, `BPO (31)` = `0`, `BRU (23)` = `52.0 V`, `BSU (26)` = `10%` (disarankan `15%`), `STG (27)` = `20%`, `STB (28)` = `95%`, `BSR (29)` = `50%`, `LOU (30)` = `44.0 V`.
  * **Seri `IHF6200` (Hal. 20 Manual):** **Tidak ada menu terpisah `BnS` (`25`) dan `BPO` (`31`)** — ketiganya digabung pada **Menu `05` (`Tipe Baterai`)**: `AGn` (default), `FLd`, `USE`, `LIA`, **`PYL` (Aktif & Terbukti)**, `t9F`, `GrO`, `FEL`, `LIb`, `LIC`. Memilih `PYL` otomatis mengatur program `24, 26, 27, dan 29` mengikuti BMS bridge.
* **Monitoring Cloud `https://solar.siseli.com/` (`Solar Plug-RWB1-06` @ `IHF6200`, Terverifikasi Live `2026-11-22 21:58–22:01`):**
  * **Identitas & Mode Inverter Teramati:** `Device Type = HPVINV02`, `Software Version = 40.09`, `Battery Type = PYL`, `Mode = Battery Mode`, `Status Code = 00`, `Working Mode = SBU`, `Charging Priority Order = OSO`.
  * **Verifikasi 1:1 Data Live (`ESP32` `V=49.66V I=-4.65A SOC=73.3% T=36.0C CVL=53.60V DVL=46.00V CCL=45.0A DCL=80.0A` vs `solar.siseli.com` pasca upload Opsi A):**
    * **`Display Mode`:** berubah dari `"Do Not Display BMS Details"` (`Menu 05 = LIA`) menjadi `"Display Data For 16 Battery Cells"` (`Menu 05 = PYL`).
    * **Sel Min/Max & Posisi (`61H`):** `Max Voltage = 3323 mV` (`Max Voltage Cell Position = ID:1(1)`), `Min Voltage = 3304 mV` (`Min Voltage Cell Position = ID:1(6)`) pada capture `21:58` — mengonfirmasi format desimal **`pack_id * 100 + cell_num`** di [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp) menggantikan `ID:2(57)`/`ID:2(64)`.
    * **Telemetri & Polaritas Arus (`61H`):** `BMS Current SOC = 73 %` (`Battery Capacity = 74 %`), `BMS Charging Current = 0 A`, `BMS Discharge Current = 3.8 A` (selisih minor hanya berasal dari interval refresh dongle; tanda arah `-A` = discharge terbukti benar), `BMS Average Temperature = 35.95 °C` (vs ESP32 `36.0 °C`; selisih `0.05 °C` dari konversi offset `0.1 K` `2731` vs `2731.5`).
    * **Limits & Enable (`63H`):** `BMS Charge Voltage Limit = 53.6 V`, `BMS Discharge Voltage Limit = 46 V`, `BMS Charge Current Limit = 45 A`, `BMS Allow Charging Flag = Yes`, `BMS Allow Discharge Flag = Yes`.
    * **Komunikasi & Flag Proteksi (`62H`):** `BMS Communication Control Function = Open`, `BMS Communication Normal = Yes`, seluruh 6 flag BMS (`Charging Overcurrent`, `Discharge Overcurrent`, `Low Battery Alarm`, `Low Power Fault`, `Low Temperature`, `Temperature Too High`) = **`No`**, dan `Warning Light Status = Off` (0 false alarm).
  * **Kondisi & Fakta Field `BMS Status` yang Masih Bernilai `0` di `solar.siseli.com`:**
    * **Daftar Field Bernilai `0`:** Pada panel `BMS Status`, field `Remaining Capacity` (`0 A`), `Nominal Capacity` (`0 A`; satuan `"A"` adalah typo label UI Siseli untuk kapasitas Ah), dan seluruh **`Battery Voltage 1` s/d `Battery Voltage 16` (`0 mV`)** tetap bernilai `0` meskipun `Display Mode` sudah berubah ke `"Display Data For 16 Battery Cells"`.
    * **Akar Masalah Terbukti (`data/ihf6200_raw.log`):**
      1. Firmware ESP32 **sudah membaca ke-15 sel secara live** (tercetak di CLI `cells` / `[CELLS P1 ...]` dan `esp-log --csv`) serta **sudah mengimplementasikan responder Pylontech V3.3 `42H`** (yang memuat daftar tegangan tiap sel serta remaining/total capacity) di `bridge_protocols.hpp`.
      2. Namun pada setting **`Menu 05 = PYL`**, firmware inverter `IHF6200` (`HPVINV02` v`40.09`) **hanya melakukan polling Pylontech V3.5 `63H -> 61H -> 62H`** dan **tidak pernah mengirim request `42H`** (maupun `60H`).
      3. Frame V3.5 **`61H`** dalam spesifikasi Pylontech hanya membawa ringkasan bank (`V`, `I`, `SOC`, `T`, serta nilai & posisi sel Max/Min) dan **tidak memiliki payload array tegangan sel 1..16 maupun Remaining/Nominal Capacity (Ah)**. Akibatnya register internal kapasitas Ah dan tegangan per-sel 1..16 di inverter tetap `0` saat dibaca dongle `Solar Plug-RWB1-06` ke `solar.siseli.com`.
      4. **Dampak Operasional:** Kondisi ini **bukan bug codebase ESP32** dan **tidak memengaruhi kontrol, limit proteksi, maupun keselamatan baterai** karena seluruh kontrol/telemetri utama (`61H/62H/63H`) berjalan 100%. Data 15 sel individual dan log CSV tetap dapat dipantau penuh lewat ESP32 Serial CLI (`cells`, `celllog`, `serial_lab.py esp-log --csv`). Apabila ingin mengecek apakah protokol lain pada `IHF6200` mem-poll `42H`, opsi eksperimen opsional adalah menguji singkat `Menu 05 = LIb` atau `LIC`.
  * **Catatan Keamanan Parameter Fallback `IHF6200`:** Halaman parameter Siseli masih menampilkan setelan internal non-BMS/fallback (`Float Charging Voltage = 56.4 V`, `Strong Charging Voltage = 56.4 V`, `Battery Equalization Voltage = 56.3 V` dengan `Battery Equalization Mode = Disable`, `Maximum Total Charging Current = 120 A`). Selama `BMS Communication Normal = Yes`, inverter mengikuti `BMS Charge Voltage Limit = 53.6 V` dan `45 A`; namun **tetap wajib dicek 1× saat PV charging siang hari** bahwa `BMS Charging Current > 0 A`, arus dibatasi `<= 45 A`, dan tegangan berhenti di `<= 53.6 V` (tidak pernah naik ke `56.4 V` pada baterai 15S LFP).

### 3.4 Hardware Bridge & Alat Uji
* **Board Aktif (Opsi A — `#define BOARD_DIY_XY_K485`):**
  * MCU: `ESP32 DevKit WROOM-32` (`BAT UART2`: TX=`GPIO17`, RX=`GPIO16`; `INV UART1`: TX=`GPIO26`, RX=`GPIO27`, DE=-1 auto-direction).
  * Transceiver: `2× XY-K485` dicatu **pin `3V3`** ESP32 via pin-splitter/breadboard (modul **non-isolated**, aman untuk bench dengan adaptor `5 V 2 A` floating; untuk panel permanen 24/7 direkomendasikan upgrade ke `2× Waveshare TTL TO RS485 (C)` terisolasi).
  * **Catatan Boot-Loop SPI Flash (`startup.c:328`):** Bila Serial Monitor memunculkan `assert failed: do_core_init startup.c:328 (flash_ret == ESP_OK)`, pastikan **Tools → Board** Arduino IDE diset ke **`ESP32 Dev Module`** (bukan `ESP32-WROOM-DA Module`), Flash Mode `DIO`, `40MHz` (`Erase All Flash` 1x bila perlu), tidak ada koneksi yang menarik `GPIO12` HIGH saat boot, dan saat operasi berisiko noise EMI pisahkan dari USB laptop ke adaptor `5 V` floating.
* **Opsi B Siap Kode (`#define BOARD_WAVESHARE_S3`):** `Waveshare ESP32-S3-RS485-CAN` (`BAT UART1`: TX=`17`, RX=`18`, DE=`21`; `INV UART2 SH1.0`: TX=`1`, RX=`2`).
* **Alat Uji PC & macOS Notes:** `Autonics SCM-US48I` (2500 Vrms isolated RS485, chip FTDI). Pada mesin macOS user, port USB-A sempat tidak aktif hingga dilakukan *USB port mapping* via Hackintool/USBToolBox, setelah itu terdeteksi stabil sebagai `/dev/cu.usbserial-B002C8GU`.

---

## 4. Apa yang Sudah Dibangun (Inventaris Codebase — `main` `c06ed15` + Branch `feat/home-assistant-integration` `v3.1.0`)

### 4.1 Peta File Repository
| Path | Peran | Status |
|---|---|---|
| [`firmware/shoto_samoto_bridge/config.h`](firmware/shoto_samoto_bridge/config.h) | Konfigurasi board, protokol Shoto, dynamic discovery, policy safety 24/7, & `#define ENABLE_HA_MQTT` (default `0`) + topik/site HA | **Aktif & Updated (`v3.1.0`)** |
| [`firmware/shoto_samoto_bridge/safety_core.hpp`](firmware/shoto_samoto_bridge/safety_core.hpp) | Struktur `Pack`, `Policy`, `Bank`, fungsi `usable()` & `aggregate()` 2-pack fail-closed | **Aktif (C++11/17 portable)** |
| [`firmware/shoto_samoto_bridge/bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp) | 4 codec Shoto + `DynamicLimits`/`compute_dynamic_caps` + `aggregate_bank` (0..4 pack) + posisi sel `61H` + Pylontech V3.5/V3.3 + `TbdProtParams` (`13` reg), `TbdAlarmParams` (`43` tag `0x00..0x2A`), `PackEepromSnapshot` | **Aktif & Updated (`v3.1.0`)** |
| [`firmware/shoto_samoto_bridge/ha_mqtt_codec.hpp`](firmware/shoto_samoto_bridge/ha_mqtt_codec.hpp) | Codec C++17/11 portabel untuk Home Assistant: serializer JSON statis `snprintf` (`<= 1400 B`), generator `212` entitas MQTT Discovery (`MAX_PACKS=4`), clamp policy 15S LFP, & validator aksi `Maintenance Unlock` | **Host-Verified (`v3.1.0`)** |
| [`firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino) | Sketch ESP32 FreeRTOS dual-core (`v3.1.0`), runtime discovery, hot-plug/unplug, `params [1..4\|all]`, `ha_mqtt_task` Core 0 prio 1 non-blocking, persistensi NVS (`shoto_ha`), OTA LAN-only, & Serial CLI | **Aktif & 9 Stub Build Pass (`v3.1.0`)** |
| [`firmware/shoto_samoto_bridge/platformio.ini`](firmware/shoto_samoto_bridge/platformio.ini) | Build targets `diy_esp32_wroom32` dan `waveshare_esp32s3` (`-std=gnu++17 -Wall -Wextra`, `lib_deps = knolleary/PubSubClient @ ^2.8`) | **Aktif & Updated (`v3.1.0`)** |
| [`firmware/shoto_samoto_bridge/secrets.h.example`](firmware/shoto_samoto_bridge/secrets.h.example) | Template kredensial lokal Wi-Fi, Mosquitto MQTT, & `OTA_PASS` (disalin user menjadi `secrets.h` yang di-gitignore) | **Siap Provisioning User** |
| [`firmware/*.yaml`](firmware/diy-reader-modbus-v12.yaml) | Template ESPHome opsional Home Assistant (`diy-*`, `waveshare-*`, `secrets.example.yaml`) | Opsional (Hanya read Shoto) |
| [`homeassistant/docker-compose.yml`](homeassistant/docker-compose.yml) | Stack Mini PC Docker Compose (`homeassistant`, `mosquitto` `:1883`, `caddy` `:443/:80`) dengan bind HA default loopback `127.0.0.1:8123` | **Host-Verified (Pending Live Mini PC)** |
| [`homeassistant/configuration.yaml`](homeassistant/configuration.yaml) | Konfigurasi referensi HA (`http` `use_x_forwarded_for`, `trusted_proxies`, `ip_ban_enabled`, `recorder` 30d/`commit_interval: 15` SSD-safe, `!include`) | **Host-Verified (Pending Live Mini PC)** |
| [`homeassistant/mosquitto/mosquitto.conf`](homeassistant/mosquitto/mosquitto.conf) | Konfigurasi broker Mosquitto LAN-only `:1883` (`allow_anonymous false`, `password_file`, UID/GID `1883:1883`) | **Host-Verified (Pending Live Mini PC)** |
| [`homeassistant/Caddyfile.example`](homeassistant/Caddyfile.example) | Template reverse proxy TLS/HTTPS otomatis Let's Encrypt + WebSocket & security headers (disalin ke `Caddyfile`) | **Host-Verified (Pending Live Mini PC)** |
| [`homeassistant/.env.example`](homeassistant/.env.example) | Template environment variables Mini PC (`TZ`, `HA_DOMAIN`, `ACME_EMAIL`, `MINI_PC_LAN_IP`, `HA_BIND_IP`) | **Host-Verified (Pending Live Mini PC)** |
| [`homeassistant/dashboards/shoto_bms_dashboard.yaml`](homeassistant/dashboards/shoto_bms_dashboard.yaml) | Dashboard Lovelace 4-view (*Overview & Power Flow*, *Packs & 15-Cell Matrix*, *Shoto EEPROM Audit*, *Action Center & Diagnostics* dengan gate `Maintenance Unlock`) | **Host-Verified (Exact Entity Match)** |
| [`homeassistant/automations_shoto_bms.yaml`](homeassistant/automations_shoto_bms.yaml) | Otomasi notifikasi keselamatan Shoto BMS berbahasa Indonesia (`INHIBIT`, `ΔV`, suhu, proteksi/fault, offline, auto-relock timer) | **Host-Verified (14 Artifact Tests)** |
| [`homeassistant/scripts_shoto_bms.yaml`](homeassistant/scripts_shoto_bms.yaml) | Script utilitas HA (tes notifikasi bank & audit EEPROM *read-only* 1..4 pack dengan ringkasan notifikasi) | **Host-Verified (14 Artifact Tests)** |
| [`GUIDE-HOME-ASSISTANT.md`](GUIDE-HOME-ASSISTANT.md) | Panduan step-by-step instalasi Mini PC (HAOS/Docker), Mosquitto, flash ESP32 HA, dashboard 4-view, & IP Publik HTTPS `:443` + 2FA | Dokumentasi Panduan |
| [`docs/superpowers/plans/2026-11-25-home-assistant-integration.md`](docs/superpowers/plans/2026-11-25-home-assistant-integration.md) | Rencana implementasi & matriks verifikasi integrasi Home Assistant Task 1–7 | Dokumentasi Rencana |
| [`tools/serial_lab.py`](tools/serial_lab.py) | CLI diagnosa PC (`probe`, `autodetect`, `capture`, `decode`, `esp-log --csv`, `esp-log --params`) | **Aktif & Updated (`fcf803c` + `--params`)** |
| [`tests/safety_test.cpp`](tests/safety_test.cpp) | Host unit test 7 skenario inti `safety_core.hpp` | **Pass (C++11 & C++17)** |
| [`tests/bridge_test.cpp`](tests/bridge_test.cpp) | Host unit test 11 suite (4 codec Shoto, V3.5/V3.3, live SD-BA430 140B, 3-pack IHF6200, dynamic 0..4 pack, posisi sel `61H`, `TbdAlarmParams` 43-tag `cmd 0x43` & `PackEepromSnapshot`) | **Pass (11/11, C++11 & C++17)** |
| [`tests/ha_mqtt_test.cpp`](tests/ha_mqtt_test.cpp) | Host unit test 4 suite codec HA MQTT (policy clamp + `NaN`, validator `Maintenance Unlock`, JSON `<= 1400 B`, keunikan & isolasi multi-site `212` entitas) | **Pass (4/4, C++11 & C++17)** |
| [`tests/test_serial_lab.py`](tests/test_serial_lab.py) | 12 Python unit test golden `.exe`, PDF V1.2, DOC V2.8, capture live `ILS3600`, & `esp-log --params` | **Pass (12/12 OK)** |
| [`tests/test_ha_artifacts.py`](tests/test_ha_artifacts.py) | 14 Python unit test hardening YAML/Docker/Mosquitto/Caddy, struktur dashboard 4-view, pencocokan persis entitas terhadap `ha_mqtt_codec.hpp`, & automations/scripts | **Pass (14/14 OK)** |
| [`.gitignore`](.gitignore) | Mengabaikan file rahasia/lokal (`secrets.h`, `homeassistant/.env`, `homeassistant/Caddyfile`, `ha_config/`, `mosquitto/passwd`, `/tmp/`, `.pio/`) | **Aktif** |
| [`data/samoto_pin12.log`](data/samoto_pin12.log) | Capture JSONL asli port BMS Samoto `ILS3600` (Pin 1/2 @ `9600 8N1`, 70 baris) | Bukti Lapangan |
| [`data/esp32_debug.log`](data/esp32_debug.log) | Capture Serial Monitor ESP32 live handshake `ILS3600` `62H/63H/61H` (184 baris) | Bukti Lapangan |
| [`data/ihf6200_raw.log`](data/ihf6200_raw.log) | Capture Serial Monitor ESP32 live `IHF6200` (`Menu 05 = PYL`, loop `63H/61H/62H`, 177 baris) | Bukti Lapangan |
| [`data/shoto_params.log`](data/shoto_params.log) | Hasil audit *read-only* EEPROM parameter Shoto `TBD_BINARY` (`cmd 0x06..0x50` & `0x43`) | Bukti Lapangan |
| [`ISSUE_REPORT_2026-11-24.md`](ISSUE_REPORT_2026-11-24.md) | Laporan lengkap diagnosa, perbaikan firmware, dan verifikasi live 3-pack issue `2026-11-24` | Dokumentasi Laporan |
| [`GLOSSARY.md`](GLOSSARY.md) | Glosarium lengkap kode protokol (`60H..63H`, `42H..92H`, `TBD`), singkatan parameter BMS/inverter, & CLI | Dokumentasi Rujukan |
| [`diagrams/*`](diagrams/shoto-cascade.svg) & [`GUIDE*.md`](GUIDE-A-DIY.md) | Panduan A/B, versi print `GUIDE-A-DIY-PRINT.pdf`, dan diagram wiring PNG/SVG | **Diagram Aktual; Teks `GUIDE*.md` Perlu Sinkronisasi** |
| [`reference/`](reference/instalasi-baterai-lithium-shoto-sda10-48100_compress.pdf) | Binary RE `.exe`, PDF `Modbus V1.2`, DOC `V2.8`, log `*.xls`, MOP PDF Westindo `SDA10-48100` | Arsip Rujukan Resmi |

### 4.2 Parameter Aktif `config.h` & Tabel Skala Otomatis (`compute_dynamic_caps`)
Konfigurasi aktif di [`firmware/shoto_samoto_bridge/config.h`](firmware/shoto_samoto_bridge/config.h#L23-L70):
* `SHOTO_PROTOCOL = PROTO_TBD_BINARY`, `SHOTO_BAUD = 9600`
* `BRIDGE_MODE = MODE_BRIDGE_ACTIVE`, `INVERTER_BAUD = 9600`, `POLICY_USER_APPROVED = true`
* `DYNAMIC_PACK_DISCOVERY = true` (dapat di-override `-DDYNAMIC_PACK_DISCOVERY=false -DFIXED_PACK_COUNT=N`), `MAX_SUPPORTED_PACKS = 4`, `PACK1_ADDR..PACK4_ADDR = 1..4`
* `PACK_OFFLINE_MISSES = 2` (2x poll = 4 frame `0x7C`/`0x7E` gagal → pindah offline), `DISCOVERY_INTERVAL_MS = 5000UL`, timeout probe slot offline = `320 ms`, timeout poll normal `PROTO_TBD_BINARY` = `500 ms`
* `POLL_INTERVAL_MS = 1500UL`, `POLICY_FRESHNESS_MS = 9000UL` (dijaga oleh compile-time `static_assert`), `POLICY_MAX_DELTA_V = 1.0f`, `POLICY_CCL_BANK_MAX_A = 90.0f`, `POLICY_DCL_BANK_MAX_A = 120.0f`, `AUTO_LOG_CELLS = true`

| Pack Online (`active_count`) | Kapasitas Bank | `CVL` / `DVL` | `CCL` | `DCL` | Barcode `60H` | Target Kasus Pakai |
|---:|---:|---:|---:|---:|---:|---|
| `0` pack | `0 Ah` | – | **`0.0 A`** | **`0.0 A`** | Silent (`return 0`) | Boot / semua BMS mati / putus bus |
| `1` pack | `100 Ah` | `53.6 V` / `46.0 V` | **`45.0 A`** (0.45C) | **`80.0 A`** (0.80C) | `1` barcode | Bench test / single-pack darurat |
| `2` pack | `200 Ah` | `53.6 V` / `46.0 V` | **`60.0 A`** (30A/pack) | **`100.0 A`** (50A/pack) | `2` barcode | **Samoto ILS3600 (24/7 awet)** |
| `3` pack | `300 Ah` | `53.6 V` / `46.0 V` | **`90.0 A`** (30A/pack) | **`120.0 A`** *(capped)* | `3` barcode | **Samoto IHF6200 (24/7 awet)** |
| `4` pack | `400 Ah` | `53.6 V` / `46.0 V` | **`90.0 A`** *(capped)* | **`120.0 A`** *(capped)* | `4` barcode | Ekspansi maksimal 4 pack |

### 4.3 Daftar Protokol & Perintah CLI
* **Protokol Baterai (Auto & Eksplisit):** `PROTO_MODBUS_V12` (`0x0FFF..0x1025` + `ba430_shifted`), `PROTO_TBD_BINARY` (Format A 86/88B + Format B TLV 140B `7E..0D`), `PROTO_TBD_1363` (`~26004642...`), `PROTO_SMARTLI_DCDC` (`0x0000` & `0x1030`).
* **Protokol Inverter (Pylontech RS485):** **V3.5** `60H` (nama `"Force_L"`, manufaktur `"Pylon"`, `1..4` barcode `B48100B210TE7001..4`), `61H` (49B system analog, posisi sel Max/Min & suhu dikodekan desimal `pack_id * 100 + cell_num` agar Samoto/Siseli menampilkan `ID:<pack>(<cell>)` mis. `ID:1(1..15)`), `62H` (alarm/protection), `63H` (9B management limits `CVL/DVL/CCL/DCL` + enable byte `0xC0`/`0x80`/`0x40`/`0x00`); **V3.3** `42H`, `44H`, `47H`, `4FH`, `51H`, `92H`. Memantulkan `VER` dan `ADR` inverter secara adaptif.
* **Serial CLI ESP32 (`115200 baud`, Newline):**
  * `status` — ringkasan protokol, mode, validitas bank, metrik bank, baris `Diag:` (`inhibit`, `dvRetry`, `peakDV`, counter `60H..63H`), & status slot `#1..#4` (`ONLINE`/`OFFLINE`, `age`, `prot`, `fault`).
  * `diag` — diagnosa mendalam penyebab `INHIBIT`, counter `dvRetry`, deteksi mode polling inverter (`PYL_FULL` vs `SCAN_61H_ONLY`), serta frame TX terakhir `63H/62H/61H`.
  * `packs` — status cepat `DynDiscovery`, jumlah pack aktif, kapasitas, limit `CCL/DCL`, `miss` counter, & tegangan per slot `#1..#4`.
  * `rescan` — memicu pemindaian ulang instan slot `addr 1..4`.
  * `cells` — mencetak detail 15 sel (mV) + `min/max/dV` tiap pack online.
  * `celllog` — toggle log `[CELLS P# ...]` otomatis tiap 5 detik (default `ON`).
  * `raw` / `batraw` — toggle log hex mentah UART Inverter / UART Baterai RS485.
  * `scan` — memicu ulang auto-detect ke-4 protokol (`PROTO_AUTO`).
  * `params [1|2|3|4|all]` — audit parameter EEPROM read-only Shoto (`TBD cmd 0x06..0x50`, `cmd 0x43`, `Modbus V1.2`, `YD/T 1363 47H`) + ringkasan 7 parameter `[PARAM MAP ...]`; mode `all` mengaudit 1 pack per siklus Core 0 agar pack lain tetap `< 9000 ms`.
* **Konfigurasi Integrasi HA ([`config.h`](firmware/shoto_samoto_bridge/config.h#L74-L97)):** `ENABLE_HA_MQTT` (default `0` offline, set `1` untuk HA), `HA_DEVICE_ID = "shoto_ils6300"`, `HA_SITE_NAME = "PLTS Samoto ILS6300"`, `MQTT_TOPIC_PREFIX = "shoto_bridge/ils6300"`, `HA_TELEMETRY_INTERVAL_MS = 3000UL`, `HA_CELLS_INTERVAL_MS = 5000UL`, `HA_MAINT_UNLOCK_TIMEOUT_MS = 300000UL`.
* **CLI PC [`tools/serial_lab.py`](tools/serial_lab.py):** `autodetect`, `probe`, `capture`, `decode`, `esp-log --port <PORT> --seconds <S> --out data/esp32_debug.log --csv data/cells.csv` (tambahkan `--params` untuk capture audit `params` hingga `[PARAM DONE]`).

---

## 5. Apa yang Akan Dibangun (Roadmap, Backlog & *Known Gaps*)

### Prioritas 1 — Validasi Live 2-Pack Cascade (`Pack #1` + `Pack #2`)
* [ ] Pasang **Plug Endloop `120 Ω` (`Pin 7 Putih-Coklat ↔ Pin 8 Coklat`, Pin 1–6 kosong)** pada **`Shoto #2 RS485B`**.
* [ ] Sambungkan `XY-K485 #1 → #1 RS485A`, cascade straight 8-pin `#1 RS485B ↔ #2 RS485A`, dan Endloop di `#2 RS485B`.
* [ ] Lakukan **reboot bersama kedua BMS** (tahan tombol `RESET` `5–10 detik` OFF → `3–5 detik` ON), lalu verifikasi di Serial CLI (`rescan`, `packs`, `cells`, `batraw`) bahwa `Slot#1 (addr=1)` dan `Slot#2 (addr=2)` keduanya `ONLINE`, `Cap=200Ah`, `CCL=60A`, `DCL=100A`, `err` tidak bertambah, dan nol `ALA 245`.
* [ ] *Fallback jika Pack #2 tetap di `addr=1` pasca Endloop + reboot:* colok `Autonics SCM-US48I` hanya ke Shoto #2 untuk memeriksa alamat internal / firmware lewat software `BMS PLUS Diagnose UI` (`reference/`).

### Prioritas 2 — Sinkronisasi Dokumentasi & Diagram Terhadap Fakta Lapangan
Dokumen panduan berikut dibuat sebelum uji fisik `SD-BA430` + `ILS3600` dan sebagian besar **sudah diselaraskan**:
* [x] Perbaiki [`diagrams/shoto-cascade.svg`](diagrams/shoto-cascade.svg) & [`shoto-cascade.png`](diagrams/shoto-cascade.png), [`diagrams/diy-xy-k485.*`](diagrams/diy-xy-k485.svg), serta [`diagrams/patchcord-xy-k485-shoto.*`](diagrams/patchcord-xy-k485-shoto.svg): arah resmi MOP Westindo (`XY-K485 #1 → #1 RS485A`, Cascade `#1 RS485B ↔ #2 RS485A`, Endloop `120 Ω` di ujung `RS485B` + reboot `RESET`).
* [x] Perbarui [`diagrams/patchcord-xy-k485-samoto.*`](diagrams/patchcord-xy-k485-samoto.svg) & pinout Samoto pada `diy-xy-k485.*` dari kandidat `Pin 5/3` menjadi pinout terbukti live **`Pin 2 (Oranye) = A(+)` dan `Pin 1 (Putih-Oranye) = B(-)`**, serta menu `IHF6200` (`Menu 05 = PYL`) dan `ILS3600` (`24 BTT=USER`, `25 BnS=On`, `31 BPO=0`).
* [ ] Perbarui teks pinout, firmware, & menu di [`GUIDE.md`](GUIDE.md), [`GUIDE-A-DIY.md`](GUIDE-A-DIY.md), [`GUIDE-B-WAVESHARE.md`](GUIDE-B-WAVESHARE.md) serta generate ulang [`GUIDE-A-DIY-PRINT.pdf`](GUIDE-A-DIY-PRINT.pdf) (`Pin 2/1` Samoto, arah MOP `RS485A/B`, `PROTO_TBD_BINARY`, `DYNAMIC_PACK_DISCOVERY = true`, `CCL 90A / DCL 120A`, serta perintah CLI `packs`, `rescan`, `celllog`, `batraw`, `params`, `diag`).

### Prioritas 3 — `Samoto IHF6200`, Dashboard Siseli & 3-Pack Cascade
* [x] Uji fisik 1-pack pada `Samoto IHF6200` dengan `Menu 05 = PYL` (pinout BMS `Pin 2/1` identik `ILS3600`; manual `IHF6200` hal. 20 tidak memiliki menu `BnS`/`BPO` terpisah): polling V3.5 `63H -> 61H -> 62H` terkunci (`err=0`), Siseli `Display Mode` berubah ke `"Display Data For 16 Battery Cells"`.
* [x] Upload firmware revisi Opsi A ke ESP32 dan verifikasi live 1:1 di `https://solar.siseli.com/` (`Max/Min Voltage Cell Position` menampilkan `ID:1(1)` / `ID:1(6)`, serta `SOC`, `Discharge Current`, `Average Temperature`, `CVL 53.6V`, `DVL 46V`, `CCL 45A`, flag allow & 6 alarm `62H` 100% sinkron dengan Serial ESP32).
* [ ] Verifikasi 1× saat **PV charging siang hari** di `IHF6200` + `solar.siseli.com`: pastikan `BMS Charging Current > 0 A`, arus charge tertahan `<= 45 A`, dan tegangan baterai berhenti di `<= 53.6 V` (tidak mengikuti parameter fallback `56.4 V`).
* [ ] *(Opsional)* Eksperimen singkat `Menu 05 = LIb` / `LIC` pada `IHF6200` + rekam `serial_lab.py esp-log` jika ingin menguji apakah inverter polling `42H` (agar `Remaining/Nominal Capacity` & `Battery Voltage 1..16` di Siseli tidak bernilai `0`).
* [x] Uji cascade **3 pack Shoto** (`#1 RS485A ← Bridge`, `#1 RS485B ↔ #2 RS485A`, `#2 RS485B ↔ #3 RS485A`, `#3 RS485B ← Endloop 120 Ω`) → terverifikasi live `P1..P3` (`Cap=299Ah`, `CCL=90A`, `DCL=120A`), `PYL_FULL` bertahan stabil di `Battery Mode` pada `49,78 V / 79,0% SOC` (`err=0`, `inhibit=0`, rincian di [`ISSUE_REPORT_2026-11-24.md`](ISSUE_REPORT_2026-11-24.md)). *(Uji lonjakan beban puncak tinggi jangka panjang tetap dipantau.)*

### Prioritas 4 — Hardening Produksi 24/7 & Ekosistem
* [ ] Upgrade modul RS485 dari `2× XY-K485` *non-isolated* ke **`2× Waveshare TTL TO RS485 (C)`** (terisolasi 2500 Vrms, pin GPIO tetap sama) atau migrasi ke **Opsi B** (`Waveshare ESP32-S3-RS485-CAN`) untuk ketahanan noise EMI jangka panjang terutama pada `IHF6200`.
* [ ] Jalankan seluruh 15 skenario commissioning `T01–T15` + uji cabut-pasang (*hot-unplug / hot-plug*) baterai multi-pack pada kondisi beban aktif.
* [ ] **Live Deployment Home Assistant + Mini PC + IP Publik (Steps 2–4 [`2026-11-25-home-assistant-integration.md`](docs/superpowers/plans/2026-11-25-home-assistant-integration.md)):** Kode firmware, `212` entitas MQTT Discovery, stack Mini PC (`homeassistant/`), dashboard 4-view, & otomasi **sudah selesai dan HOST-VERIFIED**; tinggal eksekusi lapangan:
  * Provisioning Mini PC (HAOS atau Docker Compose + Mosquitto `:1883` auth + Caddy/NPM HTTPS `:443` + 2FA TOTP).
  * Buat `secrets.h` dari `secrets.h.example` dan flash ESP32 dengan `ENABLE_HA_MQTT 1` pada hardware live (`ILS6300`/`IHF6200` + Shoto).
  * Verifikasi uji ketahanan putus Wi-Fi/MQTT 3 menit saat beban aktif (`inhibit=0`, `mtxMiss=0`, nol reboot WDT, auto-reconnect), gate `Maintenance Unlock` + NVS live, serta akses 4G/5G via HTTPS port `443` (port `8123`, `1883`, `22` tertutup dari WAN).

---

## 6. Changelog Lengkap (Git Commits + Seluruh Riwayat Sesi)

| Urutan / Commit | Waktu (`+0700`) | Ringkasan Perubahan Kode, Analisis & Temuan Lapangan |
|---|---|---|
| **1. `f158329`** | `2026-11-21 11:21` | **Fondasi Proyek (`v3.0`):** Reverse-engineering `TianBangda MS-ShotoV1.1.exe` (`SHA-256: 75d3bdf7...`), PDF `Shoto Modbus V1.2`, dan DOC `YD/T 1363.3 V2.8`. Pembuatan `safety_core.hpp`, `bridge_protocols.hpp`, `shoto_samoto_bridge.ino`, `tools/serial_lab.py`, unit test Python & C++, diagram SVG/PNG (`diy-xy-k485`, `patchcord-*`, `shoto-cascade`, `waveshare`), `GUIDE*.md`, dan `GUIDE-A-DIY-PRINT.pdf`. |
| **2. Sesi Debug 1** | `2026-11-21 12:00–17:30` | • Fix compile ESP32: ganti `std::clamp` ke template `clamp_val<T>` kompatibel IDF4 `gnu++11`, support `esp_task_wdt` IDF4 & IDF5, `#undef min/max`.<br>• Panduan hardware `3V3` pin-split untuk `2× XY-K485`.<br>• Setup macOS USB port mapping (Hackintool/USBToolBox) agar `Autonics SCM-US48I` terdeteksi (`/dev/cu.usbserial-B002C8GU`) & install `pyserial`. |
| **3. `306bced`** | `2026-11-21 18:11` | **Dukungan Live `SD-BA430-15S-V23-V22`:** Diperoleh temuan live bahwa baterai merespons langsung ke `7C 01 01 00 02 0D` dengan **TLV 140-byte** (`7E 01 01 86 ...`) tanpa prefix `16× 0xAA` (justru rusak oleh `0xAA`, sempat terbaca palsu `V=15.37V SOC=2.0%`). Ditambahkan parser TLV Format B, shifted `FC04` @ `addr=214`, `g_tbd_locked_no_wake`, turnaround glitch resync, anti-fallthrough ke Format A, dan CLI `batraw`. |
| **4. Sesi Debug 2** | `2026-11-21 19:07–23:30` | • Capture port BMS **Samoto `ILS3600`** (`data/samoto_pin12.log`): pinout terbukti **`Pin 2 (Oranye) = A+`, `Pin 1 (Putih-Oranye) = B-`** @ `9600 8N1`, polling campuran `60H` (`~20024660...`/`~20124660...`) + Modbus `01 03...`.<br>• Penjelasan 4 parameter kebijakan (`CVL/DVL/CCL/DCL`) & sifat bridge 1-arah.<br>• Penambahan responder `60H` + adaptive `VER/ADR` → **`ALA 245` hilang total**.<br>• Capture `data/esp32_debug.log` (184 baris via `serial_lab.py esp-log`) mengungkap penyebab **`ALA 235`**: bukan `DVL=46V`, melainkan `tag 0x06 word 1 = 0x0002` (status discharging normal SD-BA430) keliru di-map ke bit `0x10` Cell-UV `62H` (`10000000`). Diperbaiki → **`ALA 235` hilang total**. |
| **5. `fcf803c`** | `2026-11-21 23:55` | **Setting 24/7 + Per-Cell CSV + Mode Dinamis 0..4 Pack:** Parameter diset ke sweet-spot 24/7 (`CVL 53.6 V`, `DVL 46.0 V`, `0.30C/0.50C` per pack multi-pack), `aggregate_bank` & `60H` diperluas 1–4 pack, ditambah log `[CELLS P# ...]` + CLI `cells`/`celllog` + `esp-log --csv`. Sempat memakai `SYSTEM_PROFILE` statis lalu langsung diubah atas permintaan user menjadi **`DYNAMIC_PACK_DISCOVERY = true`** dengan `DynamicLimits`/`compute_dynamic_caps` (`0..4` pack tanpa upload ulang), CLI `packs`/`rescan`, dan Suite 8 di `tests/bridge_test.cpp`. |
| **6. `94f4dbd`** | `2026-11-22 07:58` | **Audit *Makesure* & Fix Glitch Hot-Unplug:** Ditemukan celah timing saat `PACK_OFFLINE_MISSES=3` + `POLICY_FRESHNESS_MS=6000` (miss ke-2 menembus 6 detik sebelum pack dicabut dianggap offline → kirim `SOC=0%` + fault sesaat). Diperbaiki menjadi `PACK_OFFLINE_MISSES=2`, `POLICY_FRESHNESS_MS=9000UL`, timeout `TBD_BINARY=500u`, dan ditambah compile-time `static_assert`. |
| **7. Sesi Debug 3** | `2026-11-22 00:06–00:38` | • Konfirmasi mekanisme 1-arah & prosedur charge seimbang 1-per-1 sebelum paralel.<br>• Pembuatan step-by-step paralel Pack #2, disusul uji fisik user (`XY-K485 #1 RXD` mati, collision `SOC 67.0% vs 67.3%`, `RX 256B`, `ALA 245` kedap-kedip).<br>• Cross-check MOP resmi `reference/instalasi-baterai-lithium-shoto-sda10-48100_compress.pdf` (hal. 5, 6, 11) membuktikan **arah `RS485A` vs `RS485B` di `diagrams/shoto-cascade.svg` terbalik** serta Pack #2 belum mendapat **Endloop `120 Ω`** dan **reboot `RESET` bersama**. |
| **8. Dokumen Ini** | `2026-11-22 08:05` | Pembuatan `PROJECT_CONTEXT.md` sebagai konteks global tunggal proyek. |
| **9. Sesi Debug 4 & Opsi A (`IHF6200` + `Siseli`)** | `2026-11-22` | • Pemasangan 1-pack pada **Samoto `IHF6200` + dongle `Solar Plug-RWB1-06` (`solar.siseli.com`)**.<br>• Diagnosis & resolusi boot-loop `assert failed: do_core_init startup.c:328 (flash_ret == ESP_OK)` (`ESP32-WROOM-DA Module` vs `ESP32 Dev Module`, `DIO`/`40MHz`, `GPIO12`/EMI/`3V3`).<br>• Analisis [`data/ihf6200_raw.log`](data/ihf6200_raw.log) & foto manual `IHF6200` hal. 20: sebelumnya `Menu 05 = LIA` hanya scan `~20024661`/`~20124661` + Modbus (`Display Mode: Do Not Display BMS Details`); setelah `Menu 05 = PYL`, terkunci loop Pylontech V3.5 **`63H -> 61H -> 62H`** (`err=0`, `CVL 53.6V`, `DVL 46.0V`, `CCL 45A`, `DCL 80A`, status `0xC0`) dan Siseli berubah ke `"Display Data for 16 Battery Cells"` (`Remaining/Nominal Capacity` & `Battery Voltage 1..16` tetap `0` karena `05=PYL` tidak mem-poll `42H`).<br>• Penerapan **Opsi A**: pelacakan `min_cell_pack/idx` & `max_cell_pack/idx` di 5 decoder sel Shoto + `aggregate_bank()`, serta encoding desimal `pack_id * 100 + cell_num` pada frame `61H` (`bridge_protocols.hpp` & `tests/bridge_test.cpp`) menggantikan hardcode `0x0101`/`0x0108` (`ID:2(57)`/`ID:2(64)`). |
| **10. Verifikasi Live `solar.siseli.com` (`IHF6200` + Opsi A)** | `2026-11-22 21:58–22:05` | • Upload Opsi A ke ESP32 diverifikasi live pada `solar.siseli.com` (`Device Type = HPVINV02`, `Software Version = 40.09`): `Max/Min Voltage Cell Position` kini akurat terbaca **`ID:1(1)` / `ID:1(6)`** (`3323`/`3304 mV`).<br>• Verifikasi silang 1:1 Serial Monitor ESP32 (`V=49.66V I=-4.65A SOC=73.3% T=36.0C CVL=53.60V DVL=46.00V CCL=45.0A DCL=80.0A`) vs Siseli (`BMS Current SOC = 73%`, `BMS Discharge Current = 3.8A`, `BMS Average Temperature = 35.95°C`, `CVL = 53.6V`, `DVL = 46V`, `CCL = 45A`, allow charge/discharge `Yes`, seluruh 6 flag proteksi `62H` = `No`).<br>• Dokumentasi resmi fakta & akar masalah field `Remaining Capacity = 0 A`, `Nominal Capacity = 0 A`, & `Battery Voltage 1..16 = 0 mV` (`IHF6200` pada `Menu 05 = PYL` hanya polling `63H/61H/62H` tanpa `42H`, bukan bug bridge), serta catatan verifikasi charging siang agar tetap terkunci di `<= 53.6V` / `<= 45A` dan tidak mengikuti fallback `56.4V`. |
| **11. Audit Parameter Read-Only Shoto (`params` CLI & `esp-log --params`)** | `2026-11-23` | • Analisis REA (`inspect_managed_members` `Form1::baohucs` / `timer_readbhcs_Tick` / `gaojcs_read` pada `TianBangda V1.1.634-26.exe` + `data/tbdset.tbd` + `Modbus V1.2`) memetakan 7 parameter uji user antara EEPROM Shoto BMS vs parameter inverter Samoto vs bridge (`config.h`).<br>• Penambahan decoder `TbdAlarmParams`, `parse_tbd_param_u16()`, `parse_tbd_alarm_params_43()` ([`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp)), perintah CLI ESP32 **`params`** (`run_shoto_param_audit()` Core 0 *read-only* tanpa `0xAA` dengan `esp_task_wdt_reset()`, [`shoto_samoto_bridge.ino`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino)), mode capture **`--params`** pada [`tools/serial_lab.py`](tools/serial_lab.py), serta Suite 9 di [`tests/bridge_test.cpp`](tests/bridge_test.cpp) dan unit test `test_esp_log_params_mode_captures_and_stops_on_done` di [`tests/test_serial_lab.py`](tests/test_serial_lab.py). |
| **12. Hardening 3-Pack `IHF6200` (`DCL Cap 120A`, Retry `delta_V`, & `diag` CLI)** | `2026-11-24` | • Capture live 22s pada 3 pack cascade (`V=49.81V, I=-7.35A, SOC=80.3%, ok=21 err=0, delta_V=0.06V`) memastikan sisi Shoto sehat.<br>• Penambahan plafon aman tingkat bank `POLICY_CCL_BANK_MAX_A = 90.0f` dan `POLICY_DCL_BANK_MAX_A = 120.0f` ([`config.h`](firmware/shoto_samoto_bridge/config.h), `DynamicLimits`/`compute_dynamic_caps` di [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp)) agar `63H` pada 3–4 pack tidak mengirim `DCL=150A/200A` di atas batas `120 A` `IHF6200`.<br>• Penambahan `classify_bank_inhibit()` (`InhibitReason`) di [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp), percepatan jeda antar-pack (`25 ms`), **1× konfirmasi re-poll langsung** saat `INHIBIT_DELTA_V` akibat skew polling multi-pack, counter `inhibit`/TX `60H..63H` + perintah CLI **`diag`** di [`shoto_samoto_bridge.ino`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino), serta **Suite 10** di [`tests/bridge_test.cpp`](tests/bridge_test.cpp).<br>• Verifikasi live pasca-upload dengan `Menu 05 = PYL` (`20:57`, `21:00`, `21:04`): terkunci penuh **`PYL_FULL`**, `inhibit=0`, `err=0`, bertahan di `Battery Mode` pada `49,78 V / 79,0% SOC` (`I=-8,18..-9,56 A`), disertai audit *read-only* `params` pada `addr=1`. Penyusunan laporan [`ISSUE_REPORT_2026-11-24.md`](ISSUE_REPORT_2026-11-24.md) dan glosarium [`GLOSSARY.md`](GLOSSARY.md). |
| **13. Sinkronisasi Diagram Wiring Aktual (`SVG` + `PNG`)** | `2026-11-24` | • Pembaruan ke-4 diagram wiring + re-render PNG presisi: [`patchcord-xy-k485-samoto.*`](diagrams/patchcord-xy-k485-samoto.svg) (kini `Pin 2 Oranye = A+`, `Pin 1 Putih-Oranye = B−`, `Pin 3–8 NC`, menu `PYL` / `ILS`), [`patchcord-xy-k485-shoto.*`](diagrams/patchcord-xy-k485-shoto.svg) (`#1 RS485A`, `Pin 1=A+`, `Pin 2=B−`), [`shoto-cascade.*`](diagrams/shoto-cascade.svg) (arah MOP `#1 RS485B → #2 RS485A` + Endloop `120 Ω` di `RS485B` akhir + reboot `RESET`), dan [`diy-xy-k485.*`](diagrams/diy-xy-k485.svg) (keduanya aktual). |
| **14. Integrasi Home Assistant, MQTT Discovery (`212` Entitas) & Hardening Mini PC (`e3912b4..4fae5e6`)** | `2026-11-25` | • **Implementasi (`v3.1.0`, strict read-only ke EEPROM Shoto):**<br>  – [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp): `TbdProtParams` (`13` register `cmd 0x06..0x50`), `TbdAlarmParams` penuh (`43` tag `0x00..0x2A` termasuk suhu `-50 °C` & delay ms), `PackEepromSnapshot` + Suite 11 di [`tests/bridge_test.cpp`](tests/bridge_test.cpp).<br>  – [`ha_mqtt_codec.hpp`](firmware/shoto_samoto_bridge/ha_mqtt_codec.hpp) + [`tests/ha_mqtt_test.cpp`](tests/ha_mqtt_test.cpp): encoder JSON statis `snprintf` (`<= 1400 B`), `212` entitas MQTT Discovery (`MAX_PACKS=4`), clamp 15S LFP, dan validator aksi 2-langkah `Maintenance Unlock` (`300 s`).<br>  – [`shoto_samoto_bridge.ino`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino), [`config.h`](firmware/shoto_samoto_bridge/config.h), [`platformio.ini`](firmware/shoto_samoto_bridge/platformio.ini), [`secrets.h.example`](firmware/shoto_samoto_bridge/secrets.h.example), [`.gitignore`](.gitignore): `ha_mqtt_task` (Core `0`, prio `1`, stack `8192`, guarded `#define ENABLE_HA_MQTT 0`), backoff non-blocking, LWT, NVS (`Preferences` `"shoto_ha"`), OTA LAN-only, & CLI `params [1\|2\|3\|4\|all]`.<br>  – [`homeassistant/`](homeassistant/docker-compose.yml) + [`GUIDE-HOME-ASSISTANT.md`](GUIDE-HOME-ASSISTANT.md) + [`tests/test_ha_artifacts.py`](tests/test_ha_artifacts.py): Docker Compose (`homeassistant` + `mosquitto` LAN `:1883` + `caddy` HTTPS `:443`), `configuration.yaml` (`trusted_proxies`, `ip_ban_enabled`, `recorder` 30d), dashboard Lovelace 4-view, otomasi notifikasi & script audit EEPROM.<br>• **HOST-VERIFIED (`2026-11-25`):**<br>  – `python3 -m unittest discover -s tests -v`: **26/26 OK** (`14` `test_ha_artifacts.py` + `12` `test_serial_lab.py`).<br>  – `tests/safety_test.cpp`: **7/7 skenario PASS** (`-std=gnu++17` & `-std=gnu++11`, `-Wall -Wextra -Werror`).<br>  – `tests/bridge_test.cpp`: **11/11 suites PASS** (`-std=gnu++17` & `-std=gnu++11`, `-Wall -Wextra -Werror`).<br>  – `tests/ha_mqtt_test.cpp`: **4/4 suites PASS** (`-std=gnu++17` & `-std=gnu++11`, `-Wall -Wextra -Werror`).<br>  – **9/9 `.ino` stub builds** (`IDF4/IDF5 × Dynamic/Fixed × ENABLE_HA_MQTT=0/1` + `BOARD_WAVESHARE_S3`) lulus 0 warning (`-std=gnu++17 -Wall -Wextra -Werror`).<br>• **PENDING LIVE DEPLOYMENT (Steps 2–4 [`2026-11-25-home-assistant-integration.md`](docs/superpowers/plans/2026-11-25-home-assistant-integration.md) — belum dijalankan di hardware):**<br>  – Provisioning Mini PC (HAOS/Docker + Mosquitto + reverse proxy HTTPS `:443` + 2FA TOTP), pembuatan `secrets.h` & flash ESP32 `ENABLE_HA_MQTT 1`, uji putus Wi-Fi/MQTT saat beban aktif, uji `Maintenance Unlock`/NVS live, & verifikasi akses 4G/5G via IP Publik port `443`. |

---

## 7. Perintah Verifikasi & *Hard Invariants*

### 7.1 Suite Verifikasi Standar (Jalankan Setiap Setelah Mengubah Kode)
```bash
# 1. Python Unit Tests (26 tests: 14 HA artifacts + 12 serial_lab/live captures/params)
python3 -m unittest discover -s tests -v

# 2. C++ Safety Core Tests (7 skenario, gnu++11 untuk IDF4 & gnu++17 untuk IDF5)
g++ -std=gnu++11 -Wall -Wextra -Werror -Ifirmware/shoto_samoto_bridge tests/safety_test.cpp -o /tmp/safety_11 && /tmp/safety_11
g++ -std=gnu++17 -Wall -Wextra -Werror -Ifirmware/shoto_samoto_bridge tests/safety_test.cpp -o /tmp/safety_17 && /tmp/safety_17

# 3. C++ Bridge Protocol, 43-Tag EEPROM & Dynamic Scaling Tests (11 suites)
g++ -std=gnu++11 -Wall -Wextra -Werror -Ifirmware/shoto_samoto_bridge tests/bridge_test.cpp -o /tmp/bridge_11 && /tmp/bridge_11
g++ -std=gnu++17 -Wall -Wextra -Werror -Ifirmware/shoto_samoto_bridge tests/bridge_test.cpp -o /tmp/bridge_17 && /tmp/bridge_17

# 4. C++ HA MQTT Codec, 212-Entity Discovery & Action Validator Tests (4 suites)
g++ -std=gnu++11 -Wall -Wextra -Werror -Ifirmware/shoto_samoto_bridge tests/ha_mqtt_test.cpp -o /tmp/ha_mqtt_11 && /tmp/ha_mqtt_11
g++ -std=gnu++17 -Wall -Wextra -Werror -Ifirmware/shoto_samoto_bridge tests/ha_mqtt_test.cpp -o /tmp/ha_mqtt_17 && /tmp/ha_mqtt_17

# 5. Pastikan bersih dari temporary binary (serta 9 stub build .ino IDF4/5 x Dyn/Fixed x HA=0/1 + Waveshare S3)
rm -f /tmp/safety_11 /tmp/safety_17 /tmp/bridge_11 /tmp/bridge_17 /tmp/ha_mqtt_11 /tmp/ha_mqtt_17
```
*(Terakhir diverifikasi penuh di host pada branch `feat/home-assistant-integration` basis commit `4fae5e6`: 26 Python tests, seluruh unit test C++11/C++17, dan 9/9 stub build `.ino` dengan `-Wall -Wextra -Werror`.)*

### 7.2 *Hard Invariants* (Dilarang Dilanggar Saat Modifikasi Kode/Hardware)
1. **Jangan kirim `16× 0xAA`** ketika protokol baterai sudah terkunci ke `PROTO_TBD_BINARY` atau bukan mode `PROTO_AUTO` (`SD-BA430` rusak responsnya jika diberi `0xAA`).
2. **Jangan ubah konvensi tanda arus kanonik** di `Pack::bus_a` & `BankExt` (`+A = Charging`, `-A = Discharging`).
3. **Jangan memetakan bit `tag 0x06` SD-BA430 langsung sebagai Pylontech alarm**; pertahankan evaluasi alarm dari batas tegangan sel/pack/suhu terukur.
4. **Jangan melonggarkan fail-closed safety:** `0` pack, stale `> POLICY_FRESHNESS_MS`, delta tegangan `> 1.0 V`, atau critical fault **wajib** mengunci `CCL=0 A` / `DCL=0 A`.
5. **Pertahankan kompatibilitas C++11 (`IDF 4`) + C++17 (`IDF 5`) + `#undef min/max`** serta jangan menggunakan C++14 aggregate brace-init pada struct dengan default member initializer.
6. **Pertahankan `static_assert` timing hot-unplug** di `shoto_samoto_bridge.ino` jika mengubah `PACK_OFFLINE_MISSES`, `POLL_INTERVAL_MS`, atau `POLICY_FRESHNESS_MS`.
7. **Fisik DC & Cascade:** Selalu samakan tegangan antarbaterai (`< 0.5 V`) dengan MCB OFF sebelum menggabungkan kabel daya DC paralel; ikuti arah resmi MOP (`Reader → #1 RS485A`, `#1 RS485B ↔ #2 RS485A`, `Endloop 120 Ω → Pack Terakhir RS485B`) + reboot BMS lewat `RESET`.
8. **Encoding Posisi Sel Pylontech `61H` (`Samoto/Siseli`):** Gunakan format desimal `pack_id * 100 + cell_num` (1-based) pada offset info `61H` (`13..14` untuk max cell, `17..18` untuk min cell) serta `temp_pos = 101u`; jangan kembalikan ke packing biner `0x01xx` karena `Samoto/Siseli` menafsirkannya sebagai `ID:2(57)`/`ID:2(64)`.
