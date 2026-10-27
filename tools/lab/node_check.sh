#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# node_check.sh - read-only check of one node after every boot (docs/NODE-SETUP.md).
# No root needed; anything it cannot read is a FAIL, never a silent pass.
# Writes nothing, uses no network. Exit 0 only if no line is FAIL.
#   EXPECT_STREAM_SRCVERSION=<srcversion recorded for this build>  (recommended)
#   EXPECT_PERFOPT=on|off|absent   intended PerfOpt state of this node (optional)
#   EXPECT_LINKS=N   USB4 links and domains expected (default 2: two cables)
#   ALLOW_GEN2=1     report links below Gen3 x2 as WARN instead of FAIL
set -u
fails=0; pass() { echo "PASS  $*"; }; info() { echo "INFO  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }

rel=$(uname -r); info "kernel release $rel"
cmdline=$(cat /proc/cmdline 2>/dev/null) || cmdline=
case " $cmdline " in
"  ") fail "/proc/cmdline unreadable" ;;
*" kho=off "*) pass "kho=off on the kernel command line" ;;
*) fail "kho=off missing from the kernel command line" ;;
esac
case "$cmdline" in *iommu_perfopt*) fail "iommu_perfopt on the kernel command line (belongs in modprobe.d)" ;; esac
# Kernel log of this boot: required for the PerfOpt and AMD-Vi checks.
log=
if klog=$(journalctl -k -b --no-pager -o cat 2>/dev/null) && grep -Fq "Linux version $rel " <<<"$klog"; then
	log=ok; pass "kernel log of this boot readable"
else fail "kernel log of this boot unreadable (need root or group adm/systemd-journal)"; fi

# Kernel kind and PerfOpt state.
param=/sys/module/amdgpu/parameters/iommu_perfopt
armed=0 state=unknown
[ -n "$log" ] && armed=$(grep -c 'PerfOpt armed' <<<"$klog")
if [ -e "$param" ]; then
	if ! value=$(cat "$param" 2>/dev/null); then
		fail "kernel kind: PerfOpt kernel, but $param is unreadable"
	elif [ "$value" = 0 ]; then
		info "kernel kind: PerfOpt kernel, PerfOpt disabled (iommu_perfopt=0)"
		[ "$armed" -eq 0 ] && { state=off; pass "PerfOpt not armed, as configured"; } ||
			fail "iommu_perfopt=0 but the kernel log says PerfOpt armed"
	elif [ -z "$log" ]; then
		fail "kernel kind: PerfOpt kernel (iommu_perfopt=$value); armed state unknown without the kernel log"
	elif [ "$armed" -gt 0 ]; then
		state=on; pass "kernel kind: PerfOpt kernel, iommu_perfopt=$value, 'PerfOpt armed' logged"
	else
		fail "kernel kind: PerfOpt kernel, iommu_perfopt=$value, but no 'PerfOpt armed' line"
	fi
elif [ "$armed" -gt 0 ]; then
	state=on; pass "kernel kind: PerfOpt armed (no iommu_perfopt parameter in this kernel)"
elif [ -n "$log" ]; then
	state=absent; info "kernel kind: plain kernel, PerfOpt not present"
else
	fail "kernel kind: no iommu_perfopt parameter; armed state unverified without the kernel log"
fi
case "${EXPECT_PERFOPT:-}" in
'') ;;
"$state") pass "PerfOpt state $state = expected" ;;
*) fail "PerfOpt state $state, expected $EXPECT_PERFOPT" ;;
esac
if [ -n "$log" ] && grep -qiE 'PerfOpt.*(fail|error)|(fail|error).*PerfOpt' <<<"$klog"; then
	fail "PerfOpt failure/error line in the kernel log"
fi

# USB4 stream module.
if src=$(cat /sys/module/thunderbolt_stream/srcversion 2>/dev/null); then
	case "${EXPECT_STREAM_SRCVERSION:-}" in
	'') echo "WARN  thunderbolt_stream srcversion $src not verified (set EXPECT_STREAM_SRCVERSION)" ;;
	"$src") pass "thunderbolt_stream srcversion $src = expected" ;;
	*) fail "thunderbolt_stream srcversion $src, expected $EXPECT_STREAM_SRCVERSION" ;;
	esac
else fail "thunderbolt_stream not loaded"; fi
# The 0048 variant has the batching attributes in every configfs stream.
v48=0
for st in /sys/kernel/config/thunderbolt/stream/*/*/; do
	[ -e "${st}rx_repost_batch" ] && [ -e "${st}tx_submit_batch" ] && v48=1 && break
done
[ "$v48" = 1 ] && pass "stream module is the 0048 variant (rx_repost_batch/tx_submit_batch present)" ||
	fail "no configfs stream with rx_repost_batch/tx_submit_batch: older stream module or no streams"
want=${EXPECT_LINKS:-2}

