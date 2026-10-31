# Optional multi-client normal mode (multi2 review extraction)

Extracted from frozen v1.4.3-multi2 (02a3c632f8bc1adf11ae8f1843575eae1fc4bd8c), based on upstream 9d4e310. The review branch is rebased onto upstream a6b2c30 without conflicts. Supervisor, container packaging, Windows implementation and subsequent audit-hardening changes are excluded. This is a review candidate, not a replacement release.

## Configuration

Use the existing normal-mode environment configuration and replace AWG_CLIENT_PUB with AWG_CLIENT_PUBS (comma-separated public keys) or AWG_CLIENT_PUBS_FILE (one public key per line). Either enables multi-client normal automatically. AWG_NORMAL_MULTI=1 also enables it with a single AWG_CLIENT_PUB. AWG_SRC_PORT=auto is unsupported in multi-client mode; use random or a fixed port. Clients share one AWG_REMOTE and wire profile per process. Existing AWG_REMOTE port lists/ranges remain supported.

## Routing

The optional session table maps native client address plus sender index to a proxy wire index, restores receiver indices on replies and selects the client key for handshake MAC1. Transport lookup uses a bounded slot/generation table protected by a mutex. Handshake key selection scans configured keys. No per-client processes or sockets are added. Existing single-client normal remains the default with AWG_CLIENT_PUB alone.

GRO remains enabled once selected in multi-client mode to avoid truncating pending coalesced datagrams when switching to a receive path without ancillary GRO metadata. AWG_NO_GRO=1 still selects the non-GRO path at startup.

## Known limitations and review requirements

- Cookie challenge / MAC2 is not transparently translated. Index/MAC1 rewriting changes cookie associated data. Ordinary successful handshakes do not test this case.
- DNS, remote port hopping, fallback profile and reconnect state remain shared per route. These are not independent per-client connections.
- No controlled single-client throughput regression benchmark is available. Preserving the default branch of the code is not proof of identical performance: structure layout and generated code can change.
- An operator reports five tunnels on hAP ax3 and observed container memory up to 20 MB with high download speed. This is not a simultaneous-load benchmark or an isolated measurement of this extraction.
- Existing upstream synchronization concerns are not fixed by this extraction.

## Validation

Run make test test-normal-sessions test-normal-multi. The Python integration test exercises three synthetic WG clients with colliding indices, IPv4/IPv6 and GRO/GSO on/off. tests/normal-netstack contains the actual WireGuard userspace integration test; set AWG_REAL_PROXY to the built binary and omit AWG_SUPERVISOR for the standalone path. It is separate from cookie-challenge and hardware throughput testing.
