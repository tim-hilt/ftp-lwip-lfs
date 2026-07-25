# Reproduces the `clang-tidy` GitHub Actions job (.github/workflows/clang-tidy.yml)
# exactly, so the same errors surface locally as in CI.
#
# Usage:
#   docker build -t ftp-lwip-lfs-clang-tidy .
#   docker run --rm -v "$PWD":/repo ftp-lwip-lfs-clang-tidy
#
# Requires the littlefs/lwip submodules to be checked out first:
#   git submodule update --init --recursive

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang-tidy \
        cmake \
        build-essential \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /repo

ENTRYPOINT ["/bin/sh", "-c", "cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && clang-tidy -p build ftp_server.c"]
