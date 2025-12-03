#!/usr/bin/env python3
"""
Benchmark helper to collect throughput/latency for SDR, SR, EC.
Runs local sender/receiver pairs, optionally configuring tc netem.
Outputs CSV with sender/receiver durations and computed throughput.
"""

import argparse
import csv
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


def run_cmd(cmd, check=True):
    return subprocess.run(cmd, shell=False, check=check, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def apply_netem(iface: str, dport: int, delay_ms: int, jitter_ms: int, loss_pct: float):
    # Clear existing
    run_cmd(["sudo", "tc", "qdisc", "del", "dev", iface, "root"], check=False)
    if loss_pct <= 0 and delay_ms <= 0:
        return
    run_cmd(["sudo", "tc", "qdisc", "add", "dev", iface, "root", "handle", "1:", "prio", "bands", "3"])
    netem_args = ["sudo", "tc", "qdisc", "add", "dev", iface, "parent", "1:3", "handle", "30:", "netem"]
    if delay_ms > 0:
        netem_args += ["delay", f"{delay_ms}ms"]
        if jitter_ms > 0:
            netem_args += [f"{jitter_ms}ms"]
    if loss_pct > 0:
        netem_args += ["loss", f"{loss_pct}%"]
    run_cmd(netem_args)
    run_cmd([
        "sudo", "tc", "filter", "add", "dev", iface, "protocol", "ip", "parent", "1:0", "prio", "3", "u32",
        "match", "ip", "protocol", "17", "0xff",
        "match", "ip", "dport", str(dport), "0xffff",
        "flowid", "1:3"
    ])


def clear_netem(iface: str):
    run_cmd(["sudo", "tc", "qdisc", "del", "dev", iface, "root"], check=False)


def parse_duration(text: str, pattern: str):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else None


def run_pair(mode: str, tcp_port: int, udp_port: int, size: int, config: Path, timeout: int = 120):
    recv_cmd = [
        str(BUILD / "sdr_test_receiver"),
        "--mode", mode,
        str(tcp_port),
        str(udp_port),
        str(size),
        str(config)
    ]
    send_cmd = [
        str(BUILD / "sdr_test_sender"),
        "--mode", mode,
        "127.0.0.1",
        str(tcp_port),
        str(udp_port),
        str(size)
    ]

    recv_proc = subprocess.Popen(recv_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # Wait for receiver to be ready
    start_wait = time.time()
    recv_lines = []
    while True:
        line = recv_proc.stdout.readline()
        if not line:
            break
        recv_lines.append(line)
        if "Waiting for sender connection" in line:
            break
        if time.time() - start_wait > 30:
            recv_proc.kill()
            raise RuntimeError("Receiver did not become ready")

    sender_start = time.time()
    send_out = subprocess.check_output(send_cmd, stderr=subprocess.STDOUT, text=True)
    sender_end = time.time()

    try:
        recv_out, _ = recv_proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        recv_proc.kill()
        recv_out, _ = recv_proc.communicate()

    recv_text = "".join(recv_lines) + (recv_out or "")
    send_text = send_out

    sender_ms = parse_duration(send_text, r"Done in ([0-9]+) ms")
    recv_ms = parse_duration(recv_text, r"Transfer completed in ([0-9]+) ms")
    fallback = "EC_FALLBACK_SR" in recv_text or "EC_FALLBACK_SR" in send_text

    return {
        "mode": mode,
        "bytes": size,
        "sender_ms": sender_ms if sender_ms is not None else int((sender_end - sender_start) * 1000),
        "receiver_ms": recv_ms,
        "fallback": fallback,
        "sender_log": send_text,
        "receiver_log": recv_text,
    }


def main():
    parser = argparse.ArgumentParser(description="Run SDR/SR/EC benchmarks and emit CSV.")
    parser.add_argument("--tcp", type=int, default=8888, help="TCP control port")
    parser.add_argument("--udp", type=int, default=9999, help="UDP data port")
    parser.add_argument("--iface", default="lo", help="Interface for netem")
    parser.add_argument("--loss", type=float, nargs="+", default=[0, 1, 5, 10], help="Loss percentages")
    parser.add_argument("--delay", type=int, default=50, help="Base delay ms for netem")
    parser.add_argument("--jitter", type=int, default=10, help="Jitter ms for netem")
    parser.add_argument("--sizes", type=int, nargs="+", default=[1048576], help="Message sizes (bytes)")
    parser.add_argument("--iters", type=int, default=3, help="Iterations per condition")
    parser.add_argument("--config", default=str((ROOT / "config" / "receiver.config")), help="Receiver config path")
    parser.add_argument("--output", default="results.csv", help="CSV output file")
    parser.add_argument("--no-netem", action="store_true", help="Do not apply tc; assume external setup")
    args = parser.parse_args()

    results = []
    modes = ["sdr", "sr", "ec"]
    config_path = Path(args.config)

    for loss in args.loss:
        # Only apply loss for SR/EC; SDR should stay at 0
        for size in args.sizes:
            for mode in modes:
                if mode == "sdr" and loss != 0:
                    continue
                if not args.no_netem:
                    apply_netem(args.iface, args.udp, args.delay if loss > 0 else 0, args.jitter if loss > 0 else 0, loss)
                for it in range(args.iters):
                    print(f"[RUN] mode={mode} size={size} loss={loss}% iter={it+1}")
                    res = run_pair(mode, args.tcp, args.udp, size, config_path)
                    res.update({
                        "loss_pct": loss,
                        "delay_ms": args.delay if loss > 0 else 0,
                        "jitter_ms": args.jitter if loss > 0 else 0,
                        "iter": it + 1,
                    })
                    duration_ms = res["receiver_ms"] or res["sender_ms"]
                    if duration_ms:
                        res["throughput_mbps"] = (size * 8) / (duration_ms / 1000) / 1e6
                    else:
                        res["throughput_mbps"] = None
                    results.append(res)
                if not args.no_netem:
                    clear_netem(args.iface)

    with open(args.output, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "mode", "bytes", "loss_pct", "delay_ms", "jitter_ms", "iter",
            "sender_ms", "receiver_ms", "throughput_mbps", "fallback"
        ])
        writer.writeheader()
        for r in results:
            writer.writerow({k: r.get(k) for k in writer.fieldnames})

    print(f"Wrote {len(results)} rows to {args.output}")
    print("Note: if throughput_mbps is None, the script could not parse timing from logs; check sender/receiver_log.")


if __name__ == "__main__":
    if not (BUILD / "sdr_test_sender").exists():
        print("Build binaries first (cmake --build . from sdr-udp/build)", file=sys.stderr)
        sys.exit(1)
    main()
