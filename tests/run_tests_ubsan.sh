#!/usr/bin/env bash
# SCDE test suite.  Requires: Xvfb, xdotool, x11-utils (xwininfo/xprop/xdpyinfo)
set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN="$ROOT/scde-ubsan"
TESTBIN="$ROOT/tests/testclient"
PIXELBIN="$ROOT/tests/rootpixel"
DPY=":97"
W=1024
H=768

PASS=0
FAIL=0
WORK=$(mktemp -d /tmp/scde-test.XXXXXX)
HOME_SAVE="$HOME"
SCDE_PID=""
XVFB_PID=""
CLIENTS=()

log()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS + 1)); log "  PASS: $*"; }
bad()  { FAIL=$((FAIL + 1)); log "  FAIL: $*"; }
check() { local desc="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi; }

near() { # near <value> <expected> <tolerance>
    local d=$(( $1 - $2 ))
    [ "$d" -lt 0 ] && d=$((-d))
    [ "$d" -le "$3" ]
}

geom_x() { xwininfo -id "$1" 2>/dev/null | sed -n 's/^ *Absolute upper-left X:  *\([0-9-]*\)/\1/p'; }
geom_y() { xwininfo -id "$1" 2>/dev/null | sed -n 's/^ *Absolute upper-left Y:  *\([0-9-]*\)/\1/p'; }
geom_w() { xwininfo -id "$1" 2>/dev/null | sed -n 's/^ *Width:  *\([0-9]*\)/\1/p'; }
geom_h() { xwininfo -id "$1" 2>/dev/null | sed -n 's/^ *Height:  *\([0-9]*\)/\1/p'; }
active() { xdotool getactivewindow 2>/dev/null || echo 0; }
hexid() { printf '0x%x' "$1"; }
dock_win() { xdotool search --onlyvisible --name '^scde-dock$' 2>/dev/null | head -1; }
dock_icon() { # dock_icon <task-index> -> "X Y" centre of the dock tile
    local D X Y
    D=$(dock_win)
    X=$(geom_x "$D"); Y=$(geom_y "$D")
    # launcher tile 8..48, separator 11, tiles at 59+48i, centre +20/+26
    echo $(( X + 59 + 48*$1 + 20 )) $(( Y + 26 ))
}

cleanup() {
    export HOME="$HOME_SAVE"
    local p
    for p in "${CLIENTS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    [ -n "$SCDE_PID" ] && kill "$SCDE_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
    echo "ASan test logs preserved at: $WORK"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- preflight
for tool in Xvfb xdotool xwininfo xprop xdpyinfo pgrep; do
    command -v "$tool" >/dev/null 2>&1 || { log "missing test dependency: $tool"; exit 2; }
done
[ -x "$BIN" ] || { log "build first (make)"; exit 2; }
[ -x "$TESTBIN" ] || { log "build first (make)"; exit 2; }

# ---------------------------------------------------------------- fixtures
export HOME="$WORK/home"
mkdir -p "$HOME/.config/scde"
mkdir -p "$HOME/fmdir/sub"
printf 'hello\n' > "$HOME/fmdir/file.txt"
printf 'png\n' > "$HOME/fmdir/pic.png"
MARKER1="$WORK/menu_marker"
MARKER2="$WORK/cmd_marker"
MARKER3="$WORK/menu_marker2"
cat > "$WORK/fakebrowser" <<'FB'
#!/bin/sh
printf '%s\n' "$@" > "$HOME/browser_args"
FB
chmod +x "$WORK/fakebrowser"
cat > "$HOME/.config/scde/config" <<EOF
panel_height=28
border_width=3
border_focused=#ff6600
border_unfocused=#333333
background=#101014
wallpaper_top=#101014
wallpaper_bottom=#101014
focus_follows_mouse=1
show_clock=1
clock_format=%H:%M:%S
terminal=touch $WORK/term_marker
browser=$WORK/fakebrowser
sandbox_memory_mb=2048
menu=MarkerOne|touch $MARKER1
menu=MarkerTwo|touch $MARKER3
menu=Terminal|true
EOF

export DISPLAY="$DPY"
Xvfb "$DPY" -screen 0 ${W}x${H}x24 -nolisten tcp >"$WORK/xvfb.log" 2>&1 &
XVFB_PID=$!
for i in $(seq 1 50); do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done
if ! xdpyinfo >/dev/null 2>&1; then
    log "Xvfb failed to start"; cat "$WORK/xvfb.log"; exit 2
fi
log "== Xvfb ready on $DPY (${W}x${H}) =="

# ------------------------------------------------------------- 1. start up
log "[1] startup / singleton / menu bar + dock"
"$BIN" >"$WORK/scde.log" 2>&1 &
SCDE_PID=$!
sleep 0.6
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "scde starts"; else bad "scde starts"; cat "$WORK/scde.log"; exit 1; fi

if "$BIN" >"$WORK/scde2.log" 2>&1; then bad "second instance refused"; else ok "second instance refused"; fi
grep -qi "already" "$WORK/scde2.log" && ok "singleton message printed" || bad "singleton message printed"

if xwininfo -root -children 2>/dev/null | grep -q "scde-menubar"; then
    ok "menu bar window created"
else
    bad "menu bar window created"
fi
check "menu bar sits on the top edge" \
    sh -c "xwininfo -root -children | grep scde-menubar | grep -q '+0+0'"
check "menu bar is a slim 28 px strip" \
    sh -c "xwininfo -root -children | grep scde-menubar | grep -qE '1024x28\+0\+0'"
if xdotool search --onlyvisible --name '^scde-dock$' >/dev/null 2>&1; then
    ok "dock window created"
else
    bad "dock window created"
fi
check "dock floats above the bottom edge (empty: 56 px wide, centred)" \
    sh -c "xwininfo -root -children | grep scde-dock | grep -qE '56x60\+[0-9]+\+$((H - 68))'"

BG=$("$PIXELBIN" 10 40 2>/dev/null | tr -d '\n')
if [ "$BG" = "0x101014" ]; then ok "dark desktop background ($BG)"; else bad "dark desktop background ($BG, want 0x101014)"; fi

# ------------------------------------------------------ 2. window management
log "[2] window management"
"$TESTBIN" -n winA -g 300x200+120+120 >/dev/null 2>&1 &
CLIENTS+=($!)
disown
"$TESTBIN" -n winB -g 280x180+560+380 >/dev/null 2>&1 &
B_PID=$!
CLIENTS+=($B_PID)
disown
sleep 0.7

WA=$(xdotool search --name '^winA$' 2>/dev/null | head -1)
WB=$(xdotool search --name '^winB$' 2>/dev/null | head -1)
if [ -n "$WA" ] && [ -n "$WB" ]; then ok "clients created"; else bad "clients created"; exit 1; fi
WA_HEX=$(hexid "$WA")
WB_HEX=$(hexid "$WB")

check "client A is mapped"      sh -c "xwininfo -id $WA | grep -q 'Map State: IsViewable'"

# the frame is the nameless child of the root holding the client
FA=$(xwininfo -root -children 2>/dev/null | sed -n 's/^ *\(0x[0-9a-fA-F]*\) (has no name):.*300x224.*/\1/p' | head -1)
if [ -n "$FA" ]; then
    ok "client A sits in a title-bar frame ($FA)"
    check "frame A carries our border"    sh -c "xwininfo -id $FA | grep -q 'Border width: 3'"
    check "client A itself has no border" sh -c "xwininfo -id $WA | grep -q 'Border width: 0'"
    HA=$(geom_h "$FA")
    if near "$HA" 224 4; then ok "frame A includes the title bar (h=$HA)"; else bad "frame A includes the title bar (h=$HA, want 224)"; fi
else
    bad "client A sits in a title-bar frame"
fi

CLIST=$(xprop -root _NET_CLIENT_LIST 2>/dev/null)
case "$CLIST" in
    *"$WA_HEX"*"$WB_HEX"*|*"$WB_HEX"*"$WA_HEX"*) ok "_NET_CLIENT_LIST contains both windows" ;;
    *) bad "_NET_CLIENT_LIST contains both windows ($CLIST)" ;;
