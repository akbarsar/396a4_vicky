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
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "e2fs.h"
#include "ext2fsal.h"
#include <stdio.h>

// ----------------------- BITMAP OPERATION HELPERS ----------------

/*
 * Tests if bit n is set in the bitmap.
 * Returns: 0 if the bit is clear, a non-zero value otherwise.
 */
int test_bit(uint8_t *bitmap, int n) {
    return bitmap[n / 8] & (1 << (n % 8));
}

/*
 * Sets bit n in the bitmap.
 */
void set_bit(uint8_t *bitmap, int n) {
    bitmap[n / 8] |= (1 << (n % 8));
}

/*
 * Clears bit n in the bitmap.
 */
void clear_bit(uint8_t *bitmap, int n) {
    bitmap[n / 8] &= ~(1 << (n % 8));
}

// -------------------- SYNCHRONIZATION HELPERS  ----------------

/*
 * Initializes a mutex and checks for error.
 */
static void mutex_init(pthread_mutex_t *mutex) {
	
	if (pthread_mutex_init(mutex, NULL) != 0) {
		fprintf(stderr, "mutex init failed\n");
		exit(EXIT_FAILURE);
	}
}

/*
 * Locks a mutex and checks for error.
 */
void mutex_lock(pthread_mutex_t *mutex) {
	
	if (pthread_mutex_lock(mutex) != 0) {
		fprintf(stderr, "mutex lock failed\n");
		exit(EXIT_FAILURE);
	}
}

/*
 * Unlocks a mutex and checks for error.
 */
void mutex_unlock(pthread_mutex_t *mutex) {

	if (pthread_mutex_unlock(mutex) != 0) {
		fprintf(stderr, "mutex unlock failed\n");
		exit(EXIT_FAILURE);
	}
}

/*
 * Destroys a mutex and checks for error.
 */
static void mutex_destroy(pthread_mutex_t *mutex) {
	if (pthread_mutex_destroy(mutex) != 0) {
		fprintf(stderr, "mutex destroy failed\n");
		exit(EXIT_FAILURE);
	}
}

/*
 * Initializes all synchronization locks for thread-safe filesystem 
 * access. Creates per-inode locks, per-block locks, and bitmap locks.
 */
void locks_init(int num_inodes, int num_blocks) {

	// allocate lock arrays
        inode_locks = malloc(sizeof(pthread_mutex_t) * num_inodes);
	if (inode_locks == NULL) {
		perror("malloc failed");
		exit(EXIT_FAILURE);
	}

        block_locks = malloc(sizeof(pthread_mutex_t) * num_blocks);
	if (block_locks == NULL) {
		perror("malloc failed");
		exit(EXIT_FAILURE);
	}

	// initialize bitmap locks
	mutex_init(&inode_bitmap_lock);
	mutex_init(&block_bitmap_lock);

	// initialize per-inode and per-block locks
        for (int i = 0; i < num_inodes; i++) {
		mutex_init(&inode_locks[i]);
	}

	for (int i = 0; i < num_blocks; i++) {
		mutex_init(&block_locks[i]);
	}

}

/*
 * Destroys all synchronization locks and frees the allocated memory.
 */
void locks_destroy(int num_inodes, int num_blocks) {

	// destroy per-inode and per-block locks
	for (int i = 0; i < num_inodes; i++) mutex_destroy(&inode_locks[i]);
	
	for (int i = 0; i < num_blocks; i++) mutex_destroy(&block_locks[i]);

	// destroy bitmap locks	
	mutex_destroy(&inode_bitmap_lock);
	mutex_destroy(&block_bitmap_lock);

	// free lock arrays
        free(inode_locks);
        free(block_locks);
	block_locks = NULL;
	inode_locks = NULL;
}


// NOTE ABOUT THE HELPERS (REMOVE LATER): alot of them rn lock within the helpers, 
// but im not sure if its better if we control the locks outside of 
// these helpers. I feel like it would be more efficient outside idk ;-; 


// ----------------- INODE ACCESS/ALLOCATION HELPERS ----------------

/*
 * Allocates a new inode from the bitmap:
 * - Searches for the first free inode starting from the first 
 *   non-reserved inode.
 * - Updates both group descriptor and superblock free inode counts.
 *
 * Returns: inode number (1-based) on success, -1 if no free inodes.
 */
