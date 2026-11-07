# airplay2cast

AirPlay 2 for Google Cast speakers. Every Chromecast / Google Home / Nest speaker and
Cast speaker group on the LAN shows up in the iPhone/Mac AirPlay picker under its own
name; picking it plays the audio on that speaker.

```
 iPhone / Mac ──AirPlay 2──► shairport-sync ──PCM (stdout)──► airplay2cast ──HTTP WAV──► Cast speaker
                (PTP timing via nqptp)       + metadata (UDP)   │                        ▲
                                                                └── Cast v2 (TLS :8009) ─┘
```

* **airplay2cast** (this repository, C): discovers Cast devices over mDNS, runs one
  AirPlay 2 receiver per device, buffers its PCM, serves it as an endless HTTP WAV
  stream, and controls the speaker with the Cast v2 protocol (Default Media Receiver).
  Its only library dependency is GnuTLS (for the Cast TLS connection); the mDNS
  browser, Cast protobuf framing, JSON parser, HTTP server and process supervision are
  built in.
* **shairport-sync 5.6.2** (pinned) is the AirPlay 2 receiver: HomeKit pairing,
  encryption, decoding, and timing. One instance per speaker, each with its own
  AirPlay device ID, port and NQPTP clock. `patches/shairport-sync-libgcrypt.patch`
  adds a `--with-ssl=gcrypt` option, so it uses libgcrypt instead of OpenSSL.
* **nqptp** (pinned, unmodified) provides the PTP clock AirPlay 2 requires (UDP 319/320).
* **Avahi** advertises the AirPlay 2 services (shairport-sync needs it for AirPlay 2).

## Docker (recommended)

```sh
docker compose up -d --build
docker compose logs -f
```

The image builds on the target itself (amd64 or arm64), or on a faster machine for
another architecture. All build steps run on the build machine: the programs are
cross-compiled with Debian's cross toolchain, and the target's runtime packages are
unpacked onto the official target base image. No QEMU emulation is needed:

```sh
make docker-image PLATFORM=linux/arm64        # image for the target (omit PLATFORM: native)
make docker-binaries PLATFORM=linux/arm64     # out/: programs + tests/test_unit
make docker-deploy DEPLOY_HOST=user@pine64    # docker load + compose up on the target
make docker-test DEPLOY_HOST=user@pine64 TEST_SPEAKER="Hallway speaker"
```

`docker-deploy` copies the image and `compose.yaml` to `DEPLOY_HOST:DEPLOY_DIR` (default
`~/airplay2cast`) and recreates the service; nothing is built on the target.
`docker-test` runs the unit tests from `out/` on the target, checks that the deployed
service started its receivers, and, with `TEST_SPEAKER`, runs the integration test
against that Cast device. Native image builds also run the unit tests during the
build. The targets are thin wrappers, e.g. `docker buildx build --platform linux/arm64
-t airplay2cast:latest --load .`; `make -n <target>` shows the exact commands. It runs D-Bus, Avahi, NQPTP and the bridge in one container with
host networking (required for mDNS, PTP and the streaming ports). UDP 319/320 must be
free: stop any other NQPTP / AirPlay 2 receiver (e.g. AirConnect, shairport-sync)
first.

Choose speakers or pass options in `compose.yaml` or via the environment:

```sh
AP2C_INCLUDE="Hallway speaker,Office speaker" docker compose up -d
```
```yaml
    command: ["--suffix", " (AirPlay)", "--no-groups", "--idle-timeout", "60"]
```

## Native binary

Build and run tests (needs `gcc`, `make`, `libgnutls28-dev`):

```sh
make && make test
./airplay2cast discover                 # list Cast devices
./airplay2cast tone "Hallway speaker"   # 10 s quiet test beep through the Cast path only
```

The bridge needs a `shairport-sync` built with AirPlay 2, the stdout backend, and
metadata. The Docker build applies `patches/shairport-sync-libgcrypt.patch` and
configures with
`--with-airplay-2 --with-avahi --with-ssl=gcrypt --with-soxr --with-stdout --with-metadata --with-metadata-multicast`.
An unpatched shairport-sync built with `--with-ssl=openssl` also works with the
bridge, but OpenSSL's Apache-2.0 license can't be combined with GPL-2.0 code.
It also needs a running `nqptp` and `avahi-daemon`. With those installed:

