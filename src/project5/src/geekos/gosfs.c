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
    ulong_t blockBitMapOffset;
    ulong_t inodeTableOffset;
    ulong_t dataBlockOffset;
}

struct GOSFS_File {
    struct GOSFS_Dir_Entry dirEntry;
    
    char path[VFS_MAX_PATH_LEN + 1];
    struct Mutex lock;
    ulong_t inodePtr;
    DEFINE_LINK(GOSFS_File_List, GOSFS_File);
};
DEFINE_LIST(GOSFS_File_List, GOSFS_File);
IMPLEMENT_LIST(GOSFS_File_List, GOSFS_File);

struct GOSFS {
    struct Superblock superblock;

    struct FS_Buffer_Cache *cache;
    struct GOSFS_File_List fileOpened;
    struct Mutex lock;
}

#define GOSFS_MAGIC 0x0d000721
#define GOSFS_SUPERBLOCK_OFFSEET 0
#define GOSFS_BLOCK_BITMAP_OFFSET 1

#define GOSFS_NUM_INODE_BLOCKS 4

// 0x00
#define GOSFS_ABSENT_PTR 0
// 根目录的inode指针
#define GOSFS_ROOTDIR_INODE_PTR 1

// 每个FS块包含的位数
#define GOSFS_NUM_BITS_PER_FS_BLOCK (GOSFS_FS_BLOCK_SIZE * 8)

/* ----------------------------------------------------------------------
 * Private data and functions
 * ---------------------------------------------------------------------- */

 static void ACL_Copy(
    struct VFS_ACL_Entry* dst,
    struct VFS_ACL_Entry* src
)
{
    memcpy(dst, src, VFS_MAX_ACL_ENTRIES * sizeof(struct VFS_ACL_Entry));
}

static int Insert_Entr(
    ulong_t selfPter,
    ulong_t targetPter,
    struct GOSFS* fs
)
{
    int rc = 0;
    GET_INDEX_AND_OFFSET(ulong_t, selfPter, GOSFS_DIR_ENTRIES_PER_BLOCK);
    struct FS_Buffer *buf = 0;
    struct GOSFS_Dir_Entry* self = 0;

    rc = Get_FS_Buffer(
        fs->cache, 
        fs->superblock.inodeTableOffset + selfPtrIndex,
        &buf
    );
    if (rc != 0) return rc;
    self = (struct GOSFS_Dir_Entry*) buf->data + selfPtrIndex;

    for (int i = 0; i < GOSFS_NUM_DIRECT_BLOCKS; i++) {
        ulong_t *pThisPtr = &self->blockList[i];
        bool ptr
    }
}

static int Delete_Entry(
    ulong_t selfPter,
    ulong_t targetPter,
    struct GOSFS* fs
)
{
    int rc = 0;
    struct FS_Buffer* buf = 0;
    GET_INDEX_AND_OFFSET(ulong_t, selfPter, GOSFS_DIR_ENTRIES_PER_BLOCK);
    struct GOSFS_Dir_Entry* self = 0;

    rc = Get_FS_Buffer(
        fs->cache, 
        fs->superblock.inodeTableOffset + selfPtrIndex,
        &buf
    );
    if (rc != 0) return rc;
    self = (struct GOSFS_Dir_Entry*) buf->data + selfPtrIndex;

    // Direct blocks
    for (int i = 0; i < GOSFS_NUM_DIRECT_BLOCKS; i++) {
        ulong_t *pThisPtr = &self->blockList[i];

        if (*pThisPtr == GOSFS_ABSENT_PTR) continue;

        rc = Delete_Ptr(pThisPtr, tatgetPtr, fs);
        if (rc == NOT_FOUND) continue;
        if (rc != 0) return fail;

        if (*pThisPtr == GOSFS_ABSENT_PTR) Modify_FS_Buffer(fs->cache, buf);
        Release_FS_Buffer(fs->cache, buf);
        return 0;
    }

    // Indirect blocks
    for (int i = 0; i < GOSFS_NUM_INDIRECT_BLOCKS; i++) {
        ulong_t* pThisPtr = &self->blockList[GOSFS_NUM_DIRECT_BLOCKS + i];

        if (*pThisPtr == GOSFS_ABSENT_PTR) continue;

        rc = Delete_Ptr_Rec(pThisPtr, targetPter, 1, fs);
        if (rc == NOT_FOUND) continue;
        if (rc != 0) goto fail;

        if (*pThisPtr == GOSFS_ABSENT_PTR) Modify_FS_Buffer(fs->cache, buf);
        Release_FS_Buffer(fs->cache, buf);
        return 0;
    }

    for (int i = 0; i < GOSFS_NUM_2X_INDIRECT_BLOCKS; i++) {
        ulong_t* pThisPtr = &self->blockList[
            GOSFS_NUM_DIRECT_BLOCKS + GOSFS_NUM_INDIRECT_BLOCKS + i
        ];

        if (*pThisPtr == GOSFS_ABSENT_PTR) continue;
        
        rc = Delete_Ptr_Rec(pThisPtr, targetPter, 2, fs);
        if (rc == NOT_FOUND) continue;
        if (rc != 0) goto fail;

        if (*pThisPtr == GOSFS_ABSENT_PTR) Modify_FS_Buffer(fs->cache, buf);
        Release_FS_Buffer(fs->cache, buf);
        return 0;
    }

    Release_FS_Buffer(fs->cache, buf);
    return NOT_FOUND;

fail:
    Release_FS_Buffer(fs->cache, buf);
    return rc;
}

