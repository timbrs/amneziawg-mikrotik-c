# awg-proxy FAQ

> Part of the [awg-proxy](../../README_en.md) documentation: a RouterOS container that connects MikroTik to AmneziaWG servers. The easiest way to install it is the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), which generates all commands. Other pages: [install](install.md) · [updating](update.md) · [variables](env.md) · [AWG 3.x and IPv6](protocol.md) · [routing and MTU](routing.md) · [server mode](server-mode.md) · [troubleshooting](troubleshooting.md).

The questions come from user discussions and are worded the way people usually ask them. Answers are
current for awg-proxy 1.4.5 (October 2026).

## Is it for me

### Will it run on my router (hAP ac², hEX, hAP ax lite…)?

Check `architecture-name` in `/system/resource/print`:

- `arm64` — yes. hAP ax², hAP ax³, RB5009, CCR2004/2116/2216: the fastest option.
- `arm` — yes. hAP ac², hAP ac³, RB4011, RB3011. The exception is hEX refresh (E50UG), hEX S 2025 (E60iUGS) and
  hAP ax S (E62iUGS): their CPU runs ARMv5 code only, so they need the separate `armv5` image. The configurator
  picks it automatically; the regular arm image dies there with `signal 4 (Illegal instruction)`.
- `x86_64` — yes (CHR, x86).
- `mipsbe`, `mmips`, `smips` — **no**. That is the old hEX (RB750Gr3), hEX S (RB760iGS), hAP ac lite, hAP ac,
  hAP lite, RB2011, mAP. RouterOS has no container package for MIPS, and there is no way around it.
- `tile` (CCR1009/1016/1036/1072) — no, for the same reason.

