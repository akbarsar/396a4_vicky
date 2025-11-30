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

/*
 * Copies a file from the host filesystem to the ext2 filesystem.
 *
 * src: path to source file on host filesystem
 * dst: path to destination in ext2 filesystem
 * 
 * Returns: 0 on success, errno on error.
 */
int32_t ext2_fsal_cp(const char *src, const char *dst) {
	
	if (src == NULL || dst == NULL) return ENOENT;

	// STEP 1: open and validate source file
	off_t filesize;
	int err;
	int src_fd = open_source_file(src, &filesize, &err);
	if (src_fd < 0) return errno;

	// STEP 2: parse destination path
	int parent_ino;
	char name[EXT2_NAME_LEN];

	int retval = resolve_copy_destination(dst, src, &parent_ino, name);
	if (retval != 0) {
		close(src_fd);
		return retval;
	}

	// STEP 3: handle existing target
	int target_ino, overwrite;
	retval = check_copy_target(src, &parent_ino, name, &target_ino, &overwrite);
	if (retval != 0) {
		close(src_fd);
		return retval;
	}

	// STEP 4: allocate or reuse inode
	int use_ino;
	if (overwrite && target_ino > 0) {
		free_inode_blocks_locked(target_ino);
		use_ino = target_ino;
	}
	else {
		use_ino = alloc_inode();
		if (use_ino < 0) {
			close(src_fd);
			return ENOSPC;
		}
	}

	// STEP 5: initialize inode and write file data
	struct ext2_inode new_inode;
	init_file_inode(&new_inode);

	if (lseek(src_fd, 0, SEEK_SET) < 0) {
		close(src_fd);
		if (!overwrite) free_inode(use_ino);
		return EIO;
	}

	int write_retval = write_data_into_inode(src_fd, &new_inode, filesize);

	if (write_retval != 0) {
		close(src_fd);
		if (!overwrite) {
			free_inode_blocks_locked(use_ino);
			free_inode(use_ino);
		}
		return write_retval;
	}

	// STEP 6: write inode to disk
	write_inode(use_ino, &new_inode);

	// STEP 7: add directory entry for new file
	if (!overwrite) {
		int add_retval = add_dir_entry(parent_ino, name, use_ino, EXT2_FT_REG_FILE);
		if (add_retval != 0) {
			free_inode_blocks_locked(use_ino);
			free_inode(use_ino);
			close(src_fd);
			return add_retval;
		}
	}

	close(src_fd);
	return 0;

}
