# Minimal image for the architecture-independent verification (verify/run_verify.sh).
# Works on both amd64 and arm64 hosts.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential gcc python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
