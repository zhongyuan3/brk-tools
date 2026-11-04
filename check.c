#include "brkfs.h"
#include "commands.h"
#include "common.h"
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct check_state {
	struct brkfs_ctx *ctx;
	uint8_t *inode_bitmap;
	uint8_t *data_bitmap;
	uint8_t *used_blocks;
	uint32_t *modes;
	uint32_t *nlinks;
	uint32_t *entry_refs;
	uint32_t *dir_children;
	uint32_t *parent_of;
	uint32_t *dotdot_of;
	uint32_t used_features;
	uint32_t errors;
	uint32_t repaired;
	bool tree_valid;
	bool verbose;
	bool repair;
	bool inode_bitmap_dirty;
	bool data_bitmap_dirty;
	bool super_dirty;
};

struct name_list {
	char **names;
	size_t count;
	size_t capacity;
};

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: %s [options] <image>\n\n"
		"Check brkfs inode, block and directory consistency.\n\n"
		"Options:\n"
		"  -v, --verbose   Print per-inode details\n"
		"      --repair    Fix bitmap, nlink and feature inconsistencies\n"
		"                  (run only on an unmounted image)\n"
		"      --help      Show this help and exit\n",
		get_prog_name());
	exit(exit_code);
}

static void *xcalloc(size_t count, size_t size)
{
	void *ptr = calloc(count, size);

	if (ptr == NULL)
		die_errno("calloc");
	return ptr;
}

static bool bitmap_get(const uint8_t *map, uint32_t bit)
{
	return (map[bit >> 3] & (1u << (bit & 7))) != 0;
}

