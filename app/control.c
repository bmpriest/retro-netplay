/*
 * Session control shared by the NextUI app and spruce's headless engine (see
 * control.h). Kept apart from main.c's screens so spruce's netplay.elf links
 * no SDL.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "control.h"

char   status[128] = "";
/* Appended to the armed line: whether local compatibility cores were selected
 * is exactly the kind of thing that is otherwise invisible until a desync. */
char   core_note[64] = "";

NS_Check checks[8];
int      check_count = 0;
NS_CheckResult check_worst = NS_CHECK_OK;

char local_ip[NS_IP_LEN] = "?";
char hs_ssid[NS_SSID_LEN];
char hs_psk[NS_PSK_LEN];
int  hosting_hotspot;

void (*NC_onProgress)(const char* title, const char* note, const char* stage);

//////////////////////////////////////////////////////////////////////////////
// cores
//
// What each system's row shows: a check when it plays over Netplay here, two
// when it plays in instanced (dual) mode, a hollow circle when its pak is here
// but not set up for Netplay, a dash when there is no pak. Status only, except
// where Netplay installs or configures something (">" or a pak to add).
//////////////////////////////////////////////////////////////////////////////

const CoreGroup CORE_GROUPS[] = {
	{ "GB", "GB/GBC" }, { "GBA", "GBA" }, { "GG", "Game Gear" }, { "NDS", "NDS" },
	{ "FC", "NES / FC" }, { "SFC", "SNES / SFC" }, { "MD", "Genesis / MegaDrive" },
	{ "SEGACD", "SegaCD / 32X" }, { "SMS", "Master System" }, { "PS", "PS" },
};

const CoreDef CORE_DEFS[] = {
	{ "GB", "Gambatte", "gambatte", CK_GAMBATTE, GROUP_GB },
	{ "GB", "armGBA", "armgba", CK_ARMGBA, GROUP_GB },
	{ "GBA", "gpSP", "gpsp", CK_GBA, GROUP_GBA },
#ifdef NETPLAY_SPRUCE
	{ "GBA", "mGBA", "mgba", CK_GBA, GROUP_GBA },
#else
	{ "MGBA", "mGBA Dual", "mgba", CK_MGBA, GROUP_GBA },
#endif
	{ "GBA", "armGBA", "armgba", CK_ARMGBA, GROUP_GBA },
	{ "GG", "PicoDrive", "picodrive", CK_UNSUPPORTED, GROUP_GG },
	{ "GG", "Genesis Plus GX", "genesis_plus_gx", CK_SHIM, GROUP_GG },
	{ "GG", "Gearsystem", "gearsystem", CK_SHIM, GROUP_GG },
#ifdef NETPLAY_SPRUCE
	{ "DSP", "DSperate", "dsperate", CK_SPRUCE_DS, GROUP_NDS },
	{ "NDS", "DraStic", "drastic", CK_SPRUCE_DS, GROUP_NDS },
#else
	{ "DSP", "DSperate", "dsperate", CK_DSP, GROUP_NDS },
	{ "NDS", "DraStic", "drastic", CK_NDS, GROUP_NDS },
#endif
	{ "FC", "FCEUmm", "fceumm", CK_SHIM, GROUP_NES },
	{ "SFC", "Snes9x", "snes9x", CK_SHIM, GROUP_SNES },
	{ "SFC", "Snes9x 2005", "snes9x2005", CK_SHIM, GROUP_SNES },
	{ "SFC", "Supafaust", "mednafen_supafaust", CK_SHIM, GROUP_SNES },
	{ "SFC", "ChimeraSNES", "chimerasnes", CK_UNSUPPORTED, GROUP_SNES },
	{ "SFC", "Snes9x 2010", "snes9x2010", CK_UNSUPPORTED, GROUP_SNES },
	{ "MD", "PicoDrive", "picodrive", CK_SHIM, GROUP_MD },
	{ "MD", "Genesis Plus GX", "genesis_plus_gx", CK_UNSUPPORTED, GROUP_MD },
	{ "SEGACD", "PicoDrive", "picodrive", CK_SHIM, GROUP_CD32 },
	{ "SMS", "PicoDrive", "picodrive", CK_SHIM, GROUP_SMS },
	{ "SMS", "Genesis Plus GX", "genesis_plus_gx", CK_SHIM, GROUP_SMS },
	{ "SMS", "Gearsystem", "gearsystem", CK_SHIM, GROUP_SMS },
	{ "PS", "PCSX ReARMed", "pcsx_rearmed", CK_SHIM, GROUP_PS },
};
const int core_def_count = (int)(sizeof(CORE_DEFS) / sizeof(CORE_DEFS[0]));

CoreState core_state[sizeof(CORE_DEFS) / sizeof(CORE_DEFS[0])];
bool cores_fresh;

bool gambatte_dual(void) {
	return NS_settings()->inst_core[0] && NS_gambatteDualAvailable();
}

bool mgba_instanced(void) {
	return NS_settings()->inst_core[1];
}

/* spruce: no MGBA.pak, the paired core ships with Netplay. */
bool mgba_paired(void) {
	return mgba_instanced() && NS_mgbaDualAvailable();
}

