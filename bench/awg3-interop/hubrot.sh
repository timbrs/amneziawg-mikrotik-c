#!/bin/bash
# Port rotation against our own proxy in server mode -- a MikroTik hub that
# takes a whole band of ports and dst-nats it onto the one the proxy listens on.
#
#   [wireguard-go] -> [awg-proxy normal: AWG_REMOTE=hub:27015,30000-30049, rotation]
#        -> NAT udp 30000-30049 -> 27015 -> [awg-proxy server] -> [wireguard-go hub]
#
# Each move arrives at the hub from the same client address and port, so its
# conntrack entry would clash with the old one; the kernel remaps the source
# port, and the server-mode proxy has to follow the client to it. While the new
# port has not answered, every packet goes out twice -- the hub sees the client
# at two addresses at once.
#
# Phase A: ping 50/s only, the tunnel is quiet -> moves in the quiet.
# Phase B: ping + iperf3 download, never quiet -> moves under load.
#
# Usage: ./hubrot.sh [up|run|run-a|run-b|report|down|all]   (MIN=6 minutes per phase)
set -u
cd "$(dirname "$0")"

NET=hubrotnet
HUB=hubrot-hub
CLI=hubrot-client
IMG=awg3-interop
NET4=${HUBROT_NET:-172.33}
HUB_IP=$NET4.0.10
CLI_IP=$NET4.0.30
BAND=30000-30049
MIN=${MIN:-6}
BIN=${BIN:-../../../builds/awg-proxy}
SH="$PWD/shared"

dk() { MSYS_NO_PATHCONV=1 docker "$@"; }

down() {
    dk rm -f $HUB $CLI >/dev/null 2>&1
    dk network rm $NET >/dev/null 2>&1
    return 0
}

up() {
    down
    mkdir -p "$SH"; rm -f "$SH"/hr_*
    dk network create --subnet $NET4.0.0/16 $NET >/dev/null || exit 1
    for spec in "$HUB|$HUB_IP" "$CLI|$CLI_IP"; do
        IFS='|' read -r name ip <<<"$spec"
        dk run -d --name "$name" --network $NET --ip "$ip" \
            --cap-add NET_ADMIN --cap-add NET_RAW --device /dev/net/tun \
            -v "$SH:/shared" -v "$PWD/scripts:/scripts" \
            $IMG sleep infinity >/dev/null || exit 1
        dk cp "$BIN" "$name:/usr/local/bin/awg-proxy" || exit 1
    done
    dk exec $HUB /scripts/gen-keys.sh /shared/hr_keys.env >/dev/null || exit 1
    dk exec $HUB awg-proxy --version 2>&1 | head -1

    # The hub: WireGuard, the server-mode proxy in front of it, and the band.
    dk exec -i $HUB bash -s <<EOF || exit 1
set -e
. /scripts/profiles.sh; . /shared/hr_keys.env
load_profile v2 >/dev/null
wireguard-go wg1 >/shared/hr_hubwg.log 2>&1
for _ in \$(seq 1 50); do [ -S /var/run/wireguard/wg1.sock ] && break; sleep 0.2; done
printf 'set=1\nprivate_key=%s\nlisten_port=51830\npublic_key=%s\nallowed_ip=10.55.0.2/32\n\n' \
    "\$SERVER_PRIV_HEX" "\$CLIENT_PUB_HEX" | socat - UNIX-CONNECT:/var/run/wireguard/wg1.sock | grep -q errno=0
ip addr add 10.55.0.1/24 dev wg1; ip link set mtu 1420 up dev wg1
iptables -t nat -A PREROUTING -p udp --dport ${BAND/-/:} -j REDIRECT --to-ports 27015
iperf3 -s -D -B 10.55.0.1
export AWG_MODE=server AWG_LISTEN=:27015 AWG_REMOTE=127.0.0.1:51830 AWG_LOG_LEVEL=debug
export AWG_JC=\$JC AWG_JMIN=\$JMIN AWG_JMAX=\$JMAX AWG_S1=\$S1 AWG_S2=\$S2 AWG_S3=\$S3 AWG_S4=\$S4
export AWG_H1=\$H1 AWG_H2=\$H2 AWG_H3=\$H3 AWG_H4=\$H4 AWG_I1="\$I1"
export AWG_SERVER_PUB=\$SERVER_PUB_B64 AWG_CLIENT_PUB=\$CLIENT_PUB_B64
nohup bash -c 'awg-proxy 2>&1 | while IFS= read -r l; do printf "%s %s\n" "\$(date +%s.%N)" "\$l"; done >>/shared/hr_hub.log' >/dev/null 2>&1 &
sleep 1; pgrep -x awg-proxy >/dev/null
EOF

    # The client: WireGuard pointed at the local proxy, the proxy at the band.
    dk exec -i $CLI bash -s <<EOF || exit 1
set -e
. /scripts/profiles.sh; . /shared/hr_keys.env
load_profile v2 >/dev/null
export AWG_LISTEN=:51820 AWG_REMOTE=$HUB_IP:27015,$BAND AWG_LOG_LEVEL=info
export AWG_PORT_ROTATE=1m-2m AWG_PORT_ROTATE_SOFT=on
export AWG_JC=\$JC AWG_JMIN=\$JMIN AWG_JMAX=\$JMAX AWG_S1=\$S1 AWG_S2=\$S2 AWG_S3=\$S3 AWG_S4=\$S4
export AWG_H1=\$H1 AWG_H2=\$H2 AWG_H3=\$H3 AWG_H4=\$H4 AWG_I1="\$I1"
export AWG_SERVER_PUB=\$SERVER_PUB_B64 AWG_CLIENT_PUB=\$CLIENT_PUB_B64
nohup bash -c 'awg-proxy 2>&1 | while IFS= read -r l; do printf "%s %s\n" "\$(date +%s.%N)" "\$l"; done >>/shared/hr_cli.log' >/dev/null 2>&1 &
sleep 1; pgrep -x awg-proxy >/dev/null
wireguard-go wg0 >/shared/hr_cliwg.log 2>&1
for _ in \$(seq 1 50); do [ -S /var/run/wireguard/wg0.sock ] && break; sleep 0.2; done
printf 'set=1\nprivate_key=%s\nlisten_port=51821\npublic_key=%s\nendpoint=127.0.0.1:51820\nallowed_ip=10.55.0.0/24\npersistent_keepalive_interval=25\n\n' \
    "\$CLIENT_PRIV_HEX" "\$SERVER_PUB_HEX" | socat - UNIX-CONNECT:/var/run/wireguard/wg0.sock | grep -q errno=0
ip addr add 10.55.0.2/24 dev wg0; ip link set mtu 1420 up dev wg0
EOF
    for _ in $(seq 1 20); do
        dk exec $CLI ping -c1 -W1 10.55.0.1 >/dev/null 2>&1 && { echo "tunnel up"; return 0; }
    done
    echo "tunnel never came up"; tail -20 "$SH/hr_cli.log" "$SH/hr_hub.log"; exit 1
}

