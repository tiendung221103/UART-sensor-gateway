#!/bin/sh
# cpu_stat.sh - % bận của từng CPU toàn hệ thống, tách user/sys/irq/softirq (BusyBox ash)
#   ./cpu_stat.sh [giây]       mặc định 5 giây
# Khác sd_stat.sh: tính CẢ ngắt, softirq và các kworker (ví dụ đường nhận của driver), không chỉ sensord.
I=${1:-5}
PREV=/tmp/cpu_stat.$$.prev
trap 'rm -f "$PREV" "$PREV.new"' EXIT
trap 'exit 0' INT TERM

grep '^cpu[0-9]' /proc/stat > "$PREV"
echo "gio       cpu   busy%  user%   sys%   irq%  softirq%"
while :; do
	sleep "$I"
	grep '^cpu[0-9]' /proc/stat > "$PREV.new"
	t=$(date +%H:%M:%S)
	while read -r name u n s idle iow irq sirq st rest; do
		set -- $(grep "^$name " "$PREV")
		pu=$2; pn=$3; ps=$4; pidle=$5; piow=$6; pirq=$7; psirq=$8; pst=$9
		dt=$(( (u+n+s+idle+iow+irq+sirq+st) - (pu+pn+ps+pidle+piow+pirq+psirq+pst) ))
		[ "$dt" -gt 0 ] || dt=1
		busy=$(( dt - (idle - pidle) - (iow - piow) ))
		printf '%s %-5s %5d %6d %6d %6d %9d\n' "$t" "$name" $(( busy*100/dt )) \
			$(( (u-pu + n-pn)*100/dt )) $(( (s-ps)*100/dt )) $(( (irq-pirq)*100/dt )) $(( (sirq-psirq)*100/dt ))
	done < "$PREV.new"
	mv "$PREV.new" "$PREV"
done
