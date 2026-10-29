# awg-proxy environment variables

> Part of the [awg-proxy](../../README_en.md) documentation: a RouterOS container that connects MikroTik to AmneziaWG servers. The easiest way to install it is the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), which generates all commands. Other pages: [install](install.md) · [updating](update.md) · [variables](env.md) · [AWG 3.x and IPv6](protocol.md) · [routing and MTU](routing.md) · [server mode](server-mode.md) · [troubleshooting](troubleshooting.md) · [FAQ](faq.md).

## All Environment Variables

| Variable | Required | Default | Description |
|----------|:---:|:---:|-------------|
| `AWG_LISTEN` | Yes | -- | Listen address |
| `AWG_REMOTE` | Yes | -- | AWG server address; the port may be a list or a range |
| `AWG_JC` | Yes | -- | Junk packet count |
| `AWG_JMIN` | Yes | -- | Min junk packet size |
| `AWG_JMAX` | Yes | -- | Max junk packet size |
| `AWG_S1` | Yes | -- | Handshake init padding |
| `AWG_S2` | Yes | -- | Handshake response padding |
| `AWG_H1`--`AWG_H4` | Yes | -- | Message types |
| `AWG_SERVER_PUB` | Yes | -- | Server public key |
| `AWG_CLIENT_PUB` | Yes* | -- | Client public key; in `server` mode this is the legacy single-peer / proxy-only fallback |
| `AWG_CLIENT_PUBS` | No | -- | In `server` mode: list of real public keys for direct clients |
| `AWG_CLIENT_PUBS_FILE` | No | -- | In `server` mode: path to a file containing real public keys for direct clients |
| `AWG_S3` | No | `0` | Cookie reply padding (v2) |
| `AWG_S4` | No | `0` | Transport data padding (v2) |
| `AWG_I1`--`AWG_I5` | No | -- | CPS templates (v1.5/v2/v3) |
| `AWG_HEADER_PROTECTION_KEY` | No | -- | Header protection key (v3), base64 (44 chars) or hex (64 chars). Setting it enables AmneziaWG 3.0 |
| `AWG_RANDOM_TRAILERS` | No | `off` | v3.1: random trailer on handshakes (`on`/`off`). Must match the server |
| `AWG_DISABLE_COOKIES` | No | `off` | v3.1: never send a cookie reply outbound (`on`/`off`) |
| `AWG_MODE` | No | `normal` | Operating mode: `normal`, `reverse`, `server` |
| `AWG_FB_H1`--`AWG_FB_H4` | No | -- | Fallback chain stage 1: message types. Enables the chain |
| `AWG_FB_S1`, `AWG_FB_S2` | Yes** | -- | Init/response padding for stage 1 |
| `AWG_FB_S3`, `AWG_FB_S4` | No | `0` | Cookie reply / transport padding for stage 1 |
| `AWG_FB_I1`--`AWG_FB_I5` | No | -- | CPS templates for stage 1 |
| `AWG_FB_HP` | No | `0` | `1` = stage 1 also encrypts the header (needs `AWG_HEADER_PROTECTION_KEY`) |
| `AWG_FB_RANDOM_TRAILERS` | No | `off` | `on` = stage 1 also appends a random trailer (v3.1) |
| `AWG_FB2_*`, `AWG_FB3_*` | No | -- | Chain stages 2 and 3, same set of keys |
| `AWG_FB_AFTER` | No | `20` | Seconds of remote silence before probing the next stage (initiator) |
| `AWG_SRC_PORT` | No | random | Outgoing port to server: `random` / `auto` / number |
| `AWG_TIMEOUT` | No | `60` | How long the server may stay silent on an unanswered handshake before the proxy reconnects (sec) |
| `AWG_DNS_REFRESH` | No | `60` | Background DNS re-check interval for a hostname in AWG_REMOTE (sec, `0` = off) |
| `AWG_PORT_ROTATE` | No | `30m-2h` | Move to another port of the `AWG_REMOTE` list on a random timer from a span: `30m-2h`, `1800-7200` (sec); `0` = off. Only acts on a port list |
| `AWG_PORT_ROTATE_MB` | No | -- | ...or after a volume on the port, MB: `1000`, `800-1200`, `1G`. Whichever comes first |
| `AWG_PORT_ROTATE_SOFT` | No | `auto` | Move under load too when no quiet spell comes for 15 minutes: `on` / `off` / `auto` (64-bit CPU and 4+ cores) |
| `AWG_HE_DELAY` | No | `250` | Happy Eyeballs: ms of IPv4 head start before probing IPv6 (only when the name has both an A and an AAAA) |
| `AWG_STATE_FILE` | No | `/etc/awg-proxy.state` | File with the learned address-family preference; an empty value means do not save it |
| `AWG_LOG_LEVEL` | No | `info` | Log level |
| `AWG_NO_GRO` | No | `0` | Disable UDP GRO |
| `AWG_NO_GSO` | No | `0` | Disable UDP GSO on send |
| `AWG_NO_DF` | No | `0` | Clear the DF bit on UDP packets (workaround for DPI dropping DF=1) |
| `AWG_SOCKET_BUF` | No | auto | Socket buffer size. Unset: 32 MB on a 64-bit router with 4+ cores, on a weaker one the receive buffer follows the traffic (≈50 ms at the peak rate, 416 KB to 32 MB) |
| `AWG_CPU_C2S` | No | `-1` | CPU for client→server thread |
| `AWG_CPU_S2C` | No | `-1` | CPU for server→client thread |
| `AWG_RT` | No | `0` | `SCHED_RR` priority for the I/O threads (1..50, `0` = off). Needs privileges |
| `AWG_RPS` | No | — | Core mask for RPS on the container's interface (hex, e.g. `9`). Needs privileges |
| `AWG_BUSY_POLL` | No | `0` | SO_BUSY_POLL timeout (μs). Unavailable on RouterOS — its kernel is built without `CONFIG_NET_RX_BUSY_POLL` |
| `AWG_SPIN` | No | `0` | Userspace spin instead of busy poll: μs of non-blocking re-reads before sleeping. `auto` self-tunes |
| `AWG_STATS` | No | `0` | Seconds between stat lines (throughput, our drops, kernel drops, per-socket drops) |
| `AWG_DNS` | No | -- | DNS server for hostname resolution in AWG_REMOTE |
| `AWG_REJECT_AFTER` | No | -- | v3: accepted, not emulated. Logs a WARN when the lower bound is < 150 s |
| `AWG_CONTENT_PADDING` | No | -- | v3: accepted and ignored (see below) |
| `AWG_REKEY_AFTER`, `AWG_REKEY_TIMEOUT`, `AWG_KEEPALIVE_TIMEOUT`, `AWG_MAX_HANDSHAKE_ATTEMPTS` | No | -- | v3: accepted and ignored (see below) |

