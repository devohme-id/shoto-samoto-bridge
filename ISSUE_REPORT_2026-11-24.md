# Laporan Diagnosa & Penyelesaian Issue Multi-Pack (`2026-11-24`)

> **Tanggal Kejadian & Verifikasi:** `2026-11-24` (`20:09–21:05 WIB`)  
> **Konfigurasi Sistem:** 3× Shoto `SD-BA430 / 15SBA430` (`addr=1, 2, 3`, 300 Ah total) ↔ ESP32 Bridge ↔ Inverter Samoto `IHF6200` (`HPVINV02` v`40.09`, dongle `Solar Plug-RWB1-06`)  
> **Status Akhir:** **TERATASI & TERVERIFIKASI LIVE** (`PYL_FULL`, `49,78 V / 79,0% SOC`, `err=0`, `inhibit=0`, tetap bertahan di `Battery Mode`)

---

## 1. Ringkasan Gejala (*Symptom*)

Setelah ketiga baterai Shoto (`Pack #1`, `Pack #2`, dan `Pack #3`) berhasil dirangkai berantai (*cascade*) dan terbaca seimbang oleh modul ESP32 (`dV` per-pack `8–18 mV`), muncul kendala saat komunikasi BMS dihubungkan ke inverter `Samoto IHF6200`:

* Inverter **otomatis pindah ke PLN (*auto-switch to PLN*)** dengan indikasi tegangan baterai rendah ketika tegangan/kapasitas menyentuh di bawah **`~49,9 V / 80%`** (terjadi di mode `USE`, `PYL`, maupun `LIA`).
* Sebaliknya, ketika komunikasi BMS dimatikan (inverter bekerja tanpa jembatan BMS), inverter berjalan normal mengikuti aturan tegangan manual yang selama ini stabil digunakan.

---

## 2. Investigasi & Akar Masalah (Berdasarkan Bukti Langsung)

Dari pembacaan langsung aliran data serial ESP32 (`/dev/cu.usbserial-1410`), pembacaan *read-only* memori **EEPROM** *(memori penyimpan pengaturan permanen di dalam BMS)* baterai Shoto, serta pembandingan dengan uji 1-pack pada `2026-11-22` (yang berjalan normal di `Battery Mode` pada `49,3 V / 73%`), ditemukan **4 fakta utama**:

### 2.1 Ketiga Baterai Shoto dalam Kondisi 100% Sehat (Bukan Masalah Baterai)
* Pembacaan live 3 pack menunjukkan status sangat sehat: `ok=21 err=0`, nol alarm (`prot=000000`, `fault=0000`), selisih tegangan antarbaterai hanya `0,06 V`, dan selisih tegangan antarsel hanya `5–14 mV`.
* Audit *read-only* langsung ke EEPROM BMS Shoto (`addr=1`, perintah `params` `TBD_BINARY`) memastikan **tidak ada aturan `49,9 V` atau `80%` di dalam baterai Shoto**. Batas bawah internal Shoto jauh lebih rendah:
  * Alarm tegangan rendah (`cmd 0x43`): **`42,00 V`** *(pulih/recovery `48,00 V`)*
  * Alarm kapasitas rendah (`cmd 0x43`): **`20%`** *(pulih/recovery `25%`)*
  * Pemutus proteksi/cut-off pack (`cmd 0x2E`/`0x2F`): **`40,00 V`** *(pulih/recovery `48,00 V`)*

### 2.2 Batas Arus Discharge 3 Pack (`DCL = 150 A`) Melebihi Kapasitas Inverter (`120 A`)
* Pada uji 1 pack (`2026-11-22`), bridge mengirimkan batas pengosongan **`DCL = 80 A`** (*Discharge Current Limit*) dan pengisian **`CCL = 45 A`** (*Charge Current Limit*), dan `IHF6200` bekerja normal di `49,3 V / 73%`.
* Ketika 3 pack aktif, rumus pengalian otomatis lama (`50 A/pack × 3`) mengirim **`DCL = 150 A`** di paket aturan `63H`, melampaui rating inverter `IHF6200` `6,2 kW` (`120 A`; terlihat juga pada parameter `Maximum Total Charging Current = 120 A` di `solar.siseli.com`).

### 2.3 Potensi Sinyal Pengaman Sesaat (*Transient Inhibit*) + Penguncian PLN (*Latching*)
* ESP32 membaca `Pack #1 → #2 → #3` secara berurutan. Dengan jeda antar-pack lama (`80 ms`), lonjakan beban sesaat (mis. AC/pompa menyala) berisiko menghasilkan selisih bacaan tegangan antar-pack `> 1,0 V` untuk 1 siklus.
* Firmware sebelumnya langsung mengubah status bank menjadi **`INHIBIT`** pada 1 kejadian saja, lalu mengirim `SOC = 0%` (`61H`), `Allow Discharge = No` (`63H`), dan `System Fault` (`62H`) ke inverter.
* Berdasarkan tangkapan `solar.siseli.com` (`2026-11-22`), `IHF6200` diset dengan **`BMS Returns To Battery Mode SOC = 90%`** (dan tegangan `51 V`). Artinya, begitu inverter sempat terpental ke PLN satu kali saja saat kapasitas di sekitar `80%`, inverter akan **terkunci di PLN** sampai baterai terisi kembali hingga `>= 90%`.

### 2.4 Pengaruh Mode Baterai Inverter (`USE` / `LIA` vs `PYL`)
* Tangkapan raw langsung di kabel data saat inverter berada di `Menu 05 = USE` membuktikan `IHF6200` tetap mem-poll port BMS setiap ~1,8 detik memakai campuran Modbus (`01 03...`) dan **`61H`** saja, **tanpa pernah meminta `63H` (batas `CVL/DVL/CCL/DCL`) maupun `62H` (status alarm)**.
* Protokol Pylontech V3.5 penuh (`63H + 61H + 62H`) hanya terkunci saat inverter berada di **`Menu 05 = PYL`**.

