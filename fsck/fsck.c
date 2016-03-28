#include <errno.h>
#include <irx.h>
#include <iomanX.h>
#include <loadcore.h>
#include <thbase.h>
#include <thevent.h>
#include <stdio.h>
#include "stdio_add.h"
#include <sysclib.h>

#include "pfs-opt.h"
#include "libpfs.h"
#include "hdd-ioctl.h"
#include "bitmap.h"
#include "misc.h"

#include "fsck-ioctl.h"

IRX_ID("fsck", 1, 4);

struct fsckRuntimeData{
	struct fsckStatus status;	//0x00
	int hasError;			//0x1C
	int stopFlag;			//0x20
};

static int fsckWriteEnabled;
static int fsckAutoMode;
static int fsckVerbosityLevel;
extern u32 pfsMetaSize;
extern int pfsBlockSize;
static pfs_mount_t MainPFSMount = {0};	//FIXME: if not explicitly initialized to 0, the generated IRX would somehow have garbage in this structure.

#define IO_BUFFER_SIZE			256
#define IO_BUFFER_SIZE_BYTES	(IO_BUFFER_SIZE * 512)

static struct fsckRuntimeData fsckRuntimeData;
static unsigned char IOBuffer[IO_BUFFER_SIZE_BYTES];

#define FSCK_NUM_SUPPORTED_DEVICES	1
#define FSCK_MAX_PATH_LEVELS		64
#define FSCK_MAX_PATH_SEG_LENGTH	256

static int fsckEventFlagID;
static int fsckThreadID;
static char fsckPathBuffer[FSCK_MAX_PATH_LEVELS][FSCK_MAX_PATH_SEG_LENGTH];

static u32 ZoneSizes[0x41];	//Contains the sizes of all zones, in units of 512-byte sectors.
static u32 ZoneMap[0x41];	//Contains the starting addresses of all zones, in units of blocks.

//0x00000000
const char *fsckGetChar(void)
{
	static char buffer[80];
	const char *pChar;

	if(gets(buffer) != NULL)
	{
		for(pChar = buffer; *pChar != '\0'; pChar++)
		{
			if(look_ctype_table(*(const unsigned char*)pChar) & 0x17)
				break;
		}
	}else{
		pChar = NULL;
	}

	return pChar;
}

//0x00000068
static int fsckPromptUserAction(const char *description, int mode)
{
	int result;
	unsigned char choice;

	fsckRuntimeData.status.errorCount++;

	if(fsckWriteEnabled != 0)
	{
		if(fsckAutoMode != 0)
		{
			printf("\n");
			if(mode)
				fsckRuntimeData.status.fixedErrorCount++;
			else
				fsckRuntimeData.hasError = 1;

			result = mode;
		}else{
			printf("%s (%s)? ", description, mode == 0 ? "n/y" : "y/n");
			do
			{
				choice = *fsckGetChar();
			}while(choice != 'n' && choice != 'y');

			if(choice == 'y')
			{
				fsckRuntimeData.status.fixedErrorCount++;
				result = 1;
			}else{
				fsckRuntimeData.hasError = 1;
				result = 0;
			}
		}
	}else{
		printf("\n");
		fsckRuntimeData.hasError = 1;
		result = 0;
	}

	return result;
}

#ifndef FSCK100
static int fsckPromptUserAction2(const char *description, int mode)
{
	int result;
	unsigned char choice;

	if(fsckWriteEnabled != 0)
	{
		if(fsckAutoMode != 0)
		{
			printf("\n");
			if(mode == 0)
				fsckRuntimeData.hasError = 1;

			result = mode;
		}else{
			printf("%s (%s)? ", description, mode == 0 ? "n/y" : "y/n");
			do
			{
				choice = *fsckGetChar();
			}while(choice != 'n' && choice != 'y');

			if(choice == 'y')
			{
				result = 1;
			}else{
				fsckRuntimeData.hasError = 1;
				result = 0;
			}
		}
	}else{
		printf("\n");
		fsckRuntimeData.hasError = 1;
		result = 0;
	}

	return result;
}
#endif

//0x00002654
static int DisplayUsageHelp(void)
{
	printf("fsck: error: Usage: fsck [-n <num>]\n");
	return MODULE_NO_RESIDENT_END;
}

#ifndef FSCK100
static int fsckCheckExtendedAttribute(pfs_mount_t *mount)
{
	int remaining, size, result;

	lseek(mount->fd, 0, SEEK_SET);
	for(result = 0,remaining = 0x1FF8; remaining != 0; remaining -= size)
	{
		size = (remaining - IO_BUFFER_SIZE > 0) ? IO_BUFFER_SIZE : remaining;

		if(read(mount->fd, IOBuffer, size * 512) == -EIO)
		{
			printf("fsck: cannot read extended attribute %d\n", -EIO);
			if(fsckPromptUserAction(" nullify extended attribute", 1) == 0)
				break;

			memset(IOBuffer, 0, IO_BUFFER_SIZE_BYTES);
			lseek(mount->fd, 0, SEEK_SET);
			for(remaining = 0x1FF8; remaining != 0; remaining -= size)
			{
				size = (remaining - IO_BUFFER_SIZE > 0) ? IO_BUFFER_SIZE : remaining;

				if((result = write(mount->fd, IOBuffer, size * 512)) < 0)
				{
					printf("fsck: error: could not nullify extended attribute.\n");
					break;
				}
			}

			break;
		}
	}

	return result;
}
#endif

static int fsckCheckDirentryInode(pfs_cache_t *clink);

//0x0000183c
static pfs_cache_t *CheckRootDirectory(pfs_mount_t *mount)
{
	pfs_cache_t *pResultClink, *clink;
	int result;

	if((clink = pfsInodeGetData(mount, mount->root_dir.subpart, mount->root_dir.number, &result)) != NULL)
	{
		if(fsckVerbosityLevel >= 2)
			printf("/: ");
		if(fsckCheckDirentryInode(clink) == 0)
			pResultClink = clink;
		else{
			pfsCacheFree(clink);
			pResultClink = NULL;
		}
	}else{
		pResultClink = NULL;
	}

	return pResultClink;
}

