# An AmneziaWG server on a range of ports

> Part of the [awg-proxy](../../README_en.md) documentation: a RouterOS container that connects MikroTik to AmneziaWG servers. Other sections: [FAQ](faq.md) · [install](install.md) · [update](update.md) · [variables](env.md) · [AWG 3.x and IPv6](protocol.md) · [routing and MTU](routing.md) · [server mode](server-mode.md) · [troubleshooting](troubleshooting.md).

An AmneziaWG server listens on a single UDP port. When that very port stops getting through — at the ISP, on a hotel network, on mobile data — the tunnel stops, though the server itself is fine. When the server takes clients on thousands of ports at once, awg-proxy:

- draws a **random port of the range for every connection**, so different routers and sessions do not all sit on the same port;
- **leaves a port that does not answer in about 15 seconds** instead of waiting out the one-minute `AWG_TIMEOUT`;
- now and then **changes the port on the fly, unnoticed by users** (`AWG_PORT_ROTATE`, see [variables](env.md)).

The server itself does not change: the kernel redirects every port of the range to the real AmneziaWG port. Clients that know nothing about the range (phones and laptops with the AmneziaVPN app) work as before.

## Quick: one command

On the server (Ubuntu, Debian, CentOS/Alma/Rocky and the like), as a user with `sudo`:

```bash
curl -fsSL https://raw.githubusercontent.com/timbrs/amneziawg-mikrotik-c/main/scripts/awg-ports.sh | sudo bash
```

The script:

1. **Finds the server by itself**: `awg`/`wg` interfaces on the host (the amneziawg kernel module, amneziawg-go, WireGuard) or AmneziaVPN's Docker container (`amnezia-awg`) with its UDP port. If it finds nothing, it asks for the port.
2. **Asks how many ports to take** (10,000 by default).
3. **Picks a free range** between 10000 and the start of the kernel's ephemeral ports (usually 32767), from a random start so that different servers do not end up with the same range. It steps around ports somebody listens on, ports of other NAT rules and Docker's published ports; the range then comes in several pieces.
4. **Shows the range and asks before applying**, then puts the iptables rules into a chain of their own, `AWG_PORTS`.
5. **Makes it survive a reboot**: the `awg-ports` systemd service and a check every minute (`awg-ports-check.timer`). The check puts the rules back if a firewall or Docker restart dropped them, and follows a container that came back on another address.
6. **Prints what to paste into the configurator**, for example:

   ```
   20113-30112
   ```

Running it again (`sudo awg-ports` or the same `curl` command) breaks nothing: it shows the current settings and how many connections came in through the range, and offers **r** to set up again, **d** to remove, Enter to quit.

Want to read the script first? Download it on its own:

```bash
curl -fsSLo awg-ports.sh https://raw.githubusercontent.com/timbrs/amneziawg-mikrotik-c/main/scripts/awg-ports.sh
less awg-ports.sh
sudo bash awg-ports.sh
```

No questions, for your own scripts:

```bash
sudo awg-ports --setup --port 51820 --count 10000 --yes          # server on this host, UDP 51820
sudo awg-ports --setup --container amnezia-awg --count 5000 --yes # AmneziaVPN server in Docker
sudo awg-ports --status                                          # what is set up and whether it works
sudo awg-ports --remove                                          # remove everything
```

Every option: `awg-ports --help`.

**Safety.** Only traffic addressed to the server itself is redirected (`-m addrtype --dst-type LOCAL`). The VPN clients' own traffic to other servers on the same ports passes untouched, as before.

**The provider's firewall.** If the VPS has a firewall of its own in the control panel (security groups and the like), open the UDP range the script printed there. The script cannot reach it.

## Giving the range to MikroTik

Any one of three ways:

- **The configurator.** Tick **"The server takes AmneziaWG on several ports"** and paste the range the script printed.
- **A line in the `.conf`.** Add before `[Peer]`:

  ```
  # AllowedPorts = 20113-30112
  ```

  The configurator finds it and ticks the box by itself. Some key-issuing bots write this line too.
- **By hand**, in the container's `AWG_REMOTE`: `203.0.113.5:20113-30112`. Ports and ranges can be listed with commas, up to 32 pieces: `203.0.113.5:20113-24000,24002-30113`.

