# Optional external Python tool workload for agent acceptance; not the C runtime image.
ARG RUNTIME_IMAGE=dunlin-runtime:0.2.12-bionic
FROM ${RUNTIME_IMAGE}
USER 0:0
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends python3 && rm -rf /var/lib/apt/lists/*
USER 65534:65534
