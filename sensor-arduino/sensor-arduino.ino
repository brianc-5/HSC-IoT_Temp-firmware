/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Energy-harvested BTHome v2 temperature sensor
 *
 * Target: "Seeed XIAO nRF52840" from the Adafruit-based "Seeed nRF52
 * Boards" package. This is intentionally not compatible with the mbed core.
 *
 * Power budget: the MCU/SoftDevice spends most of each 20-second interval in
 * retained system-on sleep, but wakes for measurement and a one-second radio
 * burst every interval. This six-times-more-frequent wake/radio activity
 * materially increases average energy; measure the assembled hardware against
 * the harvester budget. USB, a debugger, the charger, or flash leakage can
 * change the result substantially.
 */

#ifndef DEBUG
#define DEBUG 0
#endif

#if DEBUG
#include <Adafruit_TinyUSB.h>  // Declares USB CDC Serial on this non-mbed core.
#endif
#include <bluefruit.h>
#include <nrf_saadc.h>

#include <limits.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint32_t SAMPLE_INTERVAL_S = 20;
constexpr uint16_t ADV_INTERVAL_MS = 200;
constexpr uint16_t BURST_DURATION_S = 1;
constexpr int8_t TX_POWER_DBM = 0;

constexpr uint32_t SAMPLE_INTERVAL_MS = SAMPLE_INTERVAL_S * 1000UL;
// Legacy BLE advertising intervals are expressed in 0.625 ms units.
constexpr uint16_t ADV_INTERVAL_UNITS =
    static_cast<uint16_t>((ADV_INTERVAL_MS * 1000UL) / 625UL);
constexpr size_t ADV_PACKET_SIZE = 16;

static_assert(ADV_INTERVAL_MS * 1000UL == ADV_INTERVAL_UNITS * 625UL,
              "ADV_INTERVAL_MS must be an exact multiple of 0.625 ms");
static_assert(sizeof(int16_t) == 2, "BTHome temperature requires 16-bit int16_t");

// The advertising timeout callback runs on the core's callback task. Volatile
// scalars are sufficient here because aligned 32-bit accesses are atomic on M4.
volatile bool burstStopped = true;
volatile uint32_t burstStoppedAtMs = 0;

uint32_t burstStartedAtMs = 0;
uint32_t nextSampleAtMs = 0;
uint8_t packetSequence = 0;
bool burstStopWasReported = true;

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

float readTemperatureC() {
  // The SoftDevice owns TEMP, so use the core abstraction (sd_temp_get() while
  // BLE is enabled). Replace only this function when the MLX90614 is added.
  return readCPUTemperature();
}

uint16_t readSupplyMillivolts() {
  // The packaged nrfx SAADC driver is compiled out in this Seeed core, so use
  // the bundled nrfx HAL directly. This reads the internal VDD input: no XIAO
  // VBAT pin, divider-enable pin, or external analog divider is involved.
  // EasyDMA writes this value without a C/C++ assignment. Keep it volatile so
  // the core's -Ofast build reloads the completed sample instead of reusing 0.
  volatile nrf_saadc_value_t raw = 0;

  nrf_saadc_channel_config_t channelConfig = {};
  channelConfig.resistor_p = NRF_SAADC_RESISTOR_DISABLED;
  channelConfig.resistor_n = NRF_SAADC_RESISTOR_DISABLED;
  channelConfig.gain = NRF_SAADC_GAIN1_6;
  channelConfig.reference = NRF_SAADC_REFERENCE_INTERNAL;
  channelConfig.acq_time = NRF_SAADC_ACQTIME_10US;
  channelConfig.mode = NRF_SAADC_MODE_SINGLE_ENDED;
  channelConfig.burst = NRF_SAADC_BURST_DISABLED;

  // Start from a fully known, polling-only state. The conversion's two short
  // waits last only for the hardware transaction; long idle periods never spin.
  nrf_saadc_disable(NRF_SAADC);
  nrf_saadc_int_disable(NRF_SAADC, NRF_SAADC_INT_ALL);
  nrf_saadc_continuous_mode_disable(NRF_SAADC);
  nrf_saadc_resolution_set(NRF_SAADC, NRF_SAADC_RESOLUTION_12BIT);
  nrf_saadc_oversample_set(NRF_SAADC, NRF_SAADC_OVERSAMPLE_DISABLED);

  for (uint8_t channel = 0; channel < 8; ++channel) {
    nrf_saadc_channel_input_set(NRF_SAADC, channel,
                                NRF_SAADC_INPUT_DISABLED,
                                NRF_SAADC_INPUT_DISABLED);
  }

  nrf_saadc_channel_init(NRF_SAADC, 0, &channelConfig);
  nrf_saadc_channel_input_set(NRF_SAADC, 0, NRF_SAADC_INPUT_VDD,
                              NRF_SAADC_INPUT_DISABLED);
  nrf_saadc_buffer_init(
      NRF_SAADC, const_cast<nrf_saadc_value_t*>(&raw), 1);

  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STARTED);
  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_END);
  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STOPPED);

  nrf_saadc_enable(NRF_SAADC);
  nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_START);
  while (!nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_STARTED)) {
  }
  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STARTED);

  nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_SAMPLE);
  while (!nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_END)) {
  }
  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_END);

  // STOP and wait before disconnecting/disable. This is the power-critical
  // teardown that prevents the analog peripheral drawing current in sleep.
  nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_STOP);
  while (!nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_STOPPED)) {
  }
  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STOPPED);

  for (uint8_t channel = 0; channel < 8; ++channel) {
    nrf_saadc_channel_input_set(NRF_SAADC, channel,
                                NRF_SAADC_INPUT_DISABLED,
                                NRF_SAADC_INPUT_DISABLED);
  }
  nrf_saadc_disable(NRF_SAADC);
  NRF_SAADC->RESULT.PTR = 0;
  NRF_SAADC->RESULT.MAXCNT = 0;

  if (raw < 0) {
    raw = 0;
  }

  // NRF_SAADC_INPUT_VDD connects VDD directly. With the 0.6 V internal
  // reference, gain 1/6, and 12-bit resolution, full scale is 3.6 V.
  // Add half a denominator for rounding to the nearest millivolt.
  const uint32_t millivolts =
      (static_cast<uint32_t>(raw) * 3600UL + 2048UL) / 4096UL;
  return static_cast<uint16_t>(millivolts);
}

