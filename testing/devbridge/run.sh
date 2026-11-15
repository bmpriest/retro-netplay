#!/bin/sh
#
# Run the two-device gameplay tests through DevBridge.
#
#   run.sh [options] HOST_ADDR GUEST_ADDR TEST...
#
#   --record        capture reference screens instead of checking them
#   --record-only P only keep the recorded refs of platform P (spruce-my282),
#                   leaving the partner's existing references as they are
#   --port N        SSH port for both devices (default 22)
#   --identity KEY  SSH key (default ~/.ssh/id_ed25519)
#   --user USER     SSH user for both devices (default root; spruce: spruce).
#                   With $DEVBRIDGE_PASSWORD set, log in with that password.
#                   A device given as USER@ADDR logs in as USER instead, with
#                   $DEVBRIDGE_PASSWORD_USER if that is set (e.g. an A30 as
#                   root and a Mini Flip as spruce@ADDR in one run).
#   --keep-going    run every line even after a failure
#   --play-only     the game is already running on both (another way of
#                   starting it, such as pure RetroArch's menus): skip the
#                   test's launch, its log checks and its quit
#
# HOST_ADDR is the device that hosts the armed session (Player 1), GUEST_ADDR
# the one that joined. Either may be adb:SERIAL for a device on USB ADB, which
# mixes with an SSH one (a NextUI Brick on USB against a spruce device on WiFi).
# ADDR:PORT reaches one device on its own SSH port, such as a tunnel through
# the other device and the USB cable when its WiFi drops. Both must already be armed together and sitting in
# DevBridge, not in the Netplay app. See README.md.
#
# Tests use three placeholders, filled in here:
#   {host_refs} {guest_refs}  refs/<platform> for that device
#   {helpers}                 where device/*.sh were copied on the device
#
# On spruce (App/Netplay), refs are refs/spruce-<platform>, and OPEN_GAME's
# NextUI ROM folders ("Game Boy (GB)/...") become spruce's ("GB/...").

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
DEVBRIDGE=${DEVBRIDGE:-$ROOT/../DevBridge.pak}
HELPERS=/tmp/netplay-devbridge
OUT=$ROOT/testing/results/devbridge

record=0 record_only= port=22 identity=$HOME/.ssh/id_ed25519 keep= user=root play_only=0
while [ $# -gt 0 ]; do
	case $1 in
	--record) record=1 ;;
	--record-only) record=1; record_only=$2; shift ;;
	--port) port=$2; shift ;;
	--identity) identity=$2; shift ;;
	--user) user=$2; shift ;;
	--keep-going) keep=--keep-going ;;
	--play-only) play_only=1 ;;
	-*) echo "unknown option $1" >&2; exit 2 ;;
	*) break ;;
	esac
	shift
done
[ $# -ge 3 ] || { sed -n '3,23p' "$0" >&2; exit 2; }
host=$1 guest=$2; shift 2

[ -f "$DEVBRIDGE/tools/devbridge.py" ] || { echo "DevBridge not found at $DEVBRIDGE (set DEVBRIDGE)" >&2; exit 2; }

askpass=
if [ -n "${DEVBRIDGE_PASSWORD:-}" ]; then
	askpass=$(mktemp "${TMPDIR:-/tmp}/netplay-askpass.XXXXXX")
	# ssh's prompt is "USER@HOST's password: ": a per-user password wins.
	cat > "$askpass" <<'ASKPASS'
#!/bin/sh
u=${1%%@*}
case $u in *[!A-Za-z0-9_]*|'') u= ;; esac
[ -n "$u" ] && eval "p=\${DEVBRIDGE_PASSWORD_$u:-}" && [ -n "$p" ] && { printf '%s\n' "$p"; exit 0; }
printf '%s\n' "$DEVBRIDGE_PASSWORD"
ASKPASS
	chmod 700 "$askpass"
	trap 'rm -f "$askpass"' EXIT
fi

