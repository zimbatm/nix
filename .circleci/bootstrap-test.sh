#!/usr/bin/env bash
# Make sure that we can build on non-NixOS
#
# Depends on Ubuntu 17.10
set -euo pipefail

apt-get update -qq

apt-get install -qy \
    autoconf \
    autoconf-archive \
    bison \
    brotli \
    build-essential \
    docbook5-xml \
    flex \
    git \
    libbz2-dev \
    libcurl4-openssl-dev \
    liblzma-dev \
    libseccomp-dev \
    libsodium-dev \
    libsqlite3-dev \
    libssl-dev \
    libxml2-dev \
    libxml2-utils \
    libxslt-dev \
    pkg-config \
    xsltproc \
    && true

./bootstrap.sh

# docbook is a nightmare
./configure --disable-doc-gen

make