esac

# --------------------------------------------------- 3. focus follows mouse
log "[3] focus"
xdotool mousemove $((120 + 150)) $((120 + 100))
sleep 0.3
[ "$(active)" = "$WA" ] && ok "focus follows mouse" || bad "focus follows mouse (got $(active), want $WA)"

xdotool mousemove $((560 + 140)) $((380 + 90))
sleep 0.3
[ "$(active)" = "$WB" ] && ok "focus moves to window under pointer" || bad "focus moves to window under pointer"

xdotool mousemove 400 $((H - 14))
sleep 0.3
[ "$(active)" = "$WB" ] && ok "dock does not steal focus" || bad "dock does not steal focus (got $(active))"

# the dock lists clients in mapping order: first icon = first list entry
LIST=$(xprop -root _NET_CLIENT_LIST 2>/dev/null | sed 's/.*# //; s/,//g')
BTN1=$(( $(echo "$LIST" | awk '{print $1}') ))
BTN2=$(( $(echo "$LIST" | awk '{print $2}') ))
D0=($(dock_icon 0)); xdotool mousemove "${D0[0]}" "${D0[1]}"; xdotool click 1; sleep 0.35
[ "$(active)" = "$BTN1" ] && ok "dock first icon focuses its window" || bad "dock first icon focuses its window (got $(active), want $BTN1)"
D1=($(dock_icon 1)); xdotool mousemove "${D1[0]}" "${D1[1]}"; xdotool click 1; sleep 0.35
[ "$(active)" = "$BTN2" ] && ok "dock second icon focuses its window" || bad "dock second icon focuses its window (got $(active), want $BTN2)"

# ------------------------------------------------------------- 4. alt+tab
log "[4] Alt+Tab"
# expected cycle derived from the current focus and the client list order
LIST=$(xprop -root _NET_CLIENT_LIST 2>/dev/null | sed 's/.*# //; s/,//g')
L0=$(( $(echo "$LIST" | awk '{print $1}') ))
L1=$(( $(echo "$LIST" | awk '{print $2}') ))
F0=$(active)
if [ "$F0" = "$L1" ]; then NEXT1=$L0; NEXT2=$L1; else NEXT1=$L1; NEXT2=$L0; fi
xdotool key alt+Tab; sleep 0.3
[ "$(active)" = "$NEXT1" ] && ok "Alt+Tab switches to the other window" || bad "Alt+Tab switches (got $(active), want $NEXT1)"
xdotool key alt+Tab; sleep 0.3
[ "$(active)" = "$NEXT2" ] && ok "Alt+Tab cycles on (wraps)" || bad "Alt+Tab cycles on (got $(active), want $NEXT2)"
xdotool keydown shift; xdotool key alt+Tab; xdotool keyup shift; sleep 0.3
[ "$(active)" = "$NEXT1" ] && ok "Shift+Alt+Tab goes back" || bad "Shift+Alt+Tab goes back (got $(active), want $NEXT1)"

# ------------------------------------------------------------ 5. alt+drag
log "[5] Alt drag move / resize"
xdotool mousemove $((560 + 140)) $((380 + 90))
sleep 0.3
X0=$(geom_x "$WB"); Y0=$(geom_y "$WB")
xdotool keydown alt
xdotool mousedown 1
sleep 0.15
xdotool mousemove $((560 + 140 - 150)) $((380 + 90 + 60))
sleep 0.25
xdotool mouseup 1
xdotool keyup alt
sleep 0.4
X1=$(geom_x "$WB"); Y1=$(geom_y "$WB")
DX=$((X1 - X0)); DY=$((Y1 - Y0))
if near "$DX" -150 5 && near "$DY" 60 5; then
    ok "Alt+Button1 drag moved window (dX=$DX dY=$DY)"
else
    bad "Alt+Button1 drag moved window (dX=$DX dY=$DY, want -150/60)"
fi

xdotool mousemove $((X1 + 60)) $((Y1 + 60))
sleep 0.2
W0=$(geom_w "$WB"); H0=$(geom_h "$WB")
xdotool keydown alt
xdotool mousedown 3
sleep 0.15
xdotool mousemove $((X1 + 60 + 140)) $((Y1 + 60 + 70))
sleep 0.25
xdotool mouseup 3
xdotool keyup alt
sleep 0.4
W1=$(geom_w "$WB"); H1=$(geom_h "$WB")
DW=$((W1 - W0)); DH=$((H1 - H0))
if near "$DW" 140 5 && near "$DH" 70 5; then
    ok "Alt+Button3 drag resized window (dW=$DW dH=$DH)"
else
    bad "Alt+Button3 drag resized window (dW=$DW dH=$DH, want 140/70)"
fi

# ------------------------------------------- 6. configure request honoured
log "[6] ConfigureRequest"
W0=$(geom_w "$WA")
xdotool windowsize "$WA" 240 160
sleep 0.4
W1=$(geom_w "$WA")
if [ "$W1" = "240" ]; then ok "client resize request honoured"; else bad "client resize request honoured (got $W1, was $W0)"; fi

