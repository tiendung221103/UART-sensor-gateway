#!/bin/sh
# sb_watch.sh - theo dõi thống kê sensor_bridge trong sysfs (chạy được trên BusyBox ash)
#   ./sb_watch.sh [giây]        mặc định 1 giây; Ctrl+C để dừng
# Đổi thư mục sysfs bằng biến SB_SYSFS (dùng để thử với dữ liệu giả).
D=${SB_SYSFS:-/sys/class/sensor_bridge/sensor_bridge}
I=${1:-1}

[ -r "$D/rx_bytes" ] || { echo "khong doc duoc $D/rx_bytes (driver da nap chua?)" >&2; exit 1; }

prev_rx=$(cat "$D/rx_bytes")
prev_dr=$(cat "$D/dropped")
echo "gio       rx_tong     rx_B/s  dropped_tong  dropped_B/s  fifo_used"
while :; do
	sleep "$I"
	rx=$(cat "$D/rx_bytes")
	dr=$(cat "$D/dropped")
	fu=$(cat "$D/fifo_used")
	printf '%s %10d %10d %13d %12d %10d\n' "$(date +%H:%M:%S)" "$rx" $(( (rx - prev_rx) / I )) \
		"$dr" $(( (dr - prev_dr) / I )) "$fu"
	prev_rx=$rx
	prev_dr=$dr
done
