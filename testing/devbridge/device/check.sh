#!/bin/sh
#
# Judge the netplay game log this test produced, on this device.
#
#   check.sh MODE ROM_PATTERN [MIN_FPS [MAX_WAITS]]
#     MODE         shared | instanced | link | cable | dsp
#     ROM_PATTERN  part of the log directory name, e.g. Streets_of_Rage_2
#     MIN_FPS      fail if the session average falls below this
#     MAX_WAITS    fail if "Waiting for other player" came up more often
#
# Only logs newer than the marker run.sh touches before each test count, so a
# stale log from an earlier run can never pass for this one. Prints a summary;
# exits non-zero on anything that means the two devices did not play together.

#
#   check.sh armgba MODE [ROM_PATTERN]
#     Before the game opens: the armed session carries an armGBA Link Mode
#     agreed with the other device, and it is MODE. With ROM_PATTERN, once
#     the game is open: the shim took that agreed mode, did not find the
#     host running another (it refuses to play then), and in Together shows
#     only this device's console. Run early, so a test stops in seconds.
#   check.sh armgba-mismatch ROM_PATTERN
#     The guest's shim caught the host running a different Link Mode.

MODE=$1 ROM=$2 MIN_FPS=${3:-0} MAX_WAITS=${4:-}
MARK=/tmp/netplay-devbridge/started

fail() { echo "FAIL: $*"; exit 1; }

