T=$(mktemp -d); mkdir -p $T/net/usb0 $T/net/eth0; echo up > $T/net/usb0/operstate; echo aa:bb:cc:dd:ee:01 > $T/net/usb0/address; echo down > $T/net/eth0/operstate; echo aa:bb:cc:dd:ee:02 > $T/net/eth0/address
printf 'Iface\tDestination\neth0\t00000000\n' > $T/route
snippet=$(python3 - <<'P'
t=open('opt4/root/api_controller.sh').read().split('\n')
s=next(i for i,l in enumerate(t) if l.strip().startswith('local _sig=""'))
e=next(i for i,l in enumerate(t) if i>s and l.strip()=='NET_SIG=$_sig')
print('\n'.join(t[s:e+1]))
P
)
snippet=${snippet//\/sys\/class\/net/$T/net}; snippet=${snippet//\/proc\/net\/route/$T/route}
chk() { echo "$1 -> block ran: $2 (expected $3) $([ "$2" = "$3" ] && echo ok || echo FAIL)"; }
NET_SIG="@"; FORCE_REFRESH=0
cyc() { ran=0; LOOP_N=$1; FORCE_REFRESH=$2; eval "f() { $snippet
 }"; f; }
# emulate: the network block is entered when the 'if' inside the snippet condition holds; reproduce the condition here
cond() { LOOP_N=$1; FORCE_REFRESH=$2; local _sig=""; for _n in usb0 eth0; do if [ -d "$T/net/$_n" ]; then _o=""; _a=""; read -r _o < "$T/net/$_n/operstate"; read -r _a < "$T/net/$_n/address"; _sig="$_sig $_n:$_o:$_a"; fi; done; IFS= read -r -d '' _rt < $T/route; _sig="$_sig|$_rt"
  if [ "$_sig" != "$NET_SIG" ] || { [ "$FORCE_REFRESH" = 1 ] && [ $(( LOOP_N % 32 )) -eq 1 ]; }; then NET_SIG=$_sig; RES=yes; else RES=no; fi; }
cond 1 1; chk "cycle 1 (first)           " "$RES" yes
cond 2 0; chk "cycle 2 (nothing changed) " "$RES" no
cond 3 0; chk "cycle 3 (nothing changed) " "$RES" no
echo up > $T/net/eth0/operstate
cond 4 0; chk "cycle 4 (eth0 link up)    " "$RES" yes
cond 5 0; chk "cycle 5 (nothing changed) " "$RES" no
printf 'Iface\tDestination\nusb0\t00000000\n' > $T/route
cond 6 0; chk "cycle 6 (default route moved)" "$RES" yes
cond 9 1; chk "cycle 9 (forced, not 32nd)" "$RES" no
cond 33 1; chk "cycle 33 (forced 32nd)    " "$RES" yes
