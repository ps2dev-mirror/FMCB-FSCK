#define IOP_MOD_HDD	0x01
#define IOP_MOD_PFS	0x02
#define IOP_MOD_FSCK	0x10
#define IOP_REBOOT	0x80

int IopInitStart(unsigned int flags);
