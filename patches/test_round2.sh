S=opt2/root/api_controller.sh
ex() { sed -n "/^$1 () {/,/^}/p" $S; }
eval "$(ex read_file_v)"; eval "$(ex read_opt)"; eval "$(ex irq_sum)"; eval "$(ex netdev_rxtx)"
fails=0; chk() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails+1)); fi; }
T=$(mktemp -d); printf '5\n' > $T/a; : > $T/e; printf 'x y' > $T/n
for f in a e n missing; do read_opt $T/$f; chk "read_opt == cat ($f)" "$RF" "$(cat $T/$f 2>/dev/null)"; done
# irq_sum against the original awk pipeline on this machine's real /proc/interrupts
orig() { grep "$1" /proc/interrupts 2>/dev/null | head -1 | awk '{s=0; for(i=2;i<=NF;i++){if($i~/^[0-9]+$/)s+=$i}; print s}'; }
for p in timer eth i8042 nvme "Local timer" no_such_pattern_xyz "0:" "PCI"; do
  a=$(orig "$p"); irq_sum "$p"; chk "irq_sum '$p'" "$IRQSUM" "$a"
done
# netdev against the original grep|awk on this machine's real /proc/net/dev
orig2() { read -r a b <<< "$(grep "$1:" /proc/net/dev 2>/dev/null | awk '{print $2, $10}')"; echo "$a $b"; }
for i in lo enp0s31f6 wlp enx no_such; do
  a=$(orig2 "$i"); netdev_rxtx "$i"; chk "netdev '$i'" "$NDRX $NDTX" "$a"
done
# synthetic lines exercising the awk quirks: no space after the colon, leading zeros
cat > $T/net <<'N'
Inter-|   Receive                                                |  Transmit
 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
usb0:123456789 100 0 0 0 0 0 0 987654321 90 0 0 0 0 0 0
  eth0: 000042 7 0 0 0 0 0 0 000099 8 0 0 0 0 0 0
N
sed "s#/proc/net/dev#$T/net#" <<<"$(ex netdev_rxtx)" > $T/nd.sh; . $T/nd.sh
chk "netdev no-space quirk matches awk" "$(netdev_rxtx usb0; echo "$NDRX $NDTX")" "$(grep 'usb0:' $T/net | awk '{print $2, $10}')"
chk "netdev leading zeros match awk" "$(netdev_rxtx eth0; echo "$NDRX $NDTX")" "$(grep 'eth0:' $T/net | awk '{print $2, $10}')"
echo "failures: $fails"
