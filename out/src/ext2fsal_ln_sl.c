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


int32_t ext2_fsal_ln_sl(const char *src,
                        const char *dst)
{
    /**
     * TODO: implement the ext2_ln_sl command here ...
     * src and dst are the ln command arguments described in the handout.
     */
    char parent[PATH_MAX], name[EXT2_NAME_LEN];

    // ensure target exists
    int target_ino = path_lookup(src);
    if (target_ino < 0)
        return -ENOENT;

    // parse link_path
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
        if (S_ISLNK(eino->i_mode))
            return -EEXIST;
        // for regular file, overwrite
        return -EEXIST;
    }

    // new inod
    int new_ino = alloc_inode();
    if (new_ino < 0) return -ENOSPC;

    struct ext2_inode *inode = get_inode(new_ino);
    memset(inode, 0, sizeof(*inode));

    inode->i_mode = EXT2_S_IFLNK | 0777;
    inode->i_links_count = 1;
    inode->i_size = strlen(src);

    // add link name into parent directory
    int r = add_dir_entry(parent_ino, name, new_ino, EXT2_FT_SYMLINK);
    if (r < 0) return r;

    return 0;
}
