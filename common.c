#include "common.h"
#include "brkfs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
	return (uint32_t)div_round_up_u64(num, div);
}

uint64_t div_round_up_u64(uint64_t num, uint64_t div)
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
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t r = pread(fd, p, len, off);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			die_errno("read");
		}
		if (r == 0)
			die_prog("short read (%zu bytes missing)", len);
		p += r;
		off += r;
		len -= (size_t)r;
	}
}

static void write_full(int fd, const void *buf, size_t len, off_t off)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t r = pwrite(fd, p, len, off);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			die_errno("write");
		}
		if (r == 0)
			die_prog("short write (%zu bytes missing)", len);
		p += r;
		off += r;
		len -= (size_t)r;
	}
}

void write_super(struct brkfs_ctx *ctx, struct brkfs_super_block *sb)
{
	brkfs_super_set_checksum(sb);
	write_full(ctx->imgfd, sb, sizeof(*sb), BRKFS_SUPER_OFFSET);

	if (sb->s_backup_super != 0) {
		off_t off = (off_t)sb->s_backup_super * ctx->block_size;

		write_full(ctx->imgfd, sb, sizeof(*sb), off);
	}
}

void read_super(int imgfd, struct brkfs_super_block *sb)
{
	read_full(imgfd, sb, sizeof(*sb), BRKFS_SUPER_OFFSET);
}

void read_super_at(int imgfd, struct brkfs_super_block *sb, uint64_t offset)
{
	read_full(imgfd, sb, sizeof(*sb), (off_t)offset);
}

bool super_sane(const struct brkfs_super_block *sb)
{
	struct brkfs_super_block copy = *sb;

	if (sb->s_magic != BRKFS_MAGIC)
		return false;
	if (sb->s_version != BRKFS_FORMAT_VERSION)
		return false;
	if (copy.s_checksum != brkfs_super_checksum(&copy))
		return false;
	return true;
}

static uint32_t super_blocks(uint32_t block_size)
{
	return div_round_up_u32(BRKFS_SUPER_END_OFFSET, block_size);
}

static bool range_fits(uint64_t start, uint64_t blocks, uint64_t limit)
{
	return start <= limit && blocks <= limit - start;
}