//0x000007a8
static void pfsPrintPWD(void)
{
	int i;
	char *pName;

	for(i = 0, pName = fsckPathBuffer[0]; i < fsckRuntimeData.status.PWDLevel; i++, pName += FSCK_MAX_PATH_SEG_LENGTH)
		printf("/%s", pName);

	if(i == 0)
		printf("/");
}

//0x000008c8
static int pfsInitDirEnt(pfs_mount_t *mount, short int subpart, int inodeNumber, unsigned int number, int arg5)
{
	pfs_cache_t *clink;
	u32 *pDentry;
	int result;

	result = -EIO;
	if(fsckPromptUserAction(" initialize directory entry", 1) != 0)
	{
		if((clink = pfsCacheGetData(mount, subpart, number, PFS_CACHE_FLAG_NOLOAD, &result)) != NULL)
		{
			memset(clink->u.dentry, 0, pfsMetaSize);
			pDentry = (u32*)clink->u.dentry;

			/*	This is very much like pfs_dentry, but used for the first two entries of a directory.
				But I doubt that SCEI actually used a structure here because FSCK performs sw operations to fill the structures.

				typedef struct {
					u32	inode;
					u8	sub;
					u8	pLen;
					u16	aLen;
					char	path[4];
				} pfs_dentry_dir;	*/

			if(arg5)
			{
				pDentry[1] = 0x100C0100 | subpart;	//sub = sub, pLen = 1, aLen = FIO_S_IFDIR | 12
				pDentry[2] = 0x0000002E;		//"."
				pDentry[0] = inodeNumber;
				pDentry[3] = inodeNumber;

				pDentry[4] = 0x11F40200 | subpart;	//sub = sub, pLen = 2, aLen = FIO_S_IFDIR | 500
				pDentry[5] = 0x00002E2E;		//".."
			}else{
				pDentry[1] = 0x02000000;
			}

			pDentry[129] = 0x02000000;
			clink->flags |= PFS_CACHE_FLAG_DIRTY;
			pfsCacheFree(clink);
		}
	}

	return result;
}

//0x000001c0
static int fsckCheckFileBitmap(pfs_mount_t *mount, pfs_blockinfo *blockinfo)
{
	pfs_bitmapInfo_t bitmapinfo;
	pfs_cache_t *clink;
	unsigned int chunk, bit, count;
	u32 *pBitmap;

	pfsBitmapSetupInfo(mount, &bitmapinfo, blockinfo->subpart, blockinfo->number);
	for(count = blockinfo->count; count != 0; )
	{
		chunk = bitmapinfo.chunk;
		bitmapinfo.chunk++;
		if((clink = pfsBitmapReadPartition(mount, blockinfo->subpart, chunk)) != NULL)
		{
			for(pBitmap = &clink->u.bitmap[bitmapinfo.index]; (pBitmap < &clink->u.bitmap[0x100]) && count != 0; pBitmap++,bitmapinfo.bit = 0)
			{
				for(bit = bitmapinfo.bit; (bit < 32) && (count != 0); bit++,count--)
				{
					if((*pBitmap & (1 << bit)) == 0)
					{
						printf("fsck: not marked as used.\n");
						if(fsckPromptUserAction(" Mark in use", 1) != 0)
						{
							*pBitmap |= (1 << bit);
							clink->flags |= PFS_CACHE_FLAG_DIRTY;
						}else
							return -EINVAL;
					}
				}
			}

			bitmapinfo.index = 0;
			pfsCacheFree(clink);
		}else{
			break;
		}
	}

	return 0;
}

//0x00005550
static int fsckCheckZones(unsigned int number, unsigned int size)
{
	unsigned int index, zone, remaining, bit, startBit;
	u32 *pZone;
	pfs_bitmap_t *pBitmap;

	index = number >> 20;
	zone = (number >> 5) & 0x7FFF;
	startBit = number & 31;
	for(remaining = size; remaining != 0; )
	{
		pBitmap = pfsBitmapRead(index);
		index++;

		if(pBitmap != NULL)
		{
			for(pZone = &pBitmap->bitmap[zone]; pZone < pBitmap->bitmap + 0x8000 && remaining != 0; pZone++,startBit = 0)
			{
				for(bit = startBit; bit < 32 && remaining != 0; bit++,remaining--)
				{
					if((*pZone & (1 << bit)) != 0)
					{
						printf("fsck: error: overlapped zone found.\n");
						return 1;
					}

					*pZone |= (1 << bit);
					pBitmap->isDirty = 1;
				}
			}

			pfsBitmapFree(pBitmap);
			zone = 0;
		}else{
			return -EIO;
		}
	}

	return 0;
}

//0x000009dc	- BUGBUG - if there's no next segment (clink2 == NULL), this function will end up dereferencing a NULL pointer.
static void fsckFillInode(pfs_cache_t *clink, pfs_cache_t *clink2, u32 blocks, u32 entries, u32 segdesg)
{
	memset(&clink2->u.inode->next_segment, 0, sizeof(clink2->u.inode->next_segment));

	clink->u.inode->number_segdesg = (blocks - segdesg) * clink->pfsMount->zsize;
	clink->u.inode->subpart = 0;
	if(!FIO_S_ISDIR(clink->u.inode->mode))
		clink->u.inode->attr &= ~0x80;	//Clears sceMcFileAttrClosed

	clink->u.inode->number_blocks = blocks;
	clink->u.inode->number_data = entries;
	clink->u.inode->number_segdesg = segdesg;
	clink->u.inode->next_segment.subpart = clink2->sub;
	clink->u.inode->last_segment.number = clink2->sector >> clink2->pfsMount->inode_scale;
	clink2->flags |= PFS_CACHE_FLAG_DIRTY;
	clink->flags |= PFS_CACHE_FLAG_DIRTY;
}

