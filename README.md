# UART Sensor Gateway (Orange Pi 4 LTS)

A sensor gateway that ingests sensor frames from an ESP32 over UART, buffers them in a custom **Linux kernel driver**, and processes them via a **userspace daemon** that validates frame integrity and publishes structured telemetry to an **MQTT broker**. The entire software stack runs on a custom **Yocto Project** image.

```
 ESP32 (FreeRTOS)             Orange Pi 4 LTS (RK3399, Yocto)
┌──────────────┐  UART4    ┌─────────────────────────────────────────────┐
│ sensor task  │──────────>│ KERNEL                                      │
│ frame + CRC  │ 115200    │  serdev driver (bound via DT) -> receive_buf│
└──────────────┘           │   -> kfifo 4096 B                           │
                           │  /dev/sensor_bridge: blocking read(), poll()│
                           │  sysfs: rx_bytes, dropped, fifo_used, baud  │
                           ├─────────────────────────────────────────────┤
                           │ USERSPACE (user sensord, systemd notify)    │
                           │  sensord: poll() -> parser (CRC16, resync)  │
                           │   -> JSON -> MQTT (libmosquitto)            │
                           └──────────────────────┬──────────────────────┘
                                                  │ WiFi / MQTT
                                           ┌──────┴───────┐
                                           │ Broker (PC)  │
                                           └──────────────┘
```

## Highlights
- **`sensor_bridge` kernel driver**: Implemented with the serdev bus, kfifo buffer, and character device interface; features wait-queue blocking `read()`, `poll()`, `O_NONBLOCK` support, sysfs statistics attributes, and safe teardown handling during device unbind while readers are waiting.
- **`sensord` daemon**: Streaming frame parser with automatic resynchronization upon corrupted or shifted byte streams; MQTT publishing with configurable QoS, retain flags, and Last Will and Testament (LWT); automatic reconnect with backoff; systemd `Type=notify` with watchdog support; runs as a dedicated unprivileged user with systemd hardening.
- **Custom Yocto BSP layer**: Board support package with custom recipes and kernel/device-tree customizations managed cleanly via patch files.
- **Rigorous multi-layer measurement methodology**: End-to-end frame accounting across kernel and userspace layers, validated using deliberate fault injection (bit corruptions, dropped bytes, noise bursts), and measured with ftrace / tracefs event tracing (see [`results.md`](results.md)).
- **Empirical hardware insight**: At ~802 frames/s, the UART hardware FIFO batching engages based on the RX FIFO trigger threshold (33 bytes per interrupt), reducing per-frame CPU overhead from ~0.40 ms to ~0.16 ms (R10).

## Hardware and Wiring
| ESP32 | Orange Pi 4 LTS (26-pin header) |
|---|---|
| TX2 (GPIO17) | Pin 21 (UART4-RX, GPIO1_A7) |
| RX2 (GPIO16) | Pin 19 (UART4-TX, GPIO1_B0) |
| GND | Pin 20 (GND) |

- The dedicated 3-pin debug header (`ttyS2`, 1,500,000 baud) is reserved for the serial console and must not be used for the ESP32. Do not use pins 8/10 (UART2B) as they share the UART controller with the debug console.
- If using an ESP32 WROVER module (with PSRAM), GPIO16/17 are reserved for internal PSRAM; reassign the UART pins in the Arduino sketch.