void validate_super(const struct brkfs_super_block *sb, uint64_t image_size)
{
	uint64_t volume_size;
	uint32_t bits_per_block;
	uint32_t inodes_per_block;
	uint32_t inode_bitmap_blocks;
	uint32_t data_bitmap_blocks;
	uint32_t inode_table_blocks;
	uint32_t metadata_end;
	uint64_t data_limit;

	if (sb->s_magic != BRKFS_MAGIC)
		die_prog("invalid super block magic number");
	if (sb->s_version != BRKFS_FORMAT_VERSION)
		die_prog("unsupported format version: %u (expected %u)",
			 sb->s_version, BRKFS_FORMAT_VERSION);
	if (sb->s_features & ~BRKFS_FEATURE_ALL)
		die_prog("unknown format features: 0x%08x", sb->s_features);

	struct brkfs_super_block copy = *sb;

	if (copy.s_checksum != brkfs_super_checksum(&copy))
		die_prog("bad super block checksum");
	if (sb->s_state < BRKFS_STATE_CLEAN ||
	    sb->s_state > BRKFS_STATE_ERROR)
		die_prog("invalid super block state: %u", sb->s_state);
	if (sb->s_free_blocks > sb->s_data_blocks_count)
		die_prog("invalid free block count: %u", sb->s_free_blocks);
	if (sb->s_free_inodes > sb->s_inodes_count)
		die_prog("invalid free inode count: %u", sb->s_free_inodes);
	if (sb->s_orphan > sb->s_inodes_count)
		die_prog("invalid orphan inode: %u", sb->s_orphan);

	if (sb->s_blocksize < BRKFS_MIN_BLOCK_SIZE ||
	    sb->s_blocksize > BRKFS_MAX_BLOCK_SIZE ||
	    !is_pow_of_two(sb->s_blocksize))
		die_prog("invalid block size in super block: %u",
			 sb->s_blocksize);
	if (sb->s_inode_size < sizeof(struct brkfs_inode) ||
	    sb->s_inode_size > BRKFS_MAX_INODE_SIZE ||
	    !is_pow_of_two(sb->s_inode_size) ||
	    sb->s_blocksize % sb->s_inode_size != 0)
		die_prog("invalid inode size in super block: %u",
			 sb->s_inode_size);
	if (sb->s_blocks_count == 0)
		die_prog("invalid block count in super block");
	if (sb->s_blocks_count < 2)
		die_prog("block count is too small for a backup super block");
	if (sb->s_backup_super != sb->s_blocks_count - 1)
		die_prog("invalid backup super block: %u",
			 sb->s_backup_super);
	if (sb->s_inodes_count == 0)
		die_prog("invalid inode count in super block");
	if (sb->s_data_blocks_count == 0)
		die_prog("invalid data block count in super block");

	volume_size = (uint64_t)sb->s_blocks_count * sb->s_blocksize;
	if (image_size != 0 && volume_size > image_size)
		die_prog("image is too small for super block: need %" PRIu64
			 " bytes, got %" PRIu64 " bytes",
			 volume_size, image_size);

	bits_per_block = sb->s_blocksize * 8;
	inodes_per_block = sb->s_blocksize / sb->s_inode_size;
	inode_bitmap_blocks =
		div_round_up_u32(sb->s_inodes_count, bits_per_block);
	data_bitmap_blocks =
		div_round_up_u32(sb->s_data_blocks_count, bits_per_block);
	inode_table_blocks =
		div_round_up_u32(sb->s_inodes_count, inodes_per_block);

	if (sb->s_inode_bitmap < super_blocks(sb->s_blocksize))
		die_prog("inode bitmap overlaps reserved area");
	if (sb->s_data_block_bitmap <
	    (uint64_t)sb->s_inode_bitmap + inode_bitmap_blocks)
		die_prog("data bitmap overlaps inode bitmap");
	if (sb->s_inode_table <
	    (uint64_t)sb->s_data_block_bitmap + data_bitmap_blocks)
		die_prog("inode table overlaps data bitmap");
	if (sb->s_first_data_block <
	    (uint64_t)sb->s_inode_table + inode_table_blocks)
		die_prog("data region overlaps inode table");

	data_limit = sb->s_backup_super;

	metadata_end = sb->s_first_data_block;
	if (!range_fits(metadata_end, sb->s_data_blocks_count, data_limit))
		die_prog("data region is outside the image");
}

void super_to_ctx(const struct brkfs_super_block *sb, struct brkfs_ctx *ctx)
{
	ctx->block_size = sb->s_blocksize;
	ctx->inode_size = sb->s_inode_size;
	ctx->block_count = sb->s_blocks_count;
	ctx->inode_count = sb->s_inodes_count;
	ctx->version = sb->s_version;
	ctx->features = sb->s_features;
	ctx->state = sb->s_state;
	memcpy(ctx->uuid, sb->s_uuid, sizeof(ctx->uuid));
	memcpy(ctx->label, sb->s_label, sizeof(ctx->label));
	ctx->label[sizeof(ctx->label) - 1] = '\0';
	ctx->free_blocks = sb->s_free_blocks;
	ctx->free_inodes = sb->s_free_inodes;
	ctx->mtime = sb->s_mtime;
	ctx->lastcheck = sb->s_lastcheck;
	ctx->mnt_count = sb->s_mnt_count;
	ctx->max_mnt_count = sb->s_max_mnt_count;
	ctx->backup_super_bno = sb->s_backup_super;
	ctx->orphan = sb->s_orphan;
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
	sb->s_version = ctx->version;
	sb->s_features = ctx->features;
	sb->s_state = ctx->state;
	memcpy(sb->s_uuid, ctx->uuid, sizeof(sb->s_uuid));
	memcpy(sb->s_label, ctx->label, sizeof(sb->s_label));
	sb->s_free_blocks = ctx->free_blocks;
	sb->s_free_inodes = ctx->free_inodes;
	sb->s_mtime = ctx->mtime;
	sb->s_lastcheck = ctx->lastcheck;
	sb->s_mnt_count = ctx->mnt_count;
	sb->s_max_mnt_count = ctx->max_mnt_count;
	sb->s_backup_super = ctx->backup_super_bno;
	sb->s_orphan = ctx->orphan;
}

