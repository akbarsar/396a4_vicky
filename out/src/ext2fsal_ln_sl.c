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
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Creates a symbolic link pointing to a target path:
 *
 * - Validates the destination path (source doesn't need to exist).
 * - Allocates a new inode for the symlink.
 * - Allocates a data block to store the target path.
 * - Creates the symlink inode and directory entry.
 *
 * src: absolute target path (what the symlink points to)
 * dst: absolute path for the symlink
 * 
 * Returns: 0 on success, errno on error.
 */
int32_t ext2_fsal_ln_sl(const char *src, const char *dst) {

	// validate input paths
	if (!src || !dst) {
		return ENOENT;
	}
	if (dst[0] != '/' || src[0] != '/') {
		return ENOENT;  // paths must be absolute
	}
	
	// Acquire global lock to serialize filesystem operations
	mutex_lock(&global_fs_lock);
	
	// parse destination path
	char parent[PATH_MAX], name[EXT2_NAME_LEN];
	int retval = split_parent_name(dst, parent, name);
	if (retval != 0) {
		mutex_unlock(&global_fs_lock);
		return retval;
	}

	// lookup destination parent directory
	int parent_ino = path_lookup(parent);
	if (parent_ino < 0) {
		mutex_unlock(&global_fs_lock);
		return errno;
	}

	// intermediate inodes should all be directories
	struct ext2_inode *p_inode = get_inode(parent_ino);
	if (!S_ISDIR(p_inode->i_mode)) {
		mutex_unlock(&global_fs_lock);
		return ENOENT;
	}
	
	// check if destination name already exists
	int exists = find_dir_entry(p_inode, name);
	
	if (exists >= 0) {
		struct ext2_inode *eino = get_inode(exists);
		if (S_ISDIR(eino->i_mode)) {
			mutex_unlock(&global_fs_lock);
			return EISDIR;
		}

		// existing file (non-directory) with target name
		mutex_unlock(&global_fs_lock);
		return EEXIST;
	}
	
	// allocate inode for symlink
	int new_ino = alloc_inode();
	if (new_ino < 0) {
		mutex_unlock(&global_fs_lock);
		return ENOSPC;
	}

	// allocate data block for target path
	int new_block = alloc_block();
	if (new_block < 0) {
		free_inode(new_ino);
		mutex_unlock(&global_fs_lock);
		return ENOSPC;
	}

	// initialize symlink inode
	struct ext2_inode new_inode;
	memset(&new_inode, 0, sizeof(new_inode));
	new_inode.i_mode = EXT2_S_IFLNK | 0777;
	new_inode.i_links_count = 1;
	new_inode.i_size = strlen(src);
	new_inode.i_blocks = EXT2_BLOCK_SIZE / 512;
	new_inode.i_block[0] = new_block;
	new_inode.i_ctime = (unsigned int)time(NULL);

	// write target path to data block
	char *blk = get_block(new_block);
	memset(blk, 0, EXT2_BLOCK_SIZE);
	memcpy(blk, src, strlen(src));
	
	// write inode to disk
	write_inode(new_ino, &new_inode);
	
	// add symlink entry to parent directory
	int r = add_dir_entry(parent_ino, name, new_ino, EXT2_FT_SYMLINK);
	if (r < 0) {
		free_block(new_block);
		free_inode(new_ino);
		mutex_unlock(&global_fs_lock);
		return r;
	}
	
	mutex_unlock(&global_fs_lock);
	return 0;
}
