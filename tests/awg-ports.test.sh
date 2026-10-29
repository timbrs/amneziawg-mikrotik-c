#!/bin/bash
# scripts/awg-ports.sh against real iptables and real UDP, in Docker.
#
# "srv" runs a UDP echo on 51820 (the AmneziaWG server stand-in) and the
# script; "cli" sends datagrams to ports of the range and expects them back
# from the very port it sent to; "back" is another host the range can be sent
# to instead (what a Docker-published server looks like from the host).
#
#   bash tests/awg-ports.test.sh
set -u
cd "$(dirname "$0")/.."
SRC=$(pwd -W 2>/dev/null || pwd)
NET=awgports-test
IMG=debian:bookworm-slim
dk() { MSYS_NO_PATHCONV=1 docker "$@"; }
pass=0; fail=0
ok() { if eval "$2"; then echo "  PASS  $1"; pass=$((pass + 1)); else echo "  FAIL  $1"; fail=$((fail + 1)); fi; }

cleanup() { dk rm -f awgp-srv awgp-cli awgp-back >/dev/null 2>&1; dk network rm $NET >/dev/null 2>&1; }
cleanup
dk network create $NET >/dev/null
for n in srv cli back; do
    dk run -d --name awgp-$n --network $NET --cap-add NET_ADMIN --sysctl net.ipv4.ip_forward=1 \
        -v "$SRC/scripts:/src:ro" $IMG sleep infinity >/dev/null
done
for n in srv cli back; do
    dk exec awgp-$n sh -c "apt-get update -qq && apt-get install -y -qq iptables iproute2 socat procps >/dev/null 2>&1" &