static void bitmap_set(uint8_t *map, uint32_t bit)
{
	map[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void write_bitmap(struct brkfs_ctx *ctx, uint32_t bno, uint32_t blocks,
			 uint32_t nbits, const uint8_t *map)
{
	size_t bytes = div_round_up_u32(nbits, 8);
	uint8_t *block = xmalloc(ctx->block_size);

	for (uint32_t i = 0; i < blocks; i++) {
		size_t done = (size_t)i * ctx->block_size;
		size_t chunk;

		if (done >= bytes)
			break;
		chunk = bytes - done;
		if (chunk > ctx->block_size)
			chunk = ctx->block_size;
		memset(block, 0, ctx->block_size);
		memcpy(block, map + done, chunk);
		write_block(ctx, bno + i, block);
	}

	free(block);
}

static uint8_t *read_bitmap(struct brkfs_ctx *ctx, uint32_t bno,
			    uint32_t blocks, uint32_t nbits)
{
	size_t bytes = div_round_up_u32(nbits, 8);
	uint8_t *map = xcalloc(bytes, 1);
	uint8_t *block = xmalloc(ctx->block_size);

	for (uint32_t i = 0; i < blocks; i++) {
		size_t done = (size_t)i * ctx->block_size;
		size_t chunk;

		if (done >= bytes)
			break;
		read_block(ctx, bno + i, block);
		chunk = bytes - done;
		if (chunk > ctx->block_size)
			chunk = ctx->block_size;
		memcpy(map + done, block, chunk);
	}

	free(block);
	return map;
}

static void check_error(struct check_state *s, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", get_prog_name());
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	s->errors++;
}

static uint8_t mode_to_dt(uint32_t mode)
{
	if (S_ISDIR(mode))
		return DT_DIR;
	if (S_ISREG(mode))
		return DT_REG;
	if (S_ISLNK(mode))
		return DT_LNK;
	if (S_ISCHR(mode))
		return DT_CHR;
	if (S_ISBLK(mode))
		return DT_BLK;
	if (S_ISFIFO(mode))
		return DT_FIFO;
	if (S_ISSOCK(mode))
		return DT_SOCK;
	return DT_UNKNOWN;
}

static bool mode_type_valid(uint32_t mode)
{
	return S_ISDIR(mode) || S_ISREG(mode) || S_ISLNK(mode) ||
	       S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) ||
	       S_ISSOCK(mode);
}

static void fix_inode_nlink(struct check_state *s, uint32_t ino, uint32_t nlink)
{
	struct brkfs_inode inode = { .i_ino = ino };

	read_inode(s->ctx, &inode);
	inode.i_nlink = nlink;
	write_inode(s->ctx, &inode);
}

static void mark_block(struct check_state *s, uint32_t ino, uint32_t bno,
		       const char *what)
{
	uint32_t bit;

	if (bno < s->ctx->first_data_bno ||
	    bno >= s->ctx->first_data_bno + s->ctx->data_block_count) {
		check_error(s,
			    "inode %u: %s block %u is outside the data region",
			    ino, what, bno);
		s->tree_valid = false;
		return;
	}

	bit = bno - s->ctx->first_data_bno;
	if (bitmap_get(s->used_blocks, bit)) {
		check_error(s,
			    "inode %u: block %u is referenced more than once",
			    ino, bno);
		s->tree_valid = false;
		return;
	}
	bitmap_set(s->used_blocks, bit);
}

static void scan_indirect(struct check_state *s, uint32_t ino, uint32_t bno,
			  unsigned level)
{
	struct brkfs_ctx *ctx = s->ctx;
	uint8_t *buf;
	uint32_t ptrs_per_block = ctx->block_size / sizeof(uint32_t);

	if (level >= 2)
		s->used_features |= BRKFS_FEATURE_DOUBLE_INDIRECT;
	if (level == 3)
		s->used_features |= BRKFS_FEATURE_TRIPLE_INDIRECT;

	mark_block(s, ino, bno,
		   level == 1 ? "indirect data" : "indirect pointer");
	if (bno < ctx->first_data_bno ||
	    bno >= ctx->first_data_bno + ctx->data_block_count)
		return;

	buf = xmalloc(ctx->block_size);
	read_block(ctx, bno, buf);

	for (uint32_t i = 0; i < ptrs_per_block; i++) {
		uint32_t child = ((uint32_t *)buf)[i];

		if (child == 0)
			continue;
		if (level == 1)
			mark_block(s, ino, child, "data");
		else
			scan_indirect(s, ino, child, level - 1);
	}

	free(buf);
}

static void scan_inode_blocks(struct check_state *s,
			      const struct brkfs_inode *inode)
{
	struct brkfs_ctx *ctx = s->ctx;
	uint64_t nblocks = div_round_up_u64(inode->i_size, ctx->block_size);
	uint64_t max_blocks = max_inode_file_blocks(ctx);
	uint32_t ino = inode->i_ino;

	if (S_ISDIR(inode->i_mode)) {
		if (inode->i_size % ctx->block_size != 0) {
			check_error(
				s,
				"inode %u: directory size %u is not block-aligned",
				ino, inode->i_size);
			s->tree_valid = false;
		}
		if (nblocks > BRKFS_DIRECT_BLOCKS)
			s->used_features |= BRKFS_FEATURE_DIR_INDIRECT;
	}

	if (nblocks > max_blocks) {
		check_error(s, "inode %u: size %u needs too many blocks", ino,
			    inode->i_size);
		s->tree_valid = false;
		return;
	}

	for (uint32_t i = 0; i < BRKFS_DIRECT_BLOCKS; i++) {
		uint32_t bno = inode->i_block[i];

		if (bno != 0) {
			mark_block(s, ino, bno, "direct");
		} else if (S_ISDIR(inode->i_mode) && i < nblocks) {
			check_error(s,
				    "inode %u: directory block %u is missing",
				    ino, i);
		}
	}

	/*
	 * Blocks beyond i_size are legal: fallocate() can preallocate them.
	 * They still belong to this inode and must be marked in the bitmap.
	 */
	if (inode->i_block[BRKFS_INDIRECT_BLOCK] != 0)
		scan_indirect(s, ino, inode->i_block[BRKFS_INDIRECT_BLOCK], 1);
	if (inode->i_block[BRKFS_DOUBLE_INDIRECT_BLOCK] != 0)
		scan_indirect(s, ino,
			      inode->i_block[BRKFS_DOUBLE_INDIRECT_BLOCK], 2);
	if (inode->i_block[BRKFS_TRIPLE_INDIRECT_BLOCK] != 0)
		scan_indirect(s, ino,
			      inode->i_block[BRKFS_TRIPLE_INDIRECT_BLOCK], 3);
}

static void name_list_add(struct name_list *list, const char *name, size_t len)
{
	if (list->count == list->capacity) {
		list->capacity = list->capacity ? list->capacity * 2 : 32;
		list->names = realloc(list->names,
				      list->capacity * sizeof(*list->names));
		if (list->names == NULL)
			die_errno("realloc");
	}
	list->names[list->count] = strndup(name, len);
	if (list->names[list->count] == NULL)
		die_errno("strndup");
	list->count++;
}

static int name_cmp(const void *ap, const void *bp)
{
	const char *const *a = ap;
	const char *const *b = bp;

	return strcmp(*a, *b);
}

static void check_duplicate_names(struct check_state *s, uint32_t dir_ino,
				  struct name_list *list)
{
	if (list->count < 2)
		return;

	qsort(list->names, list->count, sizeof(*list->names), name_cmp);
	for (size_t i = 1; i < list->count; i++) {
		if (!strcmp(list->names[i - 1], list->names[i]))
			check_error(s, "directory %u: duplicate entry '%s'",
				    dir_ino, list->names[i]);
	}
}

static void free_name_list(struct name_list *list)
{
	for (size_t i = 0; i < list->count; i++)
		free(list->names[i]);
	free(list->names);
}

static void check_dir_entries(struct check_state *s, uint32_t dir_ino,
			      const struct brkfs_inode *dir)
{
	struct brkfs_ctx *ctx = s->ctx;
	uint64_t nblocks = dir->i_size / ctx->block_size;
	struct name_list names = { 0 };
	bool have_dot = false;
	bool have_dotdot = false;
	uint32_t dot_ino = 0;
	uint8_t *block = xmalloc(ctx->block_size);

	for (uint64_t bi = 0; bi < nblocks; bi++) {
		uint32_t bno = lookup_inode_bno(ctx, dir, bi);

		if (bno == 0)
			continue;
		read_block(ctx, bno, block);
		if (!brkfs_dir_checksum_ok(block, ctx->block_size)) {
			check_error(s,
				    "directory %u: block %u has a bad checksum",
				    dir_ino, bno);
			if (s->repair) {
				brkfs_dir_set_checksum(block, ctx->block_size);
				write_block(ctx, bno, block);
				s->repaired++;
			}
		}
		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)block;
		size_t left = ctx->block_size - BRKFS_DIR_CSUM_SIZE;

		while (left >= BRKFS_DIR_ENTRY_MIN_LEN) {
			uint16_t el = e->entry_len;

			if (el < BRKFS_DIR_ENTRY_MIN_LEN || el > left ||
			    (el & 3) != 0) {
				check_error(
					s,
					"directory %u: invalid entry length %u",
					dir_ino, el);
				break;
			}
			if ((size_t)e->name_len + 8 > el) {
				check_error(
					s,
					"directory %u: invalid name length %u",
					dir_ino, e->name_len);
				goto next;
			}
			if (e->inode == 0)
				goto next;

			if (memchr(e->name, '\0', e->name_len) != NULL ||
			    memchr(e->name, '/', e->name_len) != NULL) {
				check_error(
					s,
					"directory %u: invalid name for inode %u",
					dir_ino, e->inode);
				goto next;
			}
			if (e->inode > ctx->inode_count) {
				check_error(
					s,
					"directory %u: inode %u out of range",
					dir_ino, e->inode);
				goto next;
			}
			if (s->modes[e->inode] == 0) {
				check_error(
					s,
					"directory %u: entry '%s' points to free inode %u",
					dir_ino, e->name, e->inode);
				goto next;
			}
			if (e->file_type != mode_to_dt(s->modes[e->inode]))
				check_error(
					s,
					"directory %u: entry '%s' type does not match inode %u",
					dir_ino, e->name, e->inode);

			if (e->name_len == 1 && e->name[0] == '.') {
				if (have_dot)
					check_error(
						s,
						"directory %u: duplicate '.' entry",
						dir_ino);
				have_dot = true;
				dot_ino = e->inode;
				goto next;
			}
			if (e->name_len == 2 && e->name[0] == '.' &&
			    e->name[1] == '.') {
				if (have_dotdot)
					check_error(
						s,
						"directory %u: duplicate '..' entry",
						dir_ino);
				have_dotdot = true;
				s->dotdot_of[dir_ino] = e->inode;
				goto next;
			}

			if (s->entry_refs[e->inode] != UINT32_MAX)
				s->entry_refs[e->inode]++;
			if (S_ISDIR(s->modes[e->inode])) {
				if (s->parent_of[e->inode] != 0 &&
				    s->parent_of[e->inode] != dir_ino)
					check_error(
						s,
						"directory %u: inode %u has multiple parents",
						dir_ino, e->inode);
				s->parent_of[e->inode] = dir_ino;
				if (s->dir_children[dir_ino] != UINT32_MAX)
					s->dir_children[dir_ino]++;
			}
			name_list_add(&names, e->name, e->name_len);
next:
			left -= el;
			e = (struct brkfs_dir_entry *)((uint8_t *)e + el);
		}
	}

	if (!have_dot)
		check_error(s, "directory %u: missing '.' entry", dir_ino);
	else if (dot_ino != dir_ino)
		check_error(s, "directory %u: '.' points to inode %u", dir_ino,
			    dot_ino);
	if (!have_dotdot)
		check_error(s, "directory %u: missing '..' entry", dir_ino);

	check_duplicate_names(s, dir_ino, &names);
	free_name_list(&names);
	free(block);
}

