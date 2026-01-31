#include <app/util.h>

int main(int argc, char *argv[], char *envp[])
{
	cat("/etc/welcome.txt");
	run("/bin/test_mount.exe");
	return 0;
}
