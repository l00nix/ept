# ept builder: ia16-elf-gcc cross toolchain + scripts that build packages
# from git and publish them into the repository volume.
# Build context is the top of the ept source tree (see docker-compose.yml).

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Cross toolchain from the gcc-ia16 PPA, plus what ELKS needs to build its libc
RUN apt-get update \
 && apt-get install -y --no-install-recommends software-properties-common gpg-agent \
 && add-apt-repository -y ppa:tkchia/build-ia16 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        gcc-ia16-elf binutils-ia16-elf \
        git make gcc libc6-dev bison flex ca-certificates \
        libncurses-dev xxd bc bzip2 m4 patch \
 && rm -rf /var/lib/apt/lists/*

# ept-build replaces the ELKS libc bundled with the toolchain package by one
# built from the current ELKS tree, so packages link against up to date code.
COPY server/build.sh /usr/local/bin/ept-build
COPY server/eptrepo /usr/local/bin/eptrepo
RUN chmod +x /usr/local/bin/ept-build /usr/local/bin/eptrepo

ENV ELKS=/work/elks
VOLUME ["/work", "/repo"]
CMD ["ept-build", "loop"]
