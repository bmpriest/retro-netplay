#include "engines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The table. Within a system, best first; the first row both devices can run
 * wins. Tiers: 1 Miyoo Mini family, 2 A30, 3 64-bit devices.
 *
 * cross_arch comes from measurement (docs/analysis/32-vs-64bit.md): whether
 * the 32-bit and 64-bit builds compute the same game. A 64-bit spruce device
 * paired with a 32-bit one drops to its 32-bit RetroArch and cores, so the
 * column only bites where that cannot happen (a 64-bit NextUI device). */
static const NS_Engine ENGINES[] = {
	{ "FC",     "fceumm",          "fceumm",             NULL,       1, true,  ENG_GATE_NONE },
	{ "FDS",    "fceumm",          "fceumm",             NULL,       1, true,  ENG_GATE_NONE },
	{ "SFC",    "supafaust",       "mednafen_supafaust", NULL,       1, true,  ENG_GATE_NONE },
	{ "SFC",    "snes9x",          "snes9x",             NULL,       1, true,  ENG_GATE_NONE },
	{ "MD",     "picodrive",       "picodrive",          NULL,       1, true,  ENG_GATE_NONE },
	{ "SEGACD", "picodrive",       "picodrive",          NULL,       1, true,  ENG_GATE_NONE },
	{ "32X",    "picodrive",       "picodrive",          NULL,       1, true,  ENG_GATE_NONE },
	{ "MS",     "genesis_plus_gx", "genesis_plus_gx",    NULL,       3, false, ENG_GATE_NONE },
	{ "MS",     "gearsystem",      "gearsystem",         NULL,       1, true,  ENG_GATE_NONE },
	{ "MS",     "picodrive",       "picodrive",          NULL,       1, true,  ENG_GATE_NONE },
	/* Game Gear is Gearsystem. First Gear-to-Gear: Netplay's Gearsystem
	 * 3.9.25 build, both consoles and the cable in one core, each device
	 * showing its own; its armv7 and AArch64 builds compute the same game and
	 * load each other's states. Then the firmware's own Gearsystem, for a
	 * peer without the link build; picodrive only where neither has one. */
	{ "GG",     "gearsystem-link", "gearsystem",         "link",     1, true,  ENG_GATE_NONE },
	{ "GG",     "gearsystem",      "gearsystem",         NULL,       1, true,  ENG_GATE_NONE },
	{ "GG",     "picodrive",       "picodrive",          NULL,       1, true,  ENG_GATE_NONE },
	{ "GB",     "gambatte-dual",   "gambatte",           "paired",   1, true,  ENG_GATE_NONE },
	{ "GB",     "gambatte-link",   "gambatte",           "link",     1, true,  ENG_GATE_NONE },
	{ "GBC",    "gambatte-dual",   "gambatte",           "paired",   1, true,  ENG_GATE_NONE },
	{ "GBC",    "gambatte-link",   "gambatte",           "link",     1, true,  ENG_GATE_NONE },
	{ "GBA",    "armgba-together", "armgba",             "together", 2, true,  ENG_GATE_NONE },
	{ "GBA",    "armgba-separate", "armgba",             "separate", 1, true,  ENG_GATE_NONE },
	{ "GBA",    "mgba-dual",       "mgba",               "paired",   3, true,  ENG_GATE_NONE },
	{ "GBA",    "gpsp-link",       "gpsp",               "link",     1, true,  ENG_GATE_GPSP_GAMES },
	{ "PS",     "pcsx_rearmed",    "pcsx_rearmed",       NULL,       1, false, ENG_GATE_NONE },
};
#define ROWS ((int)(sizeof(ENGINES) / sizeof(ENGINES[0])))

/* Engine ids, each once: what a device's caps are indexed by. */
static const char* names[ENG_MAX_ENGINES];
static int name_count = -1;

static void index_names(void) {
	if (name_count >= 0) return;
	name_count = 0;
	for (int r = 0; r < ROWS; r++) {
		int i = 0;
		while (i < name_count && strcmp(names[i], ENGINES[r].name)) i++;
		if (i == name_count && name_count < ENG_MAX_ENGINES) names[name_count++] = ENGINES[r].name;
	}
}

int NS_engineCount(void) { index_names(); return name_count; }

/* The first row of an engine stands for its core and mode. */
const NS_Engine* NS_engineAt(int i) {
	index_names();
	if (i < 0 || i >= name_count) return NULL;
	for (int r = 0; r < ROWS; r++) if (!strcmp(ENGINES[r].name, names[i])) return &ENGINES[r];
	return NULL;
}

int NS_engineIndex(const char* name) {
	index_names();
	for (int i = 0; name && i < name_count; i++) if (!strcmp(names[i], name)) return i;
	return -1;
}

