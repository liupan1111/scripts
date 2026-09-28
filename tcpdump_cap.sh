#!/usr/bin/env bash
set -euo pipefail
umask 077

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 SERVER_IP TEST_PORT"
    exit 1
fi

server_ip=$1
test_port=$2
dev=ens3
run_id=$(date '+%Y%m%d-%H%M%S')
capture_dir="/var/log/tcp-capture-client-${run_id}"

if [[ $EUID -ne 0 ]]; then
    echo "Please run as root"
    exit 1
fi

if ! command -v tcpdump >/dev/null 2>&1; then
    echo "tcpdump is not installed"
    exit 1
fi

if [[ ! -e "/sys/class/net/$dev" ]]; then
    echo "Interface $dev does not exist"
    exit 1
fi

mkdir -p "$capture_dir"

{
    echo "role=client"
    echo "interface=$dev"
    echo "server_ip=$server_ip"
    echo "test_port=$test_port"
    echo "start_time=$(date '+%F %T.%N %z')"
} >"$capture_dir/info.txt"

echo "Capturing client traffic"
echo "Interface: $dev"
echo "Server:    $server_ip:$test_port"
echo "Output:    $capture_dir"
echo "Press Ctrl-C after a slow request occurs."

exec tcpdump \
    -Z root \
    -i "$dev" \
    -nn \
    -s 128 \
    -B 8192 \
    -U \
    -C 200 \
    -W 2000 \
    -w "/data/oss0/caps/client.pcap" \
    "host $server_ip and tcp port $test_port"
