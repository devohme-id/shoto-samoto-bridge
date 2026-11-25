# Glosarium Lengkap Proyek Bridge Shoto ↔ Samoto

> Kumpulan istilah teknis, kode protokol, singkatan parameter baterai/inverter, serta perintah diagnosa untuk mempermudah pencarian dan pemeliharaan sistem [`shoto_samoto_bridge`](PROJECT_CONTEXT.md).

---

## 1. Kode Perintah Protokol Pylontech RS485 (Bridge ↔ Inverter)

> **Catatan:** Akhiran **`H`** menandakan bilangan **Heksadesimal** *(basis-16 menggunakan angka `0–9` dan huruf `A–F`, standar komunikasi data serial BMS/inverter)*.

| Istilah / Kode | Kepanjangan / Arti | Penjelasan Sederhana & Fungsi di Sistem |
|---|---|---|
| **`60H`** | *Get System Basic Info* (V3.5) | Permintaan inverter untuk membaca identitas bank baterai: nama model (`Force_L`), pabrikan (`Pylon`), jumlah pack aktif (`1–4`), dan daftar barcode/serial tiap pack. |
| **`61H`** | *Get System Analog Value* (V3.5) | Paket data utama telemetri bank baterai (49 byte): tegangan total (`mV`), arus total (`0,01 A`), `SOC` (`%`), jumlah siklus (*cycles*), `SOH` (`%`), nilai & posisi sel tertinggi/terendah (`pack_id * 100 + cell_num` → `ID:pack(cell)`), serta suhu rata-rata/min/max. |
| **`62H`** | *Get System Alarm Info* (V3.5) | Paket status peringatan (*warning*) dan proteksi (*protection*) 4 byte (`a1, a2, p1, p2`): Over/Under Voltage sel & pack, suhu tinggi/rendah, arus berlebih, dan *System Fault* (`p2` bit 3). Nilai `00 00 00 00` menandakan seluruh sistem sehat tanpa alarm. |
| **`63H`** | *Get System Charge/Discharge Management Info* (V3.5) | Paket aturan kendali utama dari bridge ke inverter (9 byte): berisi **`CVL`**, **`DVL`**, **`CCL`**, **`DCL`**, dan 1 byte izin operasi (*Charge/Discharge Enable*). |
| **`0xC0` / `0x80` / `0x40` / `0x00`** | Byte Status Izin di `63H` | **`0xC0`** = Boleh Charge & Boleh Discharge (`Yes/Yes`); **`0x80`** = Hanya boleh Charge; **`0x40`** = Hanya boleh Discharge; **`0x00`** = **INHIBIT** (Charge & Discharge dilarang semua). |
| **`42H`** | *Get Pack Analog Value* (V3.3) | Paket telemetri per-pack yang memuat tegangan individu seluruh 15 sel serta kapasitas tersisa & nominal (`Ah`). Sudah didukung ESP32, namun **`IHF6200` pada `Menu 05 = PYL` tidak memintanya** (itu sebabnya kolom sel `1..16` & kapasitas `Ah` di `solar.siseli.com` bernilai `0`). |
| **`44H`** | *Get Pack Alarm Info* (V3.3) | Paket alarm per-sel, status MOSFET (*charge/discharge FET*), dan status sistem pada Pylontech V3.3. |
| **`47H`** | *Get Pack Charge/Discharge Management Info* (V3.3) | Versi per-pack dari `63H` (membawa alamat pack + `CVL/DVL/CCL/DCL` + byte izin). |
| **`4FH`** | *Get Protocol Version* | Permintaan pengecekan versi protokol Pylontech. |
| **`51H`** | *Get Manufacturer Info* | Permintaan nama perangkat (`SHOTO_BRDG`) & pabrikan (`PYLON`) pada protokol Pylontech V3.3. |
| **`92H`** | *Get Pack Quantity / Management* | Permintaan jumlah modul baterai paralel yang aktif. |
| **`YD/T 1363.3`** | Standar Protokol Komunikasi Telecom Tiongkok | Format bingkai (*frame*) teks ASCII yang dipakai Pylontech dan sebagian BMS Shoto: diawali **`~`** (`SOI`, `0x7E`) dan diakhiri *Carriage Return* **`\r`** (`EOI`, `0x0D`). |
| **`SOI` / `EOI`** | *Start / End of Information* | Penanda awal (`~`) dan akhir (`\r`) satu paket data pada format `YD/T 1363.3`. |
| **`VER` / `ADR`** | *Version* / *Address* | Versi protokol (mis. `20`) dan alamat tujuan/sumber baterai (mis. `02` atau `12` pada `IHF6200`). Bridge memantulkan kembali `VER` & `ADR` sesuai permintaan inverter. |
| **`CID1` / `CID2` / `RTN`** | *Command ID 1/2* & *Return Code* | `CID1 = 46H` menandakan grup baterai lithium; `CID2` adalah kode perintah (`60..63`, `42`, dll); pada balasan posisi `CID2` menjadi `RTN` (`00` = sukses, `04` = perintah tidak dikenal). |
| **`LENGTH` (`LENID` + `LCHKSUM`)** | Bidang Panjang Data + Checksum Panjang | 4 karakter hex penanda panjang isi payload agar penerima memastikan paket tidak terpotong. |
| **`CHKSUM` / `CRC-16`** | *Checksum* / *Cyclic Redundancy Check* | Kode verifikasi matematika di akhir frame untuk mendeteksi data rusak akibat *noise* kabel. Paket dengan checksum salah otomatis ditolak. |

