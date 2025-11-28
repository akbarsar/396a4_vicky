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

#ifndef CSC369_E2FS_H
#define CSC369_E2FS_H

#include "ext2.h"
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * TODO: add in here prototypes for any helpers you might need.
 * Implement the helpers in e2fs.c
 */
#define DIRECT_POINTERS 12
#define INDIRECT_INDEX  12
#define TOTAL_POINTERS  15
#define PATH_MAX 4096
int test_bit(uint8_t *bitmap, int n);
void set_bit(uint8_t *bitmap, int n);
void clear_bit(uint8_t *bitmap, int n);


// lock init and cleanup
void locks_init(int num_inodes, int num_blocks);
void locks_destroy(int num_inodes, int num_blocks);

int alloc_inode();
int alloc_block();
struct ext2_inode* get_inode(int ino);
void write_inode(int ino, struct ext2_inode* inode);
char* get_block(int block_num);
void write_block(int block_num, char* data);
struct ext2_dir_entry *next_dir_entry(struct ext2_dir_entry *entry);
int add_dir_entry(int parent_ino, const char* name, int child_ino, uint8_t type);
int find_dir_entry(struct ext2_inode *dir, const char *name);
int path_lookup(const char *path);

int dir_entry_rec_len(int name_len);
void free_inode(int ino);
void free_block(int block_num);
void strip_trailing_slashes(char *s);
int split_parent_name(const char *path, char *parent_buf, char *name_buf);

void free_inode_blocks_locked(int ino);
int write_data_into_inode(int host_fd, struct ext2_inode *inode, off_t filesize);
#endif