**Router already set up?** Generate the commands in the configurator again, with the box ticked, and run them: how to do it without duplicates — [FAQ](faq.md#the-configurator-changed-do-i-remove-everything-and-install-again).

The range on MikroTik alone is not enough: if the server does not take those ports, the proxy keeps landing on silent ports and hopping. Server first, router second.

## A MikroTik hub (server mode)

If the server is a MikroTik in "Server (1:N hub)" mode, the hub's own firewall does the range: a dst-nat rule sends every port of it to the one port of the container.

- **A new hub.** In the configurator fill in **"More server ports"**, for example `30000-30999`. The configurator puts dst-nat and the forward rule on the main port and the range on the hub, and writes them into the clients' `AWG_REMOTE`. The range is kept in the "add a client" link, so later clients get it by themselves.
- **A hub already in place.** Add the range to its two rules (`awg-server-1` is your installation's prefix, `27015` its port), and to the IPv6 ones if there are any:

  ```routeros
  /ip/firewall/nat/set [find comment=awg-server-1-dstnat] dst-port=27015,30000-30999
  /ip/firewall/filter/set [find comment=awg-server-1-awg-in] dst-port=27015,30000-30999
  ```

  Then on every client, the range in `AWG_REMOTE` and a container restart:

  ```routeros
  /container/envs/set [find list=awg-server-1-env key=AWG_REMOTE] value="203.0.113.5:27015,30000-30999"
  /container/stop [find interface=veth-awg-server-1]
  /container/start [find interface=veth-awg-server-1]
  ```

Pick a range the hub forwards nothing else on: dst-nat takes all of it. **The hub needs awg-proxy 1.4.4 or newer.** Until the new port answers, a client sends every packet down both ports, and an older hub sent the replies back to the old port — every move fell back (the link stayed up, but the port never changed).

## By hand, without the script

### A server on the host itself (kernel module, amneziawg-go, WireGuard)

Say the server listens on UDP 51820 and the range is 20000–29999:

```bash
iptables -t nat -N AWG_PORTS
iptables -t nat -A AWG_PORTS -p udp --dport 20000:29999 -j REDIRECT --to-ports 51820
iptables -t nat -I PREROUTING 1 -m addrtype --dst-type LOCAL -j AWG_PORTS
```

The same for IPv6 — the same commands with `ip6tables`. To keep it over a reboot: `apt install iptables-persistent` and `netfilter-persistent save` (Debian/Ubuntu) or your distribution's equivalent. On nftables systems these commands work too: on current Debian and Ubuntu `iptables` is the `iptables-nft` wrapper.

### An AmneziaVPN server in Docker

AmneziaVPN keeps the server in a container (`amnezia-awg`) and publishes its UDP port. `REDIRECT` will not do here: the packets have to go straight into the container.

```bash
CIP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' amnezia-awg)
iptables -t nat -N AWG_PORTS
iptables -t nat -A AWG_PORTS -p udp --dport 20000:29999 -j DNAT --to-destination $CIP:51820
iptables -t nat -I PREROUTING 1 -m addrtype --dst-type LOCAL -j AWG_PORTS
```

`51820` here is the server's port inside the container (the number right of `->` in `docker ps`). The container's address can change when it restarts, and the rule then needs updating. The script does that by itself every minute, so for Docker the script is the better choice.

## Checking

- On the server: `sudo awg-ports --status` — the number of connections through the range grows as the router connects.
- On MikroTik: the container log line `connected to <address> port <port>` shows a port of the range, a different one on every reconnect.
- A single port can be checked without the router too: `tcpdump -ni any udp port 20113` on the server, and connect from a client with that port in `Endpoint`.

## Good to know

- **Cloudflare WARP** already takes WireGuard on ports 2408, 500, 1701 and 4500, and the configurator fills them in by itself. Moving between them mid-session does not work: Cloudflare keeps a session on the port it began on, so the proxy recognises WARP by its server key and holds on to the port that answered, trying another only once it goes silent. Several WARP addresses -- in the [FAQ](faq.md#can-the-proxy-take-several-warp-addresses).
- **Ports below 10000 and the kernel's ephemeral range** (usually from 32768) are left alone: services need the former, and the kernel hands the latter to the server's own outgoing connections.
- **IPv6.** When the server has a public IPv6 address and the AmneziaWG server runs on the host itself, the script puts the same rules into `ip6tables` too. For a server in Docker — IPv4 only.
- **Removal**: `sudo awg-ports --remove` — the rules, the service and the settings go, and the server listens on its own port only again. Then drop the range from `AWG_REMOTE` on the router, or the proxy will keep landing on silent ports.
