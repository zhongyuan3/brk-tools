#include "brkfs.h"
#include "common.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LS_PROG "ls.brkfs"

struct ls_args {
	const char *path;
	const char *image;
	bool long_fmt;
	bool all;
	int imgfd;
};

struct ls_dent {
	char name[BRKFS_NAME_LEN + 1];
	uint8_t name_len;
	uint32_t ino;
};

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: " LS_PROG " [options] [<path>] <image>\n\n"
		"List a directory inside a brkfs image.\n\n"
		"If <path> is omitted, lists the root directory (/).\n\n"
		"Options:\n"
		"  -l          Long listing (inode, mode, size, name)\n"
		"  -a          Include '.' and '..'\n"
		"  -la, -al    Same as -l -a\n"
		"      --help  Show this help and exit\n");
	exit(exit_code);
}

static void parse_args(struct ls_args *a, int argc, char *argv[])
{
	a->path = "/";
	a->image = NULL;
	a->long_fmt = false;
	a->all = false;
	a->imgfd = -1;

	if (argc < 2)
		print_help(EXIT_FAILURE);

	int i = 1;
	for (; i < argc; i++) {
		if (argv[i][0] != '-')
			break;
		if (!strcmp(argv[i], "--help")) {
			print_help(EXIT_SUCCESS);
		}
		if (!strcmp(argv[i], "-l")) {
			a->long_fmt = true;
			continue;
		}
		if (!strcmp(argv[i], "-a")) {
			a->all = true;
			continue;
		}
		if (argv[i][1] != '\0' && argv[i][1] != '-') {
			for (const char *p = argv[i] + 1; *p != '\0'; p++) {
				if (*p == 'l')
					a->long_fmt = true;
				else if (*p == 'a')
					a->all = true;
				else
					die_arg("unknown option: %s", argv[i]);
			}
			continue;
		}
		die_arg("unknown option: %s", argv[i]);
	}

	if (argc - i == 1) {
		a->image = argv[i];
	} else if (argc - i == 2) {
		a->path = argv[i];
		a->image = argv[i + 1];
	} else {
		die_arg("expected [<path>] <image> (use --help)");
	}

	a->imgfd = open(a->image, O_RDWR);
	if (a->imgfd < 0)
		die_errno("open");
}

static void mode_str(char out[11], uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		out[0] = 'd';
		break;
	case S_IFREG:
		out[0] = '-';
		break;
	default:
		out[0] = '?';
		break;
	}

	out[1] = (mode & S_IRUSR) ? 'r' : '-';
	out[2] = (mode & S_IWUSR) ? 'w' : '-';
	out[3] = (mode & S_IXUSR) ? 'x' : '-';
	out[4] = (mode & S_IRGRP) ? 'r' : '-';
	out[5] = (mode & S_IWGRP) ? 'w' : '-';
	out[6] = (mode & S_IXGRP) ? 'x' : '-';
	out[7] = (mode & S_IROTH) ? 'r' : '-';
	out[8] = (mode & S_IWOTH) ? 'w' : '-';
	out[9] = (mode & S_IXOTH) ? 'x' : '-';
	out[10] = '\0';
}

static int dent_cmp(const void *ap, const void *bp)
{
	const struct ls_dent *a = ap;
	const struct ls_dent *b = bp;
	size_t na = a->name_len;
	size_t nb = b->name_len;
	size_t n = na < nb ? na : nb;
	int c = memcmp(a->name, b->name, n);

	if (c != 0)
		return c;
	if (na < nb)
		return -1;
	if (na > nb)
		return 1;
	return 0;
}

static void list_dir(struct brkfs_ctx *ctx, uint32_t dir_ino, bool long_fmt,
		     bool all)
{
	struct brkfs_inode di = { .i_ino = dir_ino };

	read_inode(ctx, &di);

	if ((di.i_mode & S_IFMT) != S_IFDIR)
		die_prog("inode %" PRIu32 " is not a directory", dir_ino);

	if (di.i_size == 0) {
		touch_inode_atime(ctx, &di);
		if (long_fmt)
			printf("total 0\n");
		return;
	}

	uint64_t nblocks = (uint64_t)di.i_size / ctx->block_size;
	uint8_t *blockbuf = xmalloc(ctx->block_size);
	struct ls_dent *ents = NULL;
	size_t nent = 0;
	size_t cap = 0;

	for (uint64_t bi = 0; bi < nblocks; bi++) {
		uint32_t bno = lookup_inode_bno(ctx, &di, bi);

		if (bno == 0)
			continue;

		read_block(ctx, bno, blockbuf);
		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)blockbuf;
		struct brkfs_dir_entry *end =
			(struct brkfs_dir_entry *)(blockbuf + ctx->block_size);
		uint32_t el = ctx->block_size;

		for (; (uint8_t *)e < (uint8_t *)end;
		     e = (struct brkfs_dir_entry *)((uint8_t *)e + el)) {
			el = e->entry_len;
			if (el == 0)
				break;
			if (el < BRKFS_DIR_ENTRY_MIN_LEN)
				continue;
			if (e->inode == 0 || e->name_len == 0)
				continue;

			if (!all) {
				if (e->name_len == 1 && e->name[0] == '.')
					continue;
				if (e->name_len == 2 && e->name[0] == '.' &&
				    e->name[1] == '.')
					continue;
			}

			if (nent >= cap) {
				cap = cap ? cap * 2 : 32;
				ents = realloc(ents, cap * sizeof(*ents));
				if (ents == NULL)
					die_errno("realloc");
			}

			struct ls_dent *d = &ents[nent];

			d->name_len = e->name_len;
			memcpy(d->name, e->name, e->name_len);
			d->name[e->name_len] = '\0';
			d->ino = e->inode;
			nent++;
		}
	}

	free(blockbuf);

	qsort(ents, nent, sizeof(*ents), dent_cmp);

	if (long_fmt)
		printf("total %zu\n", nent);

	for (size_t i = 0; i < nent; i++) {
		struct ls_dent *d = &ents[i];

		if (long_fmt) {
			struct brkfs_inode chi = { .i_ino = d->ino };
			char mstr[11];

			read_inode(ctx, &chi);
			mode_str(mstr, chi.i_mode);
			printf("%8u %s %8u %s\n", d->ino, mstr, chi.i_size,
			       d->name);
		} else {
			printf("%s\n", d->name);
		}
	}

	free(ents);
	touch_inode_atime(ctx, &di);
}

int main(int argc, char *argv[])
{
	struct ls_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };
	struct brkfs_path path;
	uint32_t dir_ino;

	set_prog_name(LS_PROG);
	parse_args(&args, argc, argv);
	open_ctx(args.imgfd, &ctx);

	dir_ino = resolve_path(&ctx, args.path, 0, &path);
	if (dir_ino == 0)
		die_prog("no such file or directory: %s", args.path);
	if (!path.is_dir)
		die_prog("not a directory: %s", args.path);

	list_dir(&ctx, dir_ino, args.long_fmt, args.all);

	close(args.imgfd);
	return EXIT_SUCCESS;
}
