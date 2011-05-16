/*
 * ifile.c - NILFS inode file
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
 * Written by Amagai Yoshiji <amagai@osrg.net>.
 * Revised by Ryusuke Konishi <ryusuke@osrg.net>.
 *
 */

#include <linux/types.h>
#include <linux/buffer_head.h>
#include "nilfs.h"
#include "mdt.h"
#include "alloc.h"
#include "ifile.h"


struct nilfs_ifile_info {
	struct nilfs_mdt_info mi;
	struct nilfs_palloc_cache palloc_cache;
};

static inline struct nilfs_ifile_info *NILFS_IFILE_I(struct inode *ifile)
{
	return (struct nilfs_ifile_info *)NILFS_MDT(ifile);
}

/**
 * nilfs_ifile_create_inode - create a new disk inode
 * @ifile: ifile inode
 * @out_ino: pointer to a variable to store inode number
 * @out_bh: buffer_head contains newly allocated disk inode
 *
 * Return Value: On success, 0 is returned and the newly allocated inode
 * number is stored in the place pointed by @ino, and buffer_head pointer
 * that contains newly allocated disk inode structure is stored in the
 * place pointed by @out_bh
 * On error, one of the following negative error codes is returned.
 *
 * %-EIO - I/O error.
 *
 * %-ENOMEM - Insufficient amount of memory available.
 *
 * %-ENOSPC - No inode left.
 */
int nilfs_ifile_create_inode(struct inode *ifile, ino_t *out_ino,
			     struct buffer_head **out_bh)
{
	struct nilfs_palloc_req req;
	int ret;

	req.pr_entry_nr = 0;  /* 0 says find free inode from beginning of
				 a group. dull code!! */
	req.pr_entry_bh = NULL;

	ret = nilfs_palloc_prepare_alloc_entry(ifile, &req);
	if (!ret) {
		ret = nilfs_palloc_get_entry_block(ifile, req.pr_entry_nr, 1,
						   &req.pr_entry_bh);
		if (ret < 0)
			nilfs_palloc_abort_alloc_entry(ifile, &req);
	}
	if (ret < 0) {
		brelse(req.pr_entry_bh);
		return ret;
	}
	nilfs_palloc_commit_alloc_entry(ifile, &req);
	mark_buffer_dirty(req.pr_entry_bh);
	nilfs_mdt_mark_dirty(ifile);
	*out_ino = (ino_t)req.pr_entry_nr;
	*out_bh = req.pr_entry_bh;
	return 0;
}

/**
 * nilfs_ifile_delete_inode - delete a disk inode
 * @ifile: ifile inode
 * @ino: inode number
 *
 * Return Value: On success, 0 is returned. On error, one of the following
 * negative error codes is returned.
 *
 * %-EIO - I/O error.
 *
 * %-ENOMEM - Insufficient amount of memory available.
 *
 * %-ENOENT - The inode number @ino have not been allocated.
 */
int nilfs_ifile_delete_inode(struct inode *ifile, ino_t ino)
{
	struct nilfs_palloc_req req = {
		.pr_entry_nr = ino, .pr_entry_bh = NULL
	};
	struct nilfs_inode *raw_inode;
	void *kaddr;
	int ret;

	ret = nilfs_palloc_prepare_free_entry(ifile, &req);
	if (!ret) {
		ret = nilfs_palloc_get_entry_block(ifile, req.pr_entry_nr, 0,
						   &req.pr_entry_bh);
		if (ret < 0)
			nilfs_palloc_abort_free_entry(ifile, &req);
	}
	if (ret < 0) {
		brelse(req.pr_entry_bh);
		return ret;
	}

	kaddr = kmap_atomic(req.pr_entry_bh->b_page, KM_USER0);
	raw_inode = nilfs_palloc_block_get_entry(ifile, req.pr_entry_nr,
						 req.pr_entry_bh, kaddr);
	raw_inode->i_flags = 0;
	kunmap_atomic(kaddr, KM_USER0);

	mark_buffer_dirty(req.pr_entry_bh);
	brelse(req.pr_entry_bh);

	nilfs_palloc_commit_free_entry(ifile, &req);

	return 0;
}

