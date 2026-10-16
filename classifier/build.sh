#!/bin/sh
# Build the classifier: native test binary, plus a static ARM binary if the cross compiler is installed.
set -e
cd "$(dirname "$0")"
mkdir -p synth
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o classifier-native classifier.c -lm
echo "native build OK"
if command -v arm-linux-gnueabihf-gcc >/dev/null; then
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-parameter -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -o classifier classifier.c -lm
  arm-linux-gnueabihf-strip classifier
  echo "ARM build OK: $(wc -c < classifier) bytes"
fi
