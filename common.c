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
#include <unistd.h>

static const char *prog_name = "unknown";

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

noreturn void die_argf(const char *fmt, ...)
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

bool is_pow_of_two_z(size_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

uint32_t div_round_up_u32(uint32_t num, uint32_t div)
{
	return (num + div - 1) / div;
}

void write_super(struct brkfs_volume *vol, struct brkfs_super_block *sb)
{
	if (lseek(vol->imgfd, BRKFS_SUPER_OFFSET, SEEK_SET) !=
	    BRKFS_SUPER_OFFSET)
		die_errno("lseek");
	if (write(vol->imgfd, sb, sizeof(*sb)) != sizeof(*sb))
		die_errno("write");
}

void read_super(int imgfd, struct brkfs_super_block *sb)
{
	if (lseek(imgfd, BRKFS_SUPER_OFFSET, SEEK_SET) != BRKFS_SUPER_OFFSET)
		die_errno("lseek");
	if (read(imgfd, sb, sizeof(*sb)) != sizeof(*sb))
		die_errno("read");
}

void read_block(struct brkfs_volume *vol, uint32_t bno, void *buf)
{
	if (bno >= vol->blocks)
		die_prog("block number out of range: %u (max %u)", bno,
			 vol->blocks);
	off_t offset = bno * vol->bs;
	if (lseek(vol->imgfd, offset, SEEK_SET) != offset)
		die_errno("lseek");
	if (read(vol->imgfd, buf, vol->bs) != vol->bs)
		die_errno("read");
}

void write_block(struct brkfs_volume *vol, uint32_t bno, void *buf)
{
	if (bno >= vol->blocks)
		die_prog("block number out of range: %u (max %u)", bno,
			 vol->blocks);
	off_t offset = bno * vol->bs;
	if (lseek(vol->imgfd, offset, SEEK_SET) != offset)
		die_errno("lseek");
	if (write(vol->imgfd, buf, vol->bs) != (ssize_t)vol->bs)
		die_errno("write");
}

static bool __bitmap_alloc(uint8_t *map, uint32_t nbits, uint32_t *bit)
{
	uint32_t byte = 0;
	uint32_t nbytes = div_round_up_u32(nbits, 8);
	for (; byte < nbytes; byte++) {
		uint32_t b = byte * 8;
		uint32_t n = nbits - b;
		if (n > 8)
			n = 8;
		for (uint32_t i = 0; i < n; i++) {
			if (map[byte] & (1 << i))
				continue;
			map[byte] |= (1 << i);
			*bit = b + i;
			return true;
		}
	}
	return false;
}

static bool bitmap_alloc(struct brkfs_volume *vol, uint32_t bmap_bno,
			 uint32_t nbits, uint32_t *bit)
{
	uint8_t *block = xmalloc(vol->bs);
	uint32_t bits_per_block = vol->bs * 8;
	uint32_t base = 0;

	while (base < nbits) {
		uint32_t bno = bmap_bno + (base / bits_per_block);
		read_block(vol, bno, block);
		uint32_t n = nbits - base;
		if (n > bits_per_block)
			n = bits_per_block;
		uint32_t rel;
		if (__bitmap_alloc(block, n, &rel)) {
			*bit = base + rel;
			write_block(vol, bno, block);
			free(block);
			return true;
		}
		base += n;
	}
	free(block);
	return false;
}

static void __alloc_inode(struct brkfs_volume *vol, struct brkfs_inode *inode)
{
	uint32_t bit = 0;

	if (!bitmap_alloc(vol, vol->ibmap_bno, vol->ibmap_bits, &bit))
		die_prog("no free inodes");
	inode->i_ino = bit + 1;
	memset(inode->i_block, 0, sizeof(inode->i_block));
	write_inode(vol, inode);
}

void alloc_inode(struct brkfs_volume *vol, struct brkfs_inode *inode,
		 uint32_t mode, uint32_t rdev, uint32_t flags)
{
	inode->i_ino = 0;
	inode->i_mode = mode;
	inode->i_rdev = rdev;
	inode->i_flags = flags;
	inode->i_nlink = 1;
	inode->i_size = 0;
	memset(inode->i_block, 0, sizeof(inode->i_block));
	__alloc_inode(vol, inode);
}

uint32_t alloc_data(struct brkfs_volume *vol)
{
	uint32_t bit = 0;

	if (bitmap_alloc(vol, vol->dbmap_bno, vol->dbmap_bits, &bit))
		return vol->dfirst_bno + bit;

	die_prog("no free data blocks");
}

static uint32_t ptrs_per_block(struct brkfs_volume *vol)
{
	return vol->bs / sizeof(uint32_t);
}

static uint64_t inode_max_file_blks(struct brkfs_volume *vol)
{
	uint64_t n = ptrs_per_block(vol);
	uint64_t d = BRKFS_DIRECT_BLOCKS;

	return d + n + n * n + n * n * n;
}

uint64_t inode_max_file_bytes(struct brkfs_volume *vol)
{
	uint64_t blks = inode_max_file_blks(vol);
	uint64_t bytes = blks * (uint64_t)vol->bs;

	if (bytes > UINT32_MAX)
		return UINT32_MAX;
	return bytes;
}

uint32_t inode_lookup_bno(struct brkfs_volume *vol,
			  const struct brkfs_inode *inode, uint64_t file_blk)
{
	uint32_t n = ptrs_per_block(vol);
	uint64_t maxb = inode_max_file_blks(vol);

	if (file_blk >= maxb)
		die_prog("file block index out of range: %" PRIu64, file_blk);

	if (file_blk < BRKFS_DIRECT_BLOCKS)
		return inode->i_block[file_blk];

	uint64_t u = file_blk - BRKFS_DIRECT_BLOCKS;

	if (u < (uint64_t)n) {
		uint32_t ib = inode->i_block[BRKFS_INDIRECT_BLOCK];

		if (ib == 0)
			return 0;
		uint8_t *buf = xmalloc(vol->bs);

		read_block(vol, ib, buf);
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
		uint8_t *b0 = xmalloc(vol->bs);

		read_block(vol, dib, b0);
		uint32_t l1 = ((uint32_t *)b0)[r1];

		free(b0);
		if (l1 == 0)
			return 0;
		uint8_t *b1 = xmalloc(vol->bs);

		read_block(vol, l1, b1);
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
		uint8_t *b0 = xmalloc(vol->bs);

		read_block(vol, tib, b0);
		uint32_t l1 = ((uint32_t *)b0)[r1];

		free(b0);
		if (l1 == 0)
			return 0;
		uint8_t *b1 = xmalloc(vol->bs);

		read_block(vol, l1, b1);
		uint32_t l2 = ((uint32_t *)b1)[r2];

		free(b1);
		if (l2 == 0)
			return 0;
		uint8_t *b2 = xmalloc(vol->bs);

		read_block(vol, l2, b2);
		uint32_t r = ((uint32_t *)b2)[r3];
		free(b2);
		return r;
	}

	die_prog("internal: inode_lookup_bno past triple");
	return 0;
}

static void ensure_indirect_root(struct brkfs_volume *vol,
				 struct brkfs_inode *inode, int slot)
{
	if (inode->i_block[slot] != 0)
		return;

	uint32_t b = alloc_data(vol);
	uint8_t *z = xmalloc(vol->bs);

	memset(z, 0, vol->bs);
	write_block(vol, b, z);
	free(z);
	inode->i_block[slot] = b;
}

void inode_assign_bno(struct brkfs_volume *vol, struct brkfs_inode *inode,
		      uint64_t file_blk, uint32_t data_bno)
{
	uint32_t n = ptrs_per_block(vol);
	uint64_t maxb = inode_max_file_blks(vol);

	if (file_blk >= maxb)
		die_prog("file block index out of range: %" PRIu64, file_blk);

	if (file_blk < BRKFS_DIRECT_BLOCKS) {
		inode->i_block[file_blk] = data_bno;
		return;
	}

	uint64_t u = file_blk - BRKFS_DIRECT_BLOCKS;

	if (u < (uint64_t)n) {
		ensure_indirect_root(vol, inode, BRKFS_INDIRECT_BLOCK);
		uint32_t ib = inode->i_block[BRKFS_INDIRECT_BLOCK];
		uint8_t *buf = xmalloc(vol->bs);

		read_block(vol, ib, buf);
		((uint32_t *)buf)[u] = data_bno;
		write_block(vol, ib, buf);
		free(buf);
		return;
	}
	u -= n;

	if (u < (uint64_t)n * n) {
		ensure_indirect_root(vol, inode, BRKFS_DOUBLE_INDIRECT_BLOCK);
		uint32_t dib = inode->i_block[BRKFS_DOUBLE_INDIRECT_BLOCK];
		uint32_t r1 = (uint32_t)(u / n);
		uint32_t r2 = (uint32_t)(u % n);
		uint8_t *b0 = xmalloc(vol->bs);
		uint8_t *b1 = xmalloc(vol->bs);

		read_block(vol, dib, b0);
		uint32_t *p0 = (uint32_t *)b0;
		uint32_t l1 = p0[r1];

		if (l1 == 0) {
			l1 = alloc_data(vol);
			memset(b1, 0, vol->bs);
			write_block(vol, l1, b1);
			p0[r1] = l1;
			write_block(vol, dib, b0);
		}
		read_block(vol, l1, b1);
		((uint32_t *)b1)[r2] = data_bno;
		write_block(vol, l1, b1);
		free(b0);
		free(b1);
		return;
	}
	u -= (uint64_t)n * n;

	if (u < (uint64_t)n * n * n) {
		ensure_indirect_root(vol, inode, BRKFS_TRIPLE_INDIRECT_BLOCK);
		uint32_t tib = inode->i_block[BRKFS_TRIPLE_INDIRECT_BLOCK];
		uint32_t r1 = (uint32_t)(u / ((uint64_t)n * n));
		uint32_t r2 = (uint32_t)((u / n) % n);
		uint32_t r3 = (uint32_t)(u % n);
		uint8_t *b0 = xmalloc(vol->bs);
		uint8_t *b1 = xmalloc(vol->bs);
		uint8_t *b2 = xmalloc(vol->bs);

		read_block(vol, tib, b0);
		uint32_t *p0 = (uint32_t *)b0;
		uint32_t l1 = p0[r1];

		if (l1 == 0) {
			l1 = alloc_data(vol);
			memset(b1, 0, vol->bs);
			write_block(vol, l1, b1);
			p0[r1] = l1;
			write_block(vol, tib, b0);
		}
		read_block(vol, l1, b1);
		uint32_t *p1 = (uint32_t *)b1;
		uint32_t l2 = p1[r2];

		if (l2 == 0) {
			l2 = alloc_data(vol);
			memset(b2, 0, vol->bs);
			write_block(vol, l2, b2);
			p1[r2] = l2;
			write_block(vol, l1, b1);
		}
		read_block(vol, l2, b2);
		((uint32_t *)b2)[r3] = data_bno;
		write_block(vol, l2, b2);
		free(b0);
		free(b1);
		free(b2);
		return;
	}

	die_prog("internal: inode_assign_bno past triple");
}

uint32_t dir_lookup(struct brkfs_volume *vol, uint32_t dir_ino,
		    const char *name)
{
	struct brkfs_inode inode = { .i_ino = dir_ino };
	size_t name_len = strlen(name);

	read_inode(vol, &inode);

	if (inode.i_size == 0)
		return 0;

	uint64_t nblks = (uint64_t)inode.i_size / vol->bs;
	uint8_t *buf = xmalloc(vol->bs);

	for (uint64_t bi = 0; bi < nblks; bi++) {
		uint32_t bno = inode_lookup_bno(vol, &inode, bi);

		if (bno == 0)
			continue;
		read_block(vol, bno, buf);
		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)buf;
		struct brkfs_dir_entry *end =
			(struct brkfs_dir_entry *)(buf + vol->bs);
		uint32_t el = vol->bs;

		for (; (uint8_t *)e < (uint8_t *)end;
		     e = (struct brkfs_dir_entry *)((uint8_t *)e + el)) {
			el = e->entry_len;
			if (el < BRKFS_DIR_ENTRY_MIN_LEN)
				continue;
			if (e->inode == 0)
				continue;
			if (e->name_len == name_len &&
			    memcmp(e->name, name, name_len) == 0) {
				uint32_t found = e->inode;
				free(buf);
				return found;
			}
		}
	}
	free(buf);
	return 0;
}

