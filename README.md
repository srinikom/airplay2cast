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
  Its own dependency is OpenSSL; the mDNS browser, Cast protobuf framing,
  JSON parser, HTTP server and process supervision are built in.
* **shairport-sync 5.6.2** (pinned, unmodified) is the AirPlay 2 receiver: HomeKit
  pairing, encryption, decoding, and timing. One instance per speaker, each with its own
  AirPlay device ID, port and NQPTP clock.
* **nqptp** (pinned, unmodified) provides the PTP clock AirPlay 2 requires (UDP 319/320).
* **Avahi** advertises the AirPlay 2 services (shairport-sync needs it for AirPlay 2).

## Docker (recommended)

```sh
docker compose up -d --build
docker compose logs -f
```

The image builds natively on the target (tested on a Pine64, aarch64, Debian 12) and
also on x86-64. It runs D-Bus, Avahi, NQPTP and the bridge in one container with
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

Build and run tests (needs `gcc`, `make`, `libssl-dev`):

```sh
make && make test
./airplay2cast discover                 # list Cast devices
./airplay2cast tone "Hallway speaker"   # 10 s quiet test beep through the Cast path only
```

The bridge needs a `shairport-sync` built with AirPlay 2, the stdout backend, and
metadata. Its configure flags are
`--with-airplay-2 --with-avahi --with-ssl=openssl --with-soxr --with-stdout --with-metadata --with-metadata-multicast`.
It also needs a running `nqptp` and `avahi-daemon`. With those installed:

```sh
./airplay2cast --shairport /usr/local/bin/shairport-sync
```

**Hosts without root access.** The Dockerfile can export the pinned binaries, and a
helper container can supply the root-only services. Avahi and D-Bus are shared
through `./run/dbus`; NQPTP's clock is shared through host IPC.

```sh
docker build --target binaries --output type=local,dest=out .   # out/shairport-sync, out/nqptp, out/lib
make
scripts/run-native.sh -i "Hallway speaker"   # starts the support-services container, then the native binaries
```

`out/shairport-sync` is built against Debian 12 libraries. The host needs these
runtime packages: `libpopt0 libconfig9 libssl3 libsoxr0 libplist3 libsodium23
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
| `-D, --duck PERCENT` | 5 | volume cap while the Cast app launches (0 = off) |
| `-H, --hw-volume` | off | AirPlay slider sets speaker volume instead of software gain |
| `-F, --no-flush-stop` | off | keep speaker media running across AirPlay pause/skip |
| `-m, --mdns-if IP` | default route | interface for Cast discovery |
| `-I, --scan-interval S` | 30 | rediscovery interval |
| `-v` | | debug log (`-vv` adds shairport-sync debug) |

## Behaviour

* **Start:** when audio arrives, the bridge connects to the speaker, launches the
  Default Media Receiver (only if it isn't already running), and LOADs
  `http://<bridge>:8089/stream/<cast-id>.wav`.
* **Launch chime:** Cast speakers play a connection sound when a receiver app
  launches. The bridge lowers the speaker to at most 5% before LAUNCH and restores the
  previous volume once playback runs (at least 2.5 s after launch). It never raises a
  quieter or muted speaker, and leaves the volume alone if you change it during that
  window.
* **Volume:** by default the AirPlay slider scales the PCM in shairport-sync. The
  speaker's own volume is never changed, so set its preferred level with Google Home
  or its buttons. `--hw-volume` maps the slider to the speaker volume instead.
* **Pause / skip:** an AirPlay flush stops the speaker's media at once, so stale
  buffered audio is cut. The next audio reloads the stream without relaunching the
  app, so there's no second chime. Short gaps are filled with silence so the speaker
  keeps the stream open.
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

| check | native binary | Docker |
|---|---|---|
| build with `-Werror`, unit tests under ASan/UBSan | pass | pass (in image build) |
| discovery of all 6 Cast devices / groups | pass | pass |
| AirPlay 2 advertisements (`_airplay._tcp` + `_raop._tcp`, features, pk) | pass | pass (6 receivers) |
| RTSP `GET /info` binary plist | | pass |
| AirPlay 2 pair-setup/verify from pyatv up to SETUP | pass | pass |
| tone through Cast: steady PLAYING, ~1.4 s latency, volume restored | pass (x86 + Pine64) | pass |
| integration test (fake shairport-sync, real Nest Audio) | pass | |
| idle footprint, 6 receivers | | 38 MiB RAM, ~2% CPU |

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

airplay2cast is MIT licensed (`LICENSE`). shairport-sync and nqptp are fetched at
pinned commits and used unmodified as separate programs, under their
own licenses (nqptp is GPL-2.0). The Docker image ships their license texts and
source; see `THIRD_PARTY_NOTICES.md`.