int nilfs_ifile_get_inode_block(struct inode *ifile, ino_t ino,
				struct buffer_head **out_bh)
{
	struct super_block *sb = ifile->i_sb;
	int err;

	if (unlikely(!NILFS_VALID_INODE(sb, ino))) {
		nilfs_error(sb, __func__, "bad inode number: %lu",
			    (unsigned long) ino);
		return -EINVAL;
	}

	err = nilfs_palloc_get_entry_block(ifile, ino, 0, out_bh);
	if (unlikely(err))
		nilfs_warning(sb, __func__, "unable to read inode: %lu",
			      (unsigned long) ino);
	return err;
}

static ssize_t
nilfs_ifile_compare_block(struct inode *ifile1, struct inode *ifile2,
			  ino_t start, const struct nilfs_bmap_diff *diff,
			  struct nilfs_ifile_change *changes,
			  size_t maxchanges)
{
	struct buffer_head *ibh1 = NULL, *ibh2 = NULL;
	struct nilfs_inode *raw_inode1, *raw_inode2;
	void *kaddr1, *kaddr2;
	size_t isz = NILFS_MDT(ifile1)->mi_entry_size;
	__u64 nr;
	ino_t ino, end;
	int t;
	int ret;
	ssize_t n = 0;

	t = nilfs_palloc_block_type(ifile1, diff->key, &nr);
	if (t != NILFS_PALLOC_ENTRY_BLOCK)
		goto out;

	ino = max_t(ino_t, nr, start);
	end = nr + NILFS_MDT(ifile1)->mi_entries_per_block - 1;
	if (ino > end)
		goto out;

	if (diff->ptr1 != NILFS_BMAP_INVALID_PTR) {
		ret = nilfs_palloc_get_entry_block(ifile1, nr, 0, &ibh1);
		if (ret < 0) {
			WARN_ON(ret == -ENOENT); /* ifile is broken */
			n = ret;
			goto out;
		}
	}
	if (diff->ptr2 != NILFS_BMAP_INVALID_PTR) {
		ret = nilfs_palloc_get_entry_block(ifile2, nr, 0, &ibh2);
		if (ret < 0) {
			WARN_ON(ret == -ENOENT); /* ifile is broken */
			n = ret;
			brelse(ibh1); /* brelse(NULL) is ignored */
			goto out;
		}
	}

	if (!ibh1) {
		if (!ibh2)
			goto out_bh;

		kaddr2 = kmap_atomic(ibh2->b_page, KM_USER0);
		raw_inode2 = nilfs_palloc_block_get_entry(ifile2, ino, ibh2,
							  kaddr2);
		for ( ; ino <= end; ino++) {
			if (le16_to_cpu(raw_inode2->i_links_count)) {
				changes[n].ino = ino;
				changes[n].bh1 = NULL;
				get_bh(ibh2);
				changes[n].bh2 = ibh2;
				n++;
				if (n == maxchanges)
					break;
			}
			raw_inode2 = (void *)raw_inode2 + isz;
		}
		kunmap_atomic(kaddr2, KM_USER0);

	} else if (!ibh2) {
		kaddr1 = kmap_atomic(ibh1->b_page, KM_USER0);
		raw_inode1 = nilfs_palloc_block_get_entry(ifile1, ino, ibh1,
							  kaddr1);
		for ( ; ino <= end; ino++) {
			if (le16_to_cpu(raw_inode1->i_links_count)) {
				changes[n].ino = ino;
				get_bh(ibh1);
				changes[n].bh1 = ibh1;
				changes[n].bh2 = NULL;
				n++;
				if (n == maxchanges)
					break;
			}
			raw_inode1 = (void *)raw_inode1 + isz;
		}
		kunmap_atomic(kaddr1, KM_USER0);
	} else {
		kaddr1 = kmap_atomic(ibh1->b_page, KM_USER0);
		raw_inode1 = nilfs_palloc_block_get_entry(ifile1, ino, ibh1,
							  kaddr1);
		kaddr2 = kmap_atomic(ibh2->b_page, KM_USER1);
		raw_inode2 = nilfs_palloc_block_get_entry(ifile2, ino, ibh2,
							  kaddr2);
		for (; ino <= end; ino++) {
			if (raw_inode1->i_ctime_nsec !=
			    raw_inode2->i_ctime_nsec ||
			    raw_inode1->i_ctime != raw_inode2->i_ctime) {
				changes[n].ino = ino;
				if (le16_to_cpu(raw_inode1->i_links_count)) {
					get_bh(ibh1);
					changes[n].bh1 = ibh1;
				} else {
					changes[n].bh1 = NULL;
				}
				if (le16_to_cpu(raw_inode2->i_links_count)) {
					get_bh(ibh2);
					changes[n].bh2 = ibh2;
				} else {
					changes[n].bh2 = NULL;
				}
				n++;
				if (n == maxchanges)
					break;
			}
			raw_inode1 = (void *)raw_inode1 + isz;
			raw_inode2 = (void *)raw_inode2 + isz;
		}
		kunmap_atomic(kaddr1, KM_USER0);
		kunmap_atomic(kaddr2, KM_USER1);
	}
out_bh:
	brelse(ibh1);
	brelse(ibh2);
out:
	return n;
}