# ------------------------------------------------------------- 7. alt+f4
log "[7] Alt+F4"
xdotool mousemove $((120 + 150)) $((120 + 100))
sleep 0.3
[ "$(active)" = "$WA" ] && ok "window A focused before close" || bad "window A focused before close"
xdotool key alt+F4
sleep 0.7
if xdotool search --name '^winA$' >/dev/null 2>&1; then
    bad "Alt+F4 closed window A"
else
    ok "Alt+F4 closed window A"
fi
CLIST=$(xprop -root _NET_CLIENT_LIST 2>/dev/null)
case "$CLIST" in *"$WA_HEX"*) bad "list updated after Alt+F4" ;; *) ok "list updated after Alt+F4" ;; esac

# --------------------------------------------------------- 8. app menu
log "[8] application menu (Alt+Space)"
xdotool key alt+space
sleep 0.4
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    ok "menu opens"
else
    bad "menu opens"
fi
xdotool key Escape
sleep 0.3
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    bad "Escape closes menu"
else
    ok "Escape closes menu"
fi

rm -f "$MARKER1"
xdotool key alt+space
sleep 0.4
xdotool key Return
sleep 0.8
if [ -f "$MARKER1" ]; then ok "menu entry runs its command"; else bad "menu entry runs its command"; fi
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    bad "menu closes after selection"
else
    ok "menu closes after selection"
fi

# ------------------------------------------------------- 9. alt+f2 launcher
log "[9] Alt+F2 command launcher"
xdotool key alt+F2
sleep 0.4
if xdotool search --onlyvisible --name '^scde-cmd$' >/dev/null 2>&1; then
    ok "command launcher opens"
else
    bad "command launcher opens"
fi
rm -f "$MARKER2"
xdotool type --delay 20 "touch $MARKER2"
sleep 0.2
xdotool key Return
sleep 0.8
if [ -f "$MARKER2" ]; then ok "command launcher runs command"; else bad "command launcher runs command"; fi
if xdotool search --onlyvisible --name '^scde-cmd$' >/dev/null 2>&1; then
    bad "command launcher closes after run"
else
    ok "command launcher closes after run"
fi

# ---------------------------------------------------------- 10. help/version
log "[10] help / version / Alt+Enter"
check "help lists Alt+Enter binding" sh -c "'$BIN' -h | grep -q 'Alt+Enter'"
check "version flag"                 sh -c "'$BIN' -v | grep -q 'scde 0.2'"
rm -f "$WORK/term_marker"
xdotool key alt+Return
sleep 0.8
if [ -f "$WORK/term_marker" ]; then ok "Alt+Enter runs the configured terminal"; else bad "Alt+Enter runs the configured terminal"; fi

# ------------------------------------------------- 10b. file manager
log "[10b] built-in file manager"
xdotool key alt+e; sleep 0.6
FMWIN=$(xdotool search --onlyvisible --name '^scde-fm' 2>/dev/null | head -1)
if [ -n "$FMWIN" ]; then ok "Alt+E opens the file manager"; else bad "Alt+E opens the file manager"; fi
check "fm window has a sane width" \
    sh -c "[ -n '${FMWIN:-}' ] && xwininfo -id $FMWIN | grep -qE 'Width: +8[0-9][0-9]'"
FMNAME=$(xdotool getwindowname "$FMWIN" 2>/dev/null)
case "$FMNAME" in
    *"scde-fm: $HOME"*) ok "fm opens in HOME" ;;
    *) bad "fm opens in HOME (got '$FMNAME')" ;;
esac
# navigate into the subfolder with the keyboard
xdotool key Down; sleep 0.2    # .. -> fmdir (dirs sort first)
xdotool key Return; sleep 0.5
FMNAME=$(xdotool getwindowname "$FMWIN" 2>/dev/null)
case "$FMNAME" in
    *"/fmdir"*) ok "Enter descends into a directory" ;;
    *) bad "Enter descends into a directory (got '$FMNAME')" ;;
esac
# .. is selected first: Backspace goes up again
xdotool key BackSpace; sleep 0.5
FMNAME=$(xdotool getwindowname "$FMWIN" 2>/dev/null)
case "$FMNAME" in
    *"scde-fm: $HOME"*) ok "Backspace goes to the parent" ;;
    *) bad "Backspace goes to the parent (got '$FMNAME')" ;;
esac
# create a folder through the edit line
xdotool key ctrl+n; sleep 0.3
xdotool type --delay 25 "newdir"; sleep 0.2
xdotool key Return; sleep 0.5
if [ -d "$HOME/newdir" ]; then ok "New Folder creates a directory"; else bad "New Folder creates a directory"; fi
xdotool key Escape; sleep 0.4
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    bad "Escape closes the file manager"
else
    ok "Escape closes the file manager"
fi

# ------------------------------------------------------ 11. signals / lock
log "[11] signals and lock"
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "still running after tests"; else bad "still running after tests"; fi

kill -TERM "$SCDE_PID"
for i in $(seq 1 50); do
    kill -0 "$SCDE_PID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$SCDE_PID" 2>/dev/null; then bad "SIGTERM shuts down"; else ok "SIGTERM shuts down"; fi
wait "$SCDE_PID" 2>/dev/null
SCDE_PID=""
check "clean shutdown logged" grep -q "clean shutdown" "$WORK/scde.log"

"$BIN" >"$WORK/scde3.log" 2>&1 &
SCDE_PID=$!
sleep 0.6
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "restarts after shutdown (lock released)"; else bad "restarts after shutdown (lock released)"; cat "$WORK/scde3.log"; fi

# --------------------------------------------------------- 12. resources
log "[12] resource usage"
"$TESTBIN" -n winC -g 200x150+40+40 >/dev/null 2>&1 &
CLIENTS+=($!)
disown
sleep 1.0
WC=$(xdotool search --name '^winC$' 2>/dev/null | head -1)
WC_HEX=$(hexid "${WC:-0}")

# count only the instances on the test display: a SCDE session running
# elsewhere on this machine must not affect the result
NR_PROC=0
for p in $(pgrep -x scde-asan 2>/dev/null); do
    if tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qxF "DISPLAY=$DPY"; then
        NR_PROC=$((NR_PROC + 1))
    fi
done
if [ "$NR_PROC" = "1" ]; then ok "exactly one scde process"; else bad "exactly one scde process ($NR_PROC)"; fi

