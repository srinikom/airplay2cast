#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
# Runs the natively built airplay2cast + shairport-sync on the host, without root.
# The root-only pieces (D-Bus + Avahi for AirPlay advertising, NQPTP on UDP 319/320)
# come from the "support-services" container, shared via ./run/dbus and host IPC.
#
#   docker build --target binaries --output type=local,dest=out .   # all programs
#     (for another architecture: docker buildx build --platform linux/arm64 ...)
#   make                                    # optional: a local ./airplay2cast is preferred
#   scripts/run-native.sh [airplay2cast options]      # also starts support-services
set -eu
cd "$(dirname "$0")/.."
# create the shared directories as the invoking user before Docker would create them as root
mkdir -p run/dbus run/state
docker compose --profile native up -d support-services >/dev/null
for _ in $(seq 50); do [ -S run/dbus/system_bus_socket ] && break; sleep 0.2; done
if [ ! -S run/dbus/system_bus_socket ]; then
  echo "support-services did not provide run/dbus/system_bus_socket" >&2
  exit 1
fi
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$PWD/run/dbus/system_bus_socket"
export LD_LIBRARY_PATH="$PWD/out/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
bridge=./airplay2cast
[ -x "$bridge" ] || bridge=out/airplay2cast
exec "$bridge" --shairport "$PWD/out/shairport-sync" --state-dir "$PWD/run/state" "$@"
