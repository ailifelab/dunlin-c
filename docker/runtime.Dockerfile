# Use dist/0.2.12/bionic-arm64 after scripts/test-arm64.sh bionic.
ARG BASE_IMAGE=ubuntu@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98
FROM ${BASE_IMAGE}
ARG RUNTIME_PACKAGES="libcurl4 libjson-c3 libedit2 ncurses-base curl"
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates $RUNTIME_PACKAGES && rm -rf /var/lib/apt/lists/*
COPY dunlin /usr/local/bin/dunlin
WORKDIR /trial
USER 65534:65534
ENTRYPOINT ["/usr/local/bin/dunlin"]