NR_TASK=$(ls "/proc/$SCDE_PID/task" 2>/dev/null | wc -l)
if [ "$NR_TASK" = "1" ]; then ok "single thread"; else bad "single thread ($NR_TASK)"; fi

RSS_KB=$(ps -o rss= -p "$SCDE_PID" 2>/dev/null | tr -d ' ')
if [ -n "$RSS_KB" ] && [ "$RSS_KB" -lt 262144 ]; then
    ok "resident memory ${RSS_KB} kB (< 256 MB)"
else
    bad "resident memory (${RSS_KB:-?} kB)"
fi

read_cpu() { awk '{print $14 + $15}' "/proc/$SCDE_PID/stat" 2>/dev/null; }
C1=$(read_cpu); sleep 3; C2=$(read_cpu)
CPU_TICKS=$((C2 - C1))
if [ "$CPU_TICKS" -le 3 ]; then
    ok "idle CPU ${CPU_TICKS} ticks over 3 s (no busy polling)"
else
    bad "idle CPU ${CPU_TICKS} ticks over 3 s"
fi

BIN_SIZE=$(stat -c %s "$BIN")
STRIPPED=$(strip -o "$WORK/scde.stripped" "$BIN" 2>/dev/null && stat -c %s "$WORK/scde.stripped")
ok "binary size: $BIN_SIZE bytes, ${STRIPPED:-?} bytes stripped"

# ------------------------------------------------------ 13. client vanishes
log "[13] client destroyed externally"
if [ -n "${B_PID:-}" ]; then
    kill -9 "$B_PID" 2>/dev/null
fi
sleep 0.7
CLIST=$(xprop -root _NET_CLIENT_LIST 2>/dev/null)
case "$CLIST" in *"$WB_HEX"*) bad "vanished client removed from list" ;; *) ok "vanished client removed from list" ;; esac

# ------------------------------------------- 14. config reload / restart
log "[14] config reload (menu bar taller) + existing windows"
kill -TERM "$SCDE_PID" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$SCDE_PID" 2>/dev/null || break; sleep 0.1; done
wait "$SCDE_PID" 2>/dev/null
SCDE_PID=""
sed -i 's/panel_height=28/panel_height=34/' "$HOME/.config/scde/config"
"$BIN" >"$WORK/scde4.log" 2>&1 &
SCDE_PID=$!
sleep 0.8
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "restarts with new config"; else bad "restarts with new config"; cat "$WORK/scde4.log"; fi
MB=$(xdotool search --onlyvisible --name '^scde-menubar$' 2>/dev/null | head -1)
check "menu bar height follows the new config" \
    sh -c "[ -n '${MB:-}' ] && xwininfo -id $MB | grep -qE 'Height: +34'"
CLIST=$(xprop -root _NET_CLIENT_LIST 2>/dev/null)
case "$CLIST" in *"$WC_HEX"*) ok "existing window re-managed after restart" ;; *) bad "existing window re-managed after restart ($CLIST)" ;; esac
xdotool mousemove 140 115; sleep 0.35
[ "$(active)" = "$WC" ] && ok "focus works after restart" || bad "focus works after restart (got $(active), want $WC)"

# ------------------------------------------------- 15. plasma style extras
log "[15] title bar / snapping / desktop menu / browser"
XC=$(active)

# maximize via Alt+Up and restore
xdotool key alt+Up; sleep 0.4
CW0=$(geom_w "$XC"); CX0=$(geom_x "$XC")
if [ "$CW0" -gt 900 ] && [ "$CX0" -le 6 ]; then
    ok "Alt+Up maximizes (w=$CW0 x=$CX0)"
else
    bad "Alt+Up maximizes (w=$CW0 x=$CX0)"
fi
xdotool key alt+Up; sleep 0.4
CW1=$(geom_w "$XC")
if [ "$CW1" = "200" ]; then ok "Alt+Up toggles back"; else bad "Alt+Up toggles back (w=$CW1)"; fi

# snap halves
xdotool key alt+Left; sleep 0.4
SWL=$(geom_w "$XC"); SXL=$(geom_x "$XC")
if near "$SWL" 512 6 && [ "$SXL" -le 6 ]; then
    ok "Alt+Left snaps to the left half (w=$SWL x=$SXL)"
else
    bad "Alt+Left snaps to the left half (w=$SWL x=$SXL)"
fi
xdotool key alt+Right; sleep 0.4
SWR=$(geom_w "$XC"); SXR=$(geom_x "$XC")
if near "$SWR" 512 6 && near "$SXR" 512 6; then
    ok "Alt+Right snaps to the right half (w=$SWR x=$SXR)"
else
    bad "Alt+Right snaps to the right half (w=$SWR x=$SXR)"
fi

# minimize via Alt+Down, restore from the dock icon
xdotool key alt+Down; sleep 0.4
if xwininfo -id "$XC" | grep -qE 'Map State: Is(Unmapped|unviewable|Unviewable)'; then
    ok "Alt+Down minimizes"
else
    bad "Alt+Down minimizes ($(xwininfo -id "$XC" 2>/dev/null | sed -n 's/.*Map State: //p'))"
fi
D0=($(dock_icon 0)); xdotool mousemove "${D0[0]}" "${D0[1]}"; xdotool click 1; sleep 0.4
if xwininfo -id "$XC" | grep -q 'Map State: IsViewable'; then
    ok "dock icon restores the window"
else
    bad "dock icon restores the window"
fi
[ "$(active)" = "$XC" ] && ok "restored window is focused" || bad "restored window is focused (got $(active))"

# show desktop
xdotool key alt+d; sleep 0.4
NM=0
for id in $(xprop -root _NET_CLIENT_LIST 2>/dev/null | sed 's/.*# //; s/,//g'); do
    hid=$((id))
    if xwininfo -id "$hid" 2>/dev/null | grep -q 'IsViewable'; then NM=$((NM + 1)); fi
done
[ "$NM" = "0" ] && ok "Alt+D hides every window" || bad "Alt+D hides every window ($NM visible)"
xdotool key alt+d; sleep 0.4
if xwininfo -id "$XC" | grep -q 'IsViewable'; then ok "Alt+D brings them back"; else bad "Alt+D brings them back"; fi

# right click on the desktop (empty area: left half, below the window)
xdotool mousemove 200 600; sleep 0.2
xdotool click 3; sleep 0.5
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    ok "desktop right click opens a menu"
else
    bad "desktop right click opens a menu"
fi
xdotool key Escape; sleep 0.3
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    bad "Escape closes the desktop menu"
else
    ok "Escape closes the desktop menu"
