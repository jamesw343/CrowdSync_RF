/*
  CrowdSync NY8-family read-only EPROM dump attempt (no Vpp)

  Target power:
    Power the CrowdSync through its normal 6 V PSU input. The Arduino does
    NOT power the target and does NOT connect to target Vpp/reset (MCU pin 4).

  Board:
    Arduino Uno/Nano using ATmega328P, 16 MHz, 5 V logic.

  Arduino <-> candidate NY8A054E / NY8A051H pin mapping:
    D2 (PD2) -> divider -> MCU pin 7 SDI
    D3 (PD3) <- 1k series <- MCU pin 6 SDO
    D4 (PD4) -> divider -> MCU pin 5 SCK
    A1       <- 10k series <- MCU pin 1 VDD (3.3 V regulated rail)
    GND      <->          MCU pin 8 VSS / PSU negative

  This is a read-only protocol attempt. It sends only the 0x53AD handshake
  and the EPROM read command (0x20 plus zero address bytes). It never sends
  program, erase, configuration-write, or Vpp commands.

  Important limitations:
    - The NY8A051H-style profile uses a 3-byte read command for 1024 words.
      The NY8A054E-style profile uses a 4-byte read command for 2048 words.
    - NY8 has no explicit ACK check in this reverse-engineered sequence.
      The handshake result below is a heuristic based on SDO staying low
      after the target's normal firmware would have started.
    - A complete transfer is not proof of correct data. Use the reported
      content statistics and repeat the same profile on a second cold start;
      matching FNV-1a hashes are a useful integrity check.
*/

#include <Arduino.h>
#include <avr/io.h>
#include <avr/interrupt.h>

#if !defined(__AVR_ATmega328P__) || (F_CPU != 16000000UL)
#error "This sketch is for a 16 MHz ATmega328P Arduino Uno/Nano."
#endif

// D2=PD2, D3=PD3, D4=PD4 on the ATmega328P Uno/Nano.
static const uint8_t SDI_MASK = _BV(PD2);
static const uint8_t SDO_MASK = _BV(PD3);
static const uint8_t SCK_MASK = _BV(PD4);
static const uint8_t PROGRAM_MASK = SDI_MASK | SCK_MASK;
static const uint8_t VDD_SENSE_PIN = A1;

// A1 senses the 0-3.3 V MCU rail through a 10k series current-limiting resistor.
// With the default 5 V ADC reference, 10 bits span about 4.89 mV per count.
static const uint16_t VDD_OFF_ADC = 4;        // MCU VDD < about 20 mV
static const uint16_t VDD_RISE_ADC = 10;      // MCU VDD > about 49 mV
static const uint16_t VDD_SAFE_ADC = 450;     // MCU VDD >= about 2.20 V
static const uint32_t HANDSHAKE_DELAY_US = 500;
static const uint32_t SAFE_RAIL_TIMEOUT_MS = 5000;
static const uint32_t OFF_STABLE_MS = 250;

// Timing for the reverse-engineered programming protocol.
#define DELAY_NANO(ns) __builtin_avr_delay_cycles(((double)(ns)) * F_CPU / 1e9)
#define DELAY_MICRO(us) DELAY_NANO(1000.0 * (us))

struct ReadProfile {
  const char *name;
  uint16_t wordCount;
  uint8_t commandBytes;
};

static const ReadProfile PROFILE_1K = {"NY8A051H-style 1K / 3-byte read command", 1024, 3};
static const ReadProfile PROFILE_2K = {"NY8A054E-style 2K / 4-byte read command", 2048, 4};

static inline void setSdi(bool high) {
  if (high) PORTD |= SDI_MASK;
  else PORTD &= (uint8_t)~SDI_MASK;
}

static inline bool readSdo() {
  return (PIND & SDO_MASK) != 0;
}

static void leaveProgrammingPinsHighZ() {
  // Keep output latches low and disable pull-ups before releasing the pins.
  PORTD &= (uint8_t)~(PROGRAM_MASK | SDO_MASK);
  DDRD &= (uint8_t)~(PROGRAM_MASK | SDO_MASK);
}

static void setProgrammingPinsActive() {
  // Establish low output latches before enabling the output drivers.
  PORTD &= (uint8_t)~(PROGRAM_MASK | SDO_MASK);
  DDRD &= (uint8_t)~SDO_MASK;
  DDRD |= PROGRAM_MASK;
}

static uint16_t readTargetVddAdc() {
  return (uint16_t)analogRead(VDD_SENSE_PIN);
}

