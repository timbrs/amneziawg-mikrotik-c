FROM debian:bookworm-slim AS build
RUN apt-get update && \
    apt-get install -y --no-install-recommends gcc musl-dev musl-tools make && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src/routeros-app
COPY routeros-app/ ./

ARG VERSION=dev
RUN make VERSION=${VERSION} CC=musl-gcc && \
    mkdir -p /out/etc && \
    cp awg-proxy awg-routeros-app /out/ && \
    touch /out/etc/resolv.conf

FROM scratch
COPY --from=build /out/awg-proxy /awg-proxy
COPY --from=build /out/awg-routeros-app /awg-routeros-app
COPY --from=build /out/etc/resolv.conf /etc/resolv.conf
ENTRYPOINT ["/awg-routeros-app"]
