FROM debian:trixie-slim AS build
RUN apt-get update && \
    apt-get install -y --no-install-recommends gcc libc6-dev libaa1-dev && \
    rm -rf /var/lib/apt/lists/*
COPY ttyraid.c aatty.c aatty.h /src/
WORKDIR /src
RUN cc -O2 -Wall -static -o ttyraid ttyraid.c aatty.c -laa -lm && strip ttyraid

FROM scratch
LABEL org.opencontainers.image.title="TTY Raid" \
      org.opencontainers.image.description="ASCII scrolling shooter drawn with AA-lib" \
      org.opencontainers.image.source="https://github.com/tenox7/ttyraid"
COPY --from=build /src/ttyraid /ttyraid
ENTRYPOINT ["/ttyraid"]