/* Store the instanced choices in the settings' existing shape. */
void set_instanced(int core, bool on) {
	NS_Settings* c = NS_settings();
	if (c->instanced == NS_INST_ALL)
		for (int i = 0; i < NS_INST_CORES; i++) c->inst_core[i] = true;
	c->inst_core[core] = on;
	bool any = false;
	for (int i = 0; i < NS_INST_CORES; i++) any = any || c->inst_core[i];
	c->instanced = any ? NS_INST_SELECTED : NS_INST_OFF;
	NS_settingsSave();
}

static void refresh_core(const CoreDef* d, CoreState* st) {
	memset(st, 0, sizeof(*st));
	switch (d->kind) {
#ifdef NETPLAY_SPRUCE
	case CK_SPRUCE_DS: {
		char path[512];
		snprintf(path, sizeof(path), "%s/Emu/NDS/%s", NS_sdPath(), d->core);
		if (!strcmp(d->core, "drastic") && access(path, X_OK) != 0)
			snprintf(path, sizeof(path), "%s/Emu/NDS/drastic64", NS_sdPath());
		bool present = access(path, X_OK) == 0;
		bool radio = true;
		if (!strcmp(d->core, "drastic")) {
			snprintf(path, sizeof(path), "%s/bin/%s/libdrastic_wifi.so", NS_pakPath(), PLATFORM);
			radio = access(path, R_OK) == 0;
		} else if (present) {
			/* The public path may already be covered by our launcher wrapper. */
			if (access("/tmp/netplay/nds/dsperate", X_OK) == 0)
				snprintf(path, sizeof(path), "/tmp/netplay/nds/dsperate");
			radio = NS_dsperateLanAvailable(path);
		}
		st->mark = present && radio ? MARK_CHECK : present ? MARK_CIRCLE : MARK_DASH;
		st->dim = !present;
		st->state = present && radio ? "ready" : present ? "present" : "absent";
		break;
	}
#endif
	case CK_SHIM:
	case CK_GAMBATTE:
	case CK_GBA:
	case CK_ARMGBA:
	case CK_UNSUPPORTED: {
		bool present = NS_coreInstalled(d->core);
		bool supported = d->kind != CK_UNSUPPORTED;
		/* Netplay picks one core per system (app/engines.c); the others it
		 * can drive are only available. The pick runs paired where that is
		 * its engine (Gambatte Dual, mGBA Dual). */
		char pick[32];
		bool table = NS_engineCore(d->tag, pick, sizeof(pick));
		bool picked = !table || !strcmp(pick, d->core);
		bool dual = picked && table && d->kind == CK_GAMBATTE && NS_gambatteDualAvailable();
		bool paired = picked && table && !strcmp(d->core, "mgba") && NS_mgbaDualAvailable();
		bool works = supported && (strcmp(d->core, "mgba") || paired);
		st->mark = !present ? MARK_DASH : !works || !picked ? MARK_CIRCLE :
				   dual || paired ? MARK_DOUBLE : MARK_CHECK;
		st->dim = !present;
		st->state = !present ? "absent" : !supported ? "unsupported" : !picked && works ? "available" :
					dual ? "dual" : paired ? "paired" : works ? "ready" : "present";
		/* Options left to the player: link settings (armGBA, gpSP). Which
		 * core, paired or not, and armGBA's Link Mode are Netplay's call. */
		st->more = present && (d->kind == CK_ARMGBA || (d->kind == CK_GBA && !strcmp(d->core, "gpsp")));
		break;
	}
	case CK_DSP:
	case CK_MGBA:
	case CK_NDS: {
		NS_ManagedPak k = d->kind == CK_DSP ? NS_PAK_DSP : d->kind == CK_MGBA ? NS_PAK_MGBA : NS_PAK_NDS;
		if (!NS_pakAvailable(k)) {
			st->mark = MARK_DASH;
			st->dim = true;
			st->state = "unavailable";
			break;
		}
		st->pak_ok = NS_pakStatus(k, &st->pak);
		const NS_PakStatus* p = &st->pak;
		bool ours = p->managed && p->installed && !p->missing;
		if (d->kind == CK_DSP) {
			if (ours && NS_settings()->dsperate) { st->mark = MARK_CHECK; st->state = "ready"; }
			else if (p->present) { st->mark = MARK_CIRCLE; st->state = "off"; }
			else { st->mark = MARK_DASH; st->state = "absent"; }
		} else if (d->kind == CK_MGBA) {
			st->more = true;
			if (ours && mgba_instanced()) { st->mark = MARK_DOUBLE; st->state = "instanced"; }
			else if (p->present) { st->mark = MARK_CIRCLE; st->state = "installed"; }
			else { st->mark = MARK_DASH; st->state = "absent"; }
		} else {
			if (ours) { st->mark = MARK_CHECK; st->state = "ready"; }
			else if (p->base_installed && p->unsupported) { st->mark = MARK_CIRCLE; st->state = "unsupported"; }
			else if (p->base_installed) { st->mark = MARK_CIRCLE; st->state = "present"; }
			else { st->mark = MARK_DASH; st->state = "absent"; }
		}
		break;
	}
	}
}

/* Reading every pak's status costs a few hashes and launch.sh reads, so it is
 * done on entering the list and after a change, never per frame. */
void refresh_cores(void) {
	for (int i = 0; i < CORE_COUNT; i++) refresh_core(&CORE_DEFS[i], &core_state[i]);
	cores_fresh = true;
}

