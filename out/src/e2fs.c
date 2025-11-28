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

/**
 * TODO: Make sure to add all necessary includes here
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "e2fs.h"
#include "ext2fsal.h"

 /**
  * TODO: Add any helper implementations here
  */

int test_bit(uint8_t *bitmap, int n) {
    return bitmap[n / 8] & (1 << (n % 8));
}

void set_bit(uint8_t *bitmap, int n) {
    bitmap[n / 8] |= (1 << (n % 8));
}

void clear_bit(uint8_t *bitmap, int n) {
    bitmap[n / 8] &= ~(1 << (n % 8));
}

// initializes all the locks
void locks_init(int num_inodes, int num_blocks) {

        inode_locks = malloc(sizeof(pthread_mutex_t) * num_inodes);
        block_locks = malloc(sizeof(pthread_mutex_t) * num_blocks);
	pthread_mutex_init(&inode_bitmap_lock, NULL);
	pthread_mutex_init(&block_bitmap_lock, NULL);
        for (int i = 0; i < num_inodes; i++) pthread_mutex_init(&inode_locks[i], NULL);
        for (int i = 0; i < num_blocks; i++) pthread_mutex_init(&block_locks[i], NULL);
}

// destroys all locks and frees the allocated space for locks
void locks_destroy(int num_inodes, int num_blocks) {
        for (int i = 0; i < num_inodes; i++) pthread_mutex_destroy(&inode_locks[i]);
        for (int i = 0; i < num_blocks; i++) pthread_mutex_destroy(&block_locks[i]);
	
	pthread_mutex_destroy(&inode_bitmap_lock);
	pthread_mutex_destroy(&block_bitmap_lock);
        free(inode_locks);
        free(block_locks);
}









// NOTE ABOUT THE HELPERS: alot of them rn lock within the helpers, 
// but im not sure if its better if we control the locks outside of 
// these helpers. I feel like it would be more efficient outside idk ;-; 


// allocate an inode, returns inode number (1-based) or -1 if none free
int alloc_inode() {
	pthread_mutex_lock(&inode_bitmap_lock);

	for (int i = EXT2_GOOD_OLD_FIRST_INO; i < num_inodes; i++) {
		if (!test_bit(inode_bitmap, i)) {
			set_bit(inode_bitmap, i);
			group_desc->bg_free_inodes_count--;
			superblock->s_free_inodes_count--;
			pthread_mutex_unlock(&inode_bitmap_lock);
			return i + 1; // inode numbers start at 1
		}
	}

	pthread_mutex_unlock(&inode_bitmap_lock);
	return -1;
}

// allocate a block, returns block number (0-based) or -1 if none free
int alloc_block() {
	pthread_mutex_lock(&block_bitmap_lock);

	for (int i = 0; i < num_blocks; i++) {
		if (!test_bit(block_bitmap, i)) {
			set_bit(block_bitmap, i);
			group_desc->bg_free_blocks_count--;
			superblock->s_free_blocks_count--;
			pthread_mutex_unlock(&block_bitmap_lock);
			return i;
		}
	}

	pthread_mutex_unlock(&block_bitmap_lock);
	return -1;
}

// get pointer to inode structure
struct ext2_inode* get_inode(int ino) {
	return &inode_table[ino - 1]; // inode numbers start at 1
}

// write inode contents back to table (already in memory, just copy)
void write_inode(int ino, struct ext2_inode* inode) {
	pthread_mutex_lock(&inode_locks[ino - 1]);
	struct ext2_inode* dest = get_inode(ino);
	*dest = *inode;
	pthread_mutex_unlock(&inode_locks[ino - 1]);
}

// get pointer to block
char* get_block(int block_num) {
	return (char*)(fs + block_num * EXT2_BLOCK_SIZE);
}

// write data to a block
void write_block(int block_num, char* data) {
	pthread_mutex_lock(&block_locks[block_num]);
	char* blk = get_block(block_num);
	memcpy(blk, data, EXT2_BLOCK_SIZE);
	pthread_mutex_unlock(&block_locks[block_num]);
}

// traverse to next dir entry based on rec_len of entry, returns next dir entry
struct ext2_dir_entry *next_dir_entry(struct ext2_dir_entry *entry) {
        return (struct ext2_dir_entry *)((uint8_t *)entry + entry->rec_len);
} 