sshd() {
	addr=$1; shift
	u=$user sp=$port
	case $addr in *@*) u=${addr%@*}; addr=${addr##*@} ;; esac
	case $addr in adb:*) ;; *:*) sp=${addr##*:}; addr=${addr%:*} ;; esac
	# adb shell ends its lines with CRLF.
	case $addr in adb:*)
		adb -s "${addr#adb:}" shell "$@" | tr -d '\r'
		return ;;
	esac
	if [ -n "$askpass" ]; then
		# Mixed fleets: some devices take the key, others only the password.
		ssh -p "$sp" -i "$identity" -o BatchMode=yes -o ConnectTimeout=10 \
			-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
			"$u@$addr" "$@" 2>/dev/null && return
		SSH_ASKPASS=$askpass SSH_ASKPASS_REQUIRE=force DEVBRIDGE_PASSWORD=$DEVBRIDGE_PASSWORD \
			setsid -w ssh -p "$sp" -o ConnectTimeout=10 -o PubkeyAuthentication=no \
			-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
			"$u@$addr" "$@"
		return
	fi
	ssh -p "$sp" -i "$identity" -o BatchMode=yes -o ConnectTimeout=10 \
		-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
		"$u@$addr" "$@"
}

# NextUI: the Tools/<platform> holding Netplay.pak. spruce: spruce-<the
# platform its App/Netplay build runs as>.
platform_of() {
	p=$(sshd "$1" 'for d in /mnt/SDCARD/Tools/*/Netplay.pak; do [ -d "$d" ] && basename "$(dirname "$d")"; done
		if [ -f /mnt/SDCARD/App/Netplay/launcher/firmware.sh ]; then
			. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1
			. /mnt/SDCARD/App/Netplay/launcher/firmware.sh
			echo "spruce-$(np_spruce_platform "$PLATFORM")"
		fi' | head -1)
	[ -n "$p" ] || { echo "no Netplay on $1" >&2; exit 1; }
	echo "$p"
}

# For a spruce device's lines (and all: lines when both are spruce):
#   - NextUI's "Name (TAG)/rom" becomes "TAG/rom";
#   - a line ending "# not-spruce" is dropped, and "#spruce LINE" becomes LINE,
#     for steps spruce's RetroArch needs done differently.
spruce_roms() {
	roles=
	case $host_platform in spruce-*) roles="$roles host" ;; esac
	case $guest_platform in spruce-*) roles="$roles guest" ;; esac
	case "$roles" in " host guest") roles="$roles all" ;; esac
	awk -v roles="$roles" '
		function spruce(line,   r) { r = line; sub(/:.*/, "", r); return index(roles " ", " " r " ") > 0 }
		/^#spruce / { if (!spruce(substr($0, 9))) next; $0 = substr($0, 9) }
		/# not-spruce$/ { if (spruce($0)) next; sub(/ *# not-spruce$/, "") }
		{
			if ($0 ~ /^[a-z]+:OPEN_GAME / && spruce($0))
				$0 = gensub(/"[^"\/]* \(([A-Z0-9]+)\)\//, "\"\\1/", 1)
			print
		}'
}

# --play-only: from `all:WAIT_FOR game` on (what launched the game is gone),
# without check.sh, quit.sh or the closing wait for DevBridge.
play_only() {
	if [ $play_only = 1 ]; then
		awk '/^all:WAIT_FOR game/ { on = 1 } !on { next }
		     /check\.sh|quit\.sh|^all:WAIT_FOR devbridge/ { next } { print }'
	else
		cat
	fi
}

