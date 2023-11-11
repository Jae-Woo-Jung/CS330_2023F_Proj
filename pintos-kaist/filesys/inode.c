#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/fat.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk {
	disk_sector_t start;                /* First data sector. */
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	type_t type;                        /* File type (REG, DIR, SYMLINK) */
	char symlink_path[499];				/* (Naive size) */
};

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
static inline size_t
bytes_to_sectors (off_t size) {
	return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
	struct list_elem elem;              /* Element in inode list. */
	disk_sector_t sector;               /* Sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	struct lock i_lock;                 /* Inode lock */
	struct inode_disk data;             /* Inode content. */
};

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */

#ifdef EFILESYS
byte_to_sector (const struct inode *inode, off_t pos) {
	off_t clst_num;
	cluster_t clst_cur;
	disk_sector_t ret = -1;

	ASSERT (inode != NULL);
	if (pos < inode->data.length) {
		clst_num = pos / (DISK_SECTOR_SIZE * SECTORS_PER_CLUSTER);
		clst_cur = sector_to_cluster(inode->data.start);
		while (clst_num > 0) {
			clst_cur = fat_get(clst_cur);
			clst_num--;
		}
		ret = cluster_to_sector(clst_cur);
	}
	return ret;
}

#else
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) {
	ASSERT (inode != NULL);
	if (pos < inode->data.length)
		return inode->data.start + pos / DISK_SECTOR_SIZE;
	else
		return -1;
}
#endif

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
static struct list open_inodes;
static struct lock open_inodes_lock;

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
	lock_init (&open_inodes_lock);
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */

