# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
# airplay2cast — AirPlay 2 receiver bridge for Chromecast / Google Cast devices.
#
# Stages:
#   toolchain build tools and target-architecture -dev libraries; runs on the build
#             machine and cross-compiles when --platform differs (amd64 <-> arm64)
#   deps      builds pinned nqptp + shairport-sync (AirPlay 2, stdout backend, metadata socket);
#             shairport-sync is patched to use libgcrypt instead of OpenSSL (patches/)
#   bridge    builds the airplay2cast C bridge and its unit tests
#   binaries  export target: the programs, license texts and sources (+ tests/test_unit)
#             docker buildx build --platform linux/arm64 --target binaries --output type=local,dest=out .
#   rootfs    runtime packages for the target, downloaded and unpacked on the build machine
#   runtime   final image (default target) used by compose.yaml
#
# Every RUN step executes on the build machine, so no emulation is needed:
#   docker compose build                                         # native
#   docker buildx build --platform linux/arm64 -t airplay2cast:latest --load .   # cross

ARG DEBIAN=bookworm

FROM --platform=$BUILDPLATFORM debian:${DEBIAN} AS toolchain
ARG TARGETARCH
ARG BUILDARCH
RUN set -e; \
    case "$TARGETARCH" in \
      amd64) triplet=x86_64-linux-gnu ;; \
      arm64) triplet=aarch64-linux-gnu ;; \
      *) echo "unsupported target architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    echo "$triplet" > /etc/target-triplet; \
    if [ "$TARGETARCH" = "$BUILDARCH" ]; then cc=build-essential; \
    else dpkg --add-architecture "$TARGETARCH"; cc="crossbuild-essential-$TARGETARCH"; fi; \
    libs="libpopt-dev libconfig-dev libsoxr-dev libavahi-client-dev libplist-dev libsodium-dev \
          libgcrypt20-dev uuid-dev libavutil-dev libavcodec-dev libavformat-dev \
          libswresample-dev libgnutls28-dev"; \
    apt-get update; \
    apt-get install -y --no-install-recommends $cc git autoconf automake libtool pkg-config \
      xxd ca-certificates libplist-utils $(for l in $libs; do echo "$l:$TARGETARCH"; done); \
    rm -rf /var/lib/apt/lists/*
# pkg-config and configure look for target libraries only
ENV PKG_CONFIG_PATH= PKG_CONFIG=pkg-config
WORKDIR /src

FROM toolchain AS deps
ARG NQPTP_REV=ab8fb67483411640fd7f1e193889482f2421c37c
ARG SPS_REV=0a34fbda83724cc06266aadcf7c88acc8fef4e7d
RUN t=$(cat /etc/target-triplet) && export PKG_CONFIG_LIBDIR=/usr/lib/$t/pkgconfig:/usr/share/pkgconfig \
    && git clone https://github.com/mikebrady/nqptp.git && cd nqptp && git checkout ${NQPTP_REV} \
    && autoreconf -fi && ./configure --host=$t && make -j"$(nproc)"
COPY patches /patches
RUN t=$(cat /etc/target-triplet) && export PKG_CONFIG_LIBDIR=/usr/lib/$t/pkgconfig:/usr/share/pkgconfig \
    && git clone https://github.com/mikebrady/shairport-sync.git && cd shairport-sync && git checkout ${SPS_REV} \
    && git apply /patches/shairport-sync-libgcrypt.patch \
    && autoreconf -fi \
    && ./configure --host=$t --sysconfdir=/etc --with-airplay-2 --with-avahi --with-ssl=gcrypt \
         --with-soxr --with-stdout --with-metadata --with-metadata-multicast --without-alsa \
    && make -j"$(nproc)"
RUN t=$(cat /etc/target-triplet) && mkdir -p /out/lib && cp nqptp/nqptp shairport-sync/shairport-sync /out/ \
    && $t-strip /out/nqptp /out/shairport-sync \
    && cp -L /usr/lib/$t/libavahi-client.so.3 /usr/lib/$t/libavahi-common.so.3 /out/lib/
