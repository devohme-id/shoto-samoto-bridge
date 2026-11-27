# Shoto ↔ Samoto Bridge

Bridge ESP32 yang membaca BMS baterai Shoto dan menerjemahkan telemetrinya ke protokol Pylontech untuk inverter Samoto. Integrasi Home Assistant tersedia melalui MQTT.

## Mulai dari sini

| Jika ingin… | Buka |
|---|---|
| Memilih antara rakitan DIY dan Waveshare | [Panduan utama](docs/guides/GUIDE.md) |
| Merakit ESP32 dengan modul XY-K485 | [Panduan DIY](docs/guides/GUIDE-A-DIY.md) · [PDF cetak](docs/guides/GUIDE-A-DIY-PRINT.pdf) |
| Merakit Waveshare ESP32-S3 | [Panduan Waveshare](docs/guides/GUIDE-B-WAVESHARE.md) |
| Menyiapkan MQTT dan dashboard | [Panduan Home Assistant](docs/guides/GUIDE-HOME-ASSISTANT.md) |
| Mencari kode error, pinout, atau perintah serial | [Wiki offline](docs/WIKI-SHOTO-SAMOTO.html) · [Glosarium](docs/GLOSSARY.md) |
| Melihat temuan lapangan dan status verifikasi | [Konteks proyek](PROJECT_CONTEXT.md) · [Laporan troubleshooting 3-pack](docs/reports/ISSUE_REPORT_2026-11-24.md) |

> Untuk pemasangan, cek [diagram wiring](docs/diagrams/) dan [fakta hardware terverifikasi](PROJECT_CONTEXT.md#3-fakta-hardware--temuan-lapangan-terverifikasi-ground-truth). Panduan rakitan dan PDF masih memuat beberapa bagian lama.

Unduh atau clone repositori, lalu buka Wiki HTML di browser untuk memakai pencarian tanpa internet. Di Wiki, tekan `/` untuk fokus ke kolom pencarian.

## Peta folder

| Folder | Isi |
|---|---|
| [`docs/`](docs/) | Wiki, glosarium, [panduan dan PDF](docs/guides/), [diagram PNG/SVG](docs/diagrams/), [laporan](docs/reports/), dan [rencana integrasi HA](docs/superpowers/plans/2026-11-25-home-assistant-integration.md) |
| [`firmware/`](firmware/) | [Sketch ESP32/PlatformIO](firmware/shoto_samoto_bridge/) dan konfigurasi pembaca ESPHome |
| [`homeassistant/`](homeassistant/) | Konfigurasi deployment, broker MQTT, [dashboard](homeassistant/dashboards/), otomasi, dan skrip |
| [`tools/`](tools/) | [CLI diagnostik serial](tools/serial_lab.py) |
| [`tests/`](tests/) | Pengujian host Python dan C++ |
| [`data/`](data/) | Capture serial dan log audit lapangan |
| [`reference/`](reference/) | Manual resmi dan arsip vendor |

Jalankan contoh perintah proyek dari root repositori kecuali panduan menyebut direktori lain. Untuk pengembangan, mulai dari [konteks proyek](PROJECT_CONTEXT.md) dan [aturan repositori](AGENTS.md).
