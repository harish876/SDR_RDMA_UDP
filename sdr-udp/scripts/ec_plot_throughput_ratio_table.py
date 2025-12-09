#!/usr/bin/env python3
"""
Create a table showing the ratio of sender to receiver throughput.

Input CSV columns (header row):
    size, loss, SNDR TP (Mbps), SDNR TT (ms), RCVR TP (Mbps), RCVR TT (ms)

Outputs PNG into ../plots/:
    - throughput_ratio_table.png

The table shows sender/receiver throughput ratios with:
    - Rows: message sizes
    - Columns: loss percentages
"""

import csv
import os
import re
from typing import Dict, List, Set
import math
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# Adjust ROOT/DATA_DIR/PLOTS_DIR to match your repo layout
ROOT = os.path.dirname(os.path.dirname(__file__))
DATA_DIR = os.path.join(ROOT, "data")
PLOTS_DIR = os.path.join(ROOT, "plots")
LOSS_CSV = os.path.join(DATA_DIR, "ec_Data.csv")

print("Data directory:", DATA_DIR)
print("PLOTS_DIR:", PLOTS_DIR)
print("LOSS_CSV:", LOSS_CSV)

os.makedirs(PLOTS_DIR, exist_ok=True)

# Column names in the CSV
COLS = {
    "size": "size",
    "loss": "loss",
    "sndr_tp": "SNDR TP (Mbps)",
    "rcvr_tp": "RCVR TP (Mbps)",
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


def read_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8-sig", errors="replace", newline="") as f:
        reader = csv.DictReader(f)

        # DEBUG: print fieldnames DictReader thinks exist
        print("DICTREADER FIELDNAMES:", reader.fieldnames)

        for r in reader:
            rows.append(r)

    return rows


def build_ratio_table(rows: List[Dict[str, str]]) -> Dict[str, Dict[float, float]]:
    """
    Build a table of throughput ratios (sender/receiver).
    
    Returns:
        { size_str: { loss_percent: ratio, ... }, ... }
    """
    table: Dict[str, Dict[float, float]] = {}
    
    for r in rows:
        size = r.get(COLS["size"], "").strip()
        loss_val = parse_loss_percent(r.get(COLS["loss"], ""))
        sndr_tp = parse_float(r.get(COLS["sndr_tp"], "nan"))
        rcvr_tp = parse_float(r.get(COLS["rcvr_tp"], "nan"))
        
        if not size or math.isnan(loss_val) or math.isnan(sndr_tp) or math.isnan(rcvr_tp):
            continue
        
        if rcvr_tp == 0:
            continue  # Skip division by zero
        
        ratio = sndr_tp / rcvr_tp
        
        if size not in table:
            table[size] = {}
        table[size][loss_val] = ratio
    
    return table


def plot_ratio_table(table: Dict[str, Dict[float, float]]):
    """
    Create a table visualization of throughput ratios.
    """
    # Get all unique sizes and loss percentages
    sizes = sorted(table.keys(), key=lambda x: (
        # Sort by size: convert to bytes for proper ordering
        parse_size_to_bytes(x) if parse_size_to_bytes(x) != float("nan") else float("inf"),
        x
    ))
    
    all_losses: Set[float] = set()
    for size_data in table.values():
        all_losses.update(size_data.keys())
    losses = sorted(all_losses)
    
    # Build the data matrix
    data = []
    row_labels = []
    
    for size in sizes:
        row = []
        row_labels.append(size)
        for loss in losses:
            if loss in table[size]:
                ratio = table[size][loss]
                row.append(ratio)
            else:
                row.append(float("nan"))
        data.append(row)
    
    # Create the figure - adjust for square data cells
    num_cols = len(losses) + 1  # +1 for row labels column
    num_rows = len(sizes) + 1   # +1 for header row
    data_cols = len(losses)
    data_rows = len(sizes)
    
    # Calculate figure size to make data cells square
    # We want the data cells to be square, accounting for row label column
    # Approximate: make figure wider to accommodate row labels while keeping data cells square
    fig_width = 7
    fig_height = 6
    fig, ax = plt.subplots(figsize=(fig_width, fig_height))
    ax.axis('tight')
    ax.axis('off')
    
    # Create column headers with loss percentages
    col_labels = [f"{loss}%" for loss in losses]
    
    # Create the table
    table_obj = ax.table(
        cellText=[[f"{val:.3f}" if not math.isnan(val) else "N/A" for val in row] for row in data],
        rowLabels=row_labels,
        colLabels=col_labels,
        cellLoc='center',
        loc='center',
        bbox=[0, 0, 1, 1]
    )
    
    # Style the table - smaller font
    table_obj.auto_set_font_size(False)
    table_obj.set_fontsize(8)
    
    # Adjust scale to make data cells square
    # The scale function: scale(width, height)
    # Increase height scale significantly to make cells taller and square
    # With 5 data columns and 5 data rows, and accounting for figure aspect ratio
    # We need to make cells taller to compensate for the wider figure
    height_scale = 1.5  # Increase this to make cells taller (more square)
    table_obj.scale(1, height_scale)
    
    # Style header row - white background with black text, half height
    for i in range(len(col_labels)):
        cell = table_obj[(0, i)]
        cell.set_facecolor('white')
        cell.set_text_props(weight='bold', color='black', size=7)
        # Make header row half height by adjusting the cell's height
        # Get the cell's rect and modify its height
        cell_height = cell.get_height()
        cell.set_height(cell_height * 0.5)
    
    # Style row labels - white background with black text
    for i in range(len(row_labels)):
        cell = table_obj[(i + 1, -1)]
        cell.set_facecolor('white')
        cell.set_text_props(weight='bold', color='black')
    
    # Find min and max ratio values for color scaling
    min_ratio = float('inf')
    max_ratio = float('-inf')
    for row in data:
        for val in row:
            if not math.isnan(val):
                min_ratio = min(min_ratio, val)
                max_ratio = max(max_ratio, val)
    
    # Color code cells based on ratio values - blue to dark red gradient
    for i, row in enumerate(data):
        for j, val in enumerate(row):
            if not math.isnan(val):
                cell = table_obj[(i + 1, j)]
                
                # Normalize value to 0-1 range (0 = min_ratio, 1 = max_ratio)
                if max_ratio > min_ratio:
                    normalized = (val - min_ratio) / (max_ratio - min_ratio)
                else:
                    normalized = 0.5
                
                # Dark blue (0, 0, 0.5) at normalized=0 to dark red (0.6, 0, 0) at normalized=1
                # Transition through purple in the middle
                if normalized < 0.5:
                    # Dark blue to purple
                    red = normalized * 0.4  # 0 -> 0.2
                    green = 0.0
                    blue = 0.5 - normalized * 0.2  # 0.5 -> 0.4
                else:
                    # Purple to dark red
                    red = 0.2 + (normalized - 0.5) * 0.8  # 0.2 -> 0.6
                    green = 0.0
                    blue = 0.4 - (normalized - 0.5) * 0.8  # 0.4 -> 0
                
                cell.set_facecolor((red, green, blue))
                # All text is white
                cell.set_text_props(color='white')
    
    plt.title("Sender/Receiver Throughput Ratio", 
              fontsize=14, fontweight='bold', pad=20)
    
    out_path = os.path.join(PLOTS_DIR, "ec_throughput_ratio_table.png")
    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Wrote {out_path}")


def parse_size_to_bytes(size_str: str) -> float:
    """
    Convert size strings like '1MiB', '10MiB', '1GiB' to bytes.
    Returns NaN if parsing fails.
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
    
    # Convert to bytes using binary units (1024-based)
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
    
    if unit.endswith('B') and not unit.endswith('IB'):
        unit_key = unit
    elif unit.endswith('IB'):
        unit_key = unit
    else:
        return float("nan")
    
    multiplier = multipliers.get(unit_key, 1)
    return value * multiplier


def main():
    print("LOSS_CSV:", LOSS_CSV)

    # DEBUG: Print raw header line
    with open(LOSS_CSV, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline()
        print("RAW HEADER:", repr(header))

    rows = read_rows(LOSS_CSV)
    print("ROWS READ:", len(rows))
    
    table = build_ratio_table(rows)
    plot_ratio_table(table)


if __name__ == "__main__":
    main()