static bool waitForTargetOff() {
  Serial.println(F("Turn the PSU OFF. Waiting for MCU VDD to fall below the off threshold..."));
  Serial.flush();
  uint32_t lowSince = 0;
  while (true) {
    uint16_t vdd = readTargetVddAdc();
    uint32_t now = millis();
    if (vdd <= VDD_OFF_ADC) {
      if (lowSince == 0) lowSince = now;
      if ((uint32_t)(now - lowSince) >= OFF_STABLE_MS) return true;
    } else {
      lowSince = 0;
    }
  }
}

static bool waitForTargetRiseAndSafeRail(uint32_t *vddRiseUs,
                                         uint32_t *safeRailUs) {
  // Detect the MCU rail directly, rather than inferring it from raw PSU VIN.
  while (readTargetVddAdc() < VDD_RISE_ADC) {
    // Wait without serial output so the rail-on edge is not delayed.
  }
  *vddRiseUs = micros();

  uint32_t startMs = millis();
  while (readTargetVddAdc() < VDD_SAFE_ADC) {
    if ((uint32_t)(millis() - startMs) > SAFE_RAIL_TIMEOUT_MS) return false;
  }
  *safeRailUs = micros();

  // Preserve the nominal 500 us post-power delay where
  // possible, while never driving the target pins before VDD is safe.
  uint32_t elapsed = (uint32_t)(micros() - *vddRiseUs);
  if (elapsed < HANDSHAKE_DELAY_US) {
    delayMicroseconds((unsigned int)(HANDSHAKE_DELAY_US - elapsed));
  }
  return true;
}

static uint8_t sendReceiveByte(uint8_t dataOut) {
  uint8_t mask = 0x80;  // MSB first
  setSdi((dataOut & mask) != 0);
  DELAY_MICRO(1);

  uint8_t dataIn = 0;
  do {
    if (readSdo()) dataIn |= mask;
    PORTD |= SCK_MASK;

    mask >>= 1;
    setSdi((dataOut & mask) != 0);
    DELAY_NANO(500);

    PORTD &= (uint8_t)~SCK_MASK;
    DELAY_NANO(500);
  } while (mask != 0);

  return dataIn;
}

static void sendHandshake() {
  (void)sendReceiveByte(0x53);
  DELAY_MICRO(1.5);
  (void)sendReceiveByte(0xAD);
}

static bool checkHandshakeHeuristic() {
  // Wait 100 ms and treat SDO remaining low as
  // programming-mode evidence. Sample repeatedly to reject active PWM/noise.
  delay(100);
  uint8_t highSamples = 0;
  bool previous = readSdo();
  uint8_t transitions = 0;
  for (uint8_t i = 0; i < 32; ++i) {
    delay(1);
    bool current = readSdo();
    if (current) ++highSamples;
    if (current != previous) ++transitions;
    previous = current;
  }

  Serial.print(F("SDO status samples: high="));
  Serial.print(highSamples);
  Serial.print(F(" / 32, transitions="));
  Serial.println(transitions);

  if (highSamples == 0 && transitions == 0) {
    Serial.println(F("HANDSHAKE_STATUS=LIKELY_ACCEPTED (SDO stayed low; heuristic, not a protocol ACK)"));
    return true;
  }
  Serial.println(F("HANDSHAKE_STATUS=NOT_DETECTED (SDO was high/active; could also be board loading)"));
  return false;
}

static void printHexWord(uint16_t word, uint16_t index) {
  if (word < 0x1000) Serial.print('0');
  if (word < 0x0100) Serial.print('0');
  if (word < 0x0010) Serial.print('0');
  Serial.print(word, HEX);
  if ((index & 7) == 7) Serial.println();
  else Serial.print(' ');
}