void open_ctx(int imgfd, struct brkfs_ctx *ctx)
{
	struct brkfs_super_block sb;
	struct stat st;
	int flags;
	bool found = false;

	memset(ctx, 0, sizeof(*ctx));

	flags = fcntl(imgfd, F_GETFL);
	if (flags < 0)
		die_errno("fcntl");
	if (fstat(imgfd, &st) < 0)
		die_errno("fstat");

	read_super(imgfd, &sb);
	if (super_sane(&sb)) {
		found = true;
	} else if (st.st_size > 0) {
		for (uint32_t bs = BRKFS_MIN_BLOCK_SIZE;
		     bs <= BRKFS_MAX_BLOCK_SIZE; bs *= 2) {
			struct brkfs_super_block backup;

			if ((uint64_t)st.st_size < bs)
				continue;
			read_super_at(imgfd, &backup,
				      (uint64_t)st.st_size - bs);
			if (!super_sane(&backup) ||
			    backup.s_blocksize != bs ||
			    backup.s_backup_super !=
				    backup.s_blocks_count - 1)
				continue;
			sb = backup;
			found = true;
			break;
		}
	}
	if (!found)
		die_prog("invalid super block magic number");

	validate_super(&sb, st.st_size > 0 ? (uint64_t)st.st_size : 0);

	ctx->imgfd = imgfd;
	ctx->read_only = (flags & O_ACCMODE) == O_RDONLY;
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

static size_t dir_usable_size(const struct brkfs_ctx *ctx)
{
	return ctx->block_size - BRKFS_DIR_CSUM_SIZE;
}

static void read_dir_block(struct brkfs_ctx *ctx, uint32_t bno, void *buf)
{
	read_block(ctx, bno, buf);
	if (!brkfs_dir_checksum_ok(buf, ctx->block_size))
		die_prog("directory block %u: bad checksum", bno);
}

static void write_dir_block(struct brkfs_ctx *ctx, uint32_t bno, void *buf)
{
	brkfs_dir_set_checksum(buf, ctx->block_size);
	write_block(ctx, bno, buf);
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
	if (ctx->free_inodes > 0)
		ctx->free_inodes--;
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
	inode->i_next_orphan = 0;
	inode->i_checksum = 0;
	memset(inode->i_block, 0, sizeof(inode->i_block));
	alloc_inode_slot(ctx, inode);
}

uint32_t alloc_data(struct brkfs_ctx *ctx)
{
	uint32_t bit = 0;

	if (bitmap_alloc(ctx, ctx->data_bitmap_bno, ctx->data_bitmap_bits,
			 &ctx->data_bitmap_hint, &bit)) {
		if (ctx->free_blocks > 0)
			ctx->free_blocks--;
		return ctx->first_data_bno + bit;
	}

	die_prog("no free data blocks");
}

static uint32_t pointers_per_block(const struct brkfs_ctx *ctx)
{
	return ctx->block_size / sizeof(uint32_t);
}

uint64_t max_inode_file_blocks(const struct brkfs_ctx *ctx)
{
	uint64_t n = pointers_per_block(ctx);
	uint64_t d = BRKFS_DIRECT_BLOCKS;

	return d + n + n * n + n * n * n;
}

uint64_t max_inode_file_bytes(const struct brkfs_ctx *ctx)
{
	uint64_t blocks = max_inode_file_blocks(ctx);
	uint64_t bytes = blocks * (uint64_t)ctx->block_size;

	if (bytes > UINT32_MAX)
		return UINT32_MAX;
	return bytes;
}

/*
 * Decompose a logical file block into the indirect path that indexes it.
 * Return 0 for a direct block, or the number of indirect levels (1..3).
 * idx[] receives the per-level indices, starting at the inode's indirect
 * root and ending at the leaf pointer for the data block.
 */
static unsigned inode_block_path(struct brkfs_ctx *ctx, uint64_t file_blk,
				 uint32_t idx[3])
{
	uint64_t n = pointers_per_block(ctx);
	uint64_t u;

	if (file_blk < BRKFS_DIRECT_BLOCKS)
		return 0;

	u = file_blk - BRKFS_DIRECT_BLOCKS;
	if (u < n) {
		idx[0] = (uint32_t)u;
		return 1;
	}
	u -= n;

	if (u < n * n) {
		idx[0] = (uint32_t)(u / n);
		idx[1] = (uint32_t)(u % n);
		return 2;
	}
	u -= n * n;

	if (u < n * n * n) {
		idx[0] = (uint32_t)(u / (n * n));
		idx[1] = (uint32_t)((u / n) % n);
		idx[2] = (uint32_t)(u % n);
		return 3;
	}

	die_prog("file block index out of range: %" PRIu64, file_blk);
}

uint32_t lookup_inode_bno(struct brkfs_ctx *ctx,
			  const struct brkfs_inode *inode, uint64_t file_blk)
{
	uint32_t idx[3];
	unsigned levels = inode_block_path(ctx, file_blk, idx);
	uint32_t bno;
	uint8_t *buf;

	if (levels == 0)
		return inode->i_block[file_blk];

	bno = inode->i_block[BRKFS_INDIRECT_BLOCK + levels - 1];
	if (bno == 0)
		return 0;

	buf = xmalloc(ctx->block_size);

	for (unsigned level = 0; level < levels; level++) {
		read_block(ctx, bno, buf);
		bno = ((uint32_t *)buf)[idx[level]];
		if (bno == 0)
			break;
	}

	free(buf);
	return bno;
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

static uint32_t feature_with_deps(uint32_t feature)
{
	/*
	 * A triple-indirect tree walks through double-indirect levels too, so
	 * an image using triple indirect must advertise both bits.
	 */
	if (feature & BRKFS_FEATURE_TRIPLE_INDIRECT)
		feature |= BRKFS_FEATURE_DOUBLE_INDIRECT;
	return feature;
}

static void set_feature(struct brkfs_ctx *ctx, uint32_t feature)
{
	feature = feature_with_deps(feature);
	if ((ctx->features & feature) == feature)
		return;
	ctx->features |= feature;
}

void assign_inode_bno(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		      uint64_t file_blk, uint32_t data_bno)
{
	uint32_t idx[3];
	unsigned levels = inode_block_path(ctx, file_blk, idx);
	uint8_t *buf;
	uint8_t *zero = NULL;
	uint32_t bno;

	if (levels == 0) {
		inode->i_block[file_blk] = data_bno;
		return;
	}

	if (S_ISDIR(inode->i_mode))
		set_feature(ctx, BRKFS_FEATURE_DIR_INDIRECT);
	if (levels == 2)
		set_feature(ctx, BRKFS_FEATURE_DOUBLE_INDIRECT);
	else if (levels == 3)
		set_feature(ctx, BRKFS_FEATURE_TRIPLE_INDIRECT);

	ensure_indirect_root(ctx, inode, BRKFS_INDIRECT_BLOCK + levels - 1);
	bno = inode->i_block[BRKFS_INDIRECT_BLOCK + levels - 1];

	buf = xmalloc(ctx->block_size);

	for (unsigned level = 0; level < levels; level++) {
		uint32_t *ptrs;

		read_block(ctx, bno, buf);
		ptrs = (uint32_t *)buf;

		if (level + 1 == levels) {
			ptrs[idx[level]] = data_bno;
			write_block(ctx, bno, buf);
			break;
		}

		if (ptrs[idx[level]] == 0) {
			uint32_t child = alloc_data(ctx);

			if (zero == NULL) {
				zero = xmalloc(ctx->block_size);
				memset(zero, 0, ctx->block_size);
			}
			write_block(ctx, child, zero);
			ptrs[idx[level]] = child;
			write_block(ctx, bno, buf);
		}
		bno = ptrs[idx[level]];
	}

	free(zero);
	free(buf);
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

	uint64_t nblocks = div_round_up_u64(inode.i_size, ctx->block_size);
	uint8_t *buf = xmalloc(ctx->block_size);

	for (uint64_t bi = 0; bi < nblocks; bi++) {
		uint32_t bno = lookup_inode_bno(ctx, &inode, bi);

		if (bno == 0)
			continue;
		read_dir_block(ctx, bno, buf);
		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)buf;
		struct brkfs_dir_entry *end =
			(struct brkfs_dir_entry *)(buf + dir_usable_size(ctx));
		uint32_t el = dir_usable_size(ctx);

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
	brkfs_inode_set_checksum(inode);
	memcpy(slot, inode, sizeof(*inode));
	write_block(ctx, bno, block);
	free(block);
}

void touch_inode_atime(struct brkfs_ctx *ctx, struct brkfs_inode *inode)
{
	uint32_t sec;
	uint32_t nsec;

	if (ctx->read_only)
		return;

	now_time(&sec, &nsec);
	if (inode->i_atime == sec && inode->i_atime_nsec == nsec)
		return;
	inode->i_atime = sec;
	inode->i_atime_nsec = nsec;
	write_inode(ctx, inode);
}

uint32_t alloc_dir_data(struct brkfs_ctx *ctx, uint32_t dir_ino,
			uint32_t parent_ino, bool first)
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
		e->inode = parent_ino;
		e->entry_len = dir_usable_size(ctx) - 12;
		e->name_len = 2;
		e->file_type = DT_DIR;
		e->name[0] = '.';
		e->name[1] = '.';
	} else {
		e->inode = 0;
		e->entry_len = dir_usable_size(ctx);
		e->name_len = 0;
		e->file_type = DT_UNKNOWN;
		e->name[0] = '\0';
	}

	write_dir_block(ctx, bno, buf);
	free(buf);
	return bno;
}

