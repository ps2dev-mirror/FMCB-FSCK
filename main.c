#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <sys/fcntl.h>
#include <hdd-ioctl.h>

#include <libgs.h>

#include "main.h"
#include "iop.h"
#include "system.h"
#include "UI.h"
#include "menu.h"
#include "pad.h"

static int SetMountPath(void)
{
	char BlockDevice[38], command[256];
	const char *MountPath;
	int BlockDeviceNameLen, result;

	//getpwd will return the path, without the filename (as parsed by libc's init.c).
	getcwd(command, sizeof(command));
	if(strlen(command)>6 && (MountPath=strchr(&command[5], ':'))!=NULL){
		BlockDeviceNameLen = (unsigned int)MountPath-(unsigned int)command;
		strncpy(BlockDevice, command, BlockDeviceNameLen);
		BlockDevice[BlockDeviceNameLen]='\0';

		MountPath++;	//This is the location of the mount path;

		if((result = fileXioMount("pfs0:", BlockDevice, FIO_MT_RDONLY)) >= 0)
			result = chdir(MountPath);

		return result;
	}

	return -EINVAL;
}

int VBlankStartSema;

static int VBlankStartHandler(int cause)
{
	ee_sema_t sema;
	iReferSemaStatus(VBlankStartSema, &sema);
	if(sema.count<sema.max_count) iSignalSema(VBlankStartSema);
	return 0;
}

static void DeinitIntrHandlers(void)
{
	DisableIntc(kINTC_VBLANK_START);
	RemoveIntcHandler(kINTC_VBLANK_START, 0);
	DeleteSema(VBlankStartSema);
}

static void DeinitServices(void)
{
	PadDeinitPads();

	DeinitIntrHandlers();

	fileXioExit();
	SifExitRpc();
}

int main(int argc, char *argv[])
{
	unsigned char PadStatus, done;
	unsigned int FrameNum;
	ee_sema_t ThreadSema;
	int result, InitSemaID, BootDevice;

	if(argc<1 || (BootDevice = GetBootDeviceID(argv[0])) == BOOT_DEVICE_UNKNOWN)
		Exit(-1);

	InitSemaID = IopInitStart(IOP_MOD_HDD | IOP_MOD_PFS);

	ThreadSema.init_count=0;
	ThreadSema.max_count=1;
	ThreadSema.attr=ThreadSema.option=0;
	VBlankStartSema=CreateSema(&ThreadSema);

	AddIntcHandler(kINTC_VBLANK_START, &VBlankStartHandler, 0);
	EnableIntc(kINTC_VBLANK_START);

	if(BootDevice != BOOT_DEVICE_HDD)
	{
		if(InitializeUI(0)!=0)
		{
			DeinitIntrHandlers();
			DeinitializeUI();
			fileXioExit();
			SifExitRpc();
			Exit(-1);
		}

		FrameNum=0;
		/* Draw something nice here while waiting... */
		do{
			RedrawLoadingScreen(FrameNum);
			FrameNum++;
		}while(PollSema(InitSemaID)!=InitSemaID);
	} else
		WaitSema(InitSemaID);
	DeleteSema(InitSemaID);

	if(BootDevice == BOOT_DEVICE_HDD)
	{
		if(SetMountPath() != 0)
		{
			DeinitServices();
			Exit(-1);
		}

		if(InitializeUI(1)!=0)
		{
			DeinitializeUI();

			fileXioUmount("pfs0:");
			DeinitServices();
			Exit(-1);
		}

		fileXioUmount("pfs0:");
	}

	if(fileXioDevctl("hdd0:", HDIOC_STATUS, NULL, 0, NULL, 0) == 0)
	{
		MainMenu();
	} else {
		DisplayErrorMessage(SYS_UI_MSG_NO_HDD);
	}

	DeinitializeUI();
	if(BootDevice == BOOT_DEVICE_HDD)
		fileXioUmount("pfs0:");
	DeinitServices();

	return 0;
}
