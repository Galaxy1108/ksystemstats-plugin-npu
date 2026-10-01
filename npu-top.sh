#!/usr/bin/env bash
# npu-top.sh — Intel NPU (intel_vpu) 实时占用监控
#
# 用法:
#   ./npu-top.sh          # 每 1 秒刷新
#   ./npu-top.sh 0.2      # 每 0.2 秒刷新
#
# 说明:
#   所有数据直接读自 /sys/class/accel/accel0/device/ ，无需 root、无需装任何工具。
#   占用率由 npu_busy_time_us 这个累计计数器做差算出。
#
#   注意: NPU 是"突发式"工作的 —— 一个推理任务可能几十毫秒就跑完，
#   然后立刻回落到 D3hot 并关掉时钟。所以采样间隔越短越准（推荐 0.2~0.5s）。

set -u

DEV="${NPU_DEV:-/sys/class/accel/accel0/device}"
INTERVAL="${1:-1}"

if [ ! -d "$DEV" ]; then
    echo "错误: 找不到 NPU 设备目录 $DEV" >&2
    echo "用 'ls /sys/class/accel/' 确认，或设置 NPU_DEV 环境变量。" >&2
    exit 1
fi

rd() { cat "$DEV/$1" 2>/dev/null || echo 0; }

MAXF=$(rd npu_max_frequency_mhz)
MODEL=$(basename "$(readlink -f "$DEV/../.." 2>/dev/null)" 2>/dev/null)

# 首次读设备会触发 NPU 上电，先预热一次让后续读数稳定
rd npu_busy_time_us >/dev/null

printf 'Intel NPU 实时监控  设备=%s  最大频率=%s MHz\n' "$DEV" "$MAXF"
printf '采样间隔=%ss  (Ctrl-C 退出)\n\n' "$INTERVAL"
printf '%-24s %7s  %-13s  %-11s  %-7s %s\n' '占用率' '' '频率(MHz)' '内存' '电源' '累计忙时'
printf '%-24s %7s  %-13s  %-11s  %-7s %s\n' '------------------------' '-------' '-------------' '-----------' '-------' '----------'

prev=$(rd npu_busy_time_us)
start_busy=$prev

cleanup() {
    end_busy=$(rd npu_busy_time_us)
    echo
    printf '\n会话累计 NPU 忙时: %d us  (%.1f ms)\n' \
        "$((end_busy - start_busy))" \
        "$(awk -v v="$((end_busy - start_busy))" 'BEGIN{print v/1000}')"
    exit 0
}
trap cleanup INT TERM

while :; do
    t0=$(date +%s%N)
    sleep "$INTERVAL"
    t1=$(date +%s%N)

    busy=$(rd npu_busy_time_us)
    elapsed_us=$(( (t1 - t0) / 1000 ))
    delta=$(( busy - prev ))
    prev=$busy

    pct=$(awk -v d="$delta" -v e="$elapsed_us" \
        'BEGIN{ if(e<=0){print "0.0"; exit} p=d/e*100; if(p>100)p=100; printf "%.1f", p }')

    freq=$(rd npu_current_frequency_mhz)
    mem=$(rd npu_memory_utilization)
    state=$(rd power_state)

    # 20 格进度条
    bar=$(awk -v p="$pct" 'BEGIN{
        n=int(p/5+0.5); if(n>20)n=20; s="";
        for(i=0;i<n;i++) s=s"#";
        for(i=n;i<20;i++) s=s".";
        print s }')

    memh=$(awk -v m="$mem" 'BEGIN{
        if (m >= 1073741824) printf "%.2f GiB", m/1073741824;
        else                 printf "%.1f MiB", m/1048576 }')

    # 频率也画个小条，方便看出升频
    fbar=$(awk -v f="$freq" -v m="$MAXF" 'BEGIN{
        if(m<=0){print ""; exit}
        n=int(f/m*8+0.5); if(n>8)n=8; s="";
        for(i=0;i<n;i++) s=s"|";
        printf "%-8s", s }')

    printf '\r[%s] %6s%%  %-8s      %-11s  %-7s %d us' \
        "$bar" "$pct" "$fbar" "$memh" "$state" "$busy"
done
