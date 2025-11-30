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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ext2fsal.h"
#include "e2fs.h"

// ----------------- (GLOBAL) VARIABLE DEFINITIONS -------------------

// FILESYSTEM METADATA STRUCTURES
struct ext2_super_block *superblock;
struct ext2_group_desc *group_desc;
uint8_t* fs;     // mmapped filesystem image

// BITMAP AND INODE TABLE POINTERS
uint8_t* inode_bitmap;         // bitmap for inode allocation
uint8_t* block_bitmap;         // bitmap for block allocation
struct ext2_inode* inode_table;  // array of inode structs

// FILESYSTEM SIZE COUNTERS
int num_inodes;        // total number of inodes
int num_blocks;        // total number of blocks

// SYNCHRONIZATION PRIMITIVES
pthread_mutex_t* inode_locks;    // per-inode locks
pthread_mutex_t* block_locks;    // per-block locks
pthread_mutex_t inode_bitmap_lock; // lock for inode bitmap access
pthread_mutex_t block_bitmap_lock; // lock for block bitmap access
pthread_mutex_t global_fs_lock;    // global lock for serializing FS operations

// PRIVATE VARIABLES
static int disk;           // file descriptor for disk image
static size_t image_size;  // size of the disk image, in bytes

// -------------------- INITIALIZATION & CLEANUP ---------------------

/*
 * Initializes the ext2 filesystem interface for the given image:
 *
 * - Opens and mmaps the disk image.
 * - Sets up pointers to filesystem structures (superblock, 
 *   group descriptor, etc.)
 * - Initializes synchronization primitives.
 */
void ext2_fsal_init(const char* image) {

	// get the image file size
	struct stat st;
	if (stat(image, &st) < 0) {
        	perror("stat");
        	exit(1);
    	}
	image_size = st.st_size;

	// open and mmap the disk image
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

	// set up pointers to file system structures
	superblock = (struct ext2_super_block*)(fs + EXT2_BLOCK_SIZE);
	group_desc = (struct ext2_group_desc*)(fs + 2 * EXT2_BLOCK_SIZE);
	
	inode_bitmap  = (uint8_t*)(fs + group_desc->bg_inode_bitmap * EXT2_BLOCK_SIZE);
	block_bitmap  = (uint8_t*)(fs + group_desc->bg_block_bitmap * EXT2_BLOCK_SIZE);
	inode_table   = (struct ext2_inode*)(fs + group_desc->bg_inode_table * EXT2_BLOCK_SIZE);

	// store filesystem size info
	num_inodes = superblock->s_inodes_count;
	num_blocks = superblock->s_blocks_count;

	// initialize synchronization primitives
	locks_init(num_inodes, num_blocks);

	// close file descriptor, mmap keeps the mapping
	close(disk);
}

/*
 * Destroys the ext2 filesystem interface and cleans up resources:
 *
 * - Destroys all synchronization primitives.
 * - Unmaps the disk image from memory.
 */
void ext2_fsal_destroy() {

	// get filesystem size for lock cleanup
        int num_inodes = superblock->s_inodes_count;
        int num_blocks = superblock->s_blocks_count;

	// destroy synchronization primitives
	locks_destroy(num_inodes, num_blocks);

	// unmap the disk image
    	munmap(fs, image_size);
}