// add a dir entry (child_ino) to parent_ino, returns 0 on success
// type is the directory entry file type (e.g., EXT2_FT_DIR, EXT2_FT_REG_FILE, EXT2_FT_SYMLINK)
int add_dir_entry(int parent_ino, const char* name, int child_ino, uint8_t type) {
	struct ext2_inode* dir_inode = get_inode(parent_ino);
	if (!S_ISDIR(dir_inode->i_mode)) {
    		return -ENOTDIR;  // parent must be a directory
	}

	int name_len = strlen(name);
	int needed = dir_entry_rec_len(name_len);

	pthread_mutex_lock(&inode_locks[parent_ino - 1]);

	// find last used block
	int last_block_index = -1;
	for (int i = 0; i < DIRECT_POINTERS; i++) {
		if (dir_inode->i_block[i] != 0) last_block_index = i;
	}

	// allocate a new block if none used
	if (last_block_index == -1) {
		int new_block = alloc_block();
		if (new_block < 0) {
			pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
			return -ENOSPC;
		}
		dir_inode->i_block[0] = new_block;
		dir_inode->i_size = EXT2_BLOCK_SIZE;
		dir_inode->i_blocks += EXT2_BLOCK_SIZE / 512;
		char* blk = get_block(new_block);
		memset(blk, 0, EXT2_BLOCK_SIZE);

		struct ext2_dir_entry* entry = (struct ext2_dir_entry*) blk;
		entry->inode = child_ino;
		entry->name_len = name_len;
		entry->file_type = type;
		memcpy(entry->name, name, name_len);
		entry->rec_len = EXT2_BLOCK_SIZE;  // first entry takes the whole block

		if (type == EXT2_FT_DIR) group_desc->bg_used_dirs_count++;

		pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
		return 0;
	}

	// block exists, check if space available to append in the last block
	int block_num = dir_inode->i_block[last_block_index];
	pthread_mutex_lock(&block_locks[block_num]);
	char* blk = get_block(block_num);
	struct ext2_dir_entry* entry = (struct ext2_dir_entry*) blk;
	struct ext2_dir_entry* last = NULL;

	// go to last entry in the block
	while ((char*)entry < blk + EXT2_BLOCK_SIZE && entry->rec_len > 0) {
		last = entry;
		if ((char*)entry + entry->rec_len >= blk + EXT2_BLOCK_SIZE) break;
		entry = next_dir_entry(entry);
	}

	if (last != NULL) {
		// check if we can split the last entry's rec_len
		int actual_size = dir_entry_rec_len(last->name_len);
		int remain = last->rec_len - actual_size;

		if (remain >= needed) {
			// there is room: shrink last entry and append new one
			last->rec_len = actual_size;
			struct ext2_dir_entry* new_entry = (struct ext2_dir_entry*)((char*)last + actual_size);
			new_entry->inode = child_ino;
			new_entry->name_len = name_len;
			new_entry->file_type = type;
			memcpy(new_entry->name, name, name_len);
			new_entry->rec_len = remain;

			if (type == EXT2_FT_DIR) group_desc->bg_used_dirs_count++;

			pthread_mutex_unlock(&block_locks[block_num]);
			pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
			return 0;
		}
	}

	// no space in last block, need to allocate new block
	pthread_mutex_unlock(&block_locks[block_num]);

	if (last_block_index + 1 >= DIRECT_POINTERS) {
		// out of direct block pointers
		pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
		return -ENOSPC;
	}

	int new_block = alloc_block();
	if (new_block < 0) {
		pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
		return -ENOSPC;
	}
	dir_inode->i_block[last_block_index + 1] = new_block;
	dir_inode->i_size += EXT2_BLOCK_SIZE;
	dir_inode->i_blocks += EXT2_BLOCK_SIZE / 512;
	char* new_blk = get_block(new_block);
	memset(new_blk, 0, EXT2_BLOCK_SIZE);

	struct ext2_dir_entry* new_entry = (struct ext2_dir_entry*) new_blk;
	new_entry->inode = child_ino;
	new_entry->name_len = name_len;
	new_entry->file_type = type;
	memcpy(new_entry->name, name, name_len);
	new_entry->rec_len = EXT2_BLOCK_SIZE;  // takes the whole block

	if (type == EXT2_FT_DIR) group_desc->bg_used_dirs_count++;

	pthread_mutex_unlock(&inode_locks[parent_ino - 1]);
	return 0;
}



