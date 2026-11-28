#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include "energy_settings.hpp"
#include "pzem_protocol.hpp"

namespace {

constexpr unsigned kBaud = 9600;
constexpr uint32_t kPollMs = 5000;
constexpr uint32_t kReconnectMs = 5000;
constexpr uint32_t kReplyTimeoutMs = 220;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
char onlineTopic[80];
uint32_t lastMqttAttempt = 0;
uint32_t lastPoll = 0;
bool pollImmediately = true;

size_t readPzem(HardwareSerial& port, energy::Read request, uint8_t address,
                 uint8_t (&frame)[25], int8_t direction = -1) {
    for (uint8_t i = 0; i < 64 && port.available(); ++i) port.read();
    if (port.available()) return 0;

    uint8_t tx[8];
    energy::buildRequest(request, address, tx);
    if (direction >= 0) {
        digitalWrite(direction, HIGH);
        delayMicroseconds(100);
    }
    const size_t written = port.write(tx, sizeof(tx));
    port.flush();
    if (direction >= 0) digitalWrite(direction, LOW);
    if (written != sizeof(tx)) return 0;

    const size_t expected = 5u + 2u * energy::registerCount(request);
    const uint32_t started = millis();
    size_t size = 0;
    while (millis() - started < kReplyTimeoutMs && size < expected) {
        const int byte = port.read();
        if (byte < 0) {
            delay(1);
            continue;
        }
        frame[size++] = static_cast<uint8_t>(byte);
        if (size == 5 && frame[1] == (energy::function(request) | 0x80)) return 0;
    }
    if (size != expected) return 0;
    delay(5);
    return port.available() ? 0 : size;
}

void publish(const char* channel, const char* payload) {
    char topic[80];
    const int size = std::snprintf(topic, sizeof(topic), "%s/%s", kMqttPrefix, channel);
    if (size > 0 && size < static_cast<int>(sizeof(topic))) mqtt.publish(topic, payload, false);
}

void publishAc(HardwareSerial& port, const char* channel, uint8_t address) {
    uint8_t frame[25];
    const size_t received = readPzem(port, energy::Read::Ac, address, frame);
    energy::Sample sample;
    if (!energy::decodeAc(frame, received, address, sample)) {
        publish(channel, "{\"ok\":false,\"reason\":\"invalid_read\"}");
        return;
    }
    char payload[192];
    const int size = energy::encodeAc(sample, payload, sizeof(payload));
    if (size > 0 && size < static_cast<int>(sizeof(payload))) publish(channel, payload);
}

void publishPv() {
#if ENERGY_ENABLE_PV
    uint8_t frame[25];
    const size_t setupSize = readPzem(Serial, energy::Read::PvShunt, ENERGY_PV_ADDR, frame, kPvDirection);
    if (!energy::validResponse(frame, setupSize, ENERGY_PV_ADDR, energy::Read::PvShunt)) {
        publish("pv", "{\"ok\":false,\"reason\":\"shunt_unreadable\"}");
        return;
    }
    if (!energy::is100AmpShunt(frame, setupSize, ENERGY_PV_ADDR)) {
        publish("pv", "{\"ok\":false,\"reason\":\"shunt_mismatch\"}");
        return;
    }
    const size_t size = readPzem(Serial, energy::Read::Pv, ENERGY_PV_ADDR, frame, kPvDirection);
    energy::Sample sample;
    if (!energy::decodePv(frame, size, ENERGY_PV_ADDR, sample)) {
        publish("pv", "{\"ok\":false,\"reason\":\"invalid_read\"}");
        return;
    }
    if (sample.voltageV > ENERGY_PV_DESIGN_VOC_V) {
        publish("pv", "{\"ok\":false,\"reason\":\"above_design_voc\"}");
        return;
    }
    char payload[192];
    const int encoded = energy::encodePv(sample, payload, sizeof(payload));
    if (encoded > 0 && encoded < static_cast<int>(sizeof(payload))) publish("pv", payload);
#else
    publish("pv", "{\"ok\":false,\"reason\":\"disabled\"}");
#endif
}

} // namespace

void setup() {
#if ENERGY_ENABLE_PV
    digitalWrite(kPvDirection, LOW);
    pinMode(kPvDirection, OUTPUT);
    // UART0 is remapped after the ROM bootloader, leaving the USB upload pins free.
    Serial.begin(kBaud, SERIAL_8N2, kPvRx, kPvTx);
    Serial.setDebugOutput(false);
#else
    Serial.begin(115200);
    Serial.printf("Energy monitor %s, PV disabled\n", kClientId);
#endif
    Serial1.begin(kBaud, SERIAL_8N1, kGridRx, kGridTx);
    Serial2.begin(kBaud, SERIAL_8N1, kLoadRx, kLoadTx);
    std::snprintf(onlineTopic, sizeof(onlineTopic), "%s/status", kMqttPrefix);

    if (ENERGY_WIFI_SSID[0] == '\0' || ENERGY_MQTT_HOST[0] == '\0') return;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ENERGY_WIFI_SSID, ENERGY_WIFI_PASSWORD);
    mqtt.setServer(ENERGY_MQTT_HOST, ENERGY_MQTT_PORT);
    mqtt.setSocketTimeout(2);
    mqtt.setKeepAlive(15);
    mqtt.setBufferSize(384);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        delay(50);
        return;
    }
    const uint32_t now = millis();
    if (!mqtt.connected()) {
        if (lastMqttAttempt == 0 || now - lastMqttAttempt >= kReconnectMs) {
            lastMqttAttempt = now;
            if (mqtt.connect(kClientId, ENERGY_MQTT_USER, ENERGY_MQTT_PASSWORD, onlineTopic, 0, true, "offline")) {
                if (!mqtt.publish(onlineTopic, "online", true)) {
                    wifiClient.stop();
                    return;
                }
                pollImmediately = true;
            }
        }
        delay(25);
        return;
    }
    if (!mqtt.loop()) return;
    if (pollImmediately || now - lastPoll >= kPollMs) {
        pollImmediately = false;
        lastPoll = now;
        publishAc(Serial1, "grid", ENERGY_GRID_ADDR);
        publishAc(Serial2, "load", ENERGY_LOAD_ADDR);
        publishPv();
    }
    delay(10);
}