## Directory Structure
```
├── README.md                          # Project overview and documentation
├── DECISIONS.md                       # Design decisions and architectural trade-offs
├── results.md                         # Benchmark results and empirical measurements
├── LICENSE                            # Dual license file (GPL-2.0-only & MIT)
├── .gitignore
├── setup-layers.sh                    # Helper script to clone required Yocto layers
│
├── build/                             # Reference Yocto build configuration
│   └── conf/
│       ├── bblayers.conf              # BitBake layer configuration
│       └── local.conf                 # BitBake local configuration
│
├── esp32/                             # ESP32 test firmware
│   └── esp32_stress.ino               # Frame generator with runtime error injection
│
├── scripts/                           # Benchmarking and testing scripts (host & target)
│   ├── cpu_stat.sh                    # Multi-core CPU utilization monitor
│   ├── fake_device.py                 # Virtual /dev/sensor_bridge device emulator
│   ├── frame_check.py                 # UART binary frame integrity and CRC analyzer
│   ├── freq_stat.sh                   # CPU frequency and governor transition logger
│   ├── mqtt_stats.py                  # End-to-end MQTT latency and loss monitor
│   ├── sb_watch.sh                    # Sysfs statistics poller for sensor_bridge
│   └── sd_stat.sh                     # Process-level CPU & RSS statistics logger
│
└── meta-rockchip/                     # Custom Yocto BSP and application layer
    ├── conf/
    │   ├── layer.conf
    │   └── machine/
    │       ├── orangepi-4-lts.conf    # Machine definition for Orange Pi 4 LTS
    │       └── include/               # Shared machine includes (rk3399.inc, etc.)
    ├── classes/                       # Custom bbclasses (auto-patch, local-git, rockchip-image)
    ├── recipes-app/
    │   └── sensord/
    │       ├── sensord_0.1.bb         # Sensord application recipe
    │       └── files/
    │           ├── sensord.c          # Userspace daemon source code
    │           ├── sensord.service    # systemd service unit
    │           ├── sensord.default    # Environment defaults (/etc/default/sensord)
    │           └── 99-sensor-bridge.rules # udev device permission rules
    ├── recipes-bsp/
    │   ├── extlinux-conf/             # Bootloader extlinux configuration
    │   └── u-boot/
    │       └── u-boot-orangepi_2024.10.bb
    ├── recipes-connectivity/
    │   └── wifi-config/               # WPA supplicant WiFi configuration
    ├── recipes-kernel/
    │   ├── linux/
    │   │   ├── linux-orangepi_6.6.bb  # Kernel recipe (vendor tree 6.6)
    │   │   ├── linux-rockchip.inc
    │   │   └── files/                 # Kernel config fragments (.cfg) and DT patches
    │   ├── sensor-bridge/
    │   │   ├── sensor-bridge_0.1.bb   # Out-of-tree kernel driver recipe
    │   │   └── files/
    │   │       ├── sensor_bridge.c    # Kernel driver source
    │   │       └── Makefile
    │   └── uwe5622-firmware/          # WiFi/BT firmware files
    └── wic/
        └── orangepi-4-lts.wks         # WIC partition layout
```

## How to Build
- **Target Release:** Yocto Scarthgap.
- **Layers:**
  - `poky` (meta, meta-poky, meta-yocto-bsp)
  - `meta-openembedded` (`meta-oe`, `meta-python`, `meta-networking` for `mosquitto`)
  - `meta-rockchip` (included in this repository)
- **Target Machine:** `orangepi-4-lts`
- **Target Image:** `core-image-minimal`
- **Key local.conf additions:**
  ```bitbake
  MACHINE = "orangepi-4-lts"
  IMAGE_INSTALL:append = " sensor-bridge sensord"
  RM_WORK_EXCLUDE += "linux-orangepi"
  ```

### Build Steps
1. Set up Poky and clone external layers:
   ```bash
   mkdir poky && cd poky
   git clone -b scarthgap git://git.yoctoproject.org/poky.git .
   ../UART-sensor/setup-layers.sh
   ln -s /path/to/UART-sensor/meta-rockchip meta-rockchip
   ```
2. Initialize the OpenEmbedded build environment:
   ```bash
   source oe-init-build-env
   ```
3. Add required layers to `conf/bblayers.conf` (or use `build/conf/bblayers.conf`):
   ```bash
   bitbake-layers add-layer ../meta-openembedded/meta-oe
   bitbake-layers add-layer ../meta-openembedded/meta-python
   bitbake-layers add-layer ../meta-openembedded/meta-networking
   bitbake-layers add-layer /path/to/UART-sensor/meta-rockchip
   ```
4. Merge or copy configuration options from `build/conf/local.conf` into `conf/local.conf`.
5. Build the image:
   ```bash
   bitbake core-image-minimal
   ```
6. Flash the image onto a micro-SD card:
   ```bash
   sudo bmaptool copy tmp/deploy/images/orangepi-4-lts/core-image-minimal-orangepi-4-lts.rootfs.wic.gz /dev/sdX
   # Alternatively using dd:
   # sudo dd if=tmp/deploy/images/orangepi-4-lts/core-image-minimal-orangepi-4-lts.rootfs.wic of=/dev/sdX bs=4M status=progress conv=fsync
   ```

## Interfaces

**UART Frame Format** (little-endian): `AA | LEN | TYPE | SEQ | PAYLOAD | CRC16_L | CRC16_H`
- CRC: CRC16-CCITT-FALSE calculated over `LEN..PAYLOAD`.
- Sensor payload (`TYPE=0x01`, 8 bytes): `int16 temp_x10`, `uint16 hum_x10`, `uint32 uptime_ms`.

**Device Tree** (serdev child node under UART4):
```dts
&uart4 {
	status = "okay";
	sensor_bridge {
		compatible = "gateway,sensor-bridge";
		current-speed = <115200>;
	};
};
```

**Kernel Driver:**
- `/dev/sensor_bridge`: Blocking `read()`, `poll()`, `O_NONBLOCK` (`-EAGAIN`); returns `-ENODEV` and flags `EPOLLHUP` upon driver unbind/device removal.
- Sysfs `/sys/class/sensor_bridge/sensor_bridge/`: `rx_bytes`, `dropped`, `fifo_used`, `baud`.
- Device node created with default permissions `0600 root`; custom udev rule grants group `sensord`, mode `0660`.

