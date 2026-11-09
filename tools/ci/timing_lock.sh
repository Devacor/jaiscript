#!/usr/bin/env bash
# The machine-wide timing lock: one timing run at a time, taken in arrival order. Builds, suites and gates never need it;
# a measurement does, so other work on the machine cannot skew it.
#
#   tools/ci/timing_lock.sh run "<who: what>" -- <command...>   # queue, run the command holding the lock, release
#   tools/ci/timing_lock.sh wait                               # block while anyone holds the lock
#   tools/ci/timing_lock.sh status                             # who holds it, who is queued
#   tools/ci/timing_lock.sh release-owner "<exact owner line>" # free a lock its owner left behind
#   . tools/ci/timing_lock.sh; lockq_take "<who: what>"; ...; lockq_release
#
# JAI_TIMING_LOCK names the lock directory (default: jai_timing.lock in the system temp directory); the queue is
# JAI_TIMING_QUEUE (default: jai_timing.queue beside it).
#
# Protocol (any language may implement it):
#  - Join the queue by creating a ticket file in the queue directory named <epoch-nanoseconds>-<anything unique>.
#  - While waiting, touch the ticket at least every 20 s; a ticket untouched for 90 s is stale and anyone may delete it.
#  - Take the lock only while holding the oldest live ticket: mkdir the lock directory, write owner.txt (with the pid),
#    then delete the ticket. Release by removing owner.txt and the directory, only when owner.txt is yours.
#  - Release from a cleanup path: an exit, an interrupt and a kill all release.
#  - A lock taken before the machine last booted has no living owner, and the next taker clears it.

lockq_temp_dir() {
	local dir="${TMPDIR:-${TEMP:-${TMP:-/tmp}}}"
	command -v cygpath >/dev/null 2>&1 && dir="$(cygpath -u "$dir")"
	echo "$dir"
}

LOCKQ_LOCK="${JAI_TIMING_LOCK:-$(lockq_temp_dir)/jai_timing.lock}"
LOCKQ_DIR="${JAI_TIMING_QUEUE:-$(dirname "$LOCKQ_LOCK")/jai_timing.queue}"
LOCKQ_TICKET=""
LOCKQ_HELD=0

lockq_clear_prereboot() {
	[ -d "$LOCKQ_LOCK" ] && [ -r /proc/uptime ] || return 0
	local boot taken
	boot=$(( $(date +%s) - $(cut -d. -f1 /proc/uptime) ))
	taken=$(stat -c %Y "$LOCKQ_LOCK" 2>/dev/null || echo "$boot")
	if [ "$taken" -lt "$boot" ]; then
		echo "clearing a timing lock taken before the last boot ($(cat "$LOCKQ_LOCK/owner.txt" 2>/dev/null))"
		rm -f "$LOCKQ_LOCK/owner.txt"
		rmdir "$LOCKQ_LOCK" 2>/dev/null
	fi
	return 0
}

lockq_take() {
	local owner="$1" deadline="${2:-14400}" start=$SECONDS oldest
	mkdir -p "$LOCKQ_DIR"
	LOCKQ_TICKET="$LOCKQ_DIR/$(date +%s%N)-$$-$RANDOM"
	echo "$owner" > "$LOCKQ_TICKET"
	trap 'lockq_release' EXIT
	trap 'lockq_release; exit 130' INT TERM HUP
	while true; do
		touch "$LOCKQ_TICKET"
		find "$LOCKQ_DIR" -type f -mmin +1.5 ! -path "$LOCKQ_TICKET" -delete 2>/dev/null
		lockq_clear_prereboot
		oldest=$(ls "$LOCKQ_DIR" 2>/dev/null | sort | head -n 1)
		if [ "$LOCKQ_DIR/$oldest" = "$LOCKQ_TICKET" ] && mkdir "$LOCKQ_LOCK" 2>/dev/null; then
			echo "$owner pid=$$ winpid=$(cat /proc/$$/winpid 2>/dev/null) $(date +%H:%M)" > "$LOCKQ_LOCK/owner.txt"
			rm -f "$LOCKQ_TICKET"
			LOCKQ_TICKET=""
			LOCKQ_HELD=1
			return 0
		fi
		if [ $((SECONDS - start)) -ge "$deadline" ]; then
			rm -f "$LOCKQ_TICKET"
			LOCKQ_TICKET=""
			return 1
		fi
		sleep 3
	done
}

lockq_release() {
	if [ -n "$LOCKQ_TICKET" ]; then
		rm -f "$LOCKQ_TICKET"
		LOCKQ_TICKET=""
	fi
	if [ "$LOCKQ_HELD" = 1 ] && grep -q "pid=$$ " "$LOCKQ_LOCK/owner.txt" 2>/dev/null; then
		rm -f "$LOCKQ_LOCK/owner.txt"
		rmdir "$LOCKQ_LOCK" 2>/dev/null
	fi
	LOCKQ_HELD=0
}

lockq_wait() {
	lockq_clear_prereboot
	while [ -d "$LOCKQ_LOCK" ]; do
		sleep 15
		lockq_clear_prereboot
	done
}

lockq_status() {
	if [ -d "$LOCKQ_LOCK" ]; then echo "held: $(cat "$LOCKQ_LOCK/owner.txt" 2>/dev/null || echo "(no owner file)")"; else echo "free"; fi
	local ticket
	for ticket in $(ls "$LOCKQ_DIR" 2>/dev/null | sort); do echo "queued: $(cat "$LOCKQ_DIR/$ticket" 2>/dev/null)"; done
}

lockq_release_owner() {
	if [ "$(cat "$LOCKQ_LOCK/owner.txt" 2>/dev/null)" = "$1" ]; then
		rm -f "$LOCKQ_LOCK/owner.txt"
		rmdir "$LOCKQ_LOCK"
		echo "released"
	else
		echo "not released; the owner is: $(cat "$LOCKQ_LOCK/owner.txt" 2>/dev/null || echo none)"
		return 1
	fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	case "${1:-}" in
		run)
			owner="${2:?usage: timing_lock.sh run \"<who: what>\" -- <command...>}"
			shift 2
			[ "${1:-}" = "--" ] && shift
			[ "$#" -gt 0 ] || { echo "timing_lock.sh run: no command given" >&2; exit 2; }
			lockq_take "$owner" || { echo "timing_lock.sh: gave up waiting for the timing lock" >&2; exit 1; }
			"$@"
			rc=$?
			lockq_release
			exit $rc
			;;
		wait) lockq_wait ;;
		status) lockq_status ;;
		release-owner) lockq_release_owner "${2:?usage: timing_lock.sh release-owner \"<exact owner line>\"}" ;;
		*) echo "usage: timing_lock.sh run \"<who: what>\" -- <command...> | wait | status | release-owner \"<owner line>\"" >&2; exit 2 ;;
	esac
fi
