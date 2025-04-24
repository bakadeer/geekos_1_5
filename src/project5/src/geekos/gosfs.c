/*
 * GeekOS file system
 * Copyright (c) 2004, David H. Hovemeyer <daveho@cs.umd.edu>
 * $Revision: 1.54 $
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <limits.h>
#include <geekos/errno.h>
#include <geekos/kassert.h>
#include <geekos/screen.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/bitset.h>
#include <geekos/synch.h>
#include <geekos/bufcache.h>
#include <geekos/gosfs.h>

struct Superblock {
    int magic;
    ulong_t numBlocks;
    ulong_t blockMapOffset;
    ulong_t inodeTableOffset;
    ulong_t dataBlockOffset;
}

struct GOSFS_File {

}

struct GOSFS {
    struct Superblock superblock;
    struct FS_Buffer_Cache *cache;
    struct Mutex lock;
}

#define GOSFS_MAGIC 0x0d000721
#define GOSFS_SUPERBLOCK_OFFSEET 0
#define GOSFS_BLOCK_MAP_OFFSET 1

#define GOSFS_NUM_INODE_BLOCKS 4

#define GOSFS_ROOTDIR_INODE_PTR 1

// 每个FS块包含的位数
#define GOSFS_NUM_BITS_PER_FS_BLOCK (GOSFS_FS_BLOCK_SIZE * 8)

/* ----------------------------------------------------------------------
 * Private data and functions
 * ---------------------------------------------------------------------- */


/* ----------------------------------------------------------------------
 * Implementation of VFS operations
 * ---------------------------------------------------------------------- */

/*
 * Get metadata for given file.
 * 获得文件元数据
 */
static int GOSFS_FStat(struct File *file, struct VFS_File_Stat *stat)
{
    TODO("GeekOS filesystem FStat operation");
}

/*
 * Read data from current position in file.
 * 读取数据
 */
static int GOSFS_Read(struct File *file, void *buf, ulong_t numBytes)
{
    TODO("GeekOS filesystem read operation");
}

/*
 * Write data to current position in file.
 * 写入数据
 */
static int GOSFS_Write(struct File *file, void *buf, ulong_t numBytes)
{
    TODO("GeekOS filesystem write operation");
}

/*
 * Seek to a position in file.
 * 找到文件位置
 */
static int GOSFS_Seek(struct File *file, ulong_t pos)
{
    TODO("GeekOS filesystem seek operation");
}

/*
 * Close a file.
 * 关闭文件
 */
static int GOSFS_Close(struct File *file)
{
    TODO("GeekOS filesystem close operation");
}

/*static*/ struct File_Ops s_gosfsFileOps = {
    &GOSFS_FStat,
    &GOSFS_Read,
    &GOSFS_Write,
    &GOSFS_Seek,
    &GOSFS_Close,
    0, /* Read_Entry */
};

/*
 * Stat operation for an already open directory.
 * 为一个已经打开的目录获取元数据
 */
static int GOSFS_FStat_Directory(struct File *dir, struct VFS_File_Stat *stat)
{
    TODO("GeekOS filesystem FStat directory operation");
}

/*
 * Directory Close operation.
 * 关闭目录
 */
static int GOSFS_Close_Directory(struct File *dir)
{
    TODO("GeekOS filesystem Close directory operation");
}

/*
 * Read a directory entry from an open directory.
 * 读取目录项
 */
static int GOSFS_Read_Entry(struct File *dir, struct VFS_Dir_Entry *entry)
{
    TODO("GeekOS filesystem Read_Entry operation");
}

/*static*/ struct File_Ops s_gosfsDirOps = {
    &GOSFS_FStat_Directory,
    0, /* Read */
    0, /* Write */
    0, /* Seek */
    &GOSFS_Close_Directory,
    &GOSFS_Read_Entry,
};

/*
 * Open a file named by given path.
 * 用给定的路径打开一个文件
 */
static int GOSFS_Open(struct Mount_Point *mountPoint, const char *path, int mode, struct File **pFile)
{
    TODO("GeekOS filesystem open operation");
}

/*
 * Create a directory named by given path.
 * 用户给定的路径创建一个目录
 */
static int GOSFS_Create_Directory(struct Mount_Point *mountPoint, const char *path)
{
    TODO("GeekOS filesystem create directory operation");
}

/*
 * Open a directory named by given path.
 * 用给定的路径打开一个目录
 */
static int GOSFS_Open_Directory(struct Mount_Point *mountPoint, const char *path, struct File **pDir)
{
    TODO("GeekOS filesystem open directory operation");
}

/*
 * Delete a directory or file named by given path.
 * 用给定的路径删除一个目录或文件
 */
static int GOSFS_Delete(struct Mount_Point *mountPoint, const char *path)
{
    TODO("GeekOS filesystem delete operation");
}

