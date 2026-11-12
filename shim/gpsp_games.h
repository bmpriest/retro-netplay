/* The GBA games Netplay's gpSP links: the entries of gpSP's own gba_over.h
 * that set a serial or Wireless Adapter mode (not the GameBoy Player ones),
 * plus the Pokemon Gen III family it recognises by header (rom_is_pokemon_
 * family in gba_memory.c). Taken from the pinned revision (Makefile
 * GPSP_REV 5819380); regenerate it when that pin moves. Advance Wars 1/2 and
 * Pokemon are tested on devices; the rest gpSP itself puts in link mode, and
 * Netplay counts them too.
 *
 * Header-only: the shim checks a game before a gpSP link session, and the
 * engine tests check the list. */
#ifndef NETPLAY_GPSP_GAMES_H
#define NETPLAY_GPSP_GAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char* const GPSP_LINK_GAMES[] = {
	"AWRP", /* Advance Wars (E) */
	"AWRE", /* Advance Wars (U) */
	"AW2P", /* Advance Wars 2: Black Hole Rising (E) */
	"AW2E", /* Advance Wars 2: Black Hole Rising (U) */
	"BKRJ", /* No No No Puzzle Chailien (J) */
	"BDGE", /* Digimon Racing (U) */
	"BDGP", /* Digimon Racing (E) */
	"BG3E", /* Dragon Ball Z - Buu's Fury (U) */
	"BMGE", /* Mario Golf: Advance Tour (U) */
	"BMGJ", /* Mario Golf Advance (J) */
	"BMGP", /* Mario Golf: Advance Tour (E) */
	"BMGS", /* Mario Golf: Advance Tour (S) */
	"BMGF", /* Mario Golf: Advance Tour (F) */
	"BMGI", /* Mario Golf: Advance Tour (I) */
	"BMGD", /* Mario Golf: Advance Tour (G) */
	"BMGU", /* Mario Golf: Advance Tour (A) */
	"BTMP", /* Mario Power Tennis (E) */
	"BTMJ", /* Mario Tennis Advance (J) */
	"BTME", /* Mario Tennis: Power Tour (U/A) */
	"BRBE", /* Mega Man Battle Network 5 Team Protoman (U) */
	"BRKE", /* Mega Man Battle Network 5 Team Colonel (U) */
	"BR5E", /* Mega Man Battle Network 6 Cybeast Gregar (U) */
	"BR6E", /* Mega Man Battle Network 6 Cybeast Falzar (U) */
	"B2WE", /* Narnia Chronicles (U) */
	"BPEE", /* Pokemon Emerald (E/U) */
	"BPEJ", /* Pokemon Emerald (J) */
	"BPED", /* Pokemon Emerald (G) */
	"BPEF", /* Pokemon Emerald (F) */
	"BPES", /* Pokemon Emerald (S) */
	"BPEI", /* Pokemon Emerald (I) */
	"AXPE", /* Pokemon Sapphire (U) */
	"AXPJ", /* Pokemon Sapphire (J) */
	"AXPD", /* Pokemon Sapphire (G) */
	"AXPI", /* Pokemon Sapphire (I) */
	"AXPS", /* Pokemon Sapphire (S) */
	"AXPF", /* Pokemon Sapphire (F) */
	"AXVE", /* Pokemon Ruby (U) */
	"AXVJ", /* Pokemon Ruby (J) */
	"AXVD", /* Pokemon Ruby (G) */
	"AXVI", /* Pokemon Ruby (I) */
	"AXVS", /* Pokemon Ruby (S) */
	"AXVF", /* Pokemon Ruby (F) */
	"BPRE", /* Pokemon Fire Red (E/U) */
	"BPRS", /* Pokemon Fire Red (S) */
	"BPRD", /* Pokemon Fire Red (G) */
	"BPRI", /* Pokemon Fire Red (I) */
	"BPRF", /* Pokemon Fire Red (F) */
	"BPGS", /* Pokemon Leaf Green (S) */
	"BPGD", /* Pokemon Leaf Green (G) */
	"BPGI", /* Pokemon Leaf Green (I) */
	"BPGF", /* Pokemon Leaf Green (F) */
	"BPGJ", /* Pokemon Leaf Green (J) */
	"B4UE", /* Shrek Super Slam (U) */
	"B4UP", /* Shrek Super Slam (E) */
	"B3AE", /* Lord of the Rings: the third Age (U) */
	"B85A", /* Tottoko Hamutaro Hamuhamu Sports (J/U) */
	"B85P", /* Hamtaro Ham Ham Games (E) */
};

/* header: the cartridge's first 0xB0 bytes or more. */
static inline bool gpsp_links_game(const unsigned char* header, size_t len) {
	if (!header || len < 0xB0) return false;
	const char* code = (const char*)header + 0xAC;
	/* rom_is_pokemon_family: any Ruby/Sapphire/Emerald/FireRed/LeafGreen
	 * region, and ROMs titled POKEMON. */
	if (!memcmp(header + 0xA0, "POKEMON", 7)) return true;
	static const char* const POKEMON[] = { "AXV", "AXP", "BPE", "BPR", "BPG" };
	for (size_t i = 0; i < sizeof(POKEMON) / sizeof(POKEMON[0]); i++)
		if (!memcmp(code, POKEMON[i], 3)) return true;
	for (size_t i = 0; i < sizeof(GPSP_LINK_GAMES) / sizeof(GPSP_LINK_GAMES[0]); i++)
		if (!memcmp(code, GPSP_LINK_GAMES[i], 4)) return true;
	return false;
}

#endif
