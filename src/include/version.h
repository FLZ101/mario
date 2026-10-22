#ifndef _VERSION_H
#define _VERSION_H

#include <build_info.h>

#ifndef MARIO_BUILD_COMMIT
#define MARIO_BUILD_COMMIT	""
#endif

#define MARIO_OS_NAME		"Mario"
#define MARIO_OS_VERSION	"1.0"
#define MARIO_NODENAME		"PC"
#define MARIO_KERNEL_RELEASE	"0.1.0"
#define MARIO_KERNEL_VERSION	"#1" MARIO_BUILD_COMMIT
#define MARIO_MACHINE		"i386"
#define MARIO_DOMAINNAME	""

#endif
