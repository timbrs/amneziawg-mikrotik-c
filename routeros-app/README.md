# awg-routeros-app

Experimental RouterOS App bootstrapper for `awg-proxy`.

This component is intentionally separate from the existing proxy. It reads one
bundle file with one or more named AmneziaWG profiles, reconciles managed
RouterOS WireGuard objects through the native RouterOS API, then starts one
`awg-proxy` process per enabled profile.

## Bundle format

```ini
#@awg-profile name=home enabled=yes
[Interface]
Address = 10.0.0.2/24
ListenPort = 51820
PrivateKey = ...
Jc = 4
Jmin = 50
Jmax = 1000
S1 = 146
S2 = 42
H1 = 532916466
H2 = 2096090865
H3 = 406337014
H4 = 57583056
MTU = 1420

[Peer]
PublicKey = ...
PresharedKey = ...
AllowedIPs = 0.0.0.0/0, ::/0
Endpoint = 109.94.176.176:47954
PersistentKeepalive = 16

#@awg-profile name=work enabled=yes
[Interface]
...
[Peer]
...
```

A plain single-profile WireGuard/AWG `.conf` without `#@awg-profile` is also
valid and becomes profile `default`.

## Runtime inputs

Only `AWG_CONFIG` is the user-provided AWG profile input.

When packaged as a RouterOS App, use the built-in App placeholders to avoid
hardcoding the container address:

```ini
AWG_CONFIG=/etc/awg-proxy/awg-bundle.conf
AWG_CONTAINER_IP=[containerIP]
AWG_CONTAINER_INTERFACE=[containerInterface]
```

The app also accepts the raw App names (`[containerIP]`, `containerIP`) and
manual container names (`CONTAINER_IP`) as fallbacks for custom wrappers. If no
container IP is supplied, it falls back to `172.18.0.2`.

`[routerIP]`, `[accessIP]`, `[accessPort]`, and `[accessProto]` are read only
for startup diagnostics. They describe RouterOS App access and UI publishing,
not the internal API endpoint used for reconciliation.

RouterOS API credentials are intentionally not part of `AWG_CONFIG`. The app
reads them from `AWG_ROUTEROS_CREDS`, defaulting to
`/etc/awg-proxy/routeros-api.conf`:

```ini
host=172.18.0.1
port=8728
user=awg-proxy
password=...
```

This file is expected to be created by a RouterOS preflight script and mounted
into the app container.

## Managed lifecycle

RouterOS does not expose a public `dynamic=yes` switch for third-party
WireGuard interface/peer creation. Back To Home can create dynamic WireGuard
objects because it is a built-in RouterOS service owner.

This app therefore emulates dynamic ownership:

- all owned objects use comment `awg-proxy:<profile>:<hash>`;
- a periodic reconcile loop restores the bundle-defined state;
- profiles removed from the bundle have their managed objects removed;
- unmanaged objects with the target names are treated as collisions and fail
  the bootstrap.
- every `AWG_RECONCILE_INTERVAL` seconds, default `30`, the bundle is reread;
  changed profiles restart workers and removed/disabled profiles are cleaned up.

## Build

```sh
make
```

The produced `awg-routeros-app` and `awg-proxy` binaries are both required by
the final image. `proxy-src/` contains the minimal proxy/transform code kept for
that purpose.

## Direct OCI image builds

RouterOS 7.21+ can import standard OCI archives. This repository therefore does
not need Docker/Buildx for CI image creation.

`.github/workflows/routeros-app-oci.yml` builds per-architecture OCI archives
directly and publishes the same images to GHCR for RouterOS `/app`:

```yaml
image: ghcr.io/iietp/awg-proxy:latest
```

Artifacts:

- `awg-proxy-amd64.tar.gz`
- `awg-proxy-arm64.tar.gz`
- `awg-proxy-armv7.tar.gz`

The workflow uses `zig cc` for Linux/musl cross-compilation and
`scripts/make-oci-image.sh` to write the OCI image layout tarball. `skopeo`
and `podman manifest` publish the generated OCI images to GHCR without Docker.

Every workflow run uploads the archives as GitHub Actions artifacts. On `v*`
tags, the same archives are uploaded directly to the matching GitHub Release.

`app-template.yml` is a starter custom App YAML. It wires
`AWG_CONTAINER_IP=[containerIP]` and mounts `disk1/awg-proxy` at
`/etc/awg-proxy`, where both `awg-bundle.conf` and `routeros-api.conf` are
expected by default.

For multi-profile safety the RouterOS WireGuard listen port is assigned as
`42000 + profile_index` by default. The `ListenPort` inside imported AWG files
is treated as source metadata and is not trusted to be unique across profiles.

`scripts/preflight-template.rsc` contains a starter RouterOS script for the
dedicated API user and credentials file. It is a template, not a ready-to-run
secret-bearing script.
