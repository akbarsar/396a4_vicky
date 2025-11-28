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
 * @file e2fs.c
 * @brief Helper functions for ext2 filesystem operations.
 *
 * This file contains utility functions for:
 * - Bitmap manipulation (inode and block allocation)
 * - Synchronization (lock management)
 * - Inode and block access
 * - Directory operations
 * - Path parsing and resolution
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "e2fs.h"
#include "ext2fsal.h"

/*
 * =============================================================================
 *                         BITMAP OPERATIONS
 * =============================================================================
 */

/**
 * Test if bit n is set in the bitmap.
 */
int test_bit(uint8_t *bitmap, int n) {
    return bitmap[n / 8] & (1 << (n % 8));
}

/**
 * Set bit n in the bitmap.
 */
void set_bit(uint8_t *bitmap, int n) {
    bitmap[n / 8] |= (1 << (n % 8));
}

/**
 * Clear bit n in the bitmap.
 */
void clear_bit(uint8_t *bitmap, int n) {
    bitmap[n / 8] &= ~(1 << (n % 8));
}

/*
 * =============================================================================
 *                      SYNCHRONIZATION PRIMITIVES
 * =============================================================================
 */

/**
 * Initialize all synchronization locks for thread-safe filesystem access.
 * Creates per-inode locks, per-block locks, and bitmap locks.
 */
void locks_init(int num_inodes, int num_blocks) {
    /* Allocate lock arrays */
    inode_locks = malloc(sizeof(pthread_mutex_t) * num_inodes);
    block_locks = malloc(sizeof(pthread_mutex_t) * num_blocks);

    /* Initialize bitmap locks */
    pthread_mutex_init(&inode_bitmap_lock, NULL);
    pthread_mutex_init(&block_bitmap_lock, NULL);

    /* Initialize per-inode and per-block locks */
    for (int i = 0; i < num_inodes; i++) {
        pthread_mutex_init(&inode_locks[i], NULL);
    }
    for (int i = 0; i < num_blocks; i++) {
        pthread_mutex_init(&block_locks[i], NULL);
    }
}

/**
 * Destroy all synchronization locks and free allocated memory.
 */
void locks_destroy(int num_inodes, int num_blocks) {
    /* Destroy per-inode and per-block locks */
    for (int i = 0; i < num_inodes; i++) {
        pthread_mutex_destroy(&inode_locks[i]);
    }
    for (int i = 0; i < num_blocks; i++) {
        pthread_mutex_destroy(&block_locks[i]);
    }

    /* Destroy bitmap locks */
    pthread_mutex_destroy(&inode_bitmap_lock);
    pthread_mutex_destroy(&block_bitmap_lock);

    /* Free lock arrays */
    free(inode_locks);
    free(block_locks);
}

/*
 * =============================================================================
 *                       INODE ALLOCATION & ACCESS
 * =============================================================================
 */

/**
 * Allocate a new inode from the bitmap.
 * Searches for the first free inode starting from the first non-reserved inode.
 * Updates both group descriptor and superblock free inode counts.
 *
 * @return Inode number (1-based) on success, -1 if no free inodes
 */
int alloc_inode(void) {
    pthread_mutex_lock(&inode_bitmap_lock);

    /* Search for first free inode (skip reserved inodes 0-10) */
    for (int i = EXT2_GOOD_OLD_FIRST_INO; i < num_inodes; i++) {
        if (!test_bit(inode_bitmap, i)) {
            set_bit(inode_bitmap, i);
            group_desc->bg_free_inodes_count--;
            superblock->s_free_inodes_count--;
            pthread_mutex_unlock(&inode_bitmap_lock);
            return i + 1;  /* Inode numbers are 1-based */
        }
    }

    pthread_mutex_unlock(&inode_bitmap_lock);
    return -1;  /* No free inodes */
}

/**
 * Free an inode and update filesystem counts.
 * 
 * @param ino Inode number to free (1-based)
 */