//0x00000b04	- I hate this function and it hates me.
static int fsckCheckDirentryInode(pfs_cache_t *clink)
{
	int i, result;
	pfs_cache_t *clink2, *clink3;
	pfs_blockinfo *pInodeInfo;
	u32 index, new_index, inodeOffset, blocks, segdesg, inodeStart, sector;

	inodeOffset = 0;
	blocks = 0;
	segdesg = 1;
	result = 0;
	clink2 = pfsCacheUsedAdd(clink);

	for(index = 0; (result == 0) && (index < clink->u.inode->number_data); index++)	//While no error occurs.
	{
		new_index = pfsFixIndex(index);

		if(index != 0 && new_index == 0)
		{
			pInodeInfo = &clink2->u.inode->next_segment;
			pfsCacheFree(clink2);
			if((clink2 = pfsCacheGetData(clink->pfsMount, pInodeInfo->subpart, pInodeInfo->number << clink->pfsMount->inode_scale, PFS_CACHE_FLAG_SEGI, &result)) == NULL)
			{
				if((result == -EIO) && (fsckPromptUserAction(" Remove rest of file", 1) != 0))
					fsckFillInode(clink, NULL, blocks, index, segdesg);	//bug?! This will cause a NULL-pointer to be dereferenced!
				break;
			}

			segdesg++;
		}

		pInodeInfo = &clink2->u.inode->data[new_index];

		//0x00000c18
		if((clink->pfsMount->num_subs < pInodeInfo->subpart)
			|| (pInodeInfo->count == 0)
			|| (pInodeInfo->number < 2)
			|| (ZoneSizes[pInodeInfo->subpart] < ((pInodeInfo->number + pInodeInfo->count) << clink->pfsMount->sector_scale)))
		{
			putchar('\n');
			pfsPrintPWD();
			printf(" contains a bad zone.");
			if(fsckPromptUserAction(" Remove rest of file", 1) != 0)
			{
				fsckFillInode(clink, clink2, blocks, index, segdesg);
				break;
			}

			result = -EINVAL;
			break;
		}

		//0x00000ccc
		if(new_index != 0)
		{
			if(FIO_S_ISDIR(clink->u.inode->mode))
			{
				//0x00000cfc
				for(i = 0; i < pInodeInfo->count; i++)
				{
					inodeStart = (pInodeInfo->number + i) << clink->pfsMount->inode_scale;

					//0x00000dbc
					for(sector = 0; (sector < (1 << clink->pfsMount->inode_scale)) && (inodeOffset < clink->u.inode->size); sector++)
					{
						inodeOffset += pfsMetaSize;
						if((clink3 = pfsCacheGetData(clink->pfsMount, pInodeInfo->subpart, inodeStart + sector, 0, &result)) != NULL)
							pfsCacheFree(clink3);
						else{
							if(result == -ENOMEM) break;

							printf("fsck: could not read directory block.\n");
							if((result = pfsInitDirEnt(clink->pfsMount, pInodeInfo->subpart, inodeStart + sector, clink->u.inode->inode_block.number, (1 == index && i == 0) ? sector < 1 : 0)) < 0)
								break;
						}
					}

					//0x00000e0c
					if(fsckVerbosityLevel >= 10)
						putchar('.');
				}
			}else{
				//0x00000e50
				for(i = 0; i < pInodeInfo->count; i++)
				{
					if(clink->pfsMount->blockDev->transfer(clink->pfsMount->fd, IOBuffer, pInodeInfo->subpart, (pInodeInfo->number + i) << clink->pfsMount->sector_scale, 1 << clink->pfsMount->sector_scale, PFS_IO_MODE_READ) == 0)
					{
						if(fsckVerbosityLevel >= 10)
							putchar('.');

						if(fsckRuntimeData.stopFlag != 0)
							goto end;
					}else{
						printf("fsck: could not read zone.\n");
						if(fsckPromptUserAction(" Remove rest of file", 1) != 0)
							fsckFillInode(clink, clink2, blocks, index, segdesg);

						break;
					}
				}
			}
		}

		//0x00000ef8
		if((result = fsckCheckFileBitmap(clink->pfsMount, pInodeInfo)) >= 0)
		{
			if((result = fsckCheckZones(pInodeInfo->number + ZoneMap[pInodeInfo->subpart], pInodeInfo->count)) > 0)
			{
				if(fsckPromptUserAction(" Remove rest of file", 1) != 0)
					fsckFillInode(clink, clink2, blocks, index, segdesg);
				break;
			}else if(result >= 0){	//result == 0
				//0x00000f7c
				fsckRuntimeData.status.inodeBlockCount += pInodeInfo->count;
				blocks += pInodeInfo->count;
				if(fsckRuntimeData.stopFlag != 0)
					break;
			}else{
				break;
			}
		}else{
			break;
		}
	}

end:
	if(result < 0)
		fsckRuntimeData.hasError = 1;

	//0x00000ff0
	if(fsckVerbosityLevel >= 2)
		printf("\n");

	pfsCacheFree(clink2);

	return result;
}

//0x00000828
static void fsckFixDEntry(pfs_cache_t *clink, pfs_dentry *dentry)
{
	unsigned int dEntrySize, offset;
	unsigned short int aLen;
	pfs_dentry *pDEntryNew, *pDEntry;

	dEntrySize = (unsigned int)((u8*)clink->u.dentry - (u8*)dentry);
	if((int)dEntrySize < 0)
		dEntrySize += 0x1FF;

	dEntrySize = dEntrySize >> 9 << 9;
	pDEntryNew = (pfs_dentry*)((u8*)clink->u.dentry + dEntrySize);
	for(pDEntry = NULL,offset = 0; offset < sizeof(pfs_dentry); offset += aLen,pDEntry = pDEntryNew,pDEntryNew = (pfs_dentry*)((u8*)pDEntryNew + aLen))
	{
		aLen = pDEntryNew->aLen & 0x0FFF;

		if(pDEntryNew == dentry)
		{
			if(pDEntry != NULL)
			{
				pDEntry->aLen = (pDEntry->aLen & FIO_S_IFMT) | ((pDEntry->aLen & 0x0FFF) + (pDEntryNew->aLen & 0x0FFF));
			}else{
				pDEntryNew->inode = 0;
				pDEntryNew->pLen = 0;
			}

			clink->flags |= PFS_CACHE_FLAG_DIRTY;
			break;
		}
	}
}