void NC_abortArm(bool may_have_activated) {
	/* One rollback path owns everything an arm attempt can start. NS_disarm is
	 * deliberately reserved for attempts that may have created a session or
	 * partial bindings; a failed preflight must not remove bindings retained
	 * from an earlier, deliberately ended session. */
	NS_announceStop();
	NS_compatServeStop();
	if (may_have_activated)
		NS_disarm();
	else
		NS_hotspotStop();
	hosting_hotspot = 0;
}

/* How a joiner decides the protocol (docs/analysis/retroarch-protocol.md):
 * an explicit choice wins; else a host found by a version 2 announcement said
 * which it runs; else a RetroArch host answering RetroArch's POKE probe makes
 * it RetroArch; else Linkplay. Set by the caller just before do_arm. */
int join_protocol_override = -1;
NS_Protocol join_peer_protocol = NS_PROTO_LINKPLAY;

static NS_Protocol decide_join_protocol(const char* peer) {
	if (join_protocol_override >= 0) return (NS_Protocol)join_protocol_override;
	if (join_peer_protocol == NS_PROTO_RETROARCH) return NS_PROTO_RETROARCH;
	return NS_probeRetroArch(peer, 1500) ? NS_PROTO_RETROARCH : NS_PROTO_LINKPLAY;
}

NC_ArmResult NC_arm(NS_Role role, const char* peer) {
	char err[96] = "";
	NS_Protocol protocol = NS_PROTO_LINKPLAY;
	if (role == NS_ROLE_CLIENT) {
		protocol = decide_join_protocol(peer);
		join_protocol_override = -1;
		join_peer_protocol = NS_PROTO_LINKPLAY;
		NS_setArmProtocol(protocol);
	} else {
		protocol = NS_armProtocol();
	}

	/* Never overwrite a live session. Re-arming used to orphan the existing
	 * peer and could replace its transport underneath a running game. */
	if (NS_isArmed()) {
		snprintf(status, sizeof(status), "End the current session with X first.");
		return NC_ARM_FAILED;
	}

	/* These are launch-path invariants, not optional diagnostics. If one fails,
	 * arming would appear successful but games would either bypass the shim or
	 * use the wrong save path. Cross-platform compatibility is decided by the
	 * core manifest exchange below, so the old crude platform check is gone. */
	check_count = NS_runChecks(checks, 8, &check_worst);
	if (check_worst == NS_CHECK_FAIL) {
		NC_abortArm(false);
		snprintf(status, sizeof(status), "Checks failed - not arming");
		return NC_ARM_CHECKS_FAILED;
	}

	if (!NS_arm(role, peer, err, sizeof(err))) {
		NC_abortArm(true);
		snprintf(status, sizeof(status), "%s", err[0] ? err : "could not arm");
		return NC_ARM_FAILED;
	}

	// Name the transport and the delay it bought. Which network a session ended
	// up on is the single thing that decides whether it is playable, and it was
	// previously invisible - a session that quietly fell back to the house
	// network looked identical to one on ad hoc until the game stuttered.
	/* Settle the builds now, while the user is still choosing a game, rather
	 * than in front of a launch. The detached host broker serves the exchange;
	 * the client drives it. */
	if (role == NS_ROLE_HOST) {
		if (!NS_brokerStart(err, sizeof(err))) {
			NC_abortArm(true);
			snprintf(status, sizeof(status), "%s", err[0] ? err : "Could not start host broker.");
			return NC_ARM_FAILED;
		}
	} else if (protocol == NS_PROTO_RETROARCH) {
		/* A RetroArch host has no broker: no manifest to compare and no
		 * session end to listen for. NS_arm chose the builds. */
		snprintf(core_note, sizeof(core_note), "  -  RetroArch host");
	} else {
		/* Always compare manifests. With compatibility disabled the exchange
		 * still records differing installed builds so both launchers can warn;
		 * the setting only controls whether a packaged fallback may be chosen. */
		if (NC_onProgress)
			NC_onProgress("Checking cores", "Comparing builds with the host.", "Comparing builds");
		char cerr[128] = "";
		int selected = NS_compatSync(peer, cerr, sizeof(cerr));
		if (NC_onProgress) NC_onProgress("Joining", "This can take up to a minute.", NULL);
		if (selected > 0)
			snprintf(core_note, sizeof(core_note), "  -  %d compatibility fallback(s)", selected);
		else if (selected < 0) {
			/* Compatibility selection is part of arming, not an optional status
			 * probe. Continuing after it fails can put the peers on different
			 * cores, which is guaranteed to desynchronize shared-screen play. */
			NC_abortArm(true);
			snprintf(status, sizeof(status), "Core check failed: %s", cerr);
			return NC_ARM_FAILED;
		}
		else
			core_note[0] = '\0';
		/* So the host's Turn off reaches this device even with the app closed. */
		NS_guestAgentStart();
	}

	int adhoc = NS_hotspotActive();
	if (role == NS_ROLE_HOST)
		snprintf(status, sizeof(status), "Hosting on %s  -  %s%s, delay %d",
		         local_ip, adhoc ? "ad hoc" : "WiFi",
		         protocol == NS_PROTO_RETROARCH ? " (RetroArch)" : "",
		         adhoc ? NS_INPUT_DELAY_ADHOC : NS_INPUT_DELAY_WIFI);
	else
		snprintf(status, sizeof(status), "Joined %s  -  %s, delay %d%s",
		         peer, adhoc ? "ad hoc" : "WiFi",
		         adhoc ? NS_INPUT_DELAY_ADHOC : NS_INPUT_DELAY_WIFI, core_note);
	return NC_ARM_OK;
}