void read_inode(struct brkfs_volume *vol, struct brkfs_inode *inode)
{
	uint32_t ino = inode->i_ino;
	if (ino < 1 || ino > vol->inodes)
		die_prog("inode number out of range: %u, [1, %u)", ino,
			 vol->inodes);
	uint32_t i_per_block = vol->bs / vol->is;
	uint32_t bno = (ino - 1) / i_per_block + vol->itable_bno;
	uint8_t *block = xmalloc(vol->bs);
	read_block(vol, bno, block);
	uint32_t i = (ino - 1) % i_per_block;
	memcpy(inode, block + i * vol->is, sizeof(*inode));
	free(block);
}

void write_inode(struct brkfs_volume *vol, struct brkfs_inode *inode)
{
	uint32_t ino = inode->i_ino;
	if (ino < 1 || ino > vol->inodes)
		die_prog("inode number out of range: %u, [1, %u)", ino,
			 vol->inodes);
	uint32_t i_per_block = vol->bs / vol->is;
	uint32_t bno = (ino - 1) / i_per_block + vol->itable_bno;
	uint8_t *block = xmalloc(vol->bs);
	read_block(vol, bno, block);
	uint32_t i = (ino - 1) % i_per_block;
	uint8_t *slot = block + i * vol->is;
	memset(slot, 0, vol->is);
	memcpy(slot, inode, sizeof(*inode));
	write_block(vol, bno, block);
	free(block);
}

