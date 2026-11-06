#!/bin/bash
# Container entrypoint: D-Bus + Avahi (unless the host's are mounted in), NQPTP, then
# the airplay2cast bridge. AP2C_MODE=services starts only the support services; it is
# used to test the natively built bridge on a host without root access.
set -u

log() { echo "entrypoint: $*" >&2; }

bus_ok() {
  dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.GetId >/dev/null 2>&1
}
avahi_ok() {
  dbus-send --system --print-reply --dest=org.freedesktop.Avahi / \
    org.freedesktop.Avahi.Server.GetVersionString >/dev/null 2>&1
}

pids=()

mkdir -p /run/dbus
if [ "${AP2C_MODE:-bridge}" = "services" ]; then
  # Host processes connect through the bind-mounted socket; dbus-daemon only accepts
  # peers whose uid it can resolve, so register the owner of the mounted directory.
  owner=$(stat -c %u /run/dbus)
  if [ "$owner" != 0 ] && ! getent passwd "$owner" >/dev/null; then
    useradd -M -u "$owner" -s /usr/sbin/nologin host-user
  fi
fi
if bus_ok; then
  log "using the host's system D-Bus"
else
  rm -f /run/dbus/pid /run/dbus/system_bus_socket
  dbus-uuidgen --ensure
  dbus-daemon --system --fork || { log "dbus-daemon failed"; exit 1; }
  for _ in $(seq 50); do bus_ok && break; sleep 0.1; done
  # let unprivileged host processes reach the bus when the socket dir is shared
  chmod 755 /run/dbus
fi

if avahi_ok; then
  log "using the host's avahi-daemon"
else
  rm -f /run/avahi-daemon/pid
  avahi-daemon --no-chroot --no-rlimits &
  pids+=($!)
  for _ in $(seq 100); do avahi_ok && break; sleep 0.1; done
  avahi_ok || { log "avahi-daemon did not start"; exit 1; }
fi

nqptp &
pids+=($!)

if [ "${AP2C_MODE:-bridge}" = "services" ]; then
  log "support services running (D-Bus, Avahi, NQPTP)"
else
  airplay2cast "$@" &
  pids+=($!)
fi

trap 'kill -TERM "${pids[@]}" 2>/dev/null; wait; exit 0' TERM INT
# exit (and let the restart policy recover) as soon as any component dies
wait -n "${pids[@]}"
rc=$?
log "a component exited (status $rc); stopping"
kill -TERM "${pids[@]}" 2>/dev/null
wait
exit "$rc"
