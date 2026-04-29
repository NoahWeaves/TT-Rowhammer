#!/usr/bin/env python3
"""
Wrap a hammer command and report actual NOC reads delivered to each DRAM
endpoint via Blackhole's NIU_SLV_RD_REQ_RECEIVED counter.

Compares the controller-side observed read count to whatever the wrapped
command claims it issued. If they match, our kernel "real activations"
counts are accurate at the NOC layer; if the controller reports fewer,
the NOC fabric or upstream is dropping/coalescing.

NOTE: this measures NOC reads delivered to the DRAM tile's NOC port, NOT
DRAM ACT commands at the GDDR6 interface. The controller may still satisfy
some of these from row buffer without issuing a real ACT — that's a
deeper level we cannot reach from here. But it bounds the upper limit of
real ACTs and verifies the NOC is doing what we think.

Usage:  sudo python3 measure_dram_reads.py -- <hammer_cmd> [args...]
"""
import sys, time, subprocess, argparse
import pyluwen

# NIU register offsets (from blackhole noc_parameters.h)
# NOC_REGS_START_ADDR = 0xFFB20000; NOC_STATUS(cnt) = base + 0x200 + cnt*4
NOC_REGS_BASE = 0xFFB20000
NIU_SLV_RD_REQ_RECEIVED       = NOC_REGS_BASE + 0x200 + 0x35 * 4   # 0xFFB202D4
NIU_SLV_NONPOSTED_WR_REQ_RECEIVED = NOC_REGS_BASE + 0x200 + 0x3A * 4
NIU_SLV_RD_RESP_SENT          = NOC_REGS_BASE + 0x200 + 0x32 * 4

# 8 GDDR6 channels × 3 sub-ports each (from blackhole_140_arch.yaml)
CHANNELS = [
    ("ch0", [(0,0), (0,1),  (0,11)]),
    ("ch1", [(0,2), (0,10), (0,3)]),
    ("ch2", [(0,9), (0,4),  (0,8)]),
    ("ch3", [(0,5), (0,7),  (0,6)]),
    ("ch4", [(9,0), (9,1),  (9,11)]),
    ("ch5", [(9,2), (9,10), (9,3)]),
    ("ch6", [(9,9), (9,4),  (9,8)]),
    ("ch7", [(9,5), (9,7),  (9,6)]),
]

def snapshot(chip):
    """Read NIU_SLV_RD_REQ_RECEIVED and NIU_SLV_RD_RESP_SENT from every DRAM endpoint."""
    snap = {}
    for ch_name, sub_ports in CHANNELS:
        for sp_idx, (x, y) in enumerate(sub_ports):
            rx = chip.noc_read32(0, x, y, NIU_SLV_RD_REQ_RECEIVED)
            tx = chip.noc_read32(0, x, y, NIU_SLV_RD_RESP_SENT)
            wr = chip.noc_read32(0, x, y, NIU_SLV_NONPOSTED_WR_REQ_RECEIVED)
            snap[(ch_name, sp_idx, x, y)] = (rx, tx, wr)
    return snap

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pci", type=int, default=0,
                    help="PCI interface id (default 0)")
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="Hammer command to run between snapshots")
    args = ap.parse_args()

    if not args.cmd:
        ap.error("No command supplied; pass after --")
    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd

    # Pre-snapshot
    chip = pyluwen.PciChip(pci_interface=args.pci).as_bh()
    pre = snapshot(chip)
    del chip   # release device so subprocess can open it
    t0 = time.time()

    # Run the wrapped command, capturing stdout
    print(f"[wrap] running: {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, file=sys.stderr, end="")

    t_elapsed = time.time() - t0

    # Post-snapshot
    chip = pyluwen.PciChip(pci_interface=args.pci).as_bh()
    post = snapshot(chip)

    # Report
    print("\n" + "=" * 78)
    print("NOC SLAVE COUNTERS (deliveries observed at each DRAM tile)")
    print("=" * 78)
    print(f"Wall time including device re-open: {t_elapsed*1000:.0f} ms")
    print()
    hdr = f"{'channel':<8}{'sp':>3}  {'NOC':>10}  {'rx_reads':>12}  {'tx_resps':>12}  {'wr_reqs':>10}  {'rate (M/s)':>11}"
    print(hdr)
    print("-" * len(hdr))
    total_rx = 0
    per_ch = {}
    for key, (rx_pre, tx_pre, wr_pre) in pre.items():
        rx_post, tx_post, wr_post = post[key]
        d_rx = (rx_post - rx_pre) & 0xFFFFFFFF
        d_tx = (tx_post - tx_pre) & 0xFFFFFFFF
        d_wr = (wr_post - wr_pre) & 0xFFFFFFFF
        rate = d_rx / t_elapsed / 1e6 if t_elapsed > 0 else 0
        ch_name, sp_idx, x, y = key
        print(f"{ch_name:<8}{sp_idx:>3}  ({x:>2},{y:>2})  {d_rx:>12}  {d_tx:>12}  {d_wr:>10}  {rate:>11.2f}")
        total_rx += d_rx
        per_ch.setdefault(ch_name, 0)
        per_ch[ch_name] += d_rx
    print("-" * len(hdr))
    print(f"{'TOTAL':<8}{'':>3}  {'':>10}  {total_rx:>12}  {'':>12}  {'':>10}  {total_rx/t_elapsed/1e6:>11.2f}")
    print()
    print("Per-channel summary:")
    for ch_name, total in per_ch.items():
        if total > 0:
            print(f"  {ch_name}:  {total:>12} reads  ({total/t_elapsed/1e6:.2f} M/s)")
    print()
    print("Compare to the wrapped command's reported 'M act/s' / 'real activations'")
    print("If totals MATCH the kernel's claimed activations -> NOC counts are honest.")
    print("If NOC count > kernel claim -> kernel undercounts (e.g. flush reads not counted).")
    print("If NOC count < kernel claim -> NOC fabric dropping or coalescing somewhere.")
    return proc.returncode

if __name__ == "__main__":
    sys.exit(main())
