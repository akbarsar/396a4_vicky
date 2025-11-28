/*
 *------------
 * This code is provided solely for the personal and private use of
 * students taking the CSC369H5 course at the University of Toronto.
 * Copying for purposes other than this use is expressly prohibited.
 * All forms of distribution of this code, whether as given or with
 * any changes, are expressly prohibited.
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2025 MCS @ UTM
 * -------------
 */

/**
 * @file ext2fsal_mkdir.c
 * @brief Implementation of the ext2 mkdir operation.
 *
 * Creates a new directory in the ext2 filesystem with proper synchronization
 * for concurrent access.
 */

#include "ext2fsal.h"
#include "e2fs.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/**
 * Create a new directory in the ext2 filesystem.
 *
 * This function:
 * 1. Validates the path and extracts parent directory and name
 * 2. Checks that parent exists and is a directory
 * 3. Verifies the target name doesn't already exist
 * 4. Allocates a new inode and block for the directory
 * 5. Initializes the directory with "." and ".." entries
 * 6. Adds an entry in the parent directory
 * 7. Updates parent's link count
 *
 * Error handling:
 * - ENOENT: Invalid path, parent doesn't exist, or parent is not a directory
 * - EEXIST: Target name already exists
 * - ENOSPC: No free inodes or blocks available
 * - ENAMETOOLONG: Target name exceeds EXT2_NAME_LEN
 *
 * @param path Absolute path of the directory to create
 * @return     0 on success, negative errno on error
 */
int32_t ext2_fsal_mkdir(const char *path)
{
    /* Validate input path */
    if (path == NULL) {
        return -ENOENT;
    }
    if (path[0] != '/') {
        return -ENOENT;  /* Must be absolute path */
    }

    /* Parse path into parent directory and target name */
    char parent_path[PATH_MAX];
    char name[EXT2_NAME_LEN];
    int rc = split_parent_name(path, parent_path, name);
    if (rc != 0) {
        return (rc == -ENAMETOOLONG) ? -ENAMETOOLONG : -ENOENT;
    }

    /* Lookup parent directory inode */
    int parent_ino = path_lookup(parent_path);
    if (parent_ino < 0) {
        return -ENOENT;
    }

    /* Verify parent is a directory */
    struct ext2_inode *parent_inode = get_inode(parent_ino);
    if (!S_ISDIR(parent_inode->i_mode)) {
        return -ENOENT;
    }

    /* Check if target name already exists */
    int existing = find_dir_entry(parent_inode, name);
    if (existing >= 0) {
        struct ext2_inode *exist_inode = get_inode(existing);
        
        /* If existing entry is a directory, return EEXIST */
        if (S_ISDIR(exist_inode->i_mode)) {
            return -EEXIST;
        }
        
        /* Handle trailing slash case: /foo/bar/blah/ where blah is a file */
        size_t plen = strlen(path);
        if (plen > 1 && path[plen - 1] == '/') {
            return -ENOENT;
        }
        
        /* Entry exists as non-directory */
        return -EEXIST;
    } else if (existing != -ENOENT) {
        return existing;  /* Propagate unexpected error */
    }

    /* Allocate inode for the new directory */
    int new_ino = alloc_inode();
    if (new_ino < 0) {
        return -ENOSPC;
    }

    /* Allocate data block for directory contents */
    int new_block = alloc_block();
    if (new_block < 0) {
        free_inode(new_ino);  /* Rollback inode allocation */
        return -ENOSPC;
    }

    /* Initialize new directory inode */
    struct ext2_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = EXT2_S_IFDIR | 0755;         /* Directory with rwxr-xr-x permissions */
    new_inode.i_size = EXT2_BLOCK_SIZE;              /* One block for directory entries */
    new_inode.i_links_count = 2;                     /* "." entry + parent's entry */
    new_inode.i_blocks = EXT2_BLOCK_SIZE / 512;      /* i_blocks in 512-byte sectors */

    /* Set creation/modification time */
    new_inode.i_ctime = (uint32_t)time(NULL);
    new_inode.i_mtime = new_inode.i_ctime;
    new_inode.i_atime = new_inode.i_ctime;

    /* Initialize block pointers */
    for (int i = 0; i < TOTAL_POINTERS; i++) {
        new_inode.i_block[i] = 0;
    }
    new_inode.i_block[0] = new_block;

    /* Write inode to disk */
    write_inode(new_ino, &new_inode);

    /* Construct directory block with "." and ".." entries */
    uint8_t block_buf[EXT2_BLOCK_SIZE];
    memset(block_buf, 0, EXT2_BLOCK_SIZE);

    /* Create "." entry (points to self) */
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *) block_buf;
    dot->inode = new_ino;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    memcpy(dot->name, ".", 1);
    dot->rec_len = dir_entry_rec_len(1);

    /* Create ".." entry (points to parent) */
    struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *)((uint8_t *)dot + dot->rec_len);
    dotdot->inode = parent_ino;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    memcpy(dotdot->name, "..", 2);
    dotdot->rec_len = EXT2_BLOCK_SIZE - dot->rec_len;  /* Rest of block */

    /* Write directory block to disk */
    write_block(new_block, (char *)block_buf);

    /* Add entry for new directory in parent */
    int addrc = add_dir_entry(parent_ino, name, new_ino, EXT2_FT_DIR);
    if (addrc < 0) {
        /* Cleanup on failure */
        free_block(new_block);
        free_inode(new_ino);
        return addrc;
    }

    /* Increment parent's link count (subdirectory ".." links to parent) */
    pthread_mutex_lock(&inode_locks[parent_ino - 1]);
    struct ext2_inode *p = get_inode(parent_ino);
    p->i_links_count += 1;
    pthread_mutex_unlock(&inode_locks[parent_ino - 1]);

    return 0;
}
