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


int32_t ext2_fsal_mkdir(const char *path)
{
    /**
     * TODO: implement the ext2_mkdir command here ...
     * the argument path is the path to the directory that is to be created.
     */
	/* validate */
	if (path == NULL) return -ENOENT;
	if (path[0] != '/') return -ENOENT;  // must be absolute path

	/* copy path buffers */
	char parent_path[PATH_MAX];
	char name[EXT2_NAME_LEN];
	int rc = split_parent_name(path, parent_path, name);
	if (rc != 0) return rc == -ENAMETOOLONG ? -ENAMETOOLONG : -ENOENT;

	/* lookup parent inode */
	int parent_ino = path_lookup(parent_path);
	if (parent_ino < 0) return -ENOENT;

	/* ensure parent is a directory */
	struct ext2_inode *parent_inode = get_inode(parent_ino);
	if (!S_ISDIR(parent_inode->i_mode)) return -ENOENT;

	/* check if name already exists in parent */
	int existing = find_dir_entry(parent_inode, name);
	if (existing >= 0) {
		struct ext2_inode *exist_inode = get_inode(existing);
		/* if entry exists and is a directory, return EEXIST */
		if (S_ISDIR(exist_inode->i_mode)) {
			return -EEXIST;
		}
		/* if entry exists but is a file/symlink and we have trailing slash,
		 * path like /foo/bar/blah/ where blah is a file => ENOENT per spec */
		size_t plen = strlen(path);
		if (plen > 1 && path[plen - 1] == '/') {
			return -ENOENT;
		}
		/* otherwise (no trailing slash), entry exists as file => EEXIST */
		return -EEXIST;
	} else if (existing != -ENOENT) {
		/* unexpected error */
		return existing;
	}

	/* allocate inode for the new directory */
	int new_ino = alloc_inode();
	if (new_ino < 0) return -ENOSPC;

	/* allocate one data block for the directory contents */
	int new_block = alloc_block();
	if (new_block < 0) {
		/* rollback inode allocation */
		free_inode(new_ino);
		return -ENOSPC;
	}

	/* prepare new inode contents */
	struct ext2_inode new_inode;
	memset(&new_inode, 0, sizeof(new_inode));

	/* set directory mode (drwxr-xr-x) — set file type bits and permissions */
	new_inode.i_mode = EXT2_S_IFDIR | 0755;
	new_inode.i_size = EXT2_BLOCK_SIZE;
	new_inode.i_links_count = 2; /* '.' and parent link (..) */
	new_inode.i_blocks = EXT2_BLOCK_SIZE / 512; /* i_blocks counted in 512-byte sectors */

	/* point the first direct block to the allocated block */
	for (int i = 0; i < TOTAL_POINTERS; i++) new_inode.i_block[i] = 0;
	new_inode.i_block[0] = new_block;

	/* write inode to disk (write_inode uses inode lock) */
	write_inode(new_ino, &new_inode);

	/* construct directory block with "." and ".." entries */
	uint8_t block_buf[EXT2_BLOCK_SIZE];
	memset(block_buf, 0, EXT2_BLOCK_SIZE);

	/* entry for "." */
	struct ext2_dir_entry *dot = (struct ext2_dir_entry *) block_buf;
	dot->inode = new_ino;
	dot->name_len = 1;
	dot->file_type = EXT2_FT_DIR;
	memcpy(dot->name, ".", 1);
	dot->rec_len = dir_entry_rec_len(1);

	/* entry for ".." immediately after "." */
	struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *) ((uint8_t *)dot + dot->rec_len);
	dotdot->inode = parent_ino;
	dotdot->name_len = 2;
	dotdot->file_type = EXT2_FT_DIR;
	memcpy(dotdot->name, "..", 2);

	/* the rest of the block is assigned to '..' */
	dotdot->rec_len = EXT2_BLOCK_SIZE - dot->rec_len;

	/* write the directory block */
	write_block(new_block, (char *)block_buf);

	/* append a directory entry into the parent directory */
	int addrc = add_dir_entry(parent_ino, name, new_ino, EXT2_FT_DIR);
	if (addrc < 0) {
		/* cleanup: free block and inode */
		free_block(new_block);
		free_inode(new_ino);
		return addrc;
	}

	/* increment parent's link count (new subdirectory increases parent's links) */
	pthread_mutex_lock(&inode_locks[parent_ino - 1]);
	struct ext2_inode *p = get_inode(parent_ino);
	p->i_links_count += 1;
	pthread_mutex_unlock(&inode_locks[parent_ino - 1]);

	return 0;
}