# license texts and the complete corresponding source of the shipped binaries
# (nqptp is GPL-2.0; see THIRD_PARTY_NOTICES.md)
RUN d=/out/share/doc/airplay2cast && s=/out/share/src/airplay2cast && mkdir -p $d $s \
    && cp nqptp/LICENSE $d/LICENSE.nqptp \
    && cp shairport-sync/LICENSES $d/LICENSES.shairport-sync \
    && cp /patches/*.patch $d/ \
    && cp /usr/share/doc/libavahi-client3/copyright $d/copyright.avahi \
    && git -C nqptp archive --format=tar.gz --prefix=nqptp-${NQPTP_REV}/ \
         -o $s/nqptp-${NQPTP_REV}.tar.gz HEAD \
    && git -C shairport-sync archive --format=tar.gz --prefix=shairport-sync-${SPS_REV}/ \
         -o $s/shairport-sync-${SPS_REV}.tar.gz HEAD \
    && cp /patches/*.patch $s/
COPY LICENSE /out/share/doc/airplay2cast/LICENSE
COPY THIRD_PARTY_NOTICES.md /out/share/doc/airplay2cast/

FROM toolchain AS bridge
ARG TARGETARCH
ARG BUILDARCH
COPY Makefile ./
COPY src ./src
COPY tests ./tests
# unit tests run here for native builds; cross builds export tests/test_unit to run on the target
RUN t=$(cat /etc/target-triplet) \
    && make -j"$(nproc)" CC=$t-gcc airplay2cast tests/test_unit \
    && if [ "$TARGETARCH" = "$BUILDARCH" ]; then ./tests/test_unit; fi \
    && $t-strip airplay2cast

FROM scratch AS binaries
COPY --from=deps /out/ /
COPY --from=bridge /src/airplay2cast /
COPY --from=bridge /src/tests/test_unit /tests/

# Target base image: used only as a filesystem (no RUN), so it needs no emulation.
FROM debian:${DEBIAN}-slim AS base

# Runtime packages for the target architecture, resolved against the base image's
# installed set, unpacked on the build machine. What their maintainer scripts would
# do at install time (system users, avahi config) is done here directly.
FROM --platform=$BUILDPLATFORM debian:${DEBIAN} AS rootfs
ARG TARGETARCH
ARG RUNTIME_PACKAGES="dbus avahi-daemon libpopt0 libconfig9 libgnutls30 libsoxr0 \
      libavahi-client3 libplist3 libsodium23 libgcrypt20 libuuid1 \
      libavutil57 libavcodec59 libavformat59 libswresample4"
COPY --from=base /var/lib/dpkg/status /target/status
COPY --from=base /etc/passwd /etc/group /etc/shadow /etc/gshadow /rootfs/etc/
COPY --from=base /etc/apt/sources.list.d/ /target/sources.list.d/
RUN set -e; \
    mkdir -p /target/lists/partial /debs/partial; \
    printf '%s\n' "APT::Architecture \"$TARGETARCH\";" "APT::Architectures { \"$TARGETARCH\"; };" \
      'Dir::State::status "/target/status";' 'Dir::State::Lists "/target/lists";' \
      'Dir::Etc::SourceParts "/target/sources.list.d";' 'Dir::Cache::archives "/debs";' \
      'APT::Install-Recommends "false";' > /target/apt.conf; \
    export APT_CONFIG=/target/apt.conf; \
    apt-get update -qq; \
    apt-get install -y -qq --download-only $RUNTIME_PACKAGES; \
    mkdir -p /rootfs/usr/bin /rootfs/usr/sbin /rootfs/usr/lib /rootfs/var/lib/dpkg/info; \
    for d in bin sbin lib; do ln -s usr/$d /rootfs/$d; done; \
    cp /target/status /rootfs/var/lib/dpkg/status; \
    for deb in /debs/*.deb; do \
      pkg=$(dpkg-deb -f "$deb" Package); \
      [ "$(dpkg-deb -f "$deb" Multi-Arch)" = same ] && pkg="$pkg:$TARGETARCH"; \
      dpkg-deb --fsys-tarfile "$deb" | tar -x --keep-directory-symlink -C /rootfs; \
      dpkg-deb --fsys-tarfile "$deb" | tar -t | sed 's|^\./|/|; s|/$||; s|^$|/.|' \
        > "/rootfs/var/lib/dpkg/info/$pkg.list"; \
      { echo; dpkg-deb -f "$deb" | sed '/^Package:/a Status: install ok installed'; } \
        >> /rootfs/var/lib/dpkg/status; \
    done; \
    echo "$(ls /debs/*.deb | wc -l) packages unpacked"; \
    echo 'messagebus:x:100:101::/nonexistent:/usr/sbin/nologin' >> /rootfs/etc/passwd; \
    echo 'avahi:x:101:102:Avahi mDNS daemon,,,:/run/avahi-daemon:/usr/sbin/nologin' >> /rootfs/etc/passwd; \
    echo 'messagebus:x:101:' >> /rootfs/etc/group; \
    echo 'avahi:x:102:' >> /rootfs/etc/group; \
    echo 'messagebus:!:20398::::::' >> /rootfs/etc/shadow; \
    echo 'avahi:!:20398::::::' >> /rootfs/etc/shadow; \
    echo 'messagebus:!::' >> /rootfs/etc/gshadow; \
    echo 'avahi:!::' >> /rootfs/etc/gshadow; \
    mkdir -p /rootfs/run/dbus /rootfs/run/avahi-daemon /rootfs/var/lib/dbus; \
    chown 101:102 /rootfs/run/avahi-daemon; \
    sed -i 's/^#\?enable-dbus=.*/enable-dbus=yes/; s/^#\?publish-workstation=.*/publish-workstation=no/' \
      /rootfs/etc/avahi/avahi-daemon.conf; \
    rm /rootfs/bin /rootfs/sbin /rootfs/lib
COPY --from=binaries /nqptp /shairport-sync /airplay2cast /rootfs/usr/local/bin/
COPY --from=binaries /share/ /rootfs/usr/share/
COPY --chmod=755 docker/entrypoint.sh /rootfs/entrypoint.sh

FROM base AS runtime
COPY --from=rootfs /rootfs/ /
ENTRYPOINT ["/entrypoint.sh"]
