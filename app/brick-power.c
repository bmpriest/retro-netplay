#include "brick-power.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BP_CHG_REG  0x19u
#define BP_ILIM_REG 0x17u
#define BP_CHG_EN   0x02u
#define BP_ILIM_BITS 0x3fu

static const char* const bp_nodes[] = {
	"/sys/class/axp/axp_reg",
	"/sys/class/axp/axp2202/axp_reg",
};
static const char* bp_node;
static int bp_original_chg = -1, bp_original_ilim = -1;
static bool bp_limited;

int BP_parseRegister(const char* value) {
	const char* equals = value ? strrchr(value, '=') : NULL;
	if (!equals) return -1;
	char* end;
	long v = strtol(equals + 1, &end, 0);
	if (end == equals + 1 || v < 0 || v > 255) return -1;
	while (*end == ' ' || *end == '\r' || *end == '\n') end++;
	return *end ? -1 : (int)v;
}

unsigned BP_chargerOff(unsigned value) { return value & ~BP_CHG_EN; }
unsigned BP_inputMinimum(unsigned value) { return value & ~BP_ILIM_BITS; }

static const char* bp_find_node(void) {
	if (bp_node) return bp_node;
	for (unsigned i = 0; i < sizeof(bp_nodes) / sizeof(bp_nodes[0]); i++)
		if (access(bp_nodes[i], R_OK | W_OK) == 0) return bp_node = bp_nodes[i];
	return NULL;
}

static bool bp_write_text(const char* value) {
	const char* node = bp_find_node();
	if (!node) return false;
	int fd = open(node, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return false;
	size_t want = strlen(value);
	bool ok = write(fd, value, want) == (ssize_t)want;
	close(fd);
	return ok;
}

static int bp_read(unsigned reg) {
	char command[8], answer[80];
	snprintf(command, sizeof(command), "0x%02x", reg);
	if (!bp_write_text(command)) return -1;
	int fd = open(bp_find_node(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t n = read(fd, answer, sizeof(answer) - 1);
	close(fd);
	if (n <= 0) return -1;
	answer[n] = '\0';
	return BP_parseRegister(answer);
}

static bool bp_write(unsigned reg, unsigned value) {
	char command[8];
	snprintf(command, sizeof(command), "0x%02x%02x", reg, value & 0xffu);
	return bp_write_text(command);
}

static bool bp_path(const char* state_dir, char* out, size_t len) {
	int n = snprintf(out, len, "%s/cable.power", state_dir);
	return n > 0 && (size_t)n < len;
}

static bool bp_load(const char* state_dir, int* chg, int* ilim) {
	char path[512];
	if (!bp_path(state_dir, path, sizeof(path))) return false;
	FILE* f = fopen(path, "r");
	if (!f) return false;
	int c = -1, i = -1;
	bool valid = fscanf(f, "%d %d", &c, &i) == 2 &&
	             c >= 0 && c <= 255 && i >= 0 && i <= 255;
	fclose(f);
	if (valid) { *chg = c; *ilim = i; }
	return valid;
}

static bool bp_save(const char* state_dir, int chg, int ilim) {
	char path[512], temp[540];
	if (!bp_path(state_dir, path, sizeof(path)) ||
	    snprintf(temp, sizeof(temp), "%s.tmp.%d", path, (int)getpid()) >= (int)sizeof(temp)) return false;
	FILE* f = fopen(temp, "w");
	if (!f) return false;
	bool ok = fprintf(f, "%d %d\n", chg, ilim) > 0 &&
	          fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0) ok = false;
	if (ok) ok = rename(temp, path) == 0;
	if (!ok) unlink(temp);
	return ok;
}

bool BP_limit(const char* state_dir) {
	if (bp_limited) return BP_refresh();
	int chg = bp_read(BP_CHG_REG), ilim = bp_read(BP_ILIM_REG);
	if (chg < 0 || ilim < 0) return false;
	/* An unclean predecessor's record holds the actual pre-link values. */
	int saved_chg, saved_ilim;
	if (bp_load(state_dir, &saved_chg, &saved_ilim)) {
		chg = saved_chg;
		ilim = saved_ilim;
	}
	if (!bp_save(state_dir, chg, ilim)) return false;
	bp_original_chg = chg;
	bp_original_ilim = ilim;
	bp_limited = true;
	return BP_refresh();
}

bool BP_refresh(void) {
	if (!bp_limited) return true;
	int chg = bp_read(BP_CHG_REG), ilim = bp_read(BP_ILIM_REG);
	if (chg < 0 || ilim < 0) return false;
	if ((unsigned)chg != BP_chargerOff((unsigned)chg) &&
	    !bp_write(BP_CHG_REG, BP_chargerOff((unsigned)chg))) return false;
	if ((unsigned)ilim != BP_inputMinimum((unsigned)ilim) &&
	    !bp_write(BP_ILIM_REG, BP_inputMinimum((unsigned)ilim))) return false;
	return true;
}

bool BP_restore(const char* state_dir) {
	int chg = bp_original_chg, ilim = bp_original_ilim;
	if (chg < 0 || ilim < 0) {
		if (!bp_load(state_dir, &chg, &ilim)) return true;
	}
	if (!bp_write(BP_CHG_REG, (unsigned)chg) ||
	    !bp_write(BP_ILIM_REG, (unsigned)ilim)) return false;
	char path[512];
	if (bp_path(state_dir, path, sizeof(path))) unlink(path);
	bp_original_chg = bp_original_ilim = -1;
	bp_limited = false;
	return true;
}