---

## 2. Protokol & Perintah Internal Baterai Shoto (ESP32 ↔ Baterai)

| Istilah / Kode | Kepanjangan / Arti | Penjelasan Sederhana |
|---|---|---|
| **`PROTO_TBD_BINARY`** | *TianBangda Binary Protocol* | Protokol aktif yang terbukti dipakai Shoto `SD-BA430` (`9600 bps`, `addr = 1..4`). Memakai paket biner diawali `0x7C` atau `0x7E` dan diakhiri `0x0D`. |
| **`TLV` (Format B, 140 Byte)** | *Tag-Length-Value* | Struktur data balasan 140 byte milik `SD-BA430` (`7E <addr> 01 86 ... 0D`): tiap data diawali *Tag* (`0x01` = 15 sel, `0x02` = arus, `0x03` = SOC, `0x04` = SOH, `0x05` = suhu, `0x06` = status kerja, `0x07` = siklus, `0x08` = tegangan pack, `0x09` = kapasitas). |
| **Format A (86/88 Byte)** | Format Biner TianBangda Lama | Format paket ukuran tetap milik firmware TianBangda generasi lama (sebelum format TLV). |
| **Prefix `16× 0xAA`** | Sinyal *Wakeup* UART Lama | Sinyal pembangun baterai pada seri TianBangda V1.1 lama. **DILARANG** dikirim ke `SD-BA430` karena merusak parsernya (dikunci oleh `g_tbd_locked_no_wake`). |
| **`cmd 0x01`** | Perintah Baca Data Live (`TBD`) | Perintah *read-only* yang dikirim ESP32 tiap 1,5 detik untuk membaca tegangan, arus, SOC, suhu, dan 15 sel tiap baterai Shoto. |
| **`cmd 0x06..0x50` & `cmd 0x43`** | Perintah Baca Parameter Proteksi & Alarm (`TBD`) | Perintah *read-only* (dipakai fitur `params`) untuk membaca tabel batas proteksi dan 43 tag alarm di EEPROM Shoto tanpa mengubah isinya. |
| **`PROTO_MODBUS_V12`** | Shoto BMS Modbus V1.2 | Protokol Modbus RTU (`FC04`, register `0x0FFF..0x1025`). Pada `SD-BA430` mandiri sempat merespons di `addr = 214` dengan pergeseran register (`ba430_shifted`). |
| **`PROTO_TBD_1363`** | TianBangda YD/T 1363.3 ASCII | Protokol alternatif berbasis teks ASCII (`~26004642...`) untuk sebagian unit Shoto. |
| **`PROTO_SMARTLI_DCDC`** | SmartLi DC/DC Modbus | Protokol alternatif Modbus (`0x0000` & `0x1030`) untuk varian Shoto SmartLi tertentu. |
| **`PROTO_AUTO`** | Deteksi Protokol Otomatis | Mode pencarian otomatis ke-4 protokol Shoto secara aman (*read-only*). |
| **`FC03` / `FC04`** | *Modbus Function Code 03 / 04* | Perintah standar Modbus RTU: `03` = baca *Holding Register*, `04` = baca *Input Register*. |
| **`Tag 0x06 = 0x0002`** | Status Normal Discharging `SD-BA430` | Indikator normal bahwa baterai sedang mengeluarkan daya (*discharging*), **bukan** bit alarm sel lemah (sempat memicu `ALA 235` palsu sebelum diperbaiki). |

