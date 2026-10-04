# Single image holding all three roles: AWG3 server (amneziawg-go),
# WG client (wireguard-go) and our awg-proxy. Role is chosen at run time.
FROM golang:1.25-bookworm

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        git iproute2 iptables iputils-ping iperf3 tcpdump socat netcat-openbsd \
        wireguard-tools xxd procps \
    && rm -rf /var/lib/apt/lists/*

# --- AmneziaWG 3.x server (userspace) ---
COPY src /build/amneziawg-go
# -f: the clone is copied in from a Windows checkout, and the CRLF line endings
# look like local changes to git inside the container.
RUN cd /build/amneziawg-go && git checkout -f -q v3.1.20260814 \
    && git log --oneline -1 \
    && go build -o /usr/local/bin/amneziawg-go .

# --- genuine pre-3.0 server ---
# Upstream has no v1.x/v2.x releases: the tag line is v0.2.x and then v3.0.x.
# "AWG 1.0 / 1.5 / 2.0" are protocol generations selected by configuration, not
# server versions. v0.2.19 is the last of the old line and the newest server
# that predates header protection entirely (its UAPI has jc/jmin/jmax, s1-s4,
# h1-h4 and i1/i2, but no header_protection_key), so it is a real, separate
# implementation to check v1/v1.5/v2 against rather than a reconfigured 3.0.3.
RUN cd /build/amneziawg-go \
    && git checkout -f -q v0.2.19 \
    && go build -o /usr/local/bin/amneziawg-go-legacy . \
    && git checkout -f -q v3.1.20260814 \
    && /usr/local/bin/amneziawg-go-legacy --version 2>/dev/null; \
    test -x /usr/local/bin/amneziawg-go-legacy

# --- plain WireGuard client (userspace) ---
RUN git clone --depth 1 https://git.zx2c4.com/wireguard-go /build/wireguard-go \
    && cd /build/wireguard-go && go build -o /usr/local/bin/wireguard-go .

# --- our proxy (prebuilt static musl binary) ---
COPY awg-proxy-test /usr/local/bin/awg-proxy
RUN chmod +x /usr/local/bin/awg-proxy

COPY scripts /scripts
RUN chmod +x /scripts/*.sh

CMD ["sleep", "infinity"]
