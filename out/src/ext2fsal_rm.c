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
 * @file ext2fsal_rm.c
 * @brief Implementation of the ext2 rm (remove) operation.
 *
 * Removes a file or symbolic link from the ext2 filesystem.
 * Handles hard links correctly by decrementing link count.
 */

#include "ext2fsal.h"
#include "e2fs.h"
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * Remove a file or symbolic link from the ext2 filesystem.
 *
 * This function:
 * 1. Validates the path and locates the target
 * 2. Removes the directory entry from the parent (by name, not inode)
 * 3. Decrements the target's link count
 * 4. If link count reaches zero, frees the inode and its data blocks
 *
 * Important: Directory entries are matched by name, not inode number,
 * to correctly handle hard links where multiple entries share an inode.
 *
 * Error handling:
 * - ENOENT: Invalid path, file doesn't exist, or trailing slash on file
 * - EISDIR: Target is a directory (cannot remove directories with rm)
 *
 * @param path Absolute path of the file to remove
 * @return     0 on success, negative errno on error
 */
int32_t ext2_fsal_rm(const char *path)
{
    /* Validate input path */
    if (!path) {
        return ENOENT;
    }
    if (path[0] != '/') {
        return ENOENT;
    }

    /* Check for trailing slash */
    size_t plen = strlen(path);
    int has_trailing_slash = (plen > 1 && path[plen - 1] == '/');

    /* Parse path into parent and target name */
    char parent_path[PATH_MAX];
    char name[EXT2_NAME_LEN];
    if (split_parent_name(path, parent_path, name) < 0) {
        return ENOENT;
    }

    /* Lookup parent directory */
    int parent_ino = path_lookup(parent_path);
    if (parent_ino < 0) {
        return ENOENT;
    }

    struct ext2_inode *parent_inode = get_inode(parent_ino);
    if (!S_ISDIR(parent_inode->i_mode)) {
        return ENOENT;
    }

    /* Find target file in parent directory */
    int target_ino = find_dir_entry(parent_inode, name);
    if (target_ino < 0) {
        return ENOENT;
    }

    struct ext2_inode *target_inode = get_inode(target_ino);

    /* Cannot remove directories */
    if (S_ISDIR(target_inode->i_mode)) {
        return EISDIR;
    }

    /* Trailing slash on non-directory is an error */
    if (has_trailing_slash) {
        return ENOENT;
    }

    size_t name_len = strlen(name);

    /* Lock parent for directory modification */
    pthread_mutex_lock(&inode_locks[parent_ino - 1]);

    /* Remove directory entry by name (not inode, for hard link correctness) */
    int found = 0;
    for (int i = 0; i < DIRECT_POINTERS && !found; i++) {
        int block_num = parent_inode->i_block[i];
        if (block_num == 0) {
            continue;
        }

        pthread_mutex_lock(&block_locks[block_num]);
        
        struct ext2_dir_entry *entry = (struct ext2_dir_entry *)get_block(block_num);
        struct ext2_dir_entry *prev = NULL;
        uint8_t *blk_start = (uint8_t*)get_block(block_num);
        uint8_t *blk_end = blk_start + EXT2_BLOCK_SIZE;

        /* Search for entry with matching name */
        while ((uint8_t*)entry < blk_end && entry->rec_len > 0) {
            if (entry->inode != 0 &&
                (size_t)entry->name_len == name_len &&
                memcmp(entry->name, name, name_len) == 0) {
                
                found = 1;
                
                if (prev) {
                    /* Merge space into previous entry */
                    prev->rec_len += entry->rec_len;
                } else {
                    /* First entry in block: zero inode, keep rec_len */
                    entry->inode = 0;
                }
                
                pthread_mutex_unlock(&block_locks[block_num]);
                break;
            }
            prev = entry;
            entry = next_dir_entry(entry);
        }
        
        if (!found) {
            pthread_mutex_unlock(&block_locks[block_num]);
        }
    }

    pthread_mutex_unlock(&inode_locks[parent_ino - 1]);

    if (!found) {
        return ENOENT;
    }

    /* Update target inode: decrement link count */
    pthread_mutex_lock(&inode_locks[target_ino - 1]);

    target_inode->i_links_count--;

    if (target_inode->i_links_count == 0) {
        /* No more links - free the file */
        
        /* Set deletion time */
        target_inode->i_dtime = (unsigned int)time(NULL);

        /* Free direct blocks */
        for (int i = 0; i < DIRECT_POINTERS; i++) {
            if (target_inode->i_block[i] != 0) {
                free_block(target_inode->i_block[i]);
                target_inode->i_block[i] = 0;
            }
        }

        /* Free indirect block and its referenced blocks */
        if (target_inode->i_block[INDIRECT_INDEX] != 0) {
            int indirect_blk = target_inode->i_block[INDIRECT_INDEX];
            uint32_t *ptrs = (uint32_t *) get_block(indirect_blk);
            int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
            
            for (int i = 0; i < per_block; i++) {
                if (ptrs[i] != 0) {
                    free_block(ptrs[i]);
                }
            }
            free_block(indirect_blk);
            target_inode->i_block[INDIRECT_INDEX] = 0;
        }

        target_inode->i_blocks = 0;
        target_inode->i_size = 0;

        pthread_mutex_unlock(&inode_locks[target_ino - 1]);

        /* Free the inode */
        free_inode(target_ino);
    } else {
        /* Other hard links still exist */
        pthread_mutex_unlock(&inode_locks[target_ino - 1]);
    }

    return 0;
}
