# IHF6200 berpindah ke PLN pada SOC 99–100% dalam mode PYL

Pada 27 November 2026 operator melaporkan bahwa IHF6200 berpindah ke PLN dengan Menu 05 di `PYL`. LCD, Siseli, dan Home Assistant (HA) sama-sama menampilkan 99–100%. Setelah operator memilih `USE`, inverter kembali ke kondisi yang disebut “VP”. Waktu transfer, daya PV/beban, kode inverter, dan frame pada detik transfer belum tersedia. Istilah “VP” dipertahankan sesuai laporan operator.

Pada unit ini, capture terdahulu menunjukkan bahwa dalam mode PYL inverter meminta SOC melalui `61H`, alarm melalui `62H`, serta batas arus dan izin melalui `63H`. Dalam uji USE terdahulu, inverter memindai port tanpa meminta `62H/63H`. Mengganti Menu 05 sekaligus mengubah masukan yang digunakan inverter untuk mengambil keputusan. Bridge menjawab permintaan data dan batas operasi. IHF6200 yang memilih sumber beban. [Riwayat commissioning IHF6200](ISSUE_REPORT_2026-11-24.md)

## Apa yang dikirim bridge pada 99–100%

Uji host memakai frame TLV 140 byte dari [fixture SD-BA430](../../tests/bridge_test.cpp), tiga alamat pack, tegangan pack 53,40 V, sel dasar 3560 mV, dan batas bank 90/120 A. SOC ketiga pack diatur ke 99,00%, 99,49%, 99,50%, lalu 100,00%. Untuk tiap SOC, satu nilai sel pack pertama divariasikan dan checksum dihitung ulang. Frame melewati parser, agregasi bank dan responder Pylontech. Tabel menunjukkan payload alarm dan byte izin hasil dekode untuk revisi `8d720ef`, dengan kondisi sel bertahan hingga publikasi.

| Satu sel pada pack pertama | SOC pada 61H | Payload 62H | Izin 63H | Batas charge / discharge |
|---|---|---|---|---|
| 3560 mV, seluruh sel normal | 99 atau 100% | `00000000` | `0xC0` | 90 / 120 A |
| 3650 mV, peringatan tegangan sel tinggi | 99 atau 100% | `20000000` | `0x40` | 0 / 120 A |
| 3750 mV, proteksi tegangan sel tinggi | 99 atau 100% | `20002000` | `0x40` | 0 / 120 A |
| 2500 mV, proteksi tegangan sel rendah | 99 atau 100% | `10001000` | `0x80` | 90 / 0 A |

SOC bank merupakan rata-rata berbobot kapasitas. Responder membulatkan SOC valid ke persen utuh. Pada 99,49% ia mengirim 99 dan pada 99,50% ia mengirim 100. Pada sel normal, kedua angka menghasilkan frame izin dan alarm yang sama. Batas arus adalah plafon. Arus charge aktual dapat turun ke 0 A meski CCL masih 90 A. [Parser, agregasi, dan responder](../../firmware/shoto_samoto_bridge/bridge_protocols.hpp)

**Revisi firmware menentukan diagnosis.** Dalam uji identik pada `1eb3ccf`, di 3650 mV bridge mengirim peringatan dengan izin `0xC0`. Di 3750 mV atau 2500 mV bank menjadi invalid, `61H` mengirim 0%, `63H` menjadi `0x00`, dan bit System Fault pada `62H` aktif. Revisi saat ini memeriksa ulang pembatasan pack setelah 150 ms dan membedakan arah charge/discharge. Adanya `protRetry=` pada keluaran Serial `diag` merupakan petunjuk bahwa build mendukung pemeriksaan ulang ini. Versi yang berjalan saat insiden belum teridentifikasi.

## Bukti yang membedakan penyebab transfer

1. **Mulai dari sel tertinggi menjelang akhir charge.** Satu sel dapat mencapai ambang lebih dulu daripada 14 sel lain meski tegangan total masih di bawah CVL 53,6 V. Bila `62H=20000000/20002000` dan `63H=0x40` mendahului PLN, bridge telah membatasi pengisian, sementara discharge masih diizinkan. Periksa apakah Siseli menerima kedua izin tersebut dan mencatat alarm atau perubahan mode saat itu. Reaksi IHF6200 terhadap kombinasi ini belum terukur. Audit lama pack alamat 1 mencatat proteksi OV internal 3,700 V dan pemulihan 3,550 V. Itu parameter fisik pada satu pack, bukan bukti proteksi saat transfer. [Audit EEPROM pack 1](../../data/shoto_params.log)
2. **Periksa apakah izin discharge hilang.** `DCL=0 / 0x80` dengan bank valid mengarahkan pemeriksaan ke sel/pack terendah dan flag UV. `Bank Valid=Off`, SOC terkirim 0%, `0x00` dan System Fault mengarahkan pemeriksaan ke INHIBIT. Bandingkan kenaikan counter inhibit dengan waktu kejadian, alasan terakhir, sel min/max, selisih tegangan antar-pack dan umur sampel. Tanpa telemetri, responder diam. Insiden sebelumnya dengan satu sampel sekitar 2250 mV dan Error 04 adalah pembanding historis. Operator belum melaporkan Error 04 pada kasus SOC tinggi ini.
3. **Bila izin dan komunikasi tetap normal sebelum dan saat transfer**, cocokkan mode keluaran aktual di Siseli/LCD dengan prioritas sumber, PV masuk (W), beban (W), tegangan dan arus baterai, serta status bypass/fault. Baca nilai aktif Program 12/13 (kembali ke PLN/kembali ke baterai) beserta ambang SOC BMS di LCD/Siseli. Nilai historis 90% adalah ambang *kembali ke baterai* dan tidak menjelaskan transfer awal ke PLN. Saat PLN tersedia, arus charge 0 A atau ikon AC masuk saja belum menunjukkan bahwa PLN memasok beban.

## Rekaman minimum saat kejadian

Simpan histori `shoto_ihf6200` sekitar dua menit sebelum dan sesudah perubahan sumber, dengan jam WIB. Ambil data ketika Menu 05 masih PYL agar perubahan ke USE tidak menutupi urutan kejadian.

| Sumber | Data yang perlu dicocokkan pada waktu yang sama |
|---|---|
| LCD / Siseli | Waktu pertama *Mains/Bypass*, mode/flow sumber beban, kode status/error, PV dan beban dalam watt, `BMS Communication Normal`, `BMS Current SOC`, `Battery Capacity`, allow charge/discharge, setelan prioritas dan ambang transfer |
| HA View 1/2/4 | Bank Valid, SOC dan Raw SOC, CCL/DCL, izin charge/discharge, sel min/max beserta pack dan nomor sel, warn/prot per pack, penghitung inhibit, umur TX serta uptime |
| Serial ESP32 | `diag`, `status`, `cells`. Catat `[DIAG TX61/62/63]` beserta umur masing-masing, perubahan penghitung balasan, `[RETRY]`, `[INHIBIT]` dan `protRetry` jika tersedia |

HA dapat tetap menampilkan SOC mentah saat bank invalid, sementara `61H` bernilai 0% jika bridge masih memiliki telemetri. Label `PYL_FULL` dan alasan inhibit terakhir berasal dari riwayat penghitung. Periksa frame yang masih segar dan perubahan penghitung. Sampel yang sudah pulih atau jeda publikasi HA dapat menyembunyikan transisi singkat. [Serial diag](../../firmware/shoto_samoto_bridge/shoto_samoto_bridge.ino) · [Payload HA](../../firmware/shoto_samoto_bridge/ha_mqtt_codec.hpp)
