# Instruksi Proyek & Aturan Dokumentasi Otomatis (`AGENTS.md`)

1. **Rujukan Utama (*Single Source of Truth*):**
   Selalu baca [`PROJECT_CONTEXT.md`](PROJECT_CONTEXT.md) sebelum memodifikasi kode, konfigurasi, jaringan, atau dokumentasi proyek ini.

2. **Aturan Wajib Auto-Update Wiki & Markdown (Setiap Percakapan Penting):**
   Setiap kali percakapan menghasilkan **informasi penting** — termasuk hasil diagnosa akar masalah (*root-cause*), bukti live (`solar.siseli.com`, HA View 4, Serial `diag`/`params`), kode error atau perilaku baru inverter/BMS, perubahan arsitektur/jaringan/Mikrotik/Home Assistant, atau perubahan kode & konfigurasi — agent **WAJIB langsung memperbarui**:
   - **[`WIKI-SHOTO-SAMOTO.html`](WIKI-SHOTO-SAMOTO.html):** perbarui artikel yang relevan (atau tambahkan artikel baru lengkap dengan tautan sidebar TOC `.toc-link` ber-`data-target` cocok dengan `id` artikel, `data-keywords` yang kaya kata kunci, serta entri di Indeks A–Z `#indeks-az`). Pastikan tetap **100% offline** (nol script/CSS eksternal), HTML/JS valid (`node --check`), dan **nol kredensial dari `secrets.h`**. *(Catatan: jika menambah/mengurangi jumlah `<article class="wiki-article">`, perbarui juga batas `len(parser.article_ids)` di [`tests/test_ha_artifacts.py`](tests/test_ha_artifacts.py)).*
   - **File Markdown terkait:** perbarui [`PROJECT_CONTEXT.md`](PROJECT_CONTEXT.md) (Bagian 3/4/5/6 sesuai jenis temuan) dan panduan terkait ([`GUIDE-HOME-ASSISTANT.md`](GUIDE-HOME-ASSISTANT.md), [`GLOSSARY.md`](GLOSSARY.md), atau `ISSUE_REPORT_*.md`).

3. ***Hard Invariants* Keselamatan & Operasional:**
   - **100% Read-Only ke BMS Shoto:** Jangan pernah mengirim perintah write EEPROM ke baterai Shoto dan jangan pernah mengirim `16× 0xAA` ke `SD-BA430` / ketika protokol sudah terkunci ke `PROTO_TBD_BINARY`.
   - **Fail-Closed Safety:** Pertahankan batas 15S LFP & logika `INHIBIT` pada [`safety_core.hpp`](firmware/shoto_samoto_bridge/safety_core.hpp) dan [`bridge_protocols.hpp`](firmware/shoto_samoto_bridge/bridge_protocols.hpp).
   - **Kredensial & Git Hygiene:** Jangan pernah membaca untuk disalin ke dokumen/Wiki, memodifikasi, me-commit, atau mem-push [`secrets.h`](firmware/shoto_samoto_bridge/secrets.h) / `secrets.h` nyata maupun `config.h.example`, serta jangan merge/push git tanpa persetujuan eksplisit user.
   - **Verifikasi Wajib:** Jalankan `python3 -m unittest discover -s tests -v` setelah mengubah dokumen/artefak HA/Wiki, serta suite C++ (`tests/safety_test.cpp`, `tests/bridge_test.cpp`, `tests/ha_mqtt_test.cpp`) jika menyentuh firmware.
