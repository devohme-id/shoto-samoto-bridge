#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace energy {

enum class Read : uint8_t { Ac, Pv, PvShunt };

inline uint8_t function(Read request) {
    return request == Read::PvShunt ? 0x03 : 0x04;
}

inline uint8_t registerCount(Read request) {
    return request == Read::Ac ? 10 : (request == Read::Pv ? 8 : 1);
}

inline uint16_t crc16(const uint8_t* bytes, size_t size) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }
    return crc;
}

inline void buildRequest(Read request, uint8_t address, uint8_t (&out)[8]) {
    out[0] = address;
    out[1] = function(request);
    out[2] = 0;
    out[3] = request == Read::PvShunt ? 3 : 0;
    out[4] = 0;
    out[5] = registerCount(request);
    const uint16_t crc = crc16(out, 6);
    out[6] = static_cast<uint8_t>(crc);
    out[7] = static_cast<uint8_t>(crc >> 8);
}

inline bool validResponse(const uint8_t* frame, size_t size, uint8_t address, Read request) {
    const uint8_t dataSize = 2 * registerCount(request);
    if (!frame || size != static_cast<size_t>(5 + dataSize) || frame[1] != function(request) ||
        frame[2] != dataSize || frame[0] == 0 || frame[0] > 0xF8 ||
        (address != 0xF8 && frame[0] != address)) {
        return false;
    }
    const uint16_t receivedCrc = frame[size - 2] | (uint16_t(frame[size - 1]) << 8);
    return receivedCrc == crc16(frame, size - 2);
}

inline uint16_t word(const uint8_t* data) {
    return (uint16_t(data[0]) << 8) | data[1];
}

inline uint32_t lowWordFirst(const uint8_t* data) {
    return word(data) | (uint32_t(word(data + 2)) << 16);
}

struct Sample {
    float voltageV = 0;
    float currentA = 0;
    float powerW = 0;
    uint32_t energyWh = 0;
    float frequencyHz = 0;
    float powerFactor = 0;
};

inline bool decodeAc(const uint8_t* frame, size_t size, uint8_t address, Sample& out) {
    if (!validResponse(frame, size, address, Read::Ac)) return false;
    const uint8_t* data = frame + 3;
    Sample sample;
    sample.voltageV = word(data) / 10.0f;
    sample.currentA = lowWordFirst(data + 2) / 1000.0f;
    sample.powerW = lowWordFirst(data + 6) / 10.0f;
    sample.energyWh = lowWordFirst(data + 10);
    sample.frequencyHz = word(data + 14) / 10.0f;
    sample.powerFactor = word(data + 16) / 100.0f;
    if (sample.voltageV < 80 || sample.voltageV > 260 || sample.currentA > 100 ||
        sample.powerW > 26000 || sample.frequencyHz < 45 || sample.frequencyHz > 65 ||
        sample.powerFactor > 1) return false;
    out = sample;
    return true;
}

inline bool is100AmpShunt(const uint8_t* frame, size_t size, uint8_t address) {
    return validResponse(frame, size, address, Read::PvShunt) && word(frame + 3) == 0;
}

inline bool decodePv(const uint8_t* frame, size_t size, uint8_t address, Sample& out) {
    if (!validResponse(frame, size, address, Read::Pv)) return false;
    const uint8_t* data = frame + 3;
    Sample sample;
    sample.voltageV = word(data) / 100.0f;
    sample.currentA = word(data + 2) / 100.0f;
    sample.powerW = lowWordFirst(data + 4) / 10.0f;
    sample.energyWh = lowWordFirst(data + 8);
    if (sample.voltageV > 300 || sample.currentA > 100 || sample.powerW > 30000) return false;
    out = sample;
    return true;
}

inline int encodeAc(const Sample& sample, char* out, size_t size) {
    return std::snprintf(out, size,
                         "{\"ok\":true,\"voltage_v\":%.1f,\"current_a\":%.3f,\"power_w\":%.1f,"
                         "\"energy_wh\":%lu,\"frequency_hz\":%.1f,\"power_factor\":%.2f}",
                         sample.voltageV, sample.currentA, sample.powerW,
                         static_cast<unsigned long>(sample.energyWh), sample.frequencyHz, sample.powerFactor);
}

inline int encodePv(const Sample& sample, char* out, size_t size) {
    return std::snprintf(out, size,
                         "{\"ok\":true,\"reason\":\"ok\",\"voltage_v\":%.2f,\"current_a\":%.2f,"
                         "\"power_w\":%.1f,\"energy_wh\":%lu}",
                         sample.voltageV, sample.currentA, sample.powerW,
                         static_cast<unsigned long>(sample.energyWh));
}

} // namespace energy