`*` In `server` mode, you must set either the legacy `AWG_CLIENT_PUB` fallback or the explicit direct-peer list via `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE`.

`**` Required only when the fallback profile is enabled (`AWG_FB_H1` is set).

The protocol version is detected automatically: **v3.1** if `AWG_RANDOM_TRAILERS` or `AWG_DISABLE_COOKIES` is on, otherwise **v3** if `AWG_HEADER_PROTECTION_KEY` is set, otherwise **v2** if S3/S4 are set or H values are ranges, otherwise **v1.5** if CPS templates (I1-I5) are set, otherwise **v1**.

## Detailed Variable Descriptions

### Required -- Obfuscation Parameters

All values are taken from the `.conf` file exported from AmneziaVPN (`[Interface]` and `[Peer]` sections). They must **exactly** match the server parameters, otherwise the handshake will fail.

**`AWG_LISTEN`** -- address and port where the proxy receives UDP packets: from the router's WireGuard client in `normal` mode, from AWG clients on the internet in `reverse` and `server` modes. Format: `address:port` or `:port` (listen on all interfaces). An IPv6 literal goes in square brackets, as in `AWG_REMOTE`.

```
AWG_LISTEN=:51820             # all interfaces, port 51820 (standard)
AWG_LISTEN=172.18.0.2:9000    # specific address and port
AWG_LISTEN=[::]:443           # both families at once (a hub reachable over IPv6)
AWG_LISTEN=[2001:db8::1]:443  # this IPv6 address only
```

**`AWG_REMOTE`** -- address and port of the AWG server (`Endpoint` from `[Peer]`). Supports IPv4, IPv6 and domain names. An IPv6 literal **must** be bracketed: the port is separated by a colon and the address itself is full of them, so without brackets the form is ambiguous and is rejected.

```
AWG_REMOTE=1.2.3.4:443            # IPv4 + port
AWG_REMOTE=[2001:db8::1]:443      # IPv6 + port (brackets required)
AWG_REMOTE=vpn.example.com:51820  # domain + port
```

Instead of a single port you can list several -- comma-separated, ranges with a dash (up to 32 items):