---

## 3. Istilah & Singkatan Parameter Baterai (BMS & Kebijakan Bridge)

| Istilah / Singkatan | Kepanjangan / Arti | Penjelasan Sederhana |
|---|---|---|
| **`15S LFP` / `16S LFP`** | 15 / 16 Sel Seri *Lithium Iron Phosphate* ($\text{LiFePO}_4$) | Jumlah sel seri dalam satu pack baterai. Shoto `SD-BA430` adalah **`15S`** (nominal `48 V`, penuh `53,6 V`), sedangkan baterai `16S` bertegangan nominal `51,2 V` (penuh `56–57,6 V`). |
| **`V` (`bus_v`)** | Tegangan Pack / Bank (Volt) | Tegangan total baterai (penjumlahan 15 sel seri). |
| **`I` (`bus_a`)** | Arus Baterai (Ampere) | Konvensi di bridge: **positif (`+A`) = mengisi (*charging*)**, **negatif (`-A`) = mengosongkan (*discharging*)**. |
| **`SOC` / `rawSOC`** | *State of Charge* (%) | Persentase sisa energi baterai (`0–100%`). `rawSOC` adalah nilai asli BMS untuk diagnosa saat bank berstatus `INHIBIT`. |
| **`SOH`** | *State of Health* (%) | Tingkat kesehatan baterai dibanding kapasitas awalnya (mis. `99%`). |
| **`Ah` (Nominal / Remaining)** | *Ampere-hour* | Kapasitas total bank (`100 Ah` per pack → `300 Ah` untuk 3 pack paralel) dan sisa kapasitas aktual (`Remaining Capacity`). |
| **`C-Rate` (`0.3C`, `0.5C`, dll)** | Rasio Arus Terhadap Kapasitas | Kecepatan isi/kosong relatif terhadap kapasitas baterai. Untuk `100 Ah`: `0,3C = 30 A`, `0,5C = 50 A`, `1C = 100 A`. Semakin rendah `C-rate`, sel semakin dingin & awet. |
| **`CVL`** | *Charge Voltage Limit* (Volt) | Batas **tegangan pengisian maksimal** ke inverter (**`53,6 V`** = `3,573 V/sel`, aman & awet 24/7 untuk 15S LFP). |
| **`DVL`** | *Discharge Voltage Limit* (Volt) | Batas **tegangan pengosongan minimal** ke inverter (**`46,0 V`** = `3,067 V/sel`). |
| **`CCL`** | *Charge Current Limit* (Ampere) | Batas **arus pengisian maksimal** ke inverter (`45 A` 1 pack, `60 A` 2 pack, **`90 A`** 3–4 pack). |
| **`DCL`** | *Discharge Current Limit* (Ampere) | Batas **arus pengosongan maksimal** ke inverter (`80 A` 1 pack, `100 A` 2 pack, **`120 A`** *capped* 3–4 pack mengikuti rating `IHF6200`). |
| **`mV` & `dV` (`delta_V`)** | Milivolt & Selisih Tegangan | `1000 mV = 1 V`. **`dV` sel** = selisih sel tertinggi vs terendah dalam 1 pack (ideal `< 30 mV`). **`delta_V` bank** = selisih tegangan antarbaterai paralel (batas safety bridge: `<= 1,0 V`). |
| **`OV` / `UV`** | *Over Voltage* / *Under Voltage* | Tegangan berlebih (`Cell OV` / `Pack OV`) atau tegangan terlalu rendah (`Cell UV` / `Pack UV`). |
| **`OT` / `UT`** | *Over Temperature* / *Under Temperature* | Suhu terlalu panas atau terlalu dingin. |
| **`OC` (`Chg OC` / `Disg OC`)** | *Over Current* (Charge / Discharge) | Arus pengisian atau pengosongan melebihi ambang aman. |
| **`Prot` / `Warn` / `Rec`** | *Protection* / *Warning* / *Recovery* | *Warning* = peringatan dini; *Protection* = proteksi aktif (memutus MOSFET); *Recovery* = titik pulih setelah proteksi/alarm aktif. |
| **`Cut-off Voltage`** | Tegangan Pemutus Akhir | Titik tegangan terbawah di mana baterai atau inverter menghentikan pengosongan total agar sel tidak rusak (*over-discharge*). |
| **`Constant / Bulk / Boost Charging V`** | Tegangan Pengisian Utama | Tegangan target saat fase pengisian aktif (*Constant Voltage / Absorption*). |
| **`Float Charging V`** | Tegangan Pengisian Mengambang | Tegangan pemeliharaan setelah baterai penuh agar siap pakai tanpa *overcharge*. |
| **`Equalization`** | Penyeimbangan Tegangan Tinggi | Mode khusus aki basah (*lead-acid*) dengan tegangan tinggi; **wajib `Disable`** pada baterai LFP. |
| **`Top-Balancing`** | Penyeimbangan Sel di Puncak | Penyeimbangan tegangan ke-15 sel saat mendekati penuh (`~53,6 V`, arus mendekati `0 A`) sekaligus kalibrasi `SOC 100%`. |
| **`Cycles`** | Jumlah Siklus | Total hitungan siklus isi-kosong penuh yang telah dialami baterai. |
| **`MOSFET` (`Chg/Disg MOS`)** | Saklar Elektronik Daya di BMS | Saklar semikonduktor internal BMS yang membuka/menutup jalur arus charge dan discharge. |
| **`EEPROM`** | *Electrically Erasable Programmable ROM* | Memori non-volatil (tersimpan meski daya mati) untuk menyimpan parameter internal BMS maupun inverter. |

