/*
 * Session control shared by Netplay's two front ends: the NextUI app (main.c)
 * and spruce's headless engine (spruce/engine.c, behind the PyUI front end).
 * Nothing here draws or links SDL: the core table, arming, the --ctl
 * commands, and reading an invitation's answer from the pad.
 */
#ifndef NETPLAY_CONTROL_H
#define NETPLAY_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "netsetup.h"

/* What the last action did: the app's status line, --ctl's answer. */
extern char   status[128];
/* Appended to the armed line: whether compatibility cores were selected. */
extern char   core_note[64];
extern NS_Check checks[8];
extern int      check_count;
extern NS_CheckResult check_worst;
extern char local_ip[NS_IP_LEN];
extern char hs_ssid[NS_SSID_LEN];
extern char hs_psk[NS_PSK_LEN];
extern int  hosting_hotspot;

enum { MARK_NONE = 0, MARK_CHECK = 1, MARK_DOUBLE = 2, MARK_CIRCLE = -1, MARK_DASH = -2 };

typedef struct { const char* tag; const char* label; } CoreGroup;
typedef enum { CK_SHIM, CK_GAMBATTE, CK_GBA, CK_DSP, CK_MGBA, CK_NDS, CK_SPRUCE_DS, CK_ARMGBA, CK_UNSUPPORTED } CoreKind;
enum { GROUP_GB, GROUP_GBA, GROUP_GG, GROUP_NDS, GROUP_NES, GROUP_SNES,
	   GROUP_MD, GROUP_CD32, GROUP_SMS, GROUP_PS, GROUP_COUNT };

typedef struct {
	const char* tag;
	const char* label;
	const char* core;
	CoreKind kind;
	int group;
} CoreDef;

typedef struct {
	int         mark;
	bool        more;
	bool        dim;
	const char* state;      /* pak_hints.json's cores.<tag>.<state> */
	NS_PakStatus pak;       /* managed paks only */
	bool        pak_ok;     /* pak status could be read */
} CoreState;

extern const CoreGroup CORE_GROUPS[];
extern const CoreDef CORE_DEFS[];
extern const int core_def_count;
#define CORE_COUNT core_def_count
extern CoreState core_state[];
extern bool cores_fresh;

bool gambatte_dual(void);
bool mgba_instanced(void);
bool mgba_paired(void);
void set_instanced(int core, bool on);
void refresh_cores(void);

/* Arming. A joiner's protocol is set by the caller just before NC_arm. */
extern int join_protocol_override;
extern NS_Protocol join_peer_protocol;
typedef enum { NC_ARM_OK, NC_ARM_FAILED, NC_ARM_CHECKS_FAILED } NC_ArmResult;
/* Told when arming starts a slow step (stage set) and when it is over
 * (stage NULL). Unset, arming says nothing. */
extern void (*NC_onProgress)(const char* title, const char* note, const char* stage);
NC_ArmResult NC_arm(NS_Role role, const char* peer);
void NC_abortArm(bool may_have_activated);

/* The built-in pad as spruce's menu reads it, found by name like DevBridge's
 * spruce-input: ANBERNIC-keys on the H700s, TRIMUI Player1 (trimui_inputd's
 * pad) on the Brick. The two disagree on which code is A. */
typedef struct {
	int fd[4];
	int count;
	int a, b, x;
} MenuPad;

long elapsed_since_ms(const struct timespec* t0);
bool menu_pad_open(MenuPad* pad, bool grab);
void menu_pad_close(MenuPad* pad);
int  menu_pad_read(MenuPad* pad, int timeout_ms);
/* An invitation's answer from the grabbed pad: prints accept, decline,
 * browse or timeout and returns 0, 1, 3 or 2. */
int NC_promptAnswer(const char* accept, const char* browse, int timeout_s,
                    const struct timespec* t0);

int NC_ctlMain(int argc, char* argv[]);
int NC_nudgeMain(void);

#endif
