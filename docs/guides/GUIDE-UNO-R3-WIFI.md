# Shoto–Samoto dengan UNO R3 + ESP8266

UNO R3 WiFi tipe RobotDyn bisa dipakai untuk uji bridge RS485 antara Shoto SD-BA430 dan Samoto IHF6200. Board ini mempunyai dua prosesor. ATmega328P berkomunikasi dengan Shoto, ESP8266 menjawab polling Samoto, dan keduanya bertukar data melalui I²C. Kedua port RS485 memakai UART hardware pada 9600 8N1. DIP mengatur rute USB/UART, sedangkan komunikasi antarprosesor pada rancangan ini memakai kabel I²C melalui level shifter.

Versi ini memakai decoder SD-BA430, agregasi 1–4 pack, konfigurasi batas 15S LFP dan responder Pylontech yang juga dipakai ESP32. Polling Shoto hanya mengirim permintaan telemetri `01H` ke alamat 1–4. ESP8266 memvalidasi checksum dan alamat, memakai ulang pemeriksaan proteksi per arah, serta memblokir izin saat snapshot kedaluwarsa. Di boot ketika telemetri belum tersedia, responder diam. Versi ini mencakup bridge PYL dan log bank TX-only. MQTT, OTA, menu Serial CLI, audit parameter, dan dukungan protokol Shoto lain tersedia pada firmware ESP32. Hasil uji masih pada tahap build dan simulasi.

## Sketch dan upload

1. Lepas kedua transceiver dari pin TTL selama flashing agar CH340 menjadi satu-satunya pengirim ke pin RX target. Matikan daya saat mengubah DIP atau kabel.
2. Buka [uno_wifi_shoto.ino](../../firmware/uno_wifi_bridge/uno_wifi_shoto/uno_wifi_shoto.ino), pilih **Arduino Uno** dan port CH340. Set DIP untuk upload ATmega lalu upload.
3. Buka [uno_wifi_samoto.ino](../../firmware/uno_wifi_bridge/uno_wifi_samoto/uno_wifi_samoto.ino). Tambahkan URL board manager `https://arduino.esp8266.com/stable/package_esp8266com_index.json` ke Arduino IDE dan instal **esp8266 by ESP8266 Community**. Pilih **Generic ESP8266 Module**, upload 115200, dan pilih ukuran flash sesuai chip board Anda. Set DIP untuk upload ESP8266. Masuk bootloader dengan mereset **ESP8266** setelah DIP 7 ON, misalnya dengan siklus daya, lalu upload melalui port CH340.
4. Matikan daya, pasang kembali transceiver dan level shifter, pindahkan semua DIP ke OFF, lalu nyalakan untuk operasi bridge.

| Mode di referensi RobotDyn | DIP ON | DIP lain |
|---|---|---|
| Upload sketch ATmega328P | 3, 4 | OFF |
| Upload sketch ESP8266 | 5, 6, 7 | OFF |
| Operasi bridge, UART terpisah | — | Semua OFF |

