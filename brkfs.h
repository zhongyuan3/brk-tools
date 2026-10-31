#ifndef BRKFS_H
#define BRKFS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * brkfs on-disk format.
 *
 * This is a manual copy of the layout definitions in the kernel header
 * src/fs/brkfs/brkfs.h.  Keep the two in sync.  The host tools cannot include
 * the kernel header directly because it also contains kernel-only code.
 */

#define BRKFS_DIRECT_BLOCKS 7 /* Total number of direct block pointers */
#define BRKFS_INDIRECT_BLOCK \
	BRKFS_DIRECT_BLOCKS /* Single indirect block pointer index */
#define BRKFS_DOUBLE_INDIRECT_BLOCK \
	(BRKFS_INDIRECT_BLOCK + 1) /* Double indirect block pointer index */
#define BRKFS_TRIPLE_INDIRECT_BLOCK    \
	(BRKFS_DOUBLE_INDIRECT_BLOCK + \
	 1) /* Triple indirect block pointer index */
#define BRKFS_BLOCKS \
	(BRKFS_TRIPLE_INDIRECT_BLOCK + 1) /* Total number of block pointers */

#define BRKFS_ROOT_INO 1
#define BRKFS_MAGIC 0x6b7262

#define BRKFS_SUPER_OFFSET 1024
#define BRKFS_SUPER_SIZE 1024
#define BRKFS_SUPER_END_OFFSET (BRKFS_SUPER_OFFSET + BRKFS_SUPER_SIZE)

#define BRKFS_DIR_ENTRY_MIN_LEN 12
#define BRKFS_DIR_CSUM_SIZE 4
#define BRKFS_NAME_LEN 255

#define BRKFS_FORMAT_VERSION 2

/* Superblock state values for format version 2 and later. */
#define BRKFS_STATE_CLEAN 1
#define BRKFS_STATE_DIRTY 2
#define BRKFS_STATE_ERROR 3

/*
 * Feature bits describe format capabilities that an image actually uses.
 * Version 0 images are legacy images with no feature bits set.
 */
#define BRKFS_FEATURE_DOUBLE_INDIRECT (1u << 0)
#define BRKFS_FEATURE_TRIPLE_INDIRECT (1u << 1)
#define BRKFS_FEATURE_DIR_INDIRECT (1u << 2)
#define BRKFS_FEATURE_ALL                                           \
	(BRKFS_FEATURE_DOUBLE_INDIRECT | BRKFS_FEATURE_TRIPLE_INDIRECT | \
	 BRKFS_FEATURE_DIR_INDIRECT)

#define BRKFS_MIN_BLOCK_SIZE 1024
#define BRKFS_MAX_BLOCK_SIZE 4096
#define BRKFS_MIN_INODE_SIZE 128
#define BRKFS_MAX_INODE_SIZE 256

struct brkfs_super_block {
	uint32_t s_blocksize; /* Block size */
	uint32_t s_inode_bitmap; /* Inode bitmap start block number */
	uint32_t s_inodes_count; /* Total number of inodes */
	uint32_t s_data_block_bitmap; /* Data block bitmap start block number */
	uint32_t s_data_blocks_count; /* Total number of data blocks */
	uint32_t s_inode_table; /* Inode table start block number */
	uint32_t s_first_data_block; /* First data block number */
	uint32_t s_magic; /* File system magic number */
	uint32_t s_inode_size; /* Size of an inode */
	uint32_t s_blocks_count; /* Total number of blocks */
	uint32_t s_version; /* On-disk format version */
	uint32_t s_features; /* Format feature bits used by this image */
	uint32_t s_state; /* BRKFS_STATE_* */
	uint8_t s_uuid[16]; /* Volume UUID */
	char s_label[64]; /* Volume label */
	uint32_t s_free_blocks; /* Free data blocks */
	uint32_t s_free_inodes; /* Free inode slots */
	uint32_t s_mtime; /* Last mount time */
	uint32_t s_lastcheck; /* Last fsck time */
	uint32_t s_mnt_count; /* Number of mounts since last check */
	uint32_t s_max_mnt_count; /* Mounts before a check is recommended */
	uint32_t s_backup_super; /* Backup superblock block number, 0 if none */
	uint32_t s_orphan; /* Head of the orphan inode list */
	uint32_t s_checksum; /* CRC32C over this superblock */
	uint8_t __s_padding[BRKFS_SUPER_SIZE - 168];
};

struct brkfs_inode {
	uint32_t i_ino;
	uint32_t i_mode;
	uint32_t i_rdev;
	uint32_t i_flags;
	uint32_t i_nlink;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_atime_nsec;
	uint32_t i_ctime;
	uint32_t i_ctime_nsec;
	uint32_t i_mtime;
	uint32_t i_mtime_nsec;
	uint32_t i_uid;
	uint32_t i_gid;
	uint32_t i_block[BRKFS_BLOCKS];
	uint32_t i_next_orphan;
	uint32_t i_checksum;
};

struct brkfs_dir_entry {
	uint32_t inode;
	uint16_t entry_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[BRKFS_NAME_LEN];
};

_Static_assert(sizeof(struct brkfs_super_block) == BRKFS_SUPER_SIZE,
	       "brkfs_super_block must match BRKFS_SUPER_SIZE");
_Static_assert(sizeof(struct brkfs_inode) <= BRKFS_MAX_INODE_SIZE,
	       "brkfs_inode is larger than BRKFS_MAX_INODE_SIZE");

/* CRC32C (Castagnoli), reflected polynomial 0x82f63b78. */
static inline uint32_t brkfs_crc32c(uint32_t crc, const void *buf,
				    uint32_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
	}
	return ~crc;
}

static inline uint32_t brkfs_super_checksum(struct brkfs_super_block *sb)
{
	uint32_t saved = sb->s_checksum;
	uint32_t crc;

	sb->s_checksum = 0;
	crc = brkfs_crc32c(0, sb, sizeof(*sb));
	sb->s_checksum = saved;
	return crc;
}

static inline void brkfs_super_set_checksum(struct brkfs_super_block *sb)
{
	sb->s_checksum = 0;
	sb->s_checksum = brkfs_crc32c(0, sb, sizeof(*sb));
}

static inline void brkfs_inode_set_checksum(struct brkfs_inode *inode)
{
	inode->i_checksum = 0;
	inode->i_checksum = brkfs_crc32c(0, inode, sizeof(*inode));
}

static inline bool brkfs_inode_checksum_ok(const struct brkfs_inode *inode)
{
	struct brkfs_inode copy = *inode;

	copy.i_checksum = 0;
	return inode->i_checksum ==
	       brkfs_crc32c(0, &copy, sizeof(copy));
}

static inline void brkfs_dir_set_checksum(void *buf, uint32_t block_size)
{
	uint32_t crc = brkfs_crc32c(0, buf, block_size - BRKFS_DIR_CSUM_SIZE);

	*(uint32_t *)((uint8_t *)buf + block_size - BRKFS_DIR_CSUM_SIZE) = crc;
}

static inline bool brkfs_dir_checksum_ok(const void *buf, uint32_t block_size)
{
	uint32_t stored =
		*(const uint32_t *)((const uint8_t *)buf + block_size -
				    BRKFS_DIR_CSUM_SIZE);

	return stored ==
	       brkfs_crc32c(0, buf, block_size - BRKFS_DIR_CSUM_SIZE);
}

#endif