int alloc_inode() {
	mutex_lock(&inode_bitmap_lock);

	// search for first free inode (skip reserved inodes 0-10)
	for (int i = EXT2_GOOD_OLD_FIRST_INO; i < num_inodes; i++) {
		// if inode is free, allocate new inode
		if (!test_bit(inode_bitmap, i)) {
			set_bit(inode_bitmap, i);
			group_desc->bg_free_inodes_count--;
			superblock->s_free_inodes_count--;
			
			mutex_unlock(&inode_bitmap_lock);
			
			// return inode number
			return i + 1;  // inode numbers start at 1
		}
	}

	mutex_unlock(&inode_bitmap_lock);
	return -1;  // No free inodes
}

/*
 * Frees the given inode (1-based index) and updates filesystem 
 * counts.
 */
void free_inode(int inode) {
	int idx = inode - 1;  // convert to 0-based index

	mutex_lock(&inode_bitmap_lock);

	clear_bit(inode_bitmap, idx);
	group_desc->bg_free_inodes_count++;
	superblock->s_free_inodes_count++;

	mutex_unlock(&inode_bitmap_lock);
}

// get pointer to inode structure
/*
 * Returns the inode struct for the given inode index (1-based) 
 * from the inode table.
 *
 * Returns: a pointer to an inode struct.
 */
struct ext2_inode* get_inode(int ino) {
	return &inode_table[ino - 1];  // convert to 0-based index
}

/*
 * Writes the given inode structure to the inode table at index 
 * ino.
 */
void write_inode(int ino, struct ext2_inode* inode) {
	mutex_lock(&inode_locks[ino - 1]);

	struct ext2_inode* dest = get_inode(ino);
	*dest = *inode;

	mutex_unlock(&inode_locks[ino - 1]);
}

// ----------------- BLOCK ACCESS/ALLOCATION HELPERS ----------------

/*
 * Allocates a new data block from the bitmap.
 * Updates both group descriptor and superblock free block counts.
 *
 * Returns: the block number (0-based) on success, or -1 if no free 
 * blocks.
 */
int alloc_block() {

	mutex_lock(&block_bitmap_lock);

	// scan bitmap for the first free block
	for (int i = 0; i < num_blocks; i++) {
		// if free, allocate block & update bookkeeping
		if (!test_bit(block_bitmap, i)) {
			set_bit(block_bitmap, i);
			group_desc->bg_free_blocks_count--;
			superblock->s_free_blocks_count--;

			mutex_unlock(&block_bitmap_lock);

			// return block number
			return i;
		}
	}

	mutex_unlock(&block_bitmap_lock);

	// no free blocks
	return -1;
}


/**
 * Returns: a pointer to the start of the data block with block 
 * number block_num (0-based).
 */
char* get_block(int block_num) {
	return (char*)(fs + block_num * EXT2_BLOCK_SIZE);
}

/*
 * Writes data to the block with block number block_num (0-based).
 */
void write_block(int block_num, char* data) {
	mutex_lock(&block_locks[block_num]);

	char* block = get_block(block_num);
	memcpy(block, data, EXT2_BLOCK_SIZE);

	mutex_unlock(&block_locks[block_num]);
}

/*
 * Frees the data block with block number block_num (0-based) and 
 * updates filesystem counts.
 */
void free_block(int block_num) {
	mutex_lock(&block_bitmap_lock);

	clear_bit(block_bitmap, block_num);
	group_desc->bg_free_blocks_count++;
	superblock->s_free_blocks_count++;

	mutex_unlock(&block_bitmap_lock);
}

// ---------------------  DIRECTORY OPERATIONS ---------------------

/*
 * Returns: struct entry's next directory entry in a block based 
 * on rec_len.
 */
struct ext2_dir_entry *next_dir_entry(struct ext2_dir_entry *entry) {
	
	return (struct ext2_dir_entry *)((uint8_t *)entry + entry->rec_len);
} 

/*
 * Calculates the minimum record length for a directory entry 
 * with a name of length name_len (4-byte aligned).
 *
 * Structure: inode(4)+rec_len(2)+name_len(1)+file_type(1)+name
 *
 * Returns: the required record length in bytes, 4-byte aligned.
 */
int dir_entry_rec_len(int name_len) {
	
	int need = 8 + name_len;  // fixed header (8 bytes) + name
	return (need + 3) & ~3;   // round up to 4-byte boundary
}

