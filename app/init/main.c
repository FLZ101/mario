#include <app/util.h>

int main(int argc, char *argv[], char *envp[])
{
	cat("/etc/welcome.txt");
	run("/bin/test_mm.exe");
	return 0;
}
