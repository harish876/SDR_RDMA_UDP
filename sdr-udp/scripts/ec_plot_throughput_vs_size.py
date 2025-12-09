#!/usr/bin/env python3
"""
Plot throughput vs message size for different loss percentages.

Input CSV columns (header row):
    size, loss, SNDR TP (Mbps), SDNR TT (ms), RCVR TP (Mbps), RCVR TT (ms)

Example rows (tab or comma delimited is fine as long as DictReader sees the names):
    1GiB,0%,197.5293,43506.47,174.944,49136.61
    1GiB,0.50%,96.43775714,89097.95714,89.98487143,95465.34286
    ...

Outputs PNGs into ../plots/:
    - sndr_throughput_vs_size.png
    - rcvr_throughput_vs_size.png
    - sndr_transfer_time_vs_size.png
    - rcvr_transfer_time_vs_size.png
"""

import csv
import os
import re
from typing import Dict, List, Tuple
import math
import matplotlib.pyplot as plt

# Adjust ROOT/DATA_DIR/PLOTS_DIR to match your repo layout
ROOT = os.path.dirname(os.path.dirname(__file__))
DATA_DIR = os.path.join(ROOT, "data")
PLOTS_DIR = os.path.join(ROOT, "plots")
LOSS_CSV = os.path.join(DATA_DIR, "ec_data.csv")

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


def parse_size_to_bytes(size_str: str) -> float:
    """
    Convert size strings like '1MiB', '10MiB', '1GiB' to bytes.
    
    Returns:
        Number of bytes, or NaN if parsing fails.
    """
    if not size_str:
        return float("nan")
    
    size_str = size_str.strip()
    
    # Pattern to match: number followed by unit (KiB, MiB, GiB, etc.)
    pattern = r'^([\d.]+)\s*([KMGT]?i?B)$'
    match = re.match(pattern, size_str, re.IGNORECASE)
    
    if not match:
        return float("nan")
    
    value = float(match.group(1))
    unit = match.group(2).upper()
    
    # Convert to bytes
    # Note: Using binary units (1024-based) since the format uses "iB"
    multipliers = {
        'B': 1,
        'KB': 1000,
        'MB': 1000**2,
        'GB': 1000**3,
        'TB': 1000**4,
        'KIB': 1024,
        'MIB': 1024**2,
        'GIB': 1024**3,
        'TIB': 1024**4,
    }
    
    # Handle both "KB" and "KiB" style
    if unit.endswith('B') and not unit.endswith('IB'):
        # If it's just "KB", "MB", etc., treat as decimal
        unit_key = unit
    elif unit.endswith('IB'):
        # Binary unit
        unit_key = unit
    else:
        return float("nan")
    
    multiplier = multipliers.get(unit_key, 1)
    return value * multiplier


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


def build_series_by_loss(
    rows: List[Dict[str, str]],
    value_col: str,
) -> Tuple[Dict[float, List[Tuple[float, float]]], Dict[float, str]]:
    """
    For a given value column (throughput or transfer time), group points by loss percentage.
    
    Returns:
        (series_dict, size_labels_dict) where:
        - series_dict: { loss_percent: [(size_bytes, value), ...], ... }
        - size_labels_dict: { size_bytes: original_size_string, ... }
    """
    grouped: Dict[float, List[Tuple[float, float]]] = {}
    size_labels: Dict[float, str] = {}  # Map bytes to original size string
    
    for r in rows:
        size_str = r.get(COLS["size"], "").strip()
        loss_val = parse_loss_percent(r.get(COLS["loss"], ""))
        size_bytes = parse_size_to_bytes(size_str)
        value = parse_float(r.get(value_col, "nan"))
        
        if math.isnan(loss_val) or math.isnan(value) or math.isnan(size_bytes):
            continue
        
        # Store the original size string for this byte value
        size_labels[size_bytes] = size_str
        
        grouped.setdefault(loss_val, []).append((size_bytes, value))
    
    # Sort each loss%'s series by size_bytes ascending
    for loss, pts in grouped.items():
        pts.sort(key=lambda x: x[0])
    
    return grouped, size_labels


def format_size_label(bytes_val: float) -> str:
    """
    Format bytes into a human-readable label (MiB, GiB, etc.)
    """
    if bytes_val >= 1024**3:
        return f"{bytes_val / (1024**3):.1f} GiB"
    elif bytes_val >= 1024**2:
        return f"{bytes_val / (1024**2):.1f} MiB"
    elif bytes_val >= 1024:
        return f"{bytes_val / 1024:.1f} KiB"
    else:
        return f"{bytes_val:.0f} B"