static void check_links(struct check_state *s)
{
	struct brkfs_ctx *ctx = s->ctx;

	for (uint32_t ino = 1; ino <= ctx->inode_count; ino++) {
		if (s->modes[ino] == 0)
			continue;

		if (S_ISDIR(s->modes[ino])) {
			uint32_t expected = 2 + s->dir_children[ino];
			uint32_t parent = ino == BRKFS_ROOT_INO ?
						  BRKFS_ROOT_INO :
						  s->parent_of[ino];

			if (s->nlinks[ino] != expected) {
				check_error(
					s,
					"directory %u: nlink %u, expected %u",
					ino, s->nlinks[ino], expected);
				if (s->repair) {
					fix_inode_nlink(s, ino, expected);
					s->repaired++;
				}
			}
			if (ino != BRKFS_ROOT_INO && parent == 0)
				check_error(s,
					    "directory %u: no parent directory",
					    ino);
			if (s->dotdot_of[ino] != parent)
				check_error(
					s,
					"directory %u: '..' points to inode %u, expected %u",
					ino, s->dotdot_of[ino], parent);
			if (ino != BRKFS_ROOT_INO && s->entry_refs[ino] != 1)
				check_error(
					s,
					"directory %u: referenced by %u entries",
					ino, s->entry_refs[ino]);
		} else if (s->nlinks[ino] != s->entry_refs[ino]) {
			check_error(s, "inode %u: nlink %u, entries %u", ino,
				    s->nlinks[ino], s->entry_refs[ino]);
			if (s->repair && s->entry_refs[ino] > 0) {
				fix_inode_nlink(s, ino, s->entry_refs[ino]);
				s->repaired++;
			}
		}
	}
}

