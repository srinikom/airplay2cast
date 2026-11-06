# airplay2cast — AirPlay 2 receiver bridge for Chromecast / Google Cast devices.
#
# Stages:
#   deps      builds pinned nqptp + shairport-sync (AirPlay 2, stdout backend, metadata socket)
#   bridge    builds the airplay2cast C bridge
#   binaries  export target for running natively on a Debian 12 host:
#             docker build --target binaries --output type=local,dest=out .
#   runtime   final image (default target) used by compose.yaml

ARG DEBIAN=bookworm

FROM debian:${DEBIAN} AS deps
ARG NQPTP_REV=ab8fb67483411640fd7f1e193889482f2421c37c
ARG SPS_REV=0a34fbda83724cc06266aadcf7c88acc8fef4e7d
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git autoconf automake libtool pkg-config xxd ca-certificates \
      libpopt-dev libconfig-dev libssl-dev libsoxr-dev libavahi-client-dev \
      libplist-dev libplist-utils libsodium-dev libgcrypt20-dev uuid-dev \
      libavutil-dev libavcodec-dev libavformat-dev libswresample-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone https://github.com/mikebrady/nqptp.git && cd nqptp && git checkout ${NQPTP_REV} \
    && autoreconf -fi && ./configure && make -j"$(nproc)"
RUN git clone https://github.com/mikebrady/shairport-sync.git && cd shairport-sync && git checkout ${SPS_REV} \
    && autoreconf -fi \
    && ./configure --sysconfdir=/etc --with-airplay-2 --with-avahi --with-ssl=openssl \
         --with-soxr --with-stdout --with-metadata --with-metadata-multicast --without-alsa \
    && make -j"$(nproc)"
RUN mkdir -p /out/lib && cp nqptp/nqptp shairport-sync/shairport-sync /out/ \
    && strip /out/nqptp /out/shairport-sync \
    && cp -L /usr/lib/*/libavahi-client.so.3 /usr/lib/*/libavahi-common.so.3 /out/lib/

FROM debian:${DEBIAN} AS bridge
RUN apt-get update && apt-get install -y --no-install-recommends build-essential libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tests ./tests
RUN make -j"$(nproc)" && make test && strip airplay2cast

FROM scratch AS binaries
COPY --from=deps /out/ /
COPY --from=bridge /src/airplay2cast /

FROM debian:${DEBIAN}-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
      dbus avahi-daemon libpopt0 libconfig9 libssl3 libsoxr0 libavahi-client3 \
      libplist3 libsodium23 libgcrypt20 libuuid1 \
      libavutil57 libavcodec59 libavformat59 libswresample4 \
    && rm -rf /var/lib/apt/lists/* \
    && sed -i 's/^#\?enable-dbus=.*/enable-dbus=yes/; s/^#\?publish-workstation=.*/publish-workstation=no/' /etc/avahi/avahi-daemon.conf
COPY --from=deps /out/nqptp /out/shairport-sync /usr/local/bin/
COPY --from=bridge /src/airplay2cast /usr/local/bin/
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod 755 /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
