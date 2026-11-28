#include <Arduino.h>
#include <Wire.h>
#include <util/atomic.h>

#if !defined(__AVR_ATmega328P__)
#error "Select Arduino Uno for the ATmega328P sketch"
#endif

// I2C request: A1, sequence, pack address, Shoto header. FF selects status.
constexpr uint8_t I2C_ADDRESS = 0x31;
constexpr uint8_t READ_TELEMETRY = 0xA1;
constexpr uint8_t STATUS_MARKER = 0xB1;
constexpr uint8_t STATUS_OFFSET = 0xFF;
constexpr uint8_t IN_PROGRESS = 0xFF;
constexpr uint8_t CHUNK_SIZE = 24;
constexpr uint8_t CAPTURE_SIZE = 192;
constexpr uint8_t RS485_DIRECTION = 4;
constexpr uint16_t SHOTO_TIMEOUT_MS = 500;
constexpr uint16_t FRAME_GAP_MS = 65;

volatile uint8_t sequence = 0;
volatile uint8_t address = 0;
volatile uint8_t header = 0;
volatile uint8_t replySize = 0;
volatile uint8_t readOffset = STATUS_OFFSET;
volatile bool pending = false;
uint8_t capture[CAPTURE_SIZE];

static void receiveCommand(int size) {
    if (size == 1) {
        readOffset = Wire.read();
        return;
    }
    if (size != 4) {
        while (Wire.available()) Wire.read();
        return;
    }

    const int command = Wire.read();
    const int nextSequence = Wire.read();
    const int nextAddress = Wire.read();
    const int nextHeader = Wire.read();
    if (command != READ_TELEMETRY || nextSequence < 0 || nextAddress < 1 ||
        nextAddress > 4 || (nextHeader != 0x7C && nextHeader != 0x7E)) return;

    sequence = nextSequence;
    address = nextAddress;
    header = nextHeader;
    replySize = IN_PROGRESS;
    readOffset = STATUS_OFFSET;
    pending = true;
}

static void sendReply() {
    const uint8_t size = replySize;
    const uint8_t offset = readOffset;
    if (offset == STATUS_OFFSET) {
        const uint8_t status[] = {STATUS_MARKER, sequence, address, header, size};
        Wire.write(status, sizeof(status));
    } else if (size != IN_PROGRESS && offset < size) {
        const uint8_t remaining = size - offset;
        Wire.write(capture + offset, min(CHUNK_SIZE, remaining));
    }
}

static uint8_t captureTelemetry(uint8_t packAddress, uint8_t frameHeader) {
    // Only command 01 is reachable from I2C, regardless of the received bytes.
    const uint8_t checksum = (frameHeader ^ packAddress ^ 0x01u) ^
                             (frameHeader + packAddress + 0x01u);
    const uint8_t request[] = {frameHeader, packAddress, 0x01, 0x00, checksum, 0x0D};
    while (Serial.available()) Serial.read();
    digitalWrite(RS485_DIRECTION, HIGH);
    delayMicroseconds(100);
    Serial.write(request, sizeof(request));
    Serial.flush();
    digitalWrite(RS485_DIRECTION, LOW);

    const uint32_t start = millis();
    uint32_t lastByte = start;
    uint8_t count = 0;
    while (millis() - start < SHOTO_TIMEOUT_MS) {
        const int value = Serial.read();
        if (value >= 0) {
            if (count == CAPTURE_SIZE) return 0;
            capture[count++] = static_cast<uint8_t>(value);
            lastByte = millis();
        } else if (count > 0 && millis() - lastByte >= FRAME_GAP_MS) {
            break;
        }
    }
    return count;
}

void setup() {
    digitalWrite(RS485_DIRECTION, LOW);
    pinMode(RS485_DIRECTION, OUTPUT);
    Serial.begin(9600);
    Wire.begin(I2C_ADDRESS);
    Wire.onReceive(receiveCommand);
    Wire.onRequest(sendReply);
}

void loop() {
    uint8_t packAddress = 0;
    uint8_t frameHeader = 0;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (pending) {
            packAddress = address;
            frameHeader = header;
            pending = false;
        }
    }
    if (packAddress == 0) return;

    const uint8_t size = captureTelemetry(packAddress, frameHeader);
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        // A new command can arrive while this UART capture is running.
        if (!pending) replySize = size;
    }
}
