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
#include <geekos/int.h>
#include <geekos/bufcache.h>
#include <geekos/gosfs.h>
#include <geekos/user.h>
#include <libc/sched.h>

int debugGOSFS = 0;
#define Debug(args...) if (debugGOSFS) Print("GOSFS:"args)

#define GOSFS_MAGIC                 0x0d000721

#define GOSFS_NUM_INODES            1024

#define GOSFS_NUM_INDIRECT_PTR_PER_BLOCK    (GOSFS_FS_BLOCK_SIZE / sizeof(ulong_t))

#define GOSFS_DIRTYP_THIS       1
#define GOSFS_DIRTYP_REGULAR    0
#define GOSFS_DIRTYP_FREE       -1

struct GOSFS_Superblock {
    ulong_t magic;
    ulong_t supersize;                                  /* size of superblock in bytes */
    ulong_t size;                                       /* number of blocks of whole fs*/
    struct GOSFS_Dir_Entry inodes[GOSFS_NUM_INODES];    /* array of inodes of this fs*/
    uchar_t bitSet[];                                   /* used/unused blocks */
};

struct GOSFS_Directory 
{
    char filename[GOSFS_FILENAME_MAX+1];    // filename
    ulong_t type;                           // type of entry
    ulong_t inode;                          // referenced inode number
};

/* on mount we create a GOSFS_Instance to work on */
struct GOSFS_Instance {
    struct Mutex lock;                    /* mutext to lock whole fs */
    struct FS_Buffer_Cache* buffercache;  /* buffer cache to work on */
    struct GOSFS_Superblock superblock;   /* superblock must be at the end of struct */
};

/* arbitrary data for file-descriptor */
struct GOSFS_File_Entry {
    struct GOSFS_Dir_Entry* inode;
    struct GOSFS_Instance* instance;
};

/**
 * 提取路径中的下一个组件名称（目录或文件名）
 *
 * @param path 原始路径字符串
 * @param prevPos 上次处理结束的位置，初始为 NULL
 * @param buf 输出缓冲区，用于存储提取出的组件名
 * @param bufSize 缓冲区大小
 * @return 下一个组件的起始位置，NULL 表示没有更多组件
 */
 char* NextPathComponent(const char* path, const char* prevPos, char* buf, int bufSize) {
    if (path == NULL || *path != '/') {
        Debug("NextPathComponent: path is NULL or not start with '/'\n");
        return NULL; // 路径非法
    }
    const char* pathEnd = path + strlen(path); // 记录路径末尾位置
    const char* start;
    if (prevPos == NULL) {
        start = path + 1; // 第一次调用，跳过开头的 '/'
    } else {
        start = prevPos + 1; // 下一次调用，从上次结束的位置后开始
    }

    if (start > pathEnd) {
        Debug("NextPathComponent: start exceeds pathEnd\n");
        return NULL; // 越界访问，非法路径
    }

    if (*start == '\0') {
        return NULL; // 没有更多组件
    }

    const char* end = strchr(start, '/');
    if (end == NULL) {
        end = start + strlen(start); // 最后一个组件
    }

    int len = end - start;
    if (len >= bufSize) {
        len = bufSize - 1; // 防止溢出
    }

    strncpy(buf, start, len);
    buf[len] = '\0'; // 确保字符串结尾

    // Debug("NextPathComponent: start='%d', end='%d'\n", (int)start, (int)end);
    Debug("NextPathComponent: extracted component='%s'\n", buf);

    return (char*)end; // 返回下一个 '/' 的位置
}

typedef struct PathIterator {
    const char* path;
    const char* current;
} PathIterator;

void InitPathIterator(PathIterator* iter, const char* path) {
    size_t len = strlen(path);
    char* safePath = Malloc(len + 1);
    if (safePath == NULL) {
        Print("InitPathIterator: Out of memory\n");
        return;
    }

    strcpy(safePath, path);  // 安全复制，包含结尾的 \0

    iter->path = safePath;
    iter->current = NULL;
    Debug("InitPathIterator: Initialized iterator with path: %s\n", safePath);
}

bool GetNextPathComponent(PathIterator* iter, char* buf, int bufSize) {
    iter->current = NextPathComponent(iter->path, iter->current, buf, bufSize);
    return (iter->current != NULL);
}

/**
 * 提取给定路径的父路径
 *
 * @param path      输入路径，必须以 '/' 开头
 * @param parentBuf 输出缓冲区，用于存储父路径
 * @param bufSize   缓冲区大小
 *
 * @return 成功返回 0；失败或路径无效返回 -1
 */
 int GetParentPath(const char* path, char* parentBuf, int bufSize) {
    const char* lastSlash;
    Debug("GetParentPath: path: %s\n", path);

    if (path == NULL || parentBuf == NULL || bufSize <= 0) {
        Debug("GetParentPath: Invalid arguments\n");
        return -1;
    }

    if (path[0] != '/') {
        Debug("GetParentPath: Invalid path\n");
        return -1; // 路径必须以 '/' 开头
    }

    lastSlash = strrchr(path, '/');
    if (lastSlash == NULL) {
        Debug("GetParentPath: Invalid path\n");
        return -1; // 不可能是合法路径
    }

    // 如果路径是根目录 "/" 或者类似 "/a" 的形式
    if (lastSlash == path) {
        // 特殊处理：父路径为根目录 "/"
        if (bufSize < 2) {
            Debug("GetParentPath: Buffer too small for root path\n");
            return -1;
        }
        parentBuf[0] = '/';
        parentBuf[1] = '\0';
        Debug("GetParentPath: parent path: %s\n", parentBuf);
        return 0;
    }

    // 提取父路径部分
    int len = lastSlash - path;
    if (len >= bufSize) {
        Debug("GetParentPath: Buffer too small\n");
        return -1; // 缓冲区不足
    }

    strncpy(parentBuf, path, len);
    parentBuf[len] = '\0';
    Debug("GetParentPath: parent path: %s\n", parentBuf);

    return 0;
}

/* 计算指定字节数所需的块数  */
int FindNumBlocks(ulong_t size)
{
    return (((size - 1) / GOSFS_FS_BLOCK_SIZE) + 1);
}

/* 查找下一个空闲索引节点inode */
int FindFreeInode(struct Mount_Point* mountPoint, ulong_t* retInode)
{
    int rc = -1;
    ulong_t i;
    
    for (i = 0; i < GOSFS_NUM_INODES; i++)
    {
        if (((struct GOSFS_Instance*)(mountPoint->fsData))->superblock.inodes[i].flags == 0)
        {
            rc = 0;
            *retInode = i;
            break;
        }
    }
    
    return rc;
}

/**
 * 检查指定目录是否为空。
 * 
 * 该函数通过遍历目录inode的所有直接数据块，判断是否存在类型为GOSFS_DIRTYP_REGULAR的目录项。
 * 若发现任何非空目录项（非GOSFS_DIRTYP_FREE），则目录非空；若所有目录项均为空，则目录为空。
 * 如果传入的inode不是目录类型，则直接返回true（视为空）。
 *
 * @param pInode       指向待检查目录的inode结构体
 * @param p_instance   指向当前GOSFS文件系统实例的指针
 *
 * @return 返回布尔值表示目录是否为空：
 *         - true  (1)：目录为空或非目录类型
 *         - false (0)：目录非空
 *         - -1（EUNSPECIFIED）：发生错误（如内存不足、I/O错误）
 *
 * @note 错误情况下会返回负值错误码，调用者应检查返回值符号以区分空/非空状态与错误
 */
bool IsDirectoryEmpty(struct GOSFS_Instance* p_instance, struct GOSFS_Dir_Entry* pInode)
{
    bool rc = 1, rc2 = 0;
    int i = 0, e = 0;
    struct FS_Buffer* p_buff = 0;
    ulong_t blockNum = 0;
    struct GOSFS_Directory* tmpDir = 0;
    
    // 检查是否为一个目录 
    if (!(pInode->flags & GOSFS_DIRENTRY_ISDIRECTORY)) 
        goto finish;
    
    // 检查是否存在包含非空目录的BLOCK
    for (i=0; i < GOSFS_NUM_DIRECT_BLOCKS; i++)
    {
        blockNum = pInode->blockList[i];
        if (blockNum != 0)
        {
            Debug("IsDirectoryEmpty: found direct block %ld\n",blockNum);
            rc2 = Get_FS_Buffer (p_instance->buffercache, blockNum, &p_buff);

            for (e=0; e < GOSFS_DIR_ENTRIES_PER_BLOCK; e++)
            {
                //Debug("checking directory entry %d\n",e);
                tmpDir = (struct GOSFS_Directory*) ( (p_buff->data)+(e*sizeof(struct GOSFS_Directory)) );
                if (tmpDir->type == GOSFS_DIRTYP_REGULAR)
                {
                    Debug(
                        "IsDirectoryEmpty: found used directory %d(%s) in block %ld\n", 
                        e, 
                        tmpDir->filename, 
                        blockNum
                    );
                    rc = 0;
                    goto finish;
                }
            }

            rc2 = Release_FS_Buffer(p_instance->buffercache, p_buff);
            p_buff = 0;
            if (rc2<0)
            {
                Debug("IsDirectoryEmpty: Unable to release fs_buffer for new-directory\n");
                rc = -1;
                goto finish;
            }

        }
    }

finish:
    if (p_buff != 0)  Release_FS_Buffer(p_instance->buffercache, p_buff);
    Debug("IsDirectoryEmpty: IsDirectoryEmpty returns %d\n", (int)rc);
    return rc;
}

/* 为inode创建下一个带有目录项的block */
int CreateNextDirectoryBlock(struct FS_Buffer* p_buff)
{
    struct GOSFS_Directory dirEntry;
    int i;
    
    for (i=0; i < GOSFS_DIR_ENTRIES_PER_BLOCK; i++)
    {
        dirEntry.type = GOSFS_DIRTYP_FREE;
        dirEntry.inode = 0;
        strcpy(dirEntry.filename, "\0");
        
        memcpy(p_buff->data + (i * sizeof(struct GOSFS_Directory)), &dirEntry, sizeof(struct GOSFS_Directory));
    }
    
    return 0;
}

