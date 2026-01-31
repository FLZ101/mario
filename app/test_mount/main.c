#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>

#include <app/util.h>

int main(int argc, char *argv[], char *envp[])
{
	ls("/mnt");

	int err = mount("/dev/rd2", "/mnt", "mariofs");
	if (-1 == err) {
		_perror();
		exit(EXIT_FAILURE);
	}

	ls("/mnt");
	cat("/mnt/quick/brown/fox.txt");

	err = umount("/mnt");
	if (-1 == err) {
		_perror();
		exit(EXIT_FAILURE);
	}

	return 0;
}
