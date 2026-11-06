#!/usr/bin/env python3
"""Đo đầu-cuối trên PC: subscribe telemetry của sensord và thống kê mất mát, khoảng trống, jitter.

  pip install paho-mqtt
  python3 mqtt_stats.py --host 192.168.1.100 --duration 600 --every 10

Cách tính (không dùng SEQ 8 bit vì quay vòng):
  - chu kỳ = giá trị phổ biến nhất của chênh lệch uptime_ms giữa hai frame liên tiếp
  - số frame dự kiến = (uptime cuối - uptime đầu) / chu kỳ + 1, theo từng đoạn chạy liên tục
    (uptime giảm nghĩa là ESP32 khởi động lại -> bắt đầu đoạn mới)
  - mất = dự kiến - nhận được
  - jitter = | chênh lệch thời điểm nhận - chênh lệch uptime | giữa hai frame liền nhau không bị mất
    (gồm cả dao động của ESP32, mạng và broker; không phải độ trễ tuyệt đối)
"""
import argparse
import json
import statistics
import sys
import time
from collections import Counter

import paho.mqtt.client as mqtt


class Collector:
    def __init__(self):
        self.segments = []      # mỗi đoạn: list[(arrival_ms, uptime_ms)]
        self.bad = 0

    def add(self, arrival_ms, up):
        if not self.segments or up < self.segments[-1][-1][1]:
            self.segments.append([])
        self.segments[-1].append((arrival_ms, up))

    def summary(self):
        recv = sum(len(s) for s in self.segments)
        deltas = [b[1] - a[1] for s in self.segments for a, b in zip(s, s[1:]) if b[1] > a[1]]
        if not deltas:
            return {"recv": recv, "note": "chua du du lieu"}
        period = Counter(deltas).most_common(1)[0][0]
        expected = sum((s[-1][1] - s[0][1]) // period + 1 for s in self.segments)
        gaps = [d // period - 1 for d in deltas if d > 1.5 * period]
        jit = [abs((b[0] - a[0]) - (b[1] - a[1]))
               for s in self.segments for a, b in zip(s, s[1:]) if b[1] - a[1] == period]
        out = {
            "recv": recv, "period_ms": period, "expected": expected,
            "lost": expected - recv,
            "loss_pct": round(100.0 * (expected - recv) / expected, 3) if expected else 0.0,
            "gap_events": len(gaps), "max_gap_frames": max(gaps) if gaps else 0,
            "restarts": len(self.segments) - 1,
        }
        if len(jit) >= 20:
            jit.sort()
            out["jitter_ms"] = {
                "mean": round(statistics.fmean(jit), 2),
                "p95": round(jit[int(0.95 * (len(jit) - 1))], 2),
                "max": round(jit[-1], 2),
            }
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="gateway/+/telemetry")
    ap.add_argument("--duration", type=float, default=0, help="giây; 0 = đến khi Ctrl+C")
    ap.add_argument("--every", type=float, default=10, help="in tóm tắt mỗi N giây")
    args = ap.parse_args()

    col = Collector()
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:               # paho-mqtt 1.x
        client = mqtt.Client()

    def on_connect(c, userdata, flags, rc, *extra):
        c.subscribe(args.topic, qos=0)

    def on_message(c, userdata, msg):
        t = time.monotonic() * 1000.0
        try:
            col.add(t, int(json.loads(msg.payload)["uptime_ms"]))
        except (ValueError, KeyError, TypeError):
            col.bad += 1

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, 30)
    client.loop_start()

    t0 = last = time.monotonic()
    try:
        while True:
            time.sleep(0.2)
            now = time.monotonic()
            if now - last >= args.every:
                last = now
                print(json.dumps(col.summary()), flush=True)
            if args.duration and now - t0 >= args.duration:
                break
    except KeyboardInterrupt:
        pass
    client.loop_stop()
    print("== tong ket ==")
    print(json.dumps(col.summary(), indent=2))
    if col.bad:
        print("payload khong hop le:", col.bad, file=sys.stderr)


if __name__ == "__main__":
    main()
