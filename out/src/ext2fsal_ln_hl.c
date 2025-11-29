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
 * @file ext2fsal_ln_hl.c
 * @brief Implementation of the ext2 hard link operation.
 *
 * Creates a hard link to an existing file in the ext2 filesystem.
 * Hard links share the same inode, so changes to one are reflected in all.
 */

#include "ext2fsal.h"
#include "e2fs.h"
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * Create a hard link to an existing file.
 *
 * This function:
 * 1. Validates both source and destination paths
 * 2. Verifies source exists and is not a directory
 * 3. Creates a new directory entry pointing to source's inode
 * 4. Increments the source inode's link count
 *
 * Note: Unlike regular ln, this implementation returns EISDIR if the
 * destination is an existing directory, rather than creating a link inside it.
 *
 * Error handling:
 * - ENOENT: Source or destination path is invalid
 * - EISDIR: Source is a directory, or destination exists as a directory
 * - EEXIST: Destination name already exists
 *
 * @param src Absolute path to the source file
 * @param dst Absolute path for the new hard link
 * @return    0 on success, negative errno on error
 */
int32_t ext2_fsal_ln_hl(const char *src, const char *dst)
{
    /* Validate input paths */
    if (!src || !dst) {
        return ENOENT;
    }
    if (src[0] != '/' || dst[0] != '/') {
        return ENOENT;
    }

    /* Lookup source file */
    int src_ino = path_lookup(src);
    if (src_ino < 0) {
        return ENOENT;
    }

    struct ext2_inode *src_inode = get_inode(src_ino);

    /* Cannot create hard links to directories */
    if (S_ISDIR(src_inode->i_mode)) {
        return EISDIR;
    }

    /* Parse destination path */
    char parent[PATH_MAX], name[EXT2_NAME_LEN];
    if (split_parent_name(dst, parent, name) < 0) {
        return ENOENT;
    }

    /* Lookup destination parent directory */
    int parent_ino = path_lookup(parent);
    if (parent_ino < 0) {
        return ENOENT;
    }

    struct ext2_inode *p_inode = get_inode(parent_ino);
    if (!S_ISDIR(p_inode->i_mode)) {
        return ENOENT;
    }

    /* Check if destination name already exists */
    int exists_ino = find_dir_entry(p_inode, name);
    if (exists_ino >= 0) {
        struct ext2_inode *exist_inode = get_inode(exists_ino);
        if (S_ISDIR(exist_inode->i_mode)) {
            return EISDIR;
        }
        return EEXIST;
    }

    /* Determine file type for directory entry */
    uint8_t file_type = EXT2_FT_REG_FILE;
    if (S_ISLNK(src_inode->i_mode)) {
        file_type = EXT2_FT_SYMLINK;
    }

    /* Add new directory entry pointing to source inode */
    int r = add_dir_entry(parent_ino, name, src_ino, file_type);
    if (r < 0) {
        return r;
    }

    /* Increment source inode's link count */
    pthread_mutex_lock(&inode_locks[src_ino - 1]);
    src_inode->i_links_count++;
    pthread_mutex_unlock(&inode_locks[src_ino - 1]);

    return 0;
}
