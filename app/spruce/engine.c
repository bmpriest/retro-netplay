/*
 * netplay.elf on spruce: the headless engine behind Netplay's PyUI front end
 * (app/spruce/__main__.py). spruce draws everything itself, so this build has
 * no screens and links no SDL - only the session control the NextUI app
 * shares (control.c):
 *
 *   --ctl COMMAND ...      session and settings commands; see control.c
 *   --prompt [--accept L] [--browse L] [--timeout S] ...
 *                          an invitation's answer, read from the grabbed pad
 *                          while PyUI draws the question (--title, --text,
 *                          --art and --input-only are accepted and ignored)
 *   --nudge                repaint spruce's paused menu
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../control.h"

static int prompt(int argc, char* argv[]) {
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	const char *accept = "PLAY", *browse = "";
	int timeout_s = 60;
	for (int i = 0; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--accept")) accept = argv[i + 1];
		else if (!strcmp(argv[i], "--browse")) browse = argv[i + 1];
		else if (!strcmp(argv[i], "--timeout")) timeout_s = atoi(argv[i + 1]);
	}
	return NC_promptAnswer(accept, browse, timeout_s, &t0);
}

int main(int argc, char* argv[]) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--ctl")) return NC_ctlMain(argc - i - 1, argv + i + 1);
		if (!strcmp(argv[i], "--prompt")) return prompt(argc - i - 1, argv + i + 1);
		if (!strcmp(argv[i], "--nudge")) return NC_nudgeMain();
	}
	fprintf(stderr, "usage: %s --ctl COMMAND ... | --prompt ... | --nudge\n"
	                "Netplay's spruce menu is the PyUI front end (launch.sh).\n", argv[0]);
	return 2;
}
