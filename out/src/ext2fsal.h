/**
 * @file ext2fsal.h
 * @brief EXT2 File System Abstraction Layer Header
 *
 * This header defines the public API for ext2 filesystem operations and
 * declares synchronization primitives shared across all filesystem tools.
 *
 * Copyright (c) 2025 MCS @ UTM
 * This code is provided solely for the personal and private use of
 * students taking the CSC369H5 course at the University of Toronto.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/*============================================================================
 * FILESYSTEM STRUCTURE POINTERS
 *
 * These global pointers reference key ext2 data structures in the memory-
 * mapped disk image. They are initialized in ext2_fsal_init().
 *===========================================================================*/

/** Pointer to the superblock (contains filesystem metadata) */
extern struct ext2_super_block *superblock;

/** Pointer to the group descriptor (contains block group info) */
extern struct ext2_group_desc *group_desc;

/** Pointer to the raw filesystem image in memory */
extern uint8_t *fs;

/** Pointer to the inode bitmap (tracks allocated inodes) */
extern uint8_t *inode_bitmap;

/** Pointer to the block bitmap (tracks allocated blocks) */
extern uint8_t *block_bitmap;

/** Pointer to the inode table (array of inode structures) */
extern struct ext2_inode *inode_table;

/** Total number of inodes in the filesystem */
extern int num_inodes;

/** Total number of blocks in the filesystem */
extern int num_blocks;

/*============================================================================
 * SYNCHRONIZATION PRIMITIVES
 *
 * Fine-grained locking for concurrent access to filesystem structures.
 * Lock ordering (to prevent deadlocks): 
 *   bitmap locks -> parent inode lock -> block locks -> child inode lock
 *===========================================================================*/

/** Per-inode locks (array of num_inodes mutexes) */
extern pthread_mutex_t *inode_locks;

/** Per-block locks (array of num_blocks mutexes) */
extern pthread_mutex_t *block_locks;

/** Lock for inode bitmap access (allocation/deallocation) */
extern pthread_mutex_t inode_bitmap_lock;

/** Lock for block bitmap access (allocation/deallocation) */
extern pthread_mutex_t block_bitmap_lock;

/*============================================================================
 * INITIALIZATION AND CLEANUP
 *===========================================================================*/

/**
 * @brief Initialize the ext2 filesystem abstraction layer
 *
 * Memory-maps the disk image and initializes all synchronization primitives.
 * Called once during server startup.
 *
 * @param image Full path to a valid ext2 disk image file
 */
void ext2_fsal_init(const char *image);

/**
 * @brief Destroy the ext2 filesystem abstraction layer
 *
 * Unmaps the disk image and destroys all synchronization primitives.
 * Called once during server shutdown.
 */
void ext2_fsal_destroy(void);

/*============================================================================
 * FILESYSTEM OPERATIONS API
 *
 * All operations return 0 on success or a negative error code on failure.
 * Common error codes: -ENOENT, -EEXIST, -EISDIR, -ENOSPC
 *===========================================================================*/

/**
 * @brief Copy a file from the host filesystem to the ext2 image
 *
 * @param src Path to source file on host filesystem
 * @param dst Absolute path for destination in ext2 image
 * @return 0 on success, negative error code on failure
 */
int32_t ext2_fsal_cp(const char *src, const char *dst);

/**
 * @brief Create a hard link in the ext2 image
 *
 * @param src Absolute path to existing file (target) in ext2 image
 * @param dst Absolute path for new hard link in ext2 image
 * @return 0 on success, negative error code on failure
 */
int32_t ext2_fsal_ln_hl(const char *src, const char *dst);

/**
 * @brief Create a symbolic link in the ext2 image
 *
 * @param src Target path stored in the symlink (need not exist)
 * @param dst Absolute path for new symlink in ext2 image
 * @return 0 on success, negative error code on failure
 */
int32_t ext2_fsal_ln_sl(const char *src, const char *dst);

/**
 * @brief Remove a file or symbolic link from the ext2 image
 *
 * @param path Absolute path to file or symlink to remove
 * @return 0 on success, negative error code on failure
 */
int32_t ext2_fsal_rm(const char *path);

/**
 * @brief Create a directory in the ext2 image
 *
 * @param path Absolute path for new directory
 * @return 0 on success, negative error code on failure
 */
int32_t ext2_fsal_mkdir(const char *path);