/*
 * Initializes a directory entry with the given inode number, name
 * (with length name_len), and type. The given rec_len will be the 
 * length of this entry.
 * 
 */
static void init_dir_entry(struct ext2_dir_entry* entry, 
		int child_inode, const char* name, int name_len, 
		uint8_t type, int rec_len) {
	
	entry->inode = child_inode;
	entry->name_len = name_len;
	entry->file_type = type;
	memcpy(entry->name, name, name_len);
	entry->rec_len = rec_len;
}

/*
 * Finds the last directory entry in the given block; traverses 
 * through all entries in the block following rec_len pointers 
 * until reaching the final entry (one whose rec_len extends to
 * block end).
 *
 * Returns: a pointer to the last directory entry in the block.
 */
static struct ext2_dir_entry* find_last_dir_entry(char* block) {

	struct ext2_dir_entry* entry = (struct ext2_dir_entry*) block;
	struct ext2_dir_entry* last = NULL;
	// traverse entries until we reach the last one
	while ((char*)entry < block + EXT2_BLOCK_SIZE 
			&& entry->rec_len > 0) {
		
		last = entry;
		
		// check if this entry's rec_len extends to block end

		if ((char*)entry + entry->rec_len >= block + EXT2_BLOCK_SIZE) {
			break;
		}

		entry = next_dir_entry(entry);
	}

	// return the pointer to last directory
	return last;
}

/*
 * Creates a directory entry in a new block:
 *
 * - Allocates a new block, initializes it to zero, and creates a 
 *   single directory entry that spans the entire block.
 *
 * dir_inode: parent directory inode
 * block_index: index in i_block array for new block
 * child_inode: inode number for new entry
 * name: name of new entry
 * name_len: length of name
 * type: file type
 * 
 * Returns: block number on success, -1 and set errno to ENOSPC if 
 * no blocks available.
 */
static int create_entry_in_new_block(struct ext2_inode* dir_inode, 
		int block_index, int child_inode, const char* name, 
		int name_len, uint8_t type) {

	// allocate a new block
	int new_block = alloc_block();
	if (new_block < 0) {
		errno = ENOSPC;
		return -1;
	}

	// update directory inode to reference new block
	dir_inode->i_block[block_index] = new_block;
	dir_inode->i_size += (block_index == 0) ? EXT2_BLOCK_SIZE : EXT2_BLOCK_SIZE;
	dir_inode->i_blocks += EXT2_BLOCK_SIZE / 512;

	// initialize the new block with zeros
	char* block = get_block(new_block);
	memset(block, 0, EXT2_BLOCK_SIZE);

	// create entry spanning the entire block
	struct ext2_dir_entry* entry = (struct ext2_dir_entry*) block;
	init_dir_entry(entry, child_inode, name, name_len, type, EXT2_BLOCK_SIZE);

	// update directory count if adding a subdirectory
	if (type == EXT2_FT_DIR) {
		group_desc->bg_used_dirs_count++;
	}

	// return block number
	return new_block;
}

/*
 * Adds a new directory entry (childe_inode; 1-based) with the given
 * name and type to a parent directory parent_inode (1-based).
 *
 * The function handles three cases:
 * 1. No blocks allocated: creates first block with the entry
 * 2. Space in last block: splits last entry and appends new one
 * 3. No space in last block: allocates new block for the entry
 *
 * Returns: 0 on success, errno on error.
 */
