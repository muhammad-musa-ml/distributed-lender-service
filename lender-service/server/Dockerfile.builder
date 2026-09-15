# Build environment for the C++ Lender gRPC server.
#
# Split from the server image deliberately: these layers are ~1.5 GB of toolchain and
# change almost never, while the source changes constantly. Keeping them in a separate
# image means an edit-compile cycle does not re-resolve apt every time.
#
# WHY THE APACHE ARROW REPO IS ADDED EXPLICITLY. Ubuntu 24.04 (noble) ships neither
# libarrow-dev nor libparquet-dev in its default archive -- VERIFIED by apt-cache policy
# inside this exact base image, which reports "*** NOT AVAILABLE ***" for both while every
# other dependency resolves. Two of the three Lender RPCs are defined in terms of Parquet
# (DbToHdfs writes it, CalcAvgLoan reads it), so this is not optional. The apt-source
# package below is Apache's own, and the URL was confirmed by HTTP probe: the
# packages.apache.org host returns 200, while the repo.apache.org/dist path that appears
# in older instructions returns 000.
#
#   docker build -f Dockerfile.builder -t lender-cpp-builder .

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Base toolchain + the gRPC/protobuf stack + MySQL and HTTP clients.
# gRPC 1.51.1 and protobuf 3.21.12 come from the Ubuntu archive; pinning to distro
# packages rather than building gRPC from source is a deliberate trade -- it costs a
# slightly older gRPC and saves ~40 minutes of compile per image rebuild.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        ca-certificates \
        wget \
        lsb-release \
        libgrpc++-dev \
        protobuf-compiler \
        protobuf-compiler-grpc \
        libprotobuf-dev \
        libmysqlclient-dev \
        libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Apache Arrow / Parquet, from Apache's own apt source.
#
# NOTE the leading `apt-get update`, and that gnupg is installed BEFORE the .deb. The first
# attempt at this layer failed with:
#     apache-arrow-apt-source : Depends: gnupg but it is not installable
#     E: Unable to correct problems, you have held broken packages
# because the previous layer ends with `rm -rf /var/lib/apt/lists/*`, so this layer starts
# with no package index at all and apt cannot resolve the apt-source package's own
# dependency. The failure message names gnupg but the cause is the missing index -- which
# is why the fix is the update, not just adding gnupg to the list above.
RUN apt-get update && apt-get install -y --no-install-recommends gnupg \
    && wget -q -O /tmp/apache-arrow-apt-source.deb \
        "https://packages.apache.org/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-noble.deb" \
    && apt-get install -y --no-install-recommends /tmp/apache-arrow-apt-source.deb \
    && apt-get update \
    && apt-get install -y --no-install-recommends libarrow-dev libparquet-dev \
    && rm -f /tmp/apache-arrow-apt-source.deb \
    && rm -rf /var/lib/apt/lists/*

# A real JSON parser, for exactly one job: reading the GETFILEBLOCKLOCATIONS response.
#
# This is its own layer so the expensive Arrow layer above stays cached when it is added.
# The dependency is deliberate rather than reflexive: the alternative is matching a nested
# wire format with hand-written string searching, which is a well-known way to ship a
# parser that is subtly wrong about escaping and nesting and looks right on the one sample
# you tested. header-only, ~1 MB, and it replaces the only code in this server that would
# otherwise be guessing at someone else's format.
RUN apt-get update \
    && apt-get install -y --no-install-recommends nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

# Fail the BUILD, not a later run, if any piece is missing or mis-versioned. A toolchain
# image that silently lacks grpc_cpp_plugin produces a confusing codegen error hours later
# in someone else's build; this turns that into an immediate, named failure here.
RUN set -e; \
    echo "--- toolchain verification ---"; \
    g++ --version | head -1; \
    cmake --version | head -1; \
    protoc --version; \
    which grpc_cpp_plugin; \
    pkg-config --modversion grpc++; \
    pkg-config --modversion protobuf; \
    pkg-config --modversion arrow; \
    pkg-config --modversion parquet; \
    test -f /usr/include/grpcpp/grpcpp.h; \
    test -f /usr/include/arrow/api.h; \
    test -f /usr/include/parquet/arrow/writer.h; \
    test -f /usr/include/mysql/mysql.h; \
    test -f /usr/include/nlohmann/json.hpp; \
    echo "--- all required headers and tools present ---"

WORKDIR /work
