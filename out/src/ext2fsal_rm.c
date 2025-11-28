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


int32_t ext2_fsal_rm(const char *path)
{
    /**
     * TODO: implement the ext2_rm command here ...
     * the argument 'path' is the path to the file to be removed.
     */
	if (!path) return -EINVAL;

	char parent_path[PATH_MAX];
	char name[EXT2_NAME_LEN];
	if (split_parent_name(path, parent_path, name) < 0)
		return -EINVAL;

	int parent_ino = path_lookup(parent_path);
	if (parent_ino < 0) 
		return parent_ino;

	struct ext2_inode *parent_inode = get_inode(parent_ino);

	// find target inode
	int target_ino = find_dir_entry(parent_inode, name);
	if (target_ino < 0) 
		return -ENOENT;

	struct ext2_inode *target_inode = get_inode(target_ino);

	// cant rm dirs
	if (S_ISDIR(target_inode->i_mode)) 
		return -EISDIR;

	// rm entry from parent
	int found = 0;
	for (int i = 0; i < 15 && !found; i++) {
		int block_num = parent_inode->i_block[i];
		if (block_num == 0) 
			continue;

		struct ext2_dir_entry *entry = (struct ext2_dir_entry *)get_block(block_num);
		struct ext2_dir_entry *prev = NULL;
		uint8_t *blk_end = (uint8_t*)entry + EXT2_BLOCK_SIZE;

		while ((uint8_t*)entry < blk_end && entry->rec_len > 0) {
			if (entry->inode == target_ino) {
				found = 1;
				if (prev) {
					// merge rec_len into previous entry
					prev->rec_len += entry->rec_len;
				} else {
					// first entry so mark inode 0, keep rec_len
					entry->inode = 0;
				}
				break;
			}
			prev = entry;
			entry = next_dir_entry(entry);
		}
	}

	if (!found) 
		return -ENOENT;

	// free all blocks of the target
	for (int i = 0; i < 15; i++) {
		if (target_inode->i_block[i] != 0) {
			free_block(target_inode->i_block[i]);
			target_inode->i_block[i] = 0;
		}
	}

	// free the inode
	free_inode(target_ino);

	return 0;
}