static bool readEprom(const ReadProfile &profile) {
  Serial.print(F("READ_PROFILE="));
  Serial.println(profile.name);
  Serial.println(F("Sending read-only command 0x20 with zero address bytes."));

  (void)sendReceiveByte(0x20);
  (void)sendReceiveByte(0x00);
  (void)sendReceiveByte(0x00);
  if (profile.commandBytes == 4) (void)sendReceiveByte(0x00);

  uint32_t nonzeroWords = 0;
  uint32_t changedWords = 0;
  uint32_t hash = 2166136261UL;  // FNV-1a, for repeatability comparison only
  uint16_t previousWord = 0;

  Serial.println(F("BEGIN_DUMP"));
  for (uint16_t i = 0; i < profile.wordCount; ++i) {
    uint8_t b1 = sendReceiveByte(0x00);
    uint8_t b2 = sendReceiveByte(0x00);
    uint16_t word = (uint16_t)(((uint16_t)(b1 & 0x7F) << 7) | (b2 & 0x7F));
    word = (uint16_t)(~word) & 0x3FFF;

    if (word != 0) ++nonzeroWords;
    if (i != 0 && word != previousWord) ++changedWords;
    previousWord = word;

    hash ^= (uint8_t)(word & 0xFF);
    hash *= 16777619UL;
    hash ^= (uint8_t)(word >> 8);
    hash *= 16777619UL;

    printHexWord(word, i);
  }
  if ((profile.wordCount & 7) != 0) Serial.println();
  Serial.println(F("END_DUMP"));
  Serial.print(F("READ_COMPLETE_WORDS="));
  Serial.println(profile.wordCount);
  Serial.print(F("NONZERO_WORDS="));
  Serial.println(nonzeroWords);
  Serial.print(F("CHANGED_ADJACENT_WORDS="));
  Serial.println(changedWords);
  Serial.print(F("FNV1A32="));
  if (hash < 0x10000000UL) Serial.print('0');
  if (hash < 0x01000000UL) Serial.print('0');
  if (hash < 0x00100000UL) Serial.print('0');
  if (hash < 0x00010000UL) Serial.print('0');
  if (hash < 0x00001000UL) Serial.print('0');
  if (hash < 0x00000100UL) Serial.print('0');
  if (hash < 0x00000010UL) Serial.print('0');
  Serial.println(hash, HEX);

  bool plausible = (nonzeroWords >= 8 && changedWords >= 4);
  if (plausible) {
    Serial.println(F("EXTRACTION_STATUS=COMPLETE_PLAUSIBLE_DATA (repeat and compare hash)"));
  } else {
    Serial.println(F("EXTRACTION_STATUS=TRANSFER_COMPLETE_BUT_DATA_SUSPICIOUS_OR_BLANK"));
  }
  Serial.println(F("Note: the NY8 read stream has no checksum here; matching repeat dumps are important."));
  return plausible;
}

static char waitForProfileChoice() {
  Serial.println();
  Serial.println(F("CrowdSync NY8 no-Vpp read-only dumper"));
  Serial.println(F("1 = 1K / NY8A051H-style (3 command bytes, 1024 words)"));
  Serial.println(F("2 = 2K / NY8A054E-style (4 command bytes, 2048 words)"));
  Serial.println(F("Select one profile. Each profile needs its own PSU power cycle."));
  while (true) {
    if (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '1' || c == '2') return c;
    }
  }
}

void setup() {
  // Leave all target-facing pins high impedance and without internal pull-ups.
  leaveProgrammingPinsHighZ();
  pinMode(VDD_SENSE_PIN, INPUT);
  analogReference(DEFAULT);

  Serial.begin(115200);
  Serial.println(F("READ-ONLY / NO VPP. Power CrowdSync from its normal 6 V PSU input."));
  Serial.println(F("Do not connect Arduino 5 V to target VDD or target programming pins."));
}

void loop() {
  const char choice = waitForProfileChoice();
  const ReadProfile &profile = (choice == '1') ? PROFILE_1K : PROFILE_2K;

  leaveProgrammingPinsHighZ();
  if (!waitForTargetOff()) return;
  Serial.println(F("ARMED: switch the normal CrowdSync PSU ON now."));
  Serial.flush();

  uint32_t vddRiseUs = 0;
  uint32_t safeRailUs = 0;
  if (!waitForTargetRiseAndSafeRail(&vddRiseUs, &safeRailUs)) {
    leaveProgrammingPinsHighZ();
    Serial.println(F("ABORT: MCU VDD did not reach 2.2 V within 5 s. No programming clocks sent."));
    return;
  }

  setProgrammingPinsActive();
  uint32_t handshakeStartUs = micros();
  Serial.flush();
  noInterrupts();
  sendHandshake();
  interrupts();

  Serial.print(F("MCU_VDD_RISE_TO_SAFE_RAIL_US="));
  Serial.println((uint32_t)(safeRailUs - vddRiseUs));
  Serial.print(F("MCU_VDD_RISE_TO_HANDSHAKE_START_US="));
  Serial.println((uint32_t)(handshakeStartUs - vddRiseUs));

  if (checkHandshakeHeuristic()) {
    noInterrupts();
    (void)readEprom(profile);
    interrupts();
  } else {
    Serial.println(F("EXTRACTION_STATUS=NOT_ATTEMPTED (handshake was not detected)"));
  }

  leaveProgrammingPinsHighZ();
  Serial.println(F("Attempt ended. Turn PSU OFF; wait for MCU VDD to discharge before the next profile/run."));
}
