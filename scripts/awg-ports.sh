#!/usr/bin/env bash
# awg-ports: let an AmneziaWG / WireGuard server take its clients on a whole
# range of UDP ports, not only on the one it listens on.
#
# Every port of the range is redirected to the server's real port by the
# kernel (iptables NAT), so nothing about the server itself changes. awg-proxy
# on MikroTik then draws a random port of the range for every connection and
# leaves one that stops answering in ~15 s.
#
#   curl -fsSL https://raw.githubusercontent.com/timbrs/amneziawg-mikrotik-c/main/scripts/awg-ports.sh | sudo bash
#
# Run again to see the settings, change the range or remove it. Options for
# scripts: awg-ports --help.
set -u

VERSION=1
CONF=/etc/awg-ports.conf
SELF=/usr/local/sbin/awg-ports
UNIT=awg-ports
CHAIN=AWG_PORTS
DEF_COUNT=10000
MIN_PORT=10000          # below: well-known and registered services
MAX_SEGMENTS=32         # what AWG_REMOTE takes

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) L=ru ;; *) L=en ;; esac

say() { if [ "$L" = ru ]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }
warn() { if [ "$L" = ru ]; then printf '! %s\n' "$1" >&2; else printf '! %s\n' "$2" >&2; fi; }
die() { warn "$1" "$2"; exit 1; }

# Questions go to the terminal even when the script itself comes in on stdin
# (curl ... | bash); without a terminal the defaults are taken.
TTY=/dev/tty
if ! { : < /dev/tty; } 2>/dev/null; then TTY=; fi
ask() {   # ask "ru question" "en question" default -> REPLY
    local q
    if [ "$L" = ru ]; then q=$1; else q=$2; fi
    if [ -n "$YES" ] || [ -z "$TTY" ]; then REPLY=$3; return; fi
    printf '%s [%s]: ' "$q" "$3" > "$TTY"
    IFS= read -r REPLY < "$TTY" || REPLY=
    [ -n "$REPLY" ] || REPLY=$3
}

usage() {
    say "awg-ports $VERSION — диапазон UDP-портов для сервера AmneziaWG / WireGuard

Без опций: мастер настройки, а если уже настроено — текущее состояние и меню.

  --status                 показать настройки и счётчики пакетов
  --setup                  настроить заново (вопросы; с --yes — без вопросов)
  --port N                 порт сервера на этом хосте (иначе — найти самому)
  --to IP:PORT             куда направлять (Docker-контейнер или другой адрес)
  --container NAME         Docker-контейнер сервера (адрес берётся при каждом применении)
  --count N                сколько портов (по умолчанию $DEF_COUNT)
  --start N                с какого порта (по умолчанию — случайно)
  --yes                    не задавать вопросов
  --remove                 убрать правила, службу и настройки
  --apply                  применить сохранённые настройки (так их зовёт systemd)
  --lang ru|en             язык" \
        "awg-ports $VERSION — a UDP port range for an AmneziaWG / WireGuard server

No options: the setup wizard, or, once set up, the current state and a menu.

  --status                 show the settings and packet counters
  --setup                  set up again (asks; with --yes, no questions)
  --port N                 the server's port on this host (found if omitted)
  --to IP:PORT             where to send it (a Docker container or another address)
  --container NAME         the server's Docker container (its address is looked up on every apply)
  --count N                how many ports (default $DEF_COUNT)
  --start N                first port (default: random)
  --yes                    ask nothing
  --remove                 remove the rules, the service and the settings
  --apply                  apply the saved settings (this is what systemd runs)
  --lang ru|en             language"
}

ACTION= YES= O_PORT= O_TO= O_CONT= O_COUNT= O_START= QUIET=
while [ $# -gt 0 ]; do
    case $1 in
        --status) ACTION=status ;;
        --setup) ACTION=setup ;;
        --remove) ACTION=remove ;;
        --apply) ACTION=apply ;;
        --flush-rules) ACTION=flush ;;
        --quiet) QUIET=1 ;;
        --yes|-y) YES=1 ;;
        --port) O_PORT=${2:-}; shift ;;
        --to) O_TO=${2:-}; shift ;;
        --container) O_CONT=${2:-}; shift ;;
        --count) O_COUNT=${2:-}; shift ;;
        --start) O_START=${2:-}; shift ;;
        --lang) L=${2:-en}; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "неизвестная опция: $1 (см. --help)" "unknown option: $1 (see --help)" ;;
    esac
    shift
