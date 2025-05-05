/*
 * GeekOS file system
 * Copyright (c) 2003, Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 * Copyright (c) 2004, David H. Hovemeyer <daveho@cs.umd.edu>
 * $Revision: 1.19 $
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#ifndef GEEKOS_GOSFS_H
#define GEEKOS_GOSFS_H

#include <geekos/blockdev.h>
#include <geekos/fileio.h>
#include <geekos/vfs.h>
#include <geekos/synch.h>

/* 
 * Number of disk sectors per filesystem block. 
 * 一个文件系统块包含的扇区数 
 */
#define GOSFS_SECTORS_PER_FS_BLOCK  8

/* 
 * Size of a filesystem block in bytes. 
 * 扇区大小512 512 * 8 = 4096 字节(Byte)
 */
#define GOSFS_FS_BLOCK_SIZE        (GOSFS_SECTORS_PER_FS_BLOCK*SECTOR_SIZE)

/* Flags bits for directory entries. */
#define GOSFS_DIRENTRY_USED            0x01    /* Directory entry is in use. */
#define GOSFS_DIRENTRY_ISDIRECTORY     0x02    /* Directory entry refers to a subdirectory. */
#define GOSFS_DIRENTRY_SETUID          0x04    /* File executes using uid of file owner. */

#define GOSFS_FILENAME_MAX          127     /* Maximum filename length. */

#define GOSFS_NUM_DIRECT_BLOCKS         8   /* Number of direct blocks in dir entry. */
#define GOSFS_NUM_INDIRECT_BLOCKS       1   /* Number of singly-indirect blocks in dir entry. */
#define GOSFS_NUM_2X_INDIRECT_BLOCKS    1   /* Number of doubly-indirect blocks in dir entry. */

/* Number of block pointers that can be stored in a single filesystem block. */
#define GOSFS_NUM_PTRS_PER_BLOCK     (GOSFS_FS_BLOCK_SIZE / sizeof(ulong_t))

/* Total number of block pointers in a directory entry. */
#define GOSFS_NUM_BLOCK_PTRS \
    (GOSFS_NUM_DIRECT_BLOCKS + GOSFS_NUM_INDIRECT_BLOCKS + GOSFS_NUM_2X_INDIRECT_BLOCKS)

/*
 * A directory entry.
 * It is strongly recommended that you don't change this struct.
 */
 struct GOSFS_Dir_Entry {
    ulong_t size;           // size of file in byte for files / or number of dir-entries for directories
    ulong_t flags;          /* Flags: used, isdirectory, setuid. */
    ulong_t blockList[GOSFS_NUM_BLOCK_PTRS];    /* Pointers to direct, indirect, and doubly-indirect blocks. */
    struct VFS_ACL_Entry acl[VFS_MAX_ACL_ENTRIES];/* List of ACL entries; first is for the file's owner. */    
};
/* 
 * Number of directory entries that fit in a filesystem block. 
 * 一个文件系统块中包含的目录项数
 */
#define GOSFS_DIR_ENTRIES_PER_BLOCK    (GOSFS_FS_BLOCK_SIZE / sizeof(struct GOSFS_Directory))

void Init_GOSFS(void);

#endif
