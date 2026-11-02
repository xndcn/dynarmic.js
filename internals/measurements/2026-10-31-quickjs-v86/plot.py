#!/usr/bin/env python3
"""Render the archived paired ratios; ranges are observed min/max, not confidence intervals."""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
report = json.loads((root / "comparison.json").read_text())
assert report["verified"] and report["completed"] == 60
fig, ax = plt.subplots(figsize=(9.5, 5.6))
labels = []
for index, (case, record) in enumerate(report["cases"].items()):
    suite, name = case.split("/")
    labels.append(suite if suite == name else suite + " / " + name)
    ratio = record["pairedRatio"]
    ax.scatter(ratio["samples"], [index - .10, index, index + .10], s=23,
               color="#90a4b7", zorder=3, label="Paired rounds" if index == 0 else None)
    ax.errorbar(ratio["median"], index,
                xerr=[[ratio["median"] - ratio["min"]], [ratio["max"] - ratio["median"]]],
                fmt="D", color="#205f97", markersize=5, capsize=3, linewidth=1.5,
                zorder=4, label="Median and observed range" if index == 0 else None)
    ax.text(2.20, index, f'{ratio["median"]:.2f}×', va="center", ha="right", fontsize=10, color="#205f97")
ax.set_yticks(range(len(labels)), labels)
ax.invert_yaxis()
ax.axvline(1, color="#6b7280", linestyle="--", linewidth=1)
ax.set_xlim(.95, 2.25)
ax.set_xticks([1, 1.25, 1.5, 1.75, 2])
ax.set_xlabel("v86 time / dynarmic.js time  (>1 favors dynarmic.js)")
ax.grid(axis="x", color="#e4e8ee", linewidth=.7)
ax.set_axisbelow(True)
for side in ["top", "right", "left"]:
    ax.spines[side].set_visible(False)
ax.tick_params(axis="y", length=0)
fig.legend(*ax.get_legend_handles_labels(), loc="upper left", bbox_to_anchor=(.26, .90),
           ncol=2, frameon=False, fontsize=9)
fig.suptitle("QuickJS: dynarmic.js vs v86", x=.04, ha="left", fontsize=16, weight="bold")
fig.text(.04, .91, "Chromium 133 · Core i7-12700 · QuickJS 2025-09-13 · O2 + switch", fontsize=10, color="#4b5563")
fig.text(.04, .02, "Three paired samples per case. Error bars show the observed range, not a confidence interval.", fontsize=9, color="#4b5563")
fig.subplots_adjust(left=.26, right=.96, top=.84, bottom=.13)
fig.savefig(root / "comparison.svg", metadata={"Date": None})
svg = root / "comparison.svg"
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
fig.savefig(root / "comparison.png", dpi=160)
