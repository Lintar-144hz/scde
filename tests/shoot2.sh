#!/usr/bin/env bash
# Visual check for desktop-left placement, taskbar close X, tray shield,
# and the built-in Security Center window.  Takes three PNGs.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
DIR=${1:-/tmp/opencode/shots}
DPY=":96"
WORK=$(mktemp -d /tmp/scde-shoot2.XXXXXX)
HOME_SAVE="$HOME"
SCDE_PID=""; CP1=""; CP2=""; XPID=""

cleanup() {
    export HOME="$HOME_SAVE"
    kill "$SCDE_PID" "$CP1" "$CP2" "$XPID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$DIR"
export HOME="$WORK/home"
mkdir -p "$HOME/.config/scde"
cat > "$HOME/.config/scde/config" <<EOF
panel=bottom
panel_height=32
border_width=1
title_height=24
background=#0e1114
wallpaper_top=#1b2436
wallpaper_bottom=#0b0d12
clock_format=%H:%M
clock_date_format=%b %d
menu=Terminal|xterm
menu=Editor|vi
menu=Files|xdg-open "\$HOME"
EOF

export DISPLAY="$DPY"
Xvfb "$DPY" -screen 0 1024x768x24 -nolisten tcp >"$WORK/xvfb.log" 2>&1 &
XPID=$!
for i in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done

"$ROOT/scde" >"$WORK/scde.log" 2>&1 &
SCDE_PID=$!
sleep 0.7

"$ROOT/tests/testclient" -n "editor — notes.txt" -g 380x260+90+90 >/dev/null 2>&1 &
CP1=$!
"$ROOT/tests/testclient" -n "build — scde" -g 420x300+520+330 >/dev/null 2>&1 &
CP2=$!
sleep 1.0
xdotool mousemove 700 450
sleep 0.3

shot() {
    "$ROOT/tests/screenshot" "$WORK/shot.ppm" >/dev/null 2>&1
    python3 - "$WORK/shot.ppm" "$DIR/$1" <<'PY'
import struct, sys, zlib
src, dst = sys.argv[1], sys.argv[2]
with open(src, 'rb') as f:
    assert f.readline().strip() == b'P6'
    line = f.readline()
    while line.startswith(b'#'):
        line = f.readline()
    w, h = map(int, line.split())
    assert int(f.readline()) == 255
    data = f.read(w * h * 3)
raw = b''.join(b'\x00' + data[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(tag, payload):
    c = struct.pack('>I', len(payload)) + tag + payload
    return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(raw, 6))
       + chunk(b'IEND', b''))
with open(dst, 'wb') as f:
    f.write(png)
print(f"wrote {dst} ({w}x{h})")
PY
}

shot "desktop_taskbar.png"

# locate the accent-blue shield in the tray band of the bottom panel
SHIELD_X=$(python3 - "$WORK/shot.ppm" <<'PY'
import sys
with open(sys.argv[1], 'rb') as f:
    assert f.readline().strip() == b'P6'
    line = f.readline()
    while line.startswith(b'#'):
        line = f.readline()
    w, h = map(int, line.split())
    f.readline()
    data = f.read(w*h*3)
hit = None
for x in range(760, w - 10):
    for y in range(h - 30, h - 4):
        i = (y*w + x)*3
        r, g, b = data[i], data[i+1], data[i+2]
        if abs(r-0) < 20 and abs(g-0x78) < 20 and abs(b-0xd7) < 20:
            hit = x
            break
    if hit:
        break
print(hit or "")
PY
)
echo "shield x: ${SHIELD_X:-not found}"
if [ -n "${SHIELD_X:-}" ]; then
    xdotool mousemove $((SHIELD_X + 7)) 752
    sleep 0.25
    xdotool click 1
    sleep 0.7
    shot "security_gui.png"
    # protection: click "Always-on" then "Scan Desktop" inside the GUI
    # GUI is centred: x0=(1024-580)/2=222, y0=(768-470)/2-40=109
    xdotool mousemove $((222 + 10 + 46)) $((109 + 54 + 11))
    sleep 0.2
    xdotool click 1
    sleep 0.3
    xdotool mousemove $((222 + 10 + 112 + 52)) $((109 + 160 + 11))
    sleep 0.2
    xdotool click 1
    sleep 0.6
    shot "security_gui_scan.png"
fi