long elapsed_since_ms(const struct timespec* t0) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* The evdev definitions we need; linux/input.h's names collide with NextUI's
 * BTN_* button masks. */
struct evdev_event {
	struct timeval time;
	uint16_t type, code;
	int32_t value;
};
#define EVDEV_SYN 0x00
#define EVDEV_KEY 0x01
#define EVDEV_ABS 0x03
#define EVDEV_SYN_REPORT 0
#define EVDEV_ABS_HAT0Y 0x11
#define EVDEV_GRAB _IOW('E', 0x90, int)

bool menu_pad_open(MenuPad* pad, bool grab) {
	memset(pad, 0, sizeof(*pad));
	for (int i = 0; i < 32 && pad->count < 4; i++) {
		char path[64], name[64] = "";
		snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
		FILE* f = fopen(path, "r");
		if (!f) continue;
		if (!fgets(name, sizeof(name), f)) name[0] = '\0';
		fclose(f);
		name[strcspn(name, "\n")] = '\0';
		if (!strcmp(name, "ANBERNIC-keys")) { pad->a = 304; pad->b = 305; pad->x = 307; }
		else if (!strcmp(name, "TRIMUI Player1")) { pad->a = 305; pad->b = 304; pad->x = 308; }
		else if (!strcmp(name, "MIYOO Player1")) { pad->a = 305; pad->b = 304; pad->x = 308; }
		else if (!strcmp(name, "retrogame_joypad")) { pad->a = 305; pad->b = 304; pad->x = 307; }
		/* The A30's buttons are keys on gpio-keys-polled; spruce's joypad
		 * daemon turns them into the pad its menu reads, so holding the
		 * source holds the menu's input too. */
		else if (!strcmp(PLATFORM, "my282") && !strcmp(name, "gpio-keys-polled")) {
			pad->a = 57; pad->b = 29; pad->x = 42;
		}
		/* The Mini Flip's keys are a keyboard on event0, the node spruce's
		 * PyUI itself reads, with the A30's codes. */
		else if (!strcmp(PLATFORM, "my285") && i == 0) {
			pad->a = 57; pad->b = 29; pad->x = 42;
		}
		else continue;
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, (grab ? O_RDONLY : O_WRONLY) | O_NONBLOCK);
		if (fd < 0) continue;
		/* Held exclusively while we ask: the paused menu would otherwise find
		 * our presses queued when it resumes, and act on them. */
		if (grab) ioctl(fd, EVDEV_GRAB, 1);
		pad->fd[pad->count++] = fd;
	}
	return pad->count > 0;
}

void menu_pad_close(MenuPad* pad) {
	for (int i = 0; i < pad->count; i++) close(pad->fd[i]);
	pad->count = 0;
}

/* The pressed button's role on the menu pad: 'a', 'b', 'x' or 0. */
int menu_pad_read(MenuPad* pad, int timeout_ms) {
	struct pollfd p[4];
	for (int i = 0; i < pad->count; i++) p[i] = (struct pollfd){ .fd = pad->fd[i], .events = POLLIN };
	if (poll(p, (nfds_t)pad->count, timeout_ms) <= 0) return 0;
	for (int i = 0; i < pad->count; i++) {
		struct evdev_event ev;
		while (read(pad->fd[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type != EVDEV_KEY || ev.value != 1) continue;
			if (ev.code == pad->a) return 'a';
			if (ev.code == pad->b) return 'b';
			if (ev.code == pad->x) return 'x';
		}
	}
	return 0;
}

int NC_promptAnswer(const char* accept, const char* browse, int timeout_s,
                    const struct timespec* t0) {
	MenuPad pad;
	if (!menu_pad_open(&pad, true)) return 2;
	int result = 2;
	while (elapsed_since_ms(t0) < timeout_s * 1000L) {
		int button = menu_pad_read(&pad, 50);
		if (button == 'a' && accept[0]) { result = 0; break; }
		if (button == 'b') { result = 1; break; }
		if (button == 'x' && browse[0]) { result = 3; break; }
	}
	printf("%s\n", result == 0 ? "accept" : result == 1 ? "decline" : result == 3 ? "browse" : "timeout");
	menu_pad_close(&pad);
	return result;
}

/* A resumed menu repaints only what changes, so whatever covered it stays on
 * screen until the player moves. Down then Up, written into the menu pad's
 * own node like DevBridge's spruce-input does, repaints it and leaves the
 * selection where it was. */
int NC_nudgeMain(void) {
	MenuPad pad;
	if (!menu_pad_open(&pad, false)) return 1;
	const int steps[] = { 1, 0, -1, 0 };
	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		struct evdev_event ev[2];
		memset(ev, 0, sizeof(ev));
		ev[0].type = EVDEV_ABS; ev[0].code = EVDEV_ABS_HAT0Y; ev[0].value = steps[i];
		ev[1].type = EVDEV_SYN; ev[1].code = EVDEV_SYN_REPORT;
		if (write(pad.fd[0], ev, sizeof(ev)) != (ssize_t)sizeof(ev)) break;
		usleep(80 * 1000);
	}
	menu_pad_close(&pad);
	return 0;
}