done

[ "$(id -u)" = 0 ] || die "нужен root: запустите через sudo" "root is needed: run it with sudo"

isnum() { case $1 in ''|*[!0-9]*) return 1 ;; *) [ "$1" -ge 1 ] && [ "$1" -le 65535 ] ;; esac; }

need_iptables() {
    command -v iptables >/dev/null 2>&1 && return 0
    warn "нет iptables" "iptables is missing"
    ask "Поставить iptables? (y/n)" "Install iptables? (y/n)" y
    case $REPLY in y|Y|yes|д|Д|да) ;; *) die "без iptables не получится" "cannot go on without iptables" ;; esac
    if command -v apt-get >/dev/null 2>&1; then apt-get update -qq && apt-get install -y -qq iptables
    elif command -v dnf >/dev/null 2>&1; then dnf install -y -q iptables
    elif command -v yum >/dev/null 2>&1; then yum install -y -q iptables
    elif command -v apk >/dev/null 2>&1; then apk add -q iptables
    fi
    command -v iptables >/dev/null 2>&1 || die "iptables не поставился" "iptables did not install"
}

v6_ok() {   # the host has IPv6 and ip6tables can NAT
    command -v ip6tables >/dev/null 2>&1 && ip6tables -t nat -S >/dev/null 2>&1 &&
        [ -n "$(ip -6 addr show scope global 2>/dev/null)" ]
}

# --- what is already taken -------------------------------------------------

busy_ports() {   # one port per line: UDP listeners, ports other NAT rules use, Docker's
    if command -v ss >/dev/null 2>&1; then
        ss -Hlun 2>/dev/null | awk '{n=split($4,a,":"); print a[n]}'
    else
        awk 'NR>1 {split($2,a,":"); printf "%d\n", strtonum("0x" a[2])}' /proc/net/udp /proc/net/udp6 2>/dev/null
    fi
    { iptables -t nat -S 2>/dev/null; ip6tables -t nat -S 2>/dev/null; } |
        grep -v -- "-A $CHAIN " | grep -E -- '-p udp' |
        grep -oE -- '--dports? [0-9:,]+' | awk '{print $2}' | tr ',' '\n' |
        awk -F: '{ if (NF == 2) for (p = $1; p <= $2; p++) print p; else print $1 }'
    if command -v docker >/dev/null 2>&1; then
        docker ps --format '{{.Ports}}' 2>/dev/null | tr ',' '\n' |
            sed -nE 's/.*:([0-9]+)(-([0-9]+))?->.*/\1 \3/p' |
            awk '{ if ($2 != "") for (p = $1; p <= $2; p++) print p; else print $1 }'
    fi
}