uint32_t alloc_dir_data(struct brkfs_volume *vol, uint32_t dir_ino, bool first)
{
	uint32_t bno = alloc_data(vol);
	void *buf = xmalloc(vol->bs);

	memset(buf, 0, vol->bs);
	struct brkfs_dir_entry *e = buf;
	if (first) {
		e->inode = dir_ino;
		e->entry_len = 12;
		e->name_len = 1;
		e->file_type = DT_DIR;
		e->name[0] = '.';
		e = (struct brkfs_dir_entry *)((uint8_t *)e + 12);
		e->inode = dir_ino;
		e->entry_len = vol->bs - 12;
		e->name_len = 2;
		e->file_type = DT_DIR;
		e->name[0] = '.';
		e->name[1] = '.';
	} else {
		e->inode = 0;
		e->entry_len = vol->bs;
		e->name_len = 0;
		e->file_type = DT_UNKNOWN;
		e->name[0] = '\0';
	}

	write_block(vol, bno, buf);
	free(buf);
	return bno;
}

void alloc_dir_inode(struct brkfs_volume *vol, struct brkfs_inode *inode,
		     uint32_t mode, uint32_t flags)
{
	mode |= S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
	alloc_inode(vol, inode, mode, 0, flags);
	uint32_t dir_ino = inode->i_ino;
	uint32_t dir_bno = alloc_dir_data(vol, dir_ino, true);
	inode->i_nlink = 2;
	inode->i_size = vol->bs; /* directory always takes up one block */
	inode->i_block[0] = dir_bno;
	write_inode(vol, inode);
}

