T=$1; ORIG=$2; NEW=$3; out=$4
mk() { # build a sandboxed copy of a watcher script
  sed -e "s#/proc/interrupts#$T/irq#g" -e "s#/sys/class/leds/ptt/brightness#$T/ptt#g" \
      -e "s#/sys/kernel/debug/iio/iio:device\([01]\)/direct_reg_access#$T/dbg\1#g" \
      -e "s#/sys/bus/iio/devices/iio:device\([01]\)/#$T/iiodev\1_#g" -e "s#/tmp/lnb.txt#$T/lnb.txt#g" -e "s#/tmp/watch.txt#$T/watch.txt#g" \
      -e 's#^loop$##' -e "s#cat /sys/bus/iio/devices/iio:device0/name#cat $T/name#" "$1" > "$2"; }
mk "$ORIG" $T/o.sh; mk "$NEW" $T/n.sh
for v in o n; do
  rm -f $T/lnb.txt; echo ad9361-phy > $T/name; echo 0x10 > $T/dbg0; echo 0 > $T/ptt; echo "  34:  1000  500  GIC-0  33 Level  7c420000.dma" > $T/irq
  mkdir -p $T/bin; printf '#!/bin/sh\necho no\n' > $T/bin/fw_printenv; chmod +x $T/bin/fw_printenv
  ( export PATH=$T/bin:$PATH; . $T/$v.sh
    seq_irq="1500 1500 1500 1500 1900 2300 2700 2700 2700 2700"   # idle x4 (first equals start), then TX x3, then idle again
    i=0; for c in $seq_irq; do
      i=$((i+1)); echo "  34:  $c  500  GIC-0  33 Level  7c420000.dma" > $T/irq
      if [ $i -eq 9 ]; then echo 1 > $T/ptt; fi            # someone flips the PTT LED on by hand while idle
      check_tx_flux; echo "$v cycle $i irq=$c ptt_led=$(cat $T/ptt)" >> $T/trace.txt
    done ) > /dev/null 2>&1
  echo "$v: log lines written: $(wc -l < $T/lnb.txt)   ($(grep -c PTT_ON $T/lnb.txt) ON, $(grep -c PTT_OFF $T/lnb.txt) OFF)"
done
