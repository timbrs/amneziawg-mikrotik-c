# Installing awg-proxy on MikroTik

> Part of the [awg-proxy](../../README_en.md) documentation: a RouterOS container that connects MikroTik to AmneziaWG servers. The easiest way to install it is the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), which generates all commands. Other pages: [install](install.md) · [updating](update.md) · [variables](env.md) · [AWG 3.x and IPv6](protocol.md) · [routing and MTU](routing.md) · [server mode](server-mode.md) · [troubleshooting](troubleshooting.md) · [FAQ](faq.md).

## Getting AWG Parameters

1. Open the **AmneziaVPN** application
2. Select the desired connection
3. Tap **Share**
4. Choose: **Protocol**: AmneziaWG, **Format**: AmneziaWG Format
5. Save the `.conf` file

The obfuscation parameters (`Jc`, `Jmin`, `Jmax`, `S1`, `S2`, `H1`--`H4`) are in the `[Interface]` section, while `Endpoint` and `PublicKey` are in the `[Peer]` section.

**If some of them are missing** -- as in AWG configs for Cloudflare WARP, which carry only `Jc`/`Jmin`/`Jmax` (sometimes `I1` too) -- the configurator takes the AmneziaWG defaults, exactly as the AmneziaWG apps do: `S1 = S2 = 0`, `H1`--`H4` = `1`--`4` (the plain WireGuard message types), a missing `Jc`/`Jmin`/`Jmax` = `0`. Filled-in fields are marked as defaults, with a note below. For WARP this is the working setup: Cloudflare speaks plain WireGuard, and what gets the handshake past DPI are the junk and CPS packets the proxy sends before it. If a config has no obfuscation at all and is not WARP, the configurator says it is plain WireGuard and offers a button that adds junk packets (`Jc = 4`, `Jmin = 40`, `Jmax = 70`) -- a plain WireGuard server simply drops them.

### No AmneziaVPN: a Cloudflare WARP config