fi

# Alt+F2 with a URL opens the configured browser
rm -f "$WORK/browser_marker"
xdotool key alt+F2; sleep 0.4
xdotool type --delay 25 "example.com"; sleep 0.2
xdotool key Return; sleep 0.8
if [ -f "$HOME/browser_args" ] && grep -q "example.com" "$HOME/browser_args"; then
    ok "Alt+F2 opens a URL in the browser"
else
    bad "Alt+F2 opens a URL in the browser"
fi

# ------------------------------------------- 16. missing app notice / detection
log "[16] missing program notice + terminal detection"
kill -TERM "$SCDE_PID" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$SCDE_PID" 2>/dev/null || break; sleep 0.1; done
wait "$SCDE_PID" 2>/dev/null
SCDE_PID=""

CFG="$HOME/.config/scde/config"
sed -i '/^menu=/d; s|^terminal=.*|terminal=scde-no-such-terminal|' "$CFG"
"$BIN" >"$WORK/scde5.log" 2>&1 &
SCDE_PID=$!
sleep 0.7
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "restarts with a missing terminal set"; else bad "restarts with a missing terminal set"; fi

xdotool key alt+space; sleep 0.4
xdotool key Return; sleep 0.6
if xdotool search --onlyvisible --name '^scde-msgbox$' >/dev/null 2>&1; then
    ok "missing terminal shows a notice dialog"
else
    bad "missing terminal shows a notice dialog"
fi
xdotool key Escape; sleep 0.4
if xdotool search --onlyvisible --name '^scde-msgbox$' >/dev/null 2>&1; then
    bad "Escape closes the notice dialog"
else
    ok "Escape closes the notice dialog"
fi

# now provide a real terminal on PATH and expect it to be launched
kill -TERM "$SCDE_PID" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$SCDE_PID" 2>/dev/null || break; sleep 0.1; done
wait "$SCDE_PID" 2>/dev/null
SCDE_PID=""
mkdir -p "$HOME/bin"
cat > "$HOME/bin/xterm" <<'TERM'
#!/bin/sh
touch "$HOME/term_marker"
TERM
chmod +x "$HOME/bin/xterm"
sed -i 's|^terminal=.*|terminal=xterm|' "$CFG"
rm -f "$HOME/term_marker"
PATH="$HOME/bin:$PATH" "$BIN" >"$WORK/scde6.log" 2>&1 &
SCDE_PID=$!
sleep 0.7
xdotool key alt+space; sleep 0.4
xdotool key Return; sleep 0.7
if [ -f "$HOME/term_marker" ]; then
    ok "detected terminal is launched from the menu"
else
    bad "detected terminal is launched from the menu"
fi

# ------------------------------------------------- 17. desktop shortcuts
log "[17] desktop shortcuts (select, open, context menu, move)"
kill -TERM "$SCDE_PID" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$SCDE_PID" 2>/dev/null || break; sleep 0.1; done
wait "$SCDE_PID" 2>/dev/null
SCDE_PID=""

# empty the desktop: close every managed window
for id in $(xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -o '0x[0-9a-fA-F]\+'); do
    xdotool windowkill $((id)) 2>/dev/null
done
sleep 0.4

DESK="$HOME/Desktop"
mkdir -p "$DESK" "$HOME/.local/share/scde"
cat > "$DESK/test.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=Test Shortcut
Exec=touch $HOME/desktop_marker
ENTRY
# pin the shortcut so its position does not depend on the grid
printf '400 300 %s/test.desktop\n' "$DESK" > "$HOME/.local/share/scde/desktop.layout"
rm -f "$HOME/desktop_marker"

"$BIN" >"$WORK/scde7.log" 2>&1 &
SCDE_PID=$!
sleep 0.9
if kill -0 "$SCDE_PID" 2>/dev/null; then
    ok "starts with a desktop shortcut"
else
    bad "starts with a desktop shortcut"; cat "$WORK/scde7.log"
fi

# the shortcut is painted on the root window at its saved position
PIX=$("$PIXELBIN" 434 310 2>/dev/null | tr -d '\n')
if [ "$PIX" = "0x6f93bb" ]; then
    ok "shortcut icon is drawn on the desktop (pixel $PIX)"
else
    bad "shortcut icon is drawn on the desktop (got $PIX, want 0x6f93bb)"
fi

# single click selects the icon
xdotool mousemove 446 338; sleep 0.25
xdotool click 1; sleep 0.35
PIX=$("$PIXELBIN" 401 301 2>/dev/null | tr -d '\n')
if [ "$PIX" != "0x101014" ]; then
    ok "single click selects the shortcut (pixel $PIX)"
else
    bad "single click selects the shortcut (selection not painted)"
fi

# a click on empty space clears the selection again
xdotool mousemove 150 500; sleep 0.25
xdotool click 1; sleep 0.3
PIX=$("$PIXELBIN" 401 301 2>/dev/null | tr -d '\n')
if [ "$PIX" = "0x101014" ]; then
    ok "click on the empty desktop clears the selection"
else
    bad "click on the empty desktop clears the selection ($PIX)"
fi

# double click runs the Exec= line
xdotool mousemove 446 338; sleep 0.25
xdotool click --repeat 2 --delay 150 1; sleep 0.7
if [ -f "$HOME/desktop_marker" ]; then
    ok "double click runs the shortcut"
else
    bad "double click runs the shortcut"
fi

# right click on the shortcut opens its own menu
xdotool mousemove 446 338; sleep 0.25
xdotool click 3; sleep 0.5
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    ok "right click on a shortcut opens a menu"
else
    bad "right click on a shortcut opens a menu"
fi
xdotool key Escape; sleep 0.3

# right click on empty space -> New Folder (8th entry of the desktop menu)
xdotool mousemove 150 400; sleep 0.25
xdotool click 3; sleep 0.5
xdotool key Down Down Down Down Down Down; sleep 0.2
xdotool key Return; sleep 0.5
if [ -d "$DESK/New Folder" ]; then
    ok "desktop menu creates a new folder"
else
    bad "desktop menu creates a new folder"
fi

# dragging the shortcut rewrites the layout file
xdotool mousemove 446 338; sleep 0.3
xdotool mousedown 1; sleep 0.3
xdotool mousemove 600 400; sleep 0.4
xdotool mouseup 1; sleep 0.5
LAY="$HOME/.local/share/scde/desktop.layout"
LX=$(awk '{print $1}' "$LAY" 2>/dev/null)
LY=$(awk '{print $2}' "$LAY" 2>/dev/null)
if [ -n "${LX:-}" ] && [ "${LX:-0}" -gt 400 ] && grep -q "$DESK/test.desktop" "$LAY"; then
    ok "dragging a shortcut saves its position ($LX $LY)"