---

## 3. Perbaikan Firmware ESP32 yang Diterapkan

Seluruh perbaikan bersifat mempertahankan prinsip keamanan *fail-closed* (tetap memutus aman jika ada bahaya nyata), dengan peningkatan di 4 file:

1. **Plafon Arus Aman Tingkat Bank (`CCL 90 A`, `DCL 120 A`)** — [`config.h`](firmware/shoto_samoto_bridge/config.h) & [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp)
   * Menambahkan `POLICY_CCL_BANK_MAX_A = 90.0f` dan `POLICY_DCL_BANK_MAX_A = 120.0f` serta pembatas di `DynamicLimits` / `compute_dynamic_caps()` agar bank 3–4 pack tidak pernah mengirim `DCL` `150 A / 200 A` ke `IHF6200`.
2. **Konfirmasi Baca Ulang Sesaat (*Anti-Glitch* `delta_V`)** — [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp) & [`shoto_samoto_bridge.ino`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino)
   * Menambahkan `classify_bank_inhibit()` (`InhibitReason`) dan mempercepat jeda antar-pack dari `80 ms` menjadi `25 ms`.
   * Jika terdeteksi `INHIBIT_DELTA_V` (`> 1,0 V`), ESP32 melakukan **1× baca ulang cepat konfirmasi langsung (`15 ms` antar-pack)** sebelum mengunci `g_bank` — menghilangkan false alarm akibat skew beban sesaat tanpa melonggarkan proteksi selisih tegangan nyata.
3. **Alat Diagnostik Live (`diag` & `status`)** — [`shoto_samoto_bridge.ino`](firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino)
   * Menambahkan counter `inhibit`, `last_inhibit_reason`, `dvRetry` (`ok/fail`), `peakDV`, `peakAge`, `mtxMiss`, counter per-perintah inverter (`60H/61H/62H/63H`), deteksi mode (`PYL_FULL` vs `SCAN_61H_ONLY`), serta tampilan frame TX terakhir (`63H/62H/61H`).
4. **Unit Test Regresi (Suite 10) & Build Check** — [`tests/bridge_test.cpp`](tests/bridge_test.cpp)
   * Memverifikasi plafon `90 A / 120 A`, frame `63H` 3-pack (`D160B3B0038404B0C0`), seluruh 8 kode `InhibitReason`, Python unittest (`12/12 OK`), C++ `gnu++11`/`gnu++17` (`-Wall -Wextra -Werror`), dan 4 varian build `.ino` (`PASS`).

---

## 4. Hasil Verifikasi Live Pasca-Upload (`Menu 05 = PYL`)

Setelah firmware baru di-upload ke ESP32 dan inverter diset ke `Menu 05 = PYL` (komunikasi BMS ON), dilakukan 3 sesi pembacaan *read-only* langsung pada `20:57`, `21:00`, dan `21:04 WIB`:

* **Mode Protokol Inverter (`diag`):** **`PYL_FULL (63H+61H+62H)`**, `err=0`, `inhibit=0`, `dvRetry=0/0`, `peakDV=0,06 V`, `mtxMiss=0`.
* **Aturan Aktif yang Dikirim ke Inverter (`63H`):** `CVL = 53,60 V`, `DVL = 46,00 V`, `CCL = 90,0 A`, `DCL = 120,0 A`, status `0xC0` (*Allow Charge + Discharge = Yes*), `62H = 00 00 00 00` (nol alarm).
* **Pembuktian Bertahan di Bawah Ambang Lama `49,9 V / 80%`:**
  * `20:57 WIB`: **`V = 49,78 V` · `SOC = 79,3%` · `I = -8,63 A`** (`Battery Mode`, stabil).
  * `21:00 WIB`: **`V = 49,78 V` · `SOC = 79,2%` · `I = -9,41 A`** (`P1=-2,79 A`, `P2=-3,06 A`, `P3=-2,90 A`, stabil).
  * `21:04 WIB` *(sesudah audit EEPROM `params` selesai)*: **`V = 49,78 V` · `SOC = 79,0%` · `I = -8,18 A`** (`Battery Mode`, stabil).

---

## 5. Catatan Batas Akses Baca Parameter Inverter & Tindak Lanjut

* **Mengapa parameter `Return To Mains Mode Voltage` / `BMS Returns To Mains Mode SOC` tidak bisa dibaca via ESP32?**  
  Karena port **BMS RS485 (`Pin 1/2`)** pada `IHF6200` bersifat master-ke-BMS (hanya polling `61H/62H/63H`) dan tidak membuka pembacaan menu internal inverter. Pengaturan internal inverter hanya terbaca melalui LCD inverter (`Program 12/13`), portal cloud `solar.siseli.com` (via dongle WiFi port RS232 `Solar Plug-RWB1-06`), atau pembacaan langsung kabel USB-RS232 PC ke port COM inverter.
* **Tindak Lanjut yang Disarankan:**
  1. Cek 1× di `solar.siseli.com` / LCD `IHF6200` nilai aktual `Return To Mains Mode Voltage`, `Return To Battery Mode Voltage`, `BMS Returns To Mains Mode SOC`, dan `BMS Returns To Battery Mode SOC` agar sesuai kebutuhan operasi harian Anda.
  2. Cek 1× saat **PV charging siang hari** di `solar.siseli.com` untuk memastikan arus pengisian dibatasi `<= 90 A` dan tegangan pengisian berhenti aman di `<= 53,6 V` (tidak mengikuti angka fallback `56,4 V`).
