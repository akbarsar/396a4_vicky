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


static int disk;                  // fd for disk image
static size_t image_size;

void ext2_fsal_init(const char* image)
{
    	/**
     	* TODO: Initialization tasks, e.g., initialize synchronization primitives used,
     	* or any other structures that may need to be initialized in your implementation,
     	* open the disk image by mmap-ing it, etc.
     	*/
	struct stat st;
	if (stat(image, &st) < 0) {
        	perror("stat");
        	exit(1);
    	}
	image_size = st.st_size;

	disk = open(image, O_RDWR);
	if (disk < 0) { perror("open"); exit(1); }
	fs = mmap(NULL, image_size, PROT_READ | PROT_WRITE, MAP_SHARED, disk, 0);
	if (fs == MAP_FAILED) { perror("mmap"); exit(1); }

	superblock = (struct ext2_super_block*)(fs + EXT2_BLOCK_SIZE);
	group_desc = (struct ext2_group_desc*)(fs + 2 * EXT2_BLOCK_SIZE);
	
	inode_bitmap  = (uint8_t*)(fs + group_desc->bg_inode_bitmap * EXT2_BLOCK_SIZE);
	block_bitmap  = (uint8_t*)(fs + group_desc->bg_block_bitmap * EXT2_BLOCK_SIZE);
	inode_table   = (struct ext2_inode*)(fs + group_desc->bg_inode_table * EXT2_BLOCK_SIZE);

	num_inodes = superblock->s_inodes_count;
	num_blocks = superblock->s_blocks_count;

	locks_init(num_inodes, num_blocks);
	close(disk);
}

void ext2_fsal_destroy()
{
    	/**
     	* TODO: Cleanup tasks, e.g., destroy synchronization primitives, munmap the image, etc.
     	*/
	// get number of inodes, blocks, and bitmaps
        int num_inodes = superblock->s_inodes_count;
        int num_blocks = superblock->s_blocks_count;
        int num_block_groups = (num_blocks + superblock->s_blocks_per_group - 1) / superblock->s_blocks_per_group;

    	locks_destroy(num_inodes, num_blocks, num_block_groups, num_block_groups);
    	munmap(fs, image_size);
}