// looks for a child named name inside parent directory inode dir.
// returns inode number on success, or -ENOENT if not found.
int find_dir_entry(struct ext2_inode *dir, const char *name)
{
	int block;
	// loop through blocks in the inode
	for (int i = 0; i < 15; i++) {

		block = dir->i_block[i];
		if (block == 0)
			continue;	// no block

		// directory block start
		uint8_t *block_ptr = (uint8_t *)get_block(block);

		// go through entries inside this block
		struct ext2_dir_entry *entry = (struct ext2_dir_entry *) block_ptr;
		uint8_t *end = block_ptr + EXT2_BLOCK_SIZE;

		while ((uint8_t *)entry < end && entry->rec_len > 0) {

			if (entry->inode != 0 &&
			    entry->name_len == strlen(name) &&
			    strncmp(entry->name, name, entry->name_len) == 0) {

				return entry->inode;
			}

			entry = next_dir_entry(entry);
		}
	}

	return -ENOENT;
}

//gives ino number for path if found, otherwise return -ENOENT
int path_lookup(const char *path)
{
	if (path == NULL) return -ENOENT;

	// writable copy of the path for strtok_r
	char buf[PATH_MAX];
	strncpy(buf, path, PATH_MAX);
	buf[PATH_MAX - 1] = '\0';

	if (strcmp(buf, "/") == 0) {
		return EXT2_ROOT_INO;
	}

	int curr_ino = EXT2_ROOT_INO;
	struct ext2_inode *curr_inode = get_inode(curr_ino);

	// skip leading '/'
        char *saveptr;
        char *token = strtok_r(buf, "/", &saveptr);

	while (token != NULL) {

		// stay in same directory)
		if (strcmp(token, ".") == 0) {
			token = strtok_r(NULL, "/", &saveptr);
			continue;
		}

		// parent directory ".."
		if (strcmp(token, "..") == 0) {
			// root's parent is root
			curr_ino = 2;
			curr_inode = get_inode(curr_ino);
			token = strtok_r(NULL, "/", &saveptr);
			continue;
		}

		// nned to be directory to continue
		if (!S_ISDIR(curr_inode->i_mode)) {
			return -ENOTDIR;
		}

		int child_ino = find_dir_entry(curr_inode, token);
		if (child_ino < 0) {
			return -ENOENT;
		}

		curr_ino = child_ino;
		curr_inode = get_inode(curr_ino);

		token = strtok_r(NULL, "/", &saveptr);
	}

	return curr_ino;
}

// compute the minimum directory entry record length (aligned to 4)
int dir_entry_rec_len(int name_len) {
	int need = 8 + name_len;          // inode(4) + rec_len(2) + name_len(1) + file_type(1)
	return (need + 3) & ~3;           // round up to 4 bytes
}

// free an inode number (ino is 1-based)
void free_inode(int ino) {
	int idx = ino - 1;
	pthread_mutex_lock(&inode_bitmap_lock);
	clear_bit(inode_bitmap, idx);
	group_desc->bg_free_inodes_count++;
	superblock->s_free_inodes_count++;
	pthread_mutex_unlock(&inode_bitmap_lock);
}

// free a block number (block_num is 0-based)
void free_block(int block_num) {
	pthread_mutex_lock(&block_bitmap_lock);
	clear_bit(block_bitmap, block_num);
	group_desc->bg_free_blocks_count++;
	superblock->s_free_blocks_count++;
	pthread_mutex_unlock(&block_bitmap_lock);
}

// strips trailing slashes (but leaves single leading '/')
void strip_trailing_slashes(char *s) {
	size_t len = strlen(s);
	while (len > 1 && s[len - 1] == '/') {
		s[len - 1] = '\0';
		len--;
	}
}

// split path into parent path and final name
// parent_buf must be large enough (PATH_MAX)
// name_buf must be large enough (EXT2_NAME_LEN or PATH_MAX)
// returns 0 on success, -ENOENT on invalid path.
int split_parent_name(const char *path, char *parent_buf, char *name_buf) {
	if (path == NULL) return -ENOENT;
	if (path[0] != '/') return -ENOENT;   // want absolute path

	// copy and normalize
	char tmp[PATH_MAX];
	strncpy(tmp, path, PATH_MAX);
	tmp[PATH_MAX - 1] = '\0';
	strip_trailing_slashes(tmp);

	// special case: trying to create root ("/")
	if (strcmp(tmp, "/") == 0) return -ENOENT;

	// find last '/'
	char *last = strrchr(tmp, '/');
	if (last == NULL) return -ENOENT;

	// parent is "/" if last points to first char 
	if (last == tmp) {
		strncpy(parent_buf, "/", PATH_MAX);
		parent_buf[PATH_MAX - 1] = '\0';
	} else {
		size_t plen = last - tmp;
		if (plen >= PATH_MAX) return -EINVAL;
		strncpy(parent_buf, tmp, plen);
		parent_buf[plen] = '\0';
	}

	// final name 
	char *name = last + 1;
	if (strlen(name) == 0) return -EINVAL;
	if (strlen(name) >= EXT2_NAME_LEN) return -ENAMETOOLONG;
	strncpy(name_buf, name, EXT2_NAME_LEN);
	name_buf[EXT2_NAME_LEN - 1] = '\0';

	return 0;
}