The "Deployment mode" list has **"No AmneziaVPN — Cloudflare WARP"**. It shows where to get a config: open the [WARP config generator](https://lanrat.github.io/wireguard-warp-generator/) (or the [wgcf](https://github.com/ViRb3/wgcf) command-line tool), press "Generate WARP Config", copy the config and paste it into the field. What gets installed is the same as in the "Container" mode. The configurator itself contacts nothing.

A WARP config pasted in the "Container" mode switches to the WARP mode by itself — the configurator recognises it by Cloudflare's server key or by `engage.cloudflareclient.com`. The WARP mode is a short form:

- **the extra settings are hidden** -- their defaults fit WARP; "Show all settings" brings them back;
- **the container DNS is asked for only when it is needed**: the endpoint is a name and the config has no public DNS (usually it has `1.1.1.1`, which is taken by itself);
- **"Random WARP port" is ticked**: Cloudflare takes WireGuard on ports 2408, 500, 1701 and 4500, and all of them go into `AWG_REMOTE` (the config's own port first). The proxy starts on a random port, holds on to the one that answered (and remembers it across restarts), and tries another only once that one has been silent for ~15 s. A `# AllowedPorts = ...` line in the config takes precedence over the box;
- **the "Make the packet before the handshake look like Chrome QUIC" button** writes an `I1` into the config with the header of an ordinary QUIC v1 Initial as Chrome sends it to Cloudflare (a 1250-byte datagram, random 8-byte DCID, empty SCID), and `Jc = 0`, so no small junk packets go in front of it. Cloudflare ignores the packet. The encrypted part is random bytes: the proxy has no keys to build a real TLS ClientHello, so an analyser that decrypts QUIC Initials will see there is no ClientHello inside. Ordinary QUIC goes to port 443, while WARP takes WireGuard on 2408/500/1701/4500 -- by its port the packet does not look like QUIC, and the configurator says so.

## Manual Installation

### 1. Enable Containers and Fetch

Install the container package from [mikrotik.com](https://mikrotik.com/download), upload it to the router, and reboot. Then:

```routeros
/system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
```

`fetch=yes` is required to download the image directly on the router via `/tool/fetch`. If you plan to upload the file manually via Winbox/SCP, `fetch=yes` is not required. `scheduler=yes` is required for RU list auto-updates (the "non-RU traffic through tunnel" scenario), `bandwidth-test=yes` -- for speed testing via `/tool/bandwidth-test`.

The router will ask for confirmation (button press or reboot, depending on the model).

### 2. Upload Image

Download `awg-proxy-{arch}.tar.gz` from [Releases](https://github.com/timbrs/amneziawg-mikrotik-c/releases) and upload it to the router via Winbox or SCP. For RouterOS 7.20 and below, use files with the `-7.20-Docker` suffix (Docker format).

> **hEX refresh (E50UG), hEX S 2025 (E60iUGS) and hAP ax S (E62iUGS):** despite `architecture-name: arm`, their CPU only executes arm32v5 images ([RouterOS limitation](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container)) — use `awg-proxy-armv5.tar.gz`, otherwise the container crashes with `exited with signal 4 (Illegal instruction)`. The configurator detects these devices automatically.

Or download directly on the router (replace URL with the actual one):

```routeros
/tool/fetch url="https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-arm64.tar.gz" dst-path=awg-proxy-arm64.tar.gz
```

**RouterOS 7.22+: straight from the registry, no file to download.** The image is published
to GHCR, the architecture is picked automatically, and a container installed this way can be
updated with a single command later (see [Updating](update.md#updating)):

```routeros
/container/add remote-image=ghcr.io/timbrs/awg-proxy:latest ...
```

No need to touch `/container/config`: the registry host is read straight from the string.
hEX refresh (E50UG), hEX S 2025 (E60iUGS) and hAP ax S (E62iUGS) take their own tag,
`ghcr.io/timbrs/awg-proxy:latest-armv5` — those CPUs only execute arm32v5, which cannot live
in the shared multi-arch index.

> On routers with 16 MB of flash, point `tmpdir` at the disk that holds the container: layers
> are downloaded there, not into root-dir — `/container/config set tmpdir=usb1/pull`.

### 3. Network Setup

```routeros
/interface/veth/add name=veth-awg-proxy address=172.18.0.2/30 gateway=172.18.0.1
/ip/address/add address=172.18.0.1/30 interface=veth-awg-proxy
/ip/firewall/nat/add chain=srcnat action=masquerade src-address=172.18.0.0/30
```

### 4. WireGuard

```routeros
/interface/wireguard/add name=wg-awg-proxy private-key="YOUR_PRIVATE_KEY" listen-port=12429
/interface/wireguard/peers/add interface=wg-awg-proxy public-key="SERVER_PUBLIC_KEY" \
    preshared-key="YOUR_PRESHARED_KEY" endpoint-address=172.18.0.2 endpoint-port=51820 \
    allowed-address=0.0.0.0/0 persistent-keepalive=25
/ip/address/add address=YOUR_TUNNEL_IP interface=wg-awg-proxy
/ip/firewall/nat/add chain=srcnat action=masquerade out-interface=wg-awg-proxy
```

Replace:
- `YOUR_PRIVATE_KEY` -- PrivateKey from `[Interface]`
- `SERVER_PUBLIC_KEY` -- PublicKey from `[Peer]`
- `YOUR_PRESHARED_KEY` -- PresharedKey from `[Peer]` (if any)
- `YOUR_TUNNEL_IP` -- Address from `[Interface]` (e.g., `10.8.0.2/32`)

### 5. Environment Variables

```routeros
/container/envs/add list=awg-proxy-env key=AWG_LISTEN value=":51820"
/container/envs/add list=awg-proxy-env key=AWG_REMOTE value="SERVER_IP:PORT"
/container/envs/add list=awg-proxy-env key=AWG_JC value="5"
/container/envs/add list=awg-proxy-env key=AWG_JMIN value="30"
/container/envs/add list=awg-proxy-env key=AWG_JMAX value="500"
/container/envs/add list=awg-proxy-env key=AWG_S1 value="20"
/container/envs/add list=awg-proxy-env key=AWG_S2 value="20"
/container/envs/add list=awg-proxy-env key=AWG_H1 value="1234567890"
/container/envs/add list=awg-proxy-env key=AWG_H2 value="1234567891"
/container/envs/add list=awg-proxy-env key=AWG_H3 value="1234567892"
/container/envs/add list=awg-proxy-env key=AWG_H4 value="1234567893"
/container/envs/add list=awg-proxy-env key=AWG_SERVER_PUB value="SERVER_PUBLIC_KEY"
/container/envs/add list=awg-proxy-env key=AWG_CLIENT_PUB value=[/interface/wireguard/get [find name=wg-awg-proxy] public-key]
```

Replace all values with parameters from your `.conf` file. `AWG_CLIENT_PUB` is read automatically from the WireGuard interface.

### 6. Create and Start Container

```routeros
/container/add file=awg-proxy-arm64.tar.gz interface=veth-awg-proxy envlist=awg-proxy-env \
    hostname=awg-proxy root-dir=disk1/awg-proxy logging=yes shm-size=4M start-on-boot=yes
/container/start [find where tag~"awg-proxy"]
```

Verify it works:

```routeros
/container/print
/interface/wireguard/peers/print
```

The container should show `running` status, and the peer should have a `last-handshake` value.

## Installing Without GitHub Access

If the router cannot reach GitHub, turn on **offline mode** in the configurator. The output gets a **step 0**: a script
that changes nothing and only prints exactly what to download and where to put it:

```
1. Container image
   file: awg-proxy-arm64.tar.gz
   url:  https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-arm64.tar.gz
   put it here: usb1/awg-proxy-arm64.tar.gz

2. RU address list
   file: ru_ranges_timeout.rsc
   url:  https://github.com/timbrs/ru-ranges/releases/latest/download/ru_ranges_timeout.rsc
   put it here: usb1/ru_ranges_timeout.rsc
```

The archive name cannot be known in advance: it depends on the architecture, the RouterOS version
(7.20 and older need the `-7.20-Docker` build) and — on arm — the board model (hEX, E50UG,
E60iUGS and E62iUGS get `armv5`). That is why step 0 asks the router itself.

The order:

1. Run step 0 on the router and note the file names.
2. Download them somewhere GitHub is reachable.
3. Upload them to the router (Files, or drag them into WinBox) at the paths shown.
4. Run step 1, the install.

In offline mode the install does not touch the network at all: no `remote-image` from the
registry, no `/tool/fetch` of the release archive. If a file is not where it should be, the
script names the missing file and stops instead of failing inside `fetch`.
The RU list update also stops waiting a minute for `github.com` on every boot and on every
daily run, and takes either of the two files — `ru_ranges_timeout.rsc` or `ru_ranges.rsc`.

A file you put there by hand is never deleted by the script — it is yours. Only what the
script downloaded itself gets removed.

No daily update scheduler is created in offline mode: it fetches the release list from
`api.github.com`. The `<prefix>-update` script is still installed — run it by hand once the
router has internet access.