void free_inode(int ino) {
    int idx = ino - 1;  /* Convert to 0-based index */
    pthread_mutex_lock(&inode_bitmap_lock);
    clear_bit(inode_bitmap, idx);
    group_desc->bg_free_inodes_count++;
    superblock->s_free_inodes_count++;
    pthread_mutex_unlock(&inode_bitmap_lock);
}

/**
 * Get a pointer to an inode structure in the inode table.
 *
 * @param ino Inode number (1-based)
 * @return    Pointer to the inode
 */
struct ext2_inode* get_inode(int ino) {
    return &inode_table[ino - 1];  /* Convert to 0-based index */
}

/**
 * Write an inode structure to the inode table with locking.
 *
 * @param ino   Inode number (1-based)
 * @param inode Pointer to the source inode data
 */
void write_inode(int ino, struct ext2_inode* inode) {
    pthread_mutex_lock(&inode_locks[ino - 1]);
    struct ext2_inode* dest = get_inode(ino);
    *dest = *inode;
    pthread_mutex_unlock(&inode_locks[ino - 1]);
}

/*
 * =============================================================================
 *                       BLOCK ALLOCATION & ACCESS
 * =============================================================================
 */

/**
 * Allocate a new data block from the bitmap.
 * Updates both group descriptor and superblock free block counts.
 *
 * @return Block number (0-based) on success, -1 if no free blocks
 */
int alloc_block(void) {
    pthread_mutex_lock(&block_bitmap_lock);

    for (int i = 0; i < num_blocks; i++) {
        if (!test_bit(block_bitmap, i)) {
            set_bit(block_bitmap, i);
            group_desc->bg_free_blocks_count--;
            superblock->s_free_blocks_count--;
            pthread_mutex_unlock(&block_bitmap_lock);
            return i;
        }
    }

    pthread_mutex_unlock(&block_bitmap_lock);
    return -1;  /* No free blocks */
}

/**
 * Free a data block and update filesystem counts.
 *
 * @param block_num Block number to free (0-based)
 */
void free_block(int block_num) {
    pthread_mutex_lock(&block_bitmap_lock);
    clear_bit(block_bitmap, block_num);
    group_desc->bg_free_blocks_count++;
    superblock->s_free_blocks_count++;
    pthread_mutex_unlock(&block_bitmap_lock);
}

/**
 * Get a pointer to a data block.
 *
 * @param block_num Block number (0-based)
 * @return          Pointer to the start of the block
 */
char* get_block(int block_num) {
    return (char*)(fs + block_num * EXT2_BLOCK_SIZE);
}

/**
 * Write data to a block with locking.
 *
 * @param block_num Block number (0-based)
 * @param data      Pointer to EXT2_BLOCK_SIZE bytes to write
 */
void write_block(int block_num, char* data) {
    pthread_mutex_lock(&block_locks[block_num]);
    char* blk = get_block(block_num);
    memcpy(blk, data, EXT2_BLOCK_SIZE);
    pthread_mutex_unlock(&block_locks[block_num]);
}

/*
 * =============================================================================
 *                       DIRECTORY OPERATIONS
 * =============================================================================
 */

/**
 * Get the next directory entry in a block based on rec_len.
 *
 * @param entry Current directory entry
 * @return      Pointer to the next directory entry
 */
struct ext2_dir_entry *next_dir_entry(struct ext2_dir_entry *entry) {
    return (struct ext2_dir_entry *)((uint8_t *)entry + entry->rec_len);
}

/**
 * Calculate the minimum record length for a directory entry (4-byte aligned).
 * Structure: inode(4) + rec_len(2) + name_len(1) + file_type(1) + name
 *
 * @param name_len Length of the filename
 * @return         Required record length in bytes, 4-byte aligned
 */
int dir_entry_rec_len(int name_len) {
    int need = 8 + name_len;  /* Fixed header (8 bytes) + name */
    return (need + 3) & ~3;   /* Round up to 4-byte boundary */
}

