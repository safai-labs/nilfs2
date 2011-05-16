/*
 * ifile.h - NILFS inode file
 *
 * Copyright (C) 2006-2008 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Written by Amagai Yoshiji <amagai@osrg.net>
 * Revised by Ryusuke Konishi <ryusuke@osrg.net>
 *
 */

#ifndef _NILFS_IFILE_H
#define _NILFS_IFILE_H

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/nilfs2_fs.h>
#include "mdt.h"
#include "alloc.h"


/**
 * struct nilfs_ifile_change - array item to store ifile comparison result
 * @ino: inode number
 * @bh1: pointers to buffer heads having source of changed disk inode
 * @bh2: pointers to buffer heads having target of changed disk inode
 *
 * For new inodes, ptr array becomes @bh1 == NULL and @bh2 != NULL.
 * For deleted inodes, ptr array becomes @bh1 != NULL and @bh2 == NULL.
 * For modifed inodes, both @bh1 and @bh2 point to a valid item.
 */
struct nilfs_ifile_change {
	ino_t ino;
	struct buffer_head *bh1;
	struct buffer_head *bh2;
};

static inline struct nilfs_inode *
nilfs_ifile_map_inode(struct inode *ifile, ino_t ino, struct buffer_head *ibh)
{
	void *kaddr = kmap(ibh->b_page);
	return nilfs_palloc_block_get_entry(ifile, ino, ibh, kaddr);
}

static inline void nilfs_ifile_unmap_inode(struct inode *ifile, ino_t ino,
					   struct buffer_head *ibh)
{
	kunmap(ibh->b_page);
}

int nilfs_ifile_create_inode(struct inode *, ino_t *, struct buffer_head **);
int nilfs_ifile_delete_inode(struct inode *, ino_t);
int nilfs_ifile_get_inode_block(struct inode *, ino_t, struct buffer_head **);

ssize_t nilfs_ifile_compare(struct inode *ifile1, struct inode *ifile2,
			    ino_t start, struct nilfs_ifile_change *buf,
			    size_t maxchanges);

int nilfs_ifile_read(struct super_block *sb, struct nilfs_root *root,
		     size_t inode_size, struct nilfs_inode *raw_inode,
		     struct inode **inodep);

#endif	/* _NILFS_IFILE_H */