else
    bad "dragging a shortcut saves its position (${LX:-?} ${LY:-?})"
fi

# --------------------------------------- 18. dock integration (internal)
log "[18] dock: File Manager + SCDE Security entries"

# The taskbar registry publishes its entries on the root window so the
# window list can be verified exactly instead of pixel guessing.  Only real
# window ids are ever extracted (a missing property must yield nothing).
tasklist()  { xprop -root _SCDE_TASK_LIST 2>/dev/null | grep -o '0x[0-9a-fA-F]*' | tr '\n' ' '; }
tasknames() { local w; for w in $(tasklist); do xdotool getwindowname $((w)) 2>/dev/null; done; }
taskcount() { tasknames | grep -c -- "$1"; }
# dock icons are computed from the live dock geometry (helper above);
# the first two taskbar entries sit under dock icons 0 and 1

# start from a clean desktop: managed windows are covered by tests [3] and
# [15]; this block exercises the internal application windows, and their
# button positions must be predictable.
for id in $(xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -o '0x[0-9a-fA-F]*'); do
    xdotool windowkill $((id)) 2>/dev/null
done
for i in $(seq 1 30); do
    xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -q '0x' || break
    sleep 0.2
done
if xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -q '0x'; then
    bad "taskbar test starts without managed windows"
else
    ok "taskbar test starts without managed windows"
fi

# 1. open the file manager -> its entry appears
xdotool key alt+e; sleep 0.7
FM=$(xdotool search --onlyvisible --name '^scde-fm' 2>/dev/null | head -1)
if [ -n "${FM:-}" ]; then ok "File Manager window opens"; else bad "File Manager window opens"; fi
if [ "$(taskcount 'scde-fm')" = "1" ]; then
    ok "File Manager taskbar entry exists"
else
    bad "File Manager taskbar entry exists (got $(taskcount 'scde-fm'))"
fi

# 2. open the security center -> its entry appears
xdotool key alt+s; sleep 0.7
SG=$(xdotool search --onlyvisible --name '^SCDE Security' 2>/dev/null | head -1)
if [ -n "${SG:-}" ]; then ok "SCDE Security window opens"; else bad "SCDE Security window opens"; fi
if [ "$(taskcount 'SCDE Security')" = "1" ]; then
    ok "SCDE Security taskbar entry exists"
else
    bad "SCDE Security taskbar entry exists (got $(taskcount 'SCDE Security'))"
fi
if [ "$(tasklist | wc -w)" = "2" ]; then
    ok "exactly two taskbar entries"
else
    bad "exactly two taskbar entries ($(tasklist | wc -w))"
fi

# 3. showing an already open window must never duplicate its entry
xdotool windowactivate "$FM"; sleep 0.4
xdotool windowactivate "$SG"; sleep 0.4
xdotool windowactivate "$FM"; sleep 0.4
xdotool windowactivate "$SG"; sleep 0.4
if [ "$(tasklist | wc -w)" = "2" ] &&
   [ "$(taskcount 'scde-fm')" = "1" ] &&
   [ "$(taskcount 'SCDE Security')" = "1" ]; then
    ok "showing already open windows keeps exactly one entry each"
else
    bad "showing already open windows keeps exactly one entry each ($(tasklist | wc -w) entries)"
fi

# 4. minimising keeps the entry, hiding the window
if [ -n "${FM:-}" ]; then xdotool windowactivate "$FM"; sleep 0.4; fi
xdotool key alt+Down; sleep 0.6
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    bad "Alt+Down hides the file manager"
else
    ok "Alt+Down hides the file manager"
fi
if [ "$(taskcount 'scde-fm')" = "1" ]; then
    ok "hidden File Manager keeps its taskbar entry"
else
    bad "hidden File Manager keeps its taskbar entry"
fi

# 5. clicking the icon restores and focuses the window
D0=($(dock_icon 0)); xdotool mousemove "${D0[0]}" "${D0[1]}"; sleep 0.25
xdotool click 1; sleep 0.6
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    ok "dock click restores the File Manager"
else
    bad "dock click restores the File Manager"
fi
FM=$(xdotool search --onlyvisible --name '^scde-fm' 2>/dev/null | head -1)
if [ "$(xdotool getwindowfocus 2>/dev/null)" = "$FM" ]; then
    ok "dock click focuses the File Manager"
else
    bad "dock click focuses the File Manager (got $(xdotool getwindowfocus 2>/dev/null), want $FM)"
fi

# 6. closing the File Manager drops only its entry
if [ -n "${FM:-}" ]; then xdotool windowactivate "$FM"; sleep 0.4; fi
xdotool key Escape; sleep 0.6
if [ "$(taskcount 'scde-fm')" = "0" ]; then
    ok "closing the File Manager removes its taskbar entry"
else
    bad "closing the File Manager removes its taskbar entry"
fi
if [ "$(taskcount 'SCDE Security')" = "1" ]; then
    ok "the other taskbar entry is untouched"
else
    bad "the other taskbar entry is untouched"
fi
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    bad "File Manager window is gone after close"
else
    ok "File Manager window is gone after close"
fi

# 7. Alt+F4 closes the security center and drops its entry
SG=$(xdotool search --onlyvisible --name '^SCDE Security' 2>/dev/null | head -1)
if [ -n "${SG:-}" ]; then xdotool windowactivate "$SG"; sleep 0.5; fi
xdotool key alt+F4; sleep 0.7
if [ "$(taskcount 'SCDE Security')" = "0" ]; then
    ok "Alt+F4 removes the SCDE Security taskbar entry"
else
    bad "Alt+F4 removes the SCDE Security taskbar entry"
fi
if [ "$(tasklist | wc -w)" = "0" ]; then
    ok "task list is empty after closing both"
else
    bad "task list is empty after closing both ($(tasknames | tr '\n' ';'))"
fi

# 8. reopening brings back exactly one entry per application
xdotool key alt+e; sleep 0.7
xdotool key alt+s; sleep 0.7
if [ "$(taskcount 'scde-fm')" = "1" ]; then
    ok "exactly one File Manager entry after reopen"
else
    bad "exactly one File Manager entry after reopen"
fi
if [ "$(taskcount 'SCDE Security')" = "1" ]; then
    ok "exactly one SCDE Security entry after reopen"
