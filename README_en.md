> This project is not affiliated with or endorsed by MikroTik / SIA Mikrotikls

# awg-proxy -- AmneziaWG for MikroTik

[![C11](https://img.shields.io/badge/C-11-blue)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[Русская версия](README.md) | [GitHub](https://github.com/timbrs/amneziawg-mikrotik-c)

Lightweight Docker container that allows MikroTik routers to connect to AmneziaWG servers. All traffic is encrypted by the router's native WireGuard client; the container only transforms the packet format.

## Table of Contents

- [How It Works](#how-it-works)
- [Quick Start (Configurator)](#quick-start-configurator)
- [Requirements](#requirements)
- [Manual Installation](#manual-installation)
- [Getting AWG Parameters](#getting-awg-parameters)
- [Server Mode (1:N) — Detailed Setup](#server-mode-1n--detailed-setup)
  - [Routing Between Clients](#routing-between-clients)
- [Additional Settings](#additional-settings)
- [Uninstallation](#uninstallation)
- [Troubleshooting](#troubleshooting)
  - [execvpe /awg-proxy: No such file or directory](#execvpe-awg-proxy-no-such-file-or-directory)
  - [Site-to-site: handshake did not complete](#site-to-site-handshake-did-not-complete)
  - [Storage device not found](#storage-device-not-found)
  - [Insufficient disk space](#insufficient-disk-space)
  - [not allowed by device-mode](#not-allowed-by-device-mode)
  - [child spawn failed / could not load next layer](#child-spawn-failed--could-not-load-next-layer)
  - [exited with signal 4 (Illegal instruction)](#exited-with-signal-4-illegal-instruction)
  - [RU address list not loading after reboot](#ru-address-list-not-loading-after-reboot)
  - [Handshake fails after backup restore](#handshake-fails-after-backup-restore)
- [Building from Source](#building-from-source)
- [License](#license)

## How It Works

### Normal Mode (default)

```
MikroTik WG client ──UDP──> [awg-proxy] ──UDP──> AmneziaWG server
   (encryption)          (transformation)          (obfuscation)
```

The proxy replaces packet headers, adds padding and junk packets so the AmneziaWG server accepts the traffic. Keys and data are not modified.

### Reverse Mode (site-to-site)

```
MikroTik1 WG ↔ [proxy1 normal] ──AWG──> [proxy2 reverse] ↔ MikroTik2 WG
```

Reverse proxy: accepts AWG traffic from a normal proxy, transforms it back to standard WireGuard, and forwards to a local WG server. Allows connecting two MikroTik routers via AWG without running a separate AWG server.

### Server Mode (hub, 1:N)

```
proxy1a (normal) ──AWG──┐
proxy1b (normal) ──AWG──┤──> [reverse-hub] ──WG──> WG server
proxy1c (normal) ──AWG──┘
```

Multi-client reverse proxy: accepts AWG connections from multiple normal proxies and routes responses from the WG server to the correct client via a built-in session table. Each peer uses ~16 bytes in the hash table.

Detailed setup instructions in [Server Mode (1:N) — Detailed Setup](#server-mode-1n--detailed-setup).

Compatible with AWG v1 and v2 -- the version is detected automatically based on the environment variables.

## Quick Start (Configurator)

0. Prepare your router:
   - Install the **container** package from [mikrotik.com](https://mikrotik.com/download) (System → Packages), upload it to the router and reboot
   - Enable device-mode:
     ```routeros
     /system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
     ```
     The router will ask for confirmation (press Reset/Mode button or wait for reboot)
1. Export a `.conf` file from AmneziaVPN (see [Getting AWG Parameters](#getting-awg-parameters))
2. Open the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/configurator.html)
3. Paste the `.conf` file contents
4. Copy the generated commands and run them in MikroTik terminal

Done. The configurator works offline; no data is sent to any server.

![Speed test on MikroTik AX3](https://github.com/user-attachments/assets/9fb34444-681b-4f34-8306-8f202f1b121d)

*Speed test on MikroTik AX3*

## Requirements

- An AmneziaWG server with known obfuscation parameters
- Configuration file `.conf` exported from AmneziaVPN
- MikroTik RouterOS 7.4+ with the **container** package
  - **RouterOS 7.21+**: standard images `awg-proxy-{arch}.tar.gz` (OCI format)
  - **RouterOS 7.20 and below**: images `awg-proxy-{arch}-7.20-Docker.tar.gz` (Docker format)
  - The configurator detects the version automatically
- Architecture: ARM64, ARM (v7), ARM (v5: hEX refresh / hEX S 2025), or x86_64 ([check your device](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container))
- At least 256 KB free disk space (or a USB drive)
- At least 16 MB free RAM

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

> **hEX refresh (E50UG) and hEX S 2025 (E60iUGS):** despite `architecture-name: arm`, the EN7562CT CPU only executes arm32v5 images ([RouterOS limitation](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container)) — use `awg-proxy-armv5.tar.gz`, otherwise the container crashes with `exited with signal 4 (Illegal instruction)`. The configurator detects these devices automatically.

Or download directly on the router (replace URL with the actual one):

```routeros
/tool/fetch url="https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-arm64.tar.gz" dst-path=awg-proxy-arm64.tar.gz
```

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

## Getting AWG Parameters

1. Open the **AmneziaVPN** application
2. Select the desired connection
3. Tap **Share**
4. Choose: **Protocol**: AmneziaWG, **Format**: AmneziaWG Format
5. Save the `.conf` file

The obfuscation parameters (`Jc`, `Jmin`, `Jmax`, `S1`, `S2`, `H1`--`H4`) are in the `[Interface]` section, while `Endpoint` and `PublicKey` are in the `[Peer]` section.

## Server Mode (1:N) — Detailed Setup

Server mode allows a single awg-proxy to serve multiple MikroTik clients simultaneously. It acts as a full AmneziaWG server, implemented via a WireGuard server + awg-proxy pair.

### Architecture

```
MikroTik1 + awg-proxy(normal) ──AWG──┐
MikroTik2 + awg-proxy(normal) ──AWG──┤──> VPS: [awg-proxy server :443] ──WG──> [WG server :51820]
MikroTik3 + awg-proxy(normal) ──AWG──┘
```

- **WireGuard server** — a standard WG server that receives WG traffic from awg-proxy. Each MikroTik client is a separate peer.
- **awg-proxy in `server` mode** — listens on a public port (e.g., 443/udp), accepts AWG traffic from client normal-proxies, converts it to standard WG, and forwards it to the local WG server. A session table routes responses back to the correct client.

### 1. Server Side Setup (VPS)

#### WireGuard Server

Install WireGuard on your VPS and create a configuration. Example `/etc/wireguard/wg0.conf`:

```ini
[Interface]
PrivateKey = <server_private_key>
Address = 10.0.0.1/24
ListenPort = 51820

# Client 1 (MikroTik1)
[Peer]
PublicKey = <client_1_public_key>
AllowedIPs = 10.0.0.2/32

# Client 2 (MikroTik2)
[Peer]
PublicKey = <client_2_public_key>
AllowedIPs = 10.0.0.3/32
```

```bash
wg-quick up wg0
```

#### awg-proxy in Server Mode

**Option A: Docker Compose** (recommended)

Create `docker-compose.yml`:

```yaml
services:
  awg-proxy:
    image: ghcr.io/timbrs/awg-proxy:latest
    container_name: awg-proxy-server
    restart: unless-stopped
    network_mode: host
    environment:
      AWG_MODE: server
      AWG_LISTEN: ":443"              # public port for AWG clients
      AWG_REMOTE: "127.0.0.1:51820"   # local WireGuard server
      AWG_JC: "4"
      AWG_JMIN: "50"
      AWG_JMAX: "1000"
      AWG_S1: "84"
      AWG_S2: "40"
      AWG_H1: "1263070671"
      AWG_H2: "1883150219"
      AWG_H3: "1505218884"
      AWG_H4: "1343091225"
      AWG_SERVER_PUB: "<wg_server_public_key>"
      AWG_CLIENT_PUB: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
      AWG_CLIENT_PUBS: "<client_public_key_1>,<client_public_key_2>"
      # or instead of the line above:
      # AWG_CLIENT_PUBS_FILE: "/etc/awg-proxy/client-pubs.txt"
      AWG_LOG_LEVEL: info
```

> Replace AWG_JC, AWG_S1, AWG_H1--H4 etc. with your values. These parameters must match on the server and all clients.

> In server mode, `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE` is the new explicit list of **real client public keys** required for direct AmneziaWG 2.0 clients. `AWG_CLIENT_PUB` remains a legacy single-peer / proxy-only fallback. The old `proxy → server → WG` placeholder setup still works because the normal client-side proxy rewrites inbound MAC1 one more time. A direct client does not have that extra rewrite, so a placeholder alone is no longer sufficient.

> If pulling the image from ghcr.io is not possible, build it locally: clone the repository, run `docker build -t awg-proxy .`, and set `image: awg-proxy` in the compose file.

```bash
docker compose up -d
```

**Option B: binary + systemd**

```bash
# Download the binary for your platform
wget https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-linux-amd64
chmod +x awg-proxy-linux-amd64

# Create a systemd service
cat > /etc/systemd/system/awg-proxy.service << 'EOF'
[Unit]
Description=AWG Proxy Server
After=network.target wg-quick@wg0.service

[Service]
Type=simple
Environment=AWG_MODE=server
Environment=AWG_LISTEN=:443
Environment=AWG_REMOTE=127.0.0.1:51820
Environment=AWG_JC=4
Environment=AWG_JMIN=50
Environment=AWG_JMAX=1000
Environment=AWG_S1=84
Environment=AWG_S2=40
Environment=AWG_H1=1263070671
Environment=AWG_H2=1883150219
Environment=AWG_H3=1505218884
Environment=AWG_H4=1343091225
Environment=AWG_SERVER_PUB=<wg_server_public_key>
Environment=AWG_CLIENT_PUB=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
Environment=AWG_CLIENT_PUBS=<client_public_key_1>,<client_public_key_2>
# Or instead of AWG_CLIENT_PUBS:
# Environment=AWG_CLIENT_PUBS_FILE=/etc/awg-proxy/client-pubs.txt
Environment=AWG_LOG_LEVEL=info
ExecStart=/usr/local/bin/awg-proxy-linux-amd64
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

cp awg-proxy-linux-amd64 /usr/local/bin/
systemctl daemon-reload
systemctl enable --now awg-proxy
```

### 2. Client Side Setup (MikroTik)

Each MikroTik client uses a standard awg-proxy in `normal` mode (default).

1. Create a `.conf` file for each client. In the `[Peer]` section:
   - `Endpoint` = YOUR_VPS_IP:443 (the AWG_LISTEN port on the server)
   - `PublicKey` = WG server public key

2. Open the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/configurator.html), paste the `.conf` and run the generated commands on MikroTik.

3. Obfuscation parameters (Jc, S1, S2, H1--H4) must **exactly match** the server parameters.

### 3. Adding a New Client

1. Generate a WireGuard key pair:
   ```bash
   wg genkey | tee client_private.key | wg pubkey > client_public.key
   ```

2. Add the peer to the WG server:
   ```bash
   wg set wg0 peer $(cat client_public.key) allowed-ips 10.0.0.X/32
   ```

3. Create a `.conf` file for the new client (based on a template, with a unique PrivateKey and Address).

4. Configure MikroTik via the configurator.

5. If this client should connect **directly as AmneziaWG 2.0**, add its real public key to `AWG_CLIENT_PUBS` (or to the file referenced by `AWG_CLIENT_PUBS_FILE`) on the server. For proxy-only clients, the old placeholder behavior via `AWG_CLIENT_PUB` still works.

> **Note:** you usually do **not** need to restart awg-proxy on the server when adding a new client — the session table updates automatically. But if you change the `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE` list itself, restart awg-proxy so it reloads the peer list.

### 4. Verification

On VPS:
```bash
# awg-proxy logs
docker logs awg-proxy-server
# or
journalctl -u awg-proxy -f

# Check WireGuard peers
wg show wg0
```

On MikroTik:
```routeros
# Check handshake
/interface/wireguard/peers/print where interface~"awg-proxy"
# Should show a recent handshake
```

### Routing Between Clients

In the star topology, each client only sees the server's LAN by default. Client 1 (192.168.8.0/24) **does not know** about client 2's network (192.168.11.0/24) and vice versa. Traffic between clients goes through the server, but routes must be added manually.

#### Diagram

```
Client 1 (192.168.8.0/24)                    Client 2 (192.168.11.0/24)
   WG: 10.10.10.2                               WG: 10.10.10.3
         \                                            /
          \_________ Server (192.168.1.0/24) ________/
                      WG: 10.10.10.1
```

Default routes only cover client ↔ server. There are **no** routes between clients.

#### How to Add

**Step 1. On the server** — add both client subnets to each WG peer's `allowed-address`:

```routeros
# View current peers
/interface/wireguard/peers/print where interface=wg-awg-server-1

# Add client 2 subnet to client 1 peer
/interface/wireguard/peers/set [find where comment=awg-server-1-client-1] \
    allowed-address=10.10.10.2/32,192.168.8.0/24,192.168.11.0/24

# Add client 1 subnet to client 2 peer
/interface/wireguard/peers/set [find where comment=awg-server-1-client-2] \
    allowed-address=10.10.10.3/32,192.168.11.0/24,192.168.8.0/24
```

**Step 2. On client 1** — add a route to client 2's network:

```routeros
/ip/route/add dst-address=192.168.11.0/24 gateway=wg-awg-server-1
```

**Step 3. On client 2** — add a route to client 1's network:

```routeros
/ip/route/add dst-address=192.168.8.0/24 gateway=wg-awg-server-1
```

#### Verification

From a device on client 1's network, ping a device on client 2's network:

```
ping 192.168.11.X
```

Traffic goes: client 1 → WG → server → WG → client 2. This is a double hop through the server — latency will be the sum of both tunnels.

#### Multiple Clients

For N clients:
- On the server: add **all other** client subnets to each peer's `allowed-address`
- On each client: add routes to **all other** client subnets

With 3+ clients, this is easier to automate with a script on the server.

## Additional Settings

### All Environment Variables

| Variable | Required | Default | Description |
|----------|:---:|:---:|-------------|
| `AWG_LISTEN` | Yes | -- | Listen address |
| `AWG_REMOTE` | Yes | -- | AWG server address |
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
| `AWG_I1`--`AWG_I5` | No | -- | CPS templates (v1.5/v2) |
| `AWG_MODE` | No | `normal` | Operating mode: `normal`, `reverse`, `server` |
| `AWG_FB_H1`--`AWG_FB_H4` | No | -- | Fallback (v1) obfuscation profile: message types. Enables fallback (`normal`/`reverse` only); requires `AWG_S4=0` |
| `AWG_FB_S1`, `AWG_FB_S2` | Yes** | -- | Init/response padding for the fallback profile |
| `AWG_FB_S3` | No | `0` | Cookie reply padding for the fallback profile |
| `AWG_FB_AFTER` | No | `20` | Seconds of remote silence before probing the other profile (initiator) |
| `AWG_SRC_PORT` | No | auto | Outgoing port to server: `auto` / number / `random` |
| `AWG_TIMEOUT` | No | `180` | Inactivity timeout (sec) |
| `AWG_DNS_REFRESH` | No | `60` | Background DNS re-check interval for a hostname in AWG_REMOTE (sec, `0` = off) |
| `AWG_LOG_LEVEL` | No | `info` | Log level |
| `AWG_NO_GRO` | No | `0` | Disable UDP GRO |
| `AWG_NO_DF` | No | `0` | Clear the DF bit on UDP packets (workaround for DPI dropping DF=1) |
| `AWG_SOCKET_BUF` | No | `16777216` | Socket buffer size |
| `AWG_CPU_C2S` | No | `-1` | CPU for client→server thread |
| `AWG_CPU_S2C` | No | `-1` | CPU for server→client thread |
| `AWG_BUSY_POLL` | No | `0` | SO_BUSY_POLL timeout (μs) |
| `AWG_DNS` | No | -- | DNS server for hostname resolution in AWG_REMOTE |

`*` In `server` mode, you must set either the legacy `AWG_CLIENT_PUB` fallback or the explicit direct-peer list via `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE`.

`**` Required only when the fallback profile is enabled (`AWG_FB_H1` is set).

The protocol version is detected automatically: **v2** if S3/S4 are set or H values are ranges, **v1.5** if CPS templates (I1-I5) are set, otherwise **v1**.

**Fallback profile (site-to-site backward compatibility / DPI resilience).** The primary profile (`AWG_S*`, `AWG_H*`, `AWG_I*`) may be v2, while `AWG_FB_*` defines a second, v1 profile (fixed H, no S3/S4/CPS). The initiator (`normal`) runs on the primary profile and, if the remote stays silent for longer than `AWG_FB_AFTER` seconds, rarely switches to the fallback and back until it finds a working one. The responder (`reverse`) accepts both profiles and replies with whichever one the handshake arrived on. For the site-to-site scenario the configurator generates identical primary+fallback profiles on both sides: if the primary obfuscation starts getting blocked, the tunnel automatically falls back to the secondary. Requires `AWG_S4=0`; not supported in `server` mode.

### Detailed Variable Descriptions

#### Required -- Obfuscation Parameters

All values are taken from the `.conf` file exported from AmneziaVPN (`[Interface]` and `[Peer]` sections). They must **exactly** match the server parameters, otherwise the handshake will fail.

**`AWG_LISTEN`** -- address and port where the proxy listens for UDP packets from the router's WireGuard client. Format: `address:port` or `:port` (listen on all interfaces).

```
AWG_LISTEN=:51820          # all interfaces, port 51820 (standard)
AWG_LISTEN=172.18.0.2:9000 # specific address and port
```

**`AWG_REMOTE`** -- address and port of the AWG server (`Endpoint` from `[Peer]`). Supports both IP addresses and domain names.

```
AWG_REMOTE=1.2.3.4:443            # IP + port
AWG_REMOTE=vpn.example.com:51820  # domain + port
```

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

#### Optional -- Protocol v2

**`AWG_S3`**, **`AWG_S4`** -- padding for cookie reply (S3) and transport data (S4). Introduced in AWG v2. If S3 > 0 or S4 > 0 are set, the proxy automatically switches to v2 mode.

```
AWG_S3=0    # default, no padding
AWG_S4=16   # +16 bytes to each transport data packet
```

**`AWG_I1`--`AWG_I5`** -- CPS templates (Constant Packet Size). Up to 5 templates for generating fixed-format packets before handshake. If set without S3/S4/H ranges, the proxy operates in v1.5 mode. Template format is described in the AWG documentation.

```
AWG_I1=b:48656c6c6f,r:10,t:4,c:4
```

#### Optional -- Operating Mode

**`AWG_MODE`** -- proxy operating mode. Determines the direction of packet transformation.

- `normal` (default) -- standard proxy: accepts WireGuard from the router, transforms to AWG, and sends to the AWG server.
- `reverse` -- reverse proxy (1:1 site-to-site): accepts AWG from another normal proxy, transforms back to WireGuard, and sends to a local WG server. Used in pair with a normal proxy on the other side.
- `server` -- reverse proxy hub (1:N): like reverse, but supports connections from multiple normal proxies simultaneously. Response routing from the WG server to the correct client is done via a session table using `sender_index`/`receiver_index` from WireGuard packets.

```
AWG_MODE=normal    # default
AWG_MODE=reverse   # reverse proxy, 1:1
AWG_MODE=server    # reverse proxy hub, 1:N
```

In `reverse` and `server` modes, `AWG_REMOTE` points to the WireGuard server (not the AWG server), while `AWG_LISTEN` accepts AWG traffic from normal proxies. Obfuscation parameters (H1--H4, S1--S4, JC, etc.) must match the normal proxy on the other side.

#### Optional -- Network and Diagnostics

**`AWG_SRC_PORT`** -- outgoing UDP port for the connection to the AWG server. By default (`auto`), the proxy uses the WireGuard client's port -- this is needed for correct NAT operation on the router. If a number is specified, a fixed port is used. The value `random` leaves the choice to the kernel: no bind, a fresh ephemeral port on every reconnect -- useful behind CGN/carrier NAT where reusing the old port+server pair after a drop may get stuck.

```
AWG_SRC_PORT=auto    # default, copies the WG client port
AWG_SRC_PORT=0       # same as auto
AWG_SRC_PORT=12345   # fixed port 12345
AWG_SRC_PORT=random  # kernel-ephemeral port, new on every reconnect
```

**`AWG_TIMEOUT`** -- inactivity timeout in seconds. If no packets are sent or received in either direction within this time, the proxy reconnects to the server (re-resolves DNS + new socket). Useful when the server's IP address changes behind DNS.

```
AWG_TIMEOUT=180   # default, 3 minutes
AWG_TIMEOUT=60    # aggressive timeout for unstable connections
AWG_TIMEOUT=3600  # 1 hour, for stable links
```

**`AWG_DNS_REFRESH`** -- background DNS re-check interval in seconds when `AWG_REMOTE` is a hostname (disabled for a literal IP). The proxy periodically re-resolves the hostname and, if the current server IP has disappeared from the A records on two consecutive checks (round-robin DNS protection), reconnects to the new address without waiting for `AWG_TIMEOUT`. Granularity is 5 seconds. The reconnect resets the client session (same as on timeout) -- WireGuard performs a new handshake on its own.

```
AWG_DNS_REFRESH=60   # default, check once a minute
AWG_DNS_REFRESH=0    # disable the background DNS check
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

**`AWG_NO_GRO`** -- disables UDP GRO (Generic Receive Offload) on the server socket. GRO coalesces multiple incoming UDP packets into a single buffer, reducing the number of system calls. Enabled by default if the kernel supports it. On some platforms (ARM64 on RouterOS) the kernel accepts the setsockopt call, but GRO doesn't actually work -- in this case the proxy hangs waiting for packets. Set `AWG_NO_GRO=1` to force disable.

```
AWG_NO_GRO=0   # default, GRO enabled (if kernel supports it)
AWG_NO_GRO=1   # force disable GRO, use recvmmsg instead
```

**`AWG_NO_DF`** -- clears the DF (Don't Fragment) bit on the proxy's outgoing UDP packets (`IP_MTU_DISCOVER=IP_PMTUDISC_DONT` on both sockets). Linux sends UDP with DF=1 by default (Path MTU Discovery); there are reports that some DPI nodes on certain networks handle DF=1 UDP worse than DF=0. This option changes the on-wire IP header, so it is off by default -- enable it only when experiencing connectivity issues: with DF=0 large packets may be fragmented along the path.

```
AWG_NO_DF=0   # default, DF bit as set by the system (usually DF=1)
AWG_NO_DF=1   # clear the DF bit on the proxy's UDP packets
```

**`AWG_SOCKET_BUF`** -- receive/send buffer sizes (SO_RCVBUF/SO_SNDBUF) for UDP sockets in bytes. The kernel typically doubles the requested value. Larger buffers reduce packet loss under load but consume more RAM.

```
AWG_SOCKET_BUF=16777216  # default, 16 MB
AWG_SOCKET_BUF=4194304   # 4 MB, for memory-constrained devices
AWG_SOCKET_BUF=1048576   # 1 MB, minimum recommended
```

#### Optional -- Performance

These parameters are only relevant on powerful devices with multiple CPU cores. On typical MikroTik routers (1-2 cores), leave the defaults.

**`AWG_CPU_C2S`**, **`AWG_CPU_S2C`** -- thread-to-CPU pinning (CPU affinity). The proxy uses two threads: c2s (client→server, outbound packet processing) and s2c (server→client, inbound). Pinning to different cores prevents thread migration and improves cache efficiency.

```
AWG_CPU_C2S=-1   # default, OS chooses the core
AWG_CPU_S2C=-1

AWG_CPU_C2S=0    # c2s on core 0
AWG_CPU_S2C=1    # s2c on core 1
```

**`AWG_BUSY_POLL`** -- enables SO_BUSY_POLL on sockets. The kernel actively polls the network driver for the specified time (in microseconds) instead of going to sleep. Reduces latency by ~50 μs but increases CPU usage. Requires network driver support.

```
AWG_BUSY_POLL=0     # default, disabled
AWG_BUSY_POLL=50    # 50 μs of active polling
AWG_BUSY_POLL=100   # 100 μs, for minimum latency
```

### Routing Traffic Through the Tunnel

Specific host:

```routeros
/ip/route/add dst-address=8.8.8.8/32 gateway=wg-awg-proxy
```

Subnet:

```routeros
/ip/route/add dst-address=10.0.0.0/8 gateway=wg-awg-proxy
```

View routes:

```routeros
/ip/route/print where gateway=wg-awg-proxy
```

Remove a route:

```routeros
/ip/route/remove [find where dst-address="8.8.8.8/32" gateway="wg-awg-proxy"]
```

### DNS Through the Tunnel

To route DNS queries through the tunnel, set DNS servers and add routes to them:

```routeros
/ip/dns/set servers=8.8.8.8,8.8.4.4
/ip/route/add dst-address=8.8.8.8/32 gateway=wg-awg-proxy
/ip/route/add dst-address=8.8.4.4/32 gateway=wg-awg-proxy
```

### Address-List Based Routing (Advanced)

For selective traffic routing through the tunnel, use routing tables and mangle rules.

Create a routing table:

```routeros
/routing/table/add disabled=no fib name=r_to_vpn
```

Default route through the tunnel for this table:

```routeros
/ip/route/add dst-address=0.0.0.0/0 gateway=wg-awg-proxy routing-table=r_to_vpn
```

Address list with destinations to route through the tunnel:

```routeros
/ip/firewall/address-list/add address=8.8.8.8 list=to_vpn
/ip/firewall/address-list/add address=1.1.1.1 list=to_vpn
```

Mangle rules for traffic marking:

```routeros
# Skip local traffic
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=10.0.0.0/8
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=172.16.0.0/12
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=192.168.0.0/16

# Mark connections to addresses in the list
/ip/firewall/mangle/add chain=prerouting action=mark-connection \
    dst-address-list=to_vpn connection-mark=no-mark \
    new-connection-mark=to-vpn-conn passthrough=yes

# Mark routing for tagged connections
/ip/firewall/mangle/add chain=prerouting action=mark-routing \
    connection-mark=to-vpn-conn new-routing-mark=r_to_vpn passthrough=yes
```

NAT for marked traffic:

```routeros
/ip/firewall/nat/add chain=srcnat action=masquerade routing-mark=r_to_vpn
```

Now all traffic to addresses in the `to_vpn` list will go through the tunnel. Add addresses to the list as needed.

## Uninstallation

If installed via the configurator:

```routeros
/system/script/run awg-proxy-uninstall
```

The script removes the container, WireGuard interface, NAT rules, routes, environment variables, restores DNS settings, and deletes itself.

## Troubleshooting

**Container does not start** -- check the container package is installed (`/system/package/print`), device mode is enabled (`/system/device-mode/print`), and there is enough disk space (`/system/resource/print`).

### execvpe /awg-proxy: No such file or directory

The container starts but immediately exits with `exited with status 255: execvpe /awg-proxy: No such file or directory`. This means the binary was not extracted — the image was downloaded incorrectly or incompletely.

1. Remove the container and root-dir:
```routeros
/container/stop [find where comment=awg-proxy]
:delay 7s
/container/remove [find where comment=awg-proxy]
/file/remove disk1/awg-proxy
:do { /file/remove [find where name~"awg-proxy.*tar"] } on-error={}
```

2. Re-download the image and verify the file size (`/file/print`) — it should be 100-300 KB, not 0.

3. Re-create the container.

### Site-to-site: handshake did not complete

In site-to-site mode (two MikroTik routers via AWG proxy), the handshake does not complete even though both containers are running. Common causes:

**1. Firewall forward chain on Side B (server)**

DSTNAT traffic goes through the `forward` chain, not `input`. If the `accept` rule is appended at the end and there's a `drop` rule above it, packets never reach the container.

Diagnosis:
```routeros
/ip/firewall/filter/print where chain=forward
```

Fix — move the rule to the top:
```routeros
/ip/firewall/filter/remove [find where comment=PREFIX-awg-in]
/ip/firewall/filter/add chain=forward action=accept protocol=udp dst-port=AWG_PORT in-interface-list=WAN place-before=0 comment=PREFIX-awg-in
```

**2. Firewall input chain on Side B (server)**

In reverse mode, the container initiates a NEW connection to MikroTik's WG port (unlike standard mode where MikroTik initiates and the container's response is "established"). If the veth interface is not in the LAN interface list, the input chain drops packets from the container.

Fix:
```routeros
/ip/firewall/filter/add chain=input action=accept protocol=udp src-address=CONTAINER_IP dst-port=WG_PORT place-before=0 comment=PREFIX-wg-in
```

**3. DNS resolution of AWG_REMOTE on Side A (client)**

If `AWG_REMOTE` is a hostname, the container needs working DNS. Set `AWG_DNS=8.8.8.8` or `AWG_DNS=1.1.1.1` in the container environment variables. The proxy re-checks DNS in the background (`AWG_DNS_REFRESH`, once per 60 s by default) and reconnects on its own when the server IP changes. If DNS also goes through the tunnel (circular dependency), resolve the hostname manually and use the IP:
```routeros
:put [:resolve vpn.example.com]
# Then set the resolved IP in AWG_REMOTE
```

**4. Diagnostics via logs**

Enable debug logging on both containers:
```routeros
/container/envs/add list=PREFIX-env key=AWG_LOG_LEVEL value=debug
```
Restart the containers and check logs — they will show DNS resolution errors, connect failures, handshake init and junk packet sending.

**5. Fallback profile**

Site-to-site configs from the configurator carry a primary (v2) and a fallback (v1) obfuscation profile. If the primary obfuscation gets blocked, after `AWG_FB_AFTER` seconds of remote silence the initiator switches to the fallback on its own — the logs show `fallback: remote silent, trying v1 fallback profile` (initiator) and `c2s: peer uses v1 fallback profile, switched` (responder). The startup line `config: v1 fallback enabled` confirms the fallback profile is set. Both ends must be generated by the same configurator run, otherwise their profiles won't match.

**No handshake** -- make sure all AWG parameters (Jc, Jmin, Jmax, S1, S2, H1--H4) exactly match the server. Verify `AWG_REMOTE`, `AWG_SERVER_PUB`, and `AWG_CLIENT_PUB`. For diagnostics, set `AWG_LOG_LEVEL=debug` -- logs will show handshake init and junk packet sending. If you see `remote read error (Connection refused)` -- the server is unreachable or the port is wrong. On ARM64, try `AWG_NO_GRO=1` -- if the kernel doesn't support GRO, the proxy may hang waiting for a response.

**No traffic after handshake** -- check the NAT rule (`/ip/firewall/nat/print`), routing, and the peer's `endpoint-address` (should be `172.18.0.2`).

**Container keeps restarting** -- set `AWG_LOG_LEVEL=info` and check the logs. Common cause: missing environment variables.

### Storage device not found

If you get `Storage device usb1 not found or has 0 free space` error -- the disk is not formatted or the mount point name doesn't match.

1. Check available disks:

```routeros
/disk/print
```

2. If the disk is visible as a block device but has no partition -- format it as ext4:

```routeros
/disk/format-drive usb1 file-system=ext4 label=usb1
```

3. After formatting, the disk will be available as a mount-point (usually `usb1`). Check the name via `/disk/print` and use it in the configurator ("Container storage" field).

> **Important:** Containers require ext4 filesystem. FAT32 is not supported.

### Insufficient disk space

If you get `Insufficient disk space` error during container installation and you have free space on an external drive (USB, SD, NVMe) -- reconfigure the image download directory:

```routeros
/container/config set tmpdir=usb1/pull memory-high=200M
```

Replace `usb1` with your drive's mount-point (see `/disk/print`).

After the container is installed, you can revert:

```routeros
/container/config set tmpdir="" memory-high=0
```

If using the configurator -- select the appropriate drive in the "Container storage" field, and tmpdir will be configured automatically.

### not allowed by device-mode

The `not allowed by device-mode` error occurs in three cases:

- When creating a container -- container support is not enabled (`container=no`)
- When downloading an image via `/tool/fetch` -- fetch is not enabled (`fetch=no`)
- When creating a scheduler (`/system/scheduler/add`) -- scheduler is not enabled (`scheduler=no`); without it the RU list never auto-updates and timeout entries silently expire after 30 days

Check the current state:

```routeros
/system/device-mode/print
```

Then enable the required features:

```routeros
/system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
```

The router will ask for confirmation -- press the Reset or Mode button on the device (depends on model) within a few minutes, or wait for automatic reboot. After reboot, retry the installation.

### child spawn failed / could not load next layer

On devices with 16 MB flash (hAP ac2, hEX, etc.) the container may fail to start with errors:
- `child spawn failed: container run error` or `exited with status 255` (RouterOS 7.20)
- `download/extract error: could not load next layer` (RouterOS 7.21+)

Checklist:

1. **Image format** -- make sure you are using the correct format:
   - RouterOS 7.21+: `awg-proxy-{arch}.tar.gz` (OCI)
   - RouterOS 7.20 and below: `awg-proxy-{arch}-7.20-Docker.tar.gz` (Docker)

2. **tmpdir on USB** -- without this, RouterOS extracts the image to internal flash, which is too small (replace `usb1` with your mount-point from `/disk/print`):
   ```routeros
   /container/config set tmpdir=usb1/pull
   ```

3. **root-dir** -- point to a folder on USB, but **do not create it manually** (RouterOS will create it automatically):
   ```routeros
   /container add ... root-dir=usb1/awg-proxy
   ```

4. **USB format** -- format the drive as ext4:
   ```routeros
   /disk/format-drive usb1 file-system=ext4 label=usb1
   ```

5. **Load from file** -- on devices with 16 MB flash, load the image from a file instead of remote-image:
   ```routeros
   /container add file=awg-proxy-arm.tar.gz ...
   ```

### exited with signal 4 (Illegal instruction)

The container crashes immediately with:

```
*** error: exited with signal 4 (Illegal instruction)
```

Cause: the image is built for a newer CPU architecture than the router has. The typical case is **hEX refresh (E50UG)** and **hEX S 2025 (E60iUGS)**: their EN7562CT CPU reports `architecture-name: arm` but only executes arm32v5 images (a [RouterOS limitation](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container)), while the standard `awg-proxy-arm.tar.gz` is built for ARMv7.

Solution -- use the armv5 image:

```routeros
/container/remove [find where comment=awg-proxy]
/tool/fetch url="https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-armv5.tar.gz" dst-path=awg-proxy-armv5.tar.gz
/container/add file=awg-proxy-armv5.tar.gz ... # other parameters as before
```

For RouterOS 7.20 and below use `awg-proxy-armv5-7.20-Docker.tar.gz`. Fresh configs from the configurator detect these devices automatically.

### RU address list not loading after reboot

When using the "All non-RU traffic through tunnel" scenario, the RU address list may not load automatically after a router reboot. Common causes:

**1. Scheduler: `start-time=startup` + `interval` are incompatible**

This is documented RouterOS behavior: if a scheduler has a non-zero `interval`, the `start-time=startup` trigger **does not fire**. The scheduler will show `run-count=0` after reboot.

Solution — two separate schedulers:

```routeros
# Boot trigger (interval MUST be 0)
/system/scheduler/add name=awg-proxy-ru-startup on-event="/system/script/run awg-proxy-ru-update" start-time=startup interval=0 comment=awg-proxy

# Daily update
/system/scheduler/add name=awg-proxy-ru-daily on-event="/system/script/run awg-proxy-ru-update" start-time=04:00:00 interval=1d comment=awg-proxy
```

If you have a single scheduler with `start-time=startup interval=1d` — remove it and create two.

**2. USB disk mounts with a delay**

USB storage appears 5-30 seconds after boot. If the script runs earlier, it won't find the `.rsc` file on USB. The configurator adds a mount wait loop to the script (up to 60 seconds):

```routeros
:if ($disk != "disk1") do={
  :local waited 0
  :while ([:len [/disk/find where mount-point=$disk]] = 0 && $waited < 60) do={
    :delay 5s
    :set waited ($waited + 5)
  }
}
```

**3. Network not ready at boot**

DHCP and DNS may be unavailable in the first seconds after boot. The script waits for network availability (up to 60 seconds) before downloading:

```routeros
:local waitNet 0
:while ($waitNet < 60) do={
  :do { :resolve "github.com"; :set waitNet 99 } on-error={ :delay 5s; :set waitNet ($waitNet + 5) }
}
```

If a cached `.rsc` file exists on disk, it is imported immediately (without waiting for network), so routing works from the first seconds. The fresh version is downloaded later.

**4. Clock lost after reboot (WG handshake fails)**

Some MikroTik models (without RTC battery) lose time on reboot. If the clock is significantly off, the WireGuard server rejects handshakes (TAI64N replay protection). Until handshake succeeds, the tunnel is down and LAN traffic is lost.

Boot sequence:
1. Clock wrong → WG handshake rejected → tunnel down
2. NTP syncs time (~5-15 sec) → clock corrected
3. WG handshake succeeds → tunnel up
4. USB mounts (~10-30 sec) → RU list imported from cache

**Important:** The router's own NTP traffic goes direct (not through tunnel), since mangle rules only apply to `in-interface-list=LAN`. The issue is NTP sync speed, not routing.

Recommendations:

```routeros
# Use IP-based NTP servers (no DNS resolution needed at boot)
/system/ntp/client/set enabled=yes servers=216.239.35.0,216.239.35.4,ntp2.vniiftri.ru,ntp.ix.ru

# Add NTP and DNS server IPs to RU list (for LAN devices to sync time directly)
/ip/firewall/address-list/add list=RU address=216.239.35.0 comment=awg-proxy-ntp
/ip/firewall/address-list/add list=RU address=216.239.35.4 comment=awg-proxy-ntp
/ip/firewall/address-list/add list=RU address=8.8.8.8 comment=awg-proxy-dns
/ip/firewall/address-list/add list=RU address=8.8.4.4 comment=awg-proxy-dns
```

LAN internet outage window is ~10-20 seconds (until NTP sync + WG handshake). NTP time-jump can also confuse scheduler timing.

### Handshake fails after backup restore

After restoring RouterOS from a backup, WireGuard handshake does not complete even though containers are running, veth interfaces are up, and ping to the container works. Logs show:

```
wg-awg-proxy: [peer] ...: Handshake for peer did not complete after 5 seconds, retrying (try 2)
```

**Cause:** Backup restore resets the system clock to the backup creation date. WireGuard uses a TAI64N timestamp in the handshake init message for replay protection — the server remembers the latest timestamp for each peer and silently drops handshakes with an older timestamp. If the router's clock is behind the real time, the server ignores all handshake packets.

**Diagnosis:**

```routeros
/system/clock/print
# If the date doesn't match the current date — this is the cause
```

**Fix:**

1. Set the correct time:
```routeros
/system/clock/set date=apr/05/2026 time=12:00:00
```

2. Enable NTP client for automatic synchronization:
```routeros
/system/ntp/client/set enabled=yes servers=time.google.com,pool.ntp.org
```

3. Restart containers and reset WG peers to force a new handshake:
```routeros
/container/stop [find]
:delay 5s
/container/start [find]
# Reset peers (disable/enable)
/interface/wireguard/peers/set [find where !disabled] disabled=yes
:delay 2s
/interface/wireguard/peers/set [find where disabled] disabled=no
```

> **Tip:** After every backup restore, always check the system clock first (`/system/clock/print`).

## Building from Source

Requires a C compiler (gcc/musl-gcc), Docker (for container images), and make.

```bash
# Tests
make test

# Local binary build
make build

# Docker images (OCI, for RouterOS 7.21+)
make docker-arm64    # ARM64
make docker-arm      # ARM v7
make docker-armv5    # ARM v5
make docker-amd64    # x86_64
make docker-all      # All architectures

# Docker images (classic format, for RouterOS 7.20 and below)
make docker-arm64-7.20-docker
make docker-arm-7.20-docker
make docker-armv5-7.20-docker
make docker-amd64-7.20-docker
make docker-all-7.20-docker
```

Artifacts are created in the `builds/` directory.

## License

MIT -- see [LICENSE](LICENSE).
