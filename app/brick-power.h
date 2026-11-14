#ifndef BRICK_POWER_H
#define BRICK_POWER_H

#include <stdbool.h>

/* AXP2202 input and charger registers on the TrimUI Brick. Keep a disk record
 * so the original values can be restored after SIGKILL. */
bool BP_limit(const char* state_dir);
bool BP_refresh(void);
bool BP_restore(const char* state_dir);

int BP_parseRegister(const char* value);
unsigned BP_chargerOff(unsigned value);
unsigned BP_inputMinimum(unsigned value);

#endif
