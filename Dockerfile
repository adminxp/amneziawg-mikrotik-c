FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc musl-dev musl-tools make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY src/ src/
COPY Makefile .
ARG VERSION=dev
RUN make build VERSION=${VERSION} CC=musl-gcc BUILD_DIR=/out

FROM scratch
COPY --from=build /out/awg-proxy /awg-proxy
ENTRYPOINT ["/awg-proxy"]