int add_dir_entry(int parent_inode, const char* name, int child_inode, uint8_t type) {
	
	struct ext2_inode* dir_inode = get_inode(parent_inode);

	// verify parent is a directory
	if (!S_ISDIR(dir_inode->i_mode)) {
		return ENOENT;
	}

	int name_len = strlen(name);
	int needed = dir_entry_rec_len(name_len);

	mutex_lock(&inode_locks[parent_inode - 1]);

	// find last used block in directory
	int last_block_index = -1;
	for (int i = 0; i < DIRECT_POINTERS; i++) {
		if (dir_inode->i_block[i] != 0) last_block_index = i;
	}

	// CASE 1: no blocks allocated yet, create first block
	if (last_block_index == -1) {
		int result = create_entry_in_new_block(dir_inode, 
				0, child_inode, name, name_len, type);
		
		if (result < 0) {
			int saved_errno = errno ? errno : ENOSPC;
			mutex_unlock(&inode_locks[parent_inode - 1]);
			return saved_errno;
		}

		// set size correctly for first block
		dir_inode->i_size = EXT2_BLOCK_SIZE;
		mutex_unlock(&inode_locks[parent_inode - 1]);
		return 0;
	}


	// CASE 2: block exists, try to append to the last block
	int block_num = dir_inode->i_block[last_block_index];

	mutex_lock(&block_locks[block_num]);

	char* block = get_block(block_num);
	struct ext2_dir_entry* last = find_last_dir_entry(block);
	
	// try to split the last entry's rec_len to make room
	if (last != NULL) {
		int actual_size = dir_entry_rec_len(last->name_len);
		int remain = last->rec_len - actual_size;

		// ensure last entry doesn't extend beyond block
		if ((char*)last + last->rec_len > block + EXT2_BLOCK_SIZE) {
			mutex_unlock(&block_locks[block_num]);
			mutex_unlock(&inode_locks[parent_inode - 1]);
			return ENOSPC;
		}

		if (remain >= needed) {
			
			// there is room: shrink last entry and 
			// append new one
			last->rec_len = actual_size;
			struct ext2_dir_entry* new_entry = (struct ext2_dir_entry*)((char*)last + actual_size);
			
			init_dir_entry(new_entry, child_inode, name, name_len, type, remain);
			
			if (type == EXT2_FT_DIR) {
				group_desc->bg_used_dirs_count++;
			}
			
			mutex_unlock(&block_locks[block_num]);
			mutex_unlock(&inode_locks[parent_inode - 1]);
			
			return 0;
		}
	}

	// CASE 3: no space in last block, allocate new block

	mutex_unlock(&block_locks[block_num]);

	if (last_block_index + 1 >= DIRECT_POINTERS) {
		
		// out of direct block pointers
		mutex_unlock(&inode_locks[parent_inode - 1]);
		return ENOSPC;
	}

	int result = create_entry_in_new_block(dir_inode, 
			last_block_index + 1, child_inode, name, 
			name_len, type);
	
	if (result < 0) {
		int saved_err = errno ? errno : ENOSPC;
		mutex_unlock(&inode_locks[parent_inode - 1]);
		return saved_err;
	}
	
	mutex_unlock(&inode_locks[parent_inode - 1]);
	return 0;
}

/*
 * Finds a directory entry by name within a directory dir.
 * Searches through all blocks in the directory's i_block array.
 *
 * Returns: inode number of the entry on success, -1 if not 
 * found.
 */
int find_dir_entry(struct ext2_inode *dir, const char *name) {

	size_t target_len = strlen(name);
	int block;

	// search through all block pointers
	for (int i = 0; i < TOTAL_POINTERS; i++) {

		block = dir->i_block[i];
		if (block == 0)
			continue;  // empty block pointer

		// directory block start
		uint8_t *block_ptr = (uint8_t *)get_block(block);
		struct ext2_dir_entry *entry = (struct ext2_dir_entry *) block_ptr;
		uint8_t *end = block_ptr + EXT2_BLOCK_SIZE;

		// iterate through entries in this block
		while ((uint8_t *)entry < end && entry->rec_len > 0) {

			if (entry->inode != 0 &&
			    entry->name_len == target_len &&
			    strncmp(entry->name, name, entry->name_len) == 0) {

				return entry->inode;
			}

			entry = next_dir_entry(entry);
		}
	}

	errno = ENOENT;
	return -1;  // not found
}

// ----------------------- PATH OPERATIONS -------------------------

/*
 * Resolves an absolute path (must start with "/") to an inode 
 * number. Handles "." (current directory) and ".." (parent directory)
 * components.
 *
 * Returns: inode number on success, -1 and sets errno on error.
 */
