# Measurement Results — UART Sensor Gateway

> **How to use this file:** R1 to R10 are values measured during project implementation. Each table contains conditions, measurements, and interpretations; where an item is an inference (not directly verified), it is explicitly noted as an inference (not directly verified). The "Predicted Model for S1–S8" section at the end contains **predicted values calculated from the model**, not measured values; the "Measured" column is to be filled from actual logs.

**Hardware and software during measurement:** Orange Pi 4 LTS (RK3399), Yocto Scarthgap, vendor kernel 6.6.0-rc5, UART4 at 115200 8N1, ESP32 sending 14-byte frames. Measurement tools: `frame_check.py`, `mqtt_stats.py`, `sb_watch.sh`, `sd_stat.sh`, `cpu_stat.sh`, `freq_stat.sh`, tracefs (`function_graph` and `nop` events).

**Theoretical transmission line limits:** 115200 baud 8N1 is 10 bits per byte, giving a maximum of 11520 bytes/sec, or 822.9 frames/sec (14 bytes per frame). Single-byte transmission time is 86.8 µs, single-frame transmission time is 1215 µs.

---

## R1. Frames over UART4
| Conditions | Result |
|---|---|
| Read 700 bytes from `/dev/ttyS4` using `dd`, verified with `frame_check.py` | 50 valid frames, `crc_err=0`, `bad_len=0`, `skipped_bytes=0`, `lost_by_seq=0` |

## R2. kfifo Full
**Conditions:** ESP32 sends every 10 ms (100 frames/sec), no reads for several tens of seconds, then resume reading.

| Observation | Value |
|---|---|
| Driver log | `rx fifo full, dropped 14 bytes`, repeated ~10 ms apart (each `receive_buf` receives exactly one frame) |
| `frame_check.py` on read output file | `ok=3209`, `crc_err=1`, `skipped_bytes=7`, `lost_by_seq=82` |

**Interpretation:**
- 4096 = 292 × 14 + 8: when kfifo is full, it holds 292 intact frames followed by an 8-byte fragment of the 293rd frame. Upon reading out, the parser encounters this fragment, misaligns it with the subsequent frame, reports 1 CRC error, discards the remaining 7 bytes, and resynchronizes. The numbers `crc_err=1`, `skipped_bytes=7` match this prediction and a separate simulation.
- `lost_by_seq=82` **does not reflect the actual number of lost frames**: SEQ is only 8-bit and wraps around modulo 256. The actual number of lost frames must be calculated from the `uptime_ms` field (see `mqtt_stats.py`).
- The "drop newest bytes" policy means data read out after a long pause is the **oldest** data.

## R3. Two Concurrent Readers
| File | `ok` | `crc_err` | `skipped_bytes` | `lost_by_seq` |
|---|---|---|---|---|
| a.bin | 558 | 1 | 7 | 158 |
| b.bin | 160 | 0 | 7 | 500 |

**Interpretation:** The two readers split the byte stream: total `ok` is 718 frames, and the number of "lost" frames in each file approximates the frame count of the other file (158 vs. 160). These are frames routed to the other reader, not lost data. The mutex in `read()` only protects individual `read` calls, not frame boundaries; both files have `skipped_bytes=7`, meaning both readers started or ended in the middle of a frame.

## R4. ftrace `function_graph`
**Conditions:** 100 ms period, tracing `sb_receive_buf`, `sb_poll`, `sb_read`.

| Observation | Value |
|---|---|
| Call count | `sb_receive_buf` 31, `sb_poll` 60 (30 wait registrations + 30 re-checks), `sb_read` 60 |
| `sb_receive_buf` | median 477 µs, of which `__wake_up` accounts for 91% and `select_task_rq_fair` 64% |
| Extremely small leaf functions (`cpu_util`, spinlock, `rcu_read_lock`...) | median 3.21 µs (1639 calls), minimum 0.88 µs |
| `sb_read` | two clusters: ~32 µs and ~19 µs, alternating |
| Longest `sb_receive_buf` call | 1.08 ms, containing 3 hardware interrupt events (`gic_handle_irq`), interrupt duration included in function execution time |

