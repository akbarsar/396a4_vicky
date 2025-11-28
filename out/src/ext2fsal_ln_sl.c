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
 * @file ext2fsal_ln_sl.c
 * @brief Implementation of the ext2 symbolic link operation.
 *
 * Creates a symbolic link in the ext2 filesystem.
 * Unlike hard links, symlinks store the path to the target as data.
 */

#include "ext2fsal.h"
#include "e2fs.h"
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/**
 * Create a symbolic link pointing to a target path.
 *
 * This function:
 * 1. Validates the destination path (source doesn't need to exist)
 * 2. Allocates a new inode for the symlink
 * 3. Allocates a data block to store the target path
 * 4. Creates the symlink inode and directory entry
 *
 * Note: Per the assignment spec, we do NOT use fast symlinks.
 * The target path is always stored in a data block.
 *
 * Important: Unlike hard links, symlinks to non-existent paths are allowed.
 * The target path is simply stored as the symlink's content.
 *
 * Error handling:
 * - ENOENT: Destination parent path is invalid
 * - EISDIR: Destination name exists as a directory
 * - EEXIST: Destination name already exists
 * - ENOSPC: No free inodes or blocks
 *
 * @param src Target path (what the symlink points to)
 * @param dst Absolute path for the symlink
 * @return    0 on success, negative errno on error
 */
int32_t ext2_fsal_ln_sl(const char *src, const char *dst)
{
    /* Validate input paths */
    if (!src || !dst) {
        return -ENOENT;
    }
    if (dst[0] != '/') {
        return -ENOENT;  /* Destination must be absolute */
    }

    /* Note: Source path does NOT need to be valid or absolute!
     * Symlinks can point to non-existent or relative paths. */

    /* Parse destination path */
    char parent[PATH_MAX], name[EXT2_NAME_LEN];
    if (split_parent_name(dst, parent, name) < 0) {
        return -ENOENT;
    }

    /* Lookup destination parent directory */
    int parent_ino = path_lookup(parent);
    if (parent_ino < 0) {
        return -ENOENT;
    }

    struct ext2_inode *p_inode = get_inode(parent_ino);
    if (!S_ISDIR(p_inode->i_mode)) {
        return -ENOTDIR;
    }

    /* Check if destination name already exists */
    int exists = find_dir_entry(p_inode, name);
    if (exists >= 0) {
        struct ext2_inode *eino = get_inode(exists);
        if (S_ISDIR(eino->i_mode)) {
            return -EISDIR;
        }
        return -EEXIST;
    }

    /* Allocate inode for symlink */
    int new_ino = alloc_inode();
    if (new_ino < 0) {
        return -ENOSPC;
    }

    /* Allocate data block for target path (no fast symlinks per spec) */
    int new_block = alloc_block();
    if (new_block < 0) {
        free_inode(new_ino);
        return -ENOSPC;
    }

    /* Initialize symlink inode */
    struct ext2_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = EXT2_S_IFLNK | 0777;
    new_inode.i_links_count = 1;
    new_inode.i_size = strlen(src);
    new_inode.i_blocks = EXT2_BLOCK_SIZE / 512;
    new_inode.i_block[0] = new_block;
    new_inode.i_ctime = (unsigned int)time(NULL);
    new_inode.i_mtime = new_inode.i_ctime;
    new_inode.i_atime = new_inode.i_ctime;

    /* Write target path to data block */
    char *blk = get_block(new_block);
    memset(blk, 0, EXT2_BLOCK_SIZE);
    memcpy(blk, src, strlen(src));

    /* Write inode to disk */
    write_inode(new_ino, &new_inode);

    /* Add symlink entry to parent directory */
    int r = add_dir_entry(parent_ino, name, new_ino, EXT2_FT_SYMLINK);
    if (r < 0) {
        free_block(new_block);
        free_inode(new_ino);
        return r;
    }

    return 0;
}