run_a() {
    dk exec $CLI pkill -INT ping >/dev/null 2>&1
    dk exec -d $CLI bash -c 'ping -D -i 0.02 -W 1 10.55.0.1 >/shared/hr_ping.txt 2>&1'
    date +%s.%N >"$SH/hr_phaseA"
    echo "phase A: ping only, $MIN min"; sleep $((MIN * 60))
}

run_b() {
    date +%s.%N >"$SH/hr_phaseB"
    echo "phase B: ping + iperf3 download 40 Mbit/s, $MIN min"
    dk exec $CLI iperf3 -c 10.55.0.1 -R -b 40M -t $((MIN * 60)) --forceflush -i 30 >"$SH/hr_iperf.txt" 2>&1
    date +%s.%N >"$SH/hr_end"
    dk exec $CLI pkill -INT ping
    sleep 1
}

run() { run_a && run_b; }

# Loss around each move: the ping replies received in [-1 s, +3 s] of the
# "moving to port" line, gaps in their sequence numbers counted as lost.
report() {
    local a b e
    a=$(cat "$SH/hr_phaseA"); b=$(cat "$SH/hr_phaseB"); e=$(cat "$SH/hr_end")
    echo "=== client: moves ==="
    grep -E "rotate: |rotation" "$SH/hr_cli.log" | sed 's/^[0-9.]* //' | sort | uniq -c | sort -rn | head -20
    echo "=== client: lines that should not be there ==="
    grep -Ei "reconnect|fatal|error|warn" "$SH/hr_cli.log" | sed 's/^[0-9.]* //' | sort | uniq -c | head
    echo "=== hub: client moved ==="
    grep -c "client moved" "$SH/hr_hub.log"
    grep -Ei "fatal|error|warn" "$SH/hr_hub.log" | sed 's/^[0-9.]* //' | sort | uniq -c | head
    echo "=== handshakes (client WG) ==="
    grep -c "Received handshake response" "$SH/hr_cliwg.log" 2>/dev/null
    echo "=== iperf3 ==="
    grep -E "receiver|sender" "$SH/hr_iperf.txt" | tail -2
    echo "=== ping ==="
    tail -3 "$SH/hr_ping.txt"
    awk -v A="$a" -v B="$b" -v E="$e" '
        FNR == NR {
            if ($0 ~ /moving to port/) mv[++m] = $1
            next
        }
        match($0, /^\[([0-9.]+)\].*icmp_seq=([0-9]+)/, x) { t[++n] = x[1]; s[n] = x[2] }
        END {
            for (i = 1; i <= m; i++) {
                lo = 0; hi = 0; c = 0
                for (j = 1; j <= n; j++)
                    if (t[j] >= mv[i] - 1 && t[j] <= mv[i] + 3) {
                        if (!c || s[j] < lo) lo = s[j]
                        if (!c || s[j] > hi) hi = s[j]
                        c++
                    }
                miss = c ? hi - lo + 1 - c : -1
                ph = mv[i] < B ? "A quiet" : "B load "
                printf "move %2d  %s  +%6.1f s  replies %3d  missing %d\n", i, ph, mv[i] - A, c, miss
                tot += miss > 0 ? miss : 0
            }
            printf "moves %d, packets missing around moves: %d\n", m, tot
        }' "$SH/hr_cli.log" "$SH/hr_ping.txt"
}

case "${1:-all}" in
    up) up ;;
    run) run ;;
    run-a) run_a ;;
    run-b) run_b ;;
    report) report ;;
    down) down ;;
    all) up && run && report; down ;;
    *) echo "usage: $0 [up|run|report|down|all]"; exit 1 ;;
esac
