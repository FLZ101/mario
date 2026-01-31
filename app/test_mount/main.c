#include <stdio.h>
#include <stdlib.h>
#include <mount.h>

#include <app/util.h>

int main(int argc, char *argv[], char *envp[])
{
	int err = mount("/dev/rd2", "/mnt", "mariofs");
	if (-1 == err) {
		_perror();
		exit(EXIT_FAILURE);
	}

	cat("/mnt/quick/brown/fox.txt");

	err = umount("/mnt");
	if (-1 == err) {
		_perror();
		exit(EXIT_FAILURE);
	}

	return 0;
}