---

## 4. Istilah & Singkatan Parameter Inverter Samoto (`IHF6200` / `HPVINV02`, `ILS3600`, & `solar.siseli.com`)

| Istilah / Singkatan | Arti & Penjelasan |
|---|---|
| **`IHF6200` / `HPVINV02` v`40.09`** | Tipe inverter hybrid 6,2 kW (`HPVINV02` adalah kode profil firmware internal yang tampil di `solar.siseli.com`). |
| **`ILS3600`** | Tipe inverter hybrid Samoto 3,6 kW (memakai menu terpisah `23–31`: `BRU`, `BTT`, `BnS`, `BPO`). |
| **`Solar Plug-RWB1-06`** | Dongle WiFi port RS232 yang mengirim telemetri inverter ke cloud **`solar.siseli.com`**. |
| **Mode Kerja (`SBU`, `SUB`, `UTL`, `SOL`, `LBU`)** | Prioritas sumber listrik beban: **`SBU`** = *Solar → Battery → Utility (PLN)*; **`SUB`** = *Solar → Utility → Battery*; **`UTL`** = PLN utama; **`SOL`** = *Solar First*; **`LBU`** = *Load → Battery → Utility*. |
| **Prioritas Charge (`OSO`, `SNU`, `CSO`, `CUB`)** | Prioritas pengisian baterai: **`OSO`** = *Only Solar*; **`SNU`** = *Solar and Utility*; **`CSO`** = *Solar First*; **`CUB`** = PLN & Solar. |
| **Menu `05` (`PYL`, `USE`, `LIA`, `LIb`, `LIC`, `AGn`, `FLd`)** | Pilihan tipe baterai pada `IHF6200`: **`PYL`** = Pylontech BMS (wajib untuk bridge ini); **`USE`** = pengaturan tegangan manual tanpa protokol Pylontech; **`LIA`/`LIb`/`LIC`** = profil lithium lain; **`AGn`/`FLd`** = aki AGM / basah (*Flooded*). |
| **`BMS Communication Control Function`** | Saklar aktif/nonaktif kendali BMS di inverter (`Open` = patuh pada BMS; `Close` = abaikan kendali BMS dan pakai setelan manual). |
| **`BMS Communication Normal`** | Status koneksi data antara inverter dan BMS/bridge (`Yes` / `No`). |
| **`Return To Mains Mode Voltage`** *(Menu 12)* | Ambang **tegangan** baterai untuk pindah otomatis dari Baterai → **PLN** (aktif saat mode manual atau sebagai fallback). |
| **`Return To Battery Mode Voltage`** *(Menu 13)* | Ambang **tegangan** baterai untuk kembali dari PLN → **Baterai** setelah terisi kembali. |
| **`BMS Returns To Mains Mode SOC`** | Ambang **persentase `SOC`** untuk pindah otomatis dari Baterai → **PLN** saat komunikasi BMS aktif. |
| **`BMS Returns To Battery Mode SOC`** | Ambang **persentase `SOC`** untuk kembali dari PLN → **Baterai** saat komunikasi BMS aktif (terakhir terekam `90%`). |
| **`BMS Low Power SOC`** | Batas `SOC` peringatan/kunci baterai sangat rendah saat BMS aktif (tercatat `20%`). |
| **`BMS Automatically Starts SOC After Low`** | Batas `SOC` agar inverter otomatis menyala kembali setelah shutdown baterai lemah (tercatat `50%`). |
| **`Low Electric Lock Voltage`** | Tegangan kunci proteksi bawah inverter agar tidak menyala saat baterai terlalu kosong (tercatat `42 V`). |
| **`Parallel Mode Turn Off Voltage / SOC`** | Batas tegangan / SOC untuk mematikan operasi paralel antar-inverter (pengaturan sisi inverter, bukan EEPROM Shoto). |
| **`Dual Output Mode` (`Second Output Battery V/SOC`)** | Fitur output AC kedua (*smart load*) pada `IHF6200` yang memutus beban sekunder di bawah tegangan/SOC tertentu. |
| **`Battery Mode` vs `Mains / Bypass Mode`** | **`Battery Mode`** = beban disuplai dari baterai/PV lewat inverter; **`Mains/Bypass Mode`** = beban dialihkan ke PLN. |
| **Menu `ILS3600` (`BRU`, `BTT`, `BnS`, `BSU`, `STG`, `STB`, `BSR`, `LOU`, `BPO`)** | Menu `ILS3600`: `23 BRU` (*Back to Grid V*), `24 BTT` (*Battery Type*), `25 BnS` (*BMS Enable*), `26 BSU` (*BMS Low Power SOC*), `27 STG` (*SOC to Grid*), `28 STB` (*SOC to Battery*), `29 BSR` (*SOC Recovery*), `30 LOU` (*Low Cut-off V*), `31 BPO` (*BMS Protocol Index*). |
| **`ALA 235` / `ALA 245`** | Kode alarm inverter Samoto: **`ALA 235`** = *Battery Low Voltage / Cell UV*; **`ALA 245`** = *BMS Communication Fault*. |