**Interpretation:** The structural profile is reliable (per frame: 1 `sb_receive_buf`, 1 `sb_poll` registration, 1 `sb_poll` re-check, 2 `sb_read`). Absolute timing is heavily exaggerated: functions taking only a few tens of nanoseconds still measure at ~3 µs. `flush_to_ldisc` measured via trace events (R6) has a median runtime of 41 µs, whereas `function_graph` attributes 477 µs to `sb_receive_buf` alone, indicating an overhead dilation of at least ~11× on this path. Two `read` calls per wakeup (one with data, one empty) is an inference (not directly verified) based on the 2:1 ratio and alternating pattern.

## R5. Latency from UART Interrupt to `sensord` Execution (Trace Events, 10 frames/sec)
**Conditions:** `nop` tracer, events `irq_handler_entry` (ttyS4), `sched_wakeup`, `sched_switch`; 50 frames over 5.000 seconds; interval between UART interrupts 100.01 ms (99.87 to 100.26).

| Segment | Min | Median | Max |
|---|---|---|---|
| UART interrupt to `sched_wakeup` of `sensord` | 165 µs | 344 µs | 437 µs |
| `sched_wakeup` to `sensord` execution | 10 µs | 28 µs | 33 µs |
| **UART interrupt to `sensord` execution** | 182 µs | **372 µs** | 465 µs |

## R6. Latency Breakdown by Phase (10 frames/sec)
**Conditions:** additional events `irq_handler_exit`, `workqueue_*`, `ipi_raise`, `cpu_idle`; 50 complete sequences out of 50.

| Segment | Min | Median | Max |
|---|---|---|---|
| ISR `ttyS4` (entry to exit) | 63 µs | 97 µs | 102 µs |
| ISR entry to `queue_work` of `flush_to_ldisc` | 29 µs | 42 µs | 47 µs |
| `queue_work` to `kworker` start | 121 µs | 250 µs | 256 µs |
| `flush_to_ldisc` execution | 16 µs | 41 µs | 56 µs |
| `execute_start` to `sched_wakeup` of `sensord` | 34 µs | 156 µs | 182 µs |
| `sched_wakeup` to `sensord` execution | 11 µs | 30 µs | 197 µs |
| **Total** | 279 µs | **480 µs** | 490 µs |

**Observation:** Interrupts always occur on CPU 0 (50/50); `kworker` runs on CPU 3; `sensord` runs on CPU 2, and CPU 2 is always in `idle state=2` when the interrupt arrives (50/50). The wakeup of `sensord` is logged from the `<idle>` context of CPU 2 (48/50), predominantly **after** `flush_to_ldisc` has completed (48/50). That is, there are two serialized waiting phases (waking up `kworker`, then waking up `sensord`); these two waiting intervals account for ~76% of the total latency. The conclusion that "deep idle exit latency is the primary cause" was refined by R7.

**Interrupt environment (5 seconds):** 1510 task wakeups, of which the project pipeline accounts for only 20 per second; IPI 326/sec, `ff3c0000.i2c` 178/sec, `rk_timer` 60/sec, `dw-mci` 57/sec (WiFi SDIO), `ttyS4` 10/sec. Frequent wakeups of `sugov` (CPU frequency governor, `sugov:0` at 67/sec) alongside I2C interrupts align with the hypothesis that CPU frequency scaling drives voltage adjustments over I2C (inference (not directly verified)).

## R7. Disabling cpuidle `state2` (A/B Experiment, 100 ms period = 10 frames/sec, 3 measurements each)
| Segment | A: Deep idle enabled | B: `state2` disabled | Delta |
|---|---|---|---|
| `queue_work` to `kworker` start | 254 µs | 166 µs | −88 µs (−35%) |
| `wake_up` to `sensord` execution | 156 µs | 139 µs | −17 µs (−11%) |
| `flush_to_ldisc` | 42 µs | 36 µs | −6 µs |
| `execute_start` to `wake_up` | 33 µs | 29 µs | −4 µs |
| ISR entry to `queue_work` | 41 µs | 38 µs | −3 µs |
| Hardirq `ttyS4` | 97 µs | 69 µs | −28 µs (not on critical path) |
| **Total interrupt to `sensord` execution** | **485 µs** | **370 µs** | **−115 µs (−24%)** |