void alloc_dir_inode(struct brkfs_ctx *ctx, struct brkfs_inode *inode,
		     uint32_t mode, uint32_t parent_ino, uint32_t flags)
{
	mode = (mode & 07777) | S_IFDIR;
	alloc_inode(ctx, inode, mode, 0, flags);
	uint32_t dir_ino = inode->i_ino;
	uint32_t dir_bno = alloc_dir_data(ctx, dir_ino, parent_ino, true);
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

	end = (struct brkfs_dir_entry *)((uint8_t *)e + dir_usable_size(ctx));
	uint32_t el = dir_usable_size(ctx);

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
		uint64_t nblocks =
			div_round_up_u64(inode.i_size, ctx->block_size);

		for (uint64_t bi = 0; bi < nblocks; bi++) {
			uint32_t bno = lookup_inode_bno(ctx, &inode, bi);

			if (bno == 0)
				continue;
			read_dir_block(ctx, bno, buf);
			if (add_entry_to_block(ctx, buf, name, name_len, type,
					       ino)) {
				write_dir_block(ctx, bno, buf);
				now_time(&inode.i_ctime,
					 &inode.i_ctime_nsec);
				inode.i_mtime = inode.i_ctime;
				inode.i_mtime_nsec = inode.i_ctime_nsec;
				if (type == DT_DIR)
					inode.i_nlink++;
				write_inode(ctx, &inode);
				free(buf);
				return;
			}
		}
	}

	uint64_t newi = 0;
	uint32_t bno;

	if (inode.i_size > 0)
		newi = div_round_up_u64(inode.i_size, ctx->block_size);

	bno = alloc_data(ctx);

	assign_inode_bno(ctx, &inode, newi, bno);
	memset(buf, 0, ctx->block_size);
	struct brkfs_dir_entry *e = buf;

	e->inode = 0;
	e->entry_len = dir_usable_size(ctx);
	e->name_len = 0;
	e->file_type = DT_UNKNOWN;
	e->name[0] = '\0';
	if (!add_entry_to_block(ctx, buf, name, name_len, type, ino))
		die_prog("directory entry failed (internal error)");
	inode.i_size += ctx->block_size;
	if (type == DT_DIR)
		inode.i_nlink++;
	now_time(&inode.i_ctime, &inode.i_ctime_nsec);
	inode.i_mtime = inode.i_ctime;
	inode.i_mtime_nsec = inode.i_ctime_nsec;
	write_dir_block(ctx, bno, buf);
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

					alloc_dir_inode(ctx, &newdir, 0777, cur, 0);
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