int16_t temperatureToCentiDegrees(float temperatureC) {
  float scaled = temperatureC * 100.0f;
  if (scaled > static_cast<float>(INT16_MAX)) {
    scaled = static_cast<float>(INT16_MAX);
  } else if (scaled < static_cast<float>(INT16_MIN)) {
    scaled = static_cast<float>(INT16_MIN);
  }

  // Round halves away from zero without depending on the C library's lroundf.
  return static_cast<int16_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

// ---------------------------------------------------------------------------
// BTHome v2 payload encoding
// ---------------------------------------------------------------------------

void buildAdvertisement(uint8_t sequence, int16_t temperatureCenti,
                        uint16_t supplyMillivolts,
                        uint8_t advertisement[ADV_PACKET_SIZE]) {
  const uint16_t temperatureBits = static_cast<uint16_t>(temperatureCenti);

  // AD structure 1: [length=2][Flags type=0x01][flags=0x06].
  advertisement[0] = 0x02;
  advertisement[1] = 0x01;
  advertisement[2] = 0x06;

  // AD structure 2: [length=12][Service Data type=0x16][UUID D2 FC], then
  // exactly the nine-byte unencrypted BTHome v2 payload from the protocol.
  advertisement[3] = 0x0C;
  advertisement[4] = 0x16;
  advertisement[5] = 0xD2;  // UUID 0xFCD2, little-endian.
  advertisement[6] = 0xFC;
  advertisement[7] = 0x40;  // BTHome v2, unencrypted.
  advertisement[8] = 0x00;  // Packet-id object.
  advertisement[9] = sequence;
  advertisement[10] = 0x02;  // Temperature object, signed 0.01 degC.
  advertisement[11] = static_cast<uint8_t>(temperatureBits & 0xFFU);
  advertisement[12] = static_cast<uint8_t>(temperatureBits >> 8);
  advertisement[13] = 0x0C;  // Voltage object, unsigned millivolts.
  advertisement[14] = static_cast<uint8_t>(supplyMillivolts & 0xFFU);
  advertisement[15] = static_cast<uint8_t>(supplyMillivolts >> 8);
}

// ---------------------------------------------------------------------------
// Radio burst
// ---------------------------------------------------------------------------

void advertisingStoppedCallback() {
  // The callback is dispatched only after the SoftDevice reports that the
  // timed advertising set has terminated, so a true flag means radio is off.
  burstStoppedAtMs = millis();
  burstStopped = true;
}

#if DEBUG
void logSample(uint8_t sequence, int16_t temperatureCenti,
               uint16_t supplyMillivolts) {
  Serial.print("sample seq=");
  Serial.print(sequence);
  Serial.print(" temp=");
  Serial.print(temperatureCenti / 100.0f, 2);
  Serial.print(" C vdd=");
  Serial.print(supplyMillivolts);
  Serial.print(" mV burst_start_ms=");
  Serial.println(burstStartedAtMs);
}

void logBurstStopped() {
  Serial.print("burst_stop_ms=");
  Serial.print(static_cast<uint32_t>(burstStoppedAtMs));
  Serial.print(" duration_ms=");
  Serial.println(static_cast<uint32_t>(burstStoppedAtMs) - burstStartedAtMs);
}
#endif

void takeSampleAndStartBurst() {
  const uint32_t sampleStartedAtMs = millis();
  nextSampleAtMs = sampleStartedAtMs + SAMPLE_INTERVAL_MS;

  const float temperatureC = readTemperatureC();
  const int16_t temperatureCenti = temperatureToCentiDegrees(temperatureC);
  const uint16_t supplyMillivolts = readSupplyMillivolts();
  const uint8_t sequence = packetSequence++;  // uint8_t wrap is intentional.

  uint8_t advertisement[ADV_PACKET_SIZE];
  buildAdvertisement(sequence, temperatureCenti, supplyMillivolts,
                     advertisement);

  // setData() copies this complete raw legacy packet into Bluefruit's buffer;
  // nothing else (name, TX-power AD, or scan response) is appended.
  const bool payloadAccepted =
      Bluefruit.Advertising.setData(advertisement, sizeof(advertisement));

  burstStartedAtMs = millis();
  burstStopped = false;
  burstStopWasReported = false;

#if DEBUG
  logSample(sequence, temperatureCenti, supplyMillivolts);
#endif

  if (!payloadAccepted ||
      !Bluefruit.Advertising.start(BURST_DURATION_S)) {
    // Avoid a permanent awake state if the stack rejects a burst. The normal
    // path reaches advertisingStoppedCallback() after the one-second timeout.
    burstStoppedAtMs = millis();
    burstStopped = true;
#if DEBUG
    Serial.println("ERROR: advertising burst did not start");
#endif
  }
}

// ---------------------------------------------------------------------------
// Setup and tickless sleep loop
// ---------------------------------------------------------------------------

void turnOnboardLedsOff() {
  // XIAO's RGB channels are active-low. Keep all three as driven-high outputs
  // so none floats or leaks through an LED while the rest of the pins remain at
  // their core defaults (inputs, with no pulls added by this sketch).
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, HIGH);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, HIGH);
}