/* Headless control for unattended tests: the steps the menus take, with no
 * screen and no pad, so a test runner can set a session up and tear it down
 * between cases. Prints one "ok ..." or "error ..." line; exits 0 on ok.
 *
 *   --ctl status
 *   --ctl host wifi|adhoc [--protocol linkplay|retroarch]
 *   --ctl join IP | adhoc | discover [SECONDS] [--protocol auto|linkplay|retroarch]
 *   --ctl set nickname NAME      shown to the other player; "" for the model name
 *   --ctl set host_protocol linkplay|retroarch
 *   --ctl end
 *   --ctl powersave on|off
 *   --ctl pak dsperate|drastic install
 *   --ctl set adhoc_switch|compatibility_cores 0|1
 *   --ctl set instanced 0|1|2      paired cores: off, all, selected
 *   --ctl peers [SECONDS]          one tab-separated line per discovered host
 *   --ctl join-peer IP PROTOCOL [SSID PSK]
 *   --ctl checks | cores | settings | off | restore-wifi
 *   --ctl sync                     guest: compare cores and agree armGBA's link
 *                                  options with the host again, as joining did
 */
static void ctl_print_status(void) {
	NS_SessionInfo si;
	char ip[NS_IP_LEN] = "";
	if (!NS_localIP(ip, sizeof(ip))) snprintf(ip, sizeof(ip), "none");
	printf("ip=%s\n", ip);
	printf("can_host_adhoc=%d\n", NS_canHostAdhoc() ? 1 : 0);
	printf("powersave_off=%d\n", NS_settings()->disable_powersave ? 1 : 0);
	printf("adhoc_switch=%d\n", NS_settings()->adhoc_switch ? 1 : 0);
	if (!NS_sessionInfo(&si)) { printf("armed=0\n"); return; }
	printf("armed=1\nrole=%s\n", si.role == NS_ROLE_HOST ? "host" : "client");
	printf("network=%s\nhost=%s\nbroker=%d\nguests=%d\n",
	       si.network[0] ? si.network : "wifi", si.host,
	       si.broker_running ? 1 : 0, si.guest_count);
	for (int i = 0; i < si.guest_count && i < 4; i++)
		printf("guest%d=%s\n", i, si.guest[i].ip);
}

static int ctl_result(bool ok) {
	printf("%s %s\n", ok ? "ok" : "error", status[0] ? status : "-");
	return ok ? 0 : 1;
}

/* Where power save changes only by reloading the driver (H700's 8821cs) or
 * power-cycling the radio (the Mini Flip's 8188fu), and a reboot turns it back
 * on. */
static bool slow_powersave(void) {
	return !strcmp(PLATFORM, "h700") || !strcmp(PLATFORM, "my285");
}

/* Before a session starts: the setting says power save off, but on those
 * radios a reboot has put it back. Apply it again now - it takes the radio
 * down for a few seconds, which only a session not yet started can afford. */
static void ctl_powersave_before_session(void) {
	if (!NS_settings()->disable_powersave || !slow_powersave() || NS_isArmed()) return;
	if (!NS_wifiPowerSaveNeedsReload()) return;
	char err[160] = "";
	if (!NS_wifiPowerSaveReload(true, err, sizeof(err)))
		fprintf(stderr, "ctl: could not turn WiFi power saving off again: %s\n", err);
}

