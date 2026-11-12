/* Which core ("engine") a session runs for each system.
 *
 * One table, in engines.c, lists every engine Netplay can run, ranked per
 * system, with the lowest performance tier that can run it and whether a
 * 32-bit and a 64-bit build of it play each other. Devices only say which
 * engines they have (each firmware finds its own files) and their tier; the
 * host resolves the table against both and both devices run the result. A
 * new device is a tier, a new firmware a way to find cores, a new core a row.
 *
 * Pure: no files, no sockets, so the whole compatibility matrix can be
 * worked out and checked on a PC (testing/engines-test.c). */
#ifndef NETPLAY_ENGINES_H
#define NETPLAY_ENGINES_H

#include <stdbool.h>
#include <stddef.h>

/* Performance tiers, slowest first; a session runs the lower of its two. */
enum { ENG_TIER_MINI = 1, ENG_TIER_A30 = 2, ENG_TIER_NORMAL = 3 };

/* Extra conditions on an engine beyond both devices having it. */
enum {
	ENG_GATE_NONE = 0,
	ENG_GATE_GPSP_GAMES,    /* links only the games gpSP knows (checked at load) */
};

typedef struct {
	const char* system;     /* canonical tag: FC, SFC, MD, MS, GB, GBA, ... */
	const char* name;       /* engine id, unique in the table's engine set */
	const char* core;       /* the libretro core a device must have */
	const char* mode;       /* together, separate, paired, link, or NULL */
	int         min_tier;
	bool        cross_arch; /* a 32-bit and a 64-bit build stay in step */
	int         gate;
} NS_Engine;

#define ENG_MAX_ENGINES 24

/* What one device brings to a session. */
typedef struct {
	int  tier;
	bool is32;              /* runs 32-bit cores */
	bool has_ra32;          /* 64-bit, but can drop to 32-bit cores (spruce) */
	bool have[ENG_MAX_ENGINES];          /* indexed like NS_engineAt */
	bool installable[ENG_MAX_ENGINES];   /* not here, but the player can add it */
} NS_EngineCaps;

int NS_engineCount(void);
const NS_Engine* NS_engineAt(int i);
int NS_engineIndex(const char* name);   /* -1 if unknown */

/* The canonical system for a firmware's tag (SMS -> MS, THIRTYTWOX -> 32X,
 * SUPA -> SFC, MGBA -> GBA); NULL if the table does not cover it. */
const char* NS_engineSystem(const char* tag);

/* The systems the table covers, in order. */
int NS_engineSystemCount(void);
const char* NS_engineSystemAt(int i);

/* The engine both devices run for a system, or -1 (no common engine: each
 * runs its own, as before engines existed). hint, when non-NULL, receives
 * advice for the players when a better engine was missed only because one
 * device lacks something it could install: "install-<core>:host",
 * "install-<core>:guest" or "install-<core>:both" (only mgba in practice),
 * else "". */
int NS_engineResolve(const char* system, const NS_EngineCaps* host,
                     const NS_EngineCaps* guest, char* hint, size_t hint_len);

/* Encodes a device's caps as "tier=N;arch=32|64|64+32;have=a,b;install=c",
 * and back. Unknown engine names are ignored, so tables can grow. */
void NS_engineCapsEncode(const NS_EngineCaps* caps, char* out, size_t len);
bool NS_engineCapsDecode(const char* text, NS_EngineCaps* caps);

#endif