# Pick COUNT ports from [MIN_PORT, below the kernel's ephemeral range], taking
# the free ones from a random start; busy ports split the range. Prints the
# spec ("20113-30112" or "20113-24000,24002-30113").
pick_range() {
    local count=$1 start=$2 lo hi eph span tries best= bestn=999 spec n
    eph=$(awk '{print $1}' /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null)
    isnum "$eph" || eph=32768
    lo=$MIN_PORT
    hi=$((eph - 1))
    [ "$hi" -gt "$lo" ] || hi=65535
    span=$((hi - lo + 1))
    [ "$count" -le $((span - 200)) ] || count=$((span - 200))
    local busy
    busy=$(busy_ports | sort -un)
    for tries in 1 2 3 4 5 6 7 8; do
        local s=$start
        if [ -z "$s" ]; then s=$((lo + ((RANDOM << 15 | RANDOM) % (span - count + 1)))); fi
        spec=$(printf '%s\n' "$busy" | awk -v s="$s" -v c="$count" -v hi="$hi" '
            NF { b[$1] = 1 }
            END {
                p = s; got = 0; out = ""; a = -1
                while (got < c && p <= hi) {
                    if (p in b) { if (a >= 0) { out = out (out ? "," : "") (a == p - 1 ? a : a "-" (p - 1)); a = -1 } }
                    else { if (a < 0) a = p; got++ }
                    p++
                }
                if (a >= 0) out = out (out ? "," : "") (a == p - 1 ? a : a "-" (p - 1))
                print out
            }')
        n=$(printf '%s' "$spec" | tr ',' '\n' | grep -c .)
        if [ "$n" -lt "$bestn" ]; then best=$spec; bestn=$n; fi
        [ "$n" -le 1 ] || [ -n "$start" ] && break
    done
    [ "$bestn" -le "$MAX_SEGMENTS" ] || return 1
    printf '%s\n' "$best"
}

spec_count() {
    printf '%s' "$1" | tr ',' '\n' | awk -F- 'NF { n += (NF == 2 ? $2 - $1 + 1 : 1) } END { print n + 0 }'
}

# --- finding the server ------------------------------------------------------

# Candidates, one per line: "host <port> <what>" or "docker <name> <hostport> <ctrport>".
find_servers() {
    local t
    for t in awg wg; do
        command -v $t >/dev/null 2>&1 || continue
        $t show all listen-port 2>/dev/null | awk -v t=$t '$2 ~ /^[0-9]+$/ && $2 > 0 { print "host", $2, t " " $1 }'
    done
    if command -v docker >/dev/null 2>&1; then
        docker ps --format '{{.Names}}|{{.Ports}}' 2>/dev/null | while IFS='|' read -r name ports; do
            printf '%s\n' "$ports" | tr ',' '\n' | grep '/udp' |
                sed -nE 's/.*:([0-9]+)->([0-9]+)\/udp.*/\1 \2/p' | sort -u |
                while read -r hp cp; do echo "docker $name $hp $cp"; done
        done | sort -u
    fi
    if command -v ss >/dev/null 2>&1; then
        ss -Hlunp 2>/dev/null | grep -E 'amneziawg|wireguard' |
            awk '{n=split($4,a,":"); m=$NF; gsub(/users:\(\("|".*/, "", m); print "host", a[n], m}'
    fi
}

docker_ip() {   # the container's address on its first network
    docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}} {{end}}' "$1" 2>/dev/null | awk '{print $1}'
}

# --- the rules ---------------------------------------------------------------

# Rules for the saved settings, one iptables argument list per line.
rules_for() {   # $1 = target "port N" | "to IP:PORT"
    local seg dp
    for seg in $(printf '%s' "$SPEC" | tr ',' ' '); do
        dp=$(printf '%s' "$seg" | tr '-' ':')
        case $1 in
            # In the form iptables -S prints them back, so a check finds no change.
            port\ *) echo "-A $CHAIN -p udp -m udp --dport $dp -j REDIRECT --to-ports ${1#port }" ;;
            to\ *) echo "-A $CHAIN -p udp -m udp --dport $dp -j DNAT --to-destination ${1#to }" ;;
        esac
    done
}

target() {   # what the rules point at, from the saved settings
    case $MODE in
        host) echo "port $PORT" ;;
        to) echo "to $TO" ;;
        docker)
            local ip
            ip=$(docker_ip "$CONTAINER")
            [ -n "$ip" ] && echo "to $ip:$CPORT" ;;
    esac
}

apply_family() {   # $1 = iptables | ip6tables, $2 = target
    local ipt=$1 want have
    want=$(rules_for "$2")
    have=$($ipt -t nat -S $CHAIN 2>/dev/null | grep -- "^-A $CHAIN ")
    if [ "$want" != "$have" ]; then
        $ipt -t nat -N $CHAIN 2>/dev/null
        $ipt -t nat -F $CHAIN
        printf '%s\n' "$want" | while read -r r; do [ -n "$r" ] && $ipt -t nat $r; done
        CHANGED=1
    fi
    # Only what comes for this host itself: forwarded traffic of the VPN's own
    # clients to some other server on the same ports must pass untouched.
    if ! $ipt -t nat -C PREROUTING -m addrtype --dst-type LOCAL -j $CHAIN 2>/dev/null; then
        $ipt -t nat -I PREROUTING 1 -m addrtype --dst-type LOCAL -j $CHAIN
        CHANGED=1
    fi
}