//0x000012b4
static void fsckCheckSelfEntry(pfs_cache_t *SelfInodeClink, pfs_cache_t *SelfDEntryClink, pfs_dentry *dentry)
{
	if((SelfInodeClink->sub != dentry->sub) || (SelfInodeClink->u.inode->inode_block.number != dentry->inode))
	{
		printf("fsck: '.' point not itself.\n");
		if(fsckPromptUserAction(" Fix", 1) != 0)
		{
			dentry->sub = SelfInodeClink->u.inode->inode_block.subpart;
			dentry->inode =  SelfInodeClink->u.inode->inode_block.number;
			SelfDEntryClink->flags |= PFS_CACHE_FLAG_DIRTY;
		}
	}
}

//0x00001374
static void fsckCheckParentEntry(pfs_cache_t *ParentInodeClink, pfs_cache_t *SelfDEntryClink, pfs_dentry *dentry)
{
	if((ParentInodeClink->sub != dentry->sub) || (ParentInodeClink->u.inode->inode_block.number != dentry->inode))
	{
		printf("fsck: '..' point not parent.\n");
		if(fsckPromptUserAction(" Fix", 1) != 0)
		{
			dentry->sub = ParentInodeClink->u.inode->inode_block.subpart;
			dentry->inode =  ParentInodeClink->u.inode->inode_block.number;
			SelfDEntryClink->flags |= PFS_CACHE_FLAG_DIRTY;
		}
	}
}

static void fsckCheckFiles(pfs_cache_t *ParentInodeClink, pfs_cache_t *InodeClink);

//0x00001054
static void fsckCheckFile(pfs_cache_t *FileInodeClink, pfs_cache_t *FileInodeDataClink, pfs_dentry *dentry)
{
	if(fsckRuntimeData.status.PWDLevel < FSCK_MAX_PATH_LEVELS - 1)
	{
		memset(fsckPathBuffer[fsckRuntimeData.status.PWDLevel], 0, FSCK_MAX_PATH_SEG_LENGTH);
		strncpy(fsckPathBuffer[fsckRuntimeData.status.PWDLevel], dentry->path, dentry->pLen);
		fsckRuntimeData.status.PWDLevel++;

		if(fsckVerbosityLevel >= 2)
		{
			pfsPrintPWD();
			if(FIO_S_ISDIR(dentry->aLen))
				printf(": ");
		}

		if(FIO_S_ISREG(dentry->aLen))
		{
			if(FileInodeDataClink->pfsMount->blockDev->transfer(FileInodeDataClink->pfsMount->fd, IOBuffer, FileInodeDataClink->sub, (FileInodeDataClink->sector + 1) << pfsBlockSize, 1 << pfsBlockSize, PFS_IO_MODE_READ) != 0)
			{
				printf("fsck: could not read extended attribute.\n");
				if(fsckPromptUserAction(" initialize attribute", 1) != 0)
				{
					memset(IOBuffer, 0, 1024);
					((pfs_aentry_t*)IOBuffer)->aLen = 1024;
					if(FileInodeDataClink->pfsMount->blockDev->transfer(FileInodeDataClink->pfsMount->fd, IOBuffer, FileInodeDataClink->sub, (FileInodeDataClink->sector + 1) << pfsBlockSize, 1 << pfsBlockSize, PFS_IO_MODE_WRITE) != 0)
						fsckRuntimeData.hasError = 1;
				}
			}
		}

		//0x00001220
		fsckCheckDirentryInode(FileInodeDataClink);
		if(FIO_S_ISDIR(dentry->aLen))
		{
			fsckRuntimeData.status.directories++;
			fsckCheckFiles(FileInodeClink, FileInodeDataClink);
		}else
			fsckRuntimeData.status.files++;

		fsckRuntimeData.status.PWDLevel--;
	}else{
		printf("fsck: error: exceed max directory depth.\n");
		fsckRuntimeData.hasError = 1;
	}
}

