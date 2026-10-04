#!/bin/bash
# AmneziaWG end-to-end interop harness.
#
#   [wireguard-go, plain WG] --> [awg-proxy] --> [amneziawg-go v3.1.20260814]
#        10.55.0.2               transform         10.55.0.1
#
# The client speaks stock WireGuard and the server is the unmodified reference
# implementation, so everything AWG-specific under test lives in our proxy.
#
# Usage: ./run.sh [build|up|test|down|all]
set -u
cd "$(dirname "$0")"

NET=awg3net
SRV=awg3-server
PRX=awg3-proxy
CLI=awg3-client
IMG=awg3-interop
SHARED="$PWD/shared"

# 172.30.0.0/16 by default; AWG3_NET=172.40 when another docker network on the
# machine already holds that range (docker refuses overlapping pools).
NET4=${AWG3_NET:-172.30}
SRV_IP=$NET4.0.10
PRX_IP=$NET4.0.20
CLI_IP=$NET4.0.30
SRV_IP6=fd00:30::10
PRX_IP6=fd00:30::20
CLI_IP6=fd00:30::30
SRV_PORT=51820  # must match scripts/profile.env
SRV_IMPL=       # "" = reference v3.1, "legacy" = real pre-3.0 server (v0.2.19)
FAMILY=v4       # v4 | v6 | dual -- how the proxy addresses the server
HE_DELAY=       # AWG_HE_DELAY override, only meaningful with FAMILY=dual
WG_MTU_OVERRIDE=  # forces the client WG MTU instead of deriving it from S4
HAS_IPV6=1      # cleared by up() when the docker daemon refuses an IPv6 network

dk() { MSYS_NO_PATHCONV=1 docker "$@"; }

