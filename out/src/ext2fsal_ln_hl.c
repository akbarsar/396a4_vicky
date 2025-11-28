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


int32_t ext2_fsal_ln_hl(const char *src,
                        const char *dst)
{
    /**
     * TODO: implement the ext2_ln_hl command here ...
     * src and dst are the ln command arguments described in the handout.
     */

    char parent[PATH_MAX], name[EXT2_NAME_LEN];

    // lookup source
    int src_ino = path_lookup(src);
    if (src_ino < 0) return -ENOENT;

    struct ext2_inode *src_inode = get_inode(src_ino);

    // no hardlink for dirs
    if (S_ISDIR(src_inode->i_mode))
        return -EISDIR;

    if (split_parent_name(dst, parent, name) < 0)
        return -ENOENT;

    int parent_ino = path_lookup(parent);
    if (parent_ino < 0) return -ENOENT;

    struct ext2_inode *p_inode = get_inode(parent_ino);
    if (!S_ISDIR(p_inode->i_mode))
        return -ENOENT;

    // check if name already exists
    int exists_ino = find_dir_entry(p_inode, name);
    if (exists_ino >= 0) {
        return -EEXIST;
    }

    // add directory entry pointing to same inode
    int r = add_dir_entry(parent_ino, name, src_ino, EXT2_FT_REG_FILE);
    if (r < 0) return r;

    src_inode->i_links_count++;

    return 0;
}