static void check_bitmaps(struct check_state *s)
{
	struct brkfs_ctx *ctx = s->ctx;
	uint32_t used_inodes = 0;
	uint32_t used_blocks = 0;
	uint32_t missing_features = s->used_features & ~ctx->features;

	for (uint32_t i = 0; i < ctx->data_block_count; i++) {
		bool disk = bitmap_get(s->data_bitmap, i);
		bool used = bitmap_get(s->used_blocks, i);

		if (used)
			used_blocks++;
		if (disk != used) {
			check_error(
				s,
				"data block %u: bitmap says %s, references say %s",
				ctx->first_data_bno + i,
				disk ? "allocated" : "free",
				used ? "used" : "unused");
			if (s->repair) {
				s->data_bitmap_dirty = true;
				s->repaired++;
			}
		}
	}

	if (s->repair) {
		for (uint32_t ino = 1; ino <= ctx->inode_count; ino++)
			if (s->modes[ino] != 0)
				used_inodes++;
	} else {
		for (uint32_t i = 0; i < ctx->inode_count; i++)
			if (bitmap_get(s->inode_bitmap, i))
				used_inodes++;
	}

	uint32_t free_blocks = ctx->data_block_count - used_blocks;
	uint32_t free_inodes = ctx->inode_count - used_inodes;

	if (ctx->free_blocks != free_blocks) {
		check_error(s, "super block free block count %u, expected %u",
			    ctx->free_blocks, free_blocks);
		if (s->repair) {
			ctx->free_blocks = free_blocks;
			s->super_dirty = true;
			s->repaired++;
		}
	}
	if (ctx->free_inodes != free_inodes) {
		check_error(s, "super block free inode count %u, expected %u",
			    ctx->free_inodes, free_inodes);
		if (s->repair) {
			ctx->free_inodes = free_inodes;
			s->super_dirty = true;
			s->repaired++;
		}
	}

	if (missing_features != 0) {
		check_error(
			s,
			"super block is missing feature bits 0x%08x used by inodes",
			missing_features);
		if (s->repair) {
			ctx->features |= s->used_features;
			s->super_dirty = true;
			s->repaired++;
		}
	}

	if (ctx->orphan != 0) {
		check_error(s, "super block orphan list head %u", ctx->orphan);
		if (s->repair) {
			ctx->orphan = 0;
			s->super_dirty = true;
			s->repaired++;
		}
	}

	if (ctx->state != BRKFS_STATE_CLEAN)
		fprintf(stderr, "%s: note: file system state is %s\n",
			get_prog_name(),
			ctx->state == BRKFS_STATE_DIRTY ? "dirty" : "error");

	if (s->repair) {
		if (s->inode_bitmap_dirty) {
			size_t bytes = div_round_up_u32(ctx->inode_count, 8);
			uint8_t *map = xcalloc(bytes, 1);

			for (uint32_t ino = 1; ino <= ctx->inode_count; ino++)
				if (s->modes[ino] != 0)
					bitmap_set(map, ino - 1);
			write_bitmap(ctx, ctx->inode_bitmap_bno,
				     ctx->inode_bitmap_blocks, ctx->inode_count,
				     map);
			free(map);
		}

		if (s->tree_valid && s->data_bitmap_dirty)
			write_bitmap(ctx, ctx->data_bitmap_bno,
				     ctx->data_bitmap_blocks,
				     ctx->data_block_count, s->used_blocks);
	}

	uint32_t remaining = s->errors > s->repaired ? s->errors - s->repaired :
						       0;

	if (s->repair && remaining == 0) {
		ctx->state = BRKFS_STATE_CLEAN;
		ctx->lastcheck = (uint32_t)time(NULL);
		ctx->mnt_count = 0;
		ctx->free_blocks = free_blocks;
		ctx->free_inodes = free_inodes;
		s->super_dirty = true;
	}

	if (s->repair && s->super_dirty) {
		struct brkfs_super_block sb = { 0 };

		ctx_to_super(ctx, &sb);
		write_super(ctx, &sb);
		if (fsync(ctx->imgfd) < 0)
			die_errno("fsync");
	}

	printf("brkfs check: %s\n", remaining == 0 ? "OK" : "FAILED");
	printf("  inodes:     %u used, %u free\n", used_inodes,
	       ctx->inode_count - used_inodes);
	printf("  data blocks: %u used, %u free\n", used_blocks,
	       ctx->data_block_count - used_blocks);
	if (s->repair)
		printf("  repairs:    %u fixed, %u remaining\n", s->repaired,
		       remaining);
	printf("  features:   ");
	print_features(ctx->features);
}