---

## 5. Perangkat Keras, Pengkabelan & Sistem Tertanam (Hardware & Firmware)

| Istilah / Singkatan | Arti & Penjelasan |
|---|---|
| **`ESP32` (`WROOM-32` / `ESP32-S3`)** | Mikrokontroler otak bridge (2 inti prosesor / *dual-core*): **Core 0** khusus membaca baterai & menghitung keamanan; **Core 1** khusus menjawab inverter secara *real-time* & melayani Serial Monitor. |
| **`FreeRTOS`** | Sistem operasi *real-time* di ESP32 agar `bat_poll_task` (Core 0) dan `inv_bridge_task` (Core 1) berjalan paralel tanpa saling menahan. |
| **`Mutex` (`g_snap_mutex`)** | Kunci antrean memori (*Mutual Exclusion*) agar Core 0 dan Core 1 tidak mengakses data snapshot bank secara bersamaan. |
| **`WDT` (*Task Watchdog Timer*)** | Pengawas otomatis (10 detik): jika tugas macet/hang, ESP32 me-restart otomatis agar tidak mengirim data beku ke inverter. |
| **`UART`** | Jalur komunikasi serial TX (kirim) dan RX (terima) pada ESP32 (`UART1` sisi inverter, `UART2` sisi baterai). |
| **`RS485` (`A+`, `B-`)** | Standar komunikasi diferensial 2-kawat (*half-duplex*) yang tahan jarak jauh dan gangguan listrik (*EMI*). |
| **`RS232`** | Standar komunikasi serial pada port COM/WiFi inverter (berbeda pin, level tegangan, dan fungsi dari RS485 BMS). |
| **`XY-K485` vs `Waveshare TTL-RS485 (C)`** | Modul konverter `UART` ↔ `RS485` dengan *auto-direction*. `XY-K485` belum terisolasi (*non-isolated*), sedangkan Waveshare (C) memiliki isolasi listrik `2500 Vrms` (direkomendasikan untuk instalasi panel permanen). |
| **`Autonics SCM-US48I`** | Konverter USB ↔ RS485 terisolasi untuk komputer/Mac saat riset & pemindaian protokol. |
| **`RJ45 T568B`** | Urutan warna standar kabel LAN 8-pin. Pada Shoto `15SBA430`: **Pin 1 (`Putih-Oranye`) = `RS485 A+`**, **Pin 2 (`Oranye`) = `RS485 B-`**, **Pin 7 (`Putih-Coklat`) = `CAN H`**, **Pin 8 (`Coklat`) = `CAN L`**. Pada port BMS Samoto: **Pin 2 = `A+`**, **Pin 1 = `B-`**. |
| **`Cascade` (`RS485A` vs `RS485B`)** | Penyambungan berantai antarbaterai sesuai MOP resmi Shoto: `Bridge → #1 RS485A`, `#1 RS485B → #2 RS485A`, `#2 RS485B → #3 RS485A`, dan ujung terakhir `#3 RS485B` dipasang Endloop. |
| **`Endloop 120 Ω`** | Plug RJ45 berisi resistor `120 Ohm` di **Pin 7 ↔ Pin 8** (`CAN H ↔ CAN L`, Pin 1–6 kosong) pada `RS485B` pack terakhir agar penomoran alamat otomatis (`addr 1, 2, 3`) berjalan tanpa collision. |
| **`Read-Only`** | Mode baca saja: ESP32 hanya membaca data baterai dan **tidak pernah menulis/mengubah** EEPROM BMS Shoto. |

