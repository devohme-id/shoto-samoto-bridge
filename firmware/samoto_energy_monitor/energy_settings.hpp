#pragma once
#include <cstdint>

#if __has_include("energy_local.h")
#include "energy_local.h"
#else
#include "energy_local.example.h"
#endif

#if defined(ENERGY_SITE_IHF6200) == defined(ENERGY_SITE_ILS6300)
#error "Select exactly one ENERGY_SITE profile in energy_local.h or the build flags"
#endif
#if defined(ENERGY_SITE_IHF6200)
#include "profiles/ihf6200.h"
#else
#include "profiles/ils6300.h"
#endif

static_assert(ENERGY_ENABLE_PV == 0 || ENERGY_ENABLE_PV == 1, "ENERGY_ENABLE_PV must be 0 or 1");
static_assert(!ENERGY_ENABLE_PV ||
              (ENERGY_PV_DESIGN_VOC_V > 0 && ENERGY_PV_DESIGN_VOC_V < 300 &&
               ENERGY_PV_DESIGN_VOC_V <= kInverterPvMaxV),
              "PV requires verified design Voc below PZEM-017 300 V and within inverter rating");
static_assert(ENERGY_GRID_ADDR >= 1 && ENERGY_GRID_ADDR <= 0xF8 &&
              ENERGY_LOAD_ADDR >= 1 && ENERGY_LOAD_ADDR <= 0xF8 &&
              ENERGY_PV_ADDR >= 1 && ENERGY_PV_ADDR <= 0xF8,
              "PZEM address must be a unicast address or the single-meter general address");

constexpr int8_t kGridRx = 16;
constexpr int8_t kGridTx = 17;
constexpr int8_t kLoadRx = 27;
constexpr int8_t kLoadTx = 26;
constexpr int8_t kPvRx = 32;
constexpr int8_t kPvTx = 33;
constexpr int8_t kPvDirection = 23;