pass=0; fail=0
ok()  { echo "  [PASS] $*"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $*"; fail=$((fail+1)); }

build() {
    echo "=== building image ==="
    dk build -t $IMG . || exit 1
}

up() {
    echo "=== bringing up topology ==="
    down >/dev/null 2>&1
    mkdir -p "$SHARED"; rm -f "$SHARED"/*.log "$SHARED"/*.pcap
    # A dual-stack network is what makes the IPv6 leg and the Happy Eyeballs
    # probe testable at all; fall back to IPv4-only rather than failing outright
    # on daemons where IPv6 is not enabled.
    if dk network create --ipv6 --subnet $NET4.0.0/16 --subnet fd00:30::/64 \
            $NET >/dev/null 2>&1; then
        HAS_IPV6=1
    else
        HAS_IPV6=0
        dk network create --subnet $NET4.0.0/16 $NET >/dev/null 2>&1
        echo "note: docker refused an IPv6 network - IPv6 scenarios will be skipped"
    fi

    for spec in "$SRV|$SRV_IP|$SRV_IP6" "$PRX|$PRX_IP|$PRX_IP6" "$CLI|$CLI_IP|$CLI_IP6"; do
        IFS='|' read -r name ip ip6 <<<"$spec"
        v6opt=(); [ "$HAS_IPV6" = 1 ] && v6opt=(--ip6 "$ip6")
        dk run -d --name "$name" --network $NET --ip "$ip" "${v6opt[@]}" \
            --cap-add NET_ADMIN --cap-add NET_RAW --device /dev/net/tun \
            -v "$SHARED:/shared" -v "$PWD/scripts:/scripts" \
            $IMG sleep infinity >/dev/null || exit 1
    done

    dk exec $SRV /scripts/gen-keys.sh /shared/keys.env >/dev/null || exit 1
    echo "keys generated"
}

stop_all() {
    dk exec $CLI pkill -f wireguard-go  2>/dev/null
    dk exec $SRV pkill -f amneziawg-go  2>/dev/null
    dk exec $PRX pkill -f awg-proxy     2>/dev/null
    sleep 1
}

hs_stamp() { dk exec $CLI wg show wg0 latest-handshakes 2>/dev/null | awk '{print $2}'; }

# $1 = timeout seconds, $2 = stamp that must be exceeded (default 0).
# Comparing against a previous stamp is what makes the reconnect test real:
# the stale handshake keeps being reported after the server restarts.
handshake_ok() {
    local t after=${2:-0}
    for _ in $(seq 1 "${1:-20}"); do
        t=$(hs_stamp)
        [ -n "$t" ] && [ "$t" -gt "$after" ] 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

# Largest 16-aligned WireGuard MTU that still fits a 1500-byte underlay:
#   IP hdr + 8 (UDP) + S4 + 16 (WG hdr) + round_up(mtu,16) + 16 (tag) <= 1500
# The IPv6 header costs 20 bytes more, which is the whole reason this exists.
# Capped at the stock 1420 so the IPv4 matrix keeps running exactly as before;
# "dual" may land on either family, so it takes the pessimistic IPv6 figure.
wg_mtu_for() {
    local family=$1 s4=$2 room mtu
    [ "$family" = v4 ] && room=$((1440 - s4)) || room=$((1420 - s4))
    mtu=$(( room / 16 * 16 ))
    [ "$mtu" -gt 1420 ] && mtu=1420
    echo "$mtu"
}

# S4 of a profile, read from the same file both ends use.
profile_s4() {
    ( . scripts/profiles.sh; load_profile "$1" >/dev/null && echo "$S4" )
}

# bring_up <server-profile> <proxy-profile> [srv_hpk] [prx_hpk] [fb_profile]
bring_up() {
    local sp=$1 pp=$2 shpk=${3:-} phpk=${4:-} fb=${5:-}
    stop_all
    rm -f "$SHARED"/server.log "$SHARED"/proxy.log "$SHARED"/client.log
    dk exec -e PROFILE="$sp" ${shpk:+-e SRV_HPK=$shpk} \
        ${SRV_IMPL:+-e SRV_IMPL=$SRV_IMPL} $SRV /scripts/server.sh \
        | sed 's/^/      /' || return 1
    dk exec -e PROFILE="$pp" ${phpk:+-e PRX_HPK=$phpk} ${fb:+-e FB_PROFILE=$fb} \
        -e FAMILY="$FAMILY" ${HE_DELAY:+-e HE_DELAY=$HE_DELAY} \
        $PRX /scripts/proxy.sh | sed 's/^/      /' || return 1
    local mtu=${WG_MTU_OVERRIDE:-}
    [ -n "$mtu" ] || mtu=$(wg_mtu_for "$FAMILY" "$(profile_s4 "$pp")")
    dk exec -e WG_MTU="$mtu" $CLI /scripts/client.sh >/dev/null || return 1
}

# Number of IPv6 fragments the proxy emitted towards the server while a
# full-MTU ping ran. Next Header 44 is the fragment header; the sending host
# is the only thing that can produce one, since IPv6 routers never fragment.
frag_count_for_mtu() {
    local ver=$1 mtu=$2 pcap=/shared/frag.pcap
    WG_MTU_OVERRIDE=$mtu
    bring_up "$ver" "$ver" >/dev/null 2>&1 || { WG_MTU_OVERRIDE=; echo ERR; return; }
    handshake_ok 25 >/dev/null || { WG_MTU_OVERRIDE=; echo ERR; return; }
    WG_MTU_OVERRIDE=

    dk exec $PRX rm -f $pcap 2>/dev/null
    dk exec -d $PRX tcpdump -i eth0 -s 96 -w $pcap "ip6 and dst host $SRV_IP6" >/dev/null 2>&1
    sleep 1
    # -M do keeps the kernel from splitting the inner packet, so the tunnel has
    # to carry it whole or fail outright: payload = mtu - 20 (IP) - 8 (ICMP).
    dk exec $CLI ping -M do -s $((mtu - 28)) -c 10 -i 0.2 -W 2 10.55.0.1 >/dev/null 2>&1 \
        || { dk exec $PRX pkill -f tcpdump 2>/dev/null; echo PINGFAIL; return; }
    sleep 1
    dk exec $PRX pkill -f tcpdump 2>/dev/null
    sleep 1
    dk exec $PRX sh -c "tcpdump -r $pcap -nn 'ip6 proto 44' 2>/dev/null | wc -l" | tr -d '\r'
}

# --- scenario: the IPv6 MTU arithmetic actually prevents fragmentation ------
frag_case() {
    local ver=$1 s4 good bad_mtu n_good n_bad
    s4=$(profile_s4 "$ver")
    good=$(wg_mtu_for v6 "$s4")
    bad_mtu=1420      # what an IPv4 config would have used
    echo
    echo "=== fragmentation over IPv6 ($ver, S4=$s4): mtu $good vs the IPv4 default $bad_mtu ==="
    FAMILY=v6

    n_good=$(frag_count_for_mtu "$ver" "$good")
    if [ "$n_good" = "0" ]; then
        ok "frag: full-size packets at mtu=$good produce no IPv6 fragments"
    else
        bad "frag: mtu=$good still fragments (result: $n_good)"
    fi

    # Control: without the correction the very same traffic must fragment,
    # otherwise the check above proves nothing.
    n_bad=$(frag_count_for_mtu "$ver" "$bad_mtu")
    if [ "$n_bad" = "ERR" ]; then
        bad "frag: control run failed to start"
    elif [ "$n_bad" = "PINGFAIL" ] || { [ "$n_bad" -gt 0 ] 2>/dev/null; }; then
        ok "frag: control - the IPv4 default mtu=$bad_mtu does fragment (result: $n_bad)"
    else
        bad "frag: control did not fragment at mtu=$bad_mtu, the test is not proving anything"
    fi
    FAMILY=v4
}

# Version the proxy reports for its primary profile, straight from its log.
proxy_proto() { dk exec $PRX sh -c "grep -o 'proto=v[0-9.]*' /shared/proxy.log | head -1"; }

# --- scenario: one protocol version, both ends configured identically --------
version_case() {
    local ver=$1 want_proto=$2
    local lbl=$ver; [ "$FAMILY" != v4 ] && lbl="$ver/$FAMILY"
    echo
    echo "=== version $ver (server=${SRV_IMPL:-current}, proxy on $ver, remote=$FAMILY) ==="
    if ! bring_up "$ver" "$ver"; then bad "$lbl: stack failed to start"; return; fi

    local got; got=$(proxy_proto)
    [ "$got" = "proto=$want_proto" ] \
        && ok "$lbl: proxy detected $got" \
        || bad "$lbl: proxy detected '$got', expected 'proto=$want_proto'"

    if handshake_ok 25; then
        ok "$lbl: handshake completed"
    else
        bad "$lbl: no handshake within 25s"
        dk exec $PRX tail -15 /shared/proxy.log | sed 's/^/      /'
        dk exec $SRV tail -8 /shared/server.log | sed 's/^/      /'
        return
    fi

    # Settle first. With AWG_SRC_PORT=auto the proxy rebinds its outbound
    # socket to the client's port on the first transport packet, so a ping
    # started right after the handshake times that one-off reconnect instead of
    # the steady state.
    dk exec $CLI ping -c 3 -W 3 10.55.0.1 >/dev/null 2>&1
    if dk exec $CLI ping -c 15 -i 0.3 -W 2 10.55.0.1 > /tmp/ping.$$ 2>&1; then
        grep -q " 0% packet loss" /tmp/ping.$$ && ok "$lbl: ping 15/15, no loss" \
                                               || bad "$lbl: ping packet loss"
    else
        bad "$lbl: ping failed"
        tail -3 /tmp/ping.$$ | sed 's/^/      /'
    fi
    rm -f /tmp/ping.$$

    # Bulk data exercises the transport fast path, where the handshake path is
    # not involved at all -- a handshake alone would not prove transport works.
    dk exec -d $SRV iperf3 -s -B 10.55.0.1 >/dev/null 2>&1
    sleep 1
    if dk exec $CLI iperf3 -c 10.55.0.1 -t 4 -O 1 --connect-timeout 5000 \
         > /tmp/ipf.$$ 2>&1; then
        local rate
        rate=$(awk '/receiver/ {print $(NF-2), $(NF-1)}' /tmp/ipf.$$)
        ok "$lbl: bulk transfer ok (${rate:-?})"
    else
        bad "$lbl: iperf3 failed"
        tail -4 /tmp/ipf.$$ | sed 's/^/      /'
    fi
    dk exec $SRV pkill -f "iperf3 -s" 2>/dev/null
    rm -f /tmp/ipf.$$
}

# --- scenario: the two ends disagree about header protection ----------------
negative_case() {
    local label=$1 sp=$2 pp=$3 shpk=$4 phpk=$5
    echo
    echo "=== negative control: $label ==="
    if ! bring_up "$sp" "$pp" "$shpk" "$phpk"; then
        bad "$label: stack failed to start"; return
    fi
    if handshake_ok 15; then
        bad "$label: handshake SUCCEEDED - the test is not exercising header protection"
    else
        ok "$label: handshake correctly fails"
    fi
}

# --- scenario: one end sends random trailers, the other does not ------------
trailer_mismatch_case() {
    local label=$1 sp=$2 pp=$3
    echo
    echo "=== negative control: $label ==="
    if ! bring_up "$sp" "$pp"; then
        bad "$label: stack failed to start"; return
    fi
    if handshake_ok 15; then
        bad "$label: handshake SUCCEEDED - random trailers are not on the wire"
    else
        ok "$label: handshake correctly fails"
    fi
}

# --- scenario: Happy Eyeballs picks IPv6 when IPv4 is a black hole ----------
#
# The server name resolves to both an A and an AAAA (docker embedded DNS), and
# the proxy's IPv4 route to it is dropped on egress -- silently, so the probe
# has to reach its AWG_HE_DELAY timeout rather than an ICMP error. A tunnel
# that comes up therefore proves the IPv6 replay worked.
he_case() {
    local ver=$1
    echo
    echo "=== happy eyeballs: dual-stack name, IPv4 black-holed, $ver ==="
    dk exec $PRX iptables -F OUTPUT 2>/dev/null
    if ! dk exec $PRX iptables -A OUTPUT -d $SRV_IP -p udp --dport $SRV_PORT -j DROP; then
        echo "      (iptables unavailable, skipped)"
        return
    fi

    FAMILY=dual HE_DELAY=250
    local rc=0
    bring_up "$ver" "$ver" || rc=1
    if [ $rc -ne 0 ]; then
        bad "happy-eyeballs: stack failed to start"
    elif handshake_ok 25; then
        ok "happy-eyeballs: tunnel came up over IPv6 despite the IPv4 black hole"
        dk exec $PRX grep -m1 "happy eyeballs" /shared/proxy.log | sed 's/^/      /'
        dk exec $CLI ping -c 5 -W 3 10.55.0.1 >/dev/null 2>&1 \
            && ok "happy-eyeballs: traffic flows on the chosen family" \
            || bad "happy-eyeballs: no traffic after the switch"
    else
        bad "happy-eyeballs: no handshake within 25s"
        dk exec $PRX tail -20 /shared/proxy.log | sed 's/^/      /'
    fi
    FAMILY=v4; HE_DELAY=
    dk exec $PRX iptables -F OUTPUT 2>/dev/null
}

# --- scenario: wire-level proof that the header really is encrypted ---------
wire_case() {
    local ver=$1 expect=$2 s1=$3 initlen=$4
    echo
    echo "=== wire check: $ver handshake init, type field at offset S1=$s1 ==="
    local pcap=/shared/wire-$ver.pcap
    stop_all
    dk exec $SRV rm -f "$pcap" 2>/dev/null
    dk exec -e PROFILE="$ver" $SRV /scripts/server.sh >/dev/null || { bad "$ver: server"; return; }
    dk exec -d $PRX tcpdump -i eth0 -s 0 -w "$pcap" \
        "udp and dst host $SRV_IP and dst port 51820" >/dev/null 2>&1
    sleep 1
    dk exec -e PROFILE="$ver" $PRX /scripts/proxy.sh >/dev/null || { bad "$ver: proxy"; return; }
    dk exec $CLI /scripts/client.sh >/dev/null || { bad "$ver: client"; return; }
    handshake_ok 20 >/dev/null
    sleep 1
    dk exec $PRX pkill -f tcpdump 2>/dev/null
    sleep 1

    local val
    val=$(dk exec $PRX /scripts/wirecheck.sh "$pcap" "$s1" "$initlen" | tr -d '\r')
    if [ "$val" = "NOPKT" ] || [ -z "$val" ]; then
        bad "$ver: no ${initlen}-byte init captured"
        return
    fi
    # H1 is 1000000 or 1000000-1000050 in every profile.
    if [ "$val" -ge 1000000 ] && [ "$val" -le 1000050 ]; then
        local seen=plaintext
    else
        local seen=encrypted
    fi
    [ "$seen" = "$expect" ] \
        && ok "$ver: type field on the wire is $seen (value $val), as expected" \
        || bad "$ver: type field is $seen (value $val), expected $expect"
}

test_all() {
    pass=0; fail=0

    # Backward compatibility: every version the proxy claims to support, spoken
    # to the reference server.
    version_case v1   v1
    version_case v1.5 v1.5
    version_case v2   v2
    version_case v3   v3
    version_case v3.1 v3.1
    # WARP-style: AmneziaWG defaults for S/H, junk + CPS only.
    version_case warp v1.5

    # Same three pre-3.0 generations against the real old server (v0.2.19),
    # which has no header-protection code at all — so this checks a separate
    # implementation, not a reconfigured 3.0.3. Set and cleared explicitly: in
    # bash a `VAR=x func` prefix leaks into the rest of the shell.
    SRV_IMPL=legacy
    version_case v1   v1
    version_case v1.5 v1.5
    version_case v2   v2
    SRV_IMPL=

    # Same matrix over the IPv6 leg. The obfuscation is family-agnostic by
    # construction, so what this really pins is that nothing in the addressing,
    # the socket setup or the MTU arithmetic breaks when the server is v6-only.
    if [ "$HAS_IPV6" = 1 ]; then
        FAMILY=v6
        version_case v1   v1
        version_case v1.5 v1.5
        version_case v2   v2
        version_case v3   v3
        FAMILY=v4
        frag_case v3
        he_case v3
    else
        echo
        echo "=== IPv6 scenarios skipped: no IPv6 on the docker network ==="
    fi

    # v2 and v3 differ only by the key, so these isolate header protection.
    negative_case "server v3 + proxy without key" v3 v3 1 0
    negative_case "server without key + proxy v3" v3 v3 0 1

    # v3.1 vs v3 differ only by the trailers, so a mismatch must break the
    # handshake — otherwise the feature is not on the wire at all.
    trailer_mismatch_case "server v3.1 + proxy v3" v3.1 v3
    trailer_mismatch_case "server v3 + proxy v3.1" v3 v3.1

    wire_case v2 plaintext 77 225
    wire_case v3 encrypted 77 225

    echo
    echo "=== fallback chain: server on v2, proxy primary v3 + AWG_FB_* v2 ==="
    stop_all
    rm -f "$SHARED"/proxy.log
    if dk exec -e PROFILE=v2 $SRV /scripts/server.sh >/dev/null \
       && dk exec -e PROFILE=v3 -e FB_PROFILE=v2 -e FB_AFTER=10 $PRX /scripts/proxy.sh \
            | sed 's/^/      /' \
       && dk exec $CLI /scripts/client.sh >/dev/null; then
        if handshake_ok 60; then
            ok "fallback: tunnel came up on the v2 stage against a v2 server"
            dk exec $PRX grep -c "switch" /shared/proxy.log >/dev/null 2>&1 \
                && dk exec $PRX grep -m2 -i "switch\|profile" /shared/proxy.log \
                     | sed 's/^/      /'
        else
            bad "fallback: no handshake within 60s"
            dk exec $PRX tail -20 /shared/proxy.log | sed 's/^/      /'
        fi
    else
        bad "fallback: stack failed to start"
    fi

    echo
    echo "=== reconnect on v3: restart the server mid-session ==="
    if ! bring_up v3 v3; then
        bad "reconnect: stack failed to start"
    elif ! handshake_ok 25; then
        bad "reconnect: no initial handshake"
    else
        local before; before=$(hs_stamp)
        dk exec $SRV pkill -f amneziawg-go; sleep 2
        dk exec -e PROFILE=v3 $SRV /scripts/server.sh >/dev/null
        # keep traffic flowing so the client re-initiates instead of idling
        dk exec -d $CLI ping -c 90 -i 1 -W 1 10.55.0.1 >/dev/null 2>&1
        if handshake_ok 90 "$before" && dk exec $CLI ping -c 3 -W 3 10.55.0.1 >/dev/null 2>&1; then
            ok "reconnect: fresh handshake and traffic after server restart"
        else
            bad "reconnect: tunnel did not recover"
            dk exec $PRX tail -15 /shared/proxy.log | sed 's/^/      /'
        fi
    fi

    echo
    echo "=================================================="
    echo "  summary: $pass passed, $fail failed"
    echo "=================================================="
    {
        echo "passed=$pass failed=$fail"
    } > "$SHARED/RESULT.txt"
    [ "$fail" -eq 0 ]
}

down() {
    dk rm -f $SRV $PRX $CLI >/dev/null 2>&1
    dk network rm $NET >/dev/null 2>&1
    echo "torn down"
}

case "${1:-all}" in
    build) build ;;
    up)    up ;;
    test)  test_all ;;
    down)  down ;;
    all)   build && up && test_all ;;
    one)   up && { pass=0; fail=0; version_case "$2" "$3"; echo; echo "passed $pass, failed $fail"; [ "$fail" = 0 ]; } ;;
    *)     echo "usage: $0 [build|up|test|down|all|one <profile> <expected-proto>]"; exit 1 ;;
esac