```
AWG_REMOTE=1.2.3.4:443,8080                    # two ports
AWG_REMOTE=1.2.3.4:20150-20299                 # a range
AWG_REMOTE=1.2.3.4:20150-20299,21500-21649     # several ranges
AWG_REMOTE=[2001:db8::1]:6000-6100             # same for IPv6
```

The port is drawn from the list **at random, anew on every connection** -- at startup and on every reconnect (timeout, a new IP from DNS, an `AWG_FB_*` stage switch). Two reasons. First, a fixed port is a distinctive detail for DPI, all the more so when every client of the server sits on the same one; a random port out of a wide range offers no such handle. Second, a blocked port stops being fatal: when the server stays silent, the proxy moves to another port after **15 seconds** instead of waiting out the full `AWG_TIMEOUT` (60 seconds by default). The log shows `no answer on this port, trying another one`, and the port in use as `connected to <address> port <port>`.

This only works if the **server really accepts every listed port** -- that is, it DNATs/REDIRECTs all of them to its AmneziaWG port. Listing ports nothing is listening on buys silence and endless hopping. How to switch a range on for your own server with one command — [ports.md](ports.md). The web configurator fills these ranges into `AWG_REMOTE` when the `.conf` carries an `# AllowedPorts = ...` line (written by the bot that issued the key). For a Cloudflare WARP config it fills in Cloudflare's WireGuard ports by itself: `2408,500,1701,4500`.

**`AWG_PORT_ROTATE`**, **`AWG_PORT_ROTATE_MB`** -- move to another port of the `AWG_REMOTE` list without a reconnect, unnoticed by users. It happens in two steps. First the port falls due: it has served a random time drawn from `AWG_PORT_ROTATE`, or carried a random volume drawn from `AWG_PORT_ROTATE_MB` (both ways), whichever comes first. Every new port draws fresh numbers, so neither the time nor the volume repeats. A due port moves as soon as the tunnel **goes quiet**: for 30 seconds no more than 64 KB per 5 s went through it in either direction. The router's own background -- netwatch probes, DNS, phones' push heartbeats -- is a few kilobytes, so at night the move comes almost at once, by day it waits for a lull.

The move happens on the first client packet the server answers: somebody's packet or a WireGuard handshake. The socket stays the same, `connect()` just points it at another port; the server follows on that very packet, as it follows any roaming peer, and the session keys do not change. The new flow opens like a fresh connection, with junk and I1--I5. Until the new port answers, every client packet goes out **on both ports**, the new and the old: the server takes whichever copy arrives first and drops the other as a replay. Whatever the server had already sent to the old port is not lost either: for another 3 seconds a second socket on the same local port keeps hearing it and hands it to the client. The answer from the new port is the check: none within a second and the proxy stops doubling and stays on the old port (its NAT entries are still alive), trying another in 5 minutes. A dead port therefore costs not a single packet. A port the server answers with ICMP "port unreachable" is dropped at once, without the wait, and is not drawn again (`refused, back to M` in the log). Should the second socket be unavailable, the move goes without doubling: a 2-second check, and the silent port is remembered.

The server does not answer keepalives, so they never move the port; on a fully idle tunnel the move waits for a scheduled handshake, which needs a `persistent-keepalive` on the WireGuard peer (the configurator sets 25 s). Works only in `normal` and only with a port list -- there it is on by default (30-120 minutes); as for the hop, the server must accept the whole range ([ports.md](ports.md)), and a MikroTik hub must run awg-proxy 1.4.4 or newer. With a single port in `AWG_REMOTE` the variable has no effect.

**`AWG_PORT_ROTATE_SOFT`** -- when no quiet spell comes, a due port still moves, but **under load**: after 15 minutes of waiting (or the low end of `AWG_PORT_ROTATE`, if that is shorter). The move is the same as in a lull: both ports carry the traffic until the new one answers, so the flow does not stop for a moment, even when the new port is dead. Under load the doubling lasts one RTT, from a few dozen to a few hundred packets. By default (`auto`) it is on only on strong hardware: a 64-bit CPU and 4+ cores (hAP ax², ax³, RB5009). On a weak router doubled sends under load would take cycles from WireGuard's crypto, so there the port waits for a lull.

```
AWG_PORT_ROTATE=30m-2h          # default: every 30-120 minutes, as soon as the tunnel is quiet
AWG_PORT_ROTATE=0               # turn it off
AWG_PORT_ROTATE_MB=800-1200     # or after 0.8-1.2 GB on the port
AWG_PORT_ROTATE_SOFT=off        # never under load, only in a lull
```

