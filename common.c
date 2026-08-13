#include "common.h"
#include "brkfs.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *prog_name = "unknown";

static void now_time(uint32_t *sec, uint32_t *nsec)
{
	struct timespec ts = { 0 };

	if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		*sec = (uint32_t)ts.tv_sec;
		*nsec = (uint32_t)ts.tv_nsec;
		return;
	}

	time_t now = time(NULL);
	*sec = now < 0 ? 0 : (uint32_t)now;
	*nsec = 0;
}

void *xmalloc(size_t size)
{
	void *ptr = malloc(size);
	if (ptr == NULL)
		die_errno("malloc");
	return ptr;
}

noreturn void die_errno(const char *what)
{
	fprintf(stderr, "%s: ", prog_name);
	perror(what);
	exit(EXIT_FAILURE);
}

noreturn void die_arg(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", prog_name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n*** Try '%s --help' for more information. ***\n",
		prog_name);
	exit(EXIT_FAILURE);
}

noreturn void die_prog(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: error: ", prog_name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

void set_prog_name(const char *name)
{
	prog_name = name;
}

const char *get_prog_name(void)
{
	return prog_name;
}

bool is_pow_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

uint32_t div_round_up_u32(uint32_t num, uint32_t div)
{
	return (num + div - 1) / div;
}

uint32_t parse_u32_arg(const char *str, const char *what)
{
	char *end;
	unsigned long long v = strtoull(str, &end, 10);

	if (end == str || *end != '\0' || v > UINT32_MAX)
		die_arg("invalid %s: '%s'", what, str);
	return (uint32_t)v;
}

static void read_full(int fd, void *buf, size_t len, off_t off)
{
	ssize_t r = pread(fd, buf, len, off);

	if (r < 0)
		die_errno("read");
	if (r != (ssize_t)len)
		die_prog("short read (%zd of %zu bytes)", r, len);
}

static void write_full(int fd, const void *buf, size_t len, off_t off)
{
	ssize_t r = pwrite(fd, buf, len, off);

	if (r < 0)
		die_errno("write");
	if (r != (ssize_t)len)
		die_prog("short write (%zd of %zu bytes)", r, len);
}

void write_super(struct brkfs_ctx *ctx, struct brkfs_super_block *sb)
{
	write_full(ctx->imgfd, sb, sizeof(*sb), BRKFS_SUPER_OFFSET);
}

void read_super(int imgfd, struct brkfs_super_block *sb)
{
	read_full(imgfd, sb, sizeof(*sb), BRKFS_SUPER_OFFSET);
}

void super_to_ctx(const struct brkfs_super_block *sb, struct brkfs_ctx *ctx)
{
	ctx->block_size = sb->s_blocksize;
	ctx->inode_size = sb->s_inode_size;
	ctx->block_count = sb->s_blocks_count;
	ctx->inode_count = sb->s_inodes_count;
	ctx->inode_bitmap_bno = sb->s_inode_bitmap;
	ctx->inode_bitmap_bits = sb->s_inodes_count;
	ctx->data_bitmap_bno = sb->s_data_block_bitmap;
	ctx->data_bitmap_bits = sb->s_data_blocks_count;
	ctx->inode_table_bno = sb->s_inode_table;
	ctx->first_data_bno = sb->s_first_data_block;
	ctx->data_block_count = sb->s_data_blocks_count;

	uint32_t bits_per_block = ctx->block_size * 8;
	uint32_t inodes_per_block = ctx->block_size / ctx->inode_size;

	ctx->inode_bitmap_blocks =
		div_round_up_u32(ctx->inode_bitmap_bits, bits_per_block);
	ctx->data_bitmap_blocks =
		div_round_up_u32(ctx->data_bitmap_bits, bits_per_block);
	ctx->inode_table_blocks =
		div_round_up_u32(ctx->inode_count, inodes_per_block);
}

void ctx_to_super(const struct brkfs_ctx *ctx, struct brkfs_super_block *sb)
{
	sb->s_blocksize = ctx->block_size;
	sb->s_inode_bitmap = ctx->inode_bitmap_bno;
	sb->s_inodes_count = ctx->inode_count;
	sb->s_data_block_bitmap = ctx->data_bitmap_bno;
	sb->s_data_blocks_count = ctx->data_block_count;
	sb->s_inode_table = ctx->inode_table_bno;
	sb->s_first_data_block = ctx->first_data_bno;
	sb->s_magic = BRKFS_MAGIC;
	sb->s_inode_size = ctx->inode_size;
	sb->s_blocks_count = ctx->block_count;
}

void open_ctx(int imgfd, struct brkfs_ctx *ctx)
{
	struct brkfs_super_block sb;

	memset(ctx, 0, sizeof(*ctx));
	read_super(imgfd, &sb);
	if (sb.s_magic != BRKFS_MAGIC)
		die_prog("invalid super block magic number");
	ctx->imgfd = imgfd;
	super_to_ctx(&sb, ctx);
}

void read_block(struct brkfs_ctx *ctx, uint32_t bno, void *buf)
{
	if (bno >= ctx->block_count)
		die_prog("block number out of range: %u (max %u)", bno,
			 ctx->block_count);
	read_full(ctx->imgfd, buf, ctx->block_size,
		  (off_t)bno * ctx->block_size);
}

void write_block(struct brkfs_ctx *ctx, uint32_t bno, void *buf)
{
	if (bno >= ctx->block_count)
		die_prog("block number out of range: %u (max %u)", bno,
			 ctx->block_count);
	write_full(ctx->imgfd, buf, ctx->block_size,
		   (off_t)bno * ctx->block_size);
}

static bool bitmap_alloc_in_block(uint8_t *map, uint32_t nbits, uint32_t start,
				  uint32_t *bit)
{
	uint32_t nbytes = div_round_up_u32(nbits, 8);

	if (start & 7) {
		uint32_t i = start & 7;
		uint32_t n = nbits - start;
		uint32_t b = start;

		if (n > 8 - i)
			n = 8 - i;
		for (uint32_t k = 0; k < n; k++) {
			if (map[start >> 3] & (1u << (i + k)))
				continue;
			map[start >> 3] |= (1u << (i + k));
			*bit = b + k;
			return true;
		}
		start = (start + 8) & ~7u;
	}

	for (uint32_t byte = start >> 3; byte < nbytes; byte++) {
		uint32_t b = byte * 8;
		uint32_t n = nbits - b;

		if (n > 8)
			n = 8;
		for (uint32_t i = 0; i < n; i++) {
			if (map[byte] & (1u << i))
				continue;
			map[byte] |= (1u << i);
			*bit = b + i;
			return true;
		}
	}
	return false;
}

static bool bitmap_alloc(struct brkfs_ctx *ctx, uint32_t bmap_bno,
			 uint32_t nbits, uint32_t *hint, uint32_t *bit)
{
	uint8_t *block = xmalloc(ctx->block_size);
	uint32_t bits_per_block = ctx->block_size * 8;
	uint32_t start = *hint < nbits ? *hint : 0;
	bool found = false;

	/*
	 * Resume scanning at the previous allocation; wrap around to cover
	 * the region before the hint.
	 */
	for (uint32_t pass = 0; pass < 2 && !found; pass++) {
		uint32_t begin = pass ? 0 : start;
		uint32_t end = pass ? start : nbits;

		while (begin < end) {
			uint32_t block_start =
				(begin / bits_per_block) * bits_per_block;
			uint32_t block_end = block_start + bits_per_block;
			uint32_t bno =
				bmap_bno + (block_start / bits_per_block);
			uint32_t n = end < block_end ? end - block_start :
						       bits_per_block;

			read_block(ctx, bno, block);
			if (bitmap_alloc_in_block(block, n, begin - block_start,
						  bit)) {
				*bit += block_start;
				*hint = (*bit + 1) % nbits;
				write_block(ctx, bno, block);
				found = true;
				break;
			}
			begin = block_start + bits_per_block;
		}
	}

	free(block);
	return found;
}

static void alloc_inode_slot(struct brkfs_ctx *ctx, struct brkfs_inode *inode)
{
	uint32_t bit = 0;

	if (!bitmap_alloc(ctx, ctx->inode_bitmap_bno, ctx->inode_bitmap_bits,
			  &ctx->inode_bitmap_hint, &bit))
		die_prog("no free inodes");
	inode->i_ino = bit + 1;
	memset(inode->i_block, 0, sizeof(inode->i_block));
	write_inode(ctx, inode);
}

void alloc_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		 uint32_t mode, uint32_t rdev, uint32_t flags)
{
	uint32_t sec;
	uint32_t nsec;

	now_time(&sec, &nsec);

	inode->i_ino = 0;
	inode->i_mode = mode;
	inode->i_rdev = rdev;
	inode->i_flags = flags;
	inode->i_nlink = 1;
	inode->i_size = 0;
	inode->i_atime = sec;
	inode->i_atime_nsec = nsec;
	inode->i_ctime = sec;
	inode->i_ctime_nsec = nsec;
	inode->i_mtime = sec;
	inode->i_mtime_nsec = nsec;
	inode->i_uid = (uint32_t)getuid();
	inode->i_gid = (uint32_t)getgid();
	memset(inode->i_block, 0, sizeof(inode->i_block));
	alloc_inode_slot(ctx, inode);
}

