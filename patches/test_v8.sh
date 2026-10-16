T=$(mktemp -d)
extract_fn() { sed -n "/^$2 () {/,/^}/p" "$1"; }
extract_loops() { python3 - "$1" <<'P'
import sys,re
t=open(sys.argv[1]).read().split('\n')
s=next(i for i,l in enumerate(t) if 'for K in "${!VMAP[@]}"' in l)
e=max(i for i,l in enumerate(t) if 'for K in "${!cVMAP[@]}"' in l)
# include the closing 'done' of an expanded loop if present
j=e
while j<len(t) and not (t[j].strip().startswith('done') or (j==e and t[j].rstrip().endswith('done'))): j+=1
print('\n'.join(t[s:j+1]))
P
}
run_variant() {  # $1 = script ; prints the message stream
  ( declare -A CACHE VMAP cVMAP; LOGF=$T/log.$2; : > $LOGF
    mqtt_publish () { echo "$1=$2" >> $LOGF; }; serial_publish () { :; }
    eval "$(extract_fn $1 read_file_v)"; eval "$(extract_fn $1 publish)"
    cat > $T/loops.$2 <<EOL
dump() {
$(extract_loops $1)
}
EOL
    . $T/loops.$2
    VMAP=( ["$T/a"]="rx/a" ["$T/b"]="rx/b" ["$T/missing"]="rx/missing" ["$T/c:d"]="gpio/led0:green" ["$T/e"]="main/e" )
    cVMAP=( ["$T/f"]="tx/f" ["$T/g"]="tx/g" )
    echo 1 > $T/a; echo hello > $T/b; echo x > "$T/c:d"; : > $T/e; echo 5 > $T/f; printf 'no newline' > $T/g
    DUMPING=1; FORCE_REFRESH=0
    for n in $(seq 1 24); do
      if [ $((n % 8)) -eq 1 ]; then FORCE_REFRESH=1; else FORCE_REFRESH=0; fi
      case $n in 3) echo 2 > $T/a;; 5) echo world > $T/b;; 6) echo 7 > $T/f;; 11) : > $T/b;; 12) echo 2 > $T/a; echo y > "$T/c:d";; 15) printf 'a\nb\n' > $T/e;; 20) rm $T/f;; esac
      echo "--- cycle $n" >> $LOGF
      dump
    done ) 
}
run_variant opt2/root/api_controller.sh old; run_variant opt4/root/api_controller.sh new
# timestamps/paths differ per variant only in the temp dir name, which is shared; compare directly
if diff -q $T/log.old $T/log.new >/dev/null; then echo "IDENTICAL message streams: $(grep -vc '^---' $T/log.old) messages over 24 cycles (6 are forced refreshes)"; else echo "DIFFERENT:"; diff $T/log.old $T/log.new | head -20; fi
echo "messages per cycle (old): $(awk '/^---/{if(n!="")printf "%s ",c; n=$3; c=0; next} {c++} END{print c}' $T/log.old)"