/*通过inode删除目录项 */
int RemoveDirEntryFromInode(struct GOSFS_Instance* p_instance, ulong_t parentInode, ulong_t inode)
{
    int rc = 0, i = 0, e = 0;
    struct FS_Buffer* p_buff = 0;
    struct GOSFS_Directory* tmpDir = 0;
    ulong_t blockNum = 0;
    
    Debug("RemoveDirEntryFromInode: About to remove inode %ld from dir-inode %ld\n", inode, parentInode);
    
    // 在目录项中搜索要删除的inode 
    for (i=0; i < GOSFS_NUM_DIRECT_BLOCKS; i++)
    {
        // Debug("RemoveDirEntryFromInode: BLOCK: %d\n", i);
        blockNum = p_instance->superblock.inodes[parentInode].blockList[i];
        if (blockNum != 0)
        {        
            rc = Get_FS_Buffer(p_instance->buffercache,blockNum,&p_buff);
            
            for (e=0; e < GOSFS_DIR_ENTRIES_PER_BLOCK; e++)
            {
                tmpDir = (struct GOSFS_Directory*)((p_buff->data) + (e * sizeof(struct GOSFS_Directory)));
                if (tmpDir->inode == inode)
                {
                    Debug("RemoveDirEntryFromInode: found directory entry %d in Block %ld\n",e,blockNum);
                    tmpDir->inode = 0;
                    tmpDir->type = GOSFS_DIRTYP_FREE;
                    strcpy(tmpDir->filename, "\0");
                    
                    Modify_FS_Buffer(p_instance->buffercache, p_buff);

                    // 减少parent_inode的数量
                    if (p_instance->superblock.inodes[parentInode].size != 0) 
                        p_instance->superblock.inodes[parentInode].size--;
                    goto finish;
                }
            }

            if (p_buff != 0) Release_FS_Buffer(p_instance->buffercache, p_buff);
            p_buff = 0;
        }
    }
finish:
    if (p_buff != 0)  Release_FS_Buffer(p_instance->buffercache, p_buff);
    if (rc == 0) {
        Debug("RemoveDirEntryFromInode: done, path = %d\n", inode);
    } else {
        Debug("RemoveDirEntryFromInode: failed, path = %d\n", inode);
    }
    return rc;
}


/* 向inode添加目录项 */
int AddDirectoryEntryToInode(
    struct GOSFS_Instance* p_instance, 
    ulong_t parentInode, 
    struct GOSFS_Directory* dirEntry
)
{
    int i = 0, e = 0, rc = 0, found = 0;
    ulong_t blockNum;
    struct FS_Buffer* p_buff = 0;
    struct GOSFS_Directory* tmpDir = 0;
    
    //在 GOSFS_Directory里面寻找空闲块
    for (i=0; i < GOSFS_NUM_DIRECT_BLOCKS; i++)
    {
            
        blockNum = p_instance->superblock.inodes[parentInode].blockList[i];
        if (blockNum == 0) continue;
        
        Debug("AddDirectoryEntryToInode: found direct block %ld\n", blockNum);
        rc = Get_FS_Buffer(p_instance->buffercache, blockNum, &p_buff);
        if (rc < 0) {
            Debug("AddDirectoryEntryToInode: Failed to get buffer for block %ld\n", blockNum);
            goto finish;
        }
        for (e=0; e < GOSFS_DIR_ENTRIES_PER_BLOCK; e++)
        {
            //Debug("checking directory entry %d\n",e);
            tmpDir = (struct GOSFS_Directory*)((p_buff->data)+(e * sizeof(struct GOSFS_Directory)));
            if (tmpDir->type == GOSFS_DIRTYP_FREE)
            {
                Debug("AddDirectoryEntryToInode: found free directory %d in block %ld\n", e, blockNum);
                
                memcpy(
                    (p_buff->data)+(e * sizeof(struct GOSFS_Directory)), 
                    dirEntry, 
                    sizeof(struct GOSFS_Directory)
                );
                found = 1;
                Modify_FS_Buffer(p_instance->buffercache, p_buff);
                p_instance->superblock.inodes[parentInode].size++;
                break;
            }
        }

        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        // Debug("AddDirectoryEntryToInode: Release_FS_Buffer done\n");
        p_buff = NULL;
        if (rc < 0) {
            Debug("AddDirectoryEntryToInode: Failed to release buffer\n");
            goto finish;
        }

        if (found) break;    
    }

    //找不到空闲目录项,创建一个新块
    if (!found)
    {    
        for (i = 0; i < GOSFS_NUM_DIRECT_BLOCKS; i++)
        {
            if (p_instance->superblock.inodes[parentInode].blockList[i] == 0)
                {        
                blockNum = Find_First_Free_Bit(p_instance->superblock.bitSet, p_instance->superblock.size);
                if (blockNum <= 0) {
                    rc = ENOSPACE;
                    goto finish;
                }
                Debug("AddDirectoryEntryToInode: found free directory 0 in block %ld\n", blockNum);
                rc = Get_FS_Buffer(p_instance->buffercache,blockNum, &p_buff);
                if (rc < 0) {
                    Debug("AddDirectoryEntryToInode: Failed to get buffer for new directory block\n");
                    goto finish;
                }
                rc = CreateNextDirectoryBlock(p_buff);
                if (rc < 0) {
                    Debug("AddDirectoryEntryToInode: Failed to initialize new directory block\n");
                    goto finish;
                }
                // 从开始位置拷贝目录
                memcpy(p_buff->data, dirEntry, sizeof(struct GOSFS_Directory));
                Modify_FS_Buffer(p_instance->buffercache, p_buff);
                rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
                p_buff = NULL;
                if (rc < 0) {
                    Debug("AddDirectoryEntryToInode: Failed to release new block buffer\n");
                    goto finish;
                }
                p_instance->superblock.inodes[parentInode].blockList[i]=blockNum;
                p_instance->superblock.inodes[parentInode].size++;
                Set_Bit(p_instance->superblock.bitSet, blockNum);
                found = 1;
                goto finish;
            }
        }
            
    }
    // 所有目录块已经被填满
    if (!found) 
    {
        rc = ENOSPACE;
        Debug("AddDirectoryEntryToInode: no free directory entry found\n");
    }else {
        Debug("AddDirectoryEntryToInode: added directory entry to inode %d\n", parentInode);
    }
    
finish:
    if (p_buff!=0) {
        Release_FS_Buffer(p_instance->buffercache, p_buff);
        Debug("AddDirectoryEntryToInode: Release_FS_Buffer done\n");
    }
    return rc;
}



/**
 * 在指定的目录 inode 中查找与给定文件名匹配的目录项，并返回对应的 inode 编号。
 *
 * 该函数遍历由 searchInode 所指向的目录的所有直接数据块（direct blocks），
 * 检查每个目录项（struct GOSFS_Directory），如果目录项类型不是空闲（GOSFS_DIRTYP_FREE）
 * 并且文件名与 path 参数匹配，则将对应的 inode 编号写入 retInode。
 *
 * @param p_instance   指向当前 GOSFS 文件系统实例的指针
 * @param path         要查找的文件名（字符串），不能为 NULL
 * @param searchInode  当前要搜索的目录的 inode 编号
 * @param retInode     [out] 存储找到的 inode 编号（若成功）
 *
 * @return             成功返回 0；否则返回 -1 表示未找到匹配项
 */
int FindInodeInDirectory(
    struct GOSFS_Instance* p_instance, 
    char* path, 
    ulong_t searchInode, 
    ulong_t* retInode
)
{
    ulong_t i = 0, e = 0, blockNum;
    int rc = -1, ret = -1;
    struct FS_Buffer *p_buff = 0;
    struct GOSFS_Directory *dirEntry;

    Debug("FindInodeInDirectory: inode=%d path=%s\n", (int)searchInode, path);
    // 查询所有直接块
    for (i=0; i<GOSFS_NUM_DIRECT_BLOCKS; i++)
    {
        // 获得searchInode指向的blockList[i]
        blockNum = p_instance->superblock.inodes[searchInode].blockList[i];
        if (blockNum != 0)
        {
            // 获取其buffer
            rc = Get_FS_Buffer(p_instance->buffercache,blockNum, &p_buff);
           
            for (e=0; e < GOSFS_DIR_ENTRIES_PER_BLOCK; e++)
            {
                // 其内容为Directory * GOSFS_DIR_ENTRIES_PER_BLOCK结构
                dirEntry = (struct GOSFS_Directory*)((p_buff->data)+(e * sizeof(struct GOSFS_Directory)));
                if (dirEntry->type != GOSFS_DIRTYP_FREE)
                {
                    if (strcmp(dirEntry->filename, path)==0)
                    {
                        *retInode = dirEntry->inode;
                        ret = 0;
                        goto finish;
                    }
                }
            }
            Release_FS_Buffer(p_instance->buffercache, p_buff);
            p_buff=0;
        }
    }
    
finish:
    if (p_buff != 0) Release_FS_Buffer(p_instance->buffercache, p_buff);

    if (ret < 0) 
    {
        Debug("FindInodeInDirectory: inode not found: %d\n", ret);
        return ret;
    }
    else 
    {
        Debug("FindInodeInDirectory: returns %d\n", (int)*retInode);
        return ret;
    }
}

