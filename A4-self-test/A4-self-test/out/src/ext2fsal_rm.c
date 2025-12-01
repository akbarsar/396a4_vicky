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
 * Removes a file or link from the ext2 filesystem:
 *
 * - Removes the directory entry from the parent (by name, not inode).
 * - Decrements the target's link count.
 * - If link count reaches zero, frees the inode and its data blocks.
 *
 * path: absolute path of the file to be removed
 *
 * Returns: 0 on success, errno on error.
 */
int32_t ext2_fsal_rm(const char *path) {

	// validate input path
	if (!path) return ENOENT;

	if (path[0] != '/') return ENOENT;

	// check for trailing slash
	size_t parent_len = strlen(path);
	int has_trailing_slash = (parent_len > 1 && path[parent_len - 1] == '/');

	// parse path into parent and target name
	char parent_path[PATH_MAX];
	char name[EXT2_NAME_LEN];

	int retval = split_parent_name(path, parent_path, name);
	if (retval != 0) return retval;

	// lookup parent directory
	int parent_ino = path_lookup(parent_path);
	if (parent_ino < 0) 
		return errno;

	struct ext2_inode *parent_inode = get_inode(parent_ino);

	// intermediates in path should be directories
	if (!S_ISDIR(parent_inode->i_mode)) {
		return ENOENT;
	}

	// find target file in parent directory
	int target_ino = find_dir_entry(parent_inode, name);
	if (target_ino < 0) 
		return ENOENT;

	struct ext2_inode *target_inode = get_inode(target_ino);

	// cannot remove directories
	if (S_ISDIR(target_inode->i_mode)) 
		return EISDIR;

	// trailing slash on non-directory is an error
	if (has_trailing_slash) {
		return ENOENT;
	}

	size_t name_len = strlen(name);

	// lock parent for directory modification
	mutex_lock(&inode_locks[parent_ino - 1]);

	// re-verify parent is still a directory after acquiring lock
	if (!S_ISDIR(parent_inode->i_mode)) {
		mutex_unlock(&inode_locks[parent_ino - 1]);
		return ENOENT;
	}


	// remove directory entry by name
	int found = 0;
	int found_target_ino = -1;  // store the inode we find
	for (int i = 0; i < DIRECT_POINTERS && !found; i++) {
		int block_num = parent_inode->i_block[i];
		if (block_num == 0) continue;

		mutex_lock(&block_locks[block_num]);

		struct ext2_dir_entry *entry = (struct ext2_dir_entry *)get_block(block_num);
		struct ext2_dir_entry *prev = NULL;
		uint8_t *blk_start = (uint8_t*)get_block(block_num);
		uint8_t *blk_end = (uint8_t*)blk_start + EXT2_BLOCK_SIZE;

		// search for entry with matching name
		while ((uint8_t*)entry < blk_end && entry->rec_len > 0) {
			if (entry->inode != 0 && (size_t)entry->name_len == name_len && memcmp(entry->name, name, name_len) == 0) {

				// re-verify: check if the found entry is a directory
				found_target_ino = entry->inode;
				struct ext2_inode *found_inode = get_inode(found_target_ino);

				if (S_ISDIR(found_inode->i_mode)) {
					// cannot remove directories with rm
					mutex_unlock(&block_locks[block_num]);
					mutex_unlock(&inode_locks[parent_ino - 1]);
					return EISDIR;
				}

				found = 1;

				if (prev) {
					// merge space into previous entry
					prev->rec_len += entry->rec_len;
				}
				else {
					// first entry in block: zero inode, keep rec_len
					entry->inode = 0;
				}

				mutex_unlock(&block_locks[block_num]);

				break;
			}
			prev = entry;
			entry = next_dir_entry(entry);
		}

		if (!found) mutex_unlock(&block_locks[block_num]);
	}

	mutex_unlock(&inode_locks[parent_ino - 1]);

	if (!found) return ENOENT;

	// update target inode: decrement link count (use the inode we actually found)
	mutex_lock(&inode_locks[found_target_ino - 1]);

	struct ext2_inode *actual_target = get_inode(found_target_ino);
	actual_target->i_links_count--;

	// if no more links, free the file
	if (actual_target->i_links_count == 0) {

		// set deletion time
		actual_target->i_dtime = (unsigned int)time(NULL);

		// free direct blocks
		for (int i = 0; i < DIRECT_POINTERS; i++) {
			if (actual_target->i_block[i] != 0) {
				free_block(actual_target->i_block[i]);
				actual_target->i_block[i] = 0;
			}
		}

		// free indirect block and its referenced blocks
		if (actual_target->i_block[INDIRECT_INDEX] != 0) {
			int indirect_blk = actual_target->i_block[INDIRECT_INDEX];
			uint32_t *ptrs = (uint32_t *) get_block(indirect_blk);
			int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
			
			for (int i = 0; i < per_block; i++) {
				if (ptrs[i] != 0) {
					free_block(ptrs[i]);
				}
			}

			free_block(indirect_blk);
			actual_target->i_block[INDIRECT_INDEX] = 0;
		}

		actual_target->i_blocks = 0;
		actual_target->i_size = 0;

		mutex_unlock(&inode_locks[found_target_ino - 1]);

		// free the inode
		free_inode(found_target_ino);
	}
	else {
		// other hard links still exist
		mutex_unlock(&inode_locks[found_target_ino - 1]);
	}

	return 0;
}