/*
 * Get metadata (size, permissions, etc.) of file named by given path.
 * 获取文件元数据（大小，权限等）
 */
static int GOSFS_Stat(struct Mount_Point *mountPoint, const char *path, struct VFS_File_Stat *stat)
{
    TODO("GeekOS filesystem stat operation");
}

/*
 * Synchronize the filesystem data with the disk
 * (i.e., flush out all buffered filesystem data).
 * 同步文件系统数据到磁盘（即刷新缓冲区中的所有文件系统数据）
 */
static int GOSFS_Sync(struct Mount_Point *mountPoint)
{
    TODO("GeekOS filesystem sync operation");
}

/*static*/ struct Mount_Point_Ops s_gosfsMountPointOps = {
    &GOSFS_Open,
    &GOSFS_Create_Directory,
    &GOSFS_Open_Directory,
    &GOSFS_Stat,
    &GOSFS_Sync,
    &GOSFS_Delete,
};

static int GOSFS_Format(struct Block_Device *blockDev)
{
    // TODO("GeekOS filesystem format operation");

    int rc = 0;
    int numBlocks, numMapBlocks, numInodeTableBlocks;
    
    struct FS_Buffer* buf = 0;
    struct Superblock superblock;
    struct pSuperblock* pSuperblock;

    // 总块数=Dev总扇区/一个文件系统块扇区数
    numBlocks = Get_Num_Blocks(blockDev) / GOSFS_SECTORS_PER_FS_BLOCK;
    // 位图所需块=总块数/一个位图块所占的块数+1
    numMapBlocks = numBlocks / GOSFS_NUM_BITS_PER_FS_BLOCK + 1;
    // Inode所需块=4
    numInodeTableBlocks = GOSFS_NUM_INODE_BLOCKS;

    superblock.magic = GOSFS_MAGIC;
    superblock.numBlocks = numBlocks;
    superblock.blockMapOffset = GOSFS_BLOCK_MAP_OFFSET;
    superblock.inodeTableOffset = GOSFS_BLOCK_MAP_OFFSET + numMapBlocks;
    superblock.dataBlockOffset = 
        GOSFS_BLOCK_MAP_OFFSET + numMapBlocks + numInodeTableBlocks;

    cache = Create_FS_Buffer_Cache(blockDev, GOSFS_FS_BLOCK_SIZE);
    if (cache == 0) {
        rc = ENOMEN;
        goto fail;
    }

    // Superblock

    rc = Get_FS_Buffer(cache, GOSFS_SUPERBLOCK_OFFSEET, &buf);
    if (rc != 0) goto fail;
    pSuperblock = (struct Superblock*) buf->data;
    *pSuperblock = superblock;
    Modify_FS_Buffer(cache, buf);
    Release_FS_Buffer(cache, buf);

    // Bitmap block
    for (int i = 0; i < numMapBlocks; i++){
        rc = Get_FS_Buffer(cache, GOSFS_BLOCK_MAP_OFFSET + i, &buf);
        if (rc != 0) goto fail;

        // clear block
        memset(buf->data, 0, GOSFS_FS_BLOCK_SIZE);
        Modify_FS_Buffer(cache, buf);

        if (i == 0){
            for (int j = 0; j <superblock.dataBlockOffset; j++){
                Set_Bit(buf->data, j);
            }
        }


    }

    // Inode table
    for (int i = 0; i < numInodeTableBlocks; i++){
        rc = Get_FS_Buffer(cache, superblock.inodeTableOffset + i, &buf);
        if (rc != 0) goto fail;

        // clear block
        memset(buf->data, 0, GOSFS_FS_BLOCK_SIZE);
        Modify_FS_Buffer(cache, buf);

        if (i == 0){
            struct GOSFS_Dir_Entry* rootDirInode = 
                (struct GOSFS_Dir_Entry*) buf->data + GOSFS_ROOTDIR_INODE_PTR;
            strcpy(rootDirInode->filename, "/");
            rootDirInode->flags = 
                GOSFS_DIRENTRY_USED | GOSFS_DIRENTRY_ISDIRECTORY;
        }
        Release_FS_Buffer(cache, buf);        
    }

fail:
    if (Buf_In_Use(buf)) Release_FS_Buffer(cache, buf);
    if (cache != 0) Destroy_FS_Buffer_Cache(cache);
    return rc;
}

static int GOSFS_Mount(struct Mount_Point *mountPoint)
{
    // TODO("GeekOS filesystem mount operation");

    
}

static struct Filesystem_Ops s_gosfsFilesystemOps = {
    &GOSFS_Format,
    &GOSFS_Mount,
};

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

void Init_GOSFS(void)
{
    Register_Filesystem("gosfs", &s_gosfsFilesystemOps);
}