//通过给定的文件路径寻找inode
int FindInodeByPath(struct GOSFS_Instance* p_instance, const char* path, ulong_t* retInode)
{
    int offset, rc = 0;
    ulong_t inode = 0;
    PathIterator iter;
    char component[GOSFS_FILENAME_MAX + 1]; // 用于存储每个路径组件
    
    Debug("FindInodeByPath: path=%s, ptr=%d\n", path, (int)path);
    
    if (path[0]!='/')  {
        Debug("FindInodeByPath: path must start with '/'\n");
        return EUNSPECIFIED;
    }
    // assume root-directory
    if (strcmp(path,"/") == 0)
    {
        *retInode = 0;
        Debug("FindInodeByPath: root-directory\n");
        return rc;
    }

    InitPathIterator(&iter, path);

    while (GetNextPathComponent(&iter, component, sizeof(component))) {
        Debug("FindInodeByPath: searching for part %s in inode %d\n", component, (int)inode);
        // 查找当前目录中的子项
        rc = FindInodeInDirectory(p_instance, component, inode, &inode);
        if (rc < 0) {
            Debug("FindInodeByPath: failed to find component %s\n", component);
            break;
        }
    }
    memset(iter.path, 0, strlen(path) + 1);
    Free((void*)iter.path);

    *retInode = inode;
    if (rc == 0) {
        Debug("FindInodeByPath: found inode %d\n",(int) inode);
    }else {
        Debug("FindInodeByPath: inode not found, returns %d\n", rc);
    }
    return rc;
}

/**
 * 创建一个新的文件inode并将其添加到父目录中。
 *
 * 该函数执行以下步骤：
 * 1. 查找一个空闲的inode编号；
 * 2. 初始化inode元数据（标记为已使用、清空ACL等）；
 * 3. 构建对应的目录项，并将其插入父目录的目录块中。
 *
 * @param mountPoint   指向挂载点结构体，用于访问文件系统实例
 * @param path         文件路径，必须是绝对路径且以'/'开头
 * @param inode        [out] 存储分配的新文件inode编号
 *
 * @return int         成功返回0；失败返回错误码：
 *                     - ENOSPACE: 无可用inode或磁盘空间不足
 *                     - ENOTFOUND: 父路径无法解析（FindInodeByPath失败）
 *                     - EEXIST: 文件已存在（由AddDirectoryEntryToInode检测）
 *                     - EUNSPECIFIED: 其他未知错误
 *
 * @note               函数依赖多个底层操作如FindFreeInode、FindInodeByPath和
 *                     AddDirectoryEntryToInode，若其中任意一步失败，会提前返回错误码。
 */
int CreateFileInode(struct Mount_Point* mountPoint, const char* path, ulong_t* inode)
{
    int rc=0;
    struct GOSFS_Dir_Entry* pInode;
    struct GOSFS_Instance* p_instance = (struct GOSFS_Instance*) mountPoint->fsData;
    struct GOSFS_Directory dirEntry;
    char *filename = 0;
    ulong_t parentInode;
    char parentpath[GOSFS_FILENAME_MAX + 1];
    
    Debug("CreateFileInode: path=%s, ptr=%d\n", path, (int)path);
    // 获得最后一个/之前的string，是为filename
    filename = strrchr(path, '/') + 1;
    
    // 找到第一个free inode
    rc = FindFreeInode(mountPoint, inode);
    if (rc < 0)
    {
        rc = ENOSPACE;
        goto finish;
    }
    Debug("CreateFileInode: Free inode finded %d\n", (int)* inode);
    
    // init inode
    pInode = &(p_instance->superblock.inodes[*inode]);
    pInode->flags = GOSFS_DIRENTRY_USED;
    memset (pInode->acl, '\0', sizeof (struct VFS_ACL_Entry) * VFS_MAX_ACL_ENTRIES);
    
    //init directory entry
    dirEntry.type = GOSFS_DIRTYP_REGULAR;
    dirEntry.inode = *inode;
    strcpy(dirEntry.filename, filename);
    
    GetParentPath(path, parentpath, sizeof(parentpath));
    Debug("CreateFileInode: searching for inode of parent path: %s%d\n", parentpath);
    
    // 搜索根节点
    rc=FindInodeByPath(p_instance, parentpath, &parentInode);
    if (rc<0)
    {
        Debug("CreateFileInode: parent inode not found\n");
        rc = -1;
        goto finish;
    }
    Debug("CreateFileInode: parent inode found\n");
    
    rc=AddDirectoryEntryToInode(p_instance, parentInode, &dirEntry);
    if (rc<0)
    {
        Debug("CreateFileInode: failed to create directory-entry\n");
        rc = -1;
        goto finish;
    }

finish:
    return rc;
}

/**
 * 检查指定文件（由 inode 表示）是否分配了第 blockNum 个逻辑数据块。
 *
 * @param p_instance      指向 GOSFS 文件系统实例的指针，用于访问缓冲区缓存和超级块信息
 * @param inode           指向 GOSFS_Dir_Entry 结构体的指针，表示目标文件的 inode
 * @param blockNum        要检查的逻辑块号（从 0 开始计数）
 *
 * @return                如果该逻辑块已分配并存在，则返回 true (1)，否则返回 false (0)
 *                        若发生错误（如 I/O 错误或参数无效），也返回 false，并可能设置错误码
 *
 * @note                  此函数支持三种类型的块：
 *                          - 直接块（direct blocks）
 *                          - 一次间接块（indirect blocks）
 *                          - 二次间接块（double indirect blocks）
 *                      函数会根据 blockNum 的值自动判断应检查哪种类型的块。
 *                      内部使用 Get_FS_Buffer 和 Release_FS_Buffer 来读取间接块的数据。
 */
bool IsFileBlockExists(struct GOSFS_Instance* p_instance, struct GOSFS_Dir_Entry* inode, ulong_t blockNum)
{
    int rc = 0;
    struct FS_Buffer* p_buff = 0;
    int indirectPtr = -1, inodePtr = -1; // 0-based// 1-based
    int indirectPtrOffset = -1, indirect2xPtrOffset = -1; // 0-based
    ulong_t indirectBlock = 0;
    ulong_t phyBlock = 0, phyIndBlock = 0;
    
    // 检查块号是否在范围内, 不在范围内就是不存在，inExists
    if (blockNum >= GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS*GOSFS_NUM_PTRS_PER_BLOCK) + (GOSFS_NUM_2X_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK * GOSFS_NUM_PTRS_PER_BLOCK)) 
    {
        rc=0;
        goto finish;
    }
    
    // 检查直接块
    if (blockNum < GOSFS_NUM_DIRECT_BLOCKS)
    {
        if (inode->blockList[blockNum] != 0){
            Debug("IsFileBlockExists: blockNum %d exists\n", blockNum);
            rc = 1;
        }
            
    }
    // 检查间接块
    else if (blockNum < GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS*GOSFS_NUM_PTRS_PER_BLOCK))
    {
        // 当前逻辑块属于第几个 间接块索引项（in inode）。
        indirectPtr = ((((blockNum+1) - (GOSFS_NUM_DIRECT_BLOCKS+1)) / GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)) + 1;
        // 在这个间接块中的偏移量（即哪一项指向实际的数据块）。
        indirectPtrOffset = ((((blockNum + 1) - (GOSFS_NUM_DIRECT_BLOCKS + 1)) % GOSFS_NUM_INDIRECT_PTR_PER_BLOCK));
        // 计算这个间接块在 inode 结构中的位置（索引）。
        inodePtr = GOSFS_NUM_DIRECT_BLOCKS + indirectPtr -1;
        
        indirectBlock=inode->blockList[inodePtr];
        if (indirectBlock == 0)
        {
            rc=0;
            Debug("IsFileBlockExists: indirect block %d not exists\n", indirectBlock);
            goto finish;
        }
        
        // 写入缓存
        rc = Get_FS_Buffer(p_instance->buffercache,indirectBlock, &p_buff);
		memcpy(&phyBlock, p_buff->data + (indirectPtrOffset * sizeof(ulong_t)), sizeof(ulong_t));
        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        p_buff = 0;
        if (phyBlock > 0) {
            Debug("IsFileBlockExists: Indirect block %d\n", phyBlock);
            rc=1;
        }
    }
	// 检查二次间接块
	else
	{
		indirectPtr = ((((blockNum+1) - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + 1)) / (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK * GOSFS_NUM_INDIRECT_PTR_PER_BLOCK))) + 1;
        indirectPtrOffset = ((((blockNum+1) - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK)+1)) / (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)));
        indirect2xPtrOffset = ((((blockNum+1) - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK)+1)) % (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)));
        inodePtr = GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + indirectPtr -1;
		
		indirectBlock = inode->blockList[inodePtr];
        if (indirectBlock == 0)
        {
            rc=0;
            Debug("IsFileBlockExists: indirect block %d not exists\n", indirectBlock);
            goto finish;
        }
        
        // 读取间接块并找到对应数据块编号到phyIndBlock
        rc = Get_FS_Buffer(p_instance->buffercache, indirectBlock,&p_buff);
		memcpy(&phyIndBlock, p_buff->data + (indirectPtrOffset * sizeof(ulong_t)), sizeof(ulong_t));
        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);

        p_buff = 0;
        // 判断数据块是否存在
		if (phyIndBlock <= 0)
		{
			rc=0;
			goto finish;
		}

        // 读取二级间接块中存放的物理块最终地址phyBlock
		rc = Get_FS_Buffer(p_instance->buffercache,phyIndBlock, &p_buff);
        memcpy(&phyBlock, p_buff->data + (indirect2xPtrOffset*sizeof(ulong_t)), sizeof(ulong_t));
        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        p_buff = 0;
        if (phyBlock > 0) {
            Debug("IsFileBlockExists: In Indirect block %d\n", phyBlock);
            rc=1;
        }

	}
    
finish:
    if (p_buff != 0) Release_FS_Buffer(p_instance->buffercache, p_buff);
    if (rc == 0) Debug("IsFileBlockExists: Block %d not found\n", blockNum);
    return rc;    
}

