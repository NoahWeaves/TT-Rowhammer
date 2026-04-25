#!/usr/bin/env python3
"""ECC-instrumented hammer wrapper.

Reads gddr*_corr_errs and gddr_uncorr_errs telemetry counters before and
after running an arbitrary hammer binary, and reports the deltas alongside
the binary's own visible-flip count.

Usage:
    python3 ecc_hammer.py <hammer_binary> [--label TAG] [-- <hammer args>]

The wrapper:
  1. Snapshots ECC counters
  2. Spawns the binary
  3. Snapshots ECC counters again
  4. Prints DELTA lines that downstream tooling can parse:
        ECC_DELTA gddr01_corr_errs <int>
        ECC_DELTA gddr_uncorr_errs <int>
"""
import argparse
import os
import subprocess
import sys

import pyluwen

ECC_FIELDS = [
    "gddr01_corr_errs",
    "gddr23_corr_errs",
    "gddr45_corr_errs",
    "gddr67_corr_errs",
    "gddr_uncorr_errs",
]

# Temperature / power telemetry, useful for next-step C7 (raise temp).
TEMP_FIELDS = [
    "max_gddr_temp",
    "asic_temperature",
    "input_power",
    "aiclk",
]


def snap(chip):
    t = chip.as_bh().get_telemetry()
    out = {f: getattr(t, f) for f in ECC_FIELDS}
    for f in TEMP_FIELDS:
        try:
            out[f] = getattr(t, f)
        except AttributeError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", help="path to hammer binary")
    ap.add_argument("--label", default="", help="annotation tag for the run")
    ap.add_argument("rest", nargs=argparse.REMAINDER,
                    help="args to forward to the binary (use -- to separate)")
    args = ap.parse_args()

    forward = args.rest
    if forward and forward[0] == "--":
        forward = forward[1:]

    chips = pyluwen.detect_chips()
    if not chips:
        print("ERROR: no Tenstorrent chips detected", file=sys.stderr)
        sys.exit(2)
    chip = chips[0]

    before = snap(chip)
    print(f"ECC_PRE  label={args.label or '-'} {before}")

    cmd = [args.binary, *forward]
    print(f"RUN      {' '.join(cmd)}")
    rc = subprocess.run(cmd, env=os.environ).returncode

    after = snap(chip)
    print(f"ECC_POST label={args.label or '-'} {after}")

    delta_total_corr = 0
    for f in ECC_FIELDS:
        d = after[f] - before[f]
        if "corr" in f and "uncorr" not in f:
            delta_total_corr += d
        print(f"ECC_DELTA {f} {d}")
    for f in TEMP_FIELDS:
        if f in before and f in after:
            print(f"TELEMETRY {f} pre={before[f]} post={after[f]}")
    print(f"ECC_SUMMARY label={args.label or '-'} total_corr={delta_total_corr}"
          f" uncorr={after['gddr_uncorr_errs'] - before['gddr_uncorr_errs']}"
          f" rc={rc}")
    sys.exit(rc)


if __name__ == "__main__":
    main()