The full table is in the [README](../../README_en.md#will-it-run-on-my-router).

### Can I run AmneziaWG on MikroTik without a container?

No. RouterOS only has plain WireGuard. awg-proxy exists so that the router's own WireGuard can talk to an
AmneziaWG server, and it needs the `container` package.

### Which RouterOS version do I need?

7.4 or newer with the `container` package. RouterOS 7.21+ takes the OCI image `awg-proxy-{arch}.tar.gz`,
7.20 and older take `awg-proxy-{arch}-7.20-Docker.tar.gz`. On 7.22+ the container installs straight from the
registry and updates with one command. On 7.23+ RouterOS restarts a crashed container itself: the
configurator sets `restart-policy=on-failure`. RouterOS 6 is not supported.

### Is there enough memory? I have 128 MB RAM / 16 MB flash

The image is about 90 KB and the proxy needs about 16 MB of free RAM (it takes 1–2 MB itself), so awg-proxy runs on a hAP ac²
(128 MB RAM), where heavy containers such as mihomo no longer fit.

Flash is the harder part. A 16 MB-flash router (hAP ac²) usually has no room to unpack image layers. Plug in a
USB stick, format it as ext4 and pick it in the configurator's "Container storage" field — the configurator
moves `tmpdir` there. Details: [troubleshooting](troubleshooting.md#child-spawn-failed--could-not-load-next-layer).

On a hAP ax² (128 MB flash, no USB) awg-proxy fits in internal storage. If RouterOS still reports
`no space to extract layer`, unpack into RAM for the install, see
[troubleshooting](troubleshooting.md#no-space-to-extract-layer).

### Do I need a public IP?

Not for the normal mode, where the router is a client of an AmneziaWG server. A public IP or IPv6 is needed
only on the side others connect to: the server router in site-to-site, or the hub in server mode.

### I have no AmneziaVPN. What do I paste into the configurator?

Any AmneziaWG `.conf` works: from the AmneziaVPN app (Share → AmneziaWG Format), from a bot or from your
provider. No server of your own? Pick the **"No AmneziaVPN — Cloudflare WARP"** mode: it links to a free WARP
config generator. Details: [install](install.md#getting-awg-parameters).

### Which AmneziaWG versions are supported?

1.0, 1.5, 2.0, 3.0 and 3.1. The version is detected from the config: `HeaderProtectionKey` means 3.0,
`RandomTrailers` or `DisableCookies` means 3.1. There is no need to move to a newer version for its own sake:
use whatever your server speaks. Details: [AWG 3.x](protocol.md).

### Is it safe to paste a config with a private key into the configurator?

Yes. The configurator is a single HTML page that works in the browser without network access. Keys and
parameters go nowhere. You can save the page and open it from disk.

## How it compares

### How is awg-proxy different from the wiktorbgu containers?

They solve the same task in different ways. According to the image descriptions on Docker Hub:

- **wiktorbgu/amneziawg-mikrotik** is a full AmneziaWG (client and server) inside the container, an image of
  about 38 MB. The container does the encryption itself, its config is an `awg.conf` file in a mounted folder,
  and traffic is routed through the container.
- **wiktorbgu/amneziawg-proxy-mikrotik** repackages awg-proxy for Docker Hub: the `latest` tag is the old Go
  version, `latest-c` is this C version. The environment variables are the same.
- **awg-proxy (this repository)** lets the native RouterOS WireGuard do the encryption; the container only
  changes how packets look. The WireGuard interface is a regular RouterOS interface: routes, netwatch,
  firewall and mangle work as usual. Images live in GHCR (`ghcr.io/timbrs/awg-proxy`) and in GitHub releases,
  no Docker Hub needed. The configurator installs the container together with routing, auto-update and a
  failover route.

Do not mix configs: commands from this configurator target awg-proxy, while wiktorbgu/amneziawg-mikrotik
expects its own `awg.conf`.

### Why awg-proxy if there is mihomo?

mihomo is a multi-protocol proxy router (VLESS, Hysteria2, subscriptions, domain rules). It is heavier: tens of
megabytes of RAM, and on weak routers the OOM killer takes it down. awg-proxy does one thing, AmneziaWG, in
about 90 KB. Many people run both: mihomo for VLESS and AmneziaWG through awg-proxy, because mihomo's built-in
AmneziaWG reportedly stutters on video.

### Is the old timbrs/amneziawg-mikrotik (Go) repository still current?

No. The Go version is no longer developed and does not support AmneziaWG 3.x. Use this repository and the
[new configurator](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html). The environment variables are
compatible: for a manual install, recreating the container with the new image is enough.

### Is there a version for Keenetic, OpenWrt or Windows?

No, awg-proxy is a RouterOS container. Keenetic and OpenWrt have their own AmneziaWG. For server mode on a
VPS there are Linux binaries in the [releases](https://github.com/timbrs/amneziawg-mikrotik-c/releases)
(`awg-proxy-linux-amd64` and others). Only the old Go version had a Windows build.

## Speed

### What speed should I expect?

The router CPU sets the ceiling, because the router itself does the WireGuard crypto; the proxy is almost
never the bottleneck. Rough figures from the author's measurements (they depend on the link and the server):

- hAP ax²: about 200–250 Mbit/s with `privileged=yes` and the `AWG_RT`/`AWG_RPS` tuning (see [variables](env.md#optional----network-and-diagnostics));
- hAP ax³: about 190–200 Mbit/s;
- hAP ac²: about 100 Mbit/s.

One tunnel is one flow, and WireGuard handles it on one or two cores. Once the CPU sits at 100 %, it will not
go faster. Turning off unneeded services and fasttracking traffic that bypasses the tunnel helps.

### Why is it slower than the AmneziaVPN app on my PC?

The app encrypts on the PC's powerful CPU; through awg-proxy the router's CPU encrypts. Compare over a cable:
Wi-Fi badly understates upload speed.

### Sites load every other time, YouTube stalls, but ping works

It is almost always MTU. The typical case is PPPoE: the link carries 1492 instead of 1500, so every full packet
goes out as two fragments. Set "Underlay path MTU" in the configurator (1492 for PPPoE). The proxy also logs
the value to use: `WARN: set the WireGuard interface MTU to … or lower`. Details:
[MTU](routing.md#mtu-on-narrow-links-pppoe-lte).

## When it does not work

### No handshake (`Handshake for peer did not complete`)

First find out where the problem is:

1. **Try the same `.conf` in AmneziaVPN on a phone on the same network** (over this router's Wi-Fi). If it
   fails there too, the server or its IP is the problem, not the MikroTik.
2. Is the container running? `/container/print` should say `running`. Logs: `/log/print where topics~"container"`.
3. `Jc`, `Jmin`, `Jmax`, `S1`–`S4`, `H1`–`H4`, `I1`–`I5` and the keys must match the server.
4. Is the endpoint a hostname? The container needs DNS (`AWG_DNS`); the configurator asks for it separately.
5. Restored the router from a backup? Check the clock: with a clock behind real time the server silently
   drops handshakes.

The full walkthrough is in [troubleshooting](troubleshooting.md).

### It worked, then stopped after a couple of days: handshake yes, data no

That is usually the server IP being blocked, not a broken router: the blocker lets the handshake through and
cuts the data. Try the same config from a phone. If it fails there too, you need another IP or server. If the
phone works and the router does not, look at MTU and the container logs.

### I picked individual services in the configurator and they don't work; only "all non-RU traffic" works

Per-service routing is built on DNS: the router sees the address a DNS reply returned and puts it into an
address-list. If a device asks someone other than the router (DoH in the browser, Android Private DNS, a
hard-coded 8.8.8.8), the router never sees the query and the traffic bypasses the tunnel. Some services
(Telegram) connect by IP without DNS at all. How to check: [limits of DNS routing](routing.md#dns-routing-what-it-can-and-cannot-do).

### Will it help against "whitelists" on mobile internet?

No. Under a whitelist the operator lets through approved addresses only, and any VPN to an unlisted server
will not connect however it is disguised.

### The log says `GRO not coalescing, falling back to recvmmsg`

That is not an error. The kernel is not merging incoming packets, so the proxy switches to plain receive. The
tunnel is unaffected. `AWG_NO_GRO=1` removes the line.

### The log says `rotate: ... moving to port`. What is that?

The proxy changed the server port on the fly (`AWG_PORT_ROTATE`). It only happens when `AWG_REMOTE` names a list or a range of ports: every 30–120 minutes the port moves to another one of the list, so the tunnel does not sit on the same port for days. The link does not drop: until the new port answers, every packet goes down both. Then the log says one of two things:

- `port N answered, staying (12 ms, 40 sent twice)` — the new port answered, the proxy stays on it;
- `port N stayed silent, back to M` or `port N refused, back to M` — the new port did not answer within 3 seconds; the proxy went back to the old one and will try later. If that happens every time, the server does not take the whole range: check it with [ports.md](ports.md). If the server is a MikroTik hub, it needs awg-proxy 1.4.4 or newer.

To switch it off: `AWG_PORT_ROTATE=0`. Cloudflare WARP never moves: Cloudflare keeps a session on the port it started on, and the proxy recognises WARP by its server key on its own (since 1.4.5; before that only the configurator switched it off).

### Can the proxy take several WARP addresses?

Yes, since 1.4.5. A scanner (for example, [warpscout](https://github.com/niklzz/warpscout-tg)) finds Cloudflare addresses and ports that work. List the ports in `AWG_REMOTE`, comma-separated, and tie the addresses to the name in `AWG_REMOTE` in one of two ways:

- **the container's `hosts` field** (WinBox: Container → the container → Hosts) -- a line per address: `engage.cloudflareclient.com` = `162.159.192.50`, and so on. It affects this container only;
- **static DNS records on the router**, when the container takes its DNS from the router (the container's DNS field = the router's address on its veth, port 53 open to the container):
  ```
  /ip dns static add name=engage.cloudflareclient.com type=A address=162.159.192.50
  /ip dns static add name=engage.cloudflareclient.com type=A address=188.114.96.203
  ```

If that name is needed elsewhere, make up your own (`my.warp.lan`) and put it into `AWG_REMOTE`. The proxy starts on the first address and a random port, holds on to the one that answered and remembers it in the state file. Should it go silent, 15 seconds later the proxy tries the next address and another port. With WARP the port never moves mid-session: Cloudflare keeps a session on the port it began on.

### How do I see which version is installed?

At start the proxy logs `awg-proxy <version> linux/c …`: `/log/print where message~"awg-proxy"`. A configurator
install also keeps the version in the `AWG_IMAGE_VER` variable of the container's env list.

### Docker Hub does not download / GitHub is blocked

awg-proxy does not need Docker Hub: the image is in GHCR (`ghcr.io/timbrs/awg-proxy`) and in GitHub releases.
If the router cannot reach GitHub either, turn on the configurator's **offline mode**: it prints which files to
download elsewhere and where to put them on the router. Details:
[installing without GitHub access](install.md#installing-without-github-access).

### How do I update the container?

`/system/script/run awg-proxy-1-update` (the name depends on the install prefix). The script compares the
version with the latest release and touches nothing when they match. A scheduler runs it nightly unless you
unticked auto-update during install. It works for registry installs (RouterOS 7.22+). Details:
[updating](update.md).

### After upgrading RouterOS to 7.25 the container no longer updates

This happens if awg-proxy was installed with a configurator older than 1.4.3 (before 8 October 2026).
The `<prefix>-update` script from that version cannot read a RouterOS version without a third number:
from "7.25" it takes only the 2, decides RouterOS is too old and quits. The container keeps running,
but the nightly update and the re-pull self-healing stop working, and while the container is alive
nothing about it reaches the log.
Betas such as 7.26beta3 hit the same problem.

To check: `/system/script/run awg-proxy-1-update` prints
`/container/repull needs RouterOS 7.22+, this router runs 7.25`. Or count the scripts with the old
version parsing, `0` means you are fine:

```routeros
:put [:len [/system script find where source~"endPos"]]
```

No reinstall is needed. Paste this whole block into the router terminal: it replaces the four
version-parsing lines in those scripts with the configurator's current ones and leaves the rest of the
script alone.

```routeros
:foreach i in=[/system script find where source~"endPos"] do={
  :local s [/system script get $i source]
  :local k1 ":local rest [:pick \$ver (\$dotPos + 1) [:len \$ver]]"
  :local k2 ":local endPos [:find \$rest \".\"]"
  :local k4 ":local minor [:tonum [:pick \$rest 0 \$endPos]]"
  :for n from=1 to=5 do={
    :local a [:find $s $k1]
    :if ([:typeof $a] != "nil") do={
      :local b [:find $s $k2 $a]
      :local c [:find $s $k4 $a]
      :if ([:typeof $b] != "nil" && [:typeof $c] != "nil") do={
        :local sep [:pick $s ($a + [:len $k1]) $b]
        :local new (":local rest ([:pick \$ver (\$dotPos + 1) [:len \$ver]] . \" \")" . $sep . ":local minor 0" . $sep . ":local mi 0" . $sep . ":while ([:typeof [:find \"0123456789\" [:pick \$rest \$mi (\$mi + 1)]]] != \"nil\") do={ :set minor (\$minor * 10 + [:tonum [:pick \$rest \$mi (\$mi + 1)]]); :set mi (\$mi + 1) }")
        :set s ([:pick $s 0 $a] . $new . [:pick $s ($c + [:len $k4]) [:len $s]])
      }
    }
  }
  :if ([:typeof [:find $s "endPos"]] = "nil") do={
    /system script set $i source=$s
    :put ([/system script get $i name] . ": fixed")
  } else={
    :put ([/system script get $i name] . ": not changed")
  }
}
```

It prints `<name>: fixed` for every script it fixed. `not changed` means the expected lines were not in
that script, so it was left as it was. Running it again changes nothing. Right after the fix,
`/system/script/run awg-proxy-1-update` updates the container without waiting for the night. You can
also apply it before moving to 7.25: on 7.24 the fixed script works the same way.

### The configurator changed. Do I remove everything and install again?

Depends on what changed.

- **The proxy itself** (a new container release): reinstall nothing, run `<prefix>-update` as in the
  previous answer. It leaves settings and keys alone.
- **Routing** (services, exclusions, "all non-RU traffic"): its script can be run on top. Since 1.4.3 it
  removes its own earlier rules first and sets everything up again.
- **Everything else** (firewall, DNS, container parameters): uninstall first. The install script expects
  a clean router; run over an old install it adds everything a second time, hence the duplicates. Run
  `/system/script/run awg-proxy-1-uninstall` (it removes the routing rules too), then the new install
  and routing scripts with the same prefix. The keys come from your `.conf`, so nothing changes on the
  server. The tunnel is down for a minute or two, and the routing script downloads the `RU` list itself.

Already installed on top and got duplicates? The same `<prefix>-uninstall` removes everything with that
prefix, duplicates included. Install cleanly after it.

A server-mode hub is not reinstalled this way: the configurator would generate new keys for it, and every
client would have to be set up again.

### Can phones connect to a MikroTik hub?

Yes. In "Server (1:N hub)" mode the configurator also produces a `.conf` and a share link for the AmneziaWG and
AmneziaVPN apps. The hub's proxy takes the public keys of such direct clients from `AWG_CLIENT_PUBS`. Details:
[server mode](server-mode.md).
