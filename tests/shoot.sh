#!/usr/bin/env bash
# Visual check: starts SCDE on Xvfb, opens two clients, saves a PNG.
# usage: tests/shoot.sh [output.png]
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/screenshot.png"}
DPY=":95"
WORK=$(mktemp -d /tmp/scde-shoot.XXXXXX)
HOME_SAVE="$HOME"

cleanup() {
    export HOME="$HOME_SAVE"
    kill "$SCDE_PID" "$CP1" "$CP2" "$XPID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

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
sleep 0.4

"$ROOT/tests/screenshot" "$WORK/shot.ppm"

python3 - "$WORK/shot.ppm" "$OUT" <<'PY'
import struct, sys, zlib
src, dst = sys.argv[1], sys.argv[2]
with open(src, 'rb') as f:
    assert f.readline().strip() == b'P6'
    line = f.readline()
    while line.startswith(b'#'):
        line = f.readline()
    w, h = map(int, line.split())
    maxv = int(f.readline())
    assert maxv == 255
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