/* ----------------------------------------------------------------------
 * Implementation of VFS operations
 * ---------------------------------------------------------------------- */

/*
 * Get metadata for given file.
 * 获得文件元数据
 */
static int GOSFS_FStat(struct File *file, struct VFS_File_Stat *stat)
{
    // TODO("GeekOS filesystem FStat operation");
    int rc = 0;
    struct GOSFS_File* fileInstance = file->fsData;

    Mutex_lock(&fileInstance->lock);
    rc = Stat_File(file, stat);
    Mutex_Unlock(&fileInstance->lock);

    return rc;
}

/*
 * Read data from current position in file.
 * 读取数据
 */
static int GOSFS_Read(struct File *file, void *buf, ulong_t numBytes)
{
    // TODO("GeekOS filesystem read operation");
    int rc = 0;
    struct GOSFS* fs = file->mountPoint->fsData;
    struct GOSFS_File* fileInstance = file->fsData;
    ulong_t blockPtr;
    struct FS_Buffer* blockBuf = 0;
    ulong_t numBytesLeft;

    Mutex_Lock(&fileInstance->lock);

    if (!(file->mode & O_READ)) {
        Mutex_Unlock(&fileInstance->lock);
        return EACCESS;
    }

    if (file->filePos + numBytes > file->endPos) {
        numBytes = file->endPos - file->filePos;
    }

    numBytesLeft = numBytes;
    while (numBytesLeft != 0) {
        ulong_t posOffset = file->filePos % GOSFS_FS_BLOCK_SIZE;
        ulong_t numBytesThisSeg = MIN(
            GOSFS_FS_BLOCK_SIZE,
            posOffset + numBytesLeft
        ) - posOffset;

        rc = Get_Or_Insert_Block_Of_Current_Byte(
            fileInstance->blockList,
            file->filePos,
            &blockPtr,
            fs
        );
        if (rc != 0) goto fail;

        rc = Get_FS_Buffer(fs->cache, blockPtr, &blockBuf);
        if (rc != 0) goto fail;

        memcpy(
            (char*) buf + (numBytes - numBytesLeft),
            (char*) blockBuf->data + file->filePos % GOSFS_FS_BLOCK_SIZE,
            numBytesThisSeg
        );
        Release_FS_Buffer(fs->cache, blockBuf);
        file->filePos += numBytesThisSeg;
        numBytesLeft -= numBytesThisSeg;
    }

    Mutex_Unlock(&fileInstance->lock);
    return numBytes;

fail:
    Mutex_Unlock(&fileInstance->lock);
    return rc;
}

/*
 * Write data to current position in file.
 * 写入数据
 */
