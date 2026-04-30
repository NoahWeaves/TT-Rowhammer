#!/usr/bin/env python3
"""Render the DRAM geometry status table as a PNG for slides."""

import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

ROWS = [
    ("Row size",                "confirmed", "8192 B (8 KB)"),
    ("Adjacent row offset",     "confirmed", "±0x2000"),
    ("Address mapping",         "confirmed", "Sequential, no XOR"),
    ("Page mode",               "confirmed", "Open-page"),
    ("Row-buffer hit / miss",   "confirmed", "833 / 873 cyc"),
    ("Channels",                "confirmed", "8 GDDR6, identical"),
    ("Rows per bank",           "inferred",  "16 (latency tier)"),
    ("Bank-group penalty",      "inferred",  "+24 cyc (unconfirmed)"),
    ("Refresh interval (tREFI)", "open",     "Open"),
    ("ECC disable path",        "open",      "None found"),
]

STATUS_COLOR = {
    "confirmed": ("#2e7d32", "✓ confirmed"),
    "inferred":  ("#ef6c00", "∼ inferred"),
    "open":      ("#c62828", "✗ open"),
}

fig, ax = plt.subplots(figsize=(11, 5.2))
ax.set_xlim(0, 100); ax.set_ylim(0, len(ROWS) + 2)
ax.axis("off")

ax.text(50, len(ROWS) + 1.3, "Blackhole DRAM Geometry — Validation Status",
        ha="center", va="center", fontsize=18, fontweight="bold")

header_y = len(ROWS) + 0.4
ax.add_patch(Rectangle((1, header_y - 0.4), 98, 0.8, facecolor="#37474f"))
for x, txt in [(3, "Property"), (45, "Status"), (65, "Value")]:
    ax.text(x, header_y, txt, color="white", fontsize=12, fontweight="bold", va="center")

for i, (prop, status, val) in enumerate(ROWS):
    y = len(ROWS) - i - 0.5
    if i % 2 == 0:
        ax.add_patch(Rectangle((1, y - 0.4), 98, 0.8, facecolor="#f5f5f5"))
    color, label = STATUS_COLOR[status]
    ax.text(3, y, prop, fontsize=11, va="center")
    ax.add_patch(Rectangle((44, y - 0.3), 19, 0.6, facecolor=color, alpha=0.85))
    ax.text(53.5, y, label, color="white", fontsize=10, fontweight="bold",
            ha="center", va="center")
    ax.text(65, y, val, fontsize=11, va="center", family="monospace")

plt.tight_layout()
out = "geometry_status_table.png"
plt.savefig(out, dpi=200, bbox_inches="tight", facecolor="white")
print(f"wrote {out}")