//0x00001434
static void fsckCheckFiles(pfs_cache_t *ParentInodeClink, pfs_cache_t *InodeClink)
{
	pfs_blockpos_t BlockPosition;
	pfs_dentry *pDEntry, *pDEntryEnd;
	int result;
	unsigned int inodeOffset, dEntrySize;	//inodeOffset doesn't seem to be 64-bit, even though the inode size field is 64-bits wide.
	pfs_cache_t *DEntryClink, *FileInodeDataClink;

	inodeOffset = 0;
	BlockPosition.inode = pfsCacheUsedAdd(InodeClink);
	BlockPosition.block_segment = 1;
	BlockPosition.block_offset = 0;
	BlockPosition.byte_offset = 0;
	if((DEntryClink = pfsGetDentriesChunk(&BlockPosition, &result)) == NULL)
	{
		pfsCacheFree(BlockPosition.inode);
		fsckRuntimeData.hasError = 1;
		return;
	}
	pDEntry = DEntryClink->u.dentry;

	//0x000017c0
	while(inodeOffset < InodeClink->u.inode->size)
	{
		//0x000014cc
		if(pDEntry >= (pfs_dentry*)(DEntryClink->u.data + 1024))
		{
			//0x000014e4
			pfsCacheFree(DEntryClink);
			if(pfsInodeSync(&BlockPosition, 1024, InodeClink->u.inode->number_data) != 0 || (DEntryClink = pfsGetDentriesChunk(&BlockPosition, &result)) == NULL)
			{
				fsckRuntimeData.hasError = 1;
				goto end;
			}
		}

		//0x0000153c
		for(pDEntry = DEntryClink->u.dentry, pDEntryEnd = DEntryClink->u.dentry + 1; pDEntry < pDEntryEnd; pDEntry = (pfs_dentry*)((u8*)pDEntry + dEntrySize),inodeOffset += dEntrySize)
		{
			if(fsckRuntimeData.stopFlag != 0)
				goto end;

			dEntrySize = pDEntry->aLen & 0x0FFF;

			if(dEntrySize & 3)
			{
				dEntrySize = (unsigned int)((u8*)pDEntryEnd - (u8*)pDEntry);
				printf("fsck: directory entry is not aligned.\n");

				if(fsckPromptUserAction(" Fix", 1) != 0)
				{
					pDEntry->aLen = (pDEntry->aLen & 0xF000) | dEntrySize;
					DEntryClink->flags |= PFS_CACHE_FLAG_DIRTY;
				}
			}

			if(dEntrySize < ((pDEntry->pLen + 11) & ~3))
			{
				dEntrySize = (unsigned int)((u8*)pDEntryEnd - (u8*)pDEntry);
				printf("fsck: directory entry is too small.\n");

				if(fsckPromptUserAction(" Fix", 1) != 0)
				{
					if(((pDEntry->pLen + 11) & ~3) < dEntrySize)
					{
						pDEntry->aLen = (pDEntry->aLen & 0xF000) | dEntrySize;
						DEntryClink->flags |= PFS_CACHE_FLAG_DIRTY;
					}else{
						fsckFixDEntry(DEntryClink, pDEntry);
						pDEntry->inode = 0;
					}
				}else{
					pDEntry->inode = 0;
				}
			}

			//0x00001654
			if(pDEntryEnd < (pfs_dentry*)((u8*)pDEntry + dEntrySize))
			{
				dEntrySize = (unsigned int)((u8*)pDEntryEnd - (u8*)pDEntry);
				printf("fsck: directory entry is too long.\n");
				if(fsckPromptUserAction(" Fix", 1) != 0)
					fsckFixDEntry(DEntryClink, pDEntry);

				pDEntry->inode = 0;
			}

			//0x00001694
			if(pDEntry->inode != 0)
			{
				if((pDEntry->pLen == 1) && (pDEntry->path[0] == '.'))
					fsckCheckSelfEntry(InodeClink, DEntryClink, pDEntry);
				else if((pDEntry->pLen == 2) && (pDEntry->path[0] == '.'))
					fsckCheckParentEntry(ParentInodeClink, DEntryClink, pDEntry);
				else{
					if((FileInodeDataClink = pfsInodeGetData(InodeClink->pfsMount, pDEntry->sub, pDEntry->inode, &result)) != NULL)
					{
						fsckCheckFile(InodeClink, FileInodeDataClink, pDEntry);
						pfsCacheFree(FileInodeDataClink);
					}else{
						if(result == -EIO)
						{
							printf(" contains an unreadable file '%.*s'.\n", pDEntry->pLen, pDEntry->path);
							if(fsckPromptUserAction(" Remove", 1) != 0)
								fsckFixDEntry(DEntryClink, pDEntry);
						}else
							fsckRuntimeData.hasError = 1;
					}
				}
			}
		}

		//0x000017ac
		if(fsckRuntimeData.stopFlag != 0)
			break;
	}

end:
	pfsCacheFree(DEntryClink);
	pfsCacheFree(BlockPosition.inode);
}

//0x0000054c
static unsigned int fsckCompareBitmap(pfs_mount_t *mount, void *buffer)
{
	int hasUpdate;
	unsigned int i, NumZones, zone;
	u32 *pBitmap, sector, *pRawBitmap, RawSize, unaligned, length, offset;

	for(i = 0,offset = 0; i < mount->num_subs + 1; i++,offset++)
	{
		RawSize = ZoneSizes[i] >> mount->sector_scale;
		NumZones = RawSize / (mount->zsize << 3);
		unaligned = RawSize % (mount->zsize << 3);

		for(zone = 0; (unaligned == 0 && zone < NumZones) || (unaligned != 0 && zone < NumZones + 1); zone++)
		{
			length = (zone == NumZones) ? unaligned : mount->zsize << 3;
			hasUpdate = 0;

			pBitmap = pfsGetBitmapEntry(offset + (zone * (mount->zsize << 3)));
			pRawBitmap = (u32*)IOBuffer;
			sector = (i == 0) ? (0x2000 >> mount->sector_scale) + 1 : 1;
			if(mount->blockDev->transfer(mount->fd, IOBuffer, i, (sector + zone) << mount->sector_scale, 1 << mount->sector_scale, PFS_IO_MODE_READ) < 0)
				break;

			for(; pRawBitmap < (u32*)(IOBuffer + (length >> 3)); pRawBitmap++,pBitmap++)
			{
				//0x00000698
				if(*pRawBitmap != *pBitmap)
				{
					printf("fsck: bitmap unmatch %08lx, %08lx\n", *pRawBitmap, *pBitmap);
#ifdef FSCK100
					if(fsckPromptUserAction(" Replace", 1) != 0)
#else
					if(fsckPromptUserAction2(" Replace", 1) != 0)
#endif
					{
						*pRawBitmap = *pBitmap;
						hasUpdate = 1;
					}
				}
			}

			if(hasUpdate != 0)
				mount->blockDev->transfer(mount->fd, IOBuffer, i, (sector + zone) << mount->sector_scale, 1 << mount->sector_scale, PFS_IO_MODE_WRITE);
		}
	}

	return(i << 2);
}