bool
inode_create (disk_sector_t sector, off_t length, type_t type) {
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT (length >= 0);

	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		size_t sectors = bytes_to_sectors (length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		disk_inode->type = type;
		disk_inode->start = 0;

#ifdef EFILESYS
		if (sectors == 0) /* No more data sectors */
			goto done;

		cluster_t cur, first_clst;
		size_t len_clst = 0;

		first_clst = fat_create_chain(0);
		if (first_clst == 0)
			goto free;

		len_clst = sectors / SECTORS_PER_CLUSTER;
		cur = first_clst;
		for (; len_clst > 1; len_clst--) {
			cur = fat_create_chain(cur);
			if (cur == 0) {
				fat_remove_chain(first_clst, 0);
				goto free;
			}
		}
		disk_inode->start = cluster_to_sector(first_clst);
done:
		disk_write(filesys_disk, sector, disk_inode);
		success = true;
free:
		free(disk_inode);
		return success;
#else
		if (free_map_allocate (sectors, &disk_inode->start)) {
			disk_write (filesys_disk, sector, disk_inode);
			if (sectors > 0) {
				static char zeros[DISK_SECTOR_SIZE];
				size_t i;

				for (i = 0; i < sectors; i++) 
					disk_write (filesys_disk, disk_inode->start + i, zeros); 
			}
			success = true; 
		} 
		free (disk_inode);
		return success;
		#endif
	}
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) {
	struct list_elem *e;
	struct inode *inode;

	/* Check whether this inode is already open. */
	lock_acquire(&open_inodes_lock);
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e)) {
		inode = list_entry (e, struct inode, elem);
		if (inode->sector == sector) {
			lock_release(&open_inodes_lock);
			inode_reopen(inode);
			return inode; 
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL) {
		lock_release(&open_inodes_lock);
		return NULL;
	}

	/* Initialize. */
	lock_init(&inode->i_lock);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	disk_read(filesys_disk, inode->sector, &inode->data);
	list_push_front (&open_inodes, &inode->elem);
	lock_release(&open_inodes_lock);
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode) {
	if (inode != NULL) {
		lock_acquire(&inode->i_lock);
		inode->open_cnt++;
		lock_release(&inode->i_lock);
	}
	return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode) {
	return inode->sector;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	#ifdef EFILESYS
	disk_write (filesys_disk, inode->sector, &inode->data);
	#endif
	/* Release resources if this was the last opener. */
	lock_acquire(&inode->i_lock);
	--inode->open_cnt;
	if (inode->open_cnt == 0) {
		lock_release(&inode->i_lock);
		/* Remove from inode list and release lock. */
		lock_acquire(&open_inodes_lock);
		list_remove(&inode->elem);
		lock_release(&open_inodes_lock);

		/* Deallocate blocks if removed. */
		if (inode->removed) {

			#ifdef EFILESYS
			fat_remove_chain(sector_to_cluster(inode->sector), 0);
			if (inode->data.start != 0)
				fat_remove_chain(sector_to_cluster(inode->data.start), 0);
			#else
			free_map_release (inode->sector, 1);
			free_map_release (inode->data.start,
					bytes_to_sectors (inode->data.length));
			#endif
		}
		free(inode);
	}else{
		lock_release(&inode->i_lock);
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void
inode_remove (struct inode *inode) {
	ASSERT (inode != NULL);
	lock_acquire(&inode->i_lock);
	inode->removed = true;
	lock_release(&inode->i_lock);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Read full sector directly into caller's buffer. */
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free (bounce);

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset) {
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;

	if (inode->deny_write_cnt)
		return 0;

	if (inode->data.length < size + offset) {
		cluster_t last_clst, cur;
		uint32_t clst_cnt = 0;

		if (inode->data.length == 0) {
			ASSERT(inode->data.start == 0);
			cur = fat_create_chain(0);
			if (cur == 0)
				return 0;
			inode->data.start = cluster_to_sector(cur);
		} else {
			cur = sector_to_cluster(inode->data.start);
			while (fat_get(cur) != EOChain)
				cur = fat_get(cur);
		}

		last_clst = cur;
		clst_cnt = DIV_ROUND_UP(size + offset, DISK_SECTOR_SIZE * SECTORS_PER_CLUSTER)
					- DIV_ROUND_UP(inode->data.length, DISK_SECTOR_SIZE * SECTORS_PER_CLUSTER);

		for (; clst_cnt > 0; clst_cnt--) {
			cur = fat_create_chain(cur);
			if (cur == 0) {
				if (fat_get(last_clst) != EOChain)
					fat_remove_chain(fat_get(last_clst), last_clst);
				return 0;
			}
		}
		inode->data.length = size + offset;
	}

	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Write full sector directly to disk. */
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else {
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			if (sector_ofs > 0 || chunk_size < sector_left) 
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free (bounce);

	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
	void
inode_deny_write (struct inode *inode) 
{
	lock_acquire(&inode->i_lock);
	inode->deny_write_cnt++;
	lock_release(&inode->i_lock);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) {
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	lock_acquire(&inode->i_lock);
	inode->deny_write_cnt--;
	lock_release(&inode->i_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode) {
	return inode->data.length;
}

/* Returns the open count of INODE. */
int
inode_open_cnt (const struct inode *inode) {
	return inode->open_cnt;
}

/* Returns whether file type is regular file or not */
bool
inode_is_reg (const struct inode *inode) {
	return inode->data.type == F_REG;
}

/* Returns whether file type is directory or not */
bool
inode_is_dir (const struct inode *inode) {
	return inode->data.type == F_DIR;
}

/* Returns whether file type is softlink or not */
bool
inode_is_symlink (const struct inode *inode) {
	return inode->data.type == F_SYML;
}

/* Set this as a symlink file, return true on success. */
bool 
inode_set_symlink (disk_sector_t inode_sector, const char *target) {
	struct inode *inode = inode_open(inode_sector);

	if (inode == NULL)
		return false;

	inode->data.type = F_SYML;
	memcpy(inode->data.symlink_path, target, strlen(target) + 1);
	inode_close(inode);
	return true;
}

/* Returns symbolic link path. */
char *
inode_symlink_path (const struct inode* inode){
	return inode->data.symlink_path;
}
