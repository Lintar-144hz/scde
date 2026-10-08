#!/bin/bash

LAB="/tmp/scde-threat-lab"

echo "================================"
echo "      SCDE THREAT LAB v0.1"
echo "      FILESYSTEM BASELINE"
echo "================================"
echo

mkdir -p "$LAB/config"

printf 'PUBLIC DATA\n' > "$LAB/public.txt"
printf 'PROTECTED DATA\n' > "$LAB/private.txt"
printf 'SCDE TEST CONFIG\n' > "$LAB/config/test.conf"

pass=0

if cat "$LAB/public.txt" >/dev/null 2>&1; then
    echo "[PASS] Read lab public file"
    pass=$((pass + 1))
else
    echo "[FAIL] Read lab public file"
fi

if cat "$LAB/private.txt" >/dev/null 2>&1; then
    echo "[INFO] Protected-file baseline: ALLOW"
else
    echo "[INFO] Protected-file baseline: DENY"
fi

TARGET="$LAB/config/../../private.txt"

if [ -f "$TARGET" ]; then
    echo "[INFO] Path traversal baseline: REACHABLE"
else
    echo "[INFO] Path traversal baseline: BLOCKED"
fi

echo
echo "================================"
echo "RESULT"
echo "================================"
echo "Baseline controlled tests: $pass passed"
echo
echo "No SCDE security policy is active yet."
