#!/usr/bin/env python3
"""Giả lập /dev/sensor_bridge trên PC: ghi frame vào một FIFO để thử sensord.

  mkfifo /tmp/fakedev
  ./sensord -d /tmp/fakedev -H 127.0.0.1 -p 1883 -n test &
  python3 fake_device.py /tmp/fakedev --period-ms 100 --count 200 --corrupt-every 50

Khuôn dạng frame giống esp32_sender.ino:
  AA | LEN | TYPE | SEQ | PAYLOAD | CRC16_L | CRC16_H   (CRC16-CCITT-FALSE trên LEN..PAYLOAD)
"""
import argparse
import struct
import time

SOF = 0xAA


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype: int, seq: int, payload: bytes) -> bytes:
    body = bytes([len(payload), ftype, seq & 0xFF]) + payload
    c = crc16(body)
    return bytes([SOF]) + body + bytes([c & 0xFF, c >> 8])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--period-ms", type=int, default=100)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--corrupt-every", type=int, default=0,
                    help="làm hỏng 1 byte payload của mỗi N frame")
    ap.add_argument("--fragment-at", type=int, default=-1,
                    help="tại frame này chỉ gửi 8 byte đầu rồi bỏ phần còn lại")
    args = ap.parse_args()

    with open(args.path, "wb", buffering=0) as f:
        for i in range(args.count):
            payload = struct.pack("<hHI", 250 + (i % 50), 600 + (i % 100), i * args.period_ms)
            fr = bytearray(build_frame(0x01, i, payload))
            if args.corrupt_every and i % args.corrupt_every == args.corrupt_every - 1:
                fr[6] ^= 0xFF
            if i == args.fragment_at:
                fr = fr[:8]
            f.write(bytes(fr))
            time.sleep(args.period_ms / 1000)


if __name__ == "__main__":
    main()