else
    bad "exactly one SCDE Security entry after reopen"
fi
if [ "$(tasklist | wc -w)" = "2" ]; then
    ok "exactly two entries after reopening both"
else
    bad "exactly two entries after reopening both ($(tasklist | wc -w))"
fi

# 9. the second icon belongs to the security center
D1=($(dock_icon 1)); xdotool mousemove "${D1[0]}" "${D1[1]}"; sleep 0.25
xdotool click 1; sleep 0.6
SG=$(xdotool search --onlyvisible --name '^SCDE Security' 2>/dev/null | head -1)
if [ "$(xdotool getwindowfocus 2>/dev/null)" = "$SG" ]; then
    ok "dock click focuses the SCDE Security window"
else
    bad "dock click focuses the SCDE Security window (got $(xdotool getwindowfocus 2>/dev/null), want ${SG:-?})"
fi

# 10. show desktop hides both, both entries stay, restore brings them back
xdotool key alt+d; sleep 0.7
if [ "$(taskcount 'scde-fm')" = "1" ] && [ "$(taskcount 'SCDE Security')" = "1" ]; then
    ok "show desktop keeps both taskbar entries"
else
    bad "show desktop keeps both taskbar entries"
fi
xdotool key alt+d; sleep 0.7
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1 &&
   xdotool search --onlyvisible --name '^SCDE Security' >/dev/null 2>&1; then
    ok "show desktop restores both windows"
else
    bad "show desktop restores both windows"
fi

# leave a clean desktop behind: close both, entry by entry
SG=$(xdotool search --onlyvisible --name '^SCDE Security' 2>/dev/null | head -1)
if [ -n "${SG:-}" ]; then
    xdotool windowactivate "$SG"; sleep 0.4
    xdotool key Escape; sleep 0.5
fi
FM=$(xdotool search --onlyvisible --name '^scde-fm' 2>/dev/null | head -1)
if [ -n "${FM:-}" ]; then
    xdotool windowactivate "$FM"; sleep 0.4
    xdotool key Escape; sleep 0.5
fi
if [ "$(tasklist | wc -w)" = "0" ]; then
    ok "desktop left with an empty taskbar"
else
    bad "desktop left with an empty taskbar ($(tasknames | tr '\n' ';'))"
fi

# --------------------------------------- 19. Harbor redesign regression
log "[19] Harbor redesign: FM sidebar + search, Security cards, wallpaper"
# File Manager geometry on this screen: 860x600, centred.
# sidebar = left 184 px, toolbar search box = right 150 px of the 36 px bar.
xdotool key alt+e; sleep 0.7
FM=$(xdotool search --onlyvisible --name '^scde-fm' 2>/dev/null | head -1)
if [ -n "${FM:-}" ]; then ok "Harbor FM opens"; else bad "Harbor FM opens"; fi
check "Harbor FM keeps its 860 px width" \
    sh -c "[ -n '${FM:-}' ] && xwininfo -id $FM | grep -qE 'Width: +860'"
FMNAME=$(xdotool getwindowname "$FM" 2>/dev/null)
if [ "$FMNAME" = "scde-fm: $HOME" ]; then
    ok "Harbor FM opens in HOME"
else
    bad "Harbor FM opens in HOME (got '$FMNAME')"
fi
FXX=$(geom_x "$FM"); FYY=$(geom_y "$FM")

# search box lives at the right end of the toolbar (36 px tall)
xdotool mousemove $((FXX + 777)) $((FYY + 44)); sleep 0.25
xdotool click 1; sleep 0.3
xdotool type --delay 30 "fmdir"; sleep 0.6
FMNAME=$(xdotool getwindowname "$FM" 2>/dev/null)
case "$FMNAME" in
    *"[filter 'fmdir': 1]"*) ok "search filter narrows to one match" ;;
    *) bad "search filter narrows to one match (got '$FMNAME')" ;;
esac

# Escape clears an active filter but keeps the window open
xdotool key Escape; sleep 0.5
FMNAME=$(xdotool getwindowname "$FM" 2>/dev/null)
if [ "$FMNAME" = "scde-fm: $HOME" ]; then
    ok "Escape clears the search filter"
else
    bad "Escape clears the search filter (got '$FMNAME')"
fi
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    ok "clearing the filter keeps the FM open"
else
    bad "clearing the filter keeps the FM open"
fi

# double click on the first visible row ("..", below the column header):
# exercises the offset list click mapping, ascends to the parent
xdotool mousemove $((FXX + 400)) $((FYY + 98)); sleep 0.25
xdotool click --repeat 2 --delay 150 1; sleep 0.7
FMNAME=$(xdotool getwindowname "$FM" 2>/dev/null)
if [ "$FMNAME" = "scde-fm: $WORK" ]; then
    ok "list click mapping works beside the sidebar"
else
    bad "list click mapping works beside the sidebar (got '$FMNAME')"
fi

# sidebar "Home" row (first place row) navigates back to HOME
xdotool mousemove $((FXX + 90)) $((FYY + 102)); sleep 0.25
xdotool click 1; sleep 0.6
FMNAME=$(xdotool getwindowname "$FM" 2>/dev/null)
if [ "$FMNAME" = "scde-fm: $HOME" ]; then
    ok "sidebar Home navigates to HOME"
else
    bad "sidebar Home navigates to HOME (got '$FMNAME')"
fi

# search + Return descends into the match
xdotool mousemove $((FXX + 777)) $((FYY + 44)); sleep 0.25
xdotool click 1; sleep 0.3
xdotool type --delay 30 "fmdir"; sleep 0.6
xdotool key Return; sleep 0.6
FMNAME=$(xdotool getwindowname "$FM" 2>/dev/null)
case "$FMNAME" in
    *"/fmdir"*) ok "search + Return descends into the match" ;;
    *) bad "search + Return descends into the match (got '$FMNAME')" ;;
esac
xdotool key Escape; sleep 0.5
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    bad "Escape closes the Harbor FM"
else
    ok "Escape closes the Harbor FM"
fi

# Security Center keeps its card size, title and taskbar entry
xdotool key alt+s; sleep 0.7
SG=$(xdotool search --onlyvisible --name '^SCDE Security' 2>/dev/null | head -1)
if [ -n "${SG:-}" ]; then ok "Harbor Security Center opens"; else bad "Harbor Security Center opens"; fi
check "Security Center keeps its 600 px card width" \
    sh -c "[ -n '${SG:-}' ] && xwininfo -id $SG | grep -qE 'Width: +600'"