//0x00001f34
static void FsckThread(void *arg)
{
	pfs_cache_t *clink;

#ifdef FSCK100
	if(fsckVerbosityLevel > 0)
		printf("fsck: Check Root Directory...\n");
#else
	if(fsckVerbosityLevel > 0)
		printf("fsck: Check Extended attribute...\n");

	if(fsckCheckExtendedAttribute(arg) < 0)
	{
		printf("fsck: error: I cannot continue, giving up.\n");
		goto fsck_thread_end;
	}

	if(fsckVerbosityLevel > 0)
	{
		printf("fsck: done.\n");

		if(fsckVerbosityLevel > 0)
			printf("fsck: Check Root Directory...\n");
	}
#endif

	if((clink = CheckRootDirectory(arg)) == NULL)
	{
		printf("fsck: error: I cannot continue, giving up.\n");
		goto fsck_thread_end;
	}

	fsckRuntimeData.status.directories++;

	if(fsckVerbosityLevel > 0)
		printf("fsck: done.\n");

	if(fsckVerbosityLevel > 0)
		printf("fsck: Check all files...\n");

	fsckCheckFiles(clink, clink);

	if(fsckVerbosityLevel > 0)
		printf("fsck: done.\n");

	pfsCacheFlushAllDirty(arg);
	pfsCacheFree(clink);

	//0x00002030
	if(fsckRuntimeData.hasError == 0)
	{
		if(fsckRuntimeData.stopFlag == 0)
		{
			if(fsckVerbosityLevel > 0)
				printf("fsck: Compare bitmap...\n");

			fsckCompareBitmap(arg, IOBuffer);

			if(fsckVerbosityLevel > 0)
				printf("fsck: done.\n");
		}

		//0x000020ac
		if(fsckRuntimeData.hasError == 0)
		{
			if(fsckRuntimeData.stopFlag == 0)
			{
				if(((pfs_mount_t*)arg)->blockDev->transfer(((pfs_mount_t*)arg)->fd, IOBuffer, 0, PFS_SUPER_SECTOR, 1, PFS_IO_MODE_READ) == 0)
				{
					if(((pfs_super_block*)IOBuffer)->pfsFsckStat & 1)
					{
						((pfs_super_block*)IOBuffer)->pfsFsckStat &= ~1;
						((pfs_mount_t*)arg)->blockDev->transfer(((pfs_mount_t*)arg)->fd, IOBuffer, 0, PFS_SUPER_SECTOR, 1, PFS_IO_MODE_WRITE);
					}
				}

				ioctl2(((pfs_mount_t*)arg)->fd, APA_IOCTL2_GET_PART_ERROR, NULL, 0, NULL, 0);
			}
		}
	}

	//0x00002164
	if(fsckRuntimeData.status.fixedErrorCount != 0)
	{
		if(((pfs_mount_t*)arg)->blockDev->transfer(((pfs_mount_t*)arg)->fd, IOBuffer, 0, PFS_SUPER_SECTOR, 1, PFS_IO_MODE_READ) == 0)
		{
			((pfs_super_block*)IOBuffer)->pfsFsckStat |= 2;
			((pfs_mount_t*)arg)->blockDev->transfer(((pfs_mount_t*)arg)->fd, IOBuffer, 0, PFS_SUPER_SECTOR, 1, PFS_IO_MODE_WRITE);
		}
	}

	((pfs_mount_t*)arg)->blockDev->flushCache(((pfs_mount_t*)arg)->fd);

fsck_thread_end:
	SetEventFlag(fsckEventFlagID, 1);
}

//0x0000264c
static int FsckUnsupported(void)
{
	return 0;
}

//0x00000340
static int fsckCheckBitmap(pfs_mount_t *mount, void *buffer)
{
	unsigned int i, count, block, BitmapStart, sector;
	int result;

	result = 0;
	for(i = 0; i < mount->num_subs + 1; i++)
	{
		block = 0;
		for(block = 0,count = pfsGetBitmapSizeBlocks(mount->sector_scale, ZoneSizes[i]); block < count; block++)
		{
			BitmapStart = block + 1;
			if(i == 0)
				BitmapStart += 0x2000 >> mount->sector_scale;

			if((result = mount->blockDev->transfer(mount->fd, buffer, i, BitmapStart << mount->sector_scale, 1 << mount->sector_scale, PFS_IO_MODE_READ)) < 0)
			{
				printf("fsck: cannot read bitmap\n");
				if(fsckPromptUserAction(" Overwrite", 1) == 0)
					return result;

				result = 0;
				for(sector = 0; sector < 1 << mount->sector_scale; sector++)
				{
					//0x0000044c
					if(mount->blockDev->transfer(mount->fd, buffer, i, (BitmapStart << mount->sector_scale) + sector, 1, PFS_IO_MODE_READ) < 0)
					{
						memset(buffer, -1, 512);
						if((result = mount->blockDev->transfer(mount->fd, buffer, i, (BitmapStart << mount->sector_scale) + sector, 1, PFS_IO_MODE_WRITE)) < 0)
						{
							printf("fsck: error: overwrite bitmap failed.\n");
							return result;
						}
					}
				}
			}
		}
	}

	return result;
}

