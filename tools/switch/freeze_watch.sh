#!/bin/bash
# freeze_watch.sh EPOCH NETLOG [DEADLINE_S] -- watch a "Jouer" freeze test on the console.
# EPOCH = PC time (date +%s) when "Jouer" was pressed; NETLOG = the file netlog_listen.php writes.
# One line per event (made to be followed line by line): progress every 60 s, heartbeat stop,
# autopsy, sys-botbase state changes (is the network of the whole console down?), verdict.
D="$(cd "$(dirname "$0")" && pwd)"   # sbb.py lives next to this script
PY=${PY:-"/c/Users/$USERNAME/AppData/Local/Programs/Python/Python312/python.exe"}
T0=$1; NL=$2; DEADLINE=${3:-540}
last_prog=0; last_ping=0; sbb=""; stale=0; aut=0; aut_t=0
while true; do
  now=$(date +%s); el=$((now-T0))
  hb=$(grep -a -E ' #[0-9]+ up=' "$NL" 2>/dev/null | tail -1)
  if [ -n "$hb" ]; then
    hbt=$(date -d "today ${hb:0:8}" +%s 2>/dev/null || echo $now)
    if [ $stale = 0 ] && [ $((now-hbt)) -ge 15 ]; then stale=1; echo "BATTEMENTS ARRÊTÉS: dernier à ${hb:0:8} (Jouer +$((hbt-T0)) s) $(echo "$hb" | grep -o 'frame=[0-9]*')"; fi
    if [ $stale = 1 ] && [ $((now-hbt)) -lt 5 ]; then stale=0; echo "battements repris à ${hb:0:8}"; fi
  fi
  if [ $aut = 0 ] && grep -a -q '\[autopsy\] #' "$NL" 2>/dev/null; then
    aut=1; aut_t=$now; echo "AUTOPSIE: $(grep -a -m1 '\[autopsy\] #' "$NL" | cut -c1-110) (Jouer +$el s)"
  fi
  if [ $((now-last_ping)) -ge 10 ]; then
    last_ping=$now
    p=$($PY -I "$D/sbb.py" ping 2>&1 | cut -c1-60); s=${p:0:1}
    if [ "$s" != "$sbb" ]; then echo "sys-botbase: $([ "$s" = 1 ] && echo répond || echo 'NE RÉPOND PLUS') ($p) à Jouer +$el s"; sbb=$s; fi
  fi
  if [ $((now-last_prog)) -ge 60 ]; then
    last_prog=$now
    fr=$(echo "$hb" | grep -o 'frame=[0-9]*'); so=$(echo "$hb" | grep -o '[0-9-]* sock open'); er=$(echo "$hb" | grep -o 'err:[^|]*' | cut -c1-80)
    tr=$(echo "$hb" | grep -o 'tx [0-9]*K rx [0-9]*K')
    echo "Jouer +$el s | ${hb:0:8} $fr | $so, $tr ${er}"
  fi
  if [ $aut = 1 ] && [ $((now-aut_t)) -ge 60 ]; then echo "fin: gel (autopsie), sys-botbase=$sbb"; exit 0; fi
  if [ $aut = 0 ] && [ $el -ge $DEADLINE ] && [ $stale = 0 ]; then echo "fin: $DEADLINE s après Jouer sans autopsie, battements vivants"; exit 0; fi
  if [ $aut = 0 ] && [ $el -ge $((DEADLINE+120)) ]; then echo "fin: délai dépassé (battements arrêtés=$stale, pas d'autopsie)"; exit 0; fi
  sleep 2
done