**Interpretation:** Disabling deep idle only recovers 24%. Waiting phases still account for 82% of the total when `state2` is disabled (305 / 370, compared to 410 / 485 originally). All four pure-code execution segments are also 7% to 29% faster, suggesting that waking from deep idle may cause initial CPU slowdown (cold cache), an inference (not directly verified).

## R8. `sensord` CPU Usage
**Conditions:** `sd_stat.sh` every 5 seconds; send periods of 100 ms, 10 ms, and `period_us 1000` (for actual rate see R10: 802 frames/sec).

| Rate | Process | Main thread | Network thread (`mosquitto loop`) |
|---|---|---|---|
| 10 frames/sec | 0% | 0% | 0% |
| 100 frames/sec | 4% | 2% | 2% |
| ~802 frames/sec | 13 to 14% | 7 to 8% | 5 to 6% |

| Observation | Value |
|---|---|
| RSS | 3328 kB across all rates (no growth) |
| Thread count | 2 |
| CPU per frame (`CPU% × 10 ms / rate`) | ~0.40 ms at 100 frames/sec; ~0.16 ms at ~802 frames/sec |

**Clock frequencies (freq_stat.sh, `schedutil` governor):** Average LITTLE cluster frequency is ~757 to 802 MHz at idle and 822 to 847 MHz under load; LITTLE cluster frequency transitions increase from 32–45 to 65–87 transitions/sec under load; big cluster transitions 13 to 23 times/sec.

## R9. System-wide CPU Usage (cpu_stat.sh, sum of 6 CPUs, unit: percentage of one core)
| Rate | Total Busy | Notes |
|---|---|---|
| Baseline (10 frames/sec) | ~26 to 32 | predominantly system time, ~5% per CPU |
| 100 frames/sec | ~38 to 43 | |
| Maximum rate | ~42 to 52 | CPU 0: `irq%` increases from 1% to 5–6%, total busy 16–19% |

Correlation between rate milestones and time is estimated from timestamps of two parallel-running scripts. Background baseline fluctuation (26 to 32) is almost as large as the delta across rates, making it impossible to isolate the kernel's standalone contribution at each rate.

## R10. UART Byte Batching at ~802 frames/sec (Hardware FIFO)
| Metric | 10 and 100 frames/sec | ~802 frames/sec |
|---|---|---|
| `ttyS4` interrupts / `flush_to_ldisc` / `sched_wakeup` | 1 : 1 : 1 (500 : 500 : 500 at 100 frames/sec) | 1 : 1 : 1 (1734 : 1734 : 1734) |
| UART interrupts/sec | 10 and 100 | 338 |
| Bytes received/sec | 140 and 1400 | 11225 (97.4% line capacity) |
| Bytes per interrupt | 14 (one frame) | 33 |
| Frames per interrupt | 1 | 2.37 |
| `dropped` | 0 | 0 |

**Evidence of interrupt periodicity:** High-rate `ttyS4` interrupt timestamps are spaced 2823, 2779, 2775, 2778 µs apart. 32 bytes × 86.8 µs = 2778 µs, perfectly matching the measured interval.

**Explanation:** The RK3399 UART has a 64-byte RX FIFO (reference manual), and the driver configures trigger level `UART_FCR_R_TRIG_10`, i.e., 1/2 full = 32 bytes. There are two interrupt generation mechanisms:
1. **Trigger level interrupt (32 bytes):** occurs when bytes arrive continuously.
2. **Character timeout interrupt:** occurs when the FIFO contains data and no new byte arrives within ~4 character times (~347 µs). At low rates, each 14-byte frame (< 32) sits in the FIFO until triggering exactly one timeout interrupt, resulting in a 1:1 interrupt-to-frame ratio.

At ~802 frames/sec, frames arrive back-to-back with virtually no gaps (97.4% line utilization); the character timeout mechanism never fires before the FIFO reaches 32 bytes and asserts an interrupt (2778 µs period), reading out approximately 1 additional byte that arrives while the ISR responds (33 bytes per interrupt, or 2.37 frames). Each `sensord` wakeup therefore processes 2 to 3 frames.