**Daemon `sensord`:**
- CLI syntax: `sensord [-d /dev/sensor_bridge] [-H host] [-p port] [-n node] [-q 0|1|2] [-k keepalive]`
- Configuration file: `/etc/default/sensord` (`MQTT_HOST`, `MQTT_PORT`, `NODE_ID`).

**MQTT Topics:**
| Topic | Payload | QoS / Retain |
|---|---|---|
| `gateway/<node>/telemetry` | `{"seq":N,"temp_c":25.3,"hum_pct":60.3,"uptime_ms":123456}` | `-q` (default: 0), retain=0 |
| `gateway/<node>/status` | `online` / `offline` (LWT) | QoS 1, retain=1 |

**systemd Integration:** `Type=notify`, `WatchdogSec=10`, `Restart=on-failure`, `RestartSec=2`, runs as unprivileged user `sensord` with systemd security hardening directives.

## Key Results
Summary of measured empirical values; comprehensive test setups, conditions, and analyses are detailed in [`results.md`](results.md).

| Metric / Scenario | Result | Reference |
|---|---|---|
| Latency from UART interrupt to `sensord` execution (10 frames/s) | Median 372 to 480 µs across runs; disabling deep idle `state2` reduces latency by ~24% | R5 to R7 |
| Maximum throughput | 11,225 bytes/s (~802 frames/s, 97.4% of 115,200 baud theoretical limit), `dropped=0` | R10 |
| `sensord` CPU utilization | 4% at 100 frames/s, 13% to 14% at 802 frames/s | R8 |
| CPU cost per frame | ~0.40 ms → ~0.16 ms once UART byte-batching engages | R8, R10 |
| `sensord` memory footprint | RSS 3,328 kB across all load conditions (no leaks) | R8 |
| Fault recovery | CRC-corrupted and truncated frames are discarded; subsequent valid frames stream cleanly without desync | R2 |
| 4-hour soak test, 20 reboots, stepped fault injection, broker disconnections | See test cases S1 to S8 in `results.md` | S1 to S8 |

## Known Limitations
- **kfifo drops incoming (newest) bytes when full**: Following a long consumer stall, the buffered data represents the oldest received data, potentially terminating in an incomplete frame fragment (the parser recovers automatically).
- **Concurrent readers**: Multiple readers share the byte stream, which can split frames between readers (the driver serializes `read()` via mutex, but does not enforce frame boundaries or exclusive open). Production environments must ensure single-reader access.
- **8-bit sequence number**: The 8-bit `SEQ` counter wraps modulo 256; `lost_by_seq` under-reports frame loss under heavy drop conditions. Accurate loss accounting relies on `uptime_ms`.
- **Unauthenticated MQTT**: The broker runs anonymously for isolated bench testing; TLS and user authentication are not configured.
- **Single-node scope**: Verified with a single ESP32 node and one sensor payload format; out-of-scope features include OTA updates, verified boot, and a web UI.

## Scripts and Tools
| Script / Tool | Description |
|---|---|
| `scripts/frame_check.py` | Analyzes captured binary frame files: CRC validation, gap detection, and loss estimation via `uptime_ms`. |
| `scripts/mqtt_stats.py` | End-to-end telemetry evaluation on PC: loss rate, gap intervals, and packet jitter. |
| `scripts/fake_device.py` | Emulates `/dev/sensor_bridge` character device on a host PC for testing `sensord`. |
| `esp32/esp32_stress.ino` | ESP32 firmware transmitting frames at configurable rates with fault injection via USB serial commands. |
| `scripts/sb_watch.sh` | Polls driver sysfs statistics (`rx_bytes`, `dropped`, `fifo_used`, `baud`). |
| `scripts/sd_stat.sh` | Monitors process-level CPU usage and RSS memory of `sensord`. |
| `scripts/cpu_stat.sh` | Multi-core system CPU utilization monitor (compatible with BusyBox). |
| `scripts/freq_stat.sh` | Logs CPU cluster frequencies and governor transitions. |

## Demo
- `docs/Hardware.jpg`: Wiring diagram for connecting an Orange Pi 4 LTS and an ESP32
- `docs/Kernel&Driver.png`: Verification of Yocto OS, `sensor_bridge` kernel module, and active `sensord` service.
- `docs/Data-mqtt.png`: Real-time MQTT transmission statistics and jitter metrics in JSON format.
- `docs/Data-sensord.png`: `sensord` daemon logs showing successful MQTT connection and error-free frame processing.

## License
This project uses dual licensing:
- Kernel module (`sensor_bridge`): GPL-2.0-only
- Userspace daemon (`sensord`), tools, and scripts: MIT License