/* 创建一个新的空闲块*/
ulong_t GetNewFreeBlock(struct GOSFS_Instance* p_instance)
{
    ulong_t freeBlock;
    int rc=0;
    struct FS_Buffer* p_buff=0;
    
    freeBlock = Find_First_Free_Bit(p_instance->superblock.bitSet, p_instance->superblock.size);
    Debug("GetNewFreeBlock: found free block %ld\n", freeBlock);
    if (freeBlock <= 0)
    {
    Debug("GetNewFreeBlock: No free Blocks found\n");
        rc = -1;
        goto finish;
    }
    
    rc = Get_FS_Buffer(p_instance->buffercache, freeBlock, &p_buff);
    // format
    memset(p_buff->data, '\0', GOSFS_FS_BLOCK_SIZE);
    Modify_FS_Buffer(p_instance->buffercache,p_buff);
    rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
    p_buff = 0;
    Set_Bit(p_instance->superblock.bitSet, freeBlock);

finish:
    if (p_buff != 0) Release_FS_Buffer(p_instance->buffercache, p_buff);
    if (rc < 0) return rc;
    else return freeBlock;
}

/* 通过块的线性地址得到块的物理地址 */
int GetPhysicalBlockByLogical(struct GOSFS_Instance* p_instance, struct GOSFS_Dir_Entry* inode, ulong_t blockNum)
{
    int rc = 0;
    int phyBlock = 0, phyIndBlock = 0;
    int indirectPtr = -1;    // which indirect inode ptr to use (1-based) 
    int indirectPtrOffset = -1; // which pointer in the indirct-block (0-based) 一次间接块
	int indirect2xPtrOffset = -1; // which pointer in the 2xindirct-block (0-based) 二次间接块
    int inodePtr = -1;    // which entry in the inode-array are we refering to (0-based)
    int indirectBlock;  // physical block with block-ptrs
    struct FS_Buffer* p_buff = 0;
    
    // 直接块
    if (blockNum < GOSFS_NUM_DIRECT_BLOCKS)
    {
        phyBlock = inode->blockList[blockNum];
    }
    // 间接块
    else if (blockNum < GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK * GOSFS_NUM_INDIRECT_BLOCKS))
    {
        indirectPtr = ((((blockNum+1) - (GOSFS_NUM_DIRECT_BLOCKS+1)) / GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)) + 1;//只能为1，因为间接块一个
        indirectPtrOffset = (((blockNum+1) - (GOSFS_NUM_DIRECT_BLOCKS+1)) % GOSFS_NUM_INDIRECT_PTR_PER_BLOCK);//间接块的偏移
        inodePtr = GOSFS_NUM_DIRECT_BLOCKS + indirectPtr -1;// 因为直接块八个，间接块一个，所以这个指针只能指向第九个
        Debug(
            "GetPhysicalBlockByLogical: blocknum: %ld, indirectPtr: %d, indirectPtrOffset: %d, inodePtr: %d\n",
            blockNum, 
            indirectPtr,
            indirectPtrOffset,
            inodePtr
        );
        
        indirectBlock = inode->blockList[inodePtr];
        if (indirectBlock == 0)
        {
            Debug("indirect pointer not initialized\n");
            rc = -1;
            goto finish;
        }
        
        rc = Get_FS_Buffer(p_instance->buffercache, indirectBlock, &p_buff);

        memcpy(&phyBlock, p_buff->data + (indirectPtrOffset*sizeof(ulong_t)), sizeof(ulong_t));

        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        p_buff = 0;

    }
	// 二次间接块
	else
	{
		indirectPtr = ((((blockNum + 1) - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + 1)) / (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK * GOSFS_NUM_INDIRECT_PTR_PER_BLOCK))) + 1;
        indirectPtrOffset = ((((blockNum + 1) - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + 1)) / (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)));
        indirect2xPtrOffset = ((((blockNum + 1) - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + 1)) % (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)));
        inodePtr = GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + indirectPtr -1;
		Debug(
            "GetPhysicalBlockByLogical: blocknum: %ld, indirectPtr: %d, indirect2xPtrOffset: %d, indirectPtrOffset: %d, inodePtr: %d\n",
            blockNum, 
            indirectPtr,
            indirect2xPtrOffset,
            indirectPtrOffset,
            inodePtr
        );
	
        indirectBlock = inode->blockList[inodePtr];
        if (indirectBlock == 0)
        {
            Debug("indirect pointer not initialized\n");
            rc = -1;
            goto finish;
        }
        
        rc = Get_FS_Buffer(p_instance->buffercache, indirectBlock, &p_buff);
      
        memcpy(&phyIndBlock, p_buff->data + (indirectPtrOffset*sizeof(ulong_t)), sizeof(ulong_t));

        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        p_buff = 0;
        rc = Get_FS_Buffer(p_instance->buffercache, phyIndBlock, &p_buff);
       
        memcpy(&phyBlock, p_buff->data + (indirect2xPtrOffset * sizeof(ulong_t)), sizeof(ulong_t));

        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        p_buff = 0;
     
		
	}
    
finish:
    if (p_buff != 0) Release_FS_Buffer(p_instance->buffercache, p_buff);
    if (rc < 0) return rc;
    else return phyBlock;
}

/* 写入间接块 */
int WriteIndirectBlockEntry(struct GOSFS_Instance* p_instance, ulong_t numBlock, ulong_t offset, ulong_t freeBlock)
{
    int rc = 0;
    struct FS_Buffer* p_buff = 0;
        
    rc = Get_FS_Buffer(p_instance->buffercache, numBlock, &p_buff);
    // format
    memcpy(p_buff->data + (offset * sizeof(ulong_t)), &freeBlock, sizeof(ulong_t));

    Modify_FS_Buffer(p_instance->buffercache, p_buff);
    rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
    p_buff = 0;

finish:
    if (p_buff != 0) Release_FS_Buffer(p_instance->buffercache, p_buff);
    return rc;
    
}

/* 为inode指向的文件分配一个新块 */
int CreateFileBlock(struct GOSFS_Instance* p_instance, struct GOSFS_Dir_Entry* inode, ulong_t blockNum)
{
    int rc = 0;
    int freeBlock;
    struct FS_Buffer* p_buff=0;
    int indirectPtr = -1;    // which indirect inode ptr to use (1-based)
    int indirectPtrOffset = -1; // which pointer in the indirct-block (0-based)
	int indirect2xPtrOffset = -1; // which pointer in the 2xindirct-block (0-based)
    int inodePtr = -1;    // which entry in the inode-array are we refering to (0-based)
    int indirectBlock;  // physical block with block-ptrs
	ulong_t phyIndBlock = -1;
    
    blockNum++; // lets start by 1 here, not 0-based
    // create block to store data in
    freeBlock = GetNewFreeBlock(p_instance);
    if (freeBlock <= 0)
    {
        Debug("CreateFileBlock: No free Blocks found\n");
        rc = -1;
        goto finish;
    }
    
    // 查看需要哪种类型的块，直接块，间接块或2x间接块 
    // direct block
    if (blockNum <= GOSFS_NUM_DIRECT_BLOCKS)
    {
        Debug("CreateFileBlock: using direct pointer\n");
        inodePtr = blockNum -1;
        inode->blockList[inodePtr] = freeBlock;
    }
    // 间接块
    else if (blockNum <= (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK * GOSFS_NUM_INDIRECT_BLOCKS ) + GOSFS_NUM_DIRECT_BLOCKS)
    {
        Debug("CreateFileBlock: using indirect pointer\n");
        indirectPtr = (((blockNum - (GOSFS_NUM_DIRECT_BLOCKS + 1)) / GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)) + 1;
        indirectPtrOffset = (((blockNum - (GOSFS_NUM_DIRECT_BLOCKS + 1)) % GOSFS_NUM_INDIRECT_PTR_PER_BLOCK));
        inodePtr = GOSFS_NUM_DIRECT_BLOCKS + indirectPtr - 1;
        
        // 如果这是间接块的首次使用，则需要对其进行初始化 
        indirectBlock = inode->blockList[inodePtr];
        if (indirectBlock == 0)
        {
            indirectBlock = GetNewFreeBlock(p_instance);
         
            Debug("CreateFileBlock: setting inode blocklistindex %d inodePtr to block %d\n",
                inodePtr,
                indirectBlock
            );
            inode->blockList[inodePtr] = indirectBlock;    
        }
        // 写入间接块
        rc = WriteIndirectBlockEntry(p_instance, indirectBlock, indirectPtrOffset, freeBlock);
         
        
    }
	// 二次间接块
    else if (blockNum <= (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK * GOSFS_NUM_PTRS_PER_BLOCK) + (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK * GOSFS_NUM_INDIRECT_BLOCKS ) + GOSFS_NUM_DIRECT_BLOCKS)
    {
        Debug("CreateFileBlock: using 2Xindirect pointer\n");
		indirectPtr = (((blockNum - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + 1)) / (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK * GOSFS_NUM_INDIRECT_PTR_PER_BLOCK))) + 1;
        indirectPtrOffset = (((blockNum - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + 1)) / (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)));
		indirect2xPtrOffset = (((blockNum - (GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + 1)) % (GOSFS_NUM_INDIRECT_PTR_PER_BLOCK)));
        inodePtr = GOSFS_NUM_DIRECT_BLOCKS + (GOSFS_NUM_INDIRECT_BLOCKS * GOSFS_NUM_PTRS_PER_BLOCK) + indirectPtr - 1;

		// 如果这是二次间接块的首次使用，则需要对其进行初始化 
        indirectBlock = inode->blockList[inodePtr];
        if (indirectBlock == 0)
        {
            indirectBlock = GetNewFreeBlock(p_instance);
         
            Debug(
                "CreateFileBlock: setting inode 2xblocklistindex %d inodePtr to block %d\n",
                inodePtr,
                indirectBlock
            );
            inode->blockList[inodePtr] = indirectBlock;    
        }
		
		rc = Get_FS_Buffer(p_instance->buffercache, indirectBlock, &p_buff);
        memcpy(&phyIndBlock, p_buff->data + (indirectPtrOffset*sizeof(ulong_t)), sizeof(ulong_t));
        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        p_buff = 0;
      
		// 如果block还没有被分配
		if (phyIndBlock <= 0)
		{
			phyIndBlock = GetNewFreeBlock(p_instance);
            if (phyIndBlock <= 0)
            {
                Debug("CreateFileBlock: No free Blocks found for 2xindirect block\n");
                rc=-1;
                goto finish;
            }
			// 写入二次间接块
        	rc = WriteIndirectBlockEntry(p_instance, indirectBlock, indirectPtrOffset, phyIndBlock);
        	if (rc<0)
        	{
            	Debug("CreateFileBlock: could not write entry to indirect block %d\n", rc);
            	rc = -1;
            	goto finish;
        	}
			
		}
		
		// 写入二次间接块
        rc = WriteIndirectBlockEntry(p_instance, phyIndBlock, indirect2xPtrOffset, freeBlock);
	}
    else
    {
        Debug("CreateFileBlock: maximum filesize reached\n");
        rc = -1;
        goto finish;
    }
        
    
