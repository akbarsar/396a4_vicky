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
 * Copy a file from the host filesystem to the ext2 filesystem.
 *
 * This function handles multiple cases:
 * 1. dst is an existing directory: copy file with source basename into it
 * 2. dst ends with '/': treat as directory path
 * 3. dst is an existing file: overwrite it
 * 4. dst is an existing symlink: return EEXIST
 * 5. dst doesn't exist: create new file
 *
 * Error handling:
 * - ENOENT: Source file doesn't exist, invalid destination path
 * - EEXIST: Destination is an existing symlink
 * - ENOSPC: No free inodes or blocks
 * - ENAMETOOLONG: Target name exceeds EXT2_NAME_LEN
 * - EIO: Error reading source file
 *
 * @param src Path to source file on host filesystem
 * @param dst Path to destination in ext2 filesystem
 * @return    0 on success, negative errno on error
 */
int32_t ext2_fsal_cp(const char *src, const char *dst)
{
    if (src == NULL || dst == NULL) {
        return -ENOENT;
    }

    /* Open and validate source file */
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        return -ENOENT;
    }
    
    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        close(src_fd);
        return -ENOENT;
    }
    
    /* Only copy regular files */
    if (!S_ISREG(st.st_mode)) {
        close(src_fd);
        return -ENOENT;
    }
    off_t filesize = st.st_size;

    /* Parse destination path */
    char parent_path[PATH_MAX];
    char name[EXT2_NAME_LEN];
    int rc;

    char tmp_dst[PATH_MAX];
    strncpy(tmp_dst, dst, PATH_MAX);
    tmp_dst[PATH_MAX - 1] = '\0';
    
    /* Handle trailing slash: treat as directory */
    if (strlen(tmp_dst) > 1 && tmp_dst[strlen(tmp_dst) - 1] == '/') {
        int dir_ino = path_lookup(tmp_dst);
        if (dir_ino < 0) {
            close(src_fd);
            return -ENOENT;
        }
        
        struct ext2_inode *dir_inode = get_inode(dir_ino);
        if (!S_ISDIR(dir_inode->i_mode)) {
            close(src_fd);
            return -ENOTDIR;
        }
        
        /* Use source basename as target name */
        snprintf(parent_path, PATH_MAX, "%s", tmp_dst);
        const char *base = strrchr(src, '/');
        if (base) base++;
        else base = src;
        
        if (strlen(base) >= EXT2_NAME_LEN) {
            close(src_fd);
            return -ENAMETOOLONG;
        }
        strncpy(name, base, EXT2_NAME_LEN);
    } else {
        /* Split path into parent and name */
        rc = split_parent_name(dst, parent_path, name);
        if (rc != 0) {
            close(src_fd);
            return (rc == -ENAMETOOLONG ? -ENAMETOOLONG : -EINVAL);
        }
    }

    /* Lookup parent directory */
    int parent_ino = path_lookup(parent_path);
    if (parent_ino < 0) {
        close(src_fd);
        return -ENOENT;
    }
    
    struct ext2_inode *parent_inode = get_inode(parent_ino);
    if (!S_ISDIR(parent_inode->i_mode)) {
        close(src_fd);
        return -ENOTDIR;
    }

    /* Check if target exists */
    int existing = find_dir_entry(parent_inode, name);
    int target_ino = -1;
    int is_existing_file = 0;

    if (existing >= 0) {
        target_ino = existing;
        struct ext2_inode *t_inode = get_inode(target_ino);
        
        /* Symlink: return EEXIST */
        if ((t_inode->i_mode & 0xF000) == EXT2_S_IFLNK) {
            close(src_fd);
            return -EEXIST;
        }
        
        /* Directory: copy file into it */
        if (S_ISDIR(t_inode->i_mode)) {
            parent_ino = target_ino;
            parent_inode = get_inode(parent_ino);
            
            const char *base = strrchr(src, '/');
            if (base) base++;
            else base = src;
            
            if (strlen(base) >= EXT2_NAME_LEN) {
                close(src_fd);
                return -ENAMETOOLONG;
            }
            strncpy(name, base, EXT2_NAME_LEN);
            
            /* Check if file exists in target directory */
            int exists2 = find_dir_entry(parent_inode, name);
            if (exists2 >= 0) {
                struct ext2_inode *e2 = get_inode(exists2);
                if ((e2->i_mode & 0xF000) == EXT2_S_IFLNK) {
                    close(src_fd);
                    return -EEXIST;
                }
                if (S_ISDIR(e2->i_mode)) {
                    close(src_fd);
                    return -EEXIST;
                }
                target_ino = exists2;
                is_existing_file = 1;
            } else {
                target_ino = -1;
            }
        } else if ((t_inode->i_mode & 0xF000) == EXT2_S_IFREG) {
            /* Regular file: will overwrite */
            is_existing_file = 1;
        } else {
            close(src_fd);
            return -EEXIST;
        }
    } else if (existing == -ENOENT) {
        target_ino = -1;
    } else {
        close(src_fd);
        return existing;
    }

    /* Allocate or reuse inode */
    int use_ino = -1;
    if (is_existing_file && target_ino > 0) {
        /* Overwrite: free existing blocks, reuse inode */
        free_inode_blocks_locked(target_ino);
        use_ino = target_ino;
    } else {
        /* Create new inode */
        int new_ino = alloc_inode();
        if (new_ino < 0) {
            close(src_fd);
            return -ENOSPC;
        }
        use_ino = new_ino;
    }

    /* Initialize new inode */
    struct ext2_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = EXT2_S_IFREG | 0644;
    new_inode.i_size = 0;
    new_inode.i_links_count = 1;
    new_inode.i_dtime = 0;
    new_inode.i_ctime = (uint32_t)time(NULL);
    new_inode.i_mtime = new_inode.i_ctime;
    new_inode.i_atime = new_inode.i_ctime;

    /* Write file data */
    if (lseek(src_fd, 0, SEEK_SET) < 0) {
        close(src_fd);
        if (!is_existing_file && use_ino > 0) {
            free_inode(use_ino);
        }
        return -EIO;
    }

    int wres = write_data_into_inode(src_fd, &new_inode, filesize);
    if (wres != 0) {
        close(src_fd);
        /* Cleanup on error */
        if (!is_existing_file && use_ino > 0) {
            pthread_mutex_lock(&inode_locks[use_ino - 1]);
            for (int i = 0; i < DIRECT_POINTERS; i++) {
                if (new_inode.i_block[i] != 0) {
                    free_block(new_inode.i_block[i]);
                }
            }
            if (new_inode.i_block[INDIRECT_INDEX] != 0) {
                uint32_t *ptrs = (uint32_t *) get_block(new_inode.i_block[INDIRECT_INDEX]);
                int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
                for (int i = 0; i < per_block; i++) {
                    if (ptrs[i] != 0) {
                        free_block(ptrs[i]);
                    }
                }
                free_block(new_inode.i_block[INDIRECT_INDEX]);
            }
            pthread_mutex_unlock(&inode_locks[use_ino - 1]);
            free_inode(use_ino);
        }
        return wres;
    }

    /* Write inode to disk */
    write_inode(use_ino, &new_inode);

    /* Add directory entry for new file */
    if (!is_existing_file) {
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