uint32_t alloc_data(struct brkfs_ctx *ctx)
{
	uint32_t bit = 0;

	if (bitmap_alloc(ctx, ctx->data_bitmap_bno, ctx->data_bitmap_bits,
			 &ctx->data_bitmap_hint, &bit))
		return ctx->first_data_bno + bit;

	die_prog("no free data blocks");
}

static uint32_t pointers_per_block(struct brkfs_ctx *ctx)
{
	return ctx->block_size / sizeof(uint32_t);
}

static uint64_t max_inode_file_blocks(struct brkfs_ctx *ctx)
{
	uint64_t n = pointers_per_block(ctx);
	uint64_t d = BRKFS_DIRECT_BLOCKS;

	return d + n + n * n + n * n * n;
}

uint64_t max_inode_file_bytes(struct brkfs_ctx *ctx)
{
	uint64_t blocks = max_inode_file_blocks(ctx);
	uint64_t bytes = blocks * (uint64_t)ctx->block_size;

	if (bytes > UINT32_MAX)
		return UINT32_MAX;
	return bytes;
}

uint32_t lookup_inode_bno(struct brkfs_ctx *ctx,
			  const struct brkfs_inode *inode, uint64_t file_blk)
{
	uint32_t n = pointers_per_block(ctx);
	uint64_t max_blocks = max_inode_file_blocks(ctx);

	if (file_blk >= max_blocks)
		die_prog("file block index out of range: %" PRIu64, file_blk);

	if (file_blk < BRKFS_DIRECT_BLOCKS)
		return inode->i_block[file_blk];

	uint64_t u = file_blk - BRKFS_DIRECT_BLOCKS;

	if (u < (uint64_t)n) {
		uint32_t ib = inode->i_block[BRKFS_INDIRECT_BLOCK];

		if (ib == 0)
			return 0;
		uint8_t *buf = xmalloc(ctx->block_size);

		read_block(ctx, ib, buf);
		uint32_t r = ((uint32_t *)buf)[u];
		free(buf);
		return r;
	}
	u -= n;

	if (u < (uint64_t)n * n) {
		uint32_t dib = inode->i_block[BRKFS_DOUBLE_INDIRECT_BLOCK];

		if (dib == 0)
			return 0;
		uint32_t r1 = (uint32_t)(u / n);
		uint32_t r2 = (uint32_t)(u % n);
		uint8_t *b0 = xmalloc(ctx->block_size);

		read_block(ctx, dib, b0);
		uint32_t l1 = ((uint32_t *)b0)[r1];

		free(b0);
		if (l1 == 0)
			return 0;
		uint8_t *b1 = xmalloc(ctx->block_size);

		read_block(ctx, l1, b1);
		uint32_t r = ((uint32_t *)b1)[r2];
		free(b1);
		return r;
	}
	u -= (uint64_t)n * n;

	if (u < (uint64_t)n * n * n) {
		uint32_t tib = inode->i_block[BRKFS_TRIPLE_INDIRECT_BLOCK];

		if (tib == 0)
			return 0;
		uint32_t r1 = (uint32_t)(u / ((uint64_t)n * n));
		uint32_t r2 = (uint32_t)((u / n) % n);
		uint32_t r3 = (uint32_t)(u % n);
		uint8_t *b0 = xmalloc(ctx->block_size);

		read_block(ctx, tib, b0);
		uint32_t l1 = ((uint32_t *)b0)[r1];

		free(b0);
		if (l1 == 0)
			return 0;
		uint8_t *b1 = xmalloc(ctx->block_size);

		read_block(ctx, l1, b1);
		uint32_t l2 = ((uint32_t *)b1)[r2];

		free(b1);
		if (l2 == 0)
			return 0;
		uint8_t *b2 = xmalloc(ctx->block_size);

		read_block(ctx, l2, b2);
		uint32_t r = ((uint32_t *)b2)[r3];
		free(b2);
		return r;
	}

	die_prog("internal: lookup_inode_bno past triple");
	return 0;
}

