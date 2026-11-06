# Third-party components

airplay2cast's own source code (`src/`, `tests/`, `scripts/`, `docker/`, build files) is
MIT licensed (see `LICENSE`). It contains no code copied from the projects below.

## How the components are combined

| component | license | relationship to airplay2cast |
|---|---|---|
| OpenSSL 3 | Apache-2.0 | dynamically linked by `airplay2cast` |
| [shairport-sync](https://github.com/mikebrady/shairport-sync) 5.6.2 (`0a34fbd`) | MIT (some files BSD-3-Clause / ISC; see its `LICENSES`) | separate program, started as a child process; talks over a pipe and UDP |
| [nqptp](https://github.com/mikebrady/nqptp) (`ab8fb67`) | GPL-2.0-only | separate program (daemon); airplay2cast only writes its shared-memory name into shairport-sync's config |
| Avahi | LGPL-2.1+ | separate daemon; client libraries linked by shairport-sync |
| D-Bus | AFL-2.1 / GPL-2.0+ | separate daemon |
| FFmpeg libraries (Debian `libav*`) | GPL-2.0+ as built by Debian | linked by shairport-sync |
| libsoxr, libgcrypt, libplist, libpopt, libconfig, libsodium, libuuid | LGPL / MIT / ISC / BSD | linked by shairport-sync |

airplay2cast is not linked with any GPL code, so the MIT license applies to it unchanged.
shairport-sync and nqptp run as independent programs. Distributing them with
airplay2cast is aggregation, so they keep their own licenses.

The shairport-sync binary in the Docker image links Debian's GPL-2.0+ FFmpeg build
and OpenSSL 3 (Apache-2.0). Those two licenses are compatible under GPL-3.0, which
the "or later" terms of the FFmpeg libraries allow. shairport-sync's own MIT code is
compatible with both.

## Obligations for the Docker image and `out/` binaries

The image (and `docker build --target binaries`) contains binaries of GPL and LGPL
software, so it includes:

* license texts: `/usr/share/doc/airplay2cast/` in the image, `out/share/doc/airplay2cast/`
  for the exported binaries, covering airplay2cast, shairport-sync, nqptp and Avahi;
* the complete corresponding source of nqptp and shairport-sync at the pinned
  commits: `/usr/share/src/airplay2cast/*.tar.gz` (`out/share/src/airplay2cast/`).

The Debian packages in the image keep their own copyright files under
`/usr/share/doc/<package>/`. Their source is available from Debian
(`apt-get source <package>`, or https://sources.debian.org) for the bookworm versions
installed.

If you redistribute the image or binaries, keep these files with them.
