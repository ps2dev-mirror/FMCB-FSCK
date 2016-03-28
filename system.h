enum BootDeviceIDs{
	BOOT_DEVICE_UNKNOWN = -1,
	BOOT_DEVICE_HDD,

	BOOT_DEVICE_COUNT,
};

enum CONSOLE_REGION{
	CONSOLE_REGION_JAPAN	= 0,
	CONSOLE_REGION_USA,	//USA and Asia
	CONSOLE_REGION_EUROPE,
	CONSOLE_REGION_CHINA,

	CONSOLE_REGION_COUNT
};

int GetBootDeviceID(const char *path);
int GetConsoleRegion(void);
int GetConsoleVMode(void);
int SysCreateThread(void *function, void *stack, unsigned int StackSize, void *arg, int priority);
int ScanDisk(int unit);