// helper to free all data blocks of an inode (direct + single indirect).
// keeps locking order: lock the inode before touching its block pointers.
// returns 0 on success.
void free_inode_blocks_locked(int ino)
{
	pthread_mutex_lock(&inode_locks[ino - 1]);
	struct ext2_inode *inode = get_inode(ino);

	// free direct blocks 
	for (int i = 0; i < DIRECT_POINTERS; i++) {
		if (inode->i_block[i] != 0) {
			free_block(inode->i_block[i]);
			inode->i_block[i] = 0;
		}
	}

	// single indirect
	if (inode->i_block[INDIRECT_INDEX] != 0) {
		int indirect_blk = inode->i_block[INDIRECT_INDEX];
		// read the block of uint32_t block numbers 
		uint32_t *ptrs = (uint32_t *) get_block(indirect_blk);
		// number of pointers per block
		int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
		for (int i = 0; i < per_block; i++) {
			if (ptrs[i] != 0) {
				free_block(ptrs[i]);
			}
		}
		// free the indirect block
		free_block(indirect_blk);
		inode->i_block[INDIRECT_INDEX] = 0;
	}

	inode->i_blocks = 0;
	inode->i_size = 0;
	pthread_mutex_unlock(&inode_locks[ino - 1]);
}

// helper to write data blocks from an open host fd into an inode.
// returns 0 on success, -ENOSPC if allocation fails, or -EIO for read error.
int write_data_into_inode(int host_fd, struct ext2_inode *inode, off_t filesize)
{
	ssize_t remaining = filesize;
	int written_blocks = 0;

	// zero out inode block pointers first
	for (int i = 0; i < TOTAL_POINTERS; i++) inode->i_block[i] = 0;

	// direct blocks
	for (int di = 0; di < DIRECT_POINTERS && remaining > 0; di++) {
		int b = alloc_block();
		if (b < 0) return -ENOSPC;
		inode->i_block[di] = b;

		// read from host_fd
		char buf[EXT2_BLOCK_SIZE];
		ssize_t r = read(host_fd, buf, EXT2_BLOCK_SIZE);
		if (r < 0) return -EIO;
		if (r < EXT2_BLOCK_SIZE) memset(buf + r, 0, EXT2_BLOCK_SIZE - r);

		// write_block handles block locking
		write_block(b, buf);

		remaining -= (r > 0 ? r : 0);
		written_blocks++;
	}

	// single indirect
	if (remaining > 0) {
		// allocate indirect
		int indirect_blk = alloc_block();
		if (indirect_blk < 0) return -ENOSPC;
		inode->i_block[INDIRECT_INDEX] = indirect_blk;

		// prepare pointer array in memory
		uint32_t *ptrs = (uint32_t *) malloc(EXT2_BLOCK_SIZE);
		if (!ptrs) return -EIO;
		memset(ptrs, 0, EXT2_BLOCK_SIZE);

		int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
		for (int i = 0; i < per_block && remaining > 0; i++) {
			int b = alloc_block();
			if (b < 0) {
				free(ptrs);
				return -ENOSPC;
			}
			ptrs[i] = b;

			char buf[EXT2_BLOCK_SIZE];
			ssize_t r = read(host_fd, buf, EXT2_BLOCK_SIZE);
			if (r < 0) {
				free(ptrs);
				return -EIO;
			}
			if (r < EXT2_BLOCK_SIZE) memset(buf + r, 0, EXT2_BLOCK_SIZE - r);

			write_block(b, buf);
			remaining -= (r > 0 ? r : 0);
			written_blocks++;
		}

		//  write the indirect block contents
		write_block(indirect_blk, (char *)ptrs);
		free(ptrs);
	}

	inode->i_size = filesize;
	inode->i_blocks = written_blocks * (EXT2_BLOCK_SIZE / 512);
	return 0;
}