```sh
./airplay2cast --shairport /usr/local/bin/shairport-sync
```

**Hosts without root access.** The Dockerfile can export the pinned binaries, and a
helper container can supply the root-only services. Avahi and D-Bus are shared
through `./run/dbus`; NQPTP's clock is shared through host IPC.

```sh
make docker-binaries PLATFORM=linux/arm64
# out/: airplay2cast, shairport-sync, nqptp, lib/ (avahi client), tests/test_unit
out/tests/test_unit                          # unit tests, on the target
scripts/run-native.sh -i "Hallway speaker"   # starts the support-services container, then the native binaries
```

`out/shairport-sync` is built against Debian 12 libraries. The host needs these
runtime packages: `libpopt0 libconfig9 libgnutls30 libsoxr0 libplist3 libsodium23
libgcrypt20 libuuid1 libavutil57 libavcodec59 libavformat59 libswresample4`.
The avahi client libraries ship in `out/lib`.

## Options

| option | default | |
|---|---|---|
| `-s, --shairport PATH` | `shairport-sync` | shairport-sync executable |
| `-d, --state-dir DIR` | `$XDG_RUNTIME_DIR/airplay2cast` | generated shairport-sync configs |
| `-p, --http-port N` | 8089 | stream server port (0 = any) |
| `-a, --http-ip IP` | auto | address speakers use to reach the bridge |
| `-b, --base-port N` | 7000 | first AirPlay port (one per speaker) |
| `-i, --include LIST` / `-x, --exclude LIST` | all | comma-separated names or Cast ids (`AP2C_INCLUDE`/`AP2C_EXCLUDE`) |
| `-G, --no-groups` | | skip Cast speaker groups |
| `-S, --suffix TEXT` | none | appended to AirPlay names |
| `-t, --idle-timeout S` | 30 | release the speaker after S seconds without audio |
| `-l, --backlog MS` | 1000 | audio a speaker starts with (sets latency, see below) |
| `-r, --rate HZ` | 44100 | output sample rate: 44100 (what AirPlay senders use, so shairport-sync doesn't resample) or 48000 |
| `-D, --duck PERCENT` | 5 | volume cap while the Cast app launches (0 = off) |
| `-H, --hw-volume` | off | AirPlay slider sets speaker volume instead of software gain |
| `-F, --no-flush-stop` | off | keep speaker media running across AirPlay pause/skip |
| `-m, --mdns-if IP` | default route | interface for Cast discovery |
| `-I, --scan-interval S` | 30 | rediscovery interval |
| `-v` | | debug log (`-vv` adds shairport-sync debug) |

## Behaviour

* **Start:** when an AirPlay session begins, the bridge connects to the speaker and
  launches the Default Media Receiver (only if it isn't already running), so its
  launch chime plays while the sender is still starting up. Once 0.8 s of audio has
  arrived it LOADs `http://<bridge>:8089/stream/<cast-id>.wav`. Starting from that
  cushion, the speaker plays about 0.6 s after the LOAD (0.4 s with `--rate 48000`);
  with less, Cast receivers tend to start and then underrun.
* **Launch chime:** Cast speakers play a connection sound when a receiver app
  launches. The bridge lowers the speaker to at most 5% before LAUNCH and restores the
  previous volume once playback runs (at least 2.5 s after launch). It never raises a
  quieter or muted speaker, and leaves the volume alone if you change it during that
  window.
* **Volume:** by default the AirPlay slider scales the PCM in shairport-sync. The
  speaker's own volume is never changed, so set its preferred level with Google Home
  or its buttons. `--hw-volume` maps the slider to the speaker volume instead.
* **Pause / skip:** an AirPlay flush stops the speaker's media at once, so stale
  buffered audio is cut. Only audio sent after the flush reloads the stream, without
  relaunching the app, so there's no second chime. Short gaps are filled with silence
  so the speaker keeps the stream open.
* **Diagnostics:** the log shows how long each start takes (`speaker playing 0.6 s
  after load`) and warns about slow starts and underruns.
* **Release:** after `--idle-timeout` seconds without audio the bridge stops its app
  and disconnects, so the speaker is free for other use.
* **Someone else casts:** if another app or sender takes over the speaker, the bridge
  doesn't fight it. It reclaims the speaker only for a new AirPlay session.
* **Discovery:** new speakers appear within `--scan-interval`. A speaker disappears
  after four missed scans, and never while it is playing. IP changes are followed.

**Latency:** about 1.3–1.5 s from the AirPlay receiver to the speaker. The Cast
player keeps whatever backlog it starts with, with a floor of about 1.2 s. The stream
is served with an explicit maximal `Content-Length`. Without it, or with chunked
encoding, the Cast player stalls about 8 s after starting and keeps that much extra
latency (measured).

## Tests

* `make test`: unit tests (JSON, Cast protobuf framing, mDNS parsing including
  truncated/looping packets, ring buffer, WAV header, device IDs), built with
  ASan/UBSan.
* `tests/integration.sh "<speaker>"`: the full bridge against a real speaker. It
  uses `tests/fake_sps.py` in place of shairport-sync and scripts play → flush → resume
  → end. It checks one launch, the reload after flush, the chime duck and restore,
  the idle release, and a clean shutdown.
* `airplay2cast tone "<speaker>"`: Cast path only; prints position, delivered audio
  and latency.

### Validation record (Pine64, aarch64, Armbian 24.11 / Debian 12)

Binaries and image cross-built on x86-64 (no emulation), tested on the Pine64.

| check | native binary | Docker |
|---|---|---|
| build with `-Werror`, unit tests under ASan/UBSan | pass (cross-built `test_unit` on Pine64) | pass (amd64 native image build) |
| discovery of all 6 Cast devices / groups | pass | pass |
| AirPlay 2 advertisements (`_airplay._tcp` + `_raop._tcp`, features, pk) | pass | pass (6 receivers) |
| RTSP `GET /info` binary plist | | pass |
| AirPlay 2 pair-setup/verify from pyatv up to SETUP | pass | pass |
| libgcrypt crypto (RSA sign/OAEP, AES-CBC, MD5, base64) identical to OpenSSL | pass (x86 test harness) | |
| live `Apple-Challenge` → `Apple-Response` RSA signature verifies | | pass |
| no `libssl`/`libcrypto` loaded by bridge, shairport-sync, nqptp (`ldd`) | pass | pass |
| tone through Cast: steady PLAYING, ~1.4 s latency, volume restored | pass (x86 + Pine64) | pass |
| integration test (fake shairport-sync, real Nest Audio) | pass | |
| idle footprint, 6 receivers; restart time | | 31 MiB RAM, ~2–4% CPU; 1.5 s |
| bridge CPU, wake-ups and memory before/after tuning | see [docs/TUNING.md](docs/TUNING.md) | |

**Not covered by automated tests:**

* Playback from an Apple device. shairport-sync accepts only PTP-timed AirPlay 2 audio,
  and no scriptable Linux sender implements that (pyatv offers NTP and gets
  `501`). Pick a speaker on an iPhone/Mac to complete this check.
* `--hw-volume` against a real sender.
* Concurrent playback to several speakers.

## Limits

* Each speaker plays on its own clock. AirPlay multi-room sync with native AirPlay
  speakers, and video lip-sync, are not achievable through Cast's buffered HTTP
  playback.
* Cast speaker groups are exposed as one AirPlay target (the group's own Cast endpoint);
  playback to a group has not been tested yet.
* Avahi warns about another mDNS stack when systemd-resolved has mDNS enabled; this
  works but can be avoided with `MulticastDNS=no` in `resolved.conf`.

## License

airplay2cast is licensed under the GNU General Public License, version 2 only
(`GPL-2.0-only`, see `LICENSE`). shairport-sync (MIT, plus the MIT-licensed libgcrypt
patch in `patches/`) and nqptp (GPL-2.0-only) are fetched at pinned commits and run as
separate programs under their own licenses. Nothing in the bridge or shairport-sync
uses OpenSSL. The Docker image ships the license texts and complete source; see
`THIRD_PARTY_NOTICES.md`.