finish:
    if (p_buff!=0) Release_FS_Buffer(p_instance->buffercache, p_buff);
    return rc;
}

/* 为inode创建第一个带有目录项的block */
int CreateFirstDirectoryBlock(ulong_t thisInode, struct FS_Buffer* p_buff, char* name)
{
    struct GOSFS_Directory dirEntry;
    int i;
    
    Debug("CreateFirstDirectoryBlock: start\n");
    // 根目录的第一个目录项设置为自己    
    for (i=0; i<GOSFS_DIR_ENTRIES_PER_BLOCK; i++)
    {   
        if (i==0)
        {
            dirEntry.type = GOSFS_DIRTYP_THIS;
            dirEntry.inode = thisInode;
            strcpy(dirEntry.filename, name);
        }
        else
        {
            dirEntry.type = GOSFS_DIRTYP_FREE;
            dirEntry.inode = 0;
            strcpy(dirEntry.filename, "\0");
        }
        
        memcpy(p_buff->data + (i*sizeof(struct GOSFS_Directory)), &dirEntry, sizeof(struct GOSFS_Directory));
    }
    
    return 0;
}

/* 将超级块从内存写入磁盘  */
int WriteSuperblock(struct GOSFS_Instance* p_instance)
{
    int numBlocks, rc = 0;
    ulong_t numBytes, bwritten = 0, i;
    struct FS_Buffer *p_buff = 0;
    
    numBytes = p_instance->superblock.supersize;
    numBlocks = FindNumBlocks(numBytes);
    
    for (i=0; i<numBlocks; i++)
    {
        
        rc = Get_FS_Buffer(p_instance->buffercache, i, &p_buff);
        if ((p_instance->superblock.supersize - bwritten) < GOSFS_FS_BLOCK_SIZE)
        {
            memcpy(p_buff->data, ((void*)&(p_instance->superblock)) + bwritten, p_instance->superblock.supersize - bwritten);
            bwritten = bwritten + (p_instance->superblock.supersize - bwritten);
        }
        else
        {
            memcpy(p_buff->data, ((void*)&(p_instance->superblock)) + bwritten, GOSFS_FS_BLOCK_SIZE);
            bwritten = bwritten + GOSFS_FS_BLOCK_SIZE;
        }
        
        Modify_FS_Buffer(p_instance->buffercache, p_buff);
        rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
        p_buff = 0;
    }
        
finish:
    if (p_buff!=0) Release_FS_Buffer(p_instance->buffercache, p_buff);
    return rc;
}

/* ----------------------------------------------------------------------
 * Implementation of VFS operations
 * ---------------------------------------------------------------------- */

/*
 * Get metadata for given file.
 * 为给定的文件得到元数据
 */
static int GOSFS_FStat(struct File *file, struct VFS_File_Stat *stat)
{
    int rc = 0;
    struct GOSFS_File_Entry* fileEntry = (struct GOSFS_File_Entry*) file->fsData;
    
    Mutex_Lock(&fileEntry->instance->lock);
    
    stat->size = fileEntry->inode->size;
    
    if (fileEntry->inode->flags & GOSFS_DIRENTRY_ISDIRECTORY)
        stat->isDirectory = 1;
    else
        stat->isDirectory = 0;
    memcpy (
        stat->acls, 
        fileEntry->inode->acl,
        sizeof(struct VFS_ACL_Entry) * VFS_MAX_ACL_ENTRIES);
    Mutex_Unlock(&fileEntry->instance->lock);

    return rc;
}

/*
 * Read data from current position in file.
 * 从给定文件的当前位置读数据
 */
static int GOSFS_Read(struct File *file, void *buf, ulong_t numBytes)
{

    int rc = 0;
    struct GOSFS_File_Entry* pFileEntry= (struct GOSFS_File_Entry*)file->fsData;
    ulong_t offset = file->filePos;
    ulong_t readTo = (file->filePos+numBytes-1);
    struct FS_Buffer* p_buff = 0;
    ulong_t startBlock = offset / GOSFS_FS_BLOCK_SIZE;
    ulong_t endBlock = readTo / GOSFS_FS_BLOCK_SIZE;
    ulong_t i = 0;
    ulong_t phyBlock, readFrom = 0, readNum = 0, bytesRead = 0;


    Debug (
        "GOSFS_Read: about to read from offs = %ld (startblk = %ld) to end = %ld (endblk = %ld)\n",
        offset, 
        startBlock, 
        readTo, 
        endBlock
        );

    Mutex_Lock(&pFileEntry->instance->lock);
    
    // 检查文件是否可读
    if (!(file->mode & O_READ))
    {
        Debug("GOSFS_Read: trying to read from wirte-only file\n");
        rc = EACCESS;
        goto finish;
    }
    
    Debug ("GOSFS_Read: %ld, endpos: %ld\n", file->filePos, file->endPos);
    // 检查文件是否已经读完
    if (file->filePos >= file->endPos)
    {
        bytesRead=0;
        goto finish;
    }        
    for (i=startBlock; i<=endBlock; i++)
    {
        //获取物理块
        phyBlock = GetPhysicalBlockByLogical(pFileEntry->instance, pFileEntry->inode, i);

        if (phyBlock == 0)
        {
            Debug("GOSFS_Read: block not allocated\n");
            rc = -1;
            goto finish;
        }
        
        rc = Get_FS_Buffer(pFileEntry->instance->buffercache,phyBlock,&p_buff);
     
        if (i == startBlock)
            readFrom = offset % GOSFS_FS_BLOCK_SIZE;
        else
            readFrom = 0;
        
        readNum = GOSFS_FS_BLOCK_SIZE - readFrom;
        if (bytesRead+readNum > numBytes) readNum = numBytes - bytesRead;
        if (readNum + bytesRead + offset > file->endPos)
            readNum = file->endPos - offset - bytesRead;
        memcpy(buf + bytesRead, p_buff->data + readFrom, readNum);
        bytesRead = bytesRead + readNum;

        rc = Release_FS_Buffer(pFileEntry->instance->buffercache, p_buff);
        p_buff = 0;
        if (rc<0)
        {
            Debug("GOSFS_Read: Unable to release fs_buffer\n");
            rc = -1;
            goto finish;
        }
    }
    file->filePos = file->filePos+numBytes;
    
finish:
    Debug ("GOSFS_Read: numBytesRead = %ld\n", bytesRead);
    if (p_buff!=0) Release_FS_Buffer(pFileEntry->instance->buffercache, p_buff);
    Mutex_Unlock(&pFileEntry->instance->lock);
    if (rc<0) return rc;
    else return bytesRead;
}

 /**
 * Write data to current position in file.
 * 将数据写入文件的当前位置。
 *
 * 该函数将用户提供的缓冲区 `buf` 中的 `numBytes` 字节数据写入文件的当前文件指针位置。
 * 支持基于逻辑块的写入，并在需要时分配新的物理块。
 *
 * @param file      指向表示已打开文件的 `struct File` 的指针
 * @param buf       指向要写入数据的缓冲区
 * @param numBytes  要写入的字节数
 *
 * @return          成功写入的字节数；若发生错误，则返回负值错误码：
 *                  - EACCESS: 文件未以写模式打开
 *                  - ENOSPACE: 磁盘空间不足或无法获取物理块
 *                  - 其他来自底层函数（如 CreateFileBlock、Get_FS_Buffer）的错误码
 *
 * @note            本函数会自动处理以下操作：
 *                    - 检查写权限
 *                    - 计算涉及的逻辑块范围
 *                    - 若逻辑块不存在，调用 `CreateFileBlock` 分配新块
 *                    - 获取对应的物理块编号并读取其内容到缓冲区
 *                    - 执行实际的数据拷贝和缓冲区修改
 *                    - 更新文件大小和文件指针位置
 */