static int GOSFS_Write(struct File *file, void *buf, ulong_t numBytes)
{
    int rc = 0;
    struct GOSFS* fs = file->mountPoint->fsData;
    struct GOSFS_File* fileInstance = file->fsData;
    ulong_t blockPtr, inodePtr = fileInstance->inodePtr, 
        fileSize = fileInstance->dirEntry.size;
    struct FS_Buffer* blockBuf = 0;
    ulong_t numBytesLeft = numBytes;
    struct GOSFS_Dir_Entry* self = 0;

    Mutex_lock(&fileInstance->lock);

    // 检查权限
    if (!(file->mode & O_WRITE)) {
        Mutex_Unlock(&fileInstance->lock);
        return EACCESS;
    }

    // 循环写入数据
    while (numBytesLeft != 0) {
        ulong_t posOffset = file->filePos % GOSFS_FS_BLOCK_SIZE;// 当前文件位置的偏移
        ulong_t numBytesThisSeg = MIN(
            GOSFS_FS_BLOCK_SIZE,
            posOffset + numBytesLeft
        ) - posOffset;// 当前段要写入的字节数

        rc = Get_Or_Insert_Block_Of_Current_Byte(
            fileInstance->dirEntry.blockList,
            file->filePos,
            &blockPtr,
            fs
        );// 获取当前字节所在的块指针，如果块不存在则分配一个新块
        if (rc != 0) goto fail;

        rc = Get_FS_Buffer(fs->cache, blockPtr, &blockBuf);
        if (rc != 0) goto fail;

        memcpy(
            (char*) blockBuf->data + file->filePos % GOSFS_FS_BLOCK_SIZE,
            (char*) buf + (numBytes - numBytesLeft),
            numBytesThisSeg
        );

        Modify_FS_Buffer(fs->cache, blockBuf);
        Release_FS_Buffer(fs->cache, blockBuf);

        // 更新文件位置和剩余字节数
        file->filePos += numBytesThisSeg;
        file->endPos += numBytesThisSeg;
        numBytesLeft -= numBytesThisSeg;
    }

    // 同步inode信息
    if (fileSize < file->endPos) fileSize = file->endPos;
    GET_INDEX_AND_OFFSET(ulong_t, inodePtr, GOSFS_DIR_ENTRIES_PER_BLOCK);
    rc = Get_FS_Buffer(
        fs->cache,
        fs->superblock.inodeTableOffset + inodePtrIndex, 
        &blockBuf
    );
    if (rc != 0) goto fail;
    self = (struct GOSFS_Dir_Entry*) blockBuf->data + inodePtrOffset;
    self->size = fileSize;
    memcpy(
        self->blockList,
        fileInstance->blockList,
        GOSFS_NUM_BLOCK_PTRS * sizeof(ulong_t)
    );
    Modify_FS_Buffer(fs->cache, blockBuf);
    Release_FS_Buffer(fs->cache, blockBuf);

    Mutex_Unlock(&fileInstance->lock);

    return numBytes;

fail:
    Mutex_Unlock(&fileInstance->lock);
    return rc;

}

/*
 * Seek to a position in file.
 * 找到文件位置
 */
static int GOSFS_Seek(struct File *file, ulong_t pos)
{
    // TODO("GeekOS filesystem seek operation");
    struct GOSFS_File* fileInstance = file->fsData;

    if (
        (fileInstance->flags & GOSFS_DIRENTRY_ISDIRECTORY || 
            (file->mode & O_READ)) &&
        pos > file->endPos
    )
        return EINVALID;
    
    Mutex_lock(&fileInstance->lock);
    file->filePos = pos;
    if (pos > file->endPos) file->endPos = pos;
    Mutex_Unlock(&fileInstance->lock);

    return 0;
}

/*
 * Close a file.
 * 关闭文件
 */
static int GOSFS_Close(struct File *file)
{
    // TODO("GeekOS filesystem close operation");
    int rc = 0;
    struct GOSFS_File* fileInstance = file->fsData;

    Mutex_lock(&fileInstance->lock);
    rc = Close_File(file);
    Mutex_Unlock(&fileInstance->lock);

    return rc;
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
    // TODO("GeekOS filesystem FStat directory operation");
    int rc = 0;
    struct GOSFS_File* fileInstance = dir->fsData;

    Mutex_lock(&fileInstance->lock);
    rc = Stat_File(dir, stat);
    Mutex_Unlock(&fileInstance->lock);

    return rc;
}

/*
 * Directory Close operation.
 * 关闭目录
 */
static int GOSFS_Close_Directory(struct File *dir)
{
    // TODO("GeekOS filesystem Close directory operation");
    int rc = 0;
    struct GOSFS_File* file = dir->fsData;

    Mutex_lock(&fileInstance->lock);
    rc = Close_File(dir);
    Mutex_Unlock(&fileInstance->lock);

    return rc;
}

/*
 * Read a directory entry from an open directory.
 * 读取目录项
 */
static int GOSFS_Read_Entry(struct File *dir, struct VFS_Dir_Entry *entry)
{
    // TODO("GeekOS filesystem Read_Entry operation");
    int rc = 0;
    struct GOSFS_File* fileInstance = dir->fsData;

    Mutex_lock(&fileInstance->lock);
    do {
        rc = Get_Next_Entry(dir, entry);
    } while (rc == NOT_FOUND);
    Mutex_Unlock(&fileInstance->lock);

    return rc;
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
    // TODO("GeekOS filesystem open operation");
    int rc = 0;
    struct GOSFS *fs = mountPoint->fsData;

    Mutex_Lock(&fs->lock);
    rc = Open_File(mountPoint, path, &s_gosfsFileOps, mode, pFile);
    Mutex_Unlock(&fs->lock);

    return rc;
}