static bool add_entry_to_block(struct brkfs_volume *vol,
			       struct brkfs_dir_entry *e, const char *name,
			       uint8_t name_len, uint8_t type, uint32_t ino)
{
	uint32_t min_len = (8 + name_len + 3) & ~3;
	struct brkfs_dir_entry *end, *new_e;

	end = (struct brkfs_dir_entry *)((uint8_t *)e + vol->bs);
	uint32_t el = vol->bs;

	for (; e < end; e = (struct brkfs_dir_entry *)((uint8_t *)e + el)) {
		el = e->entry_len;

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

void dir_add_entry(struct brkfs_volume *vol, uint32_t dir_ino, const char *name,
		   uint8_t type, uint32_t ino)
{
	void *buf = NULL;
	struct brkfs_inode inode = { .i_ino = dir_ino };
	size_t name_len = strlen(name);
	if (name_len > BRKFS_NAME_LEN)
		die_prog("name too long: %s", name);

	read_inode(vol, &inode);

	buf = xmalloc(vol->bs);

	if (inode.i_size > 0) {
		uint64_t nblks = (uint64_t)inode.i_size / vol->bs;

		for (uint64_t bi = 0; bi < nblks; bi++) {
			uint32_t bno = inode_lookup_bno(vol, &inode, bi);

			if (bno == 0)
				continue;
			read_block(vol, bno, buf);
			if (add_entry_to_block(vol, buf, name, name_len, type,
					       ino)) {
				write_block(vol, bno, buf);
				free(buf);
				return;
			}
		}
	}

	uint64_t newi = inode.i_size > 0 ? (uint64_t)inode.i_size / vol->bs : 0;
	uint32_t bno = alloc_data(vol);

	inode_assign_bno(vol, &inode, newi, bno);
	memset(buf, 0, vol->bs);
	struct brkfs_dir_entry *e = buf;

	e->inode = 0;
	e->entry_len = vol->bs;
	e->name_len = 0;
	e->file_type = DT_UNKNOWN;
	e->name[0] = '\0';
	if (!add_entry_to_block(vol, buf, name, name_len, type, ino))
		die_prog("directory entry failed (internal error)");
	inode.i_size += vol->bs;
	write_block(vol, bno, buf);
	free(buf);
	write_inode(vol, &inode);
}

static void print_row(const char *region, uint32_t start_blk, uint32_t nblk)
{
	uint32_t end_blk = nblk > 0 ? start_blk + nblk - 1 : start_blk;

	printf("  %-26s %10" PRIu32 " %10" PRIu32 " %10" PRIu32 "\n", region,
	       start_blk, nblk, end_blk);
}

void print_layout(struct brkfs_volume *vol)
{
	uint64_t nbytes = (uint64_t)vol->blocks * vol->bs;
	uint32_t last = vol->blocks > 0 ? vol->blocks - 1 : 0;

	printf("\n");
	printf("brkfs layout\n");
	printf("------------\n");
	printf("  %-26s %10" PRIu32 " bytes\n", "Block size", vol->bs);
	printf("  %-26s %10" PRIu32 " bytes\n", "Inode size", vol->is);
	printf("  %-26s %10" PRIu32 "\n", "Total blocks", vol->blocks);
	printf("  %-26s %10" PRIu32 "\n", "Total inodes", vol->inodes);
	printf("  %-26s %10" PRIu64 " bytes\n", "Volume size", nbytes);
	printf("\n");
	printf("  %-26s %10s %10s %10s\n", "Region", "Start", "Blocks", "End");
	printf("  %-26s %10s %10s %10s\n", "--------------------------",
	       "----------", "----------", "----------");

	print_row("Reserved (boot + super)", 0, vol->ibmap_bno);
	print_row("Inode bitmap", vol->ibmap_bno, vol->ibmap_blocks);
	print_row("Data block bitmap", vol->dbmap_bno, vol->dbmap_blocks);
	print_row("Inode table", vol->itable_bno, vol->itable_blocks);
	print_row("Data (file blocks)", vol->dfirst_bno, vol->dblocks);

	printf("\n");
	printf("  Last block number: %" PRIu32 "\n\n", last);
}
