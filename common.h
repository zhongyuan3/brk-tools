#ifndef COMMON_H
#define COMMON_H

#include "brkfs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>

struct brkfs_ctx {
	int imgfd;
	uint32_t block_size;
	uint32_t inode_size;
	uint32_t block_count;
	uint32_t inode_count;
	uint32_t inode_bitmap_bno;
	uint32_t inode_bitmap_blocks;
	uint32_t inode_bitmap_bits;
	uint32_t inode_bitmap_hint;
	uint32_t data_bitmap_bno;
	uint32_t data_bitmap_blocks;
	uint32_t data_bitmap_bits;
	uint32_t data_bitmap_hint;
	uint32_t inode_table_bno;
	uint32_t inode_table_blocks;
	uint32_t first_data_bno;
	uint32_t data_block_count;
};

#define BRKFS_PATH_MAX 4096

#define BRKFS_PATH_CREATE_PARENTS (1u << 0)
#define BRKFS_PATH_HINT_RECURSIVE (1u << 1)

struct brkfs_path {
	uint32_t parent_ino;
	uint32_t ino;
	bool is_dir;
	char basename[BRKFS_NAME_LEN + 1];
};

/*
 * Exit paths:
 *   die_errno(what)  — libc call failed; errno is meaningful (perror).
 *   die_arg(...)     — bad CLI or image parameters; prints hint to use --help.
 *   die_prog(...)    — cannot format (layout, range, exhausted resources);
 *                      not a libc failure, no errno message.
 */

noreturn void die_errno(const char *what);
noreturn void die_arg(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
noreturn void die_prog(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

void *xmalloc(size_t size);
void set_prog_name(const char *prog_name);
const char *get_prog_name(void);

bool is_pow_of_two(uint32_t value);
uint32_t div_round_up_u32(uint32_t num, uint32_t div);
uint32_t parse_u32_arg(const char *str, const char *what);

void print_layout(struct brkfs_ctx *ctx);

void write_super(struct brkfs_ctx *ctx, struct brkfs_super_block *sb);
void read_super(int imgfd, struct brkfs_super_block *sb);

void super_to_ctx(const struct brkfs_super_block *sb, struct brkfs_ctx *ctx);
void ctx_to_super(const struct brkfs_ctx *ctx, struct brkfs_super_block *sb);
void open_ctx(int imgfd, struct brkfs_ctx *ctx);

void read_block(struct brkfs_ctx *ctx, uint32_t bno, void *buf);
void write_block(struct brkfs_ctx *ctx, uint32_t bno, void *buf);

void alloc_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		 uint32_t mode, uint32_t rdev, uint32_t flags);
void alloc_dir_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		     uint32_t mode, uint32_t flags);
void read_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode);
void write_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode);
void touch_inode_atime(struct brkfs_ctx *ctx, struct brkfs_inode *inode);

uint32_t alloc_data(struct brkfs_ctx *ctx);
uint32_t alloc_dir_data(struct brkfs_ctx *ctx, uint32_t dir_ino, bool first);

void add_dir_entry(struct brkfs_ctx *ctx, uint32_t dir_ino, const char *name,
		   uint8_t type, uint32_t ino);

uint32_t lookup_dir_entry(struct brkfs_ctx *ctx, uint32_t dir_ino,
			  const char *name);

uint32_t lookup_inode_bno(struct brkfs_ctx *ctx,
			  const struct brkfs_inode *inode, uint64_t file_blk);
void assign_inode_bno(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		      uint64_t file_blk, uint32_t data_bno);
uint64_t max_inode_file_bytes(struct brkfs_ctx *ctx);

bool is_inode_dir(struct brkfs_ctx *ctx, uint32_t ino);
uint32_t resolve_path(struct brkfs_ctx *ctx, const char *path, unsigned flags,
		      struct brkfs_path *out);

#endif