In the log: `rotate: port due (timer)` / `(1003 MB carried)` -- the port fell due, `rotate: tunnel quiet, moving to port N, both ports carry until it answers` -- the move, `rotate: no quiet spell for N s, moving under load` and `rotate: tunnel busy, moving to port N, both ports carry until it answers` -- a soft move under load, `rotate: port N answered, staying (12 ms, 40 sent twice)` -- the port is confirmed: it answered after 12 ms, 40 packets went out on both ports; `stayed silent, back to M` -- the new port kept quiet, the traffic stayed on the old one.

Receiving over IPv6 is set up separately, through `AWG_LISTEN` (see above): in `normal` it is not needed -- that is a local veth inside the router -- while in `server`/`reverse` it is exactly what makes the hub reachable for clients.

**`AWG_JC`**, **`AWG_JMIN`**, **`AWG_JMAX`** -- junk packet parameters. Before each handshake init, `JC` random UDP packets of size between `JMIN` and `JMAX` bytes are sent. The server discards them, but they look like regular traffic to DPI. Values from `.conf` (`Jc`, `Jmin`, `Jmax`).

```
AWG_JC=5      # 5 junk packets before handshake
AWG_JMIN=30   # minimum 30 bytes
AWG_JMAX=500  # maximum 500 bytes

AWG_JC=0      # junk packets disabled
```

**`AWG_S1`**, **`AWG_S2`** -- number of padding bytes added to handshake init (S1) and handshake response (S2). Changes packet sizes so DPI cannot identify WireGuard handshake by its characteristic sizes of 148 and 92 bytes. Values from `.conf` (`S1`, `S2`).

```
AWG_S1=20   # +20 bytes to handshake init (148 → 168)
AWG_S2=20   # +20 bytes to handshake response (92 → 112)

AWG_S1=0    # padding disabled
AWG_S2=0
```

**`AWG_H1`**, **`AWG_H2`**, **`AWG_H3`**, **`AWG_H4`** -- WireGuard message type substitution. Standard types (1, 2, 3, 4) are replaced with the specified values so DPI cannot recognize the protocol. In v1 -- fixed numbers, in v2 -- can be `min-max` ranges. Values from `.conf` (`H1`--`H4`).

```
# v1: fixed values
AWG_H1=1234567890
AWG_H2=1234567891
AWG_H3=1234567892
AWG_H4=1234567893

# v2: ranges (random value from range for each packet)
AWG_H1=100-200
AWG_H4=1000-2000
```

**`AWG_SERVER_PUB`**, **`AWG_CLIENT_PUB`** -- server and client public keys in base64 format (44 characters). Used for MAC1 recalculation in handshake packets after header substitution. Without correct keys, the MAC check on the server will fail.

```
AWG_SERVER_PUB=kB3VpJIEGVTW2D4GR0cC/c3bOEG3jNIm5MjHJkSIj2I=
AWG_CLIENT_PUB=aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789+/ABCD=

# Automatic retrieval from the router's WireGuard interface:
AWG_CLIENT_PUB=[/interface/wireguard/get [find name=wg-awg-proxy] public-key]
```

- In `normal` and `reverse` modes, `AWG_CLIENT_PUB` is the regular required remote peer key.
- In `server` mode, `AWG_CLIENT_PUB` is now the **legacy fallback** for single-peer compatibility and proxy-only placeholder setups.
- For direct AmneziaWG 2.0 clients in `server` mode, list the real client keys via **`AWG_CLIENT_PUBS`** or **`AWG_CLIENT_PUBS_FILE`**.

**`AWG_CLIENT_PUBS`** -- list of real public keys for direct clients in `server` mode. Separators: comma, space, or newline.

```
AWG_CLIENT_PUBS=base64key1,base64key2
AWG_CLIENT_PUBS="base64key1 base64key2"
```

**`AWG_CLIENT_PUBS_FILE`** -- path to a file with real public keys for direct clients in `server` mode.

```
AWG_CLIENT_PUBS_FILE=/etc/awg-proxy/client-pubs.txt
```

Each file line should contain one base64 WireGuard public key. For outbound WG handshake responses, the server proxy compares the original standard WG MAC1 against the keys from this list, selects the matching peer, and then rewrites the response with that peer-specific MAC1. If no match is found, the legacy fallback from `AWG_CLIENT_PUB` is used.

### Optional -- Protocol v2

**`AWG_S3`**, **`AWG_S4`** -- padding for cookie reply (S3) and transport data (S4). Introduced in AWG v2. If S3 > 0 or S4 > 0 are set, the proxy automatically switches to v2 mode.

