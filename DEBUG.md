# Debug — LazyDragon Oculus-8 Analog IR Sensor Array

What to expect while running the **LazyDragon Oculus-8 analog IR sensor
array**, and what each odd value means. For hardware and a quick start see
[`README.md`](README.md); for the code walkthrough see [`MANUAL.md`](MANUAL.md).

---

## Expected behaviour

**Boot:** prints `Sweep the array across the line for 4 seconds...`, then
`Calibration done.` after four seconds. During the sweep, slide the array
side to side over the line so every sensor sees both floor and line.

**Normal run (array centered on line):**
- A few middle sensors show high `c` values (near 1000); outer ones near 0.
- `status = ON_LINE`, `pos` near 0, `lost = 0`, `sat = 0`.
- `dt_us` small (loop is not blocking) and `loops/s` high.

**Slide the array left so the line leaves the left edge:**
- `status` goes `ON_LINE -> EDGE_LEFT`, `pos -> -7000`, then `-> LOST` (pos holds).
- `lost` counts up while off the line and resets to 0 when the line returns.

**Move the array across a T / Y junction (two lines under the array at once):**
- `status = JUNCTION`, `jct` increments once per entry, `pos` holds.

---

## Debug guide

**`pos` stuck at -7000 or +7000 (or an extreme) while `status = EDGE_*`.**
- Line is genuinely off that edge, or one sensor's calibration is wrong.
- Check the `c` values: if the edge sensor reads high while the line looks
  centered, re-run calibration.

**`status = LOST` constantly, even on the line.**
- `ON_THRESHOLD` too high, or calibration didn't capture the line (sweep too
  slow / never crossed the line). Lower `ON_THRESHOLD` and re-calibrate.

**`sat = 1` constantly.**
- A sensor is pegged at ~0 or ~1023, so the reading is clipped and `pos` is
  biased. Causes: sensor too close/far from the floor, IR brightness out of
  range, or a broken/loose sensor. Fix spacing/power, or widen the light level
  so sensors read comfortably inside 100..900.

**`pos` flips sign wildly / jumps between large + and − values.**
- Usually a saturated or noisy sensor dominating the centroid. Check `sat`.
- Also check for a loose wire making a sensor bounce between 0 and 1023.

**`status = JUNCTION` a lot on a simple straight line.**
- A middle sensor reads LOW while its neighbours read HIGH (a dead spot),
  creating a fake gap. Check the `c` values: if one middle sensor is near 0
  while neighbours are near 1000, it is damaged/miswired, or the line is very
  thin. A thicker line or a lower `ON_THRESHOLD` reduces false junctions.

**`dt_us` is huge / `loops/s` is low.**
- The loop is blocking. In this sketch the only blocker is Serial printing
  (115200 baud). If you later add `analogRead()`, `delay()`, or other blocking
  calls, `dt_us` will spike. Keep the loop free of blocking calls for a fast
  robot.

**`c` values are all 0 or all 1000.**
- Calibration min/max are equal or the span is degenerate. This happens if the
  array never saw both floor and line during the sweep, or sensors are
  electrically stuck. Re-run calibration and check the sweep covers the full
  range.

**`c` values don't change as you move the array.**
- ISR not running (ADC never completing). Check that `adcSetup()` was called
  and interrupts are enabled. Also ensure no other code calls `analogRead()`.

---

## Reminders

- Do **not** call `analogRead()` anywhere while this sketch owns the ADC.
- `A0` is the leftmost channel and `A7` the rightmost; if mirrored, negate
  `SENSOR_WEIGHT`.
- `pos` is always a steerable number, but trust it most in `ON_LINE` and least
  in `LOST` / `JUNCTION`.