static int GOSFS_Write(struct File *file, void *buf, ulong_t numBytes)
{
    // TODO("GeekOS filesystem write operation");
    int rc=0;
    ulong_t startBlock = 0;
    ulong_t startBlockOffset = 0;
    ulong_t endBlock = 0;
    ulong_t i = 0;
    struct GOSFS_File_Entry* pFileEntry = file->fsData;
    struct FS_Buffer* p_buff = 0;
    ulong_t phyBlock, writeFrom, writeNum, bytesWritten = 0;
    struct Mutex* lock = &pFileEntry->instance->lock;
    
    Debug("GOSFS_Write: about to write %ld bytes at offset %ld\n", numBytes, file->filePos);
    
    Mutex_Lock(lock);
    
    // 检查写操作是否被允许
    if (!(file->mode & O_WRITE))
    {
        Debug("GOSFS_Write: trying to write from read-only file\n");
        rc = EACCESS;
        goto finish;
    }
    
    // 计算需要写入的数据块
    startBlock = file->filePos / GOSFS_FS_BLOCK_SIZE;
    startBlockOffset = file->filePos % GOSFS_FS_BLOCK_SIZE;
    endBlock = (file->filePos + numBytes) / GOSFS_FS_BLOCK_SIZE;

    Debug("GOSFS_Write: logical blocks %ld - %ld needed\n", startBlock, endBlock);
    
    // 遍历需要写入的块
    for (i=startBlock; i<=endBlock; i++)
    {
        // 检查inode是否分配有i块，
        if (!IsFileBlockExists(pFileEntry->instance, pFileEntry->inode, i))
        {
            Debug("GOSFS_Write: block not allocated, allocate new block\n");
            rc=CreateFileBlock(pFileEntry->instance, pFileEntry->inode, i);
            if (rc<0)
            {
                Debug("GOSFS_Write: received errorcode %d from CreateFileBlock\n", rc);
                goto finish;
            }
        }
        
        //phyBlock = pFileEntry->inode->blockList[i];
        phyBlock = GetPhysicalBlockByLogical(pFileEntry->instance, pFileEntry->inode, i);
        if (phyBlock <= 0)
        {
            Debug("GOSFS_Write: block not allocated \n");
            rc = ENOSPACE;
            goto finish;
        }
        
        Debug(
            "GOSFS_Write: About to write (logical) blocknumber %ld to physical block %ld\n",
            i,
            phyBlock
        );
        
        // write data
        rc = Get_FS_Buffer(pFileEntry->instance->buffercache, phyBlock, &p_buff);
        if (rc<0)
        {
            Debug("GOSFS_Write: Unable to get buffer\n");
            rc = -1;
            goto finish;
        }
        
        if (i == startBlock) writeFrom = startBlockOffset;
        else writeFrom = 0;
            
        writeNum = GOSFS_FS_BLOCK_SIZE - writeFrom;
        
        if (writeNum > numBytes - bytesWritten) writeNum = numBytes - bytesWritten;
        Debug("GOSFS_Write: writeFrom=%ld, writeNum=%ld\n", writeFrom, writeNum);
        
        memcpy(p_buff->data+writeFrom, buf+bytesWritten, writeNum);
        bytesWritten = bytesWritten + writeNum;
        Modify_FS_Buffer(pFileEntry->instance->buffercache, p_buff);
        rc = Release_FS_Buffer(pFileEntry->instance->buffercache, p_buff);
        p_buff = 0;

    }
    
    // 使inode信息和文件描述符保持最新 
    if (file->filePos + numBytes > pFileEntry->inode->size)
    {
        pFileEntry->inode->size = file->filePos + numBytes;
        file->endPos = pFileEntry->inode->size;
    }
    file->filePos = file->filePos + numBytes;

finish:
    if (p_buff != 0) Release_FS_Buffer(pFileEntry->instance->buffercache, p_buff);
    Mutex_Unlock(lock);
    if (rc < 0) return rc;
    else return bytesWritten;
}

/*
 * Seek to a position in file.
 * 文件指针定位
 */
static int GOSFS_Seek(struct File *file, ulong_t pos)
{
    //TODO("GeekOS filesystem seek operation");
    int rc = 0;
    file->filePos = pos;
    return rc;
}

/*
 * 关闭文件
 * Close a file.
 */
static int GOSFS_Close(struct File *file)
{
    struct GOSFS_File_Entry* pFileEntry = file->fsData;
    struct GOSFS_Instance* p_instance = pFileEntry->instance;

   if (!pFileEntry)
        return 0;
    
    Mutex_Lock (&p_instance->lock);
    Free (pFileEntry);
    Mutex_Unlock (&p_instance->lock);

    return 0;
}

static struct File_Ops s_gosfsFileOps = {
    &GOSFS_FStat,
    &GOSFS_Read,
    &GOSFS_Write,
    &GOSFS_Seek,
    &GOSFS_Close,
    0, /* Read_Entry */
};

/*
 * Stat operation for an already open directory.
 * stat已经打开的目录
 */
static int GOSFS_FStat_Directory(struct File *dir, struct VFS_File_Stat *stat)
{
    int rc=0;
    struct GOSFS_File_Entry* fileEntry = 0;
    stat->size=dir->endPos;
    stat->isDirectory=1;
    stat->isSetuid=0;
    
    if (!fileEntry)
        Print(" GOSFS_FStat_Directory: ERROR: fsData == NULL!\n");
    else
        memcpy(
            stat->acls, 
            fileEntry->inode->acl,
            sizeof(struct VFS_ACL_Entry) * VFS_MAX_ACL_ENTRIES
        );
    return rc;
}

/*
 * Directory Close operation.
 * 关闭目录操作
 */
static int GOSFS_Close_Directory(struct File *dir)
{
    //TODO("GeekOS filesystem Close directory operation");
    struct GOSFS_Directory *dirEntries = 0;

    if (!dir) return EINVALID;
    dirEntries = (struct GOSFS_Directory*)dir->fsData;
    if (!dirEntries) return EINVALID;
    Free(dirEntries);
    return 0;
}

/*
 * Read a directory entry from an open directory.
 * 从打开的目录中读取目录项
 */
static int GOSFS_Read_Entry(struct File *dir, struct VFS_Dir_Entry *entry)
{
    //TODO("GeekOS filesystem Read_Entry operation");
    int rc=0;
    ulong_t offset = dir->filePos+1;
    struct GOSFS_Directory *directory;
    struct GOSFS_Dir_Entry *inode;
    struct VFS_File_Stat stats;
        
    if (dir->filePos >= dir->endPos)
        return VFS_NO_MORE_DIR_ENTRIES;    // we are at the end of the file
    
    directory = ((struct GOSFS_Directory*) dir->fsData)+offset;
    inode = &(((struct GOSFS_Instance*)(dir->mountPoint->fsData))->superblock.inodes[directory->inode]);

    strcpy(entry->name, directory->filename);

    entry->stats.size = inode->size;
    entry->stats.isDirectory = ((inode->flags & GOSFS_DIRENTRY_ISDIRECTORY) != 0);
    entry->stats.isSetuid = ((inode->flags & GOSFS_DIRENTRY_SETUID) != 0);
    memcpy (
        entry->stats.acls, 
        inode->acl, 
        sizeof(struct VFS_ACL_Entry) * VFS_MAX_ACL_ENTRIES
    );

    Debug(
        "GOSFS_Read_Entry: name=%s, isDirectory=%d\n", 
        entry->name, 
        entry->stats.isDirectory
    );
    dir->filePos++;    // increase file pos

    return rc;
}

/*static*/ struct File_Ops s_gosfsDirOps = {
    &GOSFS_FStat_Directory,
    0, /* Read */
    0, /* Write */
    &GOSFS_Seek,
    &GOSFS_Close_Directory,
    &GOSFS_Read_Entry,
};

/*
 * Open a file named by given path.
 * 打开一个由给定路径命名的文件
 */
static int GOSFS_Open(
    struct Mount_Point *mountPoint, 
    const char *path, 
    int mode, 
    struct File **pFile
)
{
    //TODO("GeekOS filesystem open operation");
    int rc=0;
    struct GOSFS_Instance* p_instance = (struct GOSFS_Instance*) mountPoint->fsData;
    ulong_t inode;
    struct GOSFS_Dir_Entry* pInode;
    struct GOSFS_File_Entry* pFileEntry=0;
    
    Debug ("GOSFS_Open: path=%s, mode=%d\n",path, mode);
    
    Mutex_Lock(&p_instance->lock);
    
    // check if file already exists
    rc=FindInodeByPath(p_instance, path, &inode);
    if (rc<0)
    {
        Debug ("GOSFS_Open: file not found, path=%s, mode=%d\n",path, mode);
        if (!(mode & O_CREATE))
        {
            rc=ENOTFOUND;
            goto finish;
        }
        Debug ("GOSFS_Open: about to create file, path=%s, mode=%d\n",path, mode);
        
        rc = CreateFileInode(mountPoint, path, &inode);
        
        if (rc<0)
        {
            Debug ("GOSFS_Open: file could not be created, path=%s, mode=%d\n",path, mode);
            rc=-1;
            goto finish;
        }
    }

    pInode = &p_instance->superblock.inodes[inode];
    pFileEntry = Malloc(sizeof(struct GOSFS_File_Entry));
    if (pFileEntry == 0)
    {
        rc = ENOMEM;
        goto finish;
    }
    pFileEntry->inode = pInode;
    pFileEntry->instance = p_instance;
    
    // pfat中filePos也是0
    struct File *file = Allocate_File(&s_gosfsFileOps, 0, pInode->size, pFileEntry, mode, mountPoint);
    if (file == 0) {
        rc = ENOMEM;
        goto finish;
    }

    *pFile = file;
    
    if (rc  == 0) {
        Debug("GOSFS_Open: File Opend, path = %s\n",path);
    }

finish:
    if ((rc<0) && (pFileEntry!=0)) Free(pFileEntry);
    Mutex_Unlock(&p_instance->lock);
    return rc;
}

/*
 * Create a directory named by given path.
 * 创建一个指定路径的目录
 */
