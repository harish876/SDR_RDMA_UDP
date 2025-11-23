#!/usr/bin/env bash
set -euo pipefail

# show mellanox PCI devices
lspci | grep -iE 'mellanox|connectx|mlx' || true

# verbs device details
ibv_devinfo -v || true

# RDMA device - netdev mapping and link state
rdma link show -d || true

# list of all interfaces and state
ip -br link

# Collect the names of interfaces that are currently UP into an array ETHX[]
mapfile -t ETHX < <(ip -br link | awk '$2=="UP"{print $1}')
printf '%s\n' "${ETHX[@]:-<none>}"


if [ "${#ETHX[@]}" -eq 0 ]; then
	echo "No interfaces are UP; skipping per-interface details." >&2
	exit 0
fi

for iface in "${ETHX[@]}"; do
	echo "=== $iface : addresses ==="
	ip addr show dev "$iface" || true

	echo "=== $iface : driver ==="
	ethtool -i "$iface" || true
	
	echo "=== $iface : link speed & capabilities ==="
	ethtool "$iface" | grep -E "Speed|Link|Supported|Advertised|Link detected" || true
	
	echo
done