static void ensure_indirect_root(struct brkfs_ctx *ctx,
				 struct brkfs_inode *inode, int slot)
{
	if (inode->i_block[slot] != 0)
		return;

	uint32_t b = alloc_data(ctx);
	uint8_t *z = xmalloc(ctx->block_size);

	memset(z, 0, ctx->block_size);
	write_block(ctx, b, z);
	free(z);
	inode->i_block[slot] = b;
}

void assign_inode_bno(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		      uint64_t file_blk, uint32_t data_bno)
{
	uint32_t n = pointers_per_block(ctx);
	uint64_t max_blocks = max_inode_file_blocks(ctx);

	if (file_blk >= max_blocks)
		die_prog("file block index out of range: %" PRIu64, file_blk);

	if (file_blk < BRKFS_DIRECT_BLOCKS) {
		inode->i_block[file_blk] = data_bno;
		return;
	}

	uint64_t u = file_blk - BRKFS_DIRECT_BLOCKS;

	if (u < (uint64_t)n) {
		ensure_indirect_root(ctx, inode, BRKFS_INDIRECT_BLOCK);
		uint32_t ib = inode->i_block[BRKFS_INDIRECT_BLOCK];
		uint8_t *buf = xmalloc(ctx->block_size);

		read_block(ctx, ib, buf);
		((uint32_t *)buf)[u] = data_bno;
		write_block(ctx, ib, buf);
		free(buf);
		return;
	}
	u -= n;

	if (u < (uint64_t)n * n) {
		ensure_indirect_root(ctx, inode, BRKFS_DOUBLE_INDIRECT_BLOCK);
		uint32_t dib = inode->i_block[BRKFS_DOUBLE_INDIRECT_BLOCK];
		uint32_t r1 = (uint32_t)(u / n);
		uint32_t r2 = (uint32_t)(u % n);
		uint8_t *b0 = xmalloc(ctx->block_size);
		uint8_t *b1 = xmalloc(ctx->block_size);

		read_block(ctx, dib, b0);
		uint32_t *p0 = (uint32_t *)b0;
		uint32_t l1 = p0[r1];

		if (l1 == 0) {
			l1 = alloc_data(ctx);
			memset(b1, 0, ctx->block_size);
			write_block(ctx, l1, b1);
			p0[r1] = l1;
			write_block(ctx, dib, b0);
		}
		read_block(ctx, l1, b1);
		((uint32_t *)b1)[r2] = data_bno;
		write_block(ctx, l1, b1);
		free(b0);
		free(b1);
		return;
	}
	u -= (uint64_t)n * n;

	if (u < (uint64_t)n * n * n) {
		ensure_indirect_root(ctx, inode, BRKFS_TRIPLE_INDIRECT_BLOCK);
		uint32_t tib = inode->i_block[BRKFS_TRIPLE_INDIRECT_BLOCK];
		uint32_t r1 = (uint32_t)(u / ((uint64_t)n * n));
		uint32_t r2 = (uint32_t)((u / n) % n);
		uint32_t r3 = (uint32_t)(u % n);
		uint8_t *b0 = xmalloc(ctx->block_size);
		uint8_t *b1 = xmalloc(ctx->block_size);
		uint8_t *b2 = xmalloc(ctx->block_size);

		read_block(ctx, tib, b0);
		uint32_t *p0 = (uint32_t *)b0;
		uint32_t l1 = p0[r1];

		if (l1 == 0) {
			l1 = alloc_data(ctx);
			memset(b1, 0, ctx->block_size);
			write_block(ctx, l1, b1);
			p0[r1] = l1;
			write_block(ctx, tib, b0);
		}
		read_block(ctx, l1, b1);
		uint32_t *p1 = (uint32_t *)b1;
		uint32_t l2 = p1[r2];

		if (l2 == 0) {
			l2 = alloc_data(ctx);
			memset(b2, 0, ctx->block_size);
			write_block(ctx, l2, b2);
			p1[r2] = l2;
			write_block(ctx, l1, b1);
		}
		read_block(ctx, l2, b2);
		((uint32_t *)b2)[r3] = data_bno;
		write_block(ctx, l2, b2);
		free(b0);
		free(b1);
		free(b2);
		return;
	}

	die_prog("internal: assign_inode_bno past triple");
}

