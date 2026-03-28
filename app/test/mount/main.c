#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>

#include <app/util.h>

int main(int argc, char *argv[], char *envp[])
{
	ListDir("/mnt");

	// Will fail if the number of ramdisks loaded is less than 3
	int err = mount("/dev/rd2", "/mnt", "mariofs", 0, NULL);
	if (-1 == err)
		Exit();

	ListDir("/mnt");
	PrintFile("/mnt/quick/brown/fox.txt");

	err = umount("/mnt");
	if (-1 == err)
		Exit();

	ListDir("/mnt");
	return 0;
}
