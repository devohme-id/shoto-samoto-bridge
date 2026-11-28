#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>

#if !defined(ARDUINO_ARCH_ESP8266)
#error "Select Generic ESP8266 Module for the ESP8266 sketch"
#endif

#undef min
#undef max
#include "config.h"
#include "bridge_protocols.hpp"

constexpr uint8_t I2C_ADDRESS = 0x31;
constexpr uint8_t READ_TELEMETRY = 0xA1;
constexpr uint8_t STATUS_MARKER = 0xB1;
constexpr uint8_t STATUS_OFFSET = 0xFF;
constexpr uint8_t IN_PROGRESS = 0xFF;
constexpr uint8_t CHUNK_SIZE = 24;
constexpr uint8_t CAPTURE_SIZE = 192;
constexpr uint8_t I2C_SDA = 4;
constexpr uint8_t I2C_SCL = 12;
constexpr uint8_t RS485_DIRECTION = 16;
constexpr uint16_t I2C_POLL_MS = 10;
constexpr uint16_t I2C_REPLY_MS = 650;
constexpr uint16_t PROT_RECHECK_MS = 150;
constexpr uint16_t INTER_PACK_MS = 25;
constexpr uint16_t UART_CHAR_US = (10000000UL + INVERTER_BAUD - 1) / INVERTER_BAUD;
constexpr uint8_t PACK_COUNT = DYNAMIC_PACK_DISCOVERY ? MAX_SUPPORTED_PACKS : FIXED_PACK_COUNT;
static_assert(PACK_COUNT >= 1 && PACK_COUNT <= 4, "Expected 1 to 4 Shoto packs");
static_assert(SHOTO_PROTOCOL == PROTO_TBD_BINARY, "UNO bridge supports SD-BA430 TBD_BINARY");
static_assert(SHOTO_BAUD == 9600, "ATmega328P sketch polls at 9600 baud");
constexpr uint8_t ALL_PACKS = (1u << PACK_COUNT) - 1u;
constexpr uint8_t PACK_ADDRESSES[] = {PACK1_ADDR, PACK2_ADDR, PACK3_ADDR, PACK4_ADDR};

struct Slot {
    PackState pack;
    uint8_t misses = 0;
    bool online = false;
};

Slot slots[PACK_COUNT];
PolicyLimits limits;
DynamicLimits dynamicLimits;
Policy policy;
BankExt bank;
uint32_t oldestPublishedSample = 0;
bool bootSweep = true;
bool sweepActive = false;
bool deltaRetry = false;
uint8_t remainingSlots = 0;
uint8_t discoveryCursor = 0;
uint32_t nextSweep = 0;
uint32_t nextDiscovery = 0;
uint32_t nextPack = 0;

bool awaitingReply = false;
bool rechecking = false;
uint8_t activeSlot = 0;
uint8_t sequence = 0;
uint8_t header = 0x7C;
uint32_t submittedAt = 0;
uint32_t nextStatus = 0;
PackState suspect;

char inverterRx[256];
size_t inverterRxSize = 0;
uint32_t lastRxAt = 0;
char inverterTx[360];
size_t inverterTxSize = 0;
size_t inverterTxPos = 0;
int fifoCapacity = 0;
bool finalByteInProgress = false;
uint32_t fifoEmptyAt = 0;

static void serviceInverter();

static bool due(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

static bool readAt(uint8_t offset, uint8_t* out, uint8_t size) {
    Wire.beginTransmission(I2C_ADDRESS);
    Wire.write(offset);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom(I2C_ADDRESS, size) != size) return false;
    for (uint8_t i = 0; i < size; ++i) out[i] = Wire.read();
    return true;
}

static uint8_t onlineMask() {
    uint8_t mask = 0;
    for (uint8_t i = 0; i < PACK_COUNT; ++i) {
        if (slots[i].online) mask |= 1u << i;
    }
    return mask;
}

