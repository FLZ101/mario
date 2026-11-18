#include <assert.h>
#include <stdio.h>

#include <mario/input.h>

int main(void)
{
	struct mario_gui_key_event key = { 0 };
	struct mario_psaux_event mouse = { 0 };

	key.flags = GUI_KEY_EVENT_OVERFLOW;
	mouse.flags = PSAUX_EVENT_OVERFLOW;
	assert(sizeof(key) == 8);
	assert(sizeof(mouse) == 12);
	assert(key.flags == 1);
	assert(mouse.flags == 1);
	printf("input-abi: key=%u mouse=%u flags=%u/%u\n",
	       (unsigned int)sizeof(key), (unsigned int)sizeof(mouse),
	       key.flags, mouse.flags);
	mouse.flags = PSAUX_EVENT_ABSOLUTE;
	mouse.x = PSAUX_ABSOLUTE_MAX;
	mouse.y = 32768;
	assert(mouse.flags == 2);
	assert(mouse.dx == -1 && mouse.dy == -32768);
	puts("input-abi: absolute mouse shares 12-byte record");
	return 0;
}