find_log() {
	log=$(ls -t $(find /mnt/SDCARD/.userdata/*/logs/netplay-games /mnt/SDCARD/Saves/netplay/logs/netplay-games -name '*.log' -newer "$MARK" 2>/dev/null) 2>/dev/null |
		grep -F "$1" | head -1)
	[ -n "$log" ] || fail "no netplay log for $1 since this test started (per-game logs need: ctl.sh set verbose_logs 1)"
	echo "log: $log"
}

if [ "$MODE" = armgba ]; then
	WANT=$2 ROM=${3:-}
	session=
	for f in /mnt/SDCARD/Saves/netplay/session /mnt/SDCARD/.userdata/shared/Netplay/session; do
		[ -f "$f" ] && { session=$f; break; }
	done
	[ -n "$session" ] || fail "no armed session"
	grep -q '^armgba_link_agreed=1$' "$session" ||
		fail "session has no armGBA agreement (run ctl.sh sync on the guest after writing armGBA.opt)"
	got=$(sed -n 's/^armgba_link_mode=//p' "$session" | tail -1)
	[ "$got" = "$WANT" ] || fail "session agreed armGBA Link Mode '$got'; this test needs '$WANT'"
	echo "session: armGBA Link Mode $got, agreed: $(grep '^option\.armgba_' "$session" | sed 's/^option\.armgba_//' | tr '\n' ' ')"
	[ -n "$ROM" ] || exit 0
	find_log "$ROM"
	bad=$(grep 'ARMGBA LINK MODE MISMATCH' "$log" | head -1)
	[ -z "$bad" ] || fail "$bad"
	grep -q "armGBA link mode=$WANT .*agreed with the peer" "$log" ||
		fail "game did not open in the agreed $WANT: $(grep 'armGBA link mode=' "$log" | head -1)"
	if [ "$WANT" = together ]; then
		line=$(grep 'armGBA Together: this device shows and plays' "$log" | head -1)
		[ -n "$line" ] || fail "Together did not lock this device to its own console"
		echo "${line#*\] }"
		# Together exchanges state, so the two builds must agree on it; a
		# refusal here is final, and found in seconds instead of at the end.
		i=0
		while :; do
			bad=$(grep -E 'refusing peer|Peer game or core is incompatible' "$log" | head -1)
			[ -z "$bad" ] || fail "${bad#*\] }"
			grep -q 'ROM and core identity match peer' "$log" && break
			i=$((i + 1))
			[ $i -lt 20 ] || fail "peers did not compare cores within 20 s"
			sleep 1
		done
		echo "peers agree on ROM and core"
	fi
	echo "PASS: armGBA opened in the agreed Link Mode $WANT"
	exit 0
fi
if [ "$MODE" = armgba-mismatch ]; then
	find_log "$ROM"
	line=$(grep 'ARMGBA LINK MODE MISMATCH' "$log" | head -1)
	[ -n "$line" ] || fail "no Link Mode mismatch was caught"
	echo "PASS: ${line#*\] }"
	exit 0
fi

find_log "$ROM"

# Correctness first: any of these means the consoles parted ways.
# The player-facing startup notice ("Core builds differ. Desyncs may occur")
# is a warning shown before play, not a desync: judge the session itself.
bad=$(grep -v 'startup notice: ' "$log" |
	grep -iE 'disagree|diverg|desync|synchronization timed out|could not adopt|ra: frame .*crc [1-9][0-9]* bad of' | head -3)
[ -z "$bad" ] || fail "$bad"
bad=$(grep -iE 'continued this game solo|host disconnected|Peer disconnected|Peer unavailable|Core cannot save state|recovery failed' "$log" | head -3)
[ -z "$bad" ] || fail "$bad"

# RetroArch uses a different progress/identity contract from LinkPlay.
# The PC interval judge checks timestamped coverage on both sides afterward.
if grep -q '^protocol=retroarch$' /mnt/SDCARD/Saves/netplay/session 2>/dev/null; then
    [ "$MODE" = shared ] || fail "UNSUPPORTED: RetroArch adapter cannot carry link-core NETPACKET"
    grep -q 'RetroArch client plays port 2\|playing as RetroArch port 2' "$log" || fail "RetroArch peer never entered player slot 2"
    grep -q 'ra: frame' "$log" || fail "NO_DATA: no RetroArch frame progress"
    echo "PASS: RetroArch player identity and frame progress; PC checks interval coverage"
    exit 0
fi

case $MODE in
shared)
	grep -q 'ROM and core identity match peer' "$log" || fail "peers never agreed on ROM and core"
	echo "checkpoints agreed: $(grep -c 'consoles agree\|checkpoint promoted\|in sync at frame' "$log")"
	pat='netplay-shim\] pacing:' ;;
instanced)
	grep -q 'instanced link agreed by both devices' "$log" || fail "instanced link never agreed"
	echo "paired checkpoints agreed: $(grep -c 'paired consoles agree' "$log")"
	grep 'GBLC serial' "$log" | tail -1 | sed 's/^.*GBLC serial/link cable:/'
	pat='instanced pacing:' ;;
cable)
	# Gambatte's own Game Link: the core opens a direct TCP socket (port
	# 56400, DC50 in hex) between the two devices, so the shim's netpacket
	# counters stay at 0 even in a healthy session. Judge the socket and
	# the pacing instead; the test's MARIO VS. LUIGI (or VS race) screens
	# are what prove the games themselves linked.
	grep -q 'link pacing:' "$log" || fail "no link pacing logged"
	awk '$4 == "01" && ($2 ~ /:DC50$/ || $3 ~ /:DC50$/) { found = 1 } END { exit !found }' \
		/proc/net/tcp || fail "Gambatte link socket (port 56400) is not established"
	echo "Gambatte link socket established (port 56400)"
	pat='link pacing:' ;;
link)
	grep -q 'link pacing:' "$log" || fail "no link traffic logged"
	dropped=$(grep 'link pacing:' "$log" | sed -n 's/.*, \([0-9]*\) dropped.*/\1/p' | awk '{s+=$1} END {print s+0}')
	packets=$(grep 'link pacing:' "$log" | sed -n 's/.* \([0-9]*\) packets.*/\1/p' | awk '{s+=$1} END {print s+0}')
	echo "link packets: $packets, dropped: $dropped"
	[ "$packets" -gt 0 ] || fail "link carried no packets"
	[ "$dropped" -eq 0 ] || fail "$dropped link packets dropped"
	pat='link pacing:' ;;
dsp)
	# DSperate local wireless, two log formats. Netplay's patched DSperate
	# (NextUI DSP.pak) logs per-window 'lan pacing:' lines and the radios
	# mask; spruce's stock DSperate logs one session summary instead
	# ('lan: hosting/joined', 'lan: reply/host waits ... timeouts ...')
	# with no pacing windows and no timestamps. The archive is written
	# when the game exits, so this runs after QUIT_GAME.
	if grep -q 'lan pacing:' "$log"; then
		# Both radios in the session (mask 0003) at some point, and the
		# game never gave up.
		grep -q 'radios 0003' "$log" || fail "the two consoles' radios were never on together"
		grep -q '^lan: .* not answering yet' "$log" && echo "guest waited for its host"
		_timeouts=$(grep 'lan pacing:' "$log" | sed -n 's/.*, \([0-9]*\) timeouts.*/\1/p')
		echo "MP timeouts: $(echo "$_timeouts" | awk '{s+=$1} END {print s+0}'), worst window $(echo "$_timeouts" | sort -n | tail -n 1)"
		# A healthy race peaks near 40 timeouts in a 10 s window. An outage of a
		# few seconds shows as hundreds in one window, and Mario Kart answers it
		# with "Communication error": both consoles then race alone, at full
		# speed, and every other check here still passes.
		[ "$(echo "$_timeouts" | sort -n | tail -n 1)" -le "${DSP_MAX_WINDOW_TIMEOUTS:-100}" ] 2>/dev/null ||
			fail "a 10 s window had more than ${DSP_MAX_WINDOW_TIMEOUTS:-100} MP timeouts (the link went dead)"
		pat='lan pacing:'
	else
		grep -qE '^lan: (hosting, player 0|joined, player [0-9]+)' "$log" ||
			fail "the two consoles' radios were never on together"
		grep -E '^lan: (hosting|joined)' "$log" | head -1
		_waits=$(sed -n 's/^lan: reply\/host waits \([0-9]*\),.*timeouts \([0-9]*\).*/\1 \2/p' "$log" | tail -1)
		_w=${_waits% *} _t=${_waits#* }
		[ -n "$_w" ] && [ "$_w" -gt 0 ] 2>/dev/null || fail "stock DSperate log has no MP waits"
		[ "$_t" -lt "$_w" ] 2>/dev/null || fail "every MP wait timed out (the peer never answered)"
		echo "MP waits: $_w, timeouts: $_t (stock DSperate log: no pacing windows, fps not judged)"
		pat=
	fi ;;
*)
	fail "unknown mode $MODE" ;;
esac

# Modes with no pacing pattern in the log (stock DSperate) have already
# judged what their format honestly supports; skip the window maths.
if [ -n "$pat" ]; then
# "600 frames in 10139ms" -> total frames and time across every window.
grep "$pat" "$log" | sed -n 's/.* \([0-9]*\) \(paired \)\{0,1\}frames in \([0-9]*\)ms.*/\1 \3/p' |
	awk -v min="$MIN_FPS" '
		{ f += $1; ms += $2; n++ }
		END {
			if (!n) { print "FAIL: no pacing windows"; exit 1 }
			fps = f * 1000 / ms
			printf "pacing: %d windows, %.1f fps average\n", n, fps
			if (fps < min) { printf "FAIL: below %s fps\n", min; exit 1 }
		}' || exit 1

# Lockstep never drops input: a late packet holds the game instead, so the
# network shows up as stalled frames. "stalled 13 frames (2%), longest 6" or
# "input stalls 12 (1%), longest 4" per window; the slowest window shows how
# bad the worst stretch felt.
grep "$pat" "$log" | sed -n 's/.* \([0-9]*\) \(paired \)\{0,1\}frames in \([0-9]*\)ms.*\(stalled\|input stalls\) \([0-9]*\).*longest \([0-9]*\).*/\1 \3 \5 \6/p' |
	awk '
		{ f += $1; s += $3; if ($4 > l) l = $4; w = $1 * 1000 / $2; if (!n++ || w < low) low = w }
		END { if (n) printf "stalls: %d of %d frames (%.1f%%), longest %d frames, slowest window %.1f fps\n", s, f, s * 100 / f, l, low }'
# How many times the player saw "Waiting for other player": waits are input
# stalls long enough to put it up (STALL_OVERLAY_FRAMES), pauses are the other
# device's menu or a frontend hold over 250 ms ("(menu open)"). Each counted
# once. A low stall share can hide them, so they are their own numbers. The
# session total includes the last partial window; the per-window counts are
# the fallback for a log cut short.
if grep "$pat" "$log" | grep -q ', waits [0-9]'; then
	totals=$(grep 'session totals:' "$log" | tail -1)
	count() { # <field>
		if [ -n "$totals" ]; then
			echo "$totals" | sed -n "s/.* $1 \([0-9]*\).*/\1/p"
		else
			grep "$pat" "$log" | sed -n "s/.*, $1 \([0-9]*\),.*/\1/p" | awk '{ s += $1 } END { print s + 0 }'
		fi
	}
	waits=$(count waits) pauses=$(count pauses)
	echo "waiting messages: $waits (input), ${pauses:-0} (other player paused)"
	[ -n "$MAX_WAITS" ] && [ "$waits" -gt "$MAX_WAITS" ] &&
		fail "\"Waiting for other player\" $waits times, more than $MAX_WAITS"
fi
fi
hiccups=$(grep -c 'frontend stalled - told peer' "$log")
[ "$hiccups" -gt 0 ] && echo "frontend stalls reported to peer: $hiccups"
echo "PASS"