static void check_inode_table(struct check_state *s)
{
	struct brkfs_ctx *ctx = s->ctx;
	uint32_t inodes_per_block = ctx->block_size / ctx->inode_size;
	uint8_t *block = xmalloc(ctx->block_size);

	for (uint32_t bi = 0; bi < ctx->inode_table_blocks; bi++) {
		read_block(ctx, ctx->inode_table_bno + bi, block);

		for (uint32_t slot = 0; slot < inodes_per_block; slot++) {
			uint32_t ino = bi * inodes_per_block + slot + 1;
			struct brkfs_inode inode;

			if (ino > ctx->inode_count)
				break;
			memcpy(&inode, block + (size_t)slot * ctx->inode_size,
			       sizeof(inode));

			bool disk_alloc = bitmap_get(s->inode_bitmap, ino - 1);
			bool valid_type = mode_type_valid(inode.i_mode);
			bool recover_ino = disk_alloc && s->repair &&
					   inode.i_ino == 0 &&
					   inode.i_nlink > 0 && valid_type;
			bool ino_ok = inode.i_ino == ino || recover_ino;
			bool looks_alloc = ino_ok && inode.i_nlink > 0 &&
					   valid_type;

			if (recover_ino) {
				struct brkfs_inode fixed = inode;

				check_error(
					s,
					"inode %u: missing i_ino (recovering)",
					ino);
				fixed.i_ino = ino;
				write_inode(ctx, &fixed);
				inode.i_ino = ino;
				brkfs_inode_set_checksum(&inode);
				s->repaired++;
				s->inode_bitmap_dirty = true;
			}

			if ((disk_alloc || looks_alloc) &&
			    !brkfs_inode_checksum_ok(&inode)) {
				check_error(s, "inode %u: bad checksum", ino);
				if (s->repair && ino_ok) {
					write_inode(ctx, &inode);
					s->repaired++;
				}
			}

			if (!disk_alloc && looks_alloc) {
				check_error(
					s,
					"inode %u: allocated in table but free in bitmap",
					ino);
				if (s->repair) {
					s->inode_bitmap_dirty = true;
					s->repaired++;
				} else {
					continue;
				}
			}
			if (!disk_alloc && !looks_alloc)
				continue;
			if (disk_alloc && !looks_alloc) {
				check_error(
					s,
					"inode %u: allocated in bitmap but invalid in table",
					ino);
				if (s->repair) {
					s->inode_bitmap_dirty = true;
					s->repaired++;
				}
				continue;
			}

			s->modes[ino] = inode.i_mode;
			s->nlinks[ino] = inode.i_nlink;

			if (inode.i_mode & ~(S_IFMT | 07777))
				check_error(
					s, "inode %u: invalid mode bits 0x%08x",
					ino, inode.i_mode);
			if (!valid_type)
				check_error(s, "inode %u: unknown file type",
					    ino);

			if (s->verbose)
				printf("  inode %u: mode 0x%08x size %u nlink %u\n",
				       ino, inode.i_mode, inode.i_size,
				       inode.i_nlink);

			scan_inode_blocks(s, &inode);
		}
	}

	free(block);
}

