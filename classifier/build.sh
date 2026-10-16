#!/bin/sh
# Assemble classifier.c from the parts, then build native (test) and static ARM binaries.
set -e
cd "$(dirname "$0")"; mkdir -p synth
cat c/part1_core.inc c/part2_detect.inc c/part3_classify.inc c/part4_app.inc c/ws_part.inc c/part5_main.inc > classifier.c
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o classifier-native classifier.c -lm
echo "native build OK"
if command -v arm-linux-gnueabihf-gcc >/dev/null; then
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-parameter -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -o classifier classifier.c -lm
  arm-linux-gnueabihf-strip classifier
  echo "ARM build OK: $(ls -l classifier | awk '{print $5}') bytes"
fi
