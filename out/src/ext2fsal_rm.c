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
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


int32_t ext2_fsal_rm(const char *path)
{
    /**
     * TODO: implement the ext2_rm command here ...
     * the argument 'path' is the path to the file to be removed.
     */
	if (!path) return -ENOENT;
	if (path[0] != '/') return -ENOENT;

	// Check for trailing slash - if path ends with '/' and target is not a dir, error
	size_t plen = strlen(path);
	int has_trailing_slash = (plen > 1 && path[plen - 1] == '/');

	char parent_path[PATH_MAX];
	char name[EXT2_NAME_LEN];
	if (split_parent_name(path, parent_path, name) < 0)
		return -ENOENT;

	int parent_ino = path_lookup(parent_path);
	if (parent_ino < 0) 
		return -ENOENT;

	struct ext2_inode *parent_inode = get_inode(parent_ino);
	if (!S_ISDIR(parent_inode->i_mode))
		return -ENOENT;

	// find target inode
	int target_ino = find_dir_entry(parent_inode, name);
	if (target_ino < 0) 
		return -ENOENT;

	struct ext2_inode *target_inode = get_inode(target_ino);

	// can't rm dirs
	if (S_ISDIR(target_inode->i_mode)) 
		return -EISDIR;

	// if trailing slash was provided but target is not a directory, return ENOENT
	if (has_trailing_slash)
		return -ENOENT;

	size_t name_len = strlen(name);

	// lock parent inode for directory modification
	pthread_mutex_lock(&inode_locks[parent_ino - 1]);

	// rm entry from parent BY NAME (not by inode, since hard links share inodes)
	int found = 0;
	for (int i = 0; i < DIRECT_POINTERS && !found; i++) {
		int block_num = parent_inode->i_block[i];
		if (block_num == 0) 
			continue;

		pthread_mutex_lock(&block_locks[block_num]);
		struct ext2_dir_entry *entry = (struct ext2_dir_entry *)get_block(block_num);
		struct ext2_dir_entry *prev = NULL;
		uint8_t *blk_start = (uint8_t*)get_block(block_num);
		uint8_t *blk_end = blk_start + EXT2_BLOCK_SIZE;

		while ((uint8_t*)entry < blk_end && entry->rec_len > 0) {
			// Match by name, not by inode (hard links share inodes!)
			if (entry->inode != 0 &&
			    (size_t)entry->name_len == name_len &&
			    memcmp(entry->name, name, name_len) == 0) {
				found = 1;
				if (prev) {
					// merge rec_len into previous entry
					prev->rec_len += entry->rec_len;
				} else {
					// first entry in block: zero the inode, keep rec_len
					entry->inode = 0;
				}
				pthread_mutex_unlock(&block_locks[block_num]);
				break;
			}
			prev = entry;
			entry = next_dir_entry(entry);
		}
		if (!found) pthread_mutex_unlock(&block_locks[block_num]);
	}

	pthread_mutex_unlock(&inode_locks[parent_ino - 1]);

	if (!found) 
		return -ENOENT;

	// lock target inode and update
	pthread_mutex_lock(&inode_locks[target_ino - 1]);

	target_inode->i_links_count--;

	if (target_inode->i_links_count == 0) {
		// set deletion time
		target_inode->i_dtime = (unsigned int)time(NULL);

		// free all direct blocks
		for (int i = 0; i < DIRECT_POINTERS; i++) {
			if (target_inode->i_block[i] != 0) {
				free_block(target_inode->i_block[i]);
				target_inode->i_block[i] = 0;
			}
		}

		// free indirect block and its pointers
		if (target_inode->i_block[INDIRECT_INDEX] != 0) {
			int indirect_blk = target_inode->i_block[INDIRECT_INDEX];
			uint32_t *ptrs = (uint32_t *) get_block(indirect_blk);
			int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
			for (int i = 0; i < per_block; i++) {
				if (ptrs[i] != 0) {
					free_block(ptrs[i]);
				}
			}
			free_block(indirect_blk);
			target_inode->i_block[INDIRECT_INDEX] = 0;
		}

		target_inode->i_blocks = 0;
		target_inode->i_size = 0;

		pthread_mutex_unlock(&inode_locks[target_ino - 1]);

		// free the inode
		free_inode(target_ino);
	} else {
		pthread_mutex_unlock(&inode_locks[target_ino - 1]);
	}

	return 0;
}
