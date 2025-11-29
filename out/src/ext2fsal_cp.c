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
 * @file ext2fsal_cp.c
 * @brief Implementation of the ext2 cp (copy) operation.
 *
 * Copies a file from the host filesystem into the ext2 filesystem.
 * Supports overwriting existing files and copying into directories.
 */

#include "ext2fsal.h"
#include "e2fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h> 
#include <sys/stat.h>
#include <sys/types.h>

/**
 * Extract basename from a path (last component after final '/').
 */
static const char* get_basename(const char *path) {
    const char *base = strrchr(path, '/');
    return (base) ? base + 1 : path;
}

/**
 * Open and validate source file for copying.
 * @return file descriptor on success, -1 on error (sets *err)
 */
static int open_source_file(const char *src, off_t *filesize, int *err) {
    int fd = open(src, O_RDONLY);
    if (fd < 0) {
        *err = -ENOENT;
        return -1;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *err = -ENOENT;
        return -1;
    }
    
    *filesize = st.st_size;
    return fd;
}

/**
 * Parse destination path and resolve parent directory.
 * Handles trailing slashes by treating them as directory paths.
 * @return 0 on success, negative errno on error
 */
static int resolve_destination(const char *dst, const char *src,
                               int *parent_ino, char *name) {
    char parent_path[PATH_MAX];
    char tmp_dst[PATH_MAX];
    strncpy(tmp_dst, dst, PATH_MAX - 1);
    tmp_dst[PATH_MAX - 1] = '\0';

    /* Trailing slash: treat as directory, use source basename */
    if (strlen(tmp_dst) > 1 && tmp_dst[strlen(tmp_dst) - 1] == '/') {
        int dir_ino = path_lookup(tmp_dst);
        if (dir_ino < 0) return -ENOENT;
        
        struct ext2_inode *dir = get_inode(dir_ino);
        if (!S_ISDIR(dir->i_mode)) return -ENOTDIR;
        
        const char *base = get_basename(src);
        if (strlen(base) >= EXT2_NAME_LEN) return -ENAMETOOLONG;
        
        strncpy(name, base, EXT2_NAME_LEN);
        *parent_ino = dir_ino;
        return 0;
    }

    /* Normal path: split into parent and name */
    int rc = split_parent_name(dst, parent_path, name);
    if (rc != 0) return (rc == -ENAMETOOLONG) ? -ENAMETOOLONG : -EINVAL;

    int pino = path_lookup(parent_path);
    if (pino < 0) return -ENOENT;
    
    struct ext2_inode *parent = get_inode(pino);
    if (!S_ISDIR(parent->i_mode)) return -ENOTDIR;
    
    *parent_ino = pino;
    return 0;
}

/**
 * Check if target exists and handle accordingly.
 * Updates parent_ino/name if target is a directory (copy into it).
 * @return 0 on success, negative errno on error
 */
static int handle_existing_target(const char *src, int *parent_ino, char *name,
                                  int *target_ino, int *overwrite) {
    struct ext2_inode *parent = get_inode(*parent_ino);
    int existing = find_dir_entry(parent, name);
    
    *target_ino = -1;
    *overwrite = 0;

    if (existing == -ENOENT) return 0;  /* Doesn't exist, will create */
    if (existing < 0) return existing;   /* Other error */

    struct ext2_inode *target = get_inode(existing);
    uint16_t type = target->i_mode & 0xF000;

    /* Symlink: cannot overwrite */
    if (type == EXT2_S_IFLNK) return -EEXIST;

    /* Directory: copy file into it with source basename */
    if (S_ISDIR(target->i_mode)) {
        *parent_ino = existing;
        parent = get_inode(existing);
        
        const char *base = get_basename(src);
        if (strlen(base) >= EXT2_NAME_LEN) return -ENAMETOOLONG;
        strncpy(name, base, EXT2_NAME_LEN);
        
        /* Check for existing file in target directory */
        int inner = find_dir_entry(parent, name);
        if (inner >= 0) {
            struct ext2_inode *inner_node = get_inode(inner);
            uint16_t inner_type = inner_node->i_mode & 0xF000;
            if (inner_type == EXT2_S_IFLNK || S_ISDIR(inner_node->i_mode)) {
                return -EEXIST;
            }
            *target_ino = inner;
            *overwrite = 1;
        }
        return 0;
    }

    /* Regular file: overwrite */
    if (type == EXT2_S_IFREG) {
        *target_ino = existing;
        *overwrite = 1;
        return 0;
    }

    return -EEXIST;
}

/**
 * Copy a file from the host filesystem to the ext2 filesystem.
 *
 * @param src Path to source file on host filesystem
 * @param dst Path to destination in ext2 filesystem
 * @return    0 on success, negative errno on error
 */
int32_t ext2_fsal_cp(const char *src, const char *dst)
{
    if (src == NULL || dst == NULL) return -ENOENT;

    /* Step 1: Open and validate source file */
    off_t filesize;
    int err;
    int src_fd = open_source_file(src, &filesize, &err);
    if (src_fd < 0) return err;

    /* Step 2: Parse destination path */
    int parent_ino;
    char name[EXT2_NAME_LEN];
    
    int rc = resolve_destination(dst, src, &parent_ino, name);
    if (rc != 0) {
        close(src_fd);
        return rc;
    }

    /* Step 3: Handle existing target */
    int target_ino, overwrite;
    rc = handle_existing_target(src, &parent_ino, name, &target_ino, &overwrite);
    if (rc != 0) {
        close(src_fd);
        return rc;
    }

    /* Step 4: Allocate or reuse inode */
    int use_ino;
    if (overwrite && target_ino > 0) {
        free_inode_blocks_locked(target_ino);
        use_ino = target_ino;
    } else {
        use_ino = alloc_inode();
        if (use_ino < 0) {
            close(src_fd);
            return -ENOSPC;
        }
    }

    /* Step 5: Initialize inode and write file data */
    struct ext2_inode new_inode;
    init_file_inode(&new_inode);

    if (lseek(src_fd, 0, SEEK_SET) < 0) {
        close(src_fd);
        if (!overwrite) free_inode(use_ino);
        return -EIO;
    }

    int wres = write_data_into_inode(src_fd, &new_inode, filesize);
    if (wres != 0) {
        close(src_fd);
        if (!overwrite) {
            free_inode_blocks_locked(use_ino);
            free_inode(use_ino);
        }
        return wres;
    }

    /* Step 6: Write inode to disk */
    write_inode(use_ino, &new_inode);

    /* Step 7: Add directory entry for new file */
    if (!overwrite) {
        int addc = add_dir_entry(parent_ino, name, use_ino, EXT2_FT_REG_FILE);
        if (addc < 0) {
            free_inode_blocks_locked(use_ino);
            free_inode(use_ino);
            close(src_fd);
            return -ENOSPC;
        }
    }

    close(src_fd);
    return 0;
}
