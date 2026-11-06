#!/bin/sh
# sd_stat.sh - CPU% và bộ nhớ của một tiến trình, kèm CPU% từng luồng (BusyBox ash)
#   ./sd_stat.sh [tên-tiến-trình] [giây]     mặc định: sensord, 5 giây
# CPU% tính từ utime+stime trong /proc/<pid>/stat (cộng mọi luồng; có thể >100% trên nhiều lõi),
# giả định CLK_TCK=100 (đặt biến TCK nếu khác). Chỉ tính CPU của CHÍNH tiến trình này:
# phần ngắt, softirq và kworker (ví dụ đường nhận của driver) KHÔNG nằm trong đây (xem cpu_stat.sh).
NAME=${1:-sensord}
I=${2:-5}
TCK=${TCK:-100}

P=$(pidof "$NAME") || { echo "khong tim thay tien trinh $NAME" >&2; exit 1; }
P=${P%% *}
PREV=/tmp/sd_stat.$$.prev
trap 'rm -f "$PREV" "$PREV.new"' EXIT
trap 'exit 0' INT TERM

# Đọc utime+stime từ một file stat. Tên tiến trình/luồng (trường 2, trong ngoặc) có thể chứa
# dấu cách (ví dụ "mosquitto loop"), nên phải cắt bỏ phần đến dấu ") " cuối cùng trước khi tách trường.
stat_ticks() {
	_s=$(cat "$1" 2>/dev/null) || return 1
	_r=${_s##*) }
	set -- $_r
	echo $(( ${12} + ${13} ))
}

ticks() {
	stat_ticks "/proc/$P/stat"
}

snap() {
	for t in /proc/$P/task/*; do
		_tk=$(stat_ticks "$t/stat") || continue
		echo "${t##*/} $_tk"
	done
}

prev=$(ticks)
snap > "$PREV"
echo "gio       cpu_%   vm_rss_kB  threads   (dong tiep theo: CPU% tung luong)"
while [ -r "/proc/$P/stat" ]; do
	sleep "$I"
	[ -r "/proc/$P/stat" ] || break
	cur=$(ticks) || break
	cpu=$(( (cur - prev) * 100 / (TCK * I) ))
	rss=$(grep VmRSS "/proc/$P/status" | tr -s ' ' | cut -d' ' -f2)
	thr=$(grep Threads "/proc/$P/status" | tr -s '\t ' ' ' | cut -d' ' -f2)
	printf '%s %6d %11s %8s\n' "$(date +%H:%M:%S)" "$cpu" "$rss" "$thr"
	prev=$cur

	snap > "$PREV.new"
	line=""
	while read -r tid tk; do
		pk=$(grep "^$tid " "$PREV" | cut -d' ' -f2)
		pk=${pk:-0}
		comm=$(cat "/proc/$P/task/$tid/comm" 2>/dev/null)
		line="$line   $tid($comm)=$(( (tk - pk) * 100 / (TCK * I) ))%"
	done < "$PREV.new"
	echo "         $line"
	mv "$PREV.new" "$PREV"
done