static int GOSFS_Create_Directory(struct Mount_Point *mountPoint, const char *path)
{
    int rc=0,e;
    ulong_t freeBlock;
    struct GOSFS_Directory    dirEntry;
    struct FS_Buffer            *p_buff=0;
    struct GOSFS_Instance        *p_instance;
    ulong_t freeInode, parentInode, tmpInode;
    char*                filename=0;
    char parentpath[GOSFS_FILENAME_MAX + 1];
    
    p_instance = (struct GOSFS_Instance*) mountPoint->fsData;    
        
    Debug("GOSFS_Create_Directory: about to create directory %s, ptr %d\n",path, (int)path);
    
    Mutex_Lock(&p_instance->lock);
    
    GetParentPath(path, parentpath, sizeof(parentpath));
    Debug("GOSFS_Create_Directory: searching for inode of parent path %s, ptr %d\n",parentpath, (int)parentpath);
    rc=FindInodeByPath(p_instance, parentpath, &parentInode);
    
    rc=FindFreeInode(mountPoint, &freeInode);
    Debug("GOSFS_Create_Directory: found free inode %d\n",(int)freeInode);
   

    // strip filename from path including /
    filename=strrchr(path,'/')+1;
    
    // 检查是否已经存在
    rc=FindInodeInDirectory(p_instance, filename, parentInode, &tmpInode);
    // 写入 directory entry 在 父 inode
    dirEntry.type = GOSFS_DIRTYP_REGULAR;
    dirEntry.inode = freeInode;
    strcpy(dirEntry.filename, filename);
    rc = AddDirectoryEntryToInode(p_instance, parentInode, &dirEntry);
    Debug("GOSFS_Create_Directory:AddDirectoryEntryToInode done\n");
    freeBlock=Find_First_Free_Bit(p_instance->superblock.bitSet, p_instance->superblock.size);
    if (freeBlock <= 0) {
        Debug("GOSFS_Create_Directory: No free blocks available\n");
        rc = -1;
        goto finish;
    }

    rc = Get_FS_Buffer(p_instance->buffercache,freeBlock,&p_buff);
    if (rc < 0 || !p_buff) {
        Debug("GOSFS_Create_Directory: Failed to get buffer for new directory block\n");
        rc = -1;
        goto finish;
    }

    rc = CreateFirstDirectoryBlock(freeInode, p_buff, filename);
    if (rc < 0) {
        Debug("GOSFS_Create_Directory: Failed to create first directory block\n");
        goto finish;
    }

    Modify_FS_Buffer(p_instance->buffercache,p_buff);
    rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
    if (rc < 0) {
        Debug("GOSFS_Create_Directory: Failed to release buffer for new directory block\n");
    } 
    p_buff = 0;
    Set_Bit(p_instance->superblock.bitSet, freeBlock);

    p_instance->superblock.inodes[freeInode].size=1;        
    p_instance->superblock.inodes[freeInode].flags = GOSFS_DIRENTRY_ISDIRECTORY | GOSFS_DIRENTRY_USED;
    memset (p_instance->superblock.inodes[freeInode].acl, '\0', sizeof (struct VFS_ACL_Entry) * VFS_MAX_ACL_ENTRIES);
    
    p_instance->superblock.inodes[freeInode].blockList[0]=freeBlock;
    
finish:
    Mutex_Unlock(&p_instance->lock);
    if (rc==0) {
        Debug("GOSFS_Create_Directory: Create_Directory_In_FS: %s\n", path);
    }else {
        Debug("GOSFS_Create_Directory: failed to Create_Directory_In_FS: %s failed\n", path);
    }

    return rc;
}

/*
 * Open a directory named by given path.
 * 通过指定路径打开文件夹
 */
static int GOSFS_Open_Directory(
    struct Mount_Point *mountPoint, 
    const char *path, 
    struct File **pDir
)
{
    int rc = 0, i, e;
    ulong_t inodeNum, blockNum, found = 0;
    struct GOSFS_Dir_Entry    *inode = 0;
    struct GOSFS_Directory *dirEntries = 0, *tmpDir;
    struct FS_Buffer *p_buff = 0;
    struct GOSFS_Instance* p_instance = (struct GOSFS_Instance*)mountPoint->fsData;
    
    Mutex_Lock(&p_instance->lock);
    Debug("GOSFS_Open_Directory: About to open Directory %s\n", path);
    
    // spezial treatment for root-directory
    if (strcmp(path,"/") == 0)
    {
        inodeNum=0;
    }
    else
    {
        rc = FindInodeByPath((struct GOSFS_Instance*)mountPoint->fsData, path, &inodeNum);
        Debug("GOSFS_Open_Directory: done FindInodeByPath returned %d\n", rc);
        if (rc < 0)  goto finish;
    }
    
    Debug("GOSFS_Open_Directory: About to open Inode %d\n", inodeNum);
    inode = &(((struct GOSFS_Instance*)mountPoint->fsData)->superblock.inodes[inodeNum]);
    Debug("GOSFS_Open_Directory: Inode %d is type %d\n", inodeNum, inode->flags);
    
    Debug("GOSFS_Open_Directory: Init File\n");
    *pDir = Allocate_File(&s_gosfsDirOps, 0, inode->size, inode, O_READ, mountPoint);
    Debug("GOSFS_Open_Directory: About to read Inode %d\n", inodeNum);
    
    dirEntries = Malloc(inode->size * sizeof(struct GOSFS_Directory));
    if (dirEntries == 0)
    {
        rc = ENOMEM;
        goto finish;
    }
    
    // 读取目录
    for (i=0; i < GOSFS_NUM_DIRECT_BLOCKS; i++)
    {        
        blockNum = inode->blockList[i];
        if (blockNum != 0)
        {
            Debug("GOSFS_Open_Directory: found direct block %ld\n",blockNum);
            rc = Get_FS_Buffer(((struct GOSFS_Instance*)mountPoint->fsData)->buffercache,blockNum,&p_buff);
          
            for (e = 0; e < GOSFS_DIR_ENTRIES_PER_BLOCK; e++)
            {
                tmpDir = (struct GOSFS_Directory*)((p_buff->data)+(e*sizeof(struct GOSFS_Directory)));
                if (tmpDir->type != GOSFS_DIRTYP_FREE)
                {
                    Debug("GOSFS_Open_Directory: found directory entry %d\n",e);
                    memcpy(dirEntries+found, tmpDir, sizeof(struct GOSFS_Directory));
                    found++;
                }
            }
            
            rc = Release_FS_Buffer(((struct GOSFS_Instance*)mountPoint->fsData)->buffercache, p_buff);
            p_buff = 0;         
        }
    }
    (*pDir)->fsData = dirEntries;
        
    Debug ("GOSFS_Open_Directory: pid=%d fda=%p\n",
          g_currentThread->pid, dirEntries);
    
finish:
    if ((rc < 0) && (dirEntries != 0))  Free(dirEntries);
    if (p_buff != 0)  Release_FS_Buffer(((struct GOSFS_Instance*)mountPoint->fsData)->buffercache, p_buff);
    Mutex_Unlock(&p_instance->lock);
    return rc;
}

/*
 * Delete a directory named by given path.
 * 删除由给定路径命名的文件或目录
 */
static int GOSFS_Delete(struct Mount_Point *mountPoint, const char *path)
{
    //TODO("GeekOS filesystem delete operation");
    int rc=0,i=0;
    ulong_t inodeNum=-1, parentInodeNum=-1, e=0, f=0;
    struct GOSFS_Dir_Entry *pInode=0;
    ulong_t blockNum=0, blockIndirect=0, block2Indirect=0;
    struct GOSFS_Instance *p_instance = (struct GOSFS_Instance*)mountPoint->fsData;
    char  *offset;
    struct FS_Buffer *p_buff=0; 
    char parentPath[GOSFS_FILENAME_MAX + 1];
    Debug("GOSFS_Delete: About to delete %s, ptr %d\n", path, (int)path);
    Mutex_Lock(&p_instance->lock);
    
    rc = FindInodeByPath(p_instance, path, &inodeNum);
    

    
    pInode = &(p_instance->superblock.inodes[inodeNum]);
    
    if (!IsDirectoryEmpty(p_instance, pInode))
    {
        Debug("GOSFS_Delete: seems to be non-empty directory\n");
        rc = -1;
        goto finish;        
    }
    
    // find parent directory
    GetParentPath(path, parentPath, sizeof(parentPath));
    Debug("GOSFS_Delete: parent-path: %s\n",parentPath);

    rc = FindInodeByPath(p_instance, parentPath, &parentInodeNum);

    // free all asigned direct blocks of this inode
    for (i=0; i<GOSFS_NUM_DIRECT_BLOCKS; i++)
    {
        blockNum = pInode->blockList[i];
        if (blockNum != 0)
        {
            Clear_Bit(p_instance->superblock.bitSet, blockNum);
        }
    }
    
    // free indirect-blocks
    for (i=0; i<GOSFS_NUM_INDIRECT_BLOCKS; i++)
    {
        blockNum = pInode->blockList[GOSFS_NUM_DIRECT_BLOCKS+i];
        if (blockNum !=0)
        {
            Debug("GOSFS_Delete: found indirect block %ld --> freeing\n",blockNum);
            
            rc = Get_FS_Buffer(p_instance->buffercache,blockNum,&p_buff);
          
            
            for (e=0; e<GOSFS_NUM_INDIRECT_PTR_PER_BLOCK; e++)
            {
                memcpy(&blockIndirect, (void*) p_buff->data + (e*sizeof(ulong_t)), sizeof(ulong_t));
                if (blockIndirect!=0)
                {
                    Debug("GOSFS_Delete: found block %ld to delete\n",blockIndirect);
                    Clear_Bit(p_instance->superblock.bitSet, blockIndirect);
                    
                }
            }
            
            rc = Release_FS_Buffer(p_instance->buffercache, p_buff);
            if (rc < 0) {
                Debug("Failed to get buffer for indirect block %ld\n", blockNum);
                goto finish;
            }
            p_buff = 0;          
            
            Clear_Bit(p_instance->superblock.bitSet, blockNum);
        }
    }
	
	// free 2Xindirect blocks
	for (i=0;i<GOSFS_NUM_2X_INDIRECT_BLOCKS;i++)
	{
		blockNum = pInode->blockList[GOSFS_NUM_DIRECT_BLOCKS+GOSFS_NUM_INDIRECT_BLOCKS+i];
        if (blockNum !=0)
        {
            Debug("GOSFS_Delete: found indirect block %ld --> freeing\n",blockNum);
				
            rc = Get_FS_Buffer(p_instance->buffercache, blockNum, &p_buff);
            if (rc < 0) {
                Debug("Failed to get buffer for 2x indirect block %ld\n", blockNum);
                goto finish;
            }

            for (e = 0; e < GOSFS_NUM_INDIRECT_PTR_PER_BLOCK; e++) {
                memcpy(&block2Indirect, p_buff->data + e * sizeof(ulong_t), sizeof(ulong_t));
                if (block2Indirect != 0)
                    Clear_Bit(p_instance->superblock.bitSet, block2Indirect);
            }

            Release_FS_Buffer(p_instance->buffercache, p_buff);
            p_buff = NULL;

            Clear_Bit(p_instance->superblock.bitSet, blockNum);
			
		}
		
	}
    // remove directory-entry from parent directory
    rc = RemoveDirEntryFromInode(p_instance, parentInodeNum, inodeNum);
   
finish:
    if (p_buff!=0)  Release_FS_Buffer(((struct GOSFS_Instance*)mountPoint->fsData)->buffercache, p_buff);
    Mutex_Unlock(&p_instance->lock);
    return rc;
}