check "Security Center keeps its 640 px card height" \
    sh -c "[ -n '${SG:-}' ] && xwininfo -id $SG | grep -qE 'Height: +640'"
if [ "$(taskcount 'SCDE Security')" = "1" ]; then
    ok "Harbor Security keeps one taskbar entry"
else
    bad "Harbor Security keeps one taskbar entry"
fi
if [ -n "${SG:-}" ]; then xdotool windowactivate "$SG"; sleep 0.4; fi
xdotool key Escape; sleep 0.5
if [ "$(tasklist | wc -w)" = "0" ]; then
    ok "Harbor Security closes cleanly"
else
    bad "Harbor Security closes cleanly ($(tasknames | tr '\n' ';'))"
fi

# design tokens stay in the source (guards against accidental reverts)
check "Harbor accent in the palette" grep -q "#7c9cbf" "$ROOT/src/config.c"
check "menu bar in the source" grep -q "scde-menubar" "$ROOT/src/panel.c"
check "floating dock in the source" grep -q "scde-dock" "$ROOT/src/dock.c"
check "FM favorites sidebar in the source" grep -q "fav_names" "$ROOT/src/fm.c"
check "FM forward history in the source" grep -q "n_fwd" "$ROOT/src/fm.c"
check "FM search filter in the source" grep -q "sbuf" "$ROOT/src/fm.c"
check "Security cards in the source" grep -q "sg_card" "$ROOT/src/security_gui.c"
check "wallpaper motif in the source" grep -q "Harbor signature" "$ROOT/src/main.c"
check "3 GiB ceiling in the sandbox" grep -q "3072" "$ROOT/src/security/sandbox.c"
check "no SCDE CPU quota in resource defaults" grep -q "cpu_seconds = 0" "$ROOT/src/security/resource.c"

# gradient wallpaper: restart with distinct stops, sample two flat points
kill -TERM "$SCDE_PID" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$SCDE_PID" 2>/dev/null || break; sleep 0.1; done
wait "$SCDE_PID" 2>/dev/null
SCDE_PID=""
sed -i 's/^wallpaper_top=.*/wallpaper_top=#262e3b/; s/^wallpaper_bottom=.*/wallpaper_bottom=#12151b/' "$HOME/.config/scde/config"
"$BIN" >"$WORK/scde8.log" 2>&1 &
SCDE_PID=$!
sleep 0.9
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "restarts with the Harbor gradient"; else bad "restarts with the Harbor gradient"; fi
chan() { printf '%d' "0x${1:$((2 + $2 * 2)):2}"; }
TOP=$("$PIXELBIN" 700 40 2>/dev/null | tr -d '\n')
BOT=$("$PIXELBIN" 700 730 2>/dev/null | tr -d '\n')
if near "$(chan "$TOP" 0)" 37 5 && near "$(chan "$TOP" 1)" 45 5 && near "$(chan "$TOP" 2)" 58 5; then
    ok "gradient top reads back ($TOP)"
else
    bad "gradient top reads back ($TOP, want ~0x252d3a)"
fi
if near "$(chan "$BOT" 0)" 19 5 && near "$(chan "$BOT" 1)" 23 5 && near "$(chan "$BOT" 2)" 29 5; then
    ok "gradient bottom reads back ($BOT)"
else
    bad "gradient bottom reads back ($BOT, want ~0x13171d)"
fi
if [ "$TOP" != "$BOT" ]; then
    ok "wallpaper is a real gradient, not flat"
else
    bad "wallpaper is a real gradient, not flat"
fi

# --------------------------------------- 20. menu bar menus + dock + limits
log "[20] menu bar launcher, dock right-click, sandbox memory config"

# menu bar launcher mark opens the application menu
MB=$(xdotool search --onlyvisible --name '^scde-menubar$' 2>/dev/null | head -1)
if [ -n "${MB:-}" ]; then ok "menu bar present for menu test"; else bad "menu bar present for menu test"; fi
MBH=$(geom_h "$MB")
xdotool mousemove 21 $((MBH / 2)); sleep 0.25
xdotool click 1; sleep 0.5
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    ok "menu bar mark opens the application menu"
else
    bad "menu bar mark opens the application menu"
fi
xdotool key Escape; sleep 0.4
if xdotool search --onlyvisible --name '^scde-menu$' >/dev/null 2>&1; then
    bad "Escape closes the application menu"
else
    ok "Escape closes the application menu"
fi

# dock grows with entries, right-click minimises, left-click restores
DW0=$(geom_w "$(dock_win)")
xdotool key alt+e; sleep 0.7
DW1=$(geom_w "$(dock_win)")
if [ -n "${DW1:-}" ] && [ -n "${DW0:-}" ] && [ "$DW1" -gt "$DW0" ]; then
    ok "dock grows with a new entry ($DW0 -> $DW1)"
else
    bad "dock grows with a new entry (${DW0:-?} -> ${DW1:-?})"
fi
D0=($(dock_icon 0)); xdotool mousemove "${D0[0]}" "${D0[1]}"; sleep 0.25
xdotool click 3; sleep 0.6
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    bad "dock right click minimises the File Manager"
else
    ok "dock right click minimises the File Manager"
fi
if [ "$(taskcount 'scde-fm')" = "1" ]; then
    ok "minimised entry stays in the dock registry"
else
    bad "minimised entry stays in the dock registry"
fi
xdotool mousemove "${D0[0]}" "${D0[1]}"; sleep 0.25
xdotool click 1; sleep 0.6
if xdotool search --onlyvisible --name '^scde-fm' >/dev/null 2>&1; then
    ok "dock left click restores the File Manager"
else
    bad "dock left click restores the File Manager"
fi
xdotool key Escape; sleep 0.5
DW2=$(geom_w "$(dock_win)")
if [ "${DW2:-}" = "${DW0:-}" ]; then
    ok "dock shrinks back after close ($DW1 -> $DW2)"
else
    bad "dock shrinks back after close ($DW1 -> ${DW2:-?}, want ${DW0:-?})"
fi

# sandbox_memory_mb is a real config key (parser accepts it, no warning)
if grep -q "unknown key 'sandbox_memory_mb'" "$WORK/scde.log"; then
    bad "sandbox_memory_mb accepted by the config parser"
else
    ok "sandbox_memory_mb accepted by the config parser"
fi

# ------------------------------------------------------------- summary
log ""
log "== results: $PASS passed, $FAIL failed =="
if [ "$FAIL" -gt 0 ]; then
    log "---- scde log ----"
    cat "$WORK/scde.log" 2>/dev/null
    exit 1
fi
exit 0
