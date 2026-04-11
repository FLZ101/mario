#ifndef _UTSNAME_H
#define _UTSNAME_H

/*
 * The length of the arrays in a struct utsname is unspecified;
 * the fields are terminated by a null byte ('\0').
 */
struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

#endif