void setup() {
#if DEBUG
  Serial.begin(115200);
  const uint32_t serialWaitStartedAtMs = millis();
  while (!Serial && millis() - serialWaitStartedAtMs < 3000UL) {
    delay(10);
  }
  Serial.println("BTHome temperature sensor starting (DEBUG build)");
#endif

  turnOnboardLedsOff();

  Bluefruit.begin();
  // Disable Bluefruit's timer-driven connection/advertising LED before any
  // burst; otherwise it periodically wakes the CPU and drives an active-low LED.
  Bluefruit.autoConnLed(false);
  turnOnboardLedsOff();
  Bluefruit.setTxPower(TX_POWER_DBM);

  // Explicitly retain the SoftDevice's low-power (not constant-latency) mode.
  // FreeRTOS tickless idle will then enter sd_app_evt_wait between wake events.
  (void)sd_power_mode_set(NRF_POWER_MODE_LOWPWR);

  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.setType(
      BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL_UNITS,
                                    ADV_INTERVAL_UNITS);
  // Seeed core 1.1.x starts in its "fast" phase. Matching both timeout values
  // makes start(1) terminate at one second instead of the default 30 seconds.
  Bluefruit.Advertising.setFastTimeout(BURST_DURATION_S);
  Bluefruit.Advertising.setStopCallback(advertisingStoppedCallback);

  takeSampleAndStartBurst();
}

void loop() {
  if (!burstStopped) {
    // Advertising and its timeout are SoftDevice-driven. One tickless wait per
    // advertising interval avoids hot polling; the callback, not this delay,
    // decides when the burst has actually stopped.
    delay(ADV_INTERVAL_MS);
    return;
  }

  if (!burstStopWasReported) {
#if DEBUG
    logBurstStopped();
#endif
    burstStopWasReported = true;
  }

  const int32_t millisecondsUntilSample =
      static_cast<int32_t>(nextSampleAtMs - millis());
  if (millisecondsUntilSample > 0) {
    // Arduino delay() in this core is vTaskDelay(); FreeRTOS suppresses ticks
    // and the idle path calls sd_app_evt_wait. The radio and SAADC are already
    // fully stopped before this long system-on sleep begins.
    delay(static_cast<uint32_t>(millisecondsUntilSample));
    return;
  }

  takeSampleAndStartBurst();
}
