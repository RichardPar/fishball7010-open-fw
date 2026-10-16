S=opt/root/api_controller.sh
ex() { sed -n "/^$1 () {/,/^}/p" $S; }
declare -A CACHE
LOG=()
mqtt_publish () { LOG+=("MQ $1=$2"); }
serial_publish () { :; }
eval "$(ex read_file)"; eval "$(ex read_file_v)"; eval "$(ex publish)"
eval "$(sed -n '/^declare -A FWENV/,/^FWENV_MAX=/p' $S)"; eval "$(ex fwenv_get)"; eval "$(ex reg_rd)"; eval "$(ex reg_wr)"
T=$(mktemp -d); fails=0
chk() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails+1)); fi; }

# 1. read_file_v == read_file
printf '1234\n' > $T/one; printf 'a\nb c\n' > $T/multi; : > $T/empty; printf 'x' > $T/nonl
for f in one multi empty nonl missing; do chk "read_file_v == read_file ($f)" "$(read_file_v $T/$f; echo "$RF")" "$(read_file $T/$f)"; done

# 2. publish: original behaviour outside the loop, skip-unchanged inside it
DUMPING=0; FORCE_REFRESH=0; LOG=(); publish a 1; publish a 1; chk "handler path always publishes" "${#LOG[@]}" 2
DUMPING=1; FORCE_REFRESH=0; LOG=(); CACHE=(); publish k v; publish k v; publish k w; publish k w; chk "loop path: first+changed only" "${#LOG[@]}" 2
FORCE_REFRESH=1; LOG=(); publish k w; chk "forced refresh re-sends" "${#LOG[@]}" 1
FORCE_REFRESH=0; LOG=(); CACHE=(); publish e ""; publish e ""; chk "empty value sent once" "${#LOG[@]}" 1

# 3. fwenv_get caches for FWENV_MAX cycles
CNT=$(mktemp); echo 0 > $CNT
fw_printenv () { n=$(<$CNT); n=$((n+1)); echo $n > $CNT; echo "val$n"; }
for i in $(seq 1 16); do fwenv_get opt; done
chk "fwenv_get: 16 calls -> 1 read" "$(<$CNT)" 1; chk "fwenv_get cached value" "$FWV" "val1"
fwenv_get opt; chk "fwenv_get: call 17 refreshes" "$(<$CNT)" 2; chk "fwenv_get new value" "$FWV" "val2"

# 4. reg_rd/reg_wr fallback to iio_attr when debugfs is unavailable
iio_attr () { echo "0x4B"; }
reg_rd /sys/bus/iio/devices/iio:device9/ 0x5E ad9361-phy; chk "reg_rd fallback" "$REGV" "0x4B"
# ... and use the debugfs file directly when present (simulated file)
mkdir -p /tmp/dbgtest; sed "s#/sys/kernel/debug/iio#/tmp/dbgtest#g" <<<"$(ex reg_rd)" > /tmp/dbgtest/rd.sh; mkdir -p /tmp/dbgtest/iio:device9; echo "0x5E" > /tmp/dbgtest/iio:device9/direct_reg_access
. /tmp/dbgtest/rd.sh; reg_rd /sys/bus/iio/devices/iio:device9/ 0x5E ad9361-phy; chk "reg_rd via file" "$REGV" "0x5E"
echo "failures: $fails"