/*
 * Create a directory named by given path.
 * 用户给定的路径创建一个目录
 */
static int GOSFS_Create_Directory(struct Mount_Point *mountPoint, const char *path)
{
    // TODO("GeekOS filesystem create directory operation");

    int rc = 0;
    ulong_t phantomPtr;
    struct GOSFS* fs = mountPoint->fsData;

    Mutex_Lock(&fs->lock);
    rc = Create_File(mountPoint->fsData, path, true, &phantomPtr);
    Mutex_Unlock(&fs->lock);

    return rc;
}

/*
 * Open a directory named by given path.
 * 用给定的路径打开一个目录
 */
static int GOSFS_Open_Directory(struct Mount_Point *mountPoint, const char *path, struct File **pDir)
{
    // TODO("GeekOS filesystem open directory operation");

    int rc = 0;
    struct GOSFS* fs = mountPoint->fsData;

    Mutex_Lock(&fs->lock);
    rc = Open_File(mountPoint, path, &s_gosfsDorOps, pDir);
    Mutex_Unlock(&fs->lock);

    return rc;
}

/*
 * Delete a directory or file named by given path.
 * 用给定的路径删除一个目录或文件
 */
static int GOSFS_Delete(struct Mount_Point *mountPoint, const char *path)
{
    // TODO("GeekOS filesystem delete operation");

    int rc = 0;
    struct GOSFS *fs = mountPoint->fsData;
    ulong_t inodePtr, dirInodePtr;
    struct GOSFS_Dir_Entry dirInode;
    char *dirPath = 0;

    Mutex_Lock(&fs->lock);

    if ( File_Is_Open(fs, path) != 0) return EACCESS;

    rc = Find_File(fs, path, &inodePtr, 0);
    if (rc != 0) goto fail;

    dirPath = Get_Parent_Dir_Path(path);
    if (dirPath == 0) {
        Mutex_Unlock(&fs->lock);
        return ENOMEM;
    }
    rc = Find_File(fs, dirPath, &dirInodePtr, &dirInode);
    Free(dirPath);
    if (rc != 0) goto fail;

    rc = Delete_Entry(dirInodePtr, inodePtr, fs);
    if (rc != 0) goto fail;
    while (Update_Inode_Cache(fs, dirPath, dirInodePtr) != 0);

    rc = Dealloc_Inode(&fs->lock);

    return rc;

fail:
    Mutex_Unlock(&fs->lock);
    return rc;

}

/*
 * Get metadata (size, permissions, etc.) of file named by given path.
 * 获取文件元数据（大小，权限等）
 */
static int GOSFS_Stat(
    struct Mount_Point *mountPoint, 
    const char *path, 
    struct VFS_File_Stat *stat
)
{
    // TODO("GeekOS filesystem stat operation");
    int rc = 0;
    struct GOSFS *fs_instance = mountPoint->fsData;
    ulong_t inodePtr;
    struct GOSFS_Dir_Entry inode;

    Mutex_Lock(&fs_instance->lock);
    rc = Find_File(fs_instance, path, &inodePtr, &inode);

    if (rc == 0) {
        Mutex_Unlock(&fs_instance->lock);
        return rc;
    }

    Mutex_Unlock(&fs_instance->lock);

    stat->size = inode.size;
    stat->isDirectory = (inode.flags & GOSFS_DIRENTRY_ISDIRECTORY) != 0;
    stat->isSetuid = (inode.flags & GOSFS_DIRENTRY_ISSETUID) != 0;

    ACL_Copy(stat->acls, inode.acls);

    return 0;
}

/*
 * Synchronize the filesystem data with the disk
 * (i.e., flush out all buffered filesystem data).
 * 同步文件系统数据到磁盘（即刷新缓冲区中的所有文件系统数据）
 */