# Link speed per USB4 link (same sysfs attributes as tools/usb4_linkstat.py).
links=0
for dev in /sys/bus/thunderbolt/devices/*-*; do
	[ -e "$dev/rx_speed" ] || [ -e "$dev/tx_speed" ] || continue
	links=$((links + 1))
	name=${dev##*/}
	for dir in rx tx; do
		speed=$(cat "$dev/${dir}_speed" 2>/dev/null) || speed=
		lanes=$(cat "$dev/${dir}_lanes" 2>/dev/null) || lanes=
		per=${speed%%.*}; per=${per%% *}
		case "$per:$lanes" in
		*[!0-9:]* | :* | *:) fail "link $name $dir speed unreadable ('$speed' x '$lanes')"; continue ;;
		esac
		msg="link $name $dir $speed x $lanes lanes"
		if [ "$per" -ge 20 ] && [ "$lanes" -ge 2 ]; then pass "$msg (Gen3 x2, 40 Gb/s)"
		elif [ "${ALLOW_GEN2:-0}" = 1 ]; then
			echo "WARN  $msg (below Gen3 x2, allowed by ALLOW_GEN2=1)"
		else fail "$msg (below Gen3 x2; reseat the cable, see docs/NODE-SETUP.md)"; fi
	done
done
[ "$links" -eq "$want" ] && pass "$links USB4 links (expected $want)" ||
	fail "$links trained USB4 links under /sys/bus/thunderbolt/devices, expected $want"

# IOMMU: GPU group type, every NHI DMA-FQ, DMA protection per domain.
gtype() { cat "/sys/bus/pci/devices/$1/iommu_group/type" 2>/dev/null || echo unreadable; }
gpus=0
for gpu in /sys/bus/pci/drivers/amdgpu/0000:*; do
	[ -e "$gpu" ] || continue
	gpus=$((gpus + 1))
	t=$(gtype "${gpu##*/}")
	if [ "$armed" -gt 0 ] && [ "$t" != identity ]; then fail "GPU ${gpu##*/} IOMMU group '$t' (PerfOpt needs identity)"
	else info "GPU ${gpu##*/} IOMMU group $t"; fi
done
[ "$gpus" -gt 0 ] || fail "no PCI device bound to amdgpu"
domains=0
for d in /sys/bus/thunderbolt/devices/domain*; do
	[ -e "$d" ] || continue
	domains=$((domains + 1))
	nhi=$(basename "$(dirname "$(readlink -f "$d")")")
	t=$(gtype "$nhi")
	[ "$t" = DMA-FQ ] && pass "NHI $nhi (${d##*/}) IOMMU group DMA-FQ" ||
		fail "NHI $nhi (${d##*/}) IOMMU group '$t', expected DMA-FQ"
	p=$(cat "$d/iommu_dma_protection" 2>/dev/null) || p=unreadable
	[ "$p" = 1 ] && pass "${d##*/} iommu_dma_protection=1" ||
		fail "${d##*/} iommu_dma_protection=$p"
done
[ "$domains" -eq "$want" ] && pass "$domains USB4 domains (expected $want)" ||
	fail "$domains Thunderbolt/USB4 domains, expected $want"

# AMD-Vi faults and errors in this boot's kernel log.
re='IO_PAGE_FAULT|INVALID_DEVICE_REQUEST|ILLEGAL_(DEV_TABLE|COMMAND)|HARDWARE_ERROR|event logged|Completion-Wait loop timed out|AMD-Vi.*(fail|error|timeout|timed out|disabled)'
if [ -z "$log" ]; then fail "AMD-Vi fault check not possible without the kernel log"
elif n=$(grep -ciE "$re" <<<"$klog") && [ "$n" -gt 0 ]; then fail "$n AMD-Vi fault/error line(s) this boot"
else pass "no AMD-Vi fault/error lines in this boot's kernel log"; fi

# Lab service and layout file against a fresh read-only describe pass.
layout=/run/usb4stream-layout.json
systemctl is-active --quiet usb4stream-lab.service && pass "usb4stream-lab.service active" ||
	fail "usb4stream-lab.service not active"
describe=$(command -v usb4stream-describe 2>/dev/null) ||
	describe="$(dirname "$0")/usb4stream_describe.py"
if [ ! -s "$layout" ]; then
	fail "layout file $layout missing or empty"
# --layout to a path that does not exist: describe must not fall back to the file under test.
elif ! fresh=$(python3 "$describe" --layout /nonexistent/node-check-no-layout 2>/dev/null); then
	fail "layout file present, but usb4stream-describe could not run to compare it"
else
	out=$(FRESH="$fresh" python3 -c '
import json, os, sys
def proj(d):
    # Stream mapping only: the layout is written early at boot, possibly before the peer is up, so its
    # cables_up can be lower than live; link count, speed and domains are checked from sysfs above.
    return sorted((k, v.get("device"), v.get("cable")) for k, v in d.items() if "/" in k and isinstance(v, dict))
live = json.loads(os.environ["FRESH"])
if any("configfs not readable" in w for w in (live.get("topology") or {}).get("warnings", [])):
    print("configfs not readable: live streams unknown"); sys.exit(1)
try:
    a = proj(json.load(open(sys.argv[1])))
except (OSError, ValueError, AttributeError) as exc:
    print("layout file unreadable: %s" % exc); sys.exit(1)
b = proj(live)
print("streams %d" % len(b))
sys.exit(0 if a == b else 2)' "$layout" 2>&1)
	case $? in
	0) pass "layout file matches the live topology ($out)" ;;
	2) fail "layout file differs from the live topology (live: $out)" ;;
	*) fail "layout check: $out" ;;
	esac
fi

[ "$fails" -eq 0 ] && echo "RESULT PASS" || echo "RESULT FAIL ($fails)"
[ "$fails" -eq 0 ]
