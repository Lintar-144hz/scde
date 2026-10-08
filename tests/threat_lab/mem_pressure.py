#!/usr/bin/env python3
"""
SCDE SECURITY TEST - Malware Simulation (RAM Exhaustion) - TEST HARNESS ONLY.

- Standalone, TIDAK mengubah source code SCDE.
- Hanya membebani proses ini sendiri (single process, single thread).
- TIDAK kill proses lain, TIDAK sentuh filesystem sistem, TIDAK hapus/enkripsi
  file, TIDAK persistence, TIDAK network/exfil, TIDAK priv-esc, TIDAK fork bomb.
- Hard memory ceiling agar berhenti otomatis + safety floor agar host tetap responsif.
- Setelah test berhenti, seluruh memory dilepas (cleanup) + laporan sebelum/sesudah.

Cara pakai:
    python3 tests/threat_lab/mem_pressure.py
    python3 tests/threat_lab/mem_pressure.py --target-mb 256 --chunk-mb 8
    python3 tests/threat_lab/mem_pressure.py --target-mb 128 --interval 0.05 --time-limit 60

Tujuan: menguji apakah SCDE/security layer mampu mendeteksi/menangani proses
dengan extreme memory pressure. Catatan: security layer SCDE saat ini
(src/security/) adalah file scanner (hash/rules/trust/policy), BUKAN runtime
memory monitor, jadi ekspektasi default = TIDAK terdeteksi oleh SCDE.
Bukti deteksi harus diobservasi dari luar (Security Center log / audit / OOM killer).
"""

import argparse
import gc
import os
import sys
import time


def read_meminfo():
    """Return dict kB: MemTotal, MemFree, MemAvailable. Pure read /proc/meminfo."""
    info = {}
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and parts[0].rstrip(":") in (
                    "MemTotal", "MemFree", "MemAvailable", "SwapTotal", "SwapFree"
                ):
                    key = parts[0].rstrip(":")
                    try:
                        info[key] = int(parts[1])  # kB
                    except ValueError:
                        pass
    except OSError as e:
        print(f"[WARN] tidak bisa baca /proc/meminfo: {e}", flush=True)
    return info


