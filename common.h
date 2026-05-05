#ifndef COMMON_H
#define COMMON_H

#include "brkfs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>

struct brkfs_volume {
	int imgfd;
	uint32_t bs;
	uint32_t is;
	uint32_t blocks;
	uint32_t inodes;
	uint32_t ibmap_bno;
	uint32_t ibmap_blocks;
	uint32_t ibmap_bits;
	uint32_t dbmap_bno;
	uint32_t dbmap_blocks;
	uint32_t dbmap_bits;
	uint32_t itable_bno;
	uint32_t itable_blocks;
	uint32_t dfirst_bno;
	uint32_t dblocks;
};

/*
 * Exit paths:
 *   die_errno(what)  — libc call failed; errno is meaningful (perror).
 *   die_argf(...)    — bad CLI or image parameters; prints hint to use --help.
 *   die_prog(...)   — cannot format (layout, range, exhausted resources);
 *                      not a libc failure, no errno message.
 */

noreturn void die_errno(const char *what);
noreturn void die_argf(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
noreturn void die_prog(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

void *xmalloc(size_t size);
void set_prog_name(const char *prog_name);
const char *get_prog_name(void);

bool is_pow_of_two_z(size_t value);
uint32_t div_round_up_u32(uint32_t num, uint32_t div);

void print_layout(struct brkfs_volume *vol);

void write_super(struct brkfs_volume *vol, struct brkfs_super_block *sb);
void read_super(int imgfd, struct brkfs_super_block *sb);

void read_block(struct brkfs_volume *vol, uint32_t bno, void *buf);
void write_block(struct brkfs_volume *vol, uint32_t bno, void *buf);

void alloc_inode(struct brkfs_volume *vol, struct brkfs_inode *inode,
		 uint32_t mode, uint32_t rdev, uint32_t flags);
void alloc_dir_inode(struct brkfs_volume *vol, struct brkfs_inode *inode,
		     uint32_t mode, uint32_t flags);
void read_inode(struct brkfs_volume *vol, struct brkfs_inode *inode);
void write_inode(struct brkfs_volume *vol, struct brkfs_inode *inode);

uint32_t alloc_data(struct brkfs_volume *vol);
uint32_t alloc_dir_data(struct brkfs_volume *vol, uint32_t dir_ino, bool first);

void dir_add_entry(struct brkfs_volume *vol, uint32_t dir_ino, const char *name,
		   uint8_t type, uint32_t ino);

uint32_t dir_lookup(struct brkfs_volume *vol, uint32_t dir_ino,
		    const char *name);

uint32_t inode_lookup_bno(struct brkfs_volume *vol,
			  const struct brkfs_inode *inode, uint64_t file_blk);
void inode_assign_bno(struct brkfs_volume *vol, struct brkfs_inode *inode,
		      uint64_t file_blk, uint32_t data_bno);
uint64_t inode_max_file_bytes(struct brkfs_volume *vol);

#endif
