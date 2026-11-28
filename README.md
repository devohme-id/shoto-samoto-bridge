# Shoto ↔ Samoto Bridge

Bridge ESP32 yang membaca BMS baterai Shoto dan menerjemahkan telemetrinya ke protokol Pylontech untuk inverter Samoto. Integrasi Home Assistant tersedia melalui MQTT.

## Mulai dari sini

| Jika ingin… | Buka |
|---|---|
| Memilih antara rakitan DIY dan Waveshare | [Panduan utama](docs/guides/GUIDE.md) |
| Merakit ESP32 dengan modul XY-K485 | [Panduan DIY](docs/guides/GUIDE-A-DIY.md) · [PDF cetak](docs/guides/GUIDE-A-DIY-PRINT.pdf) |
| Merakit Waveshare ESP32-S3 | [Panduan Waveshare](docs/guides/GUIDE-B-WAVESHARE.md) |
| Menguji bridge dengan UNO R3 WiFi (ATmega328P + ESP8266) | [Panduan UNO R3 WiFi](docs/guides/GUIDE-UNO-R3-WIFI.md) |
| Menyiapkan MQTT dan dashboard | [Panduan Home Assistant](docs/guides/GUIDE-HOME-ASSISTANT.md) |
| Mengukur PLN, PV, beban, dan aliran daya baterai | [Panduan monitor energi PZEM](docs/guides/GUIDE-SAMOTO-ENERGY.md) · [Diagram kedua inverter](docs/diagrams/samoto-energy-pzem.svg) |
| Mencari kode error, pinout, atau perintah serial | [Wiki offline](docs/WIKI-SHOTO-SAMOTO.html) · [Glosarium](docs/GLOSSARY.md) |
| Melihat temuan lapangan dan status verifikasi | [Konteks proyek](PROJECT_CONTEXT.md) · [Laporan troubleshooting 3-pack](docs/reports/ISSUE_REPORT_2026-11-24.md) |

> Untuk pemasangan, cek [diagram wiring](docs/diagrams/) dan [fakta hardware terverifikasi](PROJECT_CONTEXT.md#3-fakta-hardware--temuan-lapangan-terverifikasi-ground-truth). Panduan rakitan dan PDF masih memuat beberapa bagian lama.

Unduh atau clone repositori, lalu buka Wiki HTML di browser untuk memakai pencarian tanpa internet. Di Wiki, tekan `/` untuk fokus ke kolom pencarian.

## Monitor energi IHF6200 dan ILS6300

Satu [ESP32 monitoring](firmware/samoto_energy_monitor/) per inverter membaca dua PZEM-004T V3.0 100 A untuk **PLN masuk** dan **beban keluar**. Setiap meter AC menyediakan V, A, W, Hz, faktor daya, dan kWh. PZEM-017 menyediakan V, A, W, serta kWh PV jika string memenuhi batas pemasangan. Home Assistant memakai telemetri MQTT bridge Shoto untuk SOC, tegangan dan arah daya baterai.

| Inverter | Profil ESP32 | Paket Home Assistant | Dashboard |
|---|---|---|---|
| IHF6200 | [`energy_ihf6200`](firmware/samoto_energy_monitor/profiles/ihf6200.h) | [Paket IHF6200](homeassistant/packages/samoto_energy_ihf6200.yaml) | [Energi IHF6200](homeassistant/dashboards/samoto_energy_ihf6200_dashboard.yaml) |
| ILS6300 | [`energy_ils6300`](firmware/samoto_energy_monitor/profiles/ils6300.h) | [Paket ILS6300](homeassistant/packages/samoto_energy_ils6300.yaml) | [Energi ILS6300](homeassistant/dashboards/samoto_energy_ils6300_dashboard.yaml) |

Masing-masing dashboard memiliki tampilan **Langsung** dan **Energi** dengan total serta rekap harian, bulanan, dan tahunan untuk PLN, PV, dan beban. Ikuti [panduan pemasangan](docs/guides/GUIDE-SAMOTO-ENERGY.md) untuk provisioning, penambahan paket HA, dan komisioning.

**PV nonaktif pada kedua profil standar.** PZEM-017 berating 300 V DC, sedangkan input IHF6200 dapat menerima hingga 500 V DC menurut spesifikasi Samoto. Untuk kedua inverter, teknisi perlu memverifikasi Voc maksimum string pada suhu terendah dan margin terhadap rating meter sebelum pemasangan. Rating ILS6300 pada spesifikasi Samoto adalah 245 V DC. Wiring data sama pada kedua profil. Firmware lulus kompilasi untuk ESP32 Dev Module pada kedua profil. Validasi pemasangan meter dan Home Assistant berjalan dilakukan saat komisioning.

## Peta folder

| Folder | Isi |
|---|---|
| [`docs/`](docs/) | Wiki, glosarium, [panduan dan PDF](docs/guides/), [diagram PNG/SVG](docs/diagrams/), [laporan](docs/reports/), dan [rencana integrasi HA](docs/superpowers/plans/2026-11-25-home-assistant-integration.md) |
| [`firmware/`](firmware/) | [Bridge ESP32](firmware/shoto_samoto_bridge/), [monitor energi ESP32](firmware/samoto_energy_monitor/), dan konfigurasi pembaca ESPHome |
| [`homeassistant/`](homeassistant/) | Konfigurasi deployment, broker MQTT, [dashboard](homeassistant/dashboards/), [paket energi](homeassistant/packages/), otomasi, dan skrip |
| [`tools/`](tools/) | [CLI diagnostik serial](tools/serial_lab.py) |
| [`tests/`](tests/) | Pengujian host Python dan C++ |
| [`data/`](data/) | Capture serial dan log audit lapangan |
| [`reference/`](reference/) | Manual resmi dan arsip vendor |

Jalankan contoh perintah proyek dari root repositori kecuali panduan menyebut direktori lain. Untuk pengembangan, mulai dari [konteks proyek](PROJECT_CONTEXT.md) dan [aturan repositori](AGENTS.md).
