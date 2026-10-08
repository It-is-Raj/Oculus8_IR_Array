/*
  LazyDragon Oculus-8 analog IR sensor array
  ISR-Driven 8-Channel Line Sensor Reader v2  -  ATmega328P / Arduino Nano

  Same ISR-driven ADC scanning as v1: all 8 reflectance sensors on A0..A7 are
  scanned continuously in the background. The main loop NEVER blocks on
  analogRead(); it just reads the latest values out of a buffer.

  v2 changes over v1:
    1) Integer position. `position` is now an int16_t scaled 1000x, range
       -7000..+7000 (0 = centered). No floating point anywhere -> faster,
       deterministic loop timing for a fast PID.
    2) Status classification + cluster detection. Each update() reports one
       of: ON_LINE, EDGE_LEFT, EDGE_RIGHT, JUNCTION, LOST.
    3) Best-effort position in every state (see update()).

  This is a sensor-reading test harness only:
    1) on boot, sweep the array back and forth over the line for 4s
    2) then watch calibrated values, position, status, lost/junction counters,
       saturation flag, loop dt, and loop rate over Serial (115200 baud)

  Motors are intentionally NOT driven here. Feed `position` into your PID
  later. Channel order assumes A0=leftmost ... A7=rightmost; flip the
  weights if your wiring is mirrored.

  NOTE: while this sketch owns the ADC, do NOT call analogRead() anywhere -
  it would fight over ADMUX/ADCSRA with the ISR.
*/

#include <util/atomic.h>

// ---------------- Configuration ----------------
#define NUM_SENSORS       8      // number of sensors (A0..A7)
#define IS_BLACK_LINE     true   // true: dark line on light floor
#define ON_THRESHOLD      500    // calibrated value (0..1000) => "on line"
#define EDGE_SENSOR_COUNT 2      // cluster confined to outer this-many sensors => EDGE
#define POSITION_SCALE    1000L  // position scaling factor (integer math)

// Linear, symmetric weights left(-) to right(+); used for the centroid.
const int8_t SENSOR_WEIGHT[NUM_SENSORS] = { -7, -5, -3, -1, 1, 3, 5, 7 };

// Line-loss / line-layout classification.
enum SensorStatus {
  ON_LINE,     // single cluster, safely inside the array -> centroid is valid
  EDGE_LEFT,   // single cluster confined to the leftmost sensors -> line off left edge
  EDGE_RIGHT,  // single cluster confined to the rightmost sensors -> line off right edge
  JUNCTION,    // two+ separated clusters -> ambiguous, main code decides the branch
  LOST         // no sensor sees the line
};

// ------------- ISR-shared variables -------------
volatile uint16_t adcRaw[NUM_SENSORS];  // latest raw reading per channel
volatile uint8_t  adcChannel = 0;       // channel currently being converted
volatile uint32_t frameCount = 0;       // +1 each time a full 8-ch scan finishes

// --------------- Calibration data ---------------
uint16_t minVal[NUM_SENSORS];
uint16_t maxVal[NUM_SENSORS];

// --------------- Derived outputs ----------------
uint16_t      calibrated[NUM_SENSORS];
int16_t       position        = 0;   // best-effort, scaled 1000x (-7000..+7000)
int16_t       lastGoodPosition = 0;  // last valid ON_LINE/EDGE_* position
SensorStatus  status          = LOST;
SensorStatus  lastStatus      = LOST;
bool          onLine          = false;
bool          saturated       = false;

// --------------- Bookkeeping --------------------
uint32_t lostFrames  = 0;           // consecutive frames in LOST
uint32_t junctions   = 0;           // rising-edge count of JUNCTION events
uint32_t lastPrintMs = 0;
uint32_t loopCounter = 0;
uint32_t lastMicros  = 0;

// Select an ADC channel while preserving the reference/ADLAR bits.
static inline void adcSelect(uint8_t ch) {
  ADMUX = (ADMUX & 0xF0) | (ch & 0x0F);
}

void adcSetup() {
  // AVcc reference, right-adjusted result, start on channel 0.
  ADMUX = (1 << REFS0);
  adcSelect(0);
  adcChannel = 0;

  // Enable ADC + ADC-complete interrupt. Prescaler /16 (ADPS=100) -> ~1 MHz
  // ADC clock -> ~13 us per conversion. ADATE stays 0: we re-trigger manually
  // in the ISR so each conversion reads exactly the channel we just selected
  // (avoids the free-running off-by-one channel problem).
  ADCSRA = (1 << ADEN) | (1 << ADIE) | (1 << ADPS2);

  // Disable digital input buffers on A0..A5 to cut noise/power.
  // (A6/A7 are analog-only and have no digital buffer.)
  DIDR0 = 0x3F;

  ADCSRA |= (1 << ADSC);  // kick off the first conversion
}

// Conversion complete: store result, advance channel, start the next read.
ISR(ADC_vect) {
  adcRaw[adcChannel] = ADC;         // reads ADCL then ADCH atomically

  adcChannel++;
  if (adcChannel >= NUM_SENSORS) {
    adcChannel = 0;
    frameCount++;
  }
  adcSelect(adcChannel);            // point mux at next channel BEFORE converting
  ADCSRA |= (1 << ADSC);            // start next conversion
}

// Copy the volatile buffer into a local array without an ISR tearing a value.
void snapshotRaw(uint16_t *dst) {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    for (uint8_t i = 0; i < NUM_SENSORS; i++) dst[i] = adcRaw[i];
  }
}

