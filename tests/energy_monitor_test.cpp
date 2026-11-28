#include "../firmware/samoto_energy_monitor/pzem_protocol.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

using energy::Read;

template <size_t N>
void setCrc(uint8_t (&frame)[N]) {
    const uint16_t check = energy::crc16(frame, N - 2);
    frame[N - 2] = static_cast<uint8_t>(check);
    frame[N - 1] = static_cast<uint8_t>(check >> 8);
}

int main() {
    uint8_t request[8];
    const uint8_t acRead[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x70, 0x0D};
    energy::buildRequest(Read::Ac, 0x01, request);
    assert(std::memcmp(request, acRead, sizeof(request)) == 0);
    energy::buildRequest(Read::Pv, 0x01, request);
    assert(request[1] == 0x04 && request[3] == 0 && request[5] == 8);
    energy::buildRequest(Read::PvShunt, 0x01, request);
    assert(request[1] == 0x03 && request[3] == 3 && request[5] == 1);

    uint8_t ac[25] = {
        0x01, 0x04, 20,
        0x08, 0xFC, 0x11, 0x94, 0x00, 0x01, 0xF5, 0x90, 0x00, 0x01,
        0xE2, 0x40, 0x00, 0x01, 0x01, 0xF4, 0x00, 0x4E, 0x00, 0x00
    };
    setCrc(ac);
    energy::Sample sample;
    assert(energy::decodeAc(ac, sizeof(ac), 0xF8, sample));
    assert(sample.voltageV == 230.0f && sample.frequencyHz == 50.0f);
    assert(std::fabs(sample.currentA - 70.036f) < 0.001f);
    assert(std::fabs(sample.powerW - 12840.0f) < 0.01f);
    assert(std::fabs(sample.powerFactor - 0.78f) < 0.001f && sample.energyWh == 123456);
    char message[192];
    int size = energy::encodeAc(sample, message, sizeof(message));
    assert(size > 0 && size < static_cast<int>(sizeof(message)));
    assert(std::strstr(message, "\"energy_wh\":123456") && std::strstr(message, "\"power_factor\":0.78"));

    assert(!energy::decodeAc(ac, sizeof(ac) - 1, 0xF8, sample));
    assert(!energy::decodeAc(ac, sizeof(ac), 0x02, sample));
    ac[24] ^= 0x80;
    assert(!energy::decodeAc(ac, sizeof(ac), 0x01, sample));
    ac[20] = 0x64; // PF now exceeds the meter's 0–1 scale.
    ac[19] = 1;
    setCrc(ac);
    assert(!energy::decodeAc(ac, sizeof(ac), 0x01, sample));

    uint8_t pv[21] = {
        0xF8, 0x04, 16,
        0x55, 0xF0, 0x02, 0xEE, 0x40, 0x74, 0x00, 0x00,
        0xD0, 0x90, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
    };
    setCrc(pv);
    assert(energy::decodePv(pv, sizeof(pv), 0xF8, sample));
    assert(sample.voltageV == 220.0f && sample.currentA == 7.5f && sample.powerW == 1650.0f);
    assert(sample.energyWh == 118928);
    assert(energy::encodePv(sample, message, sizeof(message)) > 0);
    assert(std::strstr(message, "\"voltage_v\":220.00") && std::strstr(message, "\"energy_wh\":118928"));
    pv[3] = 0x75;
    pv[4] = 0x31; // 300.01 V
    setCrc(pv);
    assert(!energy::decodePv(pv, sizeof(pv), 0xF8, sample));

    uint8_t shunt[] = {0x01, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00};
    setCrc(shunt);
    assert(energy::is100AmpShunt(shunt, sizeof(shunt), 0xF8));
    shunt[4] = 1;
    setCrc(shunt);
    assert(!energy::is100AmpShunt(shunt, sizeof(shunt), 0xF8));
    assert(!energy::validResponse(shunt, sizeof(shunt), 0x01, Read::Pv));
    const uint8_t exceptionFrame[] = {0x01, 0x84, 0x02, 0xC2, 0xC1};
    assert(!energy::decodeAc(exceptionFrame, sizeof(exceptionFrame), 0x01, sample));
    std::cout << "PZEM read-only protocol and telemetry PASS\n";
}
