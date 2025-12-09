#!/usr/bin/env python3
"""
Plot throughput vs loss% for different message sizes.

Input CSV columns (header row):
    size, loss, SNDR TP (Mbps), SDNR TT (ms), RCVR TP (Mbps), RCVR TT (ms)

Example rows (tab or comma delimited is fine as long as DictReader sees the names):
    1GiB,0%,197.5293,43506.47,174.944,49136.61
    1GiB,0.50%,96.43775714,89097.95714,89.98487143,95465.34286
    ...

Outputs PNGs into ../plots/:
    - sndr_throughput_vs_loss.png
    - rcvr_throughput_vs_loss.png
    - sndr_transfer_time_vs_loss.png
    - rcvr_transfer_time_vs_loss.png
"""

import csv
import os
from typing import Dict, List, Tuple
import math
import matplotlib.pyplot as plt

# Adjust ROOT/DATA_DIR/PLOTS_DIR to match your repo layout
ROOT = os.path.dirname(os.path.dirname(__file__))
DATA_DIR = os.path.join(ROOT, "data")
PLOTS_DIR = os.path.join(ROOT, "plots")
LOSS_CSV = os.path.join(DATA_DIR, "sr_Data.csv")  # <-- change filename if needed

print("Data directory:", DATA_DIR)
print("PLOTS_DIR:", PLOTS_DIR)
print("LOSS_CSV:", LOSS_CSV)

os.makedirs(PLOTS_DIR, exist_ok=True)

# Column names in the CSV
COLS = {
    "size": "size",
    "loss": "loss",
    "sndr_tp": "SNDR TP (Mbps)",
    "sndr_tt": "SDNR TT (ms)",  # Note: typo in CSV header "SDNR" instead of "SNDR"
    "rcvr_tp": "RCVR TP (Mbps)",
    "rcvr_tt": "RCVR TT (ms)",
}


def parse_float(s: str) -> float:
    try:
        return float(s)
    except Exception:
        return float("nan")


def parse_loss_percent(s: str) -> float:
    """
    Convert strings like '0%', '0.50%' -> float percentage value (e.g., 0.0, 0.5).
    """
    if s is None:
        return float("nan")
    s = s.strip()
    if s.endswith("%"):
        s = s[:-1]
    try:
        return float(s)
    except Exception:
        return float("nan")


def read_rows(path: str) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            # skip if no size or loss
            if not r.get(COLS["size"]) or not r.get(COLS["loss"]):
                continue
            rows.append(r)
    return rows


def build_series_by_size(
    rows: List[Dict[str, str]],
    value_col: str,
) -> Dict[str, List[Tuple[float, float]]]:
    """
    For a given value column (throughput or transfer time), group points by message size.

    Returns:
        { size_str: [(loss_percent, value), ...], ... }
    """
    grouped: Dict[str, List[Tuple[float, float]]] = {}
    for r in rows:
        size = r.get(COLS["size"], "").strip()
        loss_val = parse_loss_percent(r.get(COLS["loss"], ""))
        value = parse_float(r.get(value_col, "nan"))

        if math.isnan(loss_val) or math.isnan(value) or not size:
            continue

        grouped.setdefault(size, []).append((loss_val, value))

    # Sort each size's series by loss ascending
    for size, pts in grouped.items():
        pts.sort(key=lambda x: x[0])

    return grouped


def plot_value_vs_loss(
    series: Dict[str, List[Tuple[float, float]]],
    title: str,
    ylabel: str,
    filename: str,
):
    """
    series: {size_str: [(loss%, value), ...]}
    """
    plt.figure(figsize=(9, 5))

    # Desired order of sizes for consistent legend / colors.
    # Adjust if you add/remove sizes.
    size_order = ["1MiB", "10MiB", "100MiB", "500MiB", "1GiB"]

    # Give each size a marker/line style (similar to your other script)
    size_styles = {
        "1MiB": "o-",
        "10MiB": "s-",
        "100MiB": "^-",
        "500MiB": "d-",
        "1GiB": "x-",
    }

    for size in size_order:
        if size not in series:
            continue
        pts = series[size]
        losses = [p[0] for p in pts]
        tps = [p[1] for p in pts]
        style = size_styles.get(size, "o-")
        plt.plot(losses, tps, style, label=size)

    # If there are any sizes not in size_order, also plot them
    for size, pts in series.items():
        if size in size_order:
            continue
        losses = [p[0] for p in pts]
        tps = [p[1] for p in pts]
        plt.plot(losses, tps, "o-", label=size)

    # X-axis ticks: you can adapt this based on your data
    loss_ticks = [0.0, 0.5, 1.0, 5.0, 10.0]
    loss_labels = ["0%", "0.5%", "1%", "5%", "10%"]
    plt.xticks(loss_ticks, loss_labels)

    plt.xlabel("Loss (%)")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, which="both", linestyle=":")
    plt.legend(title="Message size")

    out_path = os.path.join(PLOTS_DIR, filename)
    plt.tight_layout()
    plt.savefig(out_path)
    plt.close()
    print(f"Wrote {out_path}")


def main():
    rows = read_rows(LOSS_CSV)

    print(f"Read {len(rows)} rows from {LOSS_CSV}")

    # Throughput plots
    sndr_tp_series = build_series_by_size(rows, COLS["sndr_tp"])
    rcvr_tp_series = build_series_by_size(rows, COLS["rcvr_tp"])

    # 1) SNDR throughput vs loss
    plot_value_vs_loss(
        sndr_tp_series,
        title="UDP SR - Sender Throughput vs Loss",
        ylabel="Sender Throughput (Mbps)",
        filename="sr_sndr_throughput_vs_loss.png",
    )

    # 2) RCVR throughput vs loss
    plot_value_vs_loss(
        rcvr_tp_series,
        title="UDP SR - Receiver Throughput vs Loss",
        ylabel="Receiver Throughput (Mbps)",
        filename="sr_rcvr_throughput_vs_loss.png",
    )

    # Transfer time plots
    sndr_tt_series = build_series_by_size(rows, COLS["sndr_tt"])
    rcvr_tt_series = build_series_by_size(rows, COLS["rcvr_tt"])

    # 3) SNDR transfer time vs loss
    plot_value_vs_loss(
        sndr_tt_series,
        title="UDP SR - Sender Transfer Time vs Loss",
        ylabel="Sender Transfer Time (ms)",
        filename="sr_sndr_transfer_time_vs_loss.png",
    )

    # 4) RCVR transfer time vs loss
    plot_value_vs_loss(
        rcvr_tt_series,
        title="UDP SR - Receiver Transfer Time vs Loss",
        ylabel="Receiver Transfer Time (ms)",
        filename="sr_rcvr_transfer_time_vs_loss.png",
    )


if __name__ == "__main__":
    main()
