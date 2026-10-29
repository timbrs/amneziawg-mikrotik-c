> This project is not affiliated with or endorsed by MikroTik / SIA Mikrotikls

# awg-proxy -- AmneziaWG for MikroTik

[![C11](https://img.shields.io/badge/C-11-blue)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[Русская версия](README.md) | [**Configurator**](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html) | [Releases](https://github.com/timbrs/amneziawg-mikrotik-c/releases) | [FAQ](docs/en/faq.md)

**awg-proxy connects a MikroTik router to an AmneziaWG VPN server** (the kind the AmneziaVPN app sets up, for example). AmneziaWG is WireGuard with disguised packets: you cannot tell the protocol by how they look. MikroTik itself only speaks plain WireGuard, so awg-proxy runs on the router as a tiny container, and from then on it is an ordinary VPN tunnel of the router: for every device on the network, with no apps on phones or laptops.

Setup takes 5–10 minutes: the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html) in your browser turns the `.conf` from AmneziaVPN into ready-to-run MikroTik commands.

## Quick start

### 1. Check that your router fits

You need a MikroTik with an **ARM, ARM64 or x86** CPU and RouterOS **7.4 or newer**. Open a terminal (WinBox → **New Terminal**) and run:

```routeros
/system/resource/print
```

`architecture-name` must say `arm64`, `arm` or `x86_64`. If it says `mipsbe`, `mmips` or `smips` (hEX, hEX S, hAP ac lite, hAP lite), the router **will not do**: MikroTik builds no container package for them. Models by architecture are in the [table below](#will-it-run-on-my-router).

### 2. Prepare the router (once)

Without this step the configurator's commands will not work.

1. **Install the `container` package.** Check: `/system/package/print` must list `container`. If it does not:
   - **RouterOS 7.18 or newer** — the router fetches the package itself. In WinBox: **System → Packages → Check For Updates**; `container` shows up in the list: select it, press **Enable**, then **Apply Changes**. The router downloads the package and reboots. The same in the terminal:

     ```routeros
     /system/package/update/check-for-updates
     /system/package/enable container
     /system/package/apply-changes
     ```

   - **RouterOS older than 7.18** — at [mikrotik.com/download](https://mikrotik.com/download) get the **Extra packages** for your architecture and **exactly your** RouterOS version, take `container-….npk` out of the archive, drag it into the **Files** window in WinBox and reboot the router (`/system/reboot`).
2. **Allow containers in device mode:**

   ```routeros
   /system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
   ```

   RouterOS asks you to prove you are next to the router: **within 5 minutes** press a button on the case (Reset or Mode) or power-cycle it. Check: `/system/device-mode/print` must show `yes` everywhere.

### 3. Get the `.conf`

In the **AmneziaVPN** app: pick the connection → **Share** → protocol **AmneziaWG**, format **AmneziaWG Format** → save the file. No server of your own? A free Cloudflare WARP config will do; the configurator has a separate mode for it, with instructions.

> **The Sponsor button at the top of the page is not a donation box.** It leads to a VPS for your own VPN: [Freakhosting BVPS-2](https://freakhosting.com/clientarea/aff.php?aff=730&redirectto=https://freakhosting.com/budget-vps/) for **$34 a year** — 2 vCPU, 2 GB RAM, 40 GB NVMe, a 10 Gbit/s port with unlimited traffic. You pay the host the usual price, and the host gives the project a small bonus: a win for both. The AmneziaVPN app installs the AmneziaWG server on that VPS by itself. Price as of the last check; it may change.

### 4. Generate the commands and paste them into the terminal

Open the [**configurator**](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), paste the `.conf`, press **Generate commands** and run the commands in the MikroTik terminal in order. The configurator works offline in the browser: your keys go nowhere. The same commands are also available [from the command line](#command-line-generator).

Done: the tunnel is up, and all traffic or only the services you picked in the configurator go through the AmneziaWG server.

> **Tip.** If the server is yours, let it take AmneziaWG on thousands of ports at once — one command on the server, [docs/en/ports.md](docs/en/ports.md). The proxy then draws a random port for every connection, and if a port stops answering, it simply moves to another.

<video src="https://github.com/user-attachments/assets/f0100789-0a23-42f8-a67f-085e5f8d13a3" controls width="100%"></video>

## What it does

- **Disguises WireGuard.** The router encrypts traffic with its own plain WireGuard, and awg-proxy re-dresses every packet as AmneziaWG on the fly: other headers, random padding, junk packets before the handshake. The server takes these packets as its own, and the ISP's DPI no longer sees WireGuard in them. awg-proxy does not know the keys and cannot see the contents.
- **Every AmneziaWG version:** 1.0, 1.5, 2.0, 3.0 and 3.1. The proxy works out which one from the `.conf` parameters; there is nothing to set.
- **Free Cloudflare WARP** if you have no server of your own.
- **Only what you want through the tunnel:** all traffic, only chosen services (YouTube, Telegram, Instagram and others) or everything except Russian sites. You pick it in the configurator.
- **Several server ports.** If the server takes AmneziaWG on a range of ports, the proxy draws a random one for every connection, leaves one that does not answer in about 15 seconds and moves to another every 30–120 minutes on the fly — with no drop that games or streams would notice. How to set up the server (a VPS with one command or a MikroTik hub) — [docs/en/ports.md](docs/en/ports.md).
- **Updates and heals itself.** On RouterOS 7.22+ the container comes from the registry, updates with one command or nightly on a schedule, and comes back up by itself if it falls over.
- **Light and fast.** The image is about 90 KB, 1–2 MB in memory. Hundreds of Mbit/s on a hAP ax² and ax³: the limit is the router's CPU doing the WireGuard crypto, not the proxy.
- **Not only a client.** Two MikroTiks to each other (site-to-site) and a hub for many clients on a VPS or a MikroTik (server mode).

This is the current repository. The old Go version, [timbrs/amneziawg-mikrotik](https://github.com/timbrs/amneziawg-mikrotik), is no longer developed.

Docs last reviewed on 29 October 2026 for awg-proxy 1.4.4. Tested on RouterOS 7.24.4 (hAP ax², arm64) and hAP ac² (arm) against AmneziaWG 3.1 servers (amneziawg-go and the kernel module).

![Speed test on MikroTik AX3](https://github.com/user-attachments/assets/9fb34444-681b-4f34-8306-8f202f1b121d)

*Speed test on a MikroTik hAP ax³*

## Will it run on my router

`/system/resource/print` gives the exact answer in `architecture-name`: `arm64`, `arm` and `x86_64` work; `mipsbe`, `mmips`, `smips`, `tile` and `ppc` do not. The configurator picks the image from the architecture, the RouterOS version and the board model itself.

| Models | Architecture | Image for RouterOS 7.21+ | Notes |
|--------|--------------|--------------------------|-------|
| hAP ax², hAP ax³, RB5009, CCR2004, CCR2116, CCR2216 | arm64 | `awg-proxy-arm64.tar.gz` or `ghcr.io/timbrs/awg-proxy:latest` | Fastest option. Thread pinning and `AWG_RT` are available on 4 cores |
| hAP ac², hAP ac³, RB4011, RB3011 | arm | `awg-proxy-arm.tar.gz` or `ghcr.io/timbrs/awg-proxy:latest` | hAP ac² has 16 MB flash: install to an ext4 USB stick, see [troubleshooting](docs/en/troubleshooting.md#child-spawn-failed--could-not-load-next-layer) |
| hEX refresh (E50UG), hEX S 2025 (E60iUGS), hAP ax S (E62iUGS) | arm, but the CPU runs ARMv5 only | `awg-proxy-armv5.tar.gz` or `ghcr.io/timbrs/awg-proxy:latest-armv5` | The regular arm image dies with `signal 4 (Illegal instruction)` |
| x86, CHR | x86_64 | `awg-proxy-amd64.tar.gz` or `ghcr.io/timbrs/awg-proxy:latest` | |
| hEX (RB750Gr3), hEX S (RB760iGS), hAP ac lite, hAP ac, hAP lite, RB2011, mAP | MIPS | — | **Not supported**: RouterOS has no container package for MIPS |
| CCR1009, CCR1016, CCR1036, CCR1072 | tile | — | **Not supported** |

For RouterOS 7.20 and older use the same names with the `-7.20-Docker` suffix, e.g. `awg-proxy-arm-7.20-Docker.tar.gz`.

**Also needed:** RouterOS 7.4+ with the `container` package, 16 MB of free RAM and about 1 MB of disk (a USB stick on routers with 16 MB flash). An AmneziaWG server with a `.conf` from AmneziaVPN, or a Cloudflare WARP config.

## How it works

```
Devices on the LAN ──> MikroTik WireGuard ──UDP──> [awg-proxy] ──UDP──> AmneziaWG server
                       (encrypts as usual)       (re-dresses packets)
```

The router encrypts traffic with its stock WireGuard and sends the packets not to the server but to the awg-proxy container on the same router. awg-proxy changes in every packet what DPI recognises WireGuard by: the type in the header, the length (it adds padding), and it sends junk packets before the handshake. The server's answers get the reverse treatment. Packet contents and keys stay as they are: the proxy cannot decrypt the traffic.

Other modes:

- **site-to-site (reverse)** — two MikroTiks connect to each other over AmneziaWG with no separate server:
  `MikroTik1 WG ↔ [awg-proxy normal] ──AWG──> [awg-proxy reverse] ↔ MikroTik2 WG`. Point-to-point only.
- **server (1:N hub)** — one awg-proxy on a VPS or a MikroTik takes many clients and hands them to a plain WireGuard server: [docs/en/server-mode.md](docs/en/server-mode.md).

## Documentation

Detailed documentation lives in [`docs/en/`](docs/en/) (Russian: [`docs/`](docs/)):

| File | Topic |
|------|-------|
| [faq.md](docs/en/faq.md) | Common questions in plain words: will it run on my router, what speed to expect, how it differs from other containers |
| [install.md](docs/en/install.md) | Where to get the `.conf`, WARP, step-by-step manual install, installing without GitHub access |
| [ports.md](docs/en/ports.md) | A server on a range of ports: the one-command script, manual setup, how to give it to the configurator |
| [update.md](docs/en/update.md) | Registry updates, self-repair, moving off an old file install |
| [env.md](docs/en/env.md) | Every container environment variable |
| [protocol.md](docs/en/protocol.md) | AmneziaWG 3.0 and 3.1 on the wire, the fallback chain, IPv6 |
| [routing.md](docs/en/routing.md) | Routes through the tunnel, DNS, MTU and PPPoE, address-list |
| [server-mode.md](docs/en/server-mode.md) | A 1:N hub on a VPS or a MikroTik |
| [troubleshooting.md](docs/en/troubleshooting.md) | Error messages and what to do about them |

A short summary with links for AI assistants: [llms.txt](https://timbrs.github.io/amneziawg-mikrotik-c/llms.txt). Working on the code? See [AGENTS.md](AGENTS.md).

## Installing without GitHub access

Turn on **offline mode** in the configurator: step 0 asks the router for its architecture and RouterOS version and prints which files to download and where to put them. Details — [docs/en/install.md](docs/en/install.md#installing-without-github-access).

## Manual installation

Without the configurator, with RouterOS commands: the container package, the image (as a file or from the `ghcr.io/timbrs/awg-proxy` registry), veth, the WireGuard interface, environment variables, the container — [docs/en/install.md](docs/en/install.md#manual-installation).

What happens when the `.conf` lacks some parameters and how WARP mode works — [docs/en/install.md](docs/en/install.md#getting-awg-parameters).

## Updating

On RouterOS 7.22+ a container installed from the registry updates with one command, `/system/script/run awg-proxy-1-update` (the configurator installs the script; it also updates nightly at 04:30 and restarts a dead container). By hand — `/container/repull`. Details — [docs/en/update.md](docs/en/update.md).

## Uninstallation

If installed via the configurator:

```routeros
/system/script/run awg-proxy-uninstall
```

The script removes the container, WireGuard interface, NAT rules, routes, environment variables, the update script with its scheduler, restores DNS settings, and deletes itself.

## Troubleshooting

Error messages with fixes are in [docs/en/troubleshooting.md](docs/en/troubleshooting.md):

- [`not allowed by device-mode`](docs/en/troubleshooting.md#not-allowed-by-device-mode) — step 2 of the quick start was skipped
- [`execvpe /awg-proxy: No such file or directory`](docs/en/troubleshooting.md#execvpe-awg-proxy-no-such-file-or-directory) — the image was not downloaded completely
- [`Site-to-site: handshake did not complete`](docs/en/troubleshooting.md#site-to-site-handshake-did-not-complete)
- [`Storage device not found`](docs/en/troubleshooting.md#storage-device-not-found) — the disk is not formatted as ext4
- [`Insufficient disk space`](docs/en/troubleshooting.md#insufficient-disk-space)
- [`no space to extract layer`](docs/en/troubleshooting.md#no-space-to-extract-layer) — no room to unpack the image, with a RAM workaround
- [`start failed: no default dns servers`](docs/en/troubleshooting.md#start-failed-no-default-dns-servers) — RouterOS 7.24, the container has no DNS set
- [`child spawn failed` / `could not load next layer`](docs/en/troubleshooting.md#child-spawn-failed--could-not-load-next-layer) — routers with 16 MB flash
- [`exited with signal 4 (Illegal instruction)`](docs/en/troubleshooting.md#exited-with-signal-4-illegal-instruction) — hEX refresh / hEX S 2025 / hAP ax S need the armv5 image
- [RU address list not loading after reboot](docs/en/troubleshooting.md#ru-address-list-not-loading-after-reboot)
- [Handshake fails after backup restore](docs/en/troubleshooting.md#handshake-fails-after-backup-restore)

No handshake, no traffic after the handshake, the container keeps restarting — at the top of the same page. Common questions — [FAQ](docs/en/faq.md).

## Building from source

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

Artifacts are created in the `builds/` directory. Code layout, invariants and how to run the tests are in [AGENTS.md](AGENTS.md).

The configurator has its own jsdom-based checks (dev only — the proxy itself has no dependencies):

```bash
npm install jsdom
node tests/conf3.0-ipv6.test.js
node tests/conf3.0-dns.test.js
node tests/conf3.0-ports.test.js
node tests/conf3.1.test.js
```

### Command-line generator

`tools/awg-gen.js` prints the same RouterOS commands as the configurator, without a browser. It opens `docs/conf3.0.html` in jsdom, fills in the form and presses Generate, so the page stays the only copy of the generation logic and the output matches it byte for byte. It runs locally and offline: the page's network calls are cut off and the private key never leaves the machine. Fit for a person and for an AI agent on the user's machine alike. Needs Node.js 20+.

```bash
npm install jsdom                        # once, in the repository root
node tools/awg-gen.js amnezia.conf > awg.rsc
node tools/awg-gen.js --scenario non-ru --exclude steam,epic amnezia.conf
node tools/awg-gen.js --part routing --services youtube,telegram amnezia.conf
node tools/awg-gen.js --mode site-to-site --endpoint 198.51.100.1 --part side-b
node tools/awg-gen.js --help             # every option, the services and the disks
```

The config is read from a file or from stdin. The script goes to stdout in parts (`# >>> [install]`, `# >>> [routing]`, …) that are run in turn, each after the previous one has finished; `--part` prints one part. The page's notes and errors go to stderr. Exit code 1 means the page refused the config, 2 a bad option.

## License

MIT -- see [LICENSE](LICENSE).