uint32_t lookup_dir_entry(struct brkfs_ctx *ctx, uint32_t dir_ino,
			  const char *name)
{
	struct brkfs_inode inode = { .i_ino = dir_ino };
	size_t name_len = strlen(name);
	uint32_t found = 0;

	read_inode(ctx, &inode);

	if (inode.i_size == 0) {
		touch_inode_atime(ctx, &inode);
		return 0;
	}

	uint64_t nblocks = (uint64_t)inode.i_size / ctx->block_size;
	uint8_t *buf = xmalloc(ctx->block_size);

	for (uint64_t bi = 0; bi < nblocks; bi++) {
		uint32_t bno = lookup_inode_bno(ctx, &inode, bi);

		if (bno == 0)
			continue;
		read_block(ctx, bno, buf);
		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)buf;
		struct brkfs_dir_entry *end =
			(struct brkfs_dir_entry *)(buf + ctx->block_size);
		uint32_t el = ctx->block_size;

		for (; (uint8_t *)e < (uint8_t *)end;
		     e = (struct brkfs_dir_entry *)((uint8_t *)e + el)) {
			el = e->entry_len;
			if (el == 0)
				break;
			if (el < BRKFS_DIR_ENTRY_MIN_LEN)
				continue;
			if (e->inode == 0)
				continue;
			if (e->name_len == name_len &&
			    memcmp(e->name, name, name_len) == 0) {
				found = e->inode;
				goto done;
			}
		}
	}