int path_lookup(const char *path) {
	if (path == NULL) {
		errno = ENOENT;
		return -1;
	}

	// ensure path is absolute (must start with '/')
	if (path[0] != '/') {
		errno = ENOENT;
		return -1;
	}

	// create a writable copy of the path for tokenization
	char buf[PATH_MAX];
	strncpy(buf, path, PATH_MAX);
	buf[PATH_MAX - 1] = '\0';

	// root directory handling
	if (strcmp(buf, "/") == 0) {
		return EXT2_ROOT_INO;
	}

	int curr_ino = EXT2_ROOT_INO;
	struct ext2_inode *curr_inode = get_inode(curr_ino);

	// tokenize path and traverse; skip leading '/'
        char *saveptr;
        char *token = strtok_r(buf, "/", &saveptr);

	while (token != NULL) {

		// handle "." -- stay in same directory
		if (strcmp(token, ".") == 0) {
			token = strtok_r(NULL, "/", &saveptr);
			continue;
		}

		// handle parent directory ".." -- go to parent
		if (strcmp(token, "..") == 0) {
			// wont be in middle of path.. 
			// root's parent is root
			curr_ino = EXT2_ROOT_INO;
			curr_inode = get_inode(curr_ino);
			token = strtok_r(NULL, "/", &saveptr);
			continue;
		}

		// current inode must be a directory to continue
		if (!S_ISDIR(curr_inode->i_mode)) {
			errno = ENOENT;
			return -1;
		}

		// find the child entry
		int child_ino = find_dir_entry(curr_inode, token);
		if (child_ino == -1) {
			errno = ENOENT;
			return -1;
		}

		curr_ino = child_ino;
		curr_inode = get_inode(curr_ino);

		token = strtok_r(NULL, "/", &saveptr);
	}

	// return child entry inode number
	return curr_ino;
}

/*
 * Strips trailing slashes from a path.
 * Preserves the leading '/' for the root directory.
 */
void strip_trailing_slashes(char *path) {
	size_t len = strlen(path);
	while (len > 1 && path[len - 1] == '/') {
		path[len - 1] = '\0';
		len--;
	}
}

/*
 * Splits an absolute path into parent directory path and final 
 * component name (ex: "/foo/bar/baz" -> parent="/foo/bar", 
 * name="baz").
 *
 * parent_buf: buffer for parent path (must be PATH_MAX bytes)
 * name_buf: buffer for final name (must be EXT2_NAME_LEN bytes)
 * 
 * Returns: 0 on success, errno on error.
 */
int split_parent_name(const char *path, char *parent_buf, char *name_buf) {
	if (path == NULL) return ENOENT;
	if (path[0] != '/') return ENOENT;   // want absolute path

	// copy and normalize the path
	char tmp[PATH_MAX];
	strncpy(tmp, path, PATH_MAX);
	tmp[PATH_MAX - 1] = '\0';
	strip_trailing_slashes(tmp);

	// special case: cannot create root ("/")
	if (strcmp(tmp, "/") == 0) return ENOENT;

	// find the last '/' separator
	char *last = strrchr(tmp, '/');
	if (last == NULL) return ENOENT;

	// extract parent path
	if (last == tmp) {
		// parent is "/" if last points to first char 
		strncpy(parent_buf, "/", PATH_MAX);
		parent_buf[PATH_MAX - 1] = '\0';
	}
	else {
		size_t parent_len = last - tmp;
		
		if (parent_len >= PATH_MAX) return ENAMETOOLONG;
		
		strncpy(parent_buf, tmp, parent_len);
		parent_buf[parent_len] = '\0';
	}

	// extract final name 
	char *name = last + 1;
	
	if (strlen(name) == 0) return ENOENT;
	if (strlen(name) >= EXT2_NAME_LEN) return ENAMETOOLONG;
	
	strncpy(name_buf, name, EXT2_NAME_LEN);
	name_buf[EXT2_NAME_LEN - 1] = '\0';

	return 0;
}

/*
 * Extracts basename from a path (last component after final '/').
 */
const char* get_path_basename(const char *path) {
    const char *base = strrchr(path, '/');
    return (base) ? base + 1 : path;
}

// ---------------------- FILE DATA OPERATIONS ----------------------

/*
 * Frees all data blocks of an inode ino (direct and single indirect),
 * clearing block pointers and resetting size/blocks fields.
 */
void free_inode_blocks_locked(int ino) {
	mutex_lock(&inode_locks[ino - 1]);
	struct ext2_inode *inode = get_inode(ino);

	// free direct blocks 
	for (int i = 0; i < DIRECT_POINTERS; i++) {
		if (inode->i_block[i] != 0) {
			free_block(inode->i_block[i]);
			inode->i_block[i] = 0;
		}
	}

	// free single indirect block and its referenced blocks
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

		free_block(indirect_blk);
		inode->i_block[INDIRECT_INDEX] = 0;
	}

	inode->i_blocks = 0;
	inode->i_size = 0;
	mutex_unlock(&inode_locks[ino - 1]);
}

