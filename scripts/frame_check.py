#!/usr/bin/env python3
"""Kiểm tra frame UART của ESP32 (chạy trên PC).

  python3 frame_check.py --selftest
  python3 frame_check.py /dev/ttyUSB0 [baud]     # đọc trực tiếp từ serial (cần pyserial)
  python3 frame_check.py --file capture.bin      # phân tích file bắt từ OPi

Frame: AA | LEN | TYPE | SEQ | PAYLOAD[LEN] | CRC16_L | CRC16_H
CRC16-CCITT-FALSE trên LEN..PAYLOAD.
"""
import struct
import sys
from collections import Counter

SOF = 0xAA
MAX_LEN = 64
TYPE_SENSOR = 0x01


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype: int, seq: int, payload: bytes) -> bytes:
    body = bytes([len(payload), ftype, seq]) + payload
    c = crc16(body)
    return bytes([SOF]) + body + bytes([c & 0xFF, c >> 8])


class Parser:
    def __init__(self):
        self.buf = bytearray()
        self.ok = 0
        self.crc_err = 0
        self.bad_len = 0
        self.skipped = 0      # byte bị bỏ khi tìm SOF
        self.lost = 0         # frame mất theo SEQ
        self.last_seq = None

    def feed(self, data: bytes):
        self.buf += data
        frames = []
        while True:
            i = self.buf.find(SOF)
            if i < 0:
                self.skipped += len(self.buf)
                self.buf.clear()
                break
            if i > 0:
                self.skipped += i
                del self.buf[:i]
            if len(self.buf) < 2:
                break
            ln = self.buf[1]
            if ln > MAX_LEN:
                self.bad_len += 1
                del self.buf[:1]          # resync: bỏ SOF giả
                continue
            total = 4 + ln + 2
            if len(self.buf) < total:
                break
            frame = bytes(self.buf[:total])
            rx = frame[-2] | (frame[-1] << 8)
            if crc16(frame[1:-2]) != rx:
                self.crc_err += 1
                del self.buf[:1]          # resync: bỏ SOF giả
                continue
            del self.buf[:total]
            seq = frame[3]
            if self.last_seq is not None:
                self.lost += (seq - self.last_seq - 1) & 0xFF
            self.last_seq = seq
            self.ok += 1
            frames.append((seq, frame[2], frame[4:-2]))
        return frames

    def summary(self) -> str:
        return (f"ok={self.ok} crc_err={self.crc_err} bad_len={self.bad_len} "
                f"skipped_bytes={self.skipped} lost_by_seq={self.lost}")


_last = {"seq": None, "up": None, "n": 0, "first_gap_at": None}
_up = {"first": None, "last": None, "deltas": Counter()}


def show(frames):
    for seq, ftype, payload in frames:
        if ftype == TYPE_SENSOR and len(payload) == 8:
            t, h, up = struct.unpack("<hHI", payload)
            # SEQ chỉ 8 bit nên lost_by_seq bị quay vòng mod 256; uptime thì không.
            if _last["seq"] is not None and seq != (_last["seq"] + 1) & 0xFF:
                if _last["first_gap_at"] is None:
                    _last["first_gap_at"] = _last["n"]
                print(f"  ^^ GAP sau {_last['n']} frame hop le: seq {_last['seq']}->{seq}, "
                      f"uptime +{up - _last['up']} ms")
            if _up["first"] is None:
                _up["first"] = up
            if _last["seq"] is not None and seq == (_last["seq"] + 1) & 0xFF:
                _up["deltas"][up - _last["up"]] += 1
            _up["last"] = up
            _last.update(seq=seq, up=up, n=_last["n"] + 1)
            print(f"seq={seq:3d} temp={t/10:.1f}C hum={h/10:.1f}% uptime={up}ms")
        else:
            print(f"seq={seq:3d} type=0x{ftype:02x} payload={payload.hex()}")


def uptime_report():
    """Ước lượng số frame đã gửi từ uptime (không bị quay vòng như SEQ 8 bit)."""
    if not _up["deltas"] or _up["first"] is None:
        return
    period = _up["deltas"].most_common(1)[0][0]
    if period <= 0:
        return
    span = _up["last"] - _up["first"]
    expected = span // period + 1
    print(f"uptime: span={span} ms, chu ky pho bien={period} ms, "
          f"du kien ~{expected} frame trong khoang nay")


def selftest():
    assert crc16(b"123456789") == 0x29B1, "CRC16-CCITT-FALSE sai"
    p = Parser()
    f1 = build_frame(TYPE_SENSOR, 0, struct.pack("<hHI", 251, 601, 1000))
    f2 = build_frame(TYPE_SENSOR, 1, struct.pack("<hHI", 252, 602, 2000))
    bad = bytearray(build_frame(TYPE_SENSOR, 2, struct.pack("<hHI", 253, 603, 3000)))
    bad[6] ^= 0xFF                       # làm hỏng payload
    f4 = build_frame(TYPE_SENSOR, 4, struct.pack("<hHI", 255, 605, 5000))  # mất seq 3
    stream = b"\x00\x11" + f1 + f2 + bytes(bad) + f4
    # đưa từng byte một: parser phải xử lý frame bị cắt giữa chừng
    frames = []
    for b in stream:
        frames += p.feed(bytes([b]))
    assert [f[0] for f in frames] == [0, 1, 4], frames
    # frame hỏng: crc_err=1; seq 2 (hỏng) và 3 (mất) -> lost_by_seq=2
    assert p.crc_err == 1 and p.lost == 2 and p.skipped >= 2, p.summary()
    print("selftest OK:", p.summary())


def main(argv):
    if len(argv) >= 2 and argv[1] == "--selftest":
        selftest()
        return
    p = Parser()
    if len(argv) >= 3 and argv[1] == "--file":
        with open(argv[2], "rb") as fh:
            show(p.feed(fh.read()))
        print(p.summary())
        uptime_report()
        return
    if len(argv) >= 2:
        import serial  # pip install pyserial
        baud = int(argv[2]) if len(argv) >= 3 else 115200
        ser = serial.Serial(argv[1], baud, timeout=0.5)
        try:
            while True:
                data = ser.read(256)
                if data:
                    show(p.feed(data))
        except KeyboardInterrupt:
            print("\n" + p.summary())
            uptime_report()
        return
    print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
