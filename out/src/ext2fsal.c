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
 * @file ext2fsal.c
 * @brief Ext2 filesystem initialization and cleanup.
 *
 * This file contains functions to initialize and destroy the ext2 filesystem
 * interface, including memory-mapping the disk image and setting up
 * synchronization primitives.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ext2fsal.h"
#include "e2fs.h"

/*
 * =============================================================================
 *                       GLOBAL VARIABLE DEFINITIONS
 * =============================================================================
 * These variables are declared extern in ext2fsal.h and shared across all
 * filesystem operation modules.
 */

/* Filesystem metadata structures */
struct ext2_super_block *superblock;    /* Superblock containing FS metadata */
struct ext2_group_desc *group_desc;     /* Block group descriptor */
uint8_t* fs;                            /* Memory-mapped filesystem image */

/* Bitmap and inode table pointers */
uint8_t* inode_bitmap;                  /* Bitmap tracking inode allocation */
uint8_t* block_bitmap;                  /* Bitmap tracking block allocation */
struct ext2_inode* inode_table;         /* Array of inode structures */

/* Filesystem size counters */
int num_inodes;                         /* Total number of inodes */
int num_blocks;                         /* Total number of blocks */

/*
 * =============================================================================
 *                       SYNCHRONIZATION PRIMITIVES
 * =============================================================================
 * Fine-grained locking for concurrent filesystem operations.
 */
pthread_mutex_t* inode_locks;           /* Per-inode locks */
pthread_mutex_t* block_locks;           /* Per-block locks */
pthread_mutex_t inode_bitmap_lock;      /* Lock for inode bitmap access */
pthread_mutex_t block_bitmap_lock;      /* Lock for block bitmap access */

/* Private variables */
static int disk;                        /* File descriptor for disk image */
static size_t image_size;               /* Size of the disk image in bytes */

/*
 * =============================================================================
 *                       INITIALIZATION & CLEANUP
 * =============================================================================
 */

/**
 * Initialize the ext2 filesystem interface.
 * 
 * This function:
 * 1. Opens and memory-maps the disk image
 * 2. Sets up pointers to filesystem structures (superblock, group descriptor, etc.)
 * 3. Initializes synchronization primitives
 *
 * @param image Path to the ext2 disk image file
 */
void ext2_fsal_init(const char* image)
{
    /* Get the image file size */
    struct stat st;
    if (stat(image, &st) < 0) {
        perror("stat");
        exit(1);
    }
    image_size = st.st_size;

    /* Open and memory-map the disk image */
    disk = open(image, O_RDWR);
    if (disk < 0) {
        perror("open");
        exit(1);
    }
    
    fs = mmap(NULL, image_size, PROT_READ | PROT_WRITE, MAP_SHARED, disk, 0);
    if (fs == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    /* Set up pointers to filesystem structures */
    superblock = (struct ext2_super_block*)(fs + EXT2_BLOCK_SIZE);
    group_desc = (struct ext2_group_desc*)(fs + 2 * EXT2_BLOCK_SIZE);
    
    inode_bitmap = (uint8_t*)(fs + group_desc->bg_inode_bitmap * EXT2_BLOCK_SIZE);
    block_bitmap = (uint8_t*)(fs + group_desc->bg_block_bitmap * EXT2_BLOCK_SIZE);
    inode_table = (struct ext2_inode*)(fs + group_desc->bg_inode_table * EXT2_BLOCK_SIZE);

    /* Store filesystem size info */
    num_inodes = superblock->s_inodes_count;
    num_blocks = superblock->s_blocks_count;

    /* Initialize synchronization primitives */
    locks_init(num_inodes, num_blocks);
    
    /* Close file descriptor (mmap keeps the mapping) */
    close(disk);
}

/**
 * Destroy the ext2 filesystem interface and cleanup resources.
 * 
 * This function:
 * 1. Destroys all synchronization primitives
 * 2. Unmaps the disk image from memory
 */
void ext2_fsal_destroy()
{
    /* Get filesystem size for lock cleanup */
    int n_inodes = superblock->s_inodes_count;
    int n_blocks = superblock->s_blocks_count;

    /* Destroy synchronization primitives */
    locks_destroy(n_inodes, n_blocks);
    
    /* Unmap the disk image */
    munmap(fs, image_size);
}
