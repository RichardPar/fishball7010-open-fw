#!/bin/sh
# Put the kernel command line into the device tree's /chosen node. The vendor U-Boot sdboot path ignores `setenv bootargs`, so the
# DTB is the only reliable place. usage: dtb-bootargs.sh <tezuka fishball.dtb> <out.dtb>
set -eu
ARGS="console=ttyPS0,115200 maxcpus=2 rootfstype=ramfs root=/dev/ram0 rw earlyprintk clk_ignore_unused cpuidle.off=1 uio_pdrv_genirq.of_id=uio_pdrv_genirq UBOOT_MODEBOOT=sdboot"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
dtc -I dtb -O dts -o "$T/in.dts" "${1:?input dtb}" 2>/dev/null
if grep -q 'bootargs' "$T/in.dts"; then sed -i "s#bootargs = \".*\";#bootargs = \"$ARGS\";#" "$T/in.dts"
else sed -i "0,/chosen {/s##chosen {\n\t\tbootargs = \"$ARGS\";#" "$T/in.dts"; fi
dtc -I dts -O dtb -o "${2:?output dtb}" "$T/in.dts" 2>/dev/null
echo "wrote $2"
