# Third-party components

airplay2cast's own source code (`src/`, `tests/`, `scripts/`, `docker/`, build files) is
licensed under the GNU General Public License version 2 only (`GPL-2.0-only`, see
`LICENSE`). It contains no code copied from the projects below.

`patches/shairport-sync-libgcrypt.patch` modifies shairport-sync and is licensed under
the MIT license, the same terms as shairport-sync, so it can be offered upstream.

No component uses OpenSSL. The bridge uses GnuTLS for the Cast TLS connection.
shairport-sync is built with the patch above, which makes libgcrypt its only crypto
library.

## How the components are combined

| component | license | relationship to airplay2cast |
|---|---|---|
| GnuTLS (with nettle, hogweed, GMP, libtasn1, libidn2, libunistring, p11-kit, libffi) | LGPL-2.1+; dependencies LGPL-3+ or GPL-2+, LGPL-2.1+, BSD-3-Clause, Expat | dynamically linked by `airplay2cast` |
| [shairport-sync](https://github.com/mikebrady/shairport-sync) 5.6.2 (`0a34fbd`) + the libgcrypt and no-libavformat patches | MIT (some files BSD-3-Clause / ISC; see its `LICENSES`) | separate program, started as a child process; talks over a pipe and UDP |
| [nqptp](https://github.com/mikebrady/nqptp) (`ab8fb67`) | GPL-2.0-only | separate program (daemon); airplay2cast only writes its shared-memory name into shairport-sync's config |
| libgcrypt, libgpg-error | LGPL-2.1+ | linked by shairport-sync |
| Avahi | LGPL-2.1+ | separate daemon; client libraries linked by shairport-sync |
| D-Bus | AFL-2.1 or GPL-2.0+ | separate daemon |
| [FFmpeg](https://ffmpeg.org) 5.1.6: libavcodec (AAC, ALAC and PCM decoders only), libavutil, libswresample | LGPL-2.1+ (built without GPL or nonfree parts) | built from source and linked statically into shairport-sync |
| libsoxr, libplist, libpopt, libconfig, libsodium, libuuid | LGPL-2.1+ / LGPL-2.1+ / MIT / LGPL-2.1+ / ISC / BSD-3-Clause | linked by shairport-sync |

Every library `airplay2cast` links is compatible with GPL-2.0-only. shairport-sync and
nqptp run as independent programs. Distributing them alongside airplay2cast is
aggregation, so each keeps its own license.

The Docker image contains no OpenSSL (`libssl3` came in only with Debian's full FFmpeg),
and none of the three programs links it.

FFmpeg is linked statically. To relink shairport-sync against a modified FFmpeg, as
LGPL-2.1 section 6 allows, rebuild it from the sources below with the same configure
options (`ffmpeg-configuration.txt`), or use the Dockerfile's `deps` stage.

### Note for a future single-process build

Moving shairport-sync and nqptp into the bridge's process would make one
GPL-2.0-only program, so everything it links must be GPL-2.0-compatible. The crypto
stack and the minimal LGPL FFmpeg already are.

## Obligations for the Docker image and `out/` binaries

The image (and `docker build --target binaries`) contains binaries of GPL and LGPL
software, so it includes:

* license texts: `/usr/share/doc/airplay2cast/` in the image, `out/share/doc/airplay2cast/`
  for the exported binaries, covering airplay2cast, shairport-sync, nqptp, FFmpeg and Avahi;
* the complete corresponding source of nqptp and shairport-sync at the pinned
  commits, plus the patches, and the FFmpeg 5.1.6 release tarball with the exact
  configure options it was built with: `/usr/share/src/airplay2cast/`
  (`out/share/src/airplay2cast/`).

The Debian packages in the image keep their own copyright files under
`/usr/share/doc/<package>/`. Their source is available from Debian
(`apt-get source <package>`, or https://sources.debian.org) for the bookworm versions
installed.

If you redistribute the image or binaries, keep these files with them.
