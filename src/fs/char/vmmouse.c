#include <fs/vmmouse.h>
#include <uapi/mario/input.h>
#include <video/fb.h>

#define VMPORT_PORT 0x5658
#define VMPORT_MAGIC 0x564d5868
#define VMPORT_GET_VERSION 10
#define VMMOUSE_DATA 39
#define VMMOUSE_STATUS 40
#define VMMOUSE_COMMAND 41
#define VMMOUSE_READ_ID 0x45414552
#define VMMOUSE_DISABLE 0x000000f5
#define VMMOUSE_REQUEST_ABSOLUTE 0x53424152
#define VMMOUSE_VERSION 0x3442554a
#define VMMOUSE_QUEUE_WORDS 1024
#define VMMOUSE_RELATIVE_PACKET 0x00010000

struct vmport_result {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
};

static struct vmport_result vmport_command(uint32_t command, uint32_t argument)
{
	struct vmport_result result = {
		.eax = VMPORT_MAGIC,
		.ebx = argument,
		.ecx = command,
		.edx = VMPORT_PORT,
	};

	__asm__ __volatile__("inl %%dx, %%eax"
			     : "+a"(result.eax), "+b"(result.ebx),
			       "+c"(result.ecx), "+d"(result.edx)
			     :
			     : "esi", "edi", "memory", "cc");
	return result;
}

int vmmouse_init(void)
{
	uint32_t eax = 1, ebx, ecx = 0, edx;
	struct vmport_result result;

	if (!fb_is_enabled())
		return 0;
	__asm__ __volatile__("cpuid"
			     : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
	if (!(ecx & (1U << 31)))
		return 0;
	result = vmport_command(VMPORT_GET_VERSION, ~VMPORT_MAGIC);
	if (result.ebx != VMPORT_MAGIC || result.eax == 0xffffffffU)
		return 0;
	vmport_command(VMMOUSE_COMMAND, VMMOUSE_READ_ID);
	result = vmport_command(VMMOUSE_STATUS, 0);
	if (result.eax != 1)
		goto failed;
	result = vmport_command(VMMOUSE_DATA, 1);
	if (result.eax != VMMOUSE_VERSION)
		goto failed;
	vmport_command(VMMOUSE_COMMAND, VMMOUSE_REQUEST_ABSOLUTE);
	return 1;

failed:
	vmport_command(VMMOUSE_COMMAND, VMMOUSE_DISABLE);
	return 0;
}

int vmmouse_read_event(struct mario_psaux_event *event)
{
	struct vmport_result result = vmport_command(VMMOUSE_STATUS, 0);

	if (result.eax > VMMOUSE_QUEUE_WORDS || result.eax % 4)
		goto failed;
	if (!result.eax)
		return 0;
	result = vmport_command(VMMOUSE_DATA, 4);
	if ((result.eax & VMMOUSE_RELATIVE_PACKET) ||
	    result.ebx > PSAUX_ABSOLUTE_MAX || result.ecx > PSAUX_ABSOLUTE_MAX)
		goto failed;
	*event = (struct mario_psaux_event) {
		.x = result.ebx,
		.y = result.ecx,
		.dz = (int8_t)result.edx,
		.buttons = ((result.eax & 0x20) ? 1 : 0) |
			   ((result.eax & 0x10) ? 2 : 0) |
			   ((result.eax & 0x08) ? 4 : 0),
		.flags = PSAUX_EVENT_ABSOLUTE,
	};
	return 1;

failed:
	vmport_command(VMMOUSE_COMMAND, VMMOUSE_DISABLE);
	return -1;
}