```
AWG_S3=0    # default, no padding
AWG_S4=16   # +16 bytes to each transport data packet
```

**`AWG_I1`--`AWG_I5`** -- CPS templates (Constant Packet Size). Up to 5 templates for generating fixed-format packets before handshake. If set without S3/S4/H ranges, the proxy operates in v1.5 mode. They go out before every handshake even with `AWG_JC=0` -- that is how a single cover packet without junk packets is configured. Random bytes (`<r>`, `<rc>`, `<rd>`) and junk packets come from ChaCha20 keyed from `/dev/urandom`: they are new in every packet and cannot be worked out from the bytes next to them.

Supported tags (matching upstream `device/obf.go`): `<b 0xHEX>` -- static bytes, `<r N>` -- N random bytes, `<rc N>` -- N random letters (52 letters, no digits), `<rd N>` -- N random digits, `<t>` -- unix time as 4 big-endian bytes, `<dz N>` -- N zero bytes, `<d>` and `<ds>` -- accepted and contribute nothing (upstream feeds I-packets an empty source). The `<c>` counter tag is our own extension; upstream will reject it.

```
AWG_I1=b:48656c6c6f,r:10,t:4,c:4
```

### Optional -- Operating Mode

**`AWG_MODE`** -- proxy operating mode. Determines the direction of packet transformation.

- `normal` (default) -- standard proxy: accepts WireGuard from the router, transforms to AWG, and sends to the AWG server.
- `reverse` -- reverse proxy (1:1 site-to-site): accepts AWG from another normal proxy, transforms back to WireGuard, and sends to a local WG server. Used in pair with a normal proxy on the other side.
- `server` -- reverse proxy hub (1:N): essentially an AmneziaWG Server equivalent. Supports connections from multiple normal proxies simultaneously. Response routing from the WG server to the correct client is done via a session table using `sender_index`/`receiver_index` from WireGuard packets.

```
AWG_MODE=normal    # default
AWG_MODE=reverse   # reverse proxy, 1:1
AWG_MODE=server    # AmneziaWG server, 1:N
```

In `reverse` and `server` modes, `AWG_REMOTE` points to the WireGuard server (not the AWG server), while `AWG_LISTEN` accepts AWG traffic from normal proxies. Obfuscation parameters (H1--H4, S1--S4, JC, etc.) must match the normal proxy on the other side.

### Optional -- Network and Diagnostics

**`AWG_SRC_PORT`** -- outgoing UDP port for the connection to the AWG server. By default (`random`) the kernel picks the port: no bind, a fresh ephemeral port on every reconnect. The value `auto` makes the proxy copy the WireGuard client's port, and a number pins a fixed port.

**Why `random` became the default (as of v1.2.7).** Copying the client's port pins the outgoing 5-tuple for the whole life of the container, and a NAT entry built from it then cannot be shaken off. Masquerade picks the source address once, when the entry is created. If the entry is born while the WAN interface has no address (typically right after a DHCP lease renewal), it carries some other router interface address, often a private one -- and the carrier drops such packets. Conntrack would normally expire it, but the proxy's keepalive refreshes the entry faster than the 30-second UDP timeout. Reconnecting does not help either: the same 5-tuple lands in the same poisoned entry. The link stays dead with a perfectly healthy server on the other end until the entry is removed by hand. An ephemeral port removes the whole problem: new port, new entry, NAT recomputed.

In `server` and `reverse` modes the variable has no effect for `random` and `auto`: the branch that copies the client's port lives only in the normal-mode handler, so no bind happens there either way. A fixed port number works in all modes.

```
AWG_SRC_PORT=random  # default, kernel-ephemeral port, new on every reconnect
AWG_SRC_PORT=auto    # copies the WG client port (the default before v1.2.7)
AWG_SRC_PORT=0       # same as auto
AWG_SRC_PORT=12345   # fixed port 12345
```

**`AWG_TIMEOUT`** -- how many seconds the server may stay silent in reply to a handshake before the proxy reconnects (re-resolve DNS + new socket + a fresh check of both address families).

What counts is not idleness but the server's silence while the client is alive: an established tunnel with no traffic is legitimately quiet, and a keepalive expects no answer, so neither triggers a reconnect on its own. The signal is an unanswered handshake init -- WireGuard always answers one when it can, so silence in reply means the path is gone, not the traffic. That is why a short value is safe and causes no false reconnects.

```
AWG_TIMEOUT=60    # default, 1 minute
AWG_TIMEOUT=30    # notice a drop sooner on unstable links
AWG_TIMEOUT=300   # 5 minutes, when reconnecting costs more than downtime
```