done:
	free(buf);
	touch_inode_atime(ctx, &inode);
	return found;
}

void read_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode)
{
	uint32_t ino = inode->i_ino;
	if (ino < 1 || ino > ctx->inode_count)
		die_prog("inode number out of range: %u, [1, %u)", ino,
			 ctx->inode_count);
	uint32_t inodes_per_block = ctx->block_size / ctx->inode_size;
	uint32_t bno = (ino - 1) / inodes_per_block + ctx->inode_table_bno;
	uint8_t *block = xmalloc(ctx->block_size);
	read_block(ctx, bno, block);
	uint32_t i = (ino - 1) % inodes_per_block;
	memcpy(inode, block + i * ctx->inode_size, sizeof(*inode));
	free(block);
}

void write_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode)
{
	uint32_t ino = inode->i_ino;
	if (ino < 1 || ino > ctx->inode_count)
		die_prog("inode number out of range: %u, [1, %u)", ino,
			 ctx->inode_count);
	uint32_t inodes_per_block = ctx->block_size / ctx->inode_size;
	uint32_t bno = (ino - 1) / inodes_per_block + ctx->inode_table_bno;
	uint8_t *block = xmalloc(ctx->block_size);
	read_block(ctx, bno, block);
	uint32_t i = (ino - 1) % inodes_per_block;
	uint8_t *slot = block + i * ctx->inode_size;
	memset(slot, 0, ctx->inode_size);
	memcpy(slot, inode, sizeof(*inode));
	write_block(ctx, bno, block);
	free(block);
}

