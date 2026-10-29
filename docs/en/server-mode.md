# Server Mode (1:N) — Detailed Setup

> Part of the [awg-proxy](../../README_en.md) documentation: a RouterOS container that connects MikroTik to AmneziaWG servers. The easiest way to install it is the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), which generates all commands. Other pages: [install](install.md) · [updating](update.md) · [variables](env.md) · [AWG 3.x and IPv6](protocol.md) · [routing and MTU](routing.md) · [server mode](server-mode.md) · [troubleshooting](troubleshooting.md) · [FAQ](faq.md).

Server mode allows a single awg-proxy to serve multiple MikroTik clients simultaneously. It acts as a full AmneziaWG server, implemented via a WireGuard server + awg-proxy pair.

## Architecture

```
MikroTik1 + awg-proxy(normal) ──AWG──┐
MikroTik2 + awg-proxy(normal) ──AWG──┤──> VPS: [awg-proxy server :443] ──WG──> [WG server :51820]
MikroTik3 + awg-proxy(normal) ──AWG──┘
```

- **WireGuard server** — a standard WG server that receives WG traffic from awg-proxy. Each MikroTik client is a separate peer.
- **awg-proxy in `server` mode** — listens on a public port (e.g., 443/udp), accepts AWG traffic from client normal-proxies, converts it to standard WG, and forwards it to the local WG server. A session table routes responses back to the correct client.

## 1. Server Side Setup (VPS)

### WireGuard Server

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

### awg-proxy in Server Mode

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

## 2. Client Side Setup (MikroTik)

Each MikroTik client uses a standard awg-proxy in `normal` mode (default).

1. Create a `.conf` file for each client. In the `[Peer]` section:
   - `Endpoint` = YOUR_VPS_IP:443 (the AWG_LISTEN port on the server)
   - `PublicKey` = WG server public key

2. Open the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), paste the `.conf` and run the generated commands on MikroTik.

3. Obfuscation parameters (Jc, S1, S2, H1--H4) must **exactly match** the server parameters.

## 3. Adding a New Client

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

## 4. Verification

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

## Several Ports on the Hub

A hub can take clients on a range of ports instead of one: client routers then draw a random port for every connection and move to another every 30–120 minutes on the fly. For a MikroTik hub that is the **"More server ports"** field in the configurator, for a hub on a VPS the `awg-ports.sh` script. How to switch it on, an existing hub included — [ports.md](ports.md#a-mikrotik-hub-server-mode). The hub needs awg-proxy 1.4.4 or newer: an older one sent replies back to the old port, and every move on the clients fell back.

## Routing Between Clients

In the star topology, each client only sees the server's LAN by default. Client 1 (192.168.8.0/24) **does not know** about client 2's network (192.168.11.0/24) and vice versa. Traffic between clients goes through the server, but routes must be added manually.

### Diagram

```
Client 1 (192.168.8.0/24)                    Client 2 (192.168.11.0/24)
   WG: 10.10.10.2                               WG: 10.10.10.3
         \                                            /
          \_________ Server (192.168.1.0/24) ________/
                      WG: 10.10.10.1
```

Routes set up by default:
- Client 1 → server network (192.168.1.0/24)
- Client 2 → server network (192.168.1.0/24)
- Server → client 1 network (192.168.8.0/24)
- Server → client 2 network (192.168.11.0/24)

There are **no** client 1 ↔ client 2 routes.

### How to Add

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

### Verification

From a device on client 1's network, ping a device on client 2's network:

```
ping 192.168.11.X
```

Traffic goes: client 1 → WG → server → WG → client 2. This is a double hop through the server — latency will be the sum of both tunnels.

### Multiple Clients

For N clients:
- On the server: add **all other** client subnets to each peer's `allowed-address`
- On each client: add routes to **all other** client subnets

With 3+ clients, this is easier to automate with a script on the server.