static int GOSFS_Sync(struct Mount_Point *mountPoint)
{
    // TODO("GeekOS filesystem sync operation");
    int rc = 0;
    struct GOSFS *fs = mountPoint->fsData;

    Mutex_Lock(&fs->lock);
    rc = Sync_FS_Buffer_Cache(fs->cache);
    Mutex_Unlock(&fs->lock);

    return rc;
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
    int numBlocks, numBitMapBlocks, numInodeTableBlocks;
    
    struct FS_Buffer* pbuf = 0;
    struct Superblock superblock;
    struct pSuperblock* pSuperblock;

    // 总块数=Dev总扇区/一个文件系统块扇区数
    numBlocks = Get_Num_Blocks(blockDev) / GOSFS_SECTORS_PER_FS_BLOCK;
    // 位图所需块=总块数/一个位图块所占的块数+1
    numBitMapBlocks = numBlocks / GOSFS_NUM_BITS_PER_FS_BLOCK + 1;

    superblock.magic = GOSFS_MAGIC;
    superblock.numBlocks = numBlocks;
    superblock.blockBitMapOffset = GOSFS_BLOCK_BITMAP_OFFSET;
    superblock.inodeTableOffset = superblock.blockBitMapOffset + numBitMapBlocks;
    superblock.dataBlockOffset = 
        superblock.inodeTableOffset + GOSFS_NUM_INODE_BLOCKS;

    cache = Create_FS_Buffer_Cache(blockDev, GOSFS_FS_BLOCK_SIZE);
    if (cache == 0) {
        rc = ENOMEN;
        goto fail;
    }

    // Superblock, offset 0
    rc = Get_FS_Buffer(cache, GOSFS_SUPERBLOCK_OFFSEET, &pbuf);
    if (rc != 0) goto fail;
    pSuperblock = (struct Superblock*) pbuf->data;
    *pSuperblock = superblock;
    Modify_FS_Buffer(cache, pbuf);
    Release_FS_Buffer(cache, pbuf);

    // Bitmap block
    for (int i = 0; i < numBitMapBlocks; i++){
        rc = Get_FS_Buffer(cache, GOSFS_BLOCK_BITMAP_OFFSET + i, &pbuf);
        if (rc != 0) goto fail;

        // clear block
        memset(pbuf->data, 0, GOSFS_FS_BLOCK_SIZE);
        Modify_FS_Buffer(cache, pbuf);

        if (i == 0){
            for (int j = 0; j <superblock.dataBlockOffset; j++){
                Set_Bit(pbuf->data, j);// j位设为1
            }
        }

        Release_FS_Buffer(cache, pbuf);

    }

    // Inode table
    for (int i = 0; i < numInodeTableBlocks; i++){
        rc = Get_FS_Buffer(cache, superblock.inodeTableOffset + i, &pbuf);
        if (rc != 0) goto fail;

        // clear block
        memset(pbuf->data, 0, GOSFS_FS_BLOCK_SIZE);
        Modify_FS_Buffer(cache, pbuf);

        if (i == 0){
            struct GOSFS_Dir_Entry* rootDirInode = 
                (struct GOSFS_Dir_Entry*) pbuf->data + GOSFS_ROOTDIR_INODE_PTR;
            strcpy(rootDirInode->filename, "/");
            rootDirInode->flags = 
                GOSFS_DIRENTRY_USED | GOSFS_DIRENTRY_ISDIRECTORY;
        }
        Release_FS_Buffer(cache, pbuf);        
    }

fail:
    if (Buf_In_Use(pbuf)) Release_FS_Buffer(cache, pbuf);
    if (cache != 0) Destroy_FS_Buffer_Cache(cache);
    return rc;
}

static int GOSFS_Mount(struct Mount_Point *mountPoint)
{
    // TODO("GeekOS filesystem mount operation");

    int rc = 0;
    struct FS_Buffer* buf = 0;
    struct Superblock* superblock;
    struct GOSFS* fs_instance;


    cache = Create_FS_Buffer_Cache(mountPoint->dev, GOSFS_FS_BLOCK_SIZE);
    if (cache == 0) return ENOMEM;

    rc = Get_FS_Buffer(cache, GOSFS_SUPERBLOCK_OFFSEET, &buf);
    if (rc != 0) goto fail;
    superblock = (struct Superblock*) buf->data;

    if (superblock->magic != GOSFS_MAGIC){
        rc = EINVALIDFS;
        goto fail;
    }

    mountPoint->fsData = Malloc(sizeof(struct GOSFS));
    if (mountPoint->fsData == 0) return ENOMEM;

    fs_instance = mountPoint->fsData;
    mountPoint->ops = &s_gosfsMountPointOps;

    fs_instance->superblock = *superblock;
    fs_instance->cache = cache;
    Mutex_Init(&fs->lock);
    Clear_GOSFS_File_List(&fs->filesOpend);

    Release_FS_Buffer(cache, buf);
    return 0;

fail:
    if (mountPoint->ops != 0) mountPoint->ops = 0;
    if (fs != 0){
        Free(fs);
        mountPoint->fsData = 0;
    }
    if (Buf_In_Use(buf)) Release_FS_Buffer(cache, buf);
    if (cache != 0) Destroy_FS_Buffer_Cache(cache);

    return rc;

    
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