void touch_inode_atime(struct brkfs_ctx *ctx, struct brkfs_inode *inode)
{
	uint32_t sec;
	uint32_t nsec;

	now_time(&sec, &nsec);
	if (inode->i_atime == sec && inode->i_atime_nsec == nsec)
		return;
	inode->i_atime = sec;
	inode->i_atime_nsec = nsec;
	write_inode(ctx, inode);
}

uint32_t alloc_dir_data(struct brkfs_ctx *ctx, uint32_t dir_ino, bool first)
{
	uint32_t bno = alloc_data(ctx);
	void *buf = xmalloc(ctx->block_size);

	memset(buf, 0, ctx->block_size);
	struct brkfs_dir_entry *e = buf;
	if (first) {
		e->inode = dir_ino;
		e->entry_len = 12;
		e->name_len = 1;
		e->file_type = DT_DIR;
		e->name[0] = '.';
		e = (struct brkfs_dir_entry *)((uint8_t *)e + 12);
		e->inode = dir_ino;
		e->entry_len = ctx->block_size - 12;
		e->name_len = 2;
		e->file_type = DT_DIR;
		e->name[0] = '.';
		e->name[1] = '.';
	} else {
		e->inode = 0;
		e->entry_len = ctx->block_size;
		e->name_len = 0;
		e->file_type = DT_UNKNOWN;
		e->name[0] = '\0';
	}

	write_block(ctx, bno, buf);
	free(buf);
	return bno;
}

void alloc_dir_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		     uint32_t mode, uint32_t flags)
{
	mode |= S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
	alloc_inode(ctx, inode, mode, 0, flags);
	uint32_t dir_ino = inode->i_ino;
	uint32_t dir_bno = alloc_dir_data(ctx, dir_ino, true);
	inode->i_nlink = 2;
	inode->i_size =
		ctx->block_size; /* directory always takes up one block */
	inode->i_block[0] = dir_bno;
	now_time(&inode->i_ctime, &inode->i_ctime_nsec);
	inode->i_mtime = inode->i_ctime;
	inode->i_mtime_nsec = inode->i_ctime_nsec;
	write_inode(ctx, inode);
}

