S=opt2/root/api_controller.sh
T=$(mktemp -d); cp /proc/interrupts $T/irq
sed -n "/^irq_sum () {/,/^}/p" $S | sed "s#/proc/interrupts#$T/irq#" > $T/f.sh; . $T/f.sh
fails=0
for p in timer "Local timer" eth i8042 nvme "0:" "PCI" "IR-" "xhci" "MSI" "no_such_pattern_xyz" "TLB" "NMI"; do
  a=$(grep "$p" $T/irq 2>/dev/null | head -1 | awk '{s=0; for(i=2;i<=NF;i++){if($i~/^[0-9]+$/)s+=$i}; print s}'); irq_sum "$p"
  if [ "$IRQSUM" = "$a" ]; then echo "ok   irq_sum '$p' = ${a:-<none>}"; else echo "FAIL irq_sum '$p': got [$IRQSUM] want [$a]"; fails=$((fails+1)); fi
done; echo "failures: $fails"