const char* NS_engineSystem(const char* tag) {
	if (!tag) return NULL;
	static const struct { const char* from; const char* to; } ALIAS[] = {
		{ "SMS", "MS" }, { "THIRTYTWOX", "32X" }, { "SUPA", "SFC" }, { "MGBA", "GBA" },
	};
	for (size_t a = 0; a < sizeof(ALIAS) / sizeof(ALIAS[0]); a++)
		if (!strcmp(tag, ALIAS[a].from)) tag = ALIAS[a].to;
	for (int r = 0; r < ROWS; r++) if (!strcmp(ENGINES[r].system, tag)) return ENGINES[r].system;
	return NULL;
}

int NS_engineSystemCount(void) {
	int n = 0;
	for (int r = 0; r < ROWS; r++) if (!r || strcmp(ENGINES[r].system, ENGINES[r - 1].system)) n++;
	return n;
}

const char* NS_engineSystemAt(int i) {
	for (int r = 0, n = 0; r < ROWS; r++) {
		if (r && !strcmp(ENGINES[r].system, ENGINES[r - 1].system)) continue;
		if (n++ == i) return ENGINES[r].system;
	}
	return NULL;
}

/* Whether the two devices run different word sizes. A 64-bit spruce device
 * facing a 32-bit one runs its 32-bit RetroArch and cores, so that pair is
 * all 32-bit. */
static bool mixed_arch(const NS_EngineCaps* a, const NS_EngineCaps* b) {
	if (a->is32 == b->is32) return false;
	const NS_EngineCaps* wide = a->is32 ? b : a;
	return !wide->has_ra32;
}

int NS_engineResolve(const char* system, const NS_EngineCaps* host,
                     const NS_EngineCaps* guest, char* hint, size_t hint_len) {
	if (hint && hint_len) hint[0] = '\0';
	if (!system || !host || !guest) return -1;
	int tier = host->tier < guest->tier ? host->tier : guest->tier;
	bool mixed = mixed_arch(host, guest);
	bool hinted = false;
	for (int r = 0; r < ROWS; r++) {
		const NS_Engine* e = &ENGINES[r];
		if (strcmp(e->system, system)) continue;
		if (e->min_tier > tier || (mixed && !e->cross_arch)) continue;
		int i = NS_engineIndex(e->name);
		bool h = host->have[i], g = guest->have[i];
		if (h && g) return i;
		/* Missed only because a device lacks what it could install: say so,
		 * for the first such engine, and keep looking for what runs now. */
		if (!hinted && hint && hint_len &&
		    (h || host->installable[i]) && (g || guest->installable[i])) {
			snprintf(hint, hint_len, "install-%s:%s", e->core,
			         !h && !g ? "both" : !h ? "host" : "guest");
			hinted = true;
		}
	}
	return -1;
}

void NS_engineCapsEncode(const NS_EngineCaps* caps, char* out, size_t len) {
	if (!len) return;
	size_t n = (size_t)snprintf(out, len, "tier=%d;arch=%s;have=", caps->tier,
	                            caps->is32 ? "32" : caps->has_ra32 ? "64+32" : "64");
	const char* sep = "";
	for (int pass = 0; pass < 2; pass++) {
		if (pass) {
			if (n < len) n += (size_t)snprintf(out + n, len - n, ";install=");
			sep = "";
		}
		for (int i = 0; i < NS_engineCount() && n < len; i++) {
			if (!(pass ? caps->installable[i] : caps->have[i])) continue;
			n += (size_t)snprintf(out + n, len - n, "%s%s", sep, names[i]);
			sep = ",";
		}
	}
	if (n >= len) out[len - 1] = '\0';
}

static void decode_list(const char* p, size_t len, bool* set) {
	while (len) {
		const char* comma = memchr(p, ',', len);
		size_t item = comma ? (size_t)(comma - p) : len;
		char name[48];
		if (item && item < sizeof(name)) {
			memcpy(name, p, item);
			name[item] = '\0';
			int i = NS_engineIndex(name);
			if (i >= 0) set[i] = true;
		}
		if (!comma) break;
		len -= item + 1;
		p = comma + 1;
	}
}

bool NS_engineCapsDecode(const char* text, NS_EngineCaps* caps) {
	memset(caps, 0, sizeof(*caps));
	if (!text) return false;
	bool tier = false;
	for (const char* p = text; *p; ) {
		const char* end = strchr(p, ';');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len > 5 && !strncmp(p, "tier=", 5)) {
			caps->tier = atoi(p + 5);
			tier = caps->tier >= ENG_TIER_MINI && caps->tier <= ENG_TIER_NORMAL;
		} else if (len > 5 && !strncmp(p, "arch=", 5)) {
			caps->is32 = len == 7 && !strncmp(p + 5, "32", 2);
			caps->has_ra32 = len == 10 && !strncmp(p + 5, "64+32", 5);
		} else if (len >= 5 && !strncmp(p, "have=", 5)) {
			decode_list(p + 5, len - 5, caps->have);
		} else if (len >= 8 && !strncmp(p, "install=", 8)) {
			decode_list(p + 8, len - 8, caps->installable);
		}
		p = end ? end + 1 : p + len;
	}
	return tier;
}