/**
 * nilfs_ifile_compare - compare two ifiles and find modified inodes
 * @ifile1: source ifile inode
 * @ifile2: target ifile inode
 * @start: start inode number
 * @changes: buffer to store nilfs_ifile_change structures
 * @maxchanges: maximum number of changes that can be stored in @changes
 */
ssize_t nilfs_ifile_compare(struct inode *ifile1, struct inode *ifile2,
			    ino_t start, struct nilfs_ifile_change *changes,
			    size_t maxchanges)
{
	struct nilfs_bmap_diff *diffs;
	__u64 blkoff;
	size_t maxdiffs;
	ssize_t nc = 0, nd, n;
	int i;

	diffs = (struct nilfs_bmap_diff *)__get_free_pages(GFP_NOFS, 0);
	if (unlikely(!diffs))
		return -ENOMEM;

	maxdiffs = PAGE_SIZE / sizeof(*diffs);

	blkoff = nilfs_palloc_entry_blkoff(ifile1, start);

	do {
		nd = nilfs_bmap_compare(NILFS_I(ifile1)->i_bmap,
					NILFS_I(ifile2)->i_bmap,
					blkoff, diffs, maxdiffs);
		if (nd < 0) {
			nc = nd;
			break;
		}
		if (nd == 0)
			break;
		for (i = 0; i < nd; i++) {
			n = nilfs_ifile_compare_block(
				ifile1, ifile2, start, &diffs[i],
				&changes[nc], maxchanges - nc);
			if (n < 0) {
				nc = n;
				goto failed;
			}
			nc += n;
			if (nc == maxchanges)
				goto out;
		}
		blkoff = diffs[i - 1].key + 1;
	} while (nd == maxdiffs);
out:
	free_pages((unsigned long)diffs, 0);
	return nc;

failed:
	for (i = 0; i < nc; i++) {
		brelse(changes[i].bh1);
		brelse(changes[i].bh2);
	}
	goto out;
}

/**
 * nilfs_ifile_read - read or get ifile inode
 * @sb: super block instance
 * @root: root object
 * @inode_size: size of an inode
 * @raw_inode: on-disk ifile inode
 * @inodep: buffer to store the inode
 */
int nilfs_ifile_read(struct super_block *sb, struct nilfs_root *root,
		     size_t inode_size, struct nilfs_inode *raw_inode,
		     struct inode **inodep)
{
	struct inode *ifile;
	int err;

	ifile = nilfs_iget_locked(sb, root, NILFS_IFILE_INO);
	if (unlikely(!ifile))
		return -ENOMEM;
	if (!(ifile->i_state & I_NEW))
		goto out;

	err = nilfs_mdt_init(ifile, NILFS_MDT_GFP,
			     sizeof(struct nilfs_ifile_info));
	if (err)
		goto failed;

	err = nilfs_palloc_init_blockgroup(ifile, inode_size);
	if (err)
		goto failed;

	nilfs_palloc_setup_cache(ifile, &NILFS_IFILE_I(ifile)->palloc_cache);

	err = nilfs_read_inode_common(ifile, raw_inode);
	if (err)
		goto failed;

	unlock_new_inode(ifile);
 out:
	*inodep = ifile;
	return 0;
 failed:
	iget_failed(ifile);
	return err;
}