flush_family() {
    local ipt=$1
    command -v $ipt >/dev/null 2>&1 || return 0
    while $ipt -t nat -D PREROUTING -m addrtype --dst-type LOCAL -j $CHAIN 2>/dev/null; do :; done
    $ipt -t nat -F $CHAIN 2>/dev/null
    $ipt -t nat -X $CHAIN 2>/dev/null
    return 0
}

load_conf() {
    MODE= PORT= TO= CONTAINER= CPORT= SPEC= V6=
    [ -f "$CONF" ] || return 1
    # shellcheck disable=SC1090
    . "$CONF"
    [ -n "$SPEC" ] && [ -n "$MODE" ]
}

do_apply() {
    load_conf || { [ -n "$QUIET" ] || warn "не настроено" "not set up"; return 1; }
    need_iptables
    local tg
    tg=$(target)
    if [ -z "$tg" ]; then
        warn "контейнер $CONTAINER не запущен — правила не тронуты" "container $CONTAINER is not running — rules left as they are"
        return 1
    fi
    CHANGED=
    apply_family iptables "$tg"
    if [ "$V6" = 1 ] && [ "$MODE" = host ] && v6_ok; then apply_family ip6tables "$tg"; fi
    if [ -n "$CHANGED" ] && command -v logger >/dev/null 2>&1; then
        logger -t awg-ports "rules applied: udp $SPEC -> ${tg#* }"
    fi
    return 0
}

# --- persistence -------------------------------------------------------------

install_units() {
    if [ "$0" != "$SELF" ]; then
        if [ -f "$0" ] && [ -r "$0" ]; then cp "$0" "$SELF"
        elif [ -n "${AWG_PORTS_SRC:-}" ]; then cp "$AWG_PORTS_SRC" "$SELF"
        else
            # Piped in from curl: there is no file of ourselves to copy.
            curl -fsSL https://raw.githubusercontent.com/timbrs/amneziawg-mikrotik-c/main/scripts/awg-ports.sh \
                -o "$SELF" 2>/dev/null || wget -qO "$SELF" \
                https://raw.githubusercontent.com/timbrs/amneziawg-mikrotik-c/main/scripts/awg-ports.sh
        fi
        chmod 755 "$SELF" 2>/dev/null
    fi
    if [ ! -d /run/systemd/system ]; then
        warn "нет systemd: после перезагрузки выполните «$SELF --apply» (например, из /etc/rc.local)" \
             "no systemd: after a reboot run \"$SELF --apply\" (e.g. from /etc/rc.local)"
        return 0
    fi
    cat > /etc/systemd/system/$UNIT.service <<EOF
[Unit]
Description=UDP port range for the AmneziaWG server (awg-ports)
After=network-online.target docker.service
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$SELF --apply --quiet
ExecStop=$SELF --flush-rules

[Install]
WantedBy=multi-user.target
EOF
    # A firewall reload or a Docker restart can drop the rules, and a container
    # can come back on another address: put them back within a minute.
    cat > /etc/systemd/system/$UNIT-check.service <<EOF
[Unit]
Description=Re-apply the awg-ports rules if they are gone or stale

[Service]
Type=oneshot
ExecStart=$SELF --apply --quiet
EOF
    cat > /etc/systemd/system/$UNIT-check.timer <<EOF
[Unit]
Description=Check the awg-ports rules every minute

[Timer]
OnBootSec=1min
OnUnitActiveSec=1min

[Install]
WantedBy=timers.target
EOF
    systemctl daemon-reload
    systemctl enable --now $UNIT.service $UNIT-check.timer >/dev/null 2>&1
}

remove_units() {
    if [ -d /run/systemd/system ]; then
        systemctl disable --now $UNIT-check.timer $UNIT.service >/dev/null 2>&1
        rm -f /etc/systemd/system/$UNIT.service /etc/systemd/system/$UNIT-check.service \
              /etc/systemd/system/$UNIT-check.timer
        systemctl daemon-reload
    fi
}

