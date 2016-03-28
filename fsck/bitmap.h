typedef struct pfs_bitmap{
	struct pfs_bitmap *next;	//0x00
	struct pfs_bitmap *prev;	//0x04
	unsigned short int isDirty;	//0x08
	unsigned short int nused;	//0x0A
	u32 index;			//0x0C
	u32 *bitmap;			//0x10
} pfs_bitmap_t;

pfs_cache_t *pfsBitmapReadPartition(pfs_mount_t *mount, unsigned short int subpart, unsigned int chunk);
pfs_bitmap_t *pfsBitmapRead(u32 index);
void pfsBitmapFree(pfs_bitmap_t *bitmap);
u32 *pfsGetBitmapEntry(u32 index);
int pfsBitmapPartInit(u32 size);
int pfsBitmapInit(void);
