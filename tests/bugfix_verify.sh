#!/usr/bin/env bash
# Regression checks: desktop icons survive a window drag, FM and the
# Security Center can be dragged from their title bars, focus follows.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DPY=":99"
WORK=$(mktemp -d /tmp/scde-bugfix.XXXXXX)
HOME_SAVE="$HOME"
SCDE_PID=""; CP=""; XPID=""
FAILS=0

ok()  { echo "  PASS: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }

cleanup() {
    export HOME="$HOME_SAVE"
    kill "$SCDE_PID" "$CP" "$XPID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

pix() {  # pix file x y -> 0xRRGGBB
    python3 -c "
import sys
f = open(sys.argv[1], 'rb')
assert f.readline().strip() == b'P6'
l = f.readline()
while l.startswith(b'#'):
    l = f.readline()
w, h = map(int, l.split())
f.readline()
d = f.read()
x, y = int(sys.argv[2]), int(sys.argv[3])
i = (y * w + x) * 3
print('0x%02x%02x%02x' % (d[i], d[i+1], d[i+2]))
" "$1" "$2" "$3"
}

shot() { "$ROOT/tests/screenshot" "$WORK/$1.ppm" >/dev/null 2>&1; }

export HOME="$WORK/home"
mkdir -p "$HOME/.config/scde"
cat > "$HOME/.config/scde/config" <<EOF
panel=bottom
panel_height=32
clock_format=%H:%M
EOF

export DISPLAY="$DPY"
Xvfb "$DPY" -screen 0 1024x768x24 -nolisten tcp >"$WORK/xvfb.log" 2>&1 &
XPID=$!
for i in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done

"$ROOT/scde" >"$WORK/scde.log" 2>&1 &
SCDE_PID=$!
sleep 1.0

echo "[1] desktop icons survive a window drag"
shot s0
A0=$(pix "$WORK/s0.ppm" 58 118)    # col0 row1 icon (triangle cutout, 0x2b2b2b)
B0=$(pix "$WORK/s0.ppm" 160 118)   # col1 row1
C0=$(pix "$WORK/s0.ppm" 58 204)    # col0 row2
D0=$(pix "$WORK/s0.ppm" 58 290)    # col0 row3
WP0=$(pix "$WORK/s0.ppm" 600 700)  # wallpaper reference
echo "  before: $A0 $B0 $C0 $D0"
if [ "$A0" != "$WP0" ]; then
    ok "icon pixels present before the drag ($A0)"
else
    bad "no icon at (58,118), wallpaper shows through ($A0)"
fi

"$ROOT/tests/testclient" -n "cover" -g 380x370+40+60 >/dev/null 2>&1 &
CP=$!
sleep 0.8
COVERWIN=$(xdotool search --name "^cover$" 2>/dev/null | head -1)
if [ -z "$COVERWIN" ]; then
    COVERWIN=$(xdotool search --name "cover" 2>/dev/null | head -1)
fi
if [ -z "$COVERWIN" ]; then
    bad "testclient window did not appear"
else
    ok "testclient window up (id $COVERWIN)"
fi
eval "$(xdotool getwindowgeometry --shell "$COVERWIN" 2>/dev/null)" || true
XW0=${X:-0}; YW0=${Y:-0}
shot s05
C5=$(pix "$WORK/s05.ppm" 58 118)
if [ "$C5" != "$A0" ]; then
    ok "window covers the icon under test before the drag ($C5)"
else
    bad "window does not cover (58,118): $C5"
fi

# title drag: frame top ~y=73, sweep across the icon columns
xdotool mousemove 200 48
sleep 0.2
xdotool mousedown 1
for p in "300 150" "400 250" "500 350" "600 450" "650 520"; do
    xdotool mousemove $p
    sleep 0.08
done
xdotool mouseup 1
sleep 0.5

shot s1
A1=$(pix "$WORK/s1.ppm" 58 118)
B1=$(pix "$WORK/s1.ppm" 160 118)
C1=$(pix "$WORK/s1.ppm" 58 204)
D1=$(pix "$WORK/s1.ppm" 58 290)
echo "  after:  $A1 $B1 $C1 $D1"
[ "$A1" = "$A0" ] && ok "icon col0 row1 back after drag" || bad "icon lost: $A0 -> $A1"
[ "$B1" = "$B0" ] && ok "icon col1 row1 back after drag" || bad "icon lost: $B0 -> $B1"
[ "$C1" = "$C0" ] && ok "icon col0 row2 back after drag" || bad "icon lost: $C0 -> $C1"
[ "$D1" = "$D0" ] && ok "icon col0 row3 back after drag" || bad "icon lost: $D0 -> $D1"

if [ -n "$COVERWIN" ]; then
    eval "$(xdotool getwindowgeometry --shell "$COVERWIN")"
    if [ "$X" -gt "$XW0" ] && [ "$Y" -gt "$YW0" ]; then
        ok "testclient moved by the drag ($XW0,$YW0 -> $X,$Y)"
    else
        bad "testclient did not move ($XW0,$YW0 -> $X,$Y)"
    fi
fi

# wallpaper under the old window position matches an untouched spot
W1=$(pix "$WORK/s1.ppm" 300 200)
W0=$(pix "$WORK/s0.ppm" 450 200)   # same row, never covered
[ "$W1" = "$W0" ] && ok "wallpaper restored under old position ($W1)" \
    || bad "wallpaper mismatch: old area $W1 vs reference $W0"

echo "[2] file manager can be dragged from its title bar"
xdotool key alt+e
sleep 0.7
FMWIN=$(xdotool search --name "scde-fm" 2>/dev/null | head -1)
if [ -z "$FMWIN" ]; then
    bad "file manager did not open"
else
    eval "$(xdotool getwindowgeometry --shell "$FMWIN")"
    X0=$X; Y0=$Y
    xdotool mousemove $((X0 + 150)) $((Y0 + 12))
    sleep 0.2
    xdotool mousedown 1
    for p in "$((X0 + 220)) $((Y0 + 60))" "$((X0 + 300)) $((Y0 + 120))" \
             "$((X0 + 340)) $((Y0 + 150))"; do
        xdotool mousemove $p
        sleep 0.08
    done
    xdotool mouseup 1
    sleep 0.4
    eval "$(xdotool getwindowgeometry --shell "$FMWIN")"
    X1=$X; Y1=$Y
    if [ "$X1" -gt "$X0" ] && [ "$Y1" -gt "$Y0" ]; then
        ok "file manager moved ($X0,$Y0 -> $X1,$Y1)"
    else
        bad "file manager did not move ($X0,$Y0 -> $X1,$Y1)"
    fi
    sleep 0.5
    eval "$(xdotool getwindowgeometry --shell "$FMWIN")"
    if [ "$X" = "$X1" ] && [ "$Y" = "$Y1" ]; then
        ok "file manager stays at the new position"
    else
        bad "file manager jumped back ($X1,$Y1 -> $X,$Y)"
    fi

    # focus moves to the file manager when clicked inside
    xdotool mousemove $((X1 + 200)) $((Y1 + 90))
    sleep 0.2
    xdotool click 1
    sleep 0.3
    FOCUS=$(xdotool getwindowfocus)
    if [ "$FOCUS" = "$FMWIN" ]; then
        ok "clicking the file manager focuses it"
    else
        bad "focus after click: got $FOCUS want $FMWIN"
    fi
    xdotool key Escape
    sleep 0.4
fi

echo "[3] Security Center can be dragged from its title bar"
# open via the tray shield (accent-blue pixel in the panel band)
SXY=$(python3 - "$WORK/s0.ppm" <<'PY'
import sys
with open(sys.argv[1], 'rb') as f:
    assert f.readline().strip() == b'P6'
    l = f.readline()
    while l.startswith(b'#'):
        l = f.readline()
    w, h = map(int, l.split())
    f.readline()
    d = f.read(w*h*3)
for x in range(760, w - 10):
    for y in range(h - 40, h - 4):
        i = (y*w + x)*3
        if abs(d[i]-0) < 20 and abs(d[i+1]-0x78) < 20 and abs(d[i+2]-0xd7) < 20:
            print(x, y)
            sys.exit(0)
print("")
PY
)
if [ -z "$SXY" ]; then
    bad "tray shield not found"
else
    set -- $SXY
    xdotool mousemove $(($1 + 7)) "$2"
    sleep 0.25
    xdotool click 1
    sleep 0.7
    SGWIN=$(xdotool search --name "SCDE Security" 2>/dev/null | head -1)
    if [ -z "$SGWIN" ]; then
        bad "Security Center did not open"
    else
        eval "$(xdotool getwindowgeometry --shell "$SGWIN")"
        X0=$X; Y0=$Y
        xdotool mousemove $((X0 + 300)) $((Y0 + 12))
        sleep 0.2
        xdotool mousedown 1
        for p in "$((X0 + 380)) $((Y0 + 70))" "$((X0 + 460)) $((Y0 + 130))" \
                 "$((X0 + 500)) $((Y0 + 160))"; do
            xdotool mousemove $p
            sleep 0.08
        done
        xdotool mouseup 1
        sleep 0.4
        eval "$(xdotool getwindowgeometry --shell "$SGWIN")"
        X1=$X; Y1=$Y
        if [ "$X1" -gt "$X0" ] && [ "$Y1" -gt "$Y0" ]; then
            ok "Security Center moved ($X0,$Y0 -> $X1,$Y1)"
        else
            bad "Security Center did not move ($X0,$Y0 -> $X1,$Y1)"
        fi
        sleep 0.5
        eval "$(xdotool getwindowgeometry --shell "$SGWIN")"
        if [ "$X" = "$X1" ] && [ "$Y" = "$Y1" ]; then
            ok "Security Center stays at the new position"
        else
            bad "Security Center jumped back ($X1,$Y1 -> $X,$Y)"
        fi
        xdotool mousemove $((X1 + 300)) $((Y1 + 200))
        sleep 0.2
        xdotool click 1
        sleep 0.3
        FOCUS=$(xdotool getwindowfocus)
        if [ "$FOCUS" = "$SGWIN" ]; then
            ok "clicking the Security Center focuses it"
        else
            bad "focus after click: got $FOCUS want $SGWIN"
        fi
        xdotool key Escape
        sleep 0.3
    fi
fi

echo "[4] managed windows still drag / maximize / close"
if [ -n "${COVERWIN:-}" ]; then
    eval "$(xdotool getwindowgeometry --shell "$COVERWIN")"
    TX=$((X + 150)); TY=$((Y - 36))
    xdotool mousemove $TX $TY
    sleep 0.2
    xdotool mousedown 1
    xdotool mousemove $((TX - 120)) $((TY - 30))
    sleep 0.15
    xdotool mouseup 1
    sleep 0.4
    eval "$(xdotool getwindowgeometry --shell "$COVERWIN")"
    if [ "$X" -lt 380 ]; then
        ok "testclient still title-draggable (x=$X)"
    else
        bad "testclient title drag broken (x=$X)"
    fi
    xdotool key alt+F4
    sleep 0.5
    if xdotool search --name "cover" >/dev/null 2>&1; then
        bad "Alt+F4 did not close the testclient"
    else
        ok "Alt+F4 closes the testclient"
    fi
fi

echo ""
echo "== bugfix results: $FAILS failure(s) =="
exit $((FAILS > 0))
