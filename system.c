#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <kernel.h>
#include <limits.h>
#include <libpad.h>
#include <fileXio_rpc.h>
#include <atahw.h>
#include <hdd-ioctl.h>

#include <libgs.h>

#include "main.h"
#include "iop.h"
#include "menu.h"
#include "UI.h"
#include "fsck/fsck-ioctl.h"
#include "system.h"

extern void *_gp;

int GetBootDeviceID(const char *path){
	int result;

	if(!strncmp(path, "hdd:", 4) || !strncmp(path, "hdd0:", 5)) result=BOOT_DEVICE_HDD;
	else result=BOOT_DEVICE_UNKNOWN;

	return result;
}

int GetConsoleRegion(void)
{
	static int region = -1;
	FILE *file;

	if(region < 0)
	{
		if((file = fopen("rom0:ROMVER", "r")) != NULL)
		{
			fseek(file, 4, SEEK_SET);
			switch(fgetc(file))
			{
				case 'J':
					region = CONSOLE_REGION_JAPAN;
					break;
				case 'A':
				case 'H':
					region = CONSOLE_REGION_USA;
					break;
				case 'E':
					region = CONSOLE_REGION_EUROPE;
					break;
				case 'C':
					region = CONSOLE_REGION_CHINA;
					break;
			}

			fclose(file);
		}
	}

	return region;
}

int GetConsoleVMode(void)
{
	switch(GetConsoleRegion())
	{
		case CONSOLE_REGION_EUROPE:
			return 1;
		default:
			return 0;
	}
}

int SysCreateThread(void *function, void *stack, unsigned int StackSize, void *arg, int priority){
	ee_thread_t ThreadData;
	int ThreadID;

	ThreadData.func=function;
	ThreadData.stack=stack;
	ThreadData.stack_size=StackSize;
	ThreadData.gp_reg=&_gp;
	ThreadData.initial_priority=priority;
	ThreadData.attr=ThreadData.option=0;

	if((ThreadID=CreateThread(&ThreadData))>=0){
		if(StartThread(ThreadID, arg)<0){
			DeleteThread(ThreadID);
			ThreadID=-1;
		}
	}

	return ThreadID;
}

static unsigned int ErrorsFixed;

static int FsckDisk(const char *partition)
{
	char cmd[64];
	struct fsckStatus status;
	int fd, result, InitSemaID;

	InitSemaID = IopInitStart(IOP_REBOOT | IOP_MOD_FSCK | IOP_MOD_HDD);
	result = 0;

	DisplayFlashStatusUpdate(SYS_UI_MSG_PLEASE_WAIT);

	WaitSema(InitSemaID);
	DeleteSema(InitSemaID);

	sprintf(cmd, "fsck:%s", partition);
	if((fd = fileXioOpen(cmd, 0, FSCK_MODE_VERBOSITY(0)|FSCK_MODE_AUTO|FSCK_MODE_WRITE)) >= 0)
	{
		if((result = fileXioIoctl2(fd, FSCK_IOCTL2_CMD_START, NULL, 0, NULL, 0)) == 0)
		{
			result = fileXioIoctl2(fd, FSCK_IOCTL2_CMD_WAIT, NULL, 0, NULL, 0);
		}

		if (result == 0 && (result = fileXioIoctl2(fd, FSCK_IOCTL2_CMD_GET_STATUS, NULL, 0, &status, sizeof(status))) == 0)
			result = (status.errorCount != status.fixedErrorCount);

		if(result == 0)
			ErrorsFixed += status.fixedErrorCount;

		fileXioClose(fd);
	} else
		result = fd;

	return result;
}

int ScanDisk(int unit)
{
	char ErrorPartName[64] = "hdd0:";

	int result, InitSemaID;

	DisplayFlashStatusUpdate(SYS_UI_MSG_PLEASE_WAIT);

	if(fileXioDevctl("hdd0:", APA_DEVCTL_GET_SECTOR_ERROR, NULL, 0, NULL, 0) == 0)
	{
		if(fileXioDevctl("hdd0:", APA_DEVCTL_GET_ERROR_PART_NAME, NULL, 0, &ErrorPartName[5], sizeof(ErrorPartName) - 5) != 0)
		{
			ErrorsFixed = 0;

			if((result = FsckDisk(ErrorPartName)) == 0)
				DisplayScanCompleteResults(ErrorsFixed);
			else
				DisplayErrorMessage(SYS_UI_MSG_HDD_FAULT);

			InitSemaID = IopInitStart(IOP_REBOOT | IOP_MOD_HDD | IOP_MOD_PFS);
			DisplayFlashStatusUpdate(SYS_UI_MSG_PLEASE_WAIT);
			WaitSema(InitSemaID);
			DeleteSema(InitSemaID);
		} else
			DisplayScanCompleteResults(0);	//No fault. Why are we here then?
	} else
		DisplayErrorMessage(SYS_UI_MSG_HDD_FAULT);

	return result;
}