void resetCalibration() {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    minVal[i] = 1023;
    maxVal[i] = 0;
  }
}

void calibrate(uint16_t durationMs) {
  resetCalibration();
  uint16_t raw[NUM_SENSORS];
  uint32_t t0 = millis();
  while (millis() - t0 < durationMs) {
    snapshotRaw(raw);
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      if (raw[i] < minVal[i]) minVal[i] = raw[i];
      if (raw[i] > maxVal[i]) maxVal[i] = raw[i];
    }
  }
}

// Build calibrated values (0..1000), integer position, and status.
void update() {
  uint16_t raw[NUM_SENSORS];
  snapshotRaw(raw);

  long    num         = 0;          // weighted sum (weight * calibrated)
  long    den         = 0;          // total calibrated brightness
  uint8_t countActive = 0;          // sensors over threshold
  uint8_t lIdx        = NUM_SENSORS; // leftmost active index
  uint8_t rIdx        = 0;          // rightmost active index

  saturated = false;

  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    if (raw[i] <= 2 || raw[i] >= 1021) saturated = true;

    int span = (int)maxVal[i] - (int)minVal[i];
    if (span < 1) span = 1;                        // guard a never-crossed channel
    long v = ((long)((int)raw[i] - (int)minVal[i]) * 1000L) / span;
    if (v < 0)    v = 0;
    if (v > 1000) v = 1000;
    if (!IS_BLACK_LINE) v = 1000 - v;              // flip polarity for a light line
    calibrated[i] = (uint16_t)v;

    if (calibrated[i] > ON_THRESHOLD) {
      if (i < lIdx) lIdx = i;
      if (i > rIdx) rIdx = i;
      countActive++;
      num += (long)SENSOR_WEIGHT[i] * (long)calibrated[i];
      den += calibrated[i];
    }
  }

  // ---- Classify the active-sensor pattern ----
  if (countActive == 0) {
    // No line visible.
    status = LOST;
    onLine = false;
    position = lastGoodPosition;                   // hold last known
  }
  else if ((rIdx - lIdx + 1) > countActive) {
    // A gap between active sensors => two+ separated clusters => junction.
    // Centroid here is meaningless; main code decides the branch.
    status = JUNCTION;
    onLine = true;
    position = lastGoodPosition;
  }
  else if (rIdx < EDGE_SENSOR_COUNT) {
    // Single cluster confined to the far-left sensors: line runs off left edge.
    status = EDGE_LEFT;
    onLine = true;
    position = (int16_t)((long)SENSOR_WEIGHT[lIdx] * POSITION_SCALE);
    lastGoodPosition = position;
  }
  else if (lIdx >= NUM_SENSORS - EDGE_SENSOR_COUNT) {
    // Single cluster confined to the far-right sensors: line runs off right edge.
    status = EDGE_RIGHT;
    onLine = true;
    position = (int16_t)((long)SENSOR_WEIGHT[rIdx] * POSITION_SCALE);
    lastGoodPosition = position;
  }
  else {
    // Single cluster inside the array: normal centroid.
    status = ON_LINE;
    onLine = true;
    if (den > 0) {
      position = (int16_t)((num * POSITION_SCALE) / den);
      lastGoodPosition = position;
    } else {
      position = lastGoodPosition;
    }
  }

  // ---- Bookkeeping ----
  if (status == LOST) {
    lostFrames++;
  } else {
    lostFrames = 0;
  }
  if (status == JUNCTION && lastStatus != JUNCTION) {
    junctions++;
  }
  lastStatus = status;
}

void printStatus(SensorStatus s) {
  switch (s) {
    case ON_LINE:    Serial.print(F("ON_LINE"));    break;
    case EDGE_LEFT:  Serial.print(F("EDGE_LEFT"));  break;
    case EDGE_RIGHT: Serial.print(F("EDGE_RIGHT")); break;
    case JUNCTION:   Serial.print(F("JUNCTION"));   break;
    case LOST:       Serial.print(F("LOST"));       break;
  }
}

void setup() {
  Serial.begin(115200);

  noInterrupts();
  adcSetup();
  interrupts();

  delay(10);                                      // let the buffer fill once

  Serial.println(F("Sweep the array across the line for 4 seconds..."));
  calibrate(4000);
  Serial.println(F("Calibration done."));

  lastMicros  = micros();
  lastPrintMs = millis();
}

void loop() {
  uint32_t now = micros();
  uint32_t dt  = now - lastMicros;                // real elapsed time, for your PID
  lastMicros = now;

  update();
  loopCounter++;

  // Print ~20x/sec so Serial traffic doesn't throttle the loop.
  if (millis() - lastPrintMs >= 50) {
    lastPrintMs = millis();

    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      Serial.print(calibrated[i]);
      Serial.print('\t');
    }
    Serial.print(F("pos="));      Serial.print(position);
    Serial.print(F("  status=")); printStatus(status);
    Serial.print(F("  lost="));   Serial.print(lostFrames);
    Serial.print(F("  jct="));    Serial.print(junctions);
    Serial.print(F("  sat="));    Serial.print(saturated);
    Serial.print(F("  dt_us="));  Serial.print(dt);
    Serial.print(F("  loops/s=")); Serial.print(loopCounter * 20); // 1000/50 = 20
    Serial.println();

    loopCounter = 0;
  }
}