//0x000018b8
static int CheckSuperBlock(pfs_mount_t *pMainPFSMount)
{
	int result, i;
	u32 *pFreeZones;

	pMainPFSMount->num_subs = pMainPFSMount->blockDev->getSubNumber(pMainPFSMount->fd);
	if(pMainPFSMount->blockDev->transfer(pMainPFSMount->fd, IOBuffer, 0, PFS_SUPER_SECTOR, 1, PFS_IO_MODE_READ) < 0 || (((pfs_super_block*)IOBuffer)->magic != PFS_SUPER_MAGIC))
	{
		printf("fsck: Read super block failed, try another.\n");

		if(pMainPFSMount->blockDev->transfer(pMainPFSMount->fd, IOBuffer, 0, PFS_SUPER_BACKUP_SECTOR, 1, PFS_IO_MODE_READ) != 0)
		{
			printf("fsck: error: could not read any super block.\n");
			return -EIO;
		}

		result = (((pfs_super_block*)IOBuffer)->magic == PFS_SUPER_MAGIC) ? 0 : -EIO;

		if(result != 0)
		{
			printf("fsck: error: could not read any super block.\n");
			return -EIO;
		}

		if(fsckPromptUserAction(" Overwrite super block", 1) == 0)
		{
			return -EIO;
		}

		if((result = pMainPFSMount->blockDev->transfer(pMainPFSMount->fd, IOBuffer, 0, PFS_SUPER_SECTOR, 1, PFS_IO_MODE_WRITE)) < 0)
		{
			printf("fsck: error: overwrite failed.\n");
			return result;
		}
	}

	//0x00001930
	if(((pfs_super_block*)IOBuffer)->version >= PFS_VERSION)
	{
		printf("fsck: error: unknown version.\n");
		return -EINVAL;
	}

	if(((((pfs_super_block*)IOBuffer)->zone_size & (((pfs_super_block*)IOBuffer)->zone_size - 1)) != 0) ||
		(((pfs_super_block*)IOBuffer)->zone_size < 0x800) ||
		(0x20000 < ((pfs_super_block*)IOBuffer)->zone_size))
	{
		printf("fsck: error: invalid zone size.\n");
		return -EINVAL;
	}

	if(pMainPFSMount->num_subs < ((pfs_super_block*)IOBuffer)->num_subs)
	{
		printf("fsck: filesystem larger than partition size.\n");

		if(fsckPromptUserAction(" Fix size", 1) == 0)
			return -EINVAL;

		((pfs_super_block*)IOBuffer)->num_subs = pMainPFSMount->num_subs;
		if((result = pMainPFSMount->blockDev->transfer(pMainPFSMount->fd, IOBuffer, 0, PFS_SUPER_SECTOR, 1, PFS_IO_MODE_WRITE)) < 0)
		{
			printf("fsck: error: could not fix the filesystem size.\n");
			return result;
		}
	}

	pMainPFSMount->zsize = ((pfs_super_block*)IOBuffer)->zone_size;
	pMainPFSMount->sector_scale = pfsGetScale(((pfs_super_block*)IOBuffer)->zone_size, 512);
	pMainPFSMount->inode_scale = pfsGetScale(((pfs_super_block*)IOBuffer)->zone_size, 1024);

	memcpy(&pMainPFSMount->root_dir, &((pfs_super_block*)IOBuffer)->root, sizeof(pMainPFSMount->root_dir));
	memcpy(&pMainPFSMount->log, &((pfs_super_block*)IOBuffer)->log, sizeof(pMainPFSMount->log));
	memcpy(&pMainPFSMount->current_dir, &((pfs_super_block*)IOBuffer)->root, sizeof(pMainPFSMount->current_dir));
	pMainPFSMount->total_sector = 0;

	if(fsckVerbosityLevel)
		printf("fsck: \tlog check...\n");

	if((result = pfsJournalRestore(pMainPFSMount)) < 0)
		return result;

	memset(ZoneSizes, 0, 0x10);
	memset(ZoneMap, 0, 0x10);

	for(i = 0; i < pMainPFSMount->num_subs + 1; i++)
	{
		ZoneSizes[i] = pMainPFSMount->blockDev->getSize(pMainPFSMount->fd, i);
		if(i != 0)
			ZoneMap[i] = (ZoneSizes[i - 1] >> pMainPFSMount->sector_scale) + ZoneMap[i - 1];
	}

	if(fsckVerbosityLevel > 0)
		printf("fsck: \tCheck Bitmaps...\n");

	//0x00001c80
	if((result = fsckCheckBitmap(pMainPFSMount, IOBuffer)) >= 0)
	{
		if(fsckVerbosityLevel > 0)
			printf("fsck: \tdone.\n");

		for(i = 0,pFreeZones = pMainPFSMount->free_zone; i < pMainPFSMount->num_subs + 1; i++,pFreeZones++)
		{
			pMainPFSMount->total_sector += ZoneSizes[i] >> pMainPFSMount->sector_scale;
			pMainPFSMount->zfree += (*pFreeZones = pfsBitmapCalcFreeZones(pMainPFSMount, i));
		}

		if(fsckVerbosityLevel > 0)
			printf("fsck: zonesz %ld, %ld zones, %ld free.\n", pMainPFSMount->zsize, pMainPFSMount->total_sector, pMainPFSMount->zfree);
	}

	return result;
}

//0x00002224
static int FsckOpen(iop_file_t *fd, const char *name, int flags, int mode)
{
	int blockfd, result, i;
	unsigned int count;
	iox_stat_t StatData;
	pfs_block_device_t *pblockDevData;

	fsckWriteEnabled = mode & 1;
	fsckAutoMode = mode & 2;
	fsckVerbosityLevel = (mode & 0xF0) >> 4;

	if(MainPFSMount.fd) return -EBUSY;

	if((result = getstat(name, &StatData)) < 0)
	{
		printf("fsck: error: could not get status.\n");
		return result;
	}

	if(StatData.mode != 0x100)	//PFS
	{
		printf("fsck: error: not PFS.\n");
		return -EINVAL;
	}

	if((pblockDevData = pfsGetBlockDeviceTable(name)) == NULL)
		return -ENXIO;

	if((blockfd = open(name, O_RDWR, 0)) < 0)	//FIXME: There is no mode argument, but our definition of open() strictly requires one (unlike the SCE open function).
	{
		printf("fsck: error: cannot open.\n");
		return blockfd;
	}

	memset(&MainPFSMount, 0, sizeof(MainPFSMount));
	MainPFSMount.fd = blockfd;
	MainPFSMount.blockDev = pblockDevData;

	if(fsckVerbosityLevel > 0)
		printf("fsck: Check Super Block...\n");

	if((result = CheckSuperBlock(&MainPFSMount)) < 0)
	{
		MainPFSMount.fd = 0;
		printf("fsck: error: cannot continue.\n");
		return result;
	}

	if(fsckVerbosityLevel > 0)
		printf("fsck: done.\n");

	if((result = pfsBitmapPartInit(MainPFSMount.total_sector)) >= 0)
	{
		//0x000023bc
		memset(&fsckRuntimeData, 0, sizeof(fsckRuntimeData));

		fsckRuntimeData.status.zoneUsed = MainPFSMount.total_sector - MainPFSMount.zfree;
		for(i = 0; i < MainPFSMount.num_subs + 1; fsckRuntimeData.status.inodeBlockCount += count,i++)
		{
			count = pfsGetBitmapSizeBlocks(MainPFSMount.sector_scale, ZoneSizes[i]) + 1;
			if(i == 0)
				count = (0x2000 >> MainPFSMount.sector_scale) + MainPFSMount.log.count + count;

			if(fsckCheckZones(ZoneMap[i], count) < 0)
				break;
		}

		//0x0000246c
		fd->privdata = &MainPFSMount;
		result = 0;
	}else{
		MainPFSMount.fd = 0;
	}

	return result;
}

