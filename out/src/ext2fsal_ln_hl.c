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
 * Creates a hard link to an existing file:
 *
 * - Validates both source and destination paths
 * - Verifies source exists and is not a directory
 * - Creates a new directory entry pointing to source's inode
 * - Increments the source inode's link count
 *
 * src: absolute path to the source file
 * dst: absolute path for the new hard link
 * 
 * Returns: 0 on success, errno on error
 */
int32_t ext2_fsal_ln_hl(const char *src, const char *dst) {

	// validate input paths
	if (!src || !dst) {
		return ENOENT;
	}
	if (src[0] != '/' || dst[0] != '/') {
		return ENOENT;
	}

	// Acquire global lock to serialize filesystem operations
	mutex_lock(&global_fs_lock);

	char parent[PATH_MAX], name[EXT2_NAME_LEN];
	
	// lookup source file
	int src_ino = path_lookup(src);
	if (src_ino < 0) {
		mutex_unlock(&global_fs_lock);
		return ENOENT;
	}

	struct ext2_inode *src_inode = get_inode(src_ino);

	// cannot create hard links for directories
	if (S_ISDIR(src_inode->i_mode)) {
		mutex_unlock(&global_fs_lock);
		return EISDIR;
	}

	// parse destination path
	if (split_parent_name(dst, parent, name) < 0) {
		mutex_unlock(&global_fs_lock);
		return ENOENT;
	}

	// lookup destination parent directory
	int parent_ino = path_lookup(parent);
	if (parent_ino < 0) {
		mutex_unlock(&global_fs_lock);
		return ENOENT;
	}

	struct ext2_inode *p_inode = get_inode(parent_ino);
	if (!S_ISDIR(p_inode->i_mode)) {
		mutex_unlock(&global_fs_lock);
		return ENOENT;
	}

	// check if destination name already exists
	int exists_ino = find_dir_entry(p_inode, name);
	if (exists_ino >= 0) {
		struct ext2_inode *exist_inode = get_inode(exists_ino);
		// return EISDIR if target exists and is a directory
		if (S_ISDIR(exist_inode->i_mode)) {
			mutex_unlock(&global_fs_lock);
			return EISDIR;
		}
		mutex_unlock(&global_fs_lock);
		return EEXIST;
	}

	// determine file type for directory entry
	uint8_t file_type = EXT2_FT_REG_FILE;
	if (S_ISLNK(src_inode->i_mode)) {
		file_type = EXT2_FT_SYMLINK;
	}

	// add new directory entry pointing to source inode
	int retval = add_dir_entry(parent_ino, name, src_ino, file_type);
	if (retval != 0) {
		mutex_unlock(&global_fs_lock);
		return retval;
	}

	// increment source inode's link count
	mutex_lock(&inode_locks[src_ino - 1]);
	src_inode->i_links_count++;
	mutex_unlock(&inode_locks[src_ino - 1]);
	
	mutex_unlock(&global_fs_lock);
	return 0;
}
