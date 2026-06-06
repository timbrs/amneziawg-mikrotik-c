# awg-routeros-app

RouterOS App image that reads one or more AmneziaWG config profiles, reconciles
the required RouterOS WireGuard objects through the RouterOS API, and starts an
AWG proxy worker for each enabled profile.

The repository is intentionally scoped to the RouterOS App flow:

- `routeros-app/src` contains the App bootstrapper and RouterOS API reconcile logic;
- `routeros-app/proxy-src` contains the minimal AWG proxy/transform code needed by the image;
- `.github/workflows/routeros-app-image.yml` builds and publishes the combined image.

Build locally from the repository root:

```sh
docker build -f routeros-app/Dockerfile -t awg-routeros-app:dev .
```

See `routeros-app/README.md` for bundle format, RouterOS preflight notes, and
App environment variables.
