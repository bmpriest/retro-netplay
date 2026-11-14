#include "brick-power.h"

#include <assert.h>

int main(void) {
	assert(BP_parseRegister("axp2202-REG[0x19]=0x6\n") == 6);
	assert(BP_parseRegister("not a register") == -1);
	assert(BP_parseRegister("reg=0x100") == -1);
	assert(BP_chargerOff(0x67) == 0x65);
	assert(BP_inputMinimum(0xff) == 0xc0);
	return 0;
}
