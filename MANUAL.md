# Manual — LazyDragon Oculus-8 Analog IR Sensor Array

How the reader works, what each function does, and the format of its Serial
output for the **LazyDragon Oculus-8 analog IR sensor array**. For hardware
and a quick start see [`README.md`](README.md); for troubleshooting see
[`DEBUG.md`](DEBUG.md).

---

## How it works

Instead of the loop polling each channel, the hardware does the work:

1. `adcSetup()` selects channel 0 and starts a conversion.
2. When it finishes, `ISR(ADC_vect)` stores the result, advances to the next
   channel, points the mux at it, and starts the next conversion.
3. After the 8th channel, a frame counter increments and the scan wraps.

Because the mux is re-pointed **before** each new conversion is triggered,
every reading lands on the channel it is meant for — there is no free-running
"off-by-one channel" ambiguity.

At a /16 prescaler the ADC clock is about 1 MHz, roughly **13 microseconds per
channel**, so a full 8-channel frame completes in well under a tenth of a
millisecond. The main loop only snapshots that buffer, normalizes it, and
classifies the pattern.

---

## Configuration

Edit these near the top of the sketch.

| Constant | Default | Meaning |
|----------|---------|---------|
| `NUM_SENSORS` | `8` | Channels scanned, A0..A( NUM-1 ). |
| `IS_BLACK_LINE` | `true` | `true` for a dark line on a light floor. |
| `ON_THRESHOLD` | `500` | Calibrated value (0..1000) above which a channel counts as "on line". |
| `EDGE_SENSOR_COUNT` | `2` | A cluster confined to this many outer sensors is reported as an edge. |
| `POSITION_SCALE` | `1000` | Integer position scale factor. |
| `SENSOR_WEIGHT[]` | `{-7,-5,-3,-1,1,3,5,7}` | Symmetric weights used by the centroid. Flip if wiring is mirrored. |

---

## Function reference

### `adcSelect(uint8_t ch)`
Writes a new channel number into the low nibble of `ADMUX` while preserving
the reference-selection bits in the high nibble. Keeping the reference bits
intact avoids re-settling the ADC whenever the channel changes.

### `adcSetup()`
One-time ADC configuration: selects AVcc as the reference and right-adjusted
results, enables the ADC and the ADC-complete interrupt, sets the /16
prescaler, disables the digital input buffers on A0..A5 to reduce noise and
power, and kicks off the very first conversion. Auto-trigger (`ADATE`) is
deliberately left off so each conversion is started manually by the ISR
against an explicitly selected channel.

### `ISR(ADC_vect)`
The engine of the reader. One conversion's worth of work:
1. Reads `ADC` (low byte then high byte, atomically) into `adcRaw[]`.
2. Increments the channel index, wrapping to 0 and incrementing `frameCount`
   at the end of a frame.
3. Calls `adcSelect()` for the next channel.
4. Starts the next conversion.

### `snapshotRaw(uint16_t *dst)`
Copies the volatile ISR buffer into a caller-provided array inside an
`ATOMIC_BLOCK`, so the main loop always sees a coherent, complete frame even
if an interrupt fires mid-copy.

### `resetCalibration()`
Initializes every channel's `minVal` to 1023 and `maxVal` to 0, ready to
capture the full range during the next calibration pass.

### `calibrate(uint16_t durationMs)`
For the given duration, repeatedly snapshots raw frames and widens each
channel's min/max to cover everything seen. The result is a per-channel span
that later normalizes that channel to 0..1000 regardless of its absolute
brightness.

### `update()`
The core computation, run once per loop iteration:
1. Snapshots the raw frame.
2. Normalizes each channel to 0..1000 against its own min/max, clamping
   out-of-range values and flipping polarity for a light line if requested.
3. Accumulates the weighted sum and total brightness for the centroid, and
   tracks which sensors are active plus the leftmost/rightmost active index.
4. Classifies the pattern (see **Status model**) and produces the best-effort
   `position`.
5. Updates the `lostFrames` and `junctions` bookkeeping counters.

### `printStatus(SensorStatus s)`
Prints the human-readable name of a status value. Kept separate so the status
can also be consumed programmatically without string handling.

### `setup()`
Starts Serial at 115200, brings up the ADC scan, waits briefly for the first
frame, prompts the user to sweep the array over the line for four seconds,
runs `calibrate(4000)`, and initializes the loop-timing counters.

### `loop()`
Measures the real elapsed time since the previous iteration (`dt`), calls
`update()`, and prints a telemetry line about twenty times per second. The
print rate is throttled so Serial traffic does not dominate the loop.

---

## Status model

Each `update()` reports exactly one state, so downstream logic can branch
cleanly.

| Status | Meaning | Position |
|--------|---------|----------|
| `ON_LINE` | A single solid cluster inside the array. | True centroid — most trustworthy. |
| `EDGE_LEFT` | Cluster confined to the leftmost sensors; line exits left. | Pinned to the outer active weight. |
| `EDGE_RIGHT` | Cluster confined to the rightmost sensors; line exits right. | Pinned to the outer active weight. |
| `JUNCTION` | Two or more separated clusters (a gap between active sensors). | Holds last good value; branch selection is left to you. |
| `LOST` | No sensor sees the line. | Holds last good value. |

A cluster is detected as a *gap* when the span between the leftmost and
rightmost active sensors is wider than the number of active sensors — the
signature of two separate clusters under one array.

---

## Output format

One line every ~50 ms at 115200 baud:

```
<c0> <c1> <c2> <c3> <c4> <c5> <c6> <c7> pos=<p> status=<s> lost=<l> jct=<j> sat=<x> dt_us=<d> loops/s=<r>
```

| Field | Meaning |
|-------|---------|
| `c0..c7` | Calibrated value per sensor, 0..1000. `0` = floor, `1000` = line (for a black line), each normalized against that sensor's own min/max. |
| `pos` | Best-effort line position, integer scaled x1000. Range `-7000..+7000`; `0` = centered. Feed to PID. |
| `status` | Active-sensor pattern classification (see **Status model**). |
| `lost` | Consecutive frames spent in `LOST`. Resets to 0 on any non-lost frame. |
| `jct` | Total `JUNCTION` events seen (counts once per entry into `JUNCTION`). |
| `sat` | `1` if any sensor is saturated (raw near 0 or 1023) — reading is unreliable. |
| `dt_us` | Microseconds since the previous loop iteration (true elapsed time). |
| `loops/s` | Loop-rate estimate (loop counter x 20 for the 50 ms window). |