/*
 * Get metadata (size, permissions, etc.) of file named by given path.
 * 获取由给定路径命名的文件的元数据（大小，权限等）
 */
static int GOSFS_Stat(struct Mount_Point *mountPoint, const char *path, struct VFS_File_Stat *stat)
{        
    int rc=0;
    ulong_t inode=0;
    struct GOSFS_Instance *p_instance = (struct GOSFS_Instance*)mountPoint->fsData;
    Mutex_Lock(&p_instance->lock);    

    if (strcmp(path,"/")==0) 
    {
        inode=0;
    }
    else
    {
        rc=FindInodeByPath((struct GOSFS_Instance*)mountPoint->fsData, path, &inode);
        if (rc<0) 
        {
            rc=ENOTFOUND;
            goto finish;
        }
    }
    
    stat->size=((struct GOSFS_Instance*)mountPoint->fsData)->superblock.inodes[inode].size;
    
    if (!(((struct GOSFS_Instance*)mountPoint->fsData)->superblock.inodes[inode].flags & GOSFS_DIRENTRY_USED))
    {
        rc = ENOTFOUND;
        goto finish;
    }
    
    if (((struct GOSFS_Instance*)mountPoint->fsData)->superblock.inodes[inode].flags & GOSFS_DIRENTRY_ISDIRECTORY)
            stat->isDirectory=1;
    else 
            stat->isDirectory=0;

    memcpy (stat->acls, ((struct GOSFS_Instance*)mountPoint->fsData)->superblock.inodes[inode].acl,
            sizeof(struct VFS_ACL_Entry) * VFS_MAX_ACL_ENTRIES);
    
finish:
    Mutex_Unlock(&p_instance->lock);
    return rc;
}


/*
 * Synchronize the filesystem data with the disk
 * (i.e., flush out all buffered filesystem data).
 * 将文件系统数据与磁盘同步 
 */
static int GOSFS_Sync(struct Mount_Point *mountPoint)
{
    //TODO("GeekOS filesystem sync operation");
    int rc=0;
    struct GOSFS_Instance *p_instance = (struct GOSFS_Instance *)mountPoint->fsData;
    struct FS_Buffer    *p_buff=0;
    Mutex_Lock(&p_instance->lock);
    
    rc = WriteSuperblock(p_instance);
    
finish:
    if (p_buff!=0)  Release_FS_Buffer(p_instance->buffercache, p_buff);
    Mutex_Unlock(&p_instance->lock);
    return rc;
}

static struct Mount_Point_Ops s_gosfsMountPointOps = {
    &GOSFS_Open,
    &GOSFS_Create_Directory,
    &GOSFS_Open_Directory,
    &GOSFS_Stat,
    &GOSFS_Sync,
    &GOSFS_Delete,

};

/*
 *将挂载区域blockDev进行GOSFS格式化
 */
static int GOSFS_Format(struct Block_Device *blockDev)
{
    //TODO("GeekOS filesystem format operation");
    struct FS_Buffer_Cache        *gosfs_cache=0;
    struct FS_Buffer            *p_buff=0;
    struct GOSFS_Superblock        *superblock=0;
    int rc=0;
    uchar_t *bitset=0;
    ulong_t bcopied=0;
    ulong_t i;
        
    
    int numBlocks = Get_Num_Blocks(blockDev)/GOSFS_SECTORS_PER_FS_BLOCK;
    
    ulong_t byteCountSuperblock = sizeof(struct GOSFS_Superblock) + FIND_NUM_BYTES(numBlocks);
    // 需要块的数量
    ulong_t blockCountSuperblock = FindNumBlocks(byteCountSuperblock);
    gosfs_cache = Create_FS_Buffer_Cache(blockDev, GOSFS_FS_BLOCK_SIZE);
    
    Debug("GOSFS_Format: About to create root-directory\n");
   
    // 建立超级块 
    superblock = Malloc(byteCountSuperblock);
    superblock->magic = GOSFS_MAGIC;
    superblock->size = numBlocks;
    superblock->supersize = byteCountSuperblock;

    for (i = 0; i < blockCountSuperblock; i++) Set_Bit(superblock->bitSet, i);
    
    // 初始化根目录 inode 0
    superblock->inodes[0].size = 1;
    superblock->inodes[0].flags = GOSFS_DIRENTRY_ISDIRECTORY | GOSFS_DIRENTRY_USED;
    memset (superblock->inodes[0].acl, '\0', sizeof (struct VFS_ACL_Entry) * VFS_MAX_ACL_ENTRIES);
    superblock->inodes[0].blockList[0] = blockCountSuperblock;

    // 标记根目录块为已使用
    Set_Bit(superblock->bitSet, blockCountSuperblock);

    rc = Get_FS_Buffer(gosfs_cache, blockCountSuperblock, &p_buff);
    if (rc == 0) {
        Debug("GOSFS_Format: CreateFirstDirectoryBlock for root directory\n");
        CreateFirstDirectoryBlock(0, p_buff, "/");
        Debug("GOSFS_Format: done CreateFirstDirectoryBlock for root directory\n");
        Modify_FS_Buffer(gosfs_cache, p_buff);
        Release_FS_Buffer(gosfs_cache, p_buff);
    } else {
        Debug("GOSFS_Format: Get_FS_Buffer failed\n");
        rc = -1;
        goto finish;
    }

    // 将超级块写入硬盘 
    for (i=0; i<blockCountSuperblock; i++)
    {
        rc = Get_FS_Buffer(gosfs_cache, i, &p_buff) ;
        if ((byteCountSuperblock-bcopied) < GOSFS_FS_BLOCK_SIZE)
        {
            memcpy(
                p_buff->data, 
                ((void*)superblock)+bcopied, 
                byteCountSuperblock-bcopied
            );
            bcopied = bcopied+(byteCountSuperblock-bcopied);
        }
        else
        {
            memcpy(
                p_buff->data, 
                ((void*)superblock)+bcopied, 
                GOSFS_FS_BLOCK_SIZE
            );
            bcopied = bcopied + GOSFS_FS_BLOCK_SIZE;
        }
        
        Modify_FS_Buffer(gosfs_cache, p_buff);
        Release_FS_Buffer(gosfs_cache, p_buff);
        // Debug("GOSFS_Format:writing block %ld\n",i);
    
    }

finish:
    if (gosfs_cache != 0) Destroy_FS_Buffer_Cache(gosfs_cache);
    return rc;
}

/*
 *挂载mountPoint指向的文件系统
 */
static int GOSFS_Mount(struct Mount_Point *mountPoint)
{
    Print("GOSFS_Mount: GeekOS filesystem mount operation\n");
    struct FS_Buffer_Cache        *gosfs_cache = 0;
    struct FS_Buffer            *p_buff = 0;
    struct GOSFS_Superblock        *superblock = 0;
    struct GOSFS_Instance        *instance;
    ulong_t numBlocks, numBytes,bwritten,i;
    int   rc;
    mountPoint->ops = &s_gosfsMountPointOps;
    gosfs_cache = Create_FS_Buffer_Cache(mountPoint->dev, GOSFS_FS_BLOCK_SIZE);
   
    // 超级块的第一个块
    rc = Get_FS_Buffer(gosfs_cache, 0, &p_buff) ;
    superblock = (struct GOSFS_Superblock*) p_buff->data;

    Print("GOSFS_Mount: found magic:%lx\n",superblock->magic);//魔数检查
    if (superblock->magic!=GOSFS_MAGIC)
    {
        Print("GOSFS_Mount: ERROR does not seem to be a GOSFS filesystem, try format first\n");
        rc = -1;
        goto finish;
    }
    Print("GOSFS_Mount: superblock size: %ld Byte\n",superblock->supersize);
    Print("GOSFS_Mount: number of blocks of whole fs %ld bocks\n",superblock->size);

    numBytes = superblock->supersize;
    numBlocks = (numBytes / GOSFS_FS_BLOCK_SIZE)+1;
        Print("GOSFS_Mount: superblock spreads %ld blocks\n",numBlocks);
    // 创建文件系统实例
    int sizeofInstance=sizeof(struct GOSFS_Instance) + FIND_NUM_BYTES(superblock->size);
    Debug("GOSFS_Mount: size of instance %d bytes\n",sizeofInstance);
    rc = Release_FS_Buffer(gosfs_cache, p_buff);
    if (rc<0)
    {
        Print("GOSFS_Mount: Unable to release fs_buffer\n");
        rc = -1;
        goto finish;
    }
    p_buff = 0;
    
    // 分配足够的内存
    instance = Malloc(sizeofInstance);
    if (instance==0)
    {
        Print("GOSFS_Mount: Malloc failed to allocate memory\n");
        rc=ENOMEM;
        goto finish;
    }
    // 初始化mutex
    Mutex_Init(&instance->lock);
    instance->buffercache = gosfs_cache;
    bwritten = 0;
    superblock = &(instance->superblock);
    for (i=0; i<numBlocks; i++)
    {
        rc = Get_FS_Buffer(gosfs_cache, i, &p_buff) ;

        if ((numBytes-bwritten)<GOSFS_FS_BLOCK_SIZE)
        {
            memcpy(((void*)superblock)+bwritten, p_buff->data, numBytes-bwritten);
            bwritten=bwritten+(numBytes-bwritten);
        }
        else
        {
            memcpy(((void*)superblock)+bwritten, p_buff->data, GOSFS_FS_BLOCK_SIZE);
            bwritten=bwritten+GOSFS_FS_BLOCK_SIZE;
        }
        
        rc = Release_FS_Buffer(gosfs_cache, p_buff);
        if (rc<0)
        {
            Print("GOSFS_Mount: Unable to release fs_buffer\n");
            rc = -1;
            goto finish;
        }
        p_buff = 0;
    }
    mountPoint->fsData = instance;
    rc = 0;
finish:
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