static void finishSweep(uint32_t now) {
    PackState active[PACK_COUNT];
    uint8_t count = 0;
    uint32_t maxAge = 0;
    for (const Slot& slot : slots) {
        if (!DYNAMIC_PACK_DISCOVERY || slot.online) {
            active[count++] = slot.pack;
            maxAge = std::max(maxAge, now - slot.pack.core.sampled_ms);
        }
    }

    compute_dynamic_caps(count, dynamicLimits, policy.charge_cap, policy.discharge_cap);
    BankExt candidate;
    aggregate_bank(active, count, now, policy, candidate);
    if (!deltaRetry && !candidate.core.valid &&
        classify_bank_inhibit(active, count, now, policy) == INHIBIT_DELTA_V) {
        remainingSlots = DYNAMIC_PACK_DISCOVERY ? onlineMask() : ALL_PACKS;
        deltaRetry = true;
        return;
    }
    bank = candidate;
    oldestPublishedSample = now - maxAge;
    sweepActive = false;
    nextSweep = now + POLL_INTERVAL_MS;

    char log[128];
    const int length = std::snprintf(
        log, sizeof(log), "[BANK] n=%u valid=%u SOC=%.1f V=%.2f CCL=%.1f DCL=%.1f min=%u max=%u warn=%04X prot=%04X\n",
        static_cast<unsigned>(bank.pack_count), static_cast<unsigned>(bank.core.valid),
        bank.raw_soc, bank.bus_v, bank.core.ccl, bank.core.dcl,
        static_cast<unsigned>(bank.min_cell_mv), static_cast<unsigned>(bank.max_cell_mv),
        static_cast<unsigned>(bank.warn_flags), static_cast<unsigned>(bank.prot_flags));
    if (length > 0 && length < static_cast<int>(sizeof(log)) && length < Serial1.availableForWrite()) {
        Serial1.write(reinterpret_cast<const uint8_t*>(log), static_cast<size_t>(length));
    }
}

static void finishPack(const PackState* pack, uint32_t now) {
    Slot& slot = slots[activeSlot];
    if (pack) {
        slot.pack = *pack;
        slot.misses = 0;
        slot.online = true;
    } else if (!deltaRetry && slot.online && ++slot.misses >= PACK_OFFLINE_MISSES) {
        slot.online = false;
        slot.misses = 0;
    }
    remainingSlots &= ~(1u << activeSlot);
    rechecking = false;
    header = 0x7C;
    nextPack = now + INTER_PACK_MS;
}

static void finishAttempt(const PackState* pack, uint32_t now) {
    awaitingReply = false;
    if (rechecking) {
        finishPack(pack ? pack : &suspect, now);
    } else if (pack && pack_needs_prot_recheck(*pack)) {
        suspect = *pack;
        rechecking = true;
        nextPack = now + PROT_RECHECK_MS;
    } else if (!pack && header == 0x7C) {
        header = 0x7E;
        nextPack = now + INTER_PACK_MS;
    } else {
        finishPack(pack, now);
    }
}

static void submitPoll(uint32_t now) {
    const uint8_t command[] = {READ_TELEMETRY, ++sequence, PACK_ADDRESSES[activeSlot], header};
    Wire.beginTransmission(I2C_ADDRESS);
    Wire.write(command, sizeof(command));
    if (Wire.endTransmission() != 0) {
        finishAttempt(nullptr, now);
        return;
    }
    awaitingReply = true;
    submittedAt = now;
    nextStatus = now + I2C_POLL_MS;
}

static void collectReply(uint32_t now) {
    if (!due(now, nextStatus)) return;
    nextStatus = now + I2C_POLL_MS;
    if (now - submittedAt >= I2C_REPLY_MS) {
        finishAttempt(nullptr, now);
        return;
    }

    uint8_t status[5];
    if (!readAt(STATUS_OFFSET, status, sizeof(status)) || status[0] != STATUS_MARKER ||
        status[1] != sequence || status[2] != PACK_ADDRESSES[activeSlot] ||
        status[3] != header || status[4] == IN_PROGRESS) return;

    uint8_t data[CAPTURE_SIZE];
    const uint8_t size = status[4];
    if (size < 140 || size > sizeof(data)) {
        finishAttempt(nullptr, now);
        return;
    }
    for (uint8_t offset = 0; offset < size; offset += CHUNK_SIZE) {
        const uint8_t chunk = std::min(CHUNK_SIZE, static_cast<uint8_t>(size - offset));
        if (!readAt(offset, data + offset, chunk)) {
            finishAttempt(nullptr, now);
            return;
        }
        serviceInverter();
    }

    PackState sample;
    // Accept only the verified SD-BA430 Format B frame from this address.
    for (size_t i = 0; i + 140 <= size; ++i) {
        const uint8_t* p = data + i;
        if (p[0] == 0x7E && p[1] == PACK_ADDRESSES[activeSlot] && p[2] == 0x01 &&
            p[3] == 0x86 && parse_tbd_binary_resp(p, 140, p[1], submittedAt, limits, sample) &&
            sample.ext.cell_count == 15) {
            finishAttempt(&sample, now);
            return;
        }
    }
    finishAttempt(nullptr, now);
}