static bool add_entry_to_block(struct brkfs_ctx *ctx, struct brkfs_dir_entry *e,
			       const char *name, uint8_t name_len, uint8_t type,
			       uint32_t ino)
{
	uint32_t min_len = (8 + name_len + 3) & ~3;
	struct brkfs_dir_entry *end, *new_e;

	end = (struct brkfs_dir_entry *)((uint8_t *)e + ctx->block_size);
	uint32_t el = ctx->block_size;

	for (; e < end; e = (struct brkfs_dir_entry *)((uint8_t *)e + el)) {
		el = e->entry_len;

		if (el == 0)
			break;
		if (el < min_len)
			continue;

		if (e->inode > 0) {
			uint32_t el_min = (8 + e->name_len + 3) & ~3;
			if (el_min > el - min_len)
				continue;

			new_e = (struct brkfs_dir_entry *)((uint8_t *)e +
							   el_min);
			e->entry_len = el_min;

			new_e->inode = ino;
			new_e->entry_len = el - el_min;
			new_e->name_len = name_len;
			new_e->file_type = type;
			memcpy(new_e->name, name, name_len);
			return true;
		}

		e->inode = ino;
		e->name_len = name_len;
		e->file_type = type;
		memcpy(e->name, name, name_len);
		return true;
	}
	return false;
}

void add_dir_entry(struct brkfs_ctx *ctx, uint32_t dir_ino, const char *name,
		   uint8_t type, uint32_t ino)
{
	void *buf = NULL;
	struct brkfs_inode inode = { .i_ino = dir_ino };
	size_t name_len = strlen(name);
	if (name_len > BRKFS_NAME_LEN)
		die_prog("name too long: %s", name);

	read_inode(ctx, &inode);

	buf = xmalloc(ctx->block_size);

	if (inode.i_size > 0) {
		uint64_t nblocks = (uint64_t)inode.i_size / ctx->block_size;

		for (uint64_t bi = 0; bi < nblocks; bi++) {
			uint32_t bno = lookup_inode_bno(ctx, &inode, bi);

			if (bno == 0)
				continue;
			read_block(ctx, bno, buf);
			if (add_entry_to_block(ctx, buf, name, name_len, type,
					       ino)) {
				write_block(ctx, bno, buf);
				free(buf);
				return;
			}
		}
	}

	uint64_t newi =
		inode.i_size > 0 ? (uint64_t)inode.i_size / ctx->block_size : 0;
	uint32_t bno = alloc_data(ctx);

	assign_inode_bno(ctx, &inode, newi, bno);
	memset(buf, 0, ctx->block_size);
	struct brkfs_dir_entry *e = buf;

	e->inode = 0;
	e->entry_len = ctx->block_size;
	e->name_len = 0;
	e->file_type = DT_UNKNOWN;
	e->name[0] = '\0';
	if (!add_entry_to_block(ctx, buf, name, name_len, type, ino))
		die_prog("directory entry failed (internal error)");
	inode.i_size += ctx->block_size;
	now_time(&inode.i_ctime, &inode.i_ctime_nsec);
	inode.i_mtime = inode.i_ctime;
	inode.i_mtime_nsec = inode.i_ctime_nsec;
	write_block(ctx, bno, buf);
	free(buf);
	write_inode(ctx, &inode);
}

bool is_inode_dir(struct brkfs_ctx *ctx, uint32_t ino)
{
	struct brkfs_inode inode = { .i_ino = ino };

	read_inode(ctx, &inode);
	return (inode.i_mode & S_IFMT) == S_IFDIR;
}

/*
 * Resolve a path name. Walks all but the last component, which may not
 * exist yet. Returns the inode of the last component (0 if it does not
 * exist) and fills *out with its parent directory inode, basename, and
 * whether the resolved component is a directory. "/" resolves to the root
 * inode with an empty basename.
 */
uint32_t resolve_path(struct brkfs_ctx *ctx, const char *path, unsigned flags,
		      struct brkfs_path *out)
{
	char buf[BRKFS_PATH_MAX];
	size_t len = strlen(path);

	if (len == 0 || len >= sizeof(buf))
		die_prog("invalid path");

	memcpy(buf, path, len + 1);

	out->parent_ino = BRKFS_ROOT_INO;
	out->ino = 0;
	out->is_dir = false;
	out->basename[0] = '\0';

	char *s = buf;

	while (*s == '/')
		s++;
	if (*s == '\0') {
		out->parent_ino = BRKFS_ROOT_INO;
		out->ino = BRKFS_ROOT_INO;
		out->is_dir = true;
		return out->ino;
	}

