#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>

#include <app/util.h>

int main(int argc, char *argv[], char *envp[])
{
	ListDir("/mnt");

	int err = mount("/dev/rd2", "/mnt", "mariofs");
	if (-1 == err)
		Exit();

	ListDir("/mnt");
	PrintFile("/mnt/quick/brown/fox.txt");

	err = umount("/mnt");
	if (-1 == err)
		Exit();

	return 0;
}