/*
 * Writes file data from an open host file descriptor into an inode.
 * Allocates blocks as needed for direct and single indirect blocks.
 *
 * host_fd: open file descriptor to read from
 * inode: pointer to inode to populate (block pointers set here)
 * filesize: total size of file to write
 *
 * Returns: 0 on success, ENOSPC or EIO on error
 */
int write_data_into_inode(int host_fd, struct ext2_inode *inode, off_t filesize) {

	ssize_t remaining = filesize;
	int written_blocks = 0;

	// initialize all block pointers to zero
	for (int i = 0; i < TOTAL_POINTERS; i++) inode->i_block[i] = 0;

	// write data to direct blocks (up to 12 blocks)
	for (int di = 0; di < DIRECT_POINTERS && remaining > 0; di++) {
		int b = alloc_block();
		if (b < 0) return ENOSPC;
		inode->i_block[di] = b;

		// read data from host file descriptor
		char buf[EXT2_BLOCK_SIZE];
		ssize_t r = read(host_fd, buf, EXT2_BLOCK_SIZE);
		if (r < 0) return EIO;

		// initialize remainder of block to zero if needed
		if (r < EXT2_BLOCK_SIZE) memset(buf + r, 0, EXT2_BLOCK_SIZE - r);

		// write_block handles block locking
		write_block(b, buf);

		remaining -= (r > 0 ? r : 0);
		written_blocks++;
	}

	// write data to single indirect blocks if needed
	if (remaining > 0) {
		// allocate indirect block
		int indirect_blk = alloc_block();
		if (indirect_blk < 0) return ENOSPC;
		inode->i_block[INDIRECT_INDEX] = indirect_blk;

		// allocate array for block pointers
		uint32_t *ptrs = (uint32_t *) malloc(EXT2_BLOCK_SIZE);
		if (!ptrs) return ENOMEM;
		memset(ptrs, 0, EXT2_BLOCK_SIZE);

		int per_block = EXT2_BLOCK_SIZE / sizeof(uint32_t);
		for (int i = 0; i < per_block && remaining > 0; i++) {
			int b = alloc_block();
			if (b < 0) {
				free(ptrs);
				return ENOSPC;
			}
			ptrs[i] = b;

			char buf[EXT2_BLOCK_SIZE];
			ssize_t r = read(host_fd, buf, EXT2_BLOCK_SIZE);
			if (r < 0) {
				free(ptrs);
				return EIO;
			}
			if (r < EXT2_BLOCK_SIZE) memset(buf + r, 0, EXT2_BLOCK_SIZE - r);

			write_block(b, buf);
			remaining -= (r > 0 ? r : 0);
			written_blocks++;
		}

		//  write the indirect block with pointers
		write_block(indirect_blk, (char *)ptrs);
		free(ptrs);
	}

	// update inode metadata
	inode->i_size = filesize;
	inode->i_blocks = written_blocks * (EXT2_BLOCK_SIZE / 512);
	return 0;
}

/*
 * Initialize a new file inode with proper mode, timestamps, and 
 * link count.
 */
void init_file_inode(struct ext2_inode *inode) {
	memset(inode, 0, sizeof(*inode));
	inode->i_mode = EXT2_S_IFREG | 0644;
	inode->i_size = 0;
	inode->i_links_count = 1;
	inode->i_dtime = 0;
	inode->i_ctime = (uint32_t)time(NULL);
	inode->i_mtime = inode->i_ctime;
	inode->i_atime = inode->i_ctime;
}

// ------------------------- COPY HELPERS ---------------------------

/*
 * Opens and validates a source file for copying. Only regular files 
 * can be copied.
 *
 * src: path to source file on host filesystem
 * filesize: pointer to store size of the file in bytes
 * err: pointer to store error code if open fails
 *
 * Returns: file descriptor on success, -1 on error
 */
int open_source_file(const char *src, off_t *filesize, int *err) {
	int fd = open(src, O_RDONLY);
	if (fd < 0) {
		*err = ENOENT;
		return -1;
	}

	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		*err = ENOENT;
		return -1;
	}

	*filesize = st.st_size;
	return fd;
}

