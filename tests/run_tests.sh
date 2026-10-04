#!/usr/bin/env bash
# SCDE test suite.  Requires: Xvfb, xdotool, x11-utils (xwininfo/xprop/xdpyinfo)
set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN="$ROOT/scde"
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

cleanup() {
    export HOME="$HOME_SAVE"
    local p
    for p in "${CLIENTS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    [ -n "$SCDE_PID" ] && kill "$SCDE_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
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
panel=bottom
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
log "[1] startup / singleton / panel"
"$BIN" >"$WORK/scde.log" 2>&1 &
SCDE_PID=$!
sleep 0.6
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "scde starts"; else bad "scde starts"; cat "$WORK/scde.log"; exit 1; fi

if "$BIN" >"$WORK/scde2.log" 2>&1; then bad "second instance refused"; else ok "second instance refused"; fi
grep -qi "already" "$WORK/scde2.log" && ok "singleton message printed" || bad "singleton message printed"

if xwininfo -root -children 2>/dev/null | grep -q "scde-panel"; then
    ok "panel window created"
else
    bad "panel window created"
fi
check "panel sits on the bottom edge" \
    sh -c "xwininfo -root -children | grep scde-panel | grep -q '+0+$((H - 28))'"

BG=$("$PIXELBIN" 2>/dev/null | tr -d '\n')
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
[ "$(active)" = "$WB" ] && ok "panel does not steal focus" || bad "panel does not steal focus (got $(active))"

# the panel lists clients in mapping order: first button = first list entry
LIST=$(xprop -root _NET_CLIENT_LIST 2>/dev/null | sed 's/.*# //; s/,//g')
BTN1=$(( $(echo "$LIST" | awk '{print $1}') ))
BTN2=$(( $(echo "$LIST" | awk '{print $2}') ))
xdotool mousemove 100 $((H - 14)); xdotool click 1; sleep 0.35
[ "$(active)" = "$BTN1" ] && ok "panel window list first button focuses its window" || bad "panel window list first button focuses its window (got $(active), want $BTN1)"
xdotool mousemove 250 $((H - 14)); xdotool click 1; sleep 0.35
[ "$(active)" = "$BTN2" ] && ok "panel window list second button focuses its window" || bad "panel window list second button focuses its window (got $(active), want $BTN2)"

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
for p in $(pgrep -x scde 2>/dev/null); do
    if tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qxF "DISPLAY=$DPY"; then
        NR_PROC=$((NR_PROC + 1))
    fi
done
if [ "$NR_PROC" = "1" ]; then ok "exactly one scde process"; else bad "exactly one scde process ($NR_PROC)"; fi

NR_TASK=$(ls "/proc/$SCDE_PID/task" 2>/dev/null | wc -l)
if [ "$NR_TASK" = "1" ]; then ok "single thread"; else bad "single thread ($NR_TASK)"; fi

RSS_KB=$(ps -o rss= -p "$SCDE_PID" 2>/dev/null | tr -d ' ')
if [ -n "$RSS_KB" ] && [ "$RSS_KB" -lt 40960 ]; then
    ok "resident memory ${RSS_KB} kB (< 40 MB)"
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
log "[14] config reload (panel=top) + existing windows"
kill -TERM "$SCDE_PID" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$SCDE_PID" 2>/dev/null || break; sleep 0.1; done
wait "$SCDE_PID" 2>/dev/null
SCDE_PID=""
sed -i 's/panel=bottom/panel=top/' "$HOME/.config/scde/config"
"$BIN" >"$WORK/scde4.log" 2>&1 &
SCDE_PID=$!
sleep 0.8
if kill -0 "$SCDE_PID" 2>/dev/null; then ok "restarts with new config"; else bad "restarts with new config"; cat "$WORK/scde4.log"; fi
check "panel sits on the top edge" \
    sh -c "xwininfo -root -children | grep scde-panel | grep -q '+0+0'"
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

# minimize via Alt+Down, restore from the panel task button
xdotool key alt+Down; sleep 0.4
if xwininfo -id "$XC" | grep -qE 'Map State: Is(Unmapped|unviewable|Unviewable)'; then
    ok "Alt+Down minimizes"
else
    bad "Alt+Down minimizes ($(xwininfo -id "$XC" 2>/dev/null | sed -n 's/.*Map State: //p'))"
fi
PANY=$([ "$(grep -c 'panel=top' "$HOME/.config/scde/config")" -gt 0 ] && echo 14 || echo $((H - 14)))
xdotool mousemove 100 "$PANY"; xdotool click 1; sleep 0.4
if xwininfo -id "$XC" | grep -q 'Map State: IsViewable'; then
    ok "panel task button restores the window"
else
    bad "panel task button restores the window"
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

# ------------------------------------------------------------- summary
log ""
log "== results: $PASS passed, $FAIL failed =="
if [ "$FAIL" -gt 0 ]; then
    log "---- scde log ----"
    cat "$WORK/scde.log" 2>/dev/null
    exit 1
fi
exit 0