/**
 * Initialize a directory entry with the given values.
 * 
 * This helper function sets all fields of a directory entry structure.
 * Used by add_dir_entry when creating new entries.
 *
 * @param entry     Pointer to directory entry to initialize
 * @param child_ino Inode number for the entry (1-based)
 * @param name      Name of the entry
 * @param name_len  Length of the name
 * @param type      File type (EXT2_FT_DIR, EXT2_FT_REG_FILE, EXT2_FT_SYMLINK)
 * @param rec_len   Record length to set for this entry
 */
void init_dir_entry(struct ext2_dir_entry* entry, int child_ino, 
                    const char* name, int name_len, uint8_t type, int rec_len) {
    entry->inode = child_ino;
    entry->name_len = name_len;
    entry->file_type = type;
    memcpy(entry->name, name, name_len);
    entry->rec_len = rec_len;
}

/**
 * Find the last directory entry in a block.
 * 
 * Traverses through all entries in the block following rec_len pointers
 * until reaching the final entry (one whose rec_len extends to block end).
 *
 * @param blk Pointer to the start of the directory block
 * @return    Pointer to the last directory entry in the block
 */
struct ext2_dir_entry* find_last_dir_entry(char* blk) {
    struct ext2_dir_entry* entry = (struct ext2_dir_entry*) blk;
    struct ext2_dir_entry* last = NULL;

    /* Traverse entries until we reach the last one */
    while ((char*)entry < blk + EXT2_BLOCK_SIZE && entry->rec_len > 0) {
        last = entry;
        /* Check if this entry's rec_len extends to block end */
        if ((char*)entry + entry->rec_len >= blk + EXT2_BLOCK_SIZE) {
            break;
        }
        entry = next_dir_entry(entry);
    }
    return last;
}

/**
 * Create a directory entry in a new block.
 * 
 * Allocates a new block, initializes it to zero, and creates a single
 * directory entry that spans the entire block.
 *
 * @param dir_inode      Parent directory inode
 * @param block_index    Index in i_block array for new block
 * @param child_ino      Inode number for new entry
 * @param name           Name of new entry
 * @param name_len       Length of name
 * @param type           File type
 * @return               Block number on success, -ENOSPC if no blocks available
 */
int create_entry_in_new_block(struct ext2_inode* dir_inode, int block_index,
                              int child_ino, const char* name, int name_len, 
                              uint8_t type) {
    /* Allocate a new block */
    int new_block = alloc_block();
    if (new_block < 0) {
        return -ENOSPC;
    }

    /* Update directory inode to reference new block */
    dir_inode->i_block[block_index] = new_block;
    dir_inode->i_size += (block_index == 0) ? EXT2_BLOCK_SIZE : EXT2_BLOCK_SIZE;
    dir_inode->i_blocks += EXT2_BLOCK_SIZE / 512;

    /* Initialize the new block with zeros */
    char* blk = get_block(new_block);
    memset(blk, 0, EXT2_BLOCK_SIZE);

    /* Create entry spanning the entire block */
    struct ext2_dir_entry* entry = (struct ext2_dir_entry*) blk;
    init_dir_entry(entry, child_ino, name, name_len, type, EXT2_BLOCK_SIZE);

    /* Update directory count if adding a subdirectory */
    if (type == EXT2_FT_DIR) {
        group_desc->bg_used_dirs_count++;
    }

    return new_block;
}

/**
 * Add a new directory entry to a parent directory.
 * 
 * This function handles three cases:
 * 1. No blocks allocated - creates first block with the entry
 * 2. Space in last block - splits last entry and appends new one
 * 3. No space in last block - allocates new block for the entry
 *
 * @param parent_ino Parent directory inode number (1-based)
 * @param name       Name of the new entry
 * @param child_ino  Inode number for the new entry (1-based)
 * @param type       File type (EXT2_FT_DIR, EXT2_FT_REG_FILE, EXT2_FT_SYMLINK)
 * @return           0 on success, negative errno on error
 */