/*
 * Parse destination path and resolve parent directory for copy 
 * operation. Handles trailing slashes by treating them as directory 
 * paths.
 *
 * dst: destination path in ext2 filesystem
 * src: source path (used for basename extraction)
 * parent_ino: pointer to store parent directory inode number
 * name: pointer to store target filename (EXT2_NAME_LEN buffer)
 * 
 * Returns: 0 on success, errno on error
 */
int resolve_copy_destination(const char *dst, const char *src, 
		int *parent_ino, char *name) {

	// copy dst into a temporary buffer
	char parent_path[PATH_MAX];
	char tmp_dst[PATH_MAX];
	strncpy(tmp_dst, dst, PATH_MAX - 1);
	tmp_dst[PATH_MAX - 1] = '\0';

	// CASE 1: trailing slash, treat as directory, use source basename
	if (strlen(tmp_dst) > 1 && tmp_dst[strlen(tmp_dst) - 1] == '/') {
		// resolve the directory inode for dst
		int dir_ino = path_lookup(tmp_dst);
		if (dir_ino < 0) return ENOENT;  // doesnt exist

		struct ext2_inode *dir = get_inode(dir_ino);
		
		// ensure dst is a directory
		if (!S_ISDIR(dir->i_mode)) return ENOENT;

		// use the source basename as the new entry name
		const char *base = get_path_basename(src);
		if (strlen(base) >= EXT2_NAME_LEN) return ENAMETOOLONG;

		// copy name & parent inode num into provided buffers
		strncpy(name, base, EXT2_NAME_LEN);
		name[EXT2_NAME_LEN - 1] = '\0';
		*parent_ino = dir_ino;

		return 0;
    }
	
	// CASE 2: normal path, split into parent and name
	int retval = split_parent_name(dst, parent_path, name);
	if (retval != 0) return retval;
	// resolve the parent directory's inode
	int p_ino = path_lookup(parent_path);
	if (p_ino < 0) return ENOENT;
	struct ext2_inode *parent = get_inode(p_ino);

	// ensure parent is a directory
	if (!S_ISDIR(parent->i_mode)) return ENOENT;

	// success; fill provided buffer with parent inode number
	*parent_ino = p_ino;
	return 0;
}

/*
 * Checks if copy target exists and determines how to handle it.
 * Updates parent_ino/name if target is a directory (copy into it).
 *
 * src: source path (for basename extraction)
 * parent_ino: parent directory inode
 * name: target name
 * target_ino: existing target inode if overwriting
 * overwrite: 1 if overwriting existing file, 0 otherwise
 * 
 * Returns: 0 on success, errno on error
 */
int check_copy_target(const char *src, int *parent_ino, char *name, int *target_ino, int *overwrite) {
	
	struct ext2_inode *parent = get_inode(*parent_ino);
	int existing = find_dir_entry(parent, name);
	
	*target_ino = -1;
	*overwrite = 0;
	
	if (existing == -1) return 0;  // doesn't exist, will create

	struct ext2_inode *target = get_inode(existing);
	uint16_t type = target->i_mode & 0xF000;
	
	// CASE 1: symlink, cannot overwrite
	if (type == EXT2_S_IFLNK) return EEXIST;

	// CASE 2: directory, copy file into it with source basename
	if (S_ISDIR(target->i_mode)) {
		*parent_ino = existing;
		parent = get_inode(existing);

		const char *base = get_path_basename(src);
		if (strlen(base) >= EXT2_NAME_LEN) return ENAMETOOLONG;
		strncpy(name, base, EXT2_NAME_LEN);

		// check for existing file in target directory
		int inner = find_dir_entry(parent, name);
		if (inner >= 0) {
			struct ext2_inode *inner_node = get_inode(inner);
			uint16_t inner_type = inner_node->i_mode & 0xF000;
			if (inner_type == EXT2_S_IFLNK || S_ISDIR(inner_node->i_mode)) {
				return EEXIST;
			}

			*target_ino = inner;
			*overwrite = 1;
		}
		return 0;
	}

	// CASE 3: regular file, overwrite
	if (type == EXT2_S_IFREG) {
		*target_ino = existing;
		*overwrite = 1;
		
		return 0;
	}

	// none, doesnt exit
	return EEXIST;
}