def read_self_rss_kb():
    """RSS proses ini sendiri (kB) via /proc/self/status. Tidak menyentuh proses lain."""
    try:
        with open("/proc/self/status", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    # fallback via resource module (ru_maxrss, satuan KB di Linux)
    try:
        import resource
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    except Exception:
        return -1


def fmt_mb(kb):
    if kb < 0:
        return "?"
    return f"{kb / 1024.0:.1f} MB"


def parse_args(argv):
    p = argparse.ArgumentParser(
        description="SCDE malware-simulation harness: gradual RAM pressure (safe, self-only)."
    )
    p.add_argument("--target-mb", type=int, default=256,
                   help="target alokasi sebelum berhenti normal (default 256)")
    p.add_argument("--chunk-mb", type=int, default=8,
                   help="ukuran tiap langkah alokasi (default 8)")
    p.add_argument("--interval", type=float, default=0.05,
                   help="jeda detik antar chunk, biar host tetap responsif (default 0.05)")
    p.add_argument("--time-limit", type=float, default=60.0,
                   help="batas durasi pressure detik (default 60)")
    p.add_argument("--hard-ceiling-mb", type=int, default=512,
                   help="HARD CEILING absolut, tidak bisa dilewati (default 512)")
    p.add_argument("--safety-floor-mb", type=int, default=250,
                   help="berhenti jika MemAvailable di bawah nilai ini (default 250)")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])

    # ---- clamp ke batas aman (tidak bisa di-bypass via arg besar) ----
    ABSOLUTE_MAX_MB = 512
    if args.hard_ceiling_mb > ABSOLUTE_MAX_MB:
        print(f"[SAFETY] --hard-ceiling-mb {args.hard_ceiling_mb} > {ABSOLUTE_MAX_MB}, "
              f"dipaksa ke {ABSOLUTE_MAX_MB}", flush=True)
        args.hard_ceiling_mb = ABSOLUTE_MAX_MB
    if args.target_mb > args.hard_ceiling_mb:
        print(f"[SAFETY] --target-mb {args.target_mb} > ceiling {args.hard_ceiling_mb}, "
              f"dipaksa ke {args.hard_ceiling_mb}", flush=True)
        args.target_mb = args.hard_ceiling_mb
    if args.target_mb < 8:
        args.target_mb = 8
    if args.chunk_mb < 1:
        args.chunk_mb = 1
    if args.chunk_mb > 32:
        print("[SAFETY] chunk dibatasi 32 MB agar bertahap", flush=True)
        args.chunk_mb = 32
    if args.interval < 0.01:
        print("[SAFETY] interval minimal 0.01s agar tidak bikin host hang", flush=True)
        args.interval = 0.01

    target_mb = args.target_mb
    chunk_mb = args.chunk_mb
    chunk_bytes = chunk_mb * 1024 * 1024

    print("=" * 62, flush=True)
    print(" SCDE THREAT LAB - MALWARE SIMULATION: RAM PRESSURE (TEST ONLY)", flush=True)
    print(" Harness standalone. TIDAK mengubah source SCDE.", flush=True)
    print(" Beban HANYA proses ini (pid %d). No fork, no kill, no FS write," % os.getpid(), flush=True)
    print(" no network, no persistence, no priv-esc.", flush=True)
    print("=" * 62, flush=True)
    print(f" RAM target      : {target_mb} MB", flush=True)
    print(f" Hard ceiling    : {args.hard_ceiling_mb} MB (auto-stop)", flush=True)
    print(f" Chunk / interval: {chunk_mb} MB / {args.interval}s", flush=True)
    print(f" Time limit      : {args.time_limit}s | Safety floor (Avail): "
          f"{args.safety_floor_mb} MB", flush=True)
    print(flush=True)

    mem_before = read_meminfo()
    rss_before = read_self_rss_kb()
    print(f"[BEFORE] MemTotal={fmt_mb(mem_before.get('MemTotal', -1))} "
          f"MemFree={fmt_mb(mem_before.get('MemFree', -1))} "
          f"MemAvailable={fmt_mb(mem_before.get('MemAvailable', -1))} "
          f"| self-RSS={fmt_mb(rss_before)}", flush=True)
    if mem_before.get("MemAvailable", 10**12) / 1024.0 < (target_mb + args.safety_floor_mb):
        print(f"[WARN] MemAvailable tipis untuk target {target_mb} MB. "
              f"Test tetap jalan tapi akan auto-stop di safety floor.", flush=True)
    print("[RUN] mulai alokasi bertahap... (Ctrl-C = stop aman + cleanup)", flush=True)
    print("-" * 62, flush=True)

    blocks = []          # referensi agar memory benar-benar resident
    allocated_mb = 0
    peak_rss_kb = rss_before if rss_before > 0 else 0
    peak_alloc_mb = 0
    stop_reason = "UNKNOWN"
    oom_hit = False
    t_start = time.monotonic()

    try:
        while True:
            elapsed = time.monotonic() - t_start

            # --- cek batas SEBELUM alokasi berikutnya ---
            if allocated_mb >= target_mb:
                stop_reason = f"TARGET_REACHED ({target_mb} MB)"
                break
            if allocated_mb >= args.hard_ceiling_mb:
                stop_reason = f"HARD_CEILING ({args.hard_ceiling_mb} MB)"
                break
            if elapsed >= args.time_limit:
                stop_reason = f"TIME_LIMIT ({args.time_limit}s)"
                break

            # --- alokasi 1 chunk + touch tiap page agar benar-benar commit ---
            try:
                buf = bytearray(chunk_bytes)
                # sentuh 1 byte per 4K page -> paksa page-fault -> resident
                for off in range(0, chunk_bytes, 4096):
                    buf[off] = 0x41
                blocks.append(buf)
            except MemoryError:
                oom_hit = True
                stop_reason = "MEMORY_ERROR (alokasi ditolak / tekanan OOM)"
                break

            allocated_mb += chunk_mb
            if allocated_mb > peak_alloc_mb:
                peak_alloc_mb = allocated_mb

            rss = read_self_rss_kb()
            if rss > peak_rss_kb:
                peak_rss_kb = rss
            mem = read_meminfo()
            avail_mb = mem.get("MemAvailable", -1) / 1024.0 if "MemAvailable" in mem else -1.0

            print(f"[+{elapsed:6.1f}s] alloc={allocated_mb:4d} MB "
                  f"self-RSS={fmt_mb(rss):>10s} "
                  f"Avail={fmt_mb(mem.get('MemAvailable', -1)):>10s} "
                  f"Free={fmt_mb(mem.get('MemFree', -1)):>10s}", flush=True)

            # --- cek safety floor SESUDAH alokasi ---
            if "MemAvailable" in mem and mem["MemAvailable"] / 1024.0 < args.safety_floor_mb:
                stop_reason = (f"SAFETY_FLOOR (Avail {mem['MemAvailable']/1024.0:.0f} MB "
                               f"< {args.safety_floor_mb} MB, stop agar host responsif)")
                break

            time.sleep(args.interval)

    except KeyboardInterrupt:
        stop_reason = "INTERRUPTED (Ctrl-C oleh operator)"
    finally:
        t_end = time.monotonic()
        pressure_dur = t_end - t_start
        # peak terakhir sebelum cleanup
        rss_at_peak_phase = read_self_rss_kb()
        if rss_at_peak_phase > peak_rss_kb:
            peak_rss_kb = rss_at_peak_phase

        print("-" * 62, flush=True)
        print(f"[STOP] alasan: {stop_reason}", flush=True)
        print(f"[STOP] OOM/termination terdeteksi saat alokasi: {'YA' if oom_hit else 'TIDAK'} "
              f"(catatan: jika proses di-kill OOM killer, script tidak sampai sini; "
              f"cek dmesg/exit-code 137 dari pembungkus)", flush=True)
        print("[CLEANUP] melepas seluruh memory...", flush=True)
        # Lepas referensi + paksa GC, lalu beri waktu kernel reclaim
        blocks.clear()
        blocks = None  # noqa: F841 (sengaja dilepas)
        gc.collect()
        time.sleep(2.0)
        gc.collect()

        mem_after = read_meminfo()
        rss_after = read_self_rss_kb()

        print(f"[AFTER ] MemFree={fmt_mb(mem_after.get('MemFree', -1))} "
              f"MemAvailable={fmt_mb(mem_after.get('MemAvailable', -1))} "
              f"| self-RSS={fmt_mb(rss_after)}", flush=True)
        stable = True  # sampai di sini = proses selamat, tidak crash/hang
        print(flush=True)
        print("=" * 62, flush=True)
        print(" HASIL AKHIR (untuk laporan SCDE security test)", flush=True)
        print("=" * 62, flush=True)
        print(f" RAM sebelum        : Avail {fmt_mb(mem_before.get('MemAvailable', -1))} / "
              f"Free {fmt_mb(mem_before.get('MemFree', -1))} / "
              f"self-RSS {fmt_mb(rss_before)}", flush=True)
        print(f" RAM target         : {target_mb} MB (ceiling {args.hard_ceiling_mb} MB)", flush=True)
        print(f" Peak alokasi       : {peak_alloc_mb} MB", flush=True)
        print(f" Peak RAM (self-RSS): {fmt_mb(peak_rss_kb)}", flush=True)
        print(f" Durasi pressure    : {pressure_dur:.1f} s", flush=True)
        print(f" Alasan berhenti    : {stop_reason}", flush=True)
        print(f" OOM/termination    : {'YA (MemoryError tertangkap)' if oom_hit else 'TIDAK (proses selamat sampai cleanup)'}", flush=True)
        print(f" RAM setelah cleanup: Avail {fmt_mb(mem_after.get('MemAvailable', -1))} / "
              f"Free {fmt_mb(mem_after.get('MemFree', -1))} / "
              f"self-RSS {fmt_mb(rss_after)}", flush=True)
        print(f" Sistem tetap stabil: {'YA (harness selesai + cleanup OK)' if stable else 'TIDAK'}", flush=True)
        print("=" * 62, flush=True)
        print(" Observasi SCDE: security layer SCDE = file scanner, bukan", flush=True)
        print(" runtime memory monitor. Jadi 'terdeteksi SCDE' = cek manual:", flush=True)
        print(" Security Center log / audit / `dmesg | grep -i oom`.", flush=True)
        print(" Harness ini TIDAK menulis/menghapus file apapun.", flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