static void pollShoto(uint32_t now) {
    if (awaitingReply) {
        collectReply(now);
        return;
    }
    if (!sweepActive) {
        if (!due(now, nextSweep)) return;
        sweepActive = true;
        deltaRetry = false;
        remainingSlots = (bootSweep || !DYNAMIC_PACK_DISCOVERY) ? ALL_PACKS : onlineMask();
        if (!bootSweep && DYNAMIC_PACK_DISCOVERY && due(now, nextDiscovery)) {
            nextDiscovery = now + DISCOVERY_INTERVAL_MS;
            for (uint8_t i = 0; i < PACK_COUNT; ++i) {
                const uint8_t slot = discoveryCursor;
                discoveryCursor = (discoveryCursor + 1) % PACK_COUNT;
                if (!slots[slot].online) {
                    remainingSlots |= 1u << slot;
                    break;
                }
            }
        }
        bootSweep = false;
        nextPack = now;
    }
    if (!due(now, nextPack)) return;
    if (remainingSlots == 0) {
        finishSweep(now);
        return;
    }
    activeSlot = 0;
    while ((remainingSlots & (1u << activeSlot)) == 0) ++activeSlot;
    submitPoll(now);
}

static void serviceInverter() {
    if (inverterTxSize > 0) {
        const int free = Serial.availableForWrite();
        if (inverterTxPos < inverterTxSize && free > 1) {
            const size_t count = std::min(inverterTxSize - inverterTxPos, static_cast<size_t>(free - 1));
            inverterTxPos += Serial.write(reinterpret_cast<uint8_t*>(inverterTx) + inverterTxPos, count);
        } else if (inverterTxPos == inverterTxSize && free == fifoCapacity) {
            // The final byte can still be in the shift register after the FIFO empties.
            if (!finalByteInProgress) {
                fifoEmptyAt = micros();
                finalByteInProgress = true;
            } else if (micros() - fifoEmptyAt >= UART_CHAR_US) {
                digitalWrite(RS485_DIRECTION, LOW);
                inverterTxSize = 0;
            }
        }
        return;
    }

    const uint32_t now = millis();
    if (now - lastRxAt > 65 || Serial.hasOverrun() || Serial.hasRxError()) inverterRxSize = 0;
    for (uint8_t i = 0; i < 64 && Serial.available(); ++i) {
        const char byte = static_cast<char>(Serial.read());
        lastRxAt = millis();
        if (byte == '~') inverterRxSize = 0;
        else if (inverterRxSize == 0) continue;

        inverterRx[inverterRxSize++] = byte;
        if (byte != '\r') {
            if (inverterRxSize == sizeof(inverterRx)) inverterRxSize = 0;
            continue;
        }

        if (bank.core.valid && millis() - oldestPublishedSample > policy.freshness_ms) {
            bank.core = Bank{};
        }
        if (BRIDGE_MODE == MODE_BRIDGE_ACTIVE) {
            inverterTxSize = handle_pylontech_request(
                inverterRx, inverterRxSize, bank, inverterTx, sizeof(inverterTx));
        }
        inverterRxSize = 0;
        if (inverterTxSize > 0) {
            inverterTxPos = 0;
            finalByteInProgress = false;
            digitalWrite(RS485_DIRECTION, HIGH);
            return;
        }
    }
}

void setup() {
    digitalWrite(RS485_DIRECTION, LOW);
    pinMode(RS485_DIRECTION, OUTPUT);
    Serial.begin(INVERTER_BAUD, SERIAL_8N1);
    Serial.setDebugOutput(false);
    Serial1.begin(115200);
    fifoCapacity = Serial.availableForWrite();
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    Wire.setClockStretchLimit(1000);

    limits.cvl_v = POLICY_CVL_MAX_V;
    limits.dvl_v = POLICY_DVL_MIN_V;
    limits.ccl_a = POLICY_CCL_BANK_MAX_A;
    limits.dcl_a = POLICY_DCL_BANK_MAX_A;
    dynamicLimits.ccl_1pack_a = POLICY_CCL_1PACK_A;
    dynamicLimits.dcl_1pack_a = POLICY_DCL_1PACK_A;
    dynamicLimits.ccl_per_pack_a = POLICY_CCL_PER_PACK_A;
    dynamicLimits.dcl_per_pack_a = POLICY_DCL_PER_PACK_A;
    dynamicLimits.ccl_max_a = POLICY_CCL_BANK_MAX_A;
    dynamicLimits.dcl_max_a = POLICY_DCL_BANK_MAX_A;
    policy.approved = POLICY_USER_APPROVED;
    policy.freshness_ms = POLICY_FRESHNESS_MS;
    policy.max_delta_v = POLICY_MAX_DELTA_V;
}

void loop() {
    serviceInverter();
    pollShoto(millis());
    serviceInverter();
    yield();
}