# --- the screens -------------------------------------------------------------

public_ip() {
    ip -4 route get 1.1.1.1 2>/dev/null | sed -nE 's/.* src ([0-9.]+).*/\1/p' | head -n1
}

show_status() {
    load_conf || { say "Диапазон портов не настроен." "The port range is not set up."; return 1; }
    local n tg pkts ip
    n=$(spec_count "$SPEC")
    tg=$(target)
    ip=$(public_ip)
    say "Диапазон портов настроен:" "The port range is set up:"
    say "  порты:        $SPEC  ($n шт.)" "  ports:        $SPEC  ($n)"
    case $MODE in
        host) say "  куда:         порт $PORT этого сервера" "  goes to:      port $PORT of this server" ;;
        to) say "  куда:         $TO" "  goes to:      $TO" ;;
        docker) say "  куда:         контейнер $CONTAINER, порт $CPORT (сейчас ${tg#to })" \
                    "  goes to:      container $CONTAINER, port $CPORT (now ${tg#to })" ;;
    esac
    if iptables -t nat -C PREROUTING -m addrtype --dst-type LOCAL -j $CHAIN 2>/dev/null; then
        pkts=$(iptables -t nat -L $CHAIN -v -n -x 2>/dev/null | awk 'NR > 2 { p += $1 } END { print p + 0 }')
        say "  правила:      на месте, новых подключений через диапазон: $pkts" \
            "  rules:        in place, new connections through the range: $pkts"
    else
        say "  правила:      НЕТ — выполните: $SELF --apply" "  rules:        MISSING — run: $SELF --apply"
    fi
    if [ -d /run/systemd/system ]; then
        say "  после перезагрузки: $(systemctl is-enabled $UNIT.service 2>/dev/null), проверка раз в минуту: $(systemctl is-active $UNIT-check.timer 2>/dev/null)" \
            "  after a reboot: $(systemctl is-enabled $UNIT.service 2>/dev/null), checked every minute: $(systemctl is-active $UNIT-check.timer 2>/dev/null)"
    fi
    echo
    say "В конфигураторе отметьте «Сервер принимает AmneziaWG на нескольких портах» и вставьте:" \
        "In the configurator tick \"The server takes AmneziaWG on several ports\" and paste:"
    echo
    echo "    $SPEC"
    echo
    say "или допишите в .conf клиента строку (перед [Peer]):" "or add this line to the client's .conf (before [Peer]):"
    echo
    echo "    # AllowedPorts = $SPEC"
    echo
    [ -n "$ip" ] && say "AWG_REMOTE для контейнера на MikroTik: $ip:$SPEC" "AWG_REMOTE for the MikroTik container: $ip:$SPEC"
    say "Если у хостера есть свой файрвол (панель VPS), откройте там UDP $SPEC." \
        "If your provider has a firewall of its own (the VPS panel), open UDP $SPEC there."
    return 0
}