int NC_ctlMain(int argc, char* argv[]) {
	/* --protocol may appear anywhere after the command; take it out so the
	 * positional arguments stay where each command expects them. */
	const char* protocol_arg = NULL;
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--protocol") || i + 1 >= argc) continue;
		protocol_arg = argv[i + 1];
		for (int j = i; j + 2 < argc; j++) argv[j] = argv[j + 2];
		argc -= 2;
		break;
	}
	if (protocol_arg && strcmp(protocol_arg, "auto") && strcmp(protocol_arg, "linkplay") &&
	    strcmp(protocol_arg, "retroarch")) {
		printf("error --protocol auto|linkplay|retroarch\n");
		return 1;
	}
	const char* cmd = argc > 0 ? argv[0] : "status";
	const char* arg = argc > 1 ? argv[1] : "";

	/* No InitSettings or WIFI_init: the session code needs neither, and the
	 * former runs amixer, whose output would bury the answer. */
	NS_init();
	/* Core inspection is safe to warm in the background while PyUI starts.
	 * It only reads spruce's emulator choices and needs no network recovery. */
	if (strcmp(cmd, "cores") && strcmp(cmd, "core-option")) {
		NS_cleanupStaleSession();
		NS_wifiRecoverIfStranded();
		if (!NS_localIP(local_ip, sizeof(local_ip))) snprintf(local_ip, sizeof(local_ip), "no network");
	}

	if (!strcmp(cmd, "status")) {
		ctl_print_status();
		return 0;
	}

	if (!strcmp(cmd, "settings")) {
		NS_Settings* c = NS_settings();
		char shown[NS_NICK_LEN];
		NS_nickname(shown, sizeof(shown));
		printf("nickname=%s\nnickname_custom=%s\nhost_protocol=%s\n", shown, c->nickname,
		       c->host_protocol == NS_PROTO_RETROARCH ? "retroarch" : "linkplay");
		printf("hybrid_network=%d\ncompatibility_cores=%d\nforce_compatibility=%d\nverbose_logs=%d\n"
		       "session_save_backups=%d\ndisable_powersave=%d\ninst_gambatte=%d\ninst_mgba=%d\n",
		       c->hybrid_network, c->compatibility_cores, c->force_compatibility, c->verbose_logs,
		       c->session_save_backups, c->disable_powersave,
		       c->inst_core[0], c->inst_core[1]);
		return 0;
	}

	if (!strcmp(cmd, "checks")) {
		NS_Check found[8];
		NS_CheckResult worst;
		int n = NS_runChecks(found, 8, &worst);
		for (int i = 0; i < n; i++)
			printf("check\t%d\t%s\t%s\n", found[i].result, found[i].label, found[i].detail);
		printf("worst=%d\n", worst);
		return 0;
	}

	if (!strcmp(cmd, "cores")) {
		if (arg[0] && strcmp(arg, "base") && strcmp(arg, "extras")) {
			snprintf(status, sizeof(status), "cores base|extras");
			return ctl_result(false);
		}
		refresh_cores();
		for (int i = 0; i < CORE_COUNT; i++) {
			const CoreDef* d = &CORE_DEFS[i];
			const CoreState* st = &core_state[i];
			printf("core_engine\t%s\t%s\t%s\t%s\t%d\n",
				   CORE_GROUPS[d->group].tag, d->core, d->label, st->state, st->more);
		}
		printf("gambatte_dual=%d\nmgba_dual=%d\n", NS_gambatteDualAvailable(), NS_mgbaDualAvailable());
		printf("inst_gambatte=%d\ninst_mgba=%d\n", gambatte_dual(), mgba_paired());
		char options[1024];
		NS_linkOptions(options, sizeof(options));
		fputs(options, stdout);
		return 0;
	}

	if (!strcmp(cmd, "core-option")) {
		if (argc == 3 && NS_setLinkOption(arg, argv[2])) return ctl_result(true);
		snprintf(status, sizeof(status), "Could not save core option");
		return ctl_result(false);
	}

	if (!strcmp(cmd, "peers")) {
		int secs = argc > 1 ? atoi(arg) : 3;
		if (secs < 1) secs = 1;
		if (secs > 15) secs = 15;
		NS_Peer found[NS_MAX_PEERS], scanned[NS_MAX_PEERS];
		NS_discoverStart();
		int n = 0;
		for (int i = 0; i < secs * 10; i++) {
			n = NS_discoverTick(found, NS_MAX_PEERS);
			usleep(100000);
		}
		NS_discoverStop();
		int sn = NS_scanAdhoc(scanned, NS_MAX_PEERS);
		for (int i = 0; i < n; i++)
			printf("peer\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n", found[i].ip,
			       found[i].nick, found[i].platform, found[i].protocol, found[i].hotspot,
			       found[i].ssid, found[i].psk);
		for (int i = 0; i < sn; i++) {
			bool duplicate = false;
			for (int j = 0; j < n; j++)
				if (found[j].hotspot && !strcmp(found[j].ssid, scanned[i].ssid)) duplicate = true;
			if (!duplicate)
				printf("peer\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n", NS_HOTSPOT_HOST_IP,
				       "", scanned[i].platform, scanned[i].protocol, 1, scanned[i].ssid, scanned[i].psk);
		}
		return 0;
	}

	if (!strcmp(cmd, "join-peer")) {
		if (argc < 3 || NS_isArmed()) return ctl_result(false);
		ctl_powersave_before_session();
		join_protocol_override = -1;
		join_peer_protocol = !strcmp(argv[2], "retroarch") ? NS_PROTO_RETROARCH : NS_PROTO_LINKPLAY;
		const char* ip = arg;
		if (argc >= 5 && argv[3][0]) {
			const char* psk = argv[4][0] ? argv[4] : NS_ADHOC_PSK;
			if (!NS_hotspotJoin(argv[3], psk, status, sizeof(status))) return ctl_result(false);
			ip = NS_HOTSPOT_HOST_IP;
		}
		NC_arm(NS_ROLE_CLIENT, ip);
		return ctl_result(NS_isArmed());
	}

	if (!strcmp(cmd, "sync")) {
		/* Joining settles the builds and armGBA's link options once. Option
		 * files edited after that (a test writing armGBA.opt) need it again
		 * before the game opens. The host's broker answers at any time. */
		char path[512], line[256], peer[64] = "";
		bool client = false;
		snprintf(path, sizeof(path), "%s/session", NS_statePath());
		FILE* f = fopen(path, "r");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				line[strcspn(line, "\r\n")] = '\0';
				if (!strcmp(line, "role=client")) client = true;
				else if (!strncmp(line, "peer=", 5)) snprintf(peer, sizeof(peer), "%s", line + 5);
			}
			fclose(f);
		}
		if (!client || !peer[0]) {
			snprintf(status, sizeof(status), "sync needs a joined session");
			return ctl_result(false);
		}
		char cerr[128] = "";
		int selected = NS_compatSync(peer, cerr, sizeof(cerr));
		if (selected < 0) {
			snprintf(status, sizeof(status), "%s", cerr);
			return ctl_result(false);
		}
		if ((f = fopen(path, "r"))) {
			while (fgets(line, sizeof(line), f))
				if (!strncmp(line, "armgba_link_", 12) || !strncmp(line, "option.armgba_", 14))
					fputs(line, stdout);
			fclose(f);
		}
		return ctl_result(true);
	}

	if (!strcmp(cmd, "off")) {
		NS_disarm();
		NS_hotspotStop();
		return ctl_result(!NS_isArmed());
	}
	if (!strcmp(cmd, "restore-wifi")) {
		NS_wifiRestore();
		return ctl_result(NS_localIP(local_ip, sizeof(local_ip)));
	}

	if (!strcmp(cmd, "host")) {
		bool adhoc = !strcmp(arg, "adhoc");
		if (!adhoc && strcmp(arg, "wifi")) {
			snprintf(status, sizeof(status), "host wifi|adhoc");
			return ctl_result(false);
		}
		if (NS_isArmed()) {
			snprintf(status, sizeof(status), "already armed");
			return ctl_result(false);
		}
		ctl_powersave_before_session();
		if (adhoc) {
			if (!NS_canHostAdhoc()) {
				snprintf(status, sizeof(status), "this device cannot host ad hoc");
				return ctl_result(false);
			}
			NS_hotspotCredentials(hs_ssid, sizeof(hs_ssid), hs_psk, sizeof(hs_psk));
			if (!NS_hotspotStart(hs_ssid, hs_psk, status, sizeof(status)))
				return ctl_result(false);
			hosting_hotspot = 1;
			snprintf(local_ip, sizeof(local_ip), "%s", NS_HOTSPOT_HOST_IP);
			NS_announceHotspot(hs_ssid, hs_psk);
		} else {
			NS_hotspotStop();
			NS_announceHotspot(NULL, NULL);
		}
		status[0] = '\0';
		NS_setArmProtocol(protocol_arg ? (!strcmp(protocol_arg, "retroarch") ? NS_PROTO_RETROARCH
		                                                                     : NS_PROTO_LINKPLAY)
		                               : NS_settings()->host_protocol);
		NC_arm(NS_ROLE_HOST, NULL);
		return ctl_result(NS_isArmed());
	}

	if (!strcmp(cmd, "join")) {
		if (NS_isArmed()) {
			snprintf(status, sizeof(status), "already armed");
			return ctl_result(false);
		}
		ctl_powersave_before_session();
		NS_Peer peer;
		memset(&peer, 0, sizeof(peer));
		bool discover = !strcmp(arg, "discover");
		if (discover || !strcmp(arg, "adhoc")) {
			/* As the Join list finds hosts: by their LAN announcement, and
			 * by scanning for Netplay ad hoc networks - the only way to find
			 * a host whose own house WiFi has dropped. Tests run one host at
			 * a time, so the first announcement, or a lone network, is it. */
			int secs = argc > 2 ? atoi(argv[2]) : 20;
			NS_Peer found[NS_MAX_PEERS];
			int n = 0;
			if (discover) {
				NS_discoverStart();
				for (int t = 0; t < secs * 5 && n == 0; t++) {
					n = NS_discoverTick(found, NS_MAX_PEERS);
					if (!n) usleep(100000);
				}
				NS_discoverStop();
			}
			/* A scan asked for now lands in the supplicant's results a few
			 * seconds later, so keep asking, 3 s apart, for the time left. */
			for (int tries = 0; n == 0 && tries < (discover ? secs / 2 : secs) / 3 + 1; tries++) {
				n = NS_scanAdhoc(found, NS_MAX_PEERS);
				if (n > 1) {
					snprintf(status, sizeof(status), "%d ad hoc networks; join one by IP", n);
					return ctl_result(false);
				}
				if (!n) sleep(3);
			}
			if (!n) {
				snprintf(status, sizeof(status), "no host found in %ds", secs);
				return ctl_result(false);
			}
			peer = found[0];
		} else if (arg[0]) {
			snprintf(peer.ip, sizeof(peer.ip), "%s", arg);
		} else {
			snprintf(status, sizeof(status), "join IP|adhoc|discover [SECONDS]");
			return ctl_result(false);
		}

		status[0] = '\0';
		join_protocol_override = !protocol_arg || !strcmp(protocol_arg, "auto") ? -1
		                       : !strcmp(protocol_arg, "retroarch") ? NS_PROTO_RETROARCH
		                       : NS_PROTO_LINKPLAY;
		join_peer_protocol = peer.protocol;
		if (peer.hotspot) {
			const char* ssid = peer.ssid[0] ? peer.ssid : NS_ADHOC_SSID;
			const char* psk  = peer.psk[0]  ? peer.psk  : NS_ADHOC_PSK;
			if (!NS_hotspotJoin(ssid, psk, status, sizeof(status)))
				return ctl_result(false);
			NC_arm(NS_ROLE_CLIENT, NS_HOTSPOT_HOST_IP);
		} else {
			NC_arm(NS_ROLE_CLIENT, peer.ip);
		}
		return ctl_result(NS_isArmed());
	}

	if (!strcmp(cmd, "end")) {
		/* X's teardown: bindings stay, WiFi comes back. */
		NS_endSession();
		NS_announceStop();
		snprintf(status, sizeof(status), "session ended");
		return ctl_result(!NS_isArmed());
	}

	if (!strcmp(cmd, "powersave")) {
		NS_Settings* c = NS_settings();
		bool off = !strcmp(arg, "off");
		if (!off && strcmp(arg, "on")) {
			snprintf(status, sizeof(status), "powersave on|off");
			return ctl_result(false);
		}
		status[0] = '\0';
		if (off) {
			if (NS_wifiPowerSaveNeedsReload() &&
			    !NS_wifiPowerSaveReload(true, status, sizeof(status)))
				return ctl_result(false);
			c->disable_powersave = true;
			NS_SessionInfo armed;
			if (NS_sessionInfo(&armed)) NS_wifiPowerSaveDisable();
		} else {
			c->disable_powersave = false;
			NS_wifiPowerSaveRestore();
			/* Restore puts back what was recorded before a session, which on
			 * a device someone had switched off by hand is off: a test asking
			 * for power save on has to get it. */
			if (!slow_powersave()) NS_wifiPowerSaveRadio(true);
			/* The app leaves an H700 (or a Mini Flip) at power save off until
			 * the next boot; a test run cannot wait for one. */
			if (slow_powersave() && !NS_wifiPowerSaveNeedsReload() &&
			    !NS_wifiPowerSaveReload(false, status, sizeof(status))) {
				NS_settingsSave();
				return ctl_result(false);
			}
		}
		NS_settingsSave();
		snprintf(status, sizeof(status), "powersave %s", off ? "disabled" : "allowed");
		return ctl_result(true);
	}

	if (!strcmp(cmd, "pak")) {
		/* pak dsperate|drastic install: what Settings' install prompt does. */
		NS_ManagedPak k = !strcmp(arg, "dsperate") ? NS_PAK_DSP
		                : !strcmp(arg, "drastic") ? NS_PAK_NDS : NS_PAK_COUNT;
		if (k == NS_PAK_COUNT || argc < 3 || strcmp(argv[2], "install")) {
			snprintf(status, sizeof(status), "pak dsperate|drastic install");
			return ctl_result(false);
		}
		status[0] = '\0';
		if (!NS_pakInstall(k, status, sizeof(status))) return ctl_result(false);
		if (k == NS_PAK_DSP) NS_settings()->dsperate = true;
		else NS_settings()->drastic = true;
		NS_settingsSave();
		snprintf(status, sizeof(status), "%s installed", arg);
		return ctl_result(true);
	}

	if (!strcmp(cmd, "set")) {
		/* set adhoc_switch|compatibility_cores 0|1, set instanced 0|1|2 */
		NS_Settings* c = NS_settings();
		if (!strcmp(arg, "instanced") && argc >= 3) {
			int v = atoi(argv[2]);
			if (v < NS_INST_OFF || v > NS_INST_SELECTED) {
				snprintf(status, sizeof(status), "set instanced 0|1|2");
				return ctl_result(false);
			}
			c->instanced = (NS_InstMode)v;
			NS_settingsSave();
			snprintf(status, sizeof(status), "instanced=%d", v);
			return ctl_result(true);
		}
		if (!strcmp(arg, "nickname")) {
			snprintf(c->nickname, sizeof(c->nickname), "%.31s", argc >= 3 ? argv[2] : "");
			NS_sanitizeNickname(c->nickname);
			NS_settingsSave();
			char shown[NS_NICK_LEN];
			NS_nickname(shown, sizeof(shown));
			snprintf(status, sizeof(status), "nickname=%s", shown);
			return ctl_result(true);
		}
		if (!strcmp(arg, "host_protocol") && argc >= 3 &&
		    (!strcmp(argv[2], "linkplay") || !strcmp(argv[2], "retroarch"))) {
			c->host_protocol = !strcmp(argv[2], "retroarch") ? NS_PROTO_RETROARCH : NS_PROTO_LINKPLAY;
			NS_settingsSave();
			snprintf(status, sizeof(status), "host_protocol=%s", argv[2]);
			return ctl_result(true);
		}
		if (argc >= 3 && (!strcmp(arg, "force_compatibility") ||
		                  !strcmp(arg, "hybrid_network") || !strcmp(arg, "verbose_logs") || !strcmp(arg, "session_save_backups") ||
		                  !strcmp(arg, "inst_gambatte") || !strcmp(arg, "inst_mgba") ||
		                  !strcmp(arg, "disable_powersave"))) {
			bool on = atoi(argv[2]) != 0;
			if (!strcmp(arg, "force_compatibility")) {
				c->force_compatibility = on;
				if (on) c->compatibility_cores = true;
			} else if (!strcmp(arg, "hybrid_network")) c->hybrid_network = on;
			else if (!strcmp(arg, "verbose_logs")) c->verbose_logs = on;
			else if (!strcmp(arg, "session_save_backups")) c->session_save_backups = on;
			else if (!strcmp(arg, "inst_gambatte")) set_instanced(0, on);
			else if (!strcmp(arg, "inst_mgba")) set_instanced(1, on);
			else {
				c->disable_powersave = on;
				if (on) NS_wifiPowerSaveDisable(); else NS_wifiPowerSaveRestore();
			}
			NS_settingsSave();
			snprintf(status, sizeof(status), "%s=%d", arg, on);
			return ctl_result(true);
		}
		bool* value = !strcmp(arg, "adhoc_switch") ? &c->adhoc_switch
		            : !strcmp(arg, "compatibility_cores") ? &c->compatibility_cores : NULL;
		if (!value || argc < 3) {
			snprintf(status, sizeof(status), "set adhoc_switch|compatibility_cores 0|1");
			return ctl_result(false);
		}
		*value = atoi(argv[2]) != 0;
		if (value == &c->compatibility_cores && !*value) c->force_compatibility = false;
		NS_settingsSave();
		snprintf(status, sizeof(status), "%s=%d", arg, *value ? 1 : 0);
		return ctl_result(true);
	}

	snprintf(status, sizeof(status), "unknown command %s", cmd);
	return ctl_result(false);
}