done
wait
SRV=$(dk inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' awgp-srv)
BACK=$(dk inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' awgp-back)
echo_on() { dk exec -d "$1" socat UDP4-LISTEN:51820,fork,reuseaddr EXEC:/bin/cat; }
echo_on awgp-srv
echo_on awgp-back
sleep 1

run() { dk exec awgp-srv bash /src/awg-ports.sh --lang en "$@"; }
conf() { dk exec awgp-srv sh -c ". /etc/awg-ports.conf && echo \$$1"; }
# A datagram to srv:port comes back from that same port: socat in connected
# mode drops anything from elsewhere.
answers() { [ "$(dk exec awgp-cli sh -c "echo hi-$1 | timeout 3 socat -T2 - UDP4:$SRV:$1" 2>/dev/null)" = "hi-$1" ]; }
counters() { dk exec awgp-srv sh -c "iptables -t nat -L AWG_PORTS -v -n -x | awk 'NR > 2 { p += \$1 } END { print p + 0 }'"; }

echo "=== awg-ports: a range to the local port"
out=$(run --setup --port 51820 --count 1000 --yes 2>&1)
spec=$(conf SPEC)
lo=${spec%%-*}; hi=${spec##*-}
ok "one piece of 1000 ports: $spec" '[ "$((hi - lo + 1))" = 1000 ] && [ "${spec#*,}" = "$spec" ]'
ok "inside 10000..32767" '[ "$lo" -ge 10000 ] && [ "$hi" -le 32767 ]'
ok "REDIRECT rule in its own chain" 'dk exec awgp-srv iptables -t nat -S AWG_PORTS | grep -q -- "--dport $lo:$hi -j REDIRECT --to-ports 51820"'
ok "hooked for traffic to this host only" 'dk exec awgp-srv iptables -t nat -S PREROUTING | grep -q -- "-m addrtype --dst-type LOCAL -j AWG_PORTS"'
ok "first port answers" 'answers $lo'
ok "last port answers" 'answers $hi'
ok "a port in the middle answers" 'answers $(((lo + hi) / 2))'
ok "the real port still answers" 'answers 51820'
ok "a port outside does not" '! answers $((hi + 1))'
ok "the output tells what to paste" 'printf "%s" "$out" | grep -q "# AllowedPorts = $spec"'

c1=$(counters)
run --apply >/dev/null 2>&1
ok "applying again leaves the rules alone (counters $c1 kept)" '[ "$(counters)" = "$c1" ] && [ "$c1" -gt 0 ]'
dk exec awgp-srv sh -c 'iptables -t nat -D PREROUTING -m addrtype --dst-type LOCAL -j AWG_PORTS; iptables -t nat -F AWG_PORTS'
ok "a firewall reload drops them" '! answers $lo'
run --apply >/dev/null 2>&1
ok "--apply (the minute timer) puts them back" 'answers $lo'
ok "--status says where it goes" 'run --status | grep -q "port 51820 of this server"'
ok "--status sees the rules" 'run --status | grep -q "in place"'

echo "=== busy ports and other NAT rules are stepped over"
run --remove >/dev/null 2>&1
dk exec awgp-srv iptables -t nat -A PREROUTING -p udp --dport 15000:15010 -j REDIRECT --to-ports 51820
dk exec -d awgp-srv socat UDP4-LISTEN:15050,reuseaddr EXEC:/bin/cat
sleep 1
run --setup --port 51820 --count 100 --start 14990 --yes >/dev/null 2>&1
ok "split around them: $(conf SPEC)" '[ "$(conf SPEC)" = "14990-14999,15011-15049,15051-15101" ]'
ok "one rule per piece" '[ "$(dk exec awgp-srv iptables -t nat -S AWG_PORTS | grep -c REDIRECT)" = 3 ]'
ok "and they answer" 'answers 14995 && answers 15011 && answers 15101'
dk exec awgp-srv iptables -t nat -D PREROUTING -p udp --dport 15000:15010 -j REDIRECT --to-ports 51820

echo "=== no terminal: the defaults"
run --remove >/dev/null 2>&1
out=$(dk exec awgp-srv bash /src/awg-ports.sh --lang en < /dev/null 2>&1)
spec=$(conf SPEC)
ok "asked nothing and set up 10000 ports: $spec" 'printf "%s" "$out" | grep -q "Free ports: .*(10000)"'
ok "a second run shows the settings and changes nothing" '[ "$(dk exec awgp-srv bash /src/awg-ports.sh --lang en < /dev/null | grep -c "is set up")" = 1 ] && [ "$(conf SPEC)" = "$spec" ]'

echo "=== curl | sudo bash: the questions come from the terminal"
run --remove >/dev/null 2>&1
# script(1) gives the run a terminal of its own; the script itself comes in on
# stdin, the way a pipe from curl brings it, and the answers go to the terminal.
out=$(dk exec awgp-srv sh -c 'printf "51820\n300\ny\n" | script -qec "cat /src/awg-ports.sh | bash -s -- --lang en" /dev/null' 2>&1)
ok "the wizard asked and took the answers: $(conf SPEC)" '[ "$(conf PORT)" = 51820 ] && [ "$(dk exec awgp-srv sh -c ". /etc/awg-ports.conf; echo \$SPEC" | awk -F- "{print \$2 - \$1 + 1}")" = 300 ]'
ok "and it works" 'answers $(conf SPEC | cut -d- -f1)'

echo "=== an AmneziaVPN server in Docker is found by itself"
run --remove >/dev/null 2>&1
dk exec -d awgp-back socat UDP4-LISTEN:47123,fork,reuseaddr EXEC:/bin/cat
# A Docker host is its containers' gateway; here the answer has to be brought back.
dk exec awgp-srv iptables -t nat -A POSTROUTING -d "$BACK" -j MASQUERADE
# No Docker inside the test container: a stand-in answers like a host where
# AmneziaVPN runs its amnezia-awg container on 47123/udp, at the "back" address.
dk exec awgp-srv sh -c "cat > /usr/local/bin/docker <<EOF
#!/bin/sh
case \"\\\$1\" in
    ps) echo 'amnezia-awg|0.0.0.0:47123->47123/udp, :::47123->47123/udp' ;;
    inspect) echo '$BACK ' ;;
esac
EOF
chmod +x /usr/local/bin/docker"
out=$(dk exec awgp-srv sh -c 'printf "1\n200\ny\n" | script -qec "bash /src/awg-ports.sh --lang en" /dev/null' 2>&1)
ok "offered the container" 'printf "%s" "$out" | grep -q "1) Docker container amnezia-awg, port 47123/udp"'
ok "chose the docker mode" '[ "$(conf MODE)" = docker ] && [ "$(conf CONTAINER)" = amnezia-awg ] && [ "$(conf CPORT)" = 47123 ]'
ok "DNAT to the container's address" 'dk exec awgp-srv iptables -t nat -S AWG_PORTS | grep -q -- "-j DNAT --to-destination $BACK:47123"'
ok "a port of the range reaches the container" 'answers $(conf SPEC | cut -d- -f1)'
dk exec awgp-srv rm -f /usr/local/bin/docker

echo "=== to another address (a Docker-published server looks like this)"
run --remove >/dev/null 2>&1
run --setup --to "$BACK:51820" --count 500 --yes >/dev/null 2>&1
spec=$(conf SPEC); lo=${spec%%-*}
ok "DNAT rule" 'dk exec awgp-srv iptables -t nat -S AWG_PORTS | grep -q -- "-j DNAT --to-destination $BACK:51820"'
ok "a port of the range reaches the other host" 'answers $lo'

echo "=== removal"
run --remove >/dev/null 2>&1
ok "chain gone" '! dk exec awgp-srv iptables -t nat -S AWG_PORTS >/dev/null 2>&1'
ok "hook gone" '! dk exec awgp-srv iptables -t nat -S PREROUTING | grep -q AWG_PORTS'
ok "settings gone" '! dk exec awgp-srv test -f /etc/awg-ports.conf'

cleanup
echo
if [ $fail -eq 0 ]; then echo "$pass/$pass checks passed"; else echo "$pass/$((pass + fail)) checks passed, $fail FAILED"; fi
[ $fail -eq 0 ]