choose_server() {   # sets MODE PORT TO CONTAINER CPORT
    MODE= PORT= TO= CONTAINER= CPORT=
    if [ -n "$O_TO" ]; then MODE=to; TO=$O_TO; return 0; fi
    if [ -n "$O_CONT" ]; then
        MODE=docker; CONTAINER=$O_CONT
        CPORT=$(find_servers | awk -v n="$O_CONT" '$1 == "docker" && $2 == n { print $4; exit }')
        [ -n "$CPORT" ] || CPORT=${O_PORT:-}
        isnum "$CPORT" || die "не нашёл UDP-порт контейнера $O_CONT — укажите --port" \
                              "no UDP port found for container $O_CONT — give --port"
        return 0
    fi
    if [ -n "$O_PORT" ]; then
        isnum "$O_PORT" || die "неверный порт: $O_PORT" "bad port: $O_PORT"
        MODE=host; PORT=$O_PORT; return 0
    fi
    local list n i line
    list=$(find_servers | awk '!seen[$1 FS $2]++')
    n=$(printf '%s' "$list" | grep -c .)
    if [ "$n" -eq 0 ]; then
        say "Сервер AmneziaWG/WireGuard не нашёлся сам." "No AmneziaWG/WireGuard server found by itself."
        ask "На каком UDP-порту он слушает" "Which UDP port does it listen on" 51820
        isnum "$REPLY" || die "неверный порт: $REPLY" "bad port: $REPLY"
        MODE=host; PORT=$REPLY; return 0
    fi
    say "Нашёл:" "Found:"
    i=0
    while IFS= read -r line; do
        i=$((i + 1))
        set -- $line
        if [ "$1" = docker ]; then
            say "  $i) Docker-контейнер $2, порт $3/udp" "  $i) Docker container $2, port $3/udp"
        else
            say "  $i) порт $2/udp ($3 ${4:-})" "  $i) port $2/udp ($3 ${4:-})"
        fi
    done <<EOF
$list
EOF
    ask "Какой из них (номер) или свой порт" "Which one (number) or a port of your own" 1
    if isnum "$REPLY" && [ "$REPLY" -le "$n" ]; then
        set -- $(printf '%s\n' "$list" | sed -n "${REPLY}p")
        if [ "$1" = docker ]; then MODE=docker; CONTAINER=$2; CPORT=$4
        else MODE=host; PORT=$2; fi
    elif isnum "$REPLY"; then
        MODE=host; PORT=$REPLY
    else
        die "не понял: $REPLY" "did not get that: $REPLY"
    fi
}

do_setup() {
    need_iptables
    choose_server
    local count start spec
    count=${O_COUNT:-}
    if [ -z "$count" ]; then
        ask "Сколько портов взять" "How many ports to take" $DEF_COUNT
        count=$REPLY
    fi
    case $count in ''|*[!0-9]*) die "неверное число: $count" "bad number: $count" ;; esac
    [ "$count" -ge 2 ] || die "нужно хотя бы 2 порта" "at least 2 ports are needed"
    start=${O_START:-}
    if [ -n "$start" ]; then isnum "$start" || die "неверный порт: $start" "bad port: $start"; fi
    spec=$(pick_range "$count" "$start") ||
        die "не нашлось столько свободных портов одним куском — уменьшите --count" \
            "not enough free ports in one piece — lower --count"
    say "Свободные порты: $spec ($(spec_count "$spec") шт.)" "Free ports: $spec ($(spec_count "$spec"))"
    if [ -z "$YES" ]; then
        ask "Применить? (y/n)" "Apply? (y/n)" y
        case $REPLY in y|Y|yes|д|Д|да) ;; *) say "Ничего не менял." "Changed nothing."; exit 0 ;; esac
    fi
    flush_family iptables
    flush_family ip6tables
    V6=0
    if [ "$MODE" = host ] && v6_ok; then V6=1; fi
    cat > "$CONF" <<EOF
# awg-ports $VERSION — written by the script; change it by running it again
MODE=$MODE
PORT=$PORT
TO=$TO
CONTAINER=$CONTAINER
CPORT=$CPORT
SPEC=$spec
V6=$V6
EOF
    do_apply || die "правила не применились" "the rules did not apply"
    install_units
    echo
    show_status
}

do_remove() {
    remove_units
    flush_family iptables
    flush_family ip6tables
    rm -f "$CONF"
    [ "$0" = "$SELF" ] || rm -f "$SELF"
    say "Диапазон портов убран, сервер слушает только свой порт." \
        "The port range is gone; the server listens on its own port only."
}

case $ACTION in
    status) show_status ;;
    apply) do_apply ;;
    flush) flush_family iptables; flush_family ip6tables ;;
    remove) do_remove ;;
    setup) do_setup ;;
    *)
        if load_conf; then
            show_status
            echo
            ask "Enter — выйти, r — перенастроить, d — удалить" "Enter — quit, r — set up again, d — remove" ""
            case $REPLY in
                r|R|к|К) do_setup ;;
                d|D|в|В) do_remove ;;
            esac
        else
            say "Настроим приём AmneziaWG на диапазоне UDP-портов." "Let us make the server take AmneziaWG on a range of UDP ports."
            do_setup
        fi
        ;;
esac
