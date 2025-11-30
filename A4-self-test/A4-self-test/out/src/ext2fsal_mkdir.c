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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Creates a new directory in the ext2 filesystem:
 * - Validates the path and extracts parent directory and name.
 * - Allocates a new inode and block for the directory.
 * - Initializes the directory with "." and ".." entries.
 * - Adds an entry in the parent directory.
 * - Updates parent's link count.
 *
 * path: absolute path of the directory that is to be created.
 * 
 * Returns: 0 on success, errno on error.
 */
int32_t ext2_fsal_mkdir(const char *path) {

	// validate input path
	if (path == NULL) return ENOENT;
	if (path[0] != '/') {
		return ENOENT;  // must be absolute path
	}

	// Acquire global lock to serialize filesystem operations
	mutex_lock(&global_fs_lock);

	// parse path into parent directory and target name
	char parent_path[PATH_MAX];
	char name[EXT2_NAME_LEN];
	int retval = split_parent_name(path, parent_path, name);
	if (retval != 0) {
		mutex_unlock(&global_fs_lock);
		return retval;
	}

	// lookup parent directory inode
	int parent_ino = path_lookup(parent_path);
	if (parent_ino < 0) {
		mutex_unlock(&global_fs_lock);
		return errno;
	}

	// ensure parent is a directory - intermediate must be
	struct ext2_inode *parent_inode = get_inode(parent_ino);
	if (!S_ISDIR(parent_inode->i_mode)) {
		mutex_unlock(&global_fs_lock);
		return ENOENT;
	}

	// check if target name already exists in parent
	int existing = find_dir_entry(parent_inode, name);
	if (existing >= 0) {
		struct ext2_inode *exist_inode = get_inode(existing);
		
		// if existing entry is a directory, return EEXIST
		if (S_ISDIR(exist_inode->i_mode)) {
			mutex_unlock(&global_fs_lock);
			return EEXIST;
		}
		
		// if path has trailing slash but is file
		size_t parent_len = strlen(path);
		if (parent_len > 1 && path[parent_len - 1] == '/') {
			mutex_unlock(&global_fs_lock);
			return ENOENT;
		}
		
		// entry exists as non-directory
		mutex_unlock(&global_fs_lock);
		return EEXIST;
	}

	// existing == -1; not found, create directory

	// allocate inode for the new directory
	int new_ino = alloc_inode();
	if (new_ino < 0) {
		mutex_unlock(&global_fs_lock);
		return ENOSPC;
	}

	// allocate data block for the directory contents
	int new_block = alloc_block();
	if (new_block < 0) {
		free_inode(new_ino);
		mutex_unlock(&global_fs_lock);
		return ENOSPC;
	}

	// initialize new directory inode
	struct ext2_inode new_inode;
	memset(&new_inode, 0, sizeof(new_inode));

	new_inode.i_mode = EXT2_S_IFDIR | 0755; // drwxr-xr-x
	new_inode.i_size = EXT2_BLOCK_SIZE;
	new_inode.i_links_count = 2;  // "." + ".."
	new_inode.i_blocks = EXT2_BLOCK_SIZE / 512;
	new_inode.i_ctime = (uint32_t)time(NULL);

	// initialize block pointers
	for (int i = 0; i < TOTAL_POINTERS; i++) {
		new_inode.i_block[i] = 0;
	}
	new_inode.i_block[0] = new_block;

	// write inode to disk
	write_inode(new_ino, &new_inode);

	// construct directory block with "." and ".." entries
	uint8_t block_buf[EXT2_BLOCK_SIZE];
	memset(block_buf, 0, EXT2_BLOCK_SIZE);

	// create "." entry
	struct ext2_dir_entry *dot = (struct ext2_dir_entry *) block_buf;
	dot->inode = new_ino;
	dot->name_len = 1;
	dot->file_type = EXT2_FT_DIR;
	memcpy(dot->name, ".", 1);
	dot->rec_len = dir_entry_rec_len(1);

	// create ".." entry
	struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *)((uint8_t *)dot + dot->rec_len);
	dotdot->inode = parent_ino;
	dotdot->name_len = 2;
	dotdot->file_type = EXT2_FT_DIR;
	memcpy(dotdot->name, "..", 2);
	// the rest of the block is assigned to ".."
	dotdot->rec_len = EXT2_BLOCK_SIZE - dot->rec_len;

	// write directory block to disk
	write_block(new_block, (char *)block_buf);

	// add entry for new directory in parent
	int add_retval = add_dir_entry(parent_ino, name, new_ino, EXT2_FT_DIR);
	if (add_retval < 0) {
		free_block(new_block);
		free_inode(new_ino);
		mutex_unlock(&global_fs_lock);
		return add_retval;
	}

	// increment parent's link count ( ".." links to parent)
	mutex_lock(&inode_locks[parent_ino - 1]);
	struct ext2_inode *p = get_inode(parent_ino);
	p->i_links_count += 1;
	mutex_unlock(&inode_locks[parent_ino - 1]);

	mutex_unlock(&global_fs_lock);
	return 0;
}
