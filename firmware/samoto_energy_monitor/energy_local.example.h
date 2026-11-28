#pragma once

// Arduino IDE: pilih satu profil. PlatformIO sudah memilih dari environment.
// #define ENERGY_SITE_IHF6200
// #define ENERGY_SITE_ILS6300

#define ENERGY_WIFI_SSID ""
#define ENERGY_WIFI_PASSWORD ""
#define ENERGY_MQTT_HOST ""
#define ENERGY_MQTT_PORT 1883
#define ENERGY_MQTT_USER ""
#define ENERGY_MQTT_PASSWORD ""

// Isi Voc desain maksimum pada suhu terendah setelah instalasi disetujui teknisi.
#ifndef ENERGY_ENABLE_PV
#define ENERGY_ENABLE_PV 0
#endif
#ifndef ENERGY_PV_DESIGN_VOC_V
#define ENERGY_PV_DESIGN_VOC_V 0
#endif

// Masing-masing UART/RS485 hanya memiliki satu meter.
#define ENERGY_GRID_ADDR 0xF8
#define ENERGY_LOAD_ADDR 0xF8
#define ENERGY_PV_ADDR 0xF8