//0x000024a4
static int FsckClose(iop_file_t *fd)
{
	close(((pfs_mount_t*)fd->privdata)->fd);
	pfsCacheClose((pfs_mount_t*)fd->privdata);
	memset(fd->privdata, 0, sizeof(pfs_mount_t));

	return 0;
}

//0x00001d7c
static int fsckGetEstimatedTime(pfs_mount_t *mount, int *result)
{
	unsigned int i;
	u64 clock;
	iop_sys_clock_t SysClock1, SysClock2, SysClockDiff;
	u32 sec, usec;

	clock = 1000000;
	for(i = 0; i < 4; i++)
	{
		GetSystemTime(&SysClock1);
		mount->blockDev->transfer(mount->fd, IOBuffer, 0, 1 << mount->sector_scale, 1 << mount->sector_scale, PFS_IO_MODE_READ);
		GetSystemTime(&SysClock2);

		SysClockDiff.lo = SysClock2.lo - SysClock1.lo;
		SysClockDiff.hi = SysClock2.hi - SysClock1.hi - (SysClock2.lo < SysClock1.lo);
		SysClock2USec(&SysClockDiff, &sec, &usec);

		printf("fsck: %ld system clocks = %ld.%06ld sec\n", SysClockDiff.lo, sec, usec);

		if(usec < clock)
			clock = usec;
	}

	//0x00001e8c
	if((*result = ((clock + 400) * fsckRuntimeData.status.zoneUsed / 1000000)) == 0)
		*result = 1;

	return 0;
}

//0x000024f0
static int FsckIoctl2(iop_file_t *fd, int cmd, void *arg, unsigned int arglen, void *buf, unsigned int buflen)
{
	int result;
	u32 FlagBits;

	switch(cmd)
	{
		case FSCK_IOCTL2_CMD_GET_ESTIMATE:	//0x00002528
			result = fsckGetEstimatedTime(fd->privdata, buf);
			break;
		case FSCK_IOCTL2_CMD_START:	//0x0000253c
			result = StartThread(fsckThreadID, fd->privdata);
			break;
		case FSCK_IOCTL2_CMD_WAIT:	//0x00002554
			result = WaitEventFlag(fsckEventFlagID, 1, WEF_OR|WEF_CLEAR, &FlagBits);
			break;
		case FSCK_IOCTL2_CMD_POLL:	//0x00002574
			result = PollEventFlag(fsckEventFlagID, 1, WEF_OR|WEF_CLEAR, &FlagBits);
			if(result == 0xFFFFFE5B) result = 1;
			break;
		case FSCK_IOCTL2_CMD_GET_STATUS:	//0x000025a8
			memcpy(buf, &fsckRuntimeData.status, sizeof(struct fsckStatus));
			result = 0;
			break;
		case FSCK_IOCTL2_CMD_STOP:	//0x0000262c
			fsckRuntimeData.stopFlag = 1;
			result = 0;
			break;
		default:
			result = 0;
	}

	return result;
}

static iop_device_ops_t FsckDeviceOps={
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	NULL,
	&FsckOpen,
	&FsckClose,
	NULL,
	NULL,
	NULL,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	(void*)&FsckUnsupported,
	&FsckIoctl2
};

static iop_device_t FsckDevice={
	"fsck",
	IOP_DT_FSEXT|IOP_DT_FS,
	1,
	"FSCK",
	&FsckDeviceOps
};

//0x0000267c
int _start(int argc, char *argv[])
{
	int buffers;

	buffers = 0x7E;
	for(argc--,argv++; argc > 0 && ((*argv)[0] == '-'); argc--,argv++)
	{
		if(!strcmp("-n", *argv))
		{
			argv++;
			if(--argc > 0)
			{
				if(strtol(*argv, NULL, 10) < buffers)
					buffers = strtol(*argv, NULL, 10);
			}
			else return DisplayUsageHelp();
		}
		else return DisplayUsageHelp();
	}

	printf("fsck: max depth %d, %d buffers.\n", FSCK_MAX_PATH_LEVELS - 1, buffers);

	if(pfsCacheInit(buffers, 1024) < 0)
	{
		printf("fsck: error: cache initialization failed.\n");
		return MODULE_NO_RESIDENT_END;
	}

	if(pfsBitmapInit() < 0)
	{
		printf("fsck: error: bitmap initialization failed.\n");
		return MODULE_NO_RESIDENT_END;
	}

	if((fsckEventFlagID = fsckCreateEventFlag()) < 0)
		return MODULE_NO_RESIDENT_END;

	if((fsckThreadID = fsckCreateThread(&FsckThread, 0x2080)) < 0)
		return MODULE_NO_RESIDENT_END;

	DelDrv("fsck");
	if(AddDrv(&FsckDevice) == 0)
	{
		printf("fsck: version %04x driver start.\n", _irx_id.v);
		return MODULE_RESIDENT_END;
	}

	return MODULE_NO_RESIDENT_END;
}