int add_dir_entry(int parent_ino, const char* name, int child_ino, uint8_t type) {
    struct ext2_inode* dir_inode = get_inode(parent_ino);
    
    /* Verify parent is a directory */
    if (!S_ISDIR(dir_inode->i_mode)) {
        return -ENOTDIR;
    }

    int name_len = strlen(name);
    int needed = dir_entry_rec_len(name_len);

    pthread_mutex_lock(&inode_locks[parent_ino - 1]);

    /* Find the last used block in the directory */
    int last_block_index = -1;
    for (int i = 0; i < DIRECT_POINTERS; i++) {
        if (dir_inode->i_block[i] != 0) {
            last_block_index = i;
        }
    }

    /* Case 1: No blocks allocated yet - create first block */
    if (last_block_index == -1) {
        int result = create_entry_in_new_block(dir_inode, 0, child_ino, 
                                                name, name_len, type);
        if (result < 0) {
            pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
            return result;
        }
        /* Fix: Set size correctly for first block */
        dir_inode->i_size = EXT2_BLOCK_SIZE;
        pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
        return 0;
    }

    /* Case 2: Try to append to the last block */
    int block_num = dir_inode->i_block[last_block_index];
    pthread_mutex_lock(&block_locks[block_num]);

    char* blk = get_block(block_num);
    struct ext2_dir_entry* last = find_last_dir_entry(blk);

    /* Try to split the last entry's rec_len to make room */
    if (last != NULL) {
        int actual_size = dir_entry_rec_len(last->name_len);
        int remain = last->rec_len - actual_size;

        if (remain >= needed) {
            /* There is room: shrink last entry and append new one */
            last->rec_len = actual_size;
            struct ext2_dir_entry* new_entry = 
                (struct ext2_dir_entry*)((char*)last + actual_size);
            
            init_dir_entry(new_entry, child_ino, name, name_len, type, remain);

            if (type == EXT2_FT_DIR) {
                group_desc->bg_used_dirs_count++;
            }

            pthread_mutex_unlock(&block_locks[block_num]);
            pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
            return 0;
        }
    }

    /* Case 3: No space in last block - allocate new block */
    pthread_mutex_unlock(&block_locks[block_num]);

    if (last_block_index + 1 >= DIRECT_POINTERS) {
        /* Out of direct block pointers */
        pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
        return -ENOSPC;
    }

    int result = create_entry_in_new_block(dir_inode, last_block_index + 1,
                                            child_ino, name, name_len, type);
    if (result < 0) {
        pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
        return result;
    }

    pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
    return 0;
}

/**
 * Find a directory entry by name within a directory.
 * Searches through all blocks in the directory's i_block array.
 *
 * @param dir  Pointer to the directory inode
 * @param name Name to search for
 * @return     Inode number of the entry on success, -ENOENT if not found
 */
int find_dir_entry(struct ext2_inode *dir, const char *name) {
    size_t target_len = strlen(name);

    /* Search through all block pointers */
    for (int i = 0; i < 15; i++) {
        int block = dir->i_block[i];
        if (block == 0) {
            continue;  /* Empty block pointer */
        }

        uint8_t *block_ptr = (uint8_t *)get_block(block);
        struct ext2_dir_entry *entry = (struct ext2_dir_entry *) block_ptr;
        uint8_t *end = block_ptr + EXT2_BLOCK_SIZE;

        /* Iterate through entries in this block */
        while ((uint8_t *)entry < end && entry->rec_len > 0) {
            if (entry->inode != 0 &&
                entry->name_len == target_len &&
                strncmp(entry->name, name, entry->name_len) == 0) {
                return entry->inode;
            }
            entry = next_dir_entry(entry);
        }
    }

    return -ENOENT;  /* Not found */
}

/*
 * =============================================================================
 *                         PATH OPERATIONS
 * =============================================================================
 */

/**
 * Resolve an absolute path to an inode number.
 * Handles "." (current directory) and ".." (parent directory) components.
 *
 * @param path Absolute path to resolve (must start with '/')
 * @return     Inode number on success, negative errno on error
 */
