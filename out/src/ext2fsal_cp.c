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

int32_t ext2_fsal_cp(const char *src,
                     const char *dst)
{
    /**
     * TODO: implement the ext2_cp command here ...
     * Arguments src and dst are the cp command arguments described in the handout.
     */
	if (src == NULL || dst == NULL) return -ENOENT;

	/* open and stat source */
	int src_fd = open(src, O_RDONLY);
	if (src_fd < 0) return -ENOENT;
	struct stat st;
	if (fstat(src_fd, &st) < 0) {
		close(src_fd);
		return -ENOENT;
	}
	if (!S_ISREG(st.st_mode)) {
		close(src_fd);
		return -ENOENT; /* only copy regular files for this assignment */
	}
	off_t filesize = st.st_size;

	/* handle target path cases:
	 * - if dst_path refers to an existing directory (or ends with '/'), copy into that dir using basename(src_path)
	 * - else split parent,name and operate on parent directory
	 */
	char parent_path[PATH_MAX];
	char name[EXT2_NAME_LEN];
	int rc;

	char tmp_dst[PATH_MAX];
	strncpy(tmp_dst, dst, PATH_MAX);
	tmp_dst[PATH_MAX - 1] = '\0';
	/* if dst ends with '/', treat as directory target */
	if (strlen(tmp_dst) > 1 && tmp_dst[strlen(tmp_dst) - 1] == '/') {
		/* resolve the directory inode */
		int dir_ino = path_lookup(tmp_dst);
		if (dir_ino < 0) { close(src_fd); return -ENOENT; }
		struct ext2_inode *dir_inode = get_inode(dir_ino);
		if (!S_ISDIR(dir_inode->i_mode)) { close(src_fd); return -ENOTDIR; }
		/* parent is this directory, name is basename of src_path */
		snprintf(parent_path, PATH_MAX, "%s", tmp_dst); /* normalized already */
		/* strip trailing slash so split_parent_name doesn't choke, but we can just set parent_ino directly */
		/* basename of src_path */
		const char *base = strrchr(src, '/');
		if (base) base++;
		else base = src;
		if (strlen(base) >= EXT2_NAME_LEN) { close(src_fd); return -ENAMETOOLONG; }
		strncpy(name, base, EXT2_NAME_LEN);
	} else {
		/* not trailing slash: split parent/name */
		rc = split_parent_name(dst, parent_path, name);
		if (rc != 0) { close(src_fd); return (rc == -ENAMETOOLONG ? -ENAMETOOLONG : -EINVAL); }
	}

	/* find parent inode */
	int parent_ino = path_lookup(parent_path);
	if (parent_ino < 0) { close(src_fd); return -ENOENT; }
	struct ext2_inode *parent_inode = get_inode(parent_ino);
	if (!S_ISDIR(parent_inode->i_mode)) { close(src_fd); return -ENOTDIR; }

	/* Check if name exists under parent */
	int existing = find_dir_entry(parent_inode, name);
	int target_ino = -1;
	int is_existing_file = 0;

	if (existing >= 0) {
		target_ino = existing;
		struct ext2_inode *t_inode = get_inode(target_ino);
		/* if symlink -> EEXIST per spec */
		if ((t_inode->i_mode & 0xF000) == EXT2_S_IFLNK) {
			close(src_fd);
			return -EEXIST;
		}
		/* if directory - then cp should copy into that directory instead:
		 * e.g., cp src /foo/bar  where bar is directory => create /foo/bar/srcbasename
		 */
		if (S_ISDIR(t_inode->i_mode)) {
			/* parent becomes that directory, name becomes basename(src) */
			parent_ino = target_ino;
			parent_inode = get_inode(parent_ino);
			const char *base = strrchr(src, '/');
			if (base) base++;
			else base = src;
			if (strlen(base) >= EXT2_NAME_LEN) { close(src_fd); return -ENAMETOOLONG; }
			strncpy(name, base, EXT2_NAME_LEN);
			/* check if this name already exists in new parent */
			int exists2 = find_dir_entry(parent_inode, name);
			if (exists2 >= 0) {
				/* if it's symlink -> EEXIST, if file -> overwrite, if dir -> EEXIST */
				struct ext2_inode *e2 = get_inode(exists2);
				if ((e2->i_mode & 0xF000) == EXT2_S_IFLNK) { close(src_fd); return -EEXIST; }
				if (S_ISDIR(e2->i_mode)) { close(src_fd); return -EEXIST; }
				/* else will overwrite file exists2 */
				target_ino = exists2;
				is_existing_file = 1;
			} else {
				target_ino = -1; /* will create new file */
			}
		} else if ((t_inode->i_mode & 0xF000) == EXT2_S_IFREG) {
			/* regular file exists: will overwrite */
			is_existing_file = 1;
		} else {
			/* other types -> treat as error */
			close(src_fd);
			return -EEXIST;
		}
	} else if (existing == -ENOENT) {
		target_ino = -1; /* will create new inode */
	} else {
		/* unexpected error */
		close(src_fd);
		return existing;
	}

	/* If overwriting an existing file, free its blocks and reuse inode */
	int use_ino = -1;
	if (is_existing_file && target_ino > 0) {
		/* free data blocks of existing inode, but keep inode number to reuse */
		free_inode_blocks_locked(target_ino);
		use_ino = target_ino;
	} else {
		/* need to allocate a new inode */
		int new_ino = alloc_inode();
		if (new_ino < 0) { close(src_fd); return -ENOSPC; }
		use_ino = new_ino;
	}

	/* build new inode struct locally, then write it later */
	struct ext2_inode new_inode;
	memset(&new_inode, 0, sizeof(new_inode));

	/* set inode fields for regular file */
	new_inode.i_mode = EXT2_S_IFREG | 0644; /* file type + permissions (permissions not important) */
	new_inode.i_size = 0; /* set after writing data */
	new_inode.i_links_count = 1;
	new_inode.i_dtime = 0;
	new_inode.i_ctime = (uint32_t)time(NULL);
	new_inode.i_mtime = new_inode.i_ctime;
	new_inode.i_atime = new_inode.i_ctime;

	/* write data into inode (allocates blocks, writes them) */
	/* We must place file data starting from beginning of host_fd */
	if (lseek(src_fd, 0, SEEK_SET) < 0) {
		close(src_fd);
		/* if we allocated the inode and not overwriting, free it */
		if (!is_existing_file && use_ino > 0) free_inode(use_ino);
		return -EIO;
	}

	int wres = write_data_into_inode(src_fd, &new_inode, filesize);
	if (wres != 0) {
		close(src_fd);
		/* if we wrote some blocks but failed, best-effort cleanup:
		   free blocks allocated in new_inode. */
		if (!is_existing_file && use_ino > 0) {
			/* free blocks referenced in new_inode */
			pthread_mutex_lock(&inode_locks[use_ino - 1]);
			for (int i = 0; i < DIRECT_POINTERS; i++) {
				if (new_inode.i_block[i] != 0) free_block(new_inode.i_block[i]);
			}
			if (new_inode.i_block[INDIRECT_INDEX] != 0) {
				uint32_t *ptrs = (uint32_t *) get_block(new_inode.i_block[INDIRECT_INDEX]);
				int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
				for (int i = 0; i < per_block; i++) if (ptrs[i] != 0) free_block(ptrs[i]);
				free_block(new_inode.i_block[INDIRECT_INDEX]);
			}
			pthread_mutex_unlock(&inode_locks[use_ino - 1]);
			free_inode(use_ino);
		}
		return wres;
	}

	/* write inode to disk (write_inode locks inode internally) */
	write_inode(use_ino, &new_inode);

	/* if we created a brand-new inode, add directory entry in parent */
	if (!is_existing_file) {
		int addc = add_dir_entry(parent_ino, name, use_ino, EXT2_FT_REG_FILE);
		if (addc < 0) {
			/* cleanup: free blocks & inode */
			free_inode_blocks_locked(use_ino);
			free_inode(use_ino);
			close(src_fd);
			return -ENOSPC;
		}
	} else {
		/* overwrote existing file: nothing to add to parent */
	}

	close(src_fd);
	return 0;

}