---

## 6. Arsitektur Pengaman & Diagnostik Firmware Bridge

| Istilah / Singkatan | Arti & Penjelasan |
|---|---|
| **`MODE_MONITOR_ONLY` vs `MODE_BRIDGE_ACTIVE`** | `MONITOR_ONLY` = hanya mengintip & mencatat data tanpa membalas inverter; `BRIDGE_ACTIVE` = aktif menjawab inverter. |
| **`Dynamic Pack Discovery` (`Hot-Plug` / `Hot-Unplug`)** | Deteksi otomatis jumlah baterai (`1–4` pack) saat boot maupun saat pack ditambah/dilepas live, otomatis men-skala `Ah`, `CCL`, `DCL`, dan barcode `60H` tanpa upload ulang. |
| **`Fail-Closed Safety`** | Prinsip keamanan gagal-aman: bila data rusak, baterai putus, kedaluwarsa, atau proteksi aktif, bridge wajib menutup izin arus (`CCL = 0 A`, `DCL = 0 A`, `INHIBIT`) alih-alih menebak angka. |
| **`BankValid` (`YES` vs `INHIBIT`)** | `YES` = semua syarat keamanan terpenuhi dan bank aktif normal; `INHIBIT` = bank ditahan aman (`0 A`). |
| **`InhibitReason`** | Penyebab spesifik `INHIBIT`: **`NONE`** (normal), **`NO_PACKS`** (0 baterai online), **`POLICY`** (policy invalid), **`CRITICAL`** (proteksi kritis aktif), **`STALE`** (data kedaluwarsa `> 9 detik`), **`INCOMPLETE`** (data pack tidak lengkap), **`LIMITS`** (`CVL <= DVL`), **`DELTA_V`** (selisih tegangan antarbaterai `> 1,0 V`). |
| **`POLICY_FRESHNESS_MS` & `age`** | Batas usia data (`9000 ms` / 9 detik). `age` adalah umur sampel terakhir dari tiap pack (`ms`). |
| **`dvRetry` (`ok/fail`)** | Penghitung konfirmasi baca ulang 1× saat `INHIBIT_DELTA_V`: angka pertama = pulih dari skew sesaat, angka kedua = selisih tegangan nyata bertahan (masuk `INHIBIT`). |
| **`peakDV` & `peakAge`** | Selisih tegangan antarbaterai tertinggi dan umur data terlama sejak ESP32 menyala. |
| **`mtxMiss`** | Jumlah kegagalan mengambil kunci memori (`g_snap_mutex`) di task inverter (normalnya `0`). |
| **`PYL_FULL` vs `SCAN_61H_ONLY`** | Deteksi di perintah `diag`: **`PYL_FULL`** = inverter terkunci penuh (`63H + 61H + 62H`, mode `PYL`); **`SCAN_61H_ONLY`** = inverter hanya memindai `61H` (mode `USE` atau `LIA`). |