# After each WAIT_FOR game, make the devices it waited for prove they are in
# the game the test just opened (host:WAIT_FOR game checks only the host: in
# dsp-mkds the guest launches after it). A device stuck at spruce's save-transfer prompt,
# still in the previous game, or back in a menu fails the test here rather
# than playing a different game from its partner for minutes.
sync_guard() {
	awk '
		/^[a-z]+:OPEN_GAME / {
			line = $0
			sub(/^[^"]*"/, "", line); sub(/".*$/, "", line)
			n = split(line, parts, "/"); rom = parts[n]
		}
		{ print }
		/^[a-z]+:WAIT_FOR game/ {
			role = $0; sub(/:.*/, "", role)
			if (rom != "") print role ":TRM sh '"$HELPERS"'/sync.sh game \"" rom "\""
		}
	'
}

# A screen that depends on the partner too (the host's core options, such as
# Gambatte's palette, reach a NextUI guest) has its reference at
# refs/<platform>/with-<partner platform>/NAME.png, used in place of
# refs/<platform>/NAME.png when it exists. Not while recording: a recorded
# screen lands at refs/<platform>/NAME.png, to be moved by hand.
pair_refs() {
	awk -v refs="$HERE/refs" -v hp="$host_platform" -v gp="$guest_platform" '
		function exists(f,   line) { if ((getline line < f) > 0) { close(f); return 1 } return 0 }
		{
			out = ""; rest = $0
			while (match(rest, /\/refs\/[^\/ ]+\/[^\/ ]+\.png/)) {
				tok = substr(rest, RSTART, RLENGTH)
				split(tok, t, "/"); plat = t[3]; name = t[4]
				partner = (plat == hp) ? gp : hp
				alt = refs "/" plat "/with-" partner "/" name
				if (exists(alt)) tok = "/refs/" plat "/with-" partner "/" name
				out = out substr(rest, 1, RSTART - 1) tok
				rest = substr(rest, RSTART + RLENGTH)
			}
			print out rest
		}'
}

push_helpers() {
	sshd "$1" "mkdir -p $HELPERS"
	for f in "$HERE"/device/*.sh; do
		case $1 in
		adb:*) adb -s "${1#adb:}" push "$f" "$HELPERS/" >/dev/null ;;
		*) sshd "$1" "cat > $HELPERS/$(basename "$f")" < "$f" ;;
		esac
	done
}

# DevBridge's way to reach each device: --adb SERIAL=ALIAS or --connect.
devbridge_target() {
	case $1 in
	adb:*) echo "--adb ${1#adb:}=$2" ;;
	*:*) echo "--connect ${1%:*}:$2:${1##*:}" ;;
	*) echo "--connect $1:$2" ;;
	esac
}

auth="--identity $identity"
# With a password available, keep the key too: mixed rigs have devices of both kinds.
[ -z "$askpass" ] || auth="--identity $identity --password-fallback"

host_platform=$(platform_of "$host")
guest_platform=$(platform_of "$guest")
echo "host $host ($host_platform), guest $guest ($guest_platform)"
if [ "${DEVBRIDGE_HELPERS_READY:-0}" != 1 ]; then
	push_helpers "$host"
	push_helpers "$guest"
fi

# An armed cable session stays armed with the cable unplugged (a replug
# relinks), so "armed" says nothing about the link. Before anything launches,
# a cable session must be linked on both devices; otherwise the guest's game
# waits for a host it cannot reach. (2026-11-15: a suite started with both
# devices on chargers.)
cable_state() { sshd "$1" "sh $HELPERS/ctl.sh status 2>/dev/null" | sed -n 's/^cable=//p'; }
if [ $play_only = 0 ]; then
	hc=$(cable_state "$host"); gc=$(cable_state "$guest")
	if { [ -n "$hc" ] && [ "$hc" != off ]; } || { [ -n "$gc" ] && [ "$gc" != off ]; }; then
		if [ "$hc" != linked ] || [ "$gc" != linked ]; then
			echo "cable session not linked: host cable=${hc:-?}, guest cable=${gc:-?} - check the cable before testing" >&2
			exit 1
		fi
		echo "cable linked on both devices"
	fi
fi
mkdir -p "$OUT" "$HERE/refs/$host_platform" "$HERE/refs/$guest_platform"

status=0
for test in "$@"; do
	name=$(basename "$test" .test)
	session=$name-$(date +%Y%m%d-%H%M%S)
	script=$(mktemp "${TMPDIR:-/tmp}/netplay-devbridge.XXXXXX")

	{
		echo "SESSION $session $OUT"
		sed -e "s#{host_refs}#$HERE/refs/$host_platform#g" \
		    -e "s#{guest_refs}#$HERE/refs/$guest_platform#g" \
		    -e "s#{helpers}#$HELPERS#g" "$test" | spruce_roms | play_only |
		if [ $record = 1 ]; then
			# VERIFY_WAIT REF TIMEOUT -> wait the whole timeout, then capture
			# the screen under a name that says where its reference belongs.
			# VERIFY_UNTIL presses until a screen shows, so it cannot become a
			# plain wait: under --record-only it steers by the partner
			# platform's reference of the same name (the same game screen),
			# then captures. A provisional reference of the platform's own
			# (e.g. one scaled from another device) steers first. Lines for platforms not being recorded keep
			# verifying against their existing references.
			partner=$host_platform
			[ "$partner" != "$record_only" ] || partner=$guest_platform
			awk -v only="$record_only" -v partner="$partner" -v refs="$HERE/refs" '
			function exists(f) { return (getline line < f) > 0 ? (close(f) || 1) : 0 }
			{
				if (!match($0, /^[a-z]+:VERIFY_(WAIT|UNTIL) /)) { print; next }
				split($0, w, " "); split(w[1], p, ":")
				n = split(w[2], parts, "/")
				dir = parts[n - 1]
				if (only != "" && dir != only) { print; next }
				ref = parts[n]; sub(/\.png$/, "", ref)
				if (w[1] ~ /:VERIFY_WAIT$/) print "WAIT " w[3]
				else {
					# Steer by the recorded platform provisional reference
					# when one exists, else by the partner platform one.
					steer = refs "/" dir "/" parts[n]
					if (!exists(steer)) steer = refs "/" partner "/" parts[n]
					if (only != "" && partner != only && exists(steer)) {
						line2 = $0; sub(w[2], steer, line2); print line2
					} else print "WAIT " w[3]
				}
				print p[1] ":SCREEN rec__" dir "__" ref
			}'
		else
			pair_refs
		fi
	} | sync_guard > "$script"
	if [ -n "${DEVBRIDGE_ATTEMPT_DIR:-}" ]; then
		cp "$script" "$DEVBRIDGE_ATTEMPT_DIR/rendered-$name.test"
	fi

	# check.sh only trusts logs newer than this. The host's confirmed checkpoint
	# outlives a clean exit (a relaunched host resumes from it), so without
	# clearing it a test would start where the previous run of the game ended.
	# A game already running (--play-only) keeps its checkpoint.
	if [ $play_only = 1 ]; then
		sshd "$host" "touch $HELPERS/started"
	else
		sshd "$host" "touch $HELPERS/started; rm -f /tmp/netplay-host-*.state"
	fi
	sshd "$guest" "touch $HELPERS/started"

	# spruce stops a core-changing launch to ask about transferring the
	# save; no test can answer it mid-run, so the question is off for the
	# length of the test and the setting is put back afterwards.
	sshd "$host" "sh $HELPERS/sync.sh prompt-off" || true
	sshd "$guest" "sh $HELPERS/sync.sh prompt-off" || true
	# keys.sh drives the game through DevBridge's input daemon; where the
	# app did not start it (an A30 reports input=unavailable), tests would
	# press nothing at all, so start it where it exists.
	sshd "$host" "[ -x /mnt/SDCARD/App/DevBridge/bin/input-on ] && /mnt/SDCARD/App/DevBridge/bin/input-on" || true
	sshd "$guest" "[ -x /mnt/SDCARD/App/DevBridge/bin/input-on ] && /mnt/SDCARD/App/DevBridge/bin/input-on" || true

	echo "== $name"
	# -P, not -I: VERIFY needs Pillow, often only in user site-packages.
	# shellcheck disable=SC2046
	if python3 -P "$DEVBRIDGE/tools/devbridge.py" $auth --port "$port" \
		--user "$user" $(devbridge_target "$host" host) $(devbridge_target "$guest" guest) \
		$keep run "$script"; then
		echo "== $name: PASS"
	else
		echo "== $name: FAIL (session $OUT/$session)"
		status=1
	fi
	rm -f "$script"
	sshd "$host" "sh $HELPERS/sync.sh prompt-restore" || true
	sshd "$guest" "sh $HELPERS/sync.sh prompt-restore" || true

	if [ $record = 1 ]; then
		for f in "$OUT/$session"/rec__*.png; do
			[ -f "$f" ] || continue
			base=$(basename "$f" .png); base=${base#rec__}
			plat=${base%%__*}; ref=${base#*__}; ref=${ref%-host}; ref=${ref%-guest}
			[ -z "$record_only" ] || [ "$plat" = "$record_only" ] || continue
			mkdir -p "$HERE/refs/$plat"
			cp "$f" "$HERE/refs/$plat/$ref.png"
			echo "recorded refs/$plat/$ref.png"
		done
	fi
done
exit $status