Pastikan posisi **ON** dan nomor sakelar mengikuti silkscreen unit fisik. DIP 1–2 akan menyambungkan UART kedua chip satu sama lain. Firmware membutuhkan UART yang independen selama operasi. Kapasitas flash dan label pin dapat berbeda antarvarian. Board referensi memiliki header `ESP Pin’s` terpisah dari header UNO. [Pinout dan tabel switch RobotDyn](https://skule.sormo.no/index.php/component/eshop/download-pdf?product_id=310) · [Boot ESP8266](https://docs.espressif.com/projects/esptool/en/latest/esp8266/advanced-topics/boot-mode-selection.html)

Folder ESP8266 berisi tiga tautan simbolik ke `config.h`, `bridge_protocols.hpp` dan `safety_core.hpp` proyek. Buka sketch dari checkout utuh di macOS/Linux. Jika salinan Windows mengubah tautannya menjadi file teks, ganti ketiganya dengan salinan header dari `firmware/shoto_samoto_bridge/` ke folder sketch ESP8266 sebelum build. Siapkan ulang salinan saat sumber berubah. Sketch ATmega bisa dibuka langsung.

## Wiring

Gunakan dua transceiver RS485 half-duplex dengan pin `DI`, `RO`, `DE` dan `/RE` untuk kontrol arah manual. Sisi TTL harus cocok dengan level I/O masing-masing chip. Gunakan catu sesuai spesifikasi transceiver. Untuk transceiver terisolasi, hubungkan GND MCU ke GND sisi TTL.

| Jalur | Pin MCU | Ke |
|---|---|---|
| Shoto TX/RX | UNO D1/TX dan D0/RX | DI dan RO transceiver Shoto, logika 5 V |
| Arah Shoto | UNO D4 | DE dan /RE yang disatukan, pull-down 10 kΩ ke GND logika |
| Samoto TX/RX | ESP TXD/GPIO1 dan RXD/GPIO3 | DI dan RO transceiver Samoto, logika 3,3 V |
| Arah Samoto | ESP GPIO16 | DE dan /RE yang disatukan, pull-down 10 kΩ ke GND logika |
| I²C SDA | UNO A4 ↔ ESP GPIO4 | Kanal SDA level shifter |
| I²C SCL | UNO A5 ↔ ESP GPIO12 | Kanal SCL level shifter |
| Log bank opsional | ESP GPIO2 | RX adapter USB–TTL 3,3 V dan GND bersama, 115200 8N1 |

Untuk level shifter I²C bidirectional, sambungkan sisi HV ke 5 V UNO, LV ke 3,3 V board, dan GND bersama. Pakai pull-up pada masing-masing sisi. GPIO2 adalah pin boot strap, jadi jalur log opsional hanya memakai input RX adapter berimpedansi tinggi. GPIO4/GPIO12 di tabel merujuk ke header ESP, sedangkan UNO D4 merujuk ke header ATmega.

Ke Shoto SD-BA430, transceiver baterai menuju **RS485A pack #1**, pin RJ45 **1 = A+**, **2 = B−**. Ke port BMS IHF6200, **2 = A+**, **1 = B−**. Pakai dua jalur RS485 terpisah. Periksa pinout dan terminator cascade bila memakai beberapa pack. [Rujukan pinout proyek](../../PROJECT_CONTEXT.md#32-pinout-rj45-shoto-15sba430--koreksi-kritis-cascade-rs485a-vs-rs485b)

## Verifikasi sebelum pemakaian

- Kompilasi target Arduino Uno dengan core AVR 1.8.7 dan Generic ESP8266 Module dengan core 3.1.2 lulus. ATmega memakai 569 dari 2048 byte SRAM statis. ESP8266 memakai 30.828 dari 80.192 byte RAM statis dan 254.080 byte kode flash. Simulasi host ESP8266 mencakup boot tanpa respons, SOC 100% pada tiga pack, konfirmasi OV, izin discharge nol pada UV, snapshot kedaluwarsa, dan checksum rusak.
- Di bench, ukur DE kedua transceiver saat power-up/reset. Pastikan jalur RS485 tenang sebelum firmware siap. Sambungkan Shoto dan cocokkan log `[BANK]` dengan jumlah pack, SOC, tegangan, CCL/DCL serta min/max sel. Simulasikan terputusnya I²C atau RS485 baterai dan verifikasi bahwa izin kedaluwarsa lalu hilang.
- Dengan IHF6200 pada Menu 05 = PYL, cocokkan respons `61H/62H/63H` di bench/commissioning. Uji minimal satu siklus pengisian sampai 99–100% dan rekam status PLN, sel min/max, CCL/DCL serta alarm. Mengganti board tidak mengubah payload pada snapshot yang sama sehingga kaitannya dengan perpindahan PLN masih perlu dibuktikan saat kejadian.