**`AWG_DNS_REFRESH`** -- background DNS re-check interval in seconds when `AWG_REMOTE` is a hostname (disabled for a literal IP, v4 or v6). The proxy periodically re-resolves the hostname and, if the current server address has disappeared from the records on two consecutive checks (round-robin DNS protection), reconnects to the new address without waiting for `AWG_TIMEOUT`. The lookup runs on `AF_UNSPEC`, and a record only counts as a match when both its family and its address bytes match — otherwise an A record would mask a vanished AAAA on a dual-stack name. Granularity is 5 seconds. The reconnect resets the client session (same as on timeout) -- WireGuard performs a new handshake on its own.

```
AWG_DNS_REFRESH=60   # default, check once a minute
AWG_DNS_REFRESH=0    # disable the background DNS check
```

**`AWG_HE_DELAY`** -- IPv4 head start in milliseconds before IPv6 is probed (Happy Eyeballs, RFC 8305). Read only when `AWG_REMOTE` is a name carrying both an A and an AAAA record; in any single-stack configuration the variable is never consulted. See the [IPv6](protocol.md#ipv6) section below.

```
AWG_HE_DELAY=250   # default, as in RFC 8305
AWG_HE_DELAY=0     # duplicate the first packet over IPv6 immediately
```

**`AWG_STATE_FILE`** -- where to keep the learned address-family preference (one byte, `6` or `4`). It is written at most once per process run and only when the value changes, so as not to wear out the router's flash. The value `4` does not turn IPv6 off; it only brings back the standard order with the probe. See the [IPv6](protocol.md#ipv6) section.

```
AWG_STATE_FILE=/etc/awg-proxy.state   # default
AWG_STATE_FILE=                       # do not remember at all
```

**`AWG_LOG_LEVEL`** -- logging level. Controls the verbosity of output in `/container/print` and the router's syslog.

- `none` -- no output (for production on low-power devices)
- `error` -- errors only (bind/connect failed, reconnect)
- `info` -- startup config, client connections, reconnects (default)
- `debug` -- packet tracing: handshake init, junk sending, GRO segments, send errors. Needed for diagnosing handshake problems

```
AWG_LOG_LEVEL=info    # default
AWG_LOG_LEVEL=debug   # full tracing for debugging
AWG_LOG_LEVEL=error   # errors only
AWG_LOG_LEVEL=none    # silence
```

**`AWG_NO_GRO`** -- disables UDP GRO (Generic Receive Offload). GRO coalesces multiple incoming UDP packets into a single buffer, reducing the number of system calls; the proxy splits that buffer back apart using the segment size from the `UDP_GRO` cmsg. Enabled by default in `normal` mode only, on both sockets (receive from the client and receive from the server); it is not used in `reverse` and `server` modes. If the kernel coalesces nothing eight times in a row, the proxy clears the socket option itself and falls back to `recvmmsg`. Set `AWG_NO_GRO=1` to force disable.

```
AWG_NO_GRO=0   # default, GRO enabled (if kernel supports it)
AWG_NO_GRO=1   # force disable GRO, use recvmmsg instead
```

**`AWG_NO_GSO`** -- disables UDP GSO (Generic Segmentation Offload) on send. It mirrors GRO: the proxy hands the kernel a run of consecutive same-size packets in one `sendmsg` with a `UDP_SEGMENT` cmsg, and the kernel slices the buffer into datagrams itself. One syscall instead of dozens; on real traffic a run comes out 15--22 packets long. It works in both directions and in every mode, including `server` (a run ends when the destination changes). If the kernel has no `UDP_SEGMENT`, the proxy logs `GSO disabled by kernel: ...` once and falls back to plain `sendmmsg`. Turning it off is only worth it to measure the same build with and without the offload.

How well the offload did is visible in the `gso:` line of the periodic statistics (`AWG_STATS`): `run` is the average run length, and `1.0` means there was nothing to coalesce.

```
AWG_NO_GSO=0   # default, GSO enabled (if kernel supports it)
AWG_NO_GSO=1   # force disable, every batch goes out via sendmmsg
```

**`AWG_NO_DF`** -- clears the DF (Don't Fragment) bit on the proxy's outgoing UDP packets (`IP_MTU_DISCOVER=IP_PMTUDISC_DONT` on both sockets). Linux sends UDP with DF=1 by default (Path MTU Discovery); there are reports that some DPI nodes on certain networks handle DF=1 UDP worse than DF=0. This option changes the on-wire IP header, so it is off by default -- enable it only when experiencing connectivity issues: with DF=0 large packets may be fragmented along the path. On an IPv6 socket `IPV6_MTU_DISCOVER=IPV6_PMTUDISC_DONT` is used instead, but IPv6 has no DF bit — there it only changes how the local stack reacts to PMTU (see the [IPv6](protocol.md#ipv6) section).

```
AWG_NO_DF=0   # default, DF bit as set by the system (usually DF=1)
AWG_NO_DF=1   # clear the DF bit on the proxy's UDP packets
```

**`AWG_RT` and `AWG_RPS`** -- two levers against loss caused by the scheduler rather than by a full buffer. Both work **only in a privileged container** (on RouterOS, `/container set ... privileged=yes`, available since 7.24): the kernel checks the capability against the init namespace, so an ordinary container gets `EPERM` from `sched_setscheduler` and cannot write to `/sys/class/net`.

`AWG_RT` puts the I/O threads into the `SCHED_RR` class at the given priority (1..50). Reach for it when drops arrive in a burst inside one window instead of being spread out: that is the signature of a reader thread not getting the CPU for tens of milliseconds, and no buffer size fixes it. It is deliberately incompatible with `AWG_SPIN` -- spinning under real time would starve the softirq that does the router's WG crypto; if both are set, `AWG_RT` is ignored with a log line.

`AWG_RPS` writes a core mask into `rps_cpus` of the container interface's receive queue, so the kernel spreads receive processing over those CPUs instead of one. The mask is hexadecimal, as the kernel expects: `9` = cores 0 and 3, `e` = cores 1--3.

Keeping the two on different cores is essential. On a hAP ax(2), with threads pinned by `AWG_CPU_C2S=1` and `AWG_CPU_S2C=2`, the mask `e` (cores 1--3) overlaps the threads themselves: real time preempts the softirq exactly where RPS puts it, and the gain is eaten. The mask `9` (cores 0 and 3) separates them and gave the best result of every combination tested.

```
AWG_RT=0            # default, ordinary scheduling class
AWG_RT=10           # SCHED_RR priority 10
AWG_RPS=9           # RPS on cores 0 and 3
AWG_CPU_C2S=1       # threads on cores 1 and 2 -- no overlap with the RPS mask
AWG_CPU_S2C=2
```

At startup the proxy reports what it got: `c2s: realtime SCHED_RR prio 10` or `c2s: realtime refused (...)`, and `rps: <interface> mask applied`. If the lines are missing, check that the container is privileged and that the log level lets INFO through.

**The set `AWG_CPU_*` + `AWG_RT` + `AWG_RPS` is for a 64-bit router with four cores or more** (hAP ax², ax³), and the container checks that by itself. With fewer than four cores (counting the container's `cpu-list`) or a 32-bit CPU the whole set is dropped with one log line (`perf: 2 cores - ... skipped` or `perf: 32-bit CPU - ... skipped`). Why: on two cores real-time threads take the CPU from WireGuard's crypto, and on a four-core 32-bit hAP ac² a thread pinned to a core and raised to real time keeps WireGuard's worker off that core -- upload fell from 100 to 34 Mbit/s with ping at 454 ms. Along the way a pin to a core that does not exist (or was not given to the container) is skipped, and the cores of pinned threads are taken out of `AWG_RPS`.

**`AWG_SOCKET_BUF`** -- receive/send buffer sizes (SO_RCVBUF/SO_SNDBUF) for UDP sockets in bytes. The kernel doubles the requested value.

**Unset (the default), the size depends on the router.** On a 64-bit router with four cores or more (hAP ax², ax³) it is a fixed 32 MB, as in 1.4.0: there the proxy is not the bottleneck, the buffer only absorbs bursts, and bigger is better (a rate-sized one brought thousands of drops back on the ax² and cost it up to 5 % of its upload). **On a weaker router -- fewer cores, or a 32-bit arm -- each socket's receive buffer sizes itself.** Every 5 seconds the proxy looks at how many packets went through the socket and keeps the buffer at about 50 ms of traffic at the peak rate: no smaller than the kernel's old 416 KB, no larger than 32 MB. The peak rises at once and falls off gradually, over tens of seconds, so a short lull does not take away the room the next burst needs. Why: a buffer is also a queue. A privileged container may have a buffer of any size, and on a weak router where the proxy itself is the bottleneck a huge one does not drop packets -- it holds them for hundreds of milliseconds, so TCP sees no loss, only a growing delay, runs into its window and slows down several times over. 50 ms is enough to ride out the thread being preempted, and no standing queue grows longer than that. Without privileges the kernel caps the buffer at `rmem_max` anyway, and the sizing stays off. With `AWG_STATS` the log gets a `rcvbuf KB: listen=… remote=…` line whenever the size moved.

Set, the buffer is fixed, as in 1.4.0 and before.

```
# unset                  # default: ax²/ax³ -- 32 MB; weaker -- receive buffer follows the traffic
AWG_SOCKET_BUF=16777216  # fixed 16 MB (32 MB once doubled) -- the 1.4.0 behaviour
AWG_SOCKET_BUF=1048576   # fixed 1 MB
```

### Optional -- Performance

These parameters are only relevant on powerful devices with multiple CPU cores. On typical MikroTik routers (1-2 cores), leave the defaults.

**`AWG_CPU_C2S`**, **`AWG_CPU_S2C`** -- thread-to-CPU pinning (CPU affinity). The proxy uses two threads: c2s (client→server, outbound packet processing) and s2c (server→client, inbound). Pinning to different cores prevents thread migration and improves cache efficiency.

```
AWG_CPU_C2S=-1   # default, OS chooses the core
AWG_CPU_S2C=-1

AWG_CPU_C2S=0    # c2s on core 0
AWG_CPU_S2C=1    # s2c on core 1
```

**`AWG_BUSY_POLL`** -- enables SO_BUSY_POLL on sockets. The kernel actively polls the network driver for the specified time (in microseconds) instead of going to sleep. Reduces latency by ~50 μs but increases CPU usage. Requires network driver support.

**RouterOS does not have this knob at all:** its kernel is built without `CONFIG_NET_RX_BUSY_POLL` and `setsockopt` answers `ENOPROTOOPT`. The proxy reports the outcome at startup so that a refusal does not read like a working setting:

```
INFO: busy-poll: kernel 5.6.3, SO_BUSY_POLL=Protocol not available prefer=no budget=no
```

```
AWG_BUSY_POLL=0     # default, disabled
AWG_BUSY_POLL=50    # 50 μs of active polling
AWG_BUSY_POLL=100   # 100 μs, for minimum latency
```

**`AWG_SPIN`** -- the same idea done by the proxy itself, asking the kernel for no privilege. After a read comes up empty the thread keeps re-reading the socket without blocking for the given number of microseconds before letting itself be put to sleep, so a burst arriving inside that window is picked up with no wakeup in the path at all. Works where `SO_BUSY_POLL` is unavailable.

`auto` turns on self-tuning: the controller walks a 0/50/100/200/400/800 μs ladder, scores each value by the share of packets dropped for want of socket buffer and re-checks the others now and then. Idle windows do not vote; ties go to the smaller value.

**On a router that is CPU-bound, leave it at 0.** Spinning takes cycles from the softirq doing WireGuard's crypto, and that costs throughput: on a hAP ax3 a speedtest to Amsterdam gave 155-192 Mbit/s with spin and 194-206 without it. It is worth enabling where CPU is plentiful and packets are being lost.

```
AWG_SPIN=0      # default, disabled
AWG_SPIN=200    # 200 μs of non-blocking re-reads before sleeping
AWG_SPIN=auto   # pick the value from the drop rate
```

**`AWG_STATS`** -- seconds between stat lines (`0` -- off). Printed only when something moved, so an idle tunnel keeps the log clean:

```
stats: c2s rx=173791 tx=173791 drop=0 | s2c tx=168402 drop=0 |
       kernel udp in_err=1952 rcvbuf_err=1952 sndbuf_err=0 |
       sockdrop listen=706 remote=233 | spin=0
```

The line separates three kinds of loss: `drop` -- ours (the packet arrived but could not be sent), `kernel udp rcvbuf_err` -- the kernel discarded it before we got there, having run out of receive buffer, and `sockdrop` -- which socket actually overflowed. The last one matters more than it looks: during an upload the data goes into the listen socket and only ACKs come back, so that pair of numbers says immediately which direction is failing.

A line of its own follows with the segmentation offload's report (see [`AWG_NO_GSO`](#optional----network-and-diagnostics)); it is printed only when the offload fired at least once during the period:

```
gso: c2s msgs=3104 segs=68443 run=22.0 | s2c msgs=4723 segs=95671 run=20.2
```

`msgs` is how many `sendmsg` calls carried a coalesced run, `segs` how many packets those runs held, and `run` the average per call. `run=1.0` means there was nothing to coalesce (mixed sizes); no line at all means the offload never fired. It is a separate line rather than more fields on `stats:` because RouterOS truncates a container's log message at 179 characters including the container name, and `stats:` already sits on that edge.