	char *slash = strrchr(s, '/');
	char *base = s;

	if (slash) {
		*slash = '\0';
		base = slash + 1;
		if (*base == '\0')
			die_prog("invalid path (trailing '/')");

		if (*s != '\0') {
			char *save = NULL;
			char *tok = strtok_r(s, "/", &save);
			uint32_t cur = BRKFS_ROOT_INO;

			for (; tok != NULL; tok = strtok_r(NULL, "/", &save)) {
				if (strlen(tok) > BRKFS_NAME_LEN)
					die_prog("path component too long: %s",
						 tok);

				uint32_t ch = lookup_dir_entry(ctx, cur, tok);

				if (ch == 0) {
					if (!(flags &
					      BRKFS_PATH_CREATE_PARENTS)) {
						if (flags &
						    BRKFS_PATH_HINT_RECURSIVE)
							die_prog(
								"missing directory '%s' (use -r or --recursive to create parents)",
								tok);
						die_prog(
							"no such file or directory: %s",
							path);
					}
					struct brkfs_inode newdir;

					alloc_dir_inode(ctx, &newdir, 0, 0);
					add_dir_entry(ctx, cur, tok, DT_DIR,
						      newdir.i_ino);
					cur = newdir.i_ino;
				} else {
					if (!is_inode_dir(ctx, ch))
						die_prog("not a directory: %s",
							 path);
					cur = ch;
				}
			}
			out->parent_ino = cur;
		}
	}

	size_t blen = strlen(base);

	if (blen > BRKFS_NAME_LEN)
		die_prog("name too long: %s", base);
	memcpy(out->basename, base, blen + 1);

	out->ino = lookup_dir_entry(ctx, out->parent_ino, out->basename);
	if (out->ino != 0)
		out->is_dir = is_inode_dir(ctx, out->ino);
	return out->ino;
}

static void print_row(const char *region, uint32_t start_blk, uint32_t nblocks)
{
	uint32_t end_blk = nblocks > 0 ? start_blk + nblocks - 1 : start_blk;

	printf("  %-26s %10" PRIu32 " %10" PRIu32 " %10" PRIu32 "\n", region,
	       start_blk, nblocks, end_blk);
}

void print_layout(struct brkfs_ctx *ctx)
{
	uint64_t nbytes = (uint64_t)ctx->block_count * ctx->block_size;
	uint32_t last = ctx->block_count > 0 ? ctx->block_count - 1 : 0;

	printf("\n");
	printf("brkfs layout\n");
	printf("------------\n");
	printf("  %-26s %10" PRIu32 " bytes\n", "Block size", ctx->block_size);
	printf("  %-26s %10" PRIu32 " bytes\n", "Inode size", ctx->inode_size);
	printf("  %-26s %10" PRIu32 "\n", "Total blocks", ctx->block_count);
	printf("  %-26s %10" PRIu32 "\n", "Total inodes", ctx->inode_count);
	printf("  %-26s %10" PRIu64 " bytes\n", "Volume size", nbytes);
	printf("\n");
	printf("  %-26s %10s %10s %10s\n", "Region", "Start", "Blocks", "End");
	printf("  %-26s %10s %10s %10s\n", "--------------------------",
	       "----------", "----------", "----------");

	print_row("Reserved (boot + super)", 0, ctx->inode_bitmap_bno);
	print_row("Inode bitmap", ctx->inode_bitmap_bno,
		  ctx->inode_bitmap_blocks);
	print_row("Data block bitmap", ctx->data_bitmap_bno,
		  ctx->data_bitmap_blocks);
	print_row("Inode table", ctx->inode_table_bno, ctx->inode_table_blocks);
	print_row("Data (file blocks)", ctx->first_data_bno,
		  ctx->data_block_count);

	printf("\n");
	printf("  Last block number: %" PRIu32 "\n\n", last);
}
