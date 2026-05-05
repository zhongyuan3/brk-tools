#ifndef BRKFS_H
#define BRKFS_H

#include <stddef.h>
#include <stdint.h>

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

#define BRKFS_NAME_LEN 255

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
	uint8_t __s_padding[BRKFS_SUPER_SIZE - 40];
};

struct brkfs_inode {
	uint32_t i_ino;
	uint32_t i_mode;
	uint32_t i_rdev;
	uint32_t i_flags;
	uint32_t i_nlink;
	uint32_t i_size;
	uint32_t i_block[BRKFS_BLOCKS];
};

struct brkfs_dir_entry {
	uint32_t inode;
	uint16_t entry_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[];
};

#define BRKFS_MIN_INODE_SIZE 64
#define BRKFS_MAX_INODE_SIZE 256

#endif
