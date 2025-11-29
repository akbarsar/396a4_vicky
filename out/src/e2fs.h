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

#ifndef CSC369_E2FS_H
#define CSC369_E2FS_H

#include "ext2.h"
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * =============================================================================
 *                              CONSTANTS
 * =============================================================================
 */

#define DIRECT_POINTERS 12   /* Number of direct block pointers in inode */
#define INDIRECT_INDEX  12   /* Index of single indirect pointer in i_block */
#define TOTAL_POINTERS  15   /* Total block pointers in inode (12 direct + 3 indirect) */
#define PATH_MAX 4096        /* Maximum path length */

/*
 * =============================================================================
 *                         BITMAP OPERATIONS
 * =============================================================================
 * Low-level bit manipulation functions for inode and block bitmaps.
 */

/**
 * Test if bit n is set in the bitmap.
 * @param bitmap Pointer to the bitmap array
 * @param n      Bit index to test (0-based)
 * @return       Non-zero if bit is set, 0 otherwise
 */
int test_bit(uint8_t *bitmap, int n);

/**
 * Set bit n in the bitmap.
 * @param bitmap Pointer to the bitmap array
 * @param n      Bit index to set (0-based)
 */
void set_bit(uint8_t *bitmap, int n);

/**
 * Clear bit n in the bitmap.
 * @param bitmap Pointer to the bitmap array
 * @param n      Bit index to clear (0-based)
 */
void clear_bit(uint8_t *bitmap, int n);

/*
 * =============================================================================
 *                      SYNCHRONIZATION PRIMITIVES
 * =============================================================================
 * Lock initialization and cleanup for thread-safe filesystem operations.
 */

/**
 * Initialize all synchronization locks.
 * Allocates and initializes per-inode locks, per-block locks, and bitmap locks.
 * @param num_inodes Total number of inodes in the filesystem
 * @param num_blocks Total number of blocks in the filesystem
 */
void locks_init(int num_inodes, int num_blocks);

/**
 * Destroy all synchronization locks and free allocated memory.
 * @param num_inodes Total number of inodes in the filesystem
 * @param num_blocks Total number of blocks in the filesystem
 */
void locks_destroy(int num_inodes, int num_blocks);

/*
 * =============================================================================
 *                       INODE ALLOCATION & ACCESS
 * =============================================================================
 * Functions for allocating, accessing, and freeing inodes.
 */

/**
 * Allocate a new inode from the bitmap.
 * Thread-safe: acquires inode_bitmap_lock internally.
 * @return Inode number (1-based) on success, -1 if no free inodes
 */
int alloc_inode(void);

/**
 * Free an inode and update filesystem counts.
 * Thread-safe: acquires inode_bitmap_lock internally.
 * @param ino Inode number to free (1-based)
 */
void free_inode(int ino);

/**
 * Get a pointer to an inode structure.
 * @param ino Inode number (1-based)
 * @return    Pointer to the inode in the inode table
 */
struct ext2_inode* get_inode(int ino);

/**
 * Write an inode structure to the inode table.
 * Thread-safe: acquires inode lock internally.
 * @param ino   Inode number (1-based)
 * @param inode Pointer to the inode data to write
 */
void write_inode(int ino, struct ext2_inode* inode);

/*
 * =============================================================================
 *                       BLOCK ALLOCATION & ACCESS
 * =============================================================================
 * Functions for allocating, accessing, and freeing data blocks.
 */

/**
 * Allocate a new data block from the bitmap.
 * Thread-safe: acquires block_bitmap_lock internally.
 * @return Block number (0-based) on success, -1 if no free blocks
 */
int alloc_block(void);

/**
 * Free a data block and update filesystem counts.
 * Thread-safe: acquires block_bitmap_lock internally.
 * @param block_num Block number to free (0-based)
 */
void free_block(int block_num);

/**
 * Get a pointer to a data block.
 * @param block_num Block number (0-based)
 * @return          Pointer to the start of the block
 */
char* get_block(int block_num);

/**
 * Write data to a block.
 * Thread-safe: acquires block lock internally.
 * @param block_num Block number (0-based)
 * @param data      Pointer to EXT2_BLOCK_SIZE bytes to write
 */
void write_block(int block_num, char* data);

/*
 * =============================================================================
 *                       DIRECTORY OPERATIONS
 * =============================================================================
 * Functions for manipulating directory entries and traversing paths.
 */

/**
 * Get the next directory entry in a block.
 * @param entry Current directory entry
 * @return      Pointer to the next directory entry
 */
struct ext2_dir_entry *next_dir_entry(struct ext2_dir_entry *entry);

/**
 * Calculate the record length for a directory entry (4-byte aligned).
 * @param name_len Length of the filename
 * @return         Required record length in bytes
 */
int dir_entry_rec_len(int name_len);

/**
 * Add a new directory entry to a parent directory.
 * Thread-safe: acquires parent inode and block locks internally.
 * @param parent_ino Parent directory inode number (1-based)
 * @param name       Name of the new entry
 * @param child_ino  Inode number for the new entry (1-based)
 * @param type       File type (EXT2_FT_DIR, EXT2_FT_REG_FILE, EXT2_FT_SYMLINK)
 * @return           0 on success, negative errno on error
 */
int add_dir_entry(int parent_ino, const char* name, int child_ino, uint8_t type);

/**
 * Find a directory entry by name within a directory.
 * @param dir  Pointer to the directory inode
 * @param name Name to search for
 * @return     Inode number of the entry on success, -ENOENT if not found
 */
int find_dir_entry(struct ext2_inode *dir, const char *name);

/*
 * =============================================================================
 *                         PATH OPERATIONS
 * =============================================================================
 * Functions for parsing and resolving filesystem paths.
 */

/**
 * Resolve an absolute path to an inode number.
 * Handles "." and ".." path components.
 * @param path Absolute path to resolve
 * @return     Inode number on success, negative errno on error
 */
int path_lookup(const char *path);

/**
 * Strip trailing slashes from a path (preserving leading '/').
 * Modifies the string in place.
 * @param s Path string to modify
 */
void strip_trailing_slashes(char *s);

/**
 * Split a path into parent directory path and final component name.
 * @param path       Input path (must be absolute)
 * @param parent_buf Buffer for parent path (must be PATH_MAX bytes)
 * @param name_buf   Buffer for final name (must be EXT2_NAME_LEN bytes)
 * @return           0 on success, negative errno on error
 */
int split_parent_name(const char *path, char *parent_buf, char *name_buf);

/*
 * =============================================================================
 *                       FILE DATA OPERATIONS
 * =============================================================================
 * Functions for managing file data blocks.
 */

/**
 * Free all data blocks of an inode (direct and single indirect).
 * Thread-safe: acquires inode lock internally.
 * @param ino Inode number (1-based)
 */
void free_inode_blocks_locked(int ino);

/**
 * Write file data from a host file descriptor into an inode.
 * Allocates blocks and updates inode size/blocks fields.
 * @param host_fd  Open file descriptor to read from
 * @param inode    Pointer to inode to populate (block pointers set here)
 * @param filesize Total size of file to write
 * @return         0 on success, -ENOSPC or -EIO on error
 */
int write_data_into_inode(int host_fd, struct ext2_inode *inode, off_t filesize);

/**
 * Initialize a new file inode with proper mode, timestamps, and link count.
 * Sets i_mode to regular file with 0644 permissions, i_links_count to 1,
 * and sets ctime/mtime/atime to current time.
 * @param inode Pointer to inode structure to initialize
 */
void init_file_inode(struct ext2_inode *inode);

#endif /* CSC369_E2FS_H */