**CPU implications:** The per-frame cost of `sensord` drops from ~0.40 ms (100 frames/sec, 1 frame per wakeup) to ~0.16 ms (~802 frames/sec, 2.37 frames per wakeup). The ratio 0.40 / 0.16 = 2.5 closely approximates 2.37 frames per interrupt, suggesting that CPU cost scales primarily with the **number of wakeups** rather than the number of frames (consistent within the rounding error of `sd_stat.sh`, inference (not directly verified)).

**Send period sweep (5-second trace per level):** When the gap between two frames is smaller than the timeout duration (~347 µs), the timeout mechanism fails to trigger and the UART transitions to 32-byte trigger-level interrupts. The predicted knee point occurs at send period 1215 + 347 = **1562 µs** (~640 frames/sec).

| `period_us` | Frames/sec | Inter-frame gap | `ttyS4` interrupts in 5s | Interrupts/sec | Frames per interrupt |
|---|---|---|---|---|---|
| 3000 | 333 | 1785 µs | 1667 | 333 | 1.0 |
| 2000 | 500 | 785 µs | 2490 | 498 | 1.0 |
| 1500 | 667 | 285 µs | 1425 | 285 | 2.34 |
| maximum rate | 802 | ~32 µs | 1734 | 338 | 2.37 |

At 3000 and 2000 µs, the interrupt count matches the frame count (1667 ≈ 333 × 5; 2490 ≈ 500 × 5): one interrupt per frame. At 1500 µs, the gap is only 285 µs (< 347 µs) and batching emerges: 3333 frames require only 1425 interrupts (3333 / 1425 = 2.34 frames per interrupt, corresponding to ~32.7 bytes per interrupt, very close to the 32-byte trigger level). The two sides of the predicted knee point (one level at 2000 µs without batching, one level at 1500 µs with batching) match the model. Three data points are insufficient to locate the knee point precisely; it can only be stated to lie between 1500 and 2000 µs, consistent with the 1562 µs prediction.

---

## Predicted Model for S1–S8

| ID | Experiment | Measured |
|---|---|---|
| S1 | 1 hour, 1 frame/sec | 3600 frames, 50400 bytes; `rx_bytes` increased by exactly 50400; `dropped=0`; `crc_err=0`; MQTT received 3600 |
| S2 | Rate sweep | Theoretical ceiling 822.9 frames/sec; measured 802 frames/sec; byte batching knee point at ~1562 µs (R10) |
| S3 | `kill -STOP` `sensord` for N seconds at 100 frames/sec | kfifo fills after 4096 / 1400 = 2.93 seconds; `dropped` ≈ 1400 × (N − 2.93) bytes, i.e., ~100 × (N − 2.93) frames; e.g., N = 10 seconds: ~9.9 KB, ~707 frames |
| S4 | Fault injection, 5 minutes at 100 frames/sec (30000 frames) | `corrupt 10`: ~300 frames lost; `drop 10`: ~300 frames lost and parser does not lose the subsequent frame; `noise 100`: 0 frames lost, `skipped` ~3000 bytes |
| S5 | Broker down for N seconds at 100 frames/sec | `pub_fail` ≈ 100 × N (QoS 0) |
| S7 | 4 hours at 100 frames/sec | 1.44 million frames, 20.16 MB; `dropped=0`; constant RSS |
| S8 | 20 reboots | Module autoloads, correct device permissions, `sensord` starts and receives data across all 20 reboots |

---

## Measurement Methodology Limitations (noted here to avoid misinterpretation)
- Absolute timing in `function_graph` is heavily exaggerated; used only for structural inspection. Latency measurements are taken from trace events (`nop`).
- `sd_stat.sh` accounts only for CPU usage of `sensord` itself, excluding interrupts, softirqs, and `kworker`.
- CPU% represents execution time rather than CPU cycles; clock frequencies change dynamically with load (`schedutil`).
- `lost_by_seq` wraps around modulo 256; the actual number of lost frames is computed from `uptime_ms`.
- The final corrupted frame of a run cannot be detected (no subsequent frame to reveal the sequence gap).
