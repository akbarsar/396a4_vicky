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
#include <time.h>


int32_t ext2_fsal_ln_sl(const char *src,
                        const char *dst)
{
    /**
     * TODO: implement the ext2_ln_sl command here ...
     * src and dst are the ln command arguments described in the handout.
     */
    if (!src || !dst) return -ENOENT;
    if (dst[0] != '/') return -ENOENT;  // destination must be absolute

    // NOTE: For symlinks, the source path does NOT need to be valid!
    // We just store whatever path is given.

    char parent[PATH_MAX], name[EXT2_NAME_LEN];

    // parse link_path (destination)
    if (split_parent_name(dst, parent, name) < 0)
        return -ENOENT;

    int parent_ino = path_lookup(parent);
    if (parent_ino < 0)
        return -ENOENT;

    struct ext2_inode *p_inode = get_inode(parent_ino);
    if (!S_ISDIR(p_inode->i_mode))
        return -ENOTDIR;

    // if link name exists
    int exists = find_dir_entry(p_inode, name);
    if (exists >= 0) {
        struct ext2_inode *eino = get_inode(exists);
        // if existing entry is a directory, return EISDIR per spec
        if (S_ISDIR(eino->i_mode))
            return -EISDIR;
        // otherwise return EEXIST (for file or symlink)
        return -EEXIST;
    }

    // allocate new inode for symlink
    int new_ino = alloc_inode();
    if (new_ino < 0) return -ENOSPC;

    // allocate a data block to store the path (no fast symlinks per spec)
    int new_block = alloc_block();
    if (new_block < 0) {
        free_inode(new_ino);
        return -ENOSPC;
    }

    // prepare inode
    struct ext2_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = EXT2_S_IFLNK | 0777;
    new_inode.i_links_count = 1;
    new_inode.i_size = strlen(src);
    new_inode.i_blocks = EXT2_BLOCK_SIZE / 512;
    new_inode.i_block[0] = new_block;
    new_inode.i_ctime = (unsigned int)time(NULL);
    new_inode.i_mtime = new_inode.i_ctime;
    new_inode.i_atime = new_inode.i_ctime;

    // write the source path into the data block
    char *blk = get_block(new_block);
    memset(blk, 0, EXT2_BLOCK_SIZE);
    memcpy(blk, src, strlen(src));

    // write inode
    write_inode(new_ino, &new_inode);

    // add link name into parent directory
    int r = add_dir_entry(parent_ino, name, new_ino, EXT2_FT_SYMLINK);
    if (r < 0) {
        free_block(new_block);
        free_inode(new_ino);
        return r;
    }

    return 0;
}