int path_lookup(const char *path) {
    if (path == NULL) {
        return -ENOENT;
    }

    /* Create a writable copy for tokenization */
    char buf[PATH_MAX];
    strncpy(buf, path, PATH_MAX);
    buf[PATH_MAX - 1] = '\0';

    /* Handle root directory */
    if (strcmp(buf, "/") == 0) {
        return EXT2_ROOT_INO;
    }

    int curr_ino = EXT2_ROOT_INO;
    struct ext2_inode *curr_inode = get_inode(curr_ino);

    /* Tokenize path and traverse */
    char *saveptr;
    char *token = strtok_r(buf, "/", &saveptr);

    while (token != NULL) {
        /* Handle "." - stay in current directory */
        if (strcmp(token, ".") == 0) {
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        /* Handle ".." - go to parent (root's parent is itself) */
        if (strcmp(token, "..") == 0) {
            curr_ino = EXT2_ROOT_INO;  /* Simplified: always go to root for ".." */
            curr_inode = get_inode(curr_ino);
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        /* Current must be a directory to continue */
        if (!S_ISDIR(curr_inode->i_mode)) {
            return -ENOTDIR;
        }

        /* Find the child entry */
        int child_ino = find_dir_entry(curr_inode, token);
        if (child_ino < 0) {
            return -ENOENT;
        }

        curr_ino = child_ino;
        curr_inode = get_inode(curr_ino);
        token = strtok_r(NULL, "/", &saveptr);
    }

    return curr_ino;
}

/**
 * Strip trailing slashes from a path.
 * Preserves the leading '/' for the root directory.
 *
 * @param s Path string to modify in place
 */
void strip_trailing_slashes(char *s) {
    size_t len = strlen(s);
    while (len > 1 && s[len - 1] == '/') {
        s[len - 1] = '\0';
        len--;
    }
}

/**
 * Split a path into parent directory path and final component name.
 *
 * Example: "/foo/bar/baz" -> parent="/foo/bar", name="baz"
 *
 * @param path       Input path (must be absolute)
 * @param parent_buf Buffer for parent path (must be PATH_MAX bytes)
 * @param name_buf   Buffer for final name (must be EXT2_NAME_LEN bytes)
 * @return           0 on success, negative errno on error
 */
int split_parent_name(const char *path, char *parent_buf, char *name_buf) {
    if (path == NULL || path[0] != '/') {
        return -ENOENT;  /* Must be absolute path */
    }

    /* Copy and normalize the path */
    char tmp[PATH_MAX];
    strncpy(tmp, path, PATH_MAX);
    tmp[PATH_MAX - 1] = '\0';
    strip_trailing_slashes(tmp);

    /* Cannot split root "/" */
    if (strcmp(tmp, "/") == 0) {
        return -ENOENT;
    }

    /* Find the last '/' separator */
    char *last = strrchr(tmp, '/');
    if (last == NULL) {
        return -ENOENT;
    }

    /* Extract parent path */
    if (last == tmp) {
        /* Parent is root "/" */
        strncpy(parent_buf, "/", PATH_MAX);
        parent_buf[PATH_MAX - 1] = '\0';
    } else {
        size_t plen = last - tmp;
        if (plen >= PATH_MAX) {
            return -EINVAL;
        }
        strncpy(parent_buf, tmp, plen);
        parent_buf[plen] = '\0';
    }

    /* Extract final name */
    char *name = last + 1;
    if (strlen(name) == 0) {
        return -EINVAL;
    }
    if (strlen(name) >= EXT2_NAME_LEN) {
        return -ENAMETOOLONG;
    }
    strncpy(name_buf, name, EXT2_NAME_LEN);
    name_buf[EXT2_NAME_LEN - 1] = '\0';

    return 0;
}

/*
 * =============================================================================
 *                       FILE DATA OPERATIONS
 * =============================================================================
 */

/**
 * Free all data blocks of an inode (direct and single indirect).
 * Clears block pointers and resets size/blocks fields.
 *
 * @param ino Inode number (1-based)
 */
void free_inode_blocks_locked(int ino) {
    pthread_mutex_lock(&inode_locks[ino - 1]);
    struct ext2_inode *inode = get_inode(ino);

    /* Free direct blocks */
    for (int i = 0; i < DIRECT_POINTERS; i++) {
        if (inode->i_block[i] != 0) {
            free_block(inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }

    /* Free single indirect block and its referenced blocks */
    if (inode->i_block[INDIRECT_INDEX] != 0) {
        int indirect_blk = inode->i_block[INDIRECT_INDEX];
        uint32_t *ptrs = (uint32_t *) get_block(indirect_blk);
        int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);

        for (int i = 0; i < per_block; i++) {
            if (ptrs[i] != 0) {
                free_block(ptrs[i]);
            }
        }
        free_block(indirect_blk);
        inode->i_block[INDIRECT_INDEX] = 0;
    }

    inode->i_blocks = 0;
    inode->i_size = 0;
    pthread_mutex_unlock(&inode_locks[ino - 1]);
}

/**
 * Write file data from a host file descriptor into an inode.
 * Allocates blocks as needed for direct and single indirect blocks.
 *
 * @param host_fd  Open file descriptor to read from
 * @param inode    Pointer to inode to populate (block pointers set here)
 * @param filesize Total size of file to write
 * @return         0 on success, -ENOSPC or -EIO on error
 */
int write_data_into_inode(int host_fd, struct ext2_inode *inode, off_t filesize) {
    ssize_t remaining = filesize;
    int written_blocks = 0;

    /* Initialize all block pointers to zero */
    for (int i = 0; i < TOTAL_POINTERS; i++) {
        inode->i_block[i] = 0;
    }

    /* Write data to direct blocks (up to 12 blocks) */
    for (int di = 0; di < DIRECT_POINTERS && remaining > 0; di++) {
        int b = alloc_block();
        if (b < 0) {
            return -ENOSPC;
        }
        inode->i_block[di] = b;

        /* Read data from host file */
        char buf[EXT2_BLOCK_SIZE];
        ssize_t r = read(host_fd, buf, EXT2_BLOCK_SIZE);
        if (r < 0) {
            return -EIO;
        }
        
        /* Zero-fill remainder of block if needed */
        if (r < EXT2_BLOCK_SIZE) {
            memset(buf + r, 0, EXT2_BLOCK_SIZE - r);
        }

        write_block(b, buf);
        remaining -= (r > 0 ? r : 0);
        written_blocks++;
    }

    /* Write data to single indirect blocks if needed */
    if (remaining > 0) {
        int indirect_blk = alloc_block();
        if (indirect_blk < 0) {
            return -ENOSPC;
        }
        inode->i_block[INDIRECT_INDEX] = indirect_blk;

        /* Allocate array for block pointers */
        uint32_t *ptrs = (uint32_t *) malloc(EXT2_BLOCK_SIZE);
        if (!ptrs) {
            return -EIO;
        }
        memset(ptrs, 0, EXT2_BLOCK_SIZE);

        int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
        for (int i = 0; i < per_block && remaining > 0; i++) {
            int b = alloc_block();
            if (b < 0) {
                free(ptrs);
                return -ENOSPC;
            }
            ptrs[i] = b;

            char buf[EXT2_BLOCK_SIZE];
            ssize_t r = read(host_fd, buf, EXT2_BLOCK_SIZE);
            if (r < 0) {
                free(ptrs);
                return -EIO;
            }
            if (r < EXT2_BLOCK_SIZE) {
                memset(buf + r, 0, EXT2_BLOCK_SIZE - r);
            }

            write_block(b, buf);
            remaining -= (r > 0 ? r : 0);
            written_blocks++;
        }

        /* Write the indirect block with pointers */
        write_block(indirect_blk, (char *)ptrs);
        free(ptrs);
    }

    inode->i_size = filesize;
    inode->i_blocks = written_blocks * (EXT2_BLOCK_SIZE / 512);
    return 0;
}