int cmd_check(int argc, char *argv[])
{
	struct check_state s = { 0 };
	struct brkfs_ctx ctx = { 0 };
	int imgfd;
	const char *image = NULL;
	int i = 1;

	if (argc < 2)
		print_help(EXIT_FAILURE);

	for (; i < argc; i++) {
		if (argv[i][0] != '-')
			break;
		if (!strcmp(argv[i], "--help"))
			print_help(EXIT_SUCCESS);
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			s.verbose = true;
			continue;
		}
		if (!strcmp(argv[i], "--repair")) {
			s.repair = true;
			continue;
		}
		die_arg("unknown option: %s", argv[i]);
	}

	if (argc - i != 1)
		die_arg("expected <image> (use --help)");
	image = argv[i];

	imgfd = open(image, s.repair ? O_RDWR : O_RDONLY);
	if (imgfd < 0)
		die_errno("open");

	open_ctx(imgfd, &ctx);

	s.ctx = &ctx;
	s.tree_valid = true;
	s.inode_bitmap = read_bitmap(&ctx, ctx.inode_bitmap_bno,
				     ctx.inode_bitmap_blocks, ctx.inode_count);
	s.data_bitmap = read_bitmap(&ctx, ctx.data_bitmap_bno,
				    ctx.data_bitmap_blocks,
				    ctx.data_block_count);
	s.used_blocks = xcalloc(div_round_up_u32(ctx.data_block_count, 8), 1);
	s.modes = xcalloc(ctx.inode_count + 1, sizeof(*s.modes));
	s.nlinks = xcalloc(ctx.inode_count + 1, sizeof(*s.nlinks));
	s.entry_refs = xcalloc(ctx.inode_count + 1, sizeof(*s.entry_refs));
	s.dir_children = xcalloc(ctx.inode_count + 1, sizeof(*s.dir_children));
	s.parent_of = xcalloc(ctx.inode_count + 1, sizeof(*s.parent_of));
	s.dotdot_of = xcalloc(ctx.inode_count + 1, sizeof(*s.dotdot_of));

	check_inode_table(&s);

	if (s.modes[BRKFS_ROOT_INO] == 0)
		check_error(&s, "root inode %u is not allocated",
			    BRKFS_ROOT_INO);
	else if (!S_ISDIR(s.modes[BRKFS_ROOT_INO]))
		check_error(&s, "root inode %u is not a directory",
			    BRKFS_ROOT_INO);
	else
		s.parent_of[BRKFS_ROOT_INO] = BRKFS_ROOT_INO;

	if (s.tree_valid) {
		for (uint32_t ino = 1; ino <= ctx.inode_count; ino++) {
			struct brkfs_inode inode;

			if (s.modes[ino] == 0 || !S_ISDIR(s.modes[ino]))
				continue;
			inode.i_ino = ino;
			read_inode(&ctx, &inode);
			if ((inode.i_mode & S_IFMT) == S_IFDIR)
				check_dir_entries(&s, ino, &inode);
		}
		check_links(&s);
		check_bitmaps(&s);
	} else {
		uint32_t remaining;

		check_error(
			&s,
			"skipping directory and bitmap checks due to invalid block references");
		remaining = s.errors > s.repaired ? s.errors - s.repaired : 0;
		printf("brkfs check: %s\n", remaining == 0 ? "OK" : "FAILED");
		if (s.repair)
			printf("  repairs:    %u fixed, %u remaining\n",
			       s.repaired, remaining);
	}

	free(s.inode_bitmap);
	free(s.data_bitmap);
	free(s.used_blocks);
	free(s.modes);
	free(s.nlinks);
	free(s.entry_refs);
	free(s.dir_children);
	free(s.parent_of);
	free(s.dotdot_of);
	if (s.repair && s.repaired > 0 && fsync(imgfd) < 0)
		die_errno("fsync");
	close(imgfd);

	return s.errors > s.repaired ? EXIT_FAILURE : EXIT_SUCCESS;
}