void print_features(uint32_t features)
{
	static const struct {
		uint32_t bit;
		const char *name;
	} names[] = {
		{ BRKFS_FEATURE_DOUBLE_INDIRECT, "double-indirect" },
		{ BRKFS_FEATURE_TRIPLE_INDIRECT, "triple-indirect" },
		{ BRKFS_FEATURE_DIR_INDIRECT, "directory-indirect" },
	};
	bool first = true;

	if (features == 0) {
		printf("none\n");
		return;
	}

	printf("0x%08" PRIx32 " (", features);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!(features & names[i].bit))
			continue;
		printf("%s%s", first ? "" : ", ", names[i].name);
		first = false;
	}
	printf(")\n");
}

static const char *state_name(uint32_t state)
{
	switch (state) {
	case BRKFS_STATE_CLEAN:
		return "clean";
	case BRKFS_STATE_DIRTY:
		return "dirty";
	case BRKFS_STATE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static void print_uuid(const uint8_t uuid[16])
{
	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			putchar('-');
		printf("%02x", uuid[i]);
	}
}

void print_layout(const struct brkfs_ctx *ctx, const char *title)
{
	uint64_t nbytes = (uint64_t)ctx->block_count * ctx->block_size;
	uint32_t last = ctx->block_count > 0 ? ctx->block_count - 1 : 0;

	printf("\n");
	printf("%s\n", title);
	printf("------------\n");
	printf("  %-26s %10" PRIu32 "\n", "Format version", ctx->version);
	printf("  %-26s ", "Format features");
	print_features(ctx->features);
	printf("  %-26s %10s\n", "State", state_name(ctx->state));
	printf("  %-26s ", "UUID");
	print_uuid(ctx->uuid);
	printf("\n");
	printf("  %-26s %10s\n", "Label",
	       ctx->label[0] ? ctx->label : "(none)");
	printf("  %-26s %10" PRIu32 " bytes\n", "Block size", ctx->block_size);
	printf("  %-26s %10" PRIu32 " bytes\n", "Inode size", ctx->inode_size);
	printf("  %-26s %10" PRIu32 "\n", "Total blocks", ctx->block_count);
	printf("  %-26s %10" PRIu32 "\n", "Total inodes", ctx->inode_count);
	printf("  %-26s %10" PRIu32 "\n", "Free blocks", ctx->free_blocks);
	printf("  %-26s %10" PRIu32 "\n", "Free inodes", ctx->free_inodes);
	printf("  %-26s %10" PRIu32 "\n", "Mount count", ctx->mnt_count);
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
	if (ctx->backup_super_bno != 0)
		print_row("Backup superblock", ctx->backup_super_bno, 1);

	printf("\n");
	printf("  %-26s %10" PRIu32 "\n", "Direct blocks/inode",
	       (uint32_t)BRKFS_DIRECT_BLOCKS);
	printf("  %-26s %10" PRIu64 "\n", "Pointers per block",
	       (uint64_t)pointers_per_block(ctx));
	printf("  %-26s %10" PRIu64 "\n", "Max blocks/inode",
	       max_inode_file_blocks(ctx));
	printf("  %-26s %10" PRIu64 " bytes\n", "Max file size (i_size)",
	       max_inode_file_bytes(ctx));

	printf("\n");
	printf("  Last block number: %" PRIu32 "\n\n", last);
}
