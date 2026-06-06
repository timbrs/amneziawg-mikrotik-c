# awg-routeros-app

RouterOS App image that reads one or more AmneziaWG config profiles, reconciles
the required RouterOS WireGuard objects through the RouterOS API, and starts an
AWG proxy worker for each enabled profile.

The repository is intentionally scoped to the RouterOS App flow:

- `routeros-app/src` contains the App bootstrapper and RouterOS API reconcile logic;
- `routeros-app/proxy-src` contains the minimal AWG proxy/transform code needed by the image;
- `.github/workflows/routeros-app-oci.yml` builds RouterOS 7.21+ OCI image archives.

The GitHub workflow does not use Docker. It cross-compiles static Linux/musl
binaries with Zig, writes OCI image layout archives directly, and publishes the
same images to GHCR for RouterOS `/app`:

```yaml
image: ghcr.io/iietp/awg-proxy:latest
```

The current test branch publishes:

```yaml
image: ghcr.io/iietp/awg-proxy:codex-routeros-app-full-cycle
```

The workflow also produces RouterOS 7.21+ OCI archives:

- `awg-proxy-amd64.tar.gz`
- `awg-proxy-arm64.tar.gz`
- `awg-proxy-armv7.tar.gz`

Every workflow run keeps these files as Actions artifacts. Tag pushes such as
`v0.1.0` also publish them directly to the matching GitHub Release.

See `routeros-app/README.md` for bundle format, RouterOS preflight notes, and
App environment variables.

## RouterOS App Store

Use this custom app-store URL while testing the branch:

```routeros
/app/settings/set app-store-urls=https://raw.githubusercontent.com/IIeTp/amneziawg-mikrotik-c/refs/heads/codex/routeros-app-full-cycle/app-store.yml
```

The entry uses the branch image tag:

```yaml
image: ghcr.io/iietp/awg-proxy:codex-routeros-app-full-cycle
```
