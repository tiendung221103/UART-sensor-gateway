#!/bin/sh
# freq_stat.sh - tần số trung bình của từng cụm CPU (cpufreq policy) và số lần đổi tần số (BusyBox ash)
#   ./freq_stat.sh [giây]       mặc định 5 giây
# Dùng cpufreq/stats/time_in_state (nếu kernel có CONFIG_CPU_FREQ_STAT) để tính tần số TRUNG BÌNH
# theo thời gian trong mỗi khoảng. Nếu không có, chỉ in tần số tại thời điểm đọc (scaling_cur_freq).
# Đổi thư mục gốc bằng biến CPUFREQ_DIR (dùng để thử với dữ liệu giả).
D=${CPUFREQ_DIR:-/sys/devices/system/cpu/cpufreq}
I=${1:-5}
T=/tmp/freq_stat.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
trap 'exit 0' INT TERM

avg() {		# $1 = snapshot trước, $2 = snapshot hiện tại
	tot=0; wsum=0
	while read -r f t; do
		pt=$(grep "^$f " "$1" | cut -d' ' -f2)
		pt=${pt:-0}
		dt=$(( t - pt ))
		tot=$(( tot + dt ))
		wsum=$(( wsum + f * dt ))
	done < "$2"
	if [ "$tot" -gt 0 ]; then echo $(( wsum / tot )); else echo 0; fi
}

for p in "$D"/policy*; do
	n=${p##*/}
	cat "$p/stats/time_in_state" > "$T/$n.prev" 2>/dev/null
	cat "$p/stats/total_trans" > "$T/$n.trans" 2>/dev/null
done
echo "gio       policy    tan_so_TB_kHz  tan_so_hien_tai_kHz  doi_tan_so/giay"
while :; do
	sleep "$I"
	t=$(date +%H:%M:%S)
	for p in "$D"/policy*; do
		n=${p##*/}
		cur=$(cat "$p/scaling_cur_freq" 2>/dev/null)
		if [ -r "$p/stats/time_in_state" ]; then
			cat "$p/stats/time_in_state" > "$T/$n.cur"
			a=$(avg "$T/$n.prev" "$T/$n.cur")
			mv "$T/$n.cur" "$T/$n.prev"
			tr=$(cat "$p/stats/total_trans" 2>/dev/null)
			ptr=$(cat "$T/$n.trans" 2>/dev/null)
			rate=$(( (${tr:-0} - ${ptr:-0}) / I ))
			echo "${tr:-0}" > "$T/$n.trans"
		else
			a="-"; rate="-"
		fi
		printf '%s %-9s %13s %20s %16s\n' "$t" "$n" "$a" "$cur" "$rate"
	done
done