def plot_value_vs_size(
    series: Dict[float, List[Tuple[float, float]]],
    size_labels: Dict[float, str],
    title: str,
    ylabel: str,
    filename: str,
):
    """
    series: {loss_percent: [(size_bytes, value), ...]}
    size_labels: {size_bytes: original_size_string, ...}
    """
    plt.figure(figsize=(9, 5))
    
    # Sort loss percentages for consistent ordering
    loss_percentages = sorted(series.keys())
    
    # Give each loss% a marker/line style
    markers = ['o', 's', '^', 'd', 'x', '*', 'v', '<', '>', 'p']
    colors = plt.cm.tab10(range(len(loss_percentages)))
    
    for i, loss_pct in enumerate(loss_percentages):
        pts = series[loss_pct]
        sizes = [p[0] for p in pts]
        tps = [p[1] for p in pts]
        
        marker = markers[i % len(markers)]
        color = colors[i]
        label = f"{loss_pct}%"
        
        plt.plot(sizes, tps, marker=marker, linestyle='-', color=color, label=label, markersize=6)
    
    # Set x-axis to use log scale for better visualization across size ranges
    plt.xscale('log')
    
    # Collect all unique size values from the data
    all_sizes = set()
    for pts in series.values():
        for size_bytes, _ in pts:
            all_sizes.add(size_bytes)
    
    # Sort the sizes and use them as tick positions
    tick_positions = sorted(all_sizes)
    
    # Create labels using the original size strings
    tick_labels = []
    for pos in tick_positions:
        if pos in size_labels:
            # Use the original size string (e.g., "1MiB", "10MiB", "1GiB")
            tick_labels.append(size_labels[pos])
        else:
            # Fallback to formatted label if original not found
            tick_labels.append(format_size_label(pos))
    
    ax = plt.gca()
    ax.set_xticks(tick_positions)
    ax.set_xticklabels(tick_labels, rotation=45, ha='right')
    
    plt.xlabel("Message Size")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, which="both", linestyle=":", alpha=0.5)
    plt.legend(title="Loss %", loc='best')
    
    out_path = os.path.join(PLOTS_DIR, filename)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close()
    print(f"Wrote {out_path}")


def main():
    rows = read_rows(LOSS_CSV)
    
    # Throughput plots
    sndr_tp_series, sndr_tp_size_labels = build_series_by_loss(rows, COLS["sndr_tp"])
    rcvr_tp_series, rcvr_tp_size_labels = build_series_by_loss(rows, COLS["rcvr_tp"])
    
    # 1) SNDR throughput vs size
    plot_value_vs_size(
        sndr_tp_series,
        sndr_tp_size_labels,
        title="UDP EC - Sender Throughput vs Message Size",
        ylabel="Sender Throughput (Mbps)",
        filename="ec_sndr_throughput_vs_size.png",
    )
    
    # 2) RCVR throughput vs size
    plot_value_vs_size(
        rcvr_tp_series,
        rcvr_tp_size_labels,
        title="UDP EC - Receiver Throughput vs Message Size",
        ylabel="Receiver Throughput (Mbps)",
        filename="ec_rcvr_throughput_vs_size.png",
    )

    # Transfer time plots
    sndr_tt_series, sndr_tt_size_labels = build_series_by_loss(rows, COLS["sndr_tt"])
    rcvr_tt_series, rcvr_tt_size_labels = build_series_by_loss(rows, COLS["rcvr_tt"])
    
    # 3) SNDR transfer time vs size
    plot_value_vs_size(
        sndr_tt_series,
        sndr_tt_size_labels,
        title="UDP EC - Sender Transfer Time vs Message Size",
        ylabel="Sender Transfer Time (ms)",
        filename="ec_sndr_transfer_time_vs_size.png",
    )
    
    # 4) RCVR transfer time vs size
    plot_value_vs_size(
        rcvr_tt_series,
        rcvr_tt_size_labels,
        title="UDP EC - Receiver Transfer Time vs Message Size",
        ylabel="Receiver Transfer Time (ms)",
        filename="ec_rcvr_transfer_time_vs_size.png",
    )


if __name__ == "__main__":
    main()

