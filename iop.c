#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <libpad.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

#include <fileXio_rpc.h>

#include "main.h"
#include "iop.h"

extern unsigned char SIO2MAN_irx_start[];
extern unsigned int SIO2MAN_irx_size;

extern unsigned char PADMAN_irx_start[];
extern unsigned int PADMAN_irx_size;

extern unsigned char POWEROFF_irx_start[];
extern unsigned int POWEROFF_irx_size;

extern unsigned char DEV9_irx_start[];
extern unsigned int DEV9_irx_size;

extern unsigned char ATAD_irx_start[];
extern unsigned int ATAD_irx_size;

extern unsigned char HDD_irx_start[];
extern unsigned int HDD_irx_size;

extern unsigned char PFS_irx_start[];
extern unsigned int PFS_irx_size;

extern unsigned char FSCK_irx_start[];
extern unsigned int FSCK_irx_size;

extern unsigned char IOMANX_irx_start[];
extern unsigned int IOMANX_irx_size;

extern unsigned char FILEXIO_irx_start[];
extern unsigned int FILEXIO_irx_size;

#define SYSTEM_INIT_THREAD_STACK_SIZE	0x1000

struct SystemInitParams{
	int InitCompleteSema;
	unsigned int flags;
};

static void SystemInitThread(struct SystemInitParams *SystemInitParams)
{
	static const char HDD_args[]="-o\0""3\0""-n\0""16";
	static const char PFS_args[]="-o\0""2\0""-n\0""12";
	int i;

	SifExecModuleBuffer(ATAD_irx_start, ATAD_irx_size, 0, NULL, NULL);
	if(SystemInitParams->flags & IOP_MOD_HDD)
		SifExecModuleBuffer(HDD_irx_start, HDD_irx_size, sizeof(HDD_args), HDD_args, NULL);
	if(SystemInitParams->flags & IOP_MOD_PFS)
		SifExecModuleBuffer(PFS_irx_start, PFS_irx_size, sizeof(PFS_args), PFS_args, NULL);
	if(SystemInitParams->flags & IOP_MOD_FSCK)
		SifExecModuleBuffer(FSCK_irx_start, FSCK_irx_size, 0, NULL, NULL);

	SifExitIopHeap();
	SifLoadFileExit();

	SignalSema(SystemInitParams->InitCompleteSema);
	ExitDeleteThread();
}

int IopInitStart(unsigned int flags)
{
	ee_sema_t ThreadSema;
	static struct SystemInitParams InitThreadParams;
	static unsigned char SysInitThreadStack[SYSTEM_INIT_THREAD_STACK_SIZE] ALIGNED(64);

	if(!(flags & IOP_REBOOT))
	{
		SifInitRpc(0);
	} else {
		PadDeinitPads();
	}

	while(!SifIopReset(NULL, 0)){};

	//Do something useful while the IOP resets.
	ThreadSema.init_count=0;
	ThreadSema.max_count=1;
	ThreadSema.attr=ThreadSema.option=0;
	InitThreadParams.InitCompleteSema=CreateSema(&ThreadSema);
	InitThreadParams.flags = flags;

	while(!SifIopSync()){};

	SifInitRpc(0);
	SifInitIopHeap();
	SifLoadFileInit();

	sbv_patch_enable_lmb();

	SifExecModuleBuffer(IOMANX_irx_start, IOMANX_irx_size, 0, NULL, NULL);
	SifExecModuleBuffer(FILEXIO_irx_start, FILEXIO_irx_size, 0, NULL, NULL);

	fileXioInit();

	SifExecModuleBuffer(POWEROFF_irx_start, POWEROFF_irx_size, 0, NULL, NULL);
	SifExecModuleBuffer(DEV9_irx_start, DEV9_irx_size, 0, NULL, NULL);

	SifExecModuleBuffer(SIO2MAN_irx_start, SIO2MAN_irx_size, 0, NULL, NULL);
	SifExecModuleBuffer(PADMAN_irx_start, PADMAN_irx_size, 0, NULL, NULL);

	SysCreateThread(SystemInitThread, SysInitThreadStack, SYSTEM_INIT_THREAD_STACK_SIZE, &InitThreadParams, 0x1);

	PadInitPads();

	return InitThreadParams.InitCompleteSema;
}