---

## 7. Daftar Perintah Diagnostik (Serial Monitor ESP32 & Tool PC)

| Perintah | Lokasi | Fungsi |
|---|---|---|
| **`status`** | Serial Monitor ESP32 (`115200`) | Menampilkan ringkasan protokol, status bank (`V`, `I`, `SOC`, `CVL/DVL`, `CCL/DCL`), baris `Diag:`, dan status tiap `Pack #1..#4`. |
| **`diag`** | Serial Monitor ESP32 | Menampilkan diagnosa rinci: counter `inhibit` & penyebabnya, `dvRetry`, `peakDV/peakAge`, mode inverter (`PYL_FULL` vs `SCAN_61H_ONLY`), jumlah frame `60H–63H`, serta TX terakhir `63H/62H/61H`. |
| **`packs`** | Serial Monitor ESP32 | Status ringkas slot `#1..#4` (`ONLINE`/`OFFLINE`, tegangan, arus, SOC, `miss` counter, dan `age`). |
| **`cells`** / **`celllog`** | Serial Monitor ESP32 | `cells` mencetak tegangan 15 sel semua pack saat itu; `celllog` toggle log rincian sel otomatis tiap 5 detik. |
| **`rescan`** / **`scan`** | Serial Monitor ESP32 | `rescan` memindai ulang slot `addr 1..4` seketika; `scan` memicu ulang auto-detect ke-4 protokol Shoto. |
| **`raw`** / **`batraw`** | Serial Monitor ESP32 | Toggle tampilan frame hex mentah UART inverter (`raw`) atau RS485 baterai (`batraw`). |
| **`params`** | Serial Monitor ESP32 | Audit *read-only* parameter EEPROM BMS Shoto (`cmd 0x06..0x50` & `0x43`) tanpa mengubah isi BMS. |
| **`help`** | Serial Monitor ESP32 | Menampilkan daftar semua perintah CLI ESP32 yang tersedia. |
| **`serial_lab.py`** (`autodetect`, `probe`, `capture`, `decode`, `esp-log --csv`, `esp-log --params`) | Terminal PC ([`tools/serial_lab.py`](tools/serial_lab.py)) | Utilitas PC untuk mendeteksi protokol Shoto, merekam log Serial ESP32 ke file `.log`, mengekspor data 15 sel ke `.csv`, dan menyimpan hasil audit `params`. |
