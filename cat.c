#include "brkfs.h"
#include "common.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>

#define CAT_PROG "cat.brkfs"

struct cat_args {
	const char *path;
	const char *image;
	int imgfd;
};

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: " CAT_PROG " [options] <path> <image>\n\n"
		"Print the contents of a regular file inside a brkfs image "
		"to standard output.\n\n"
		"Options:\n"
		"      --help  Show this help and exit\n");
	exit(exit_code);
}

static void parse_args(struct cat_args *a, int argc, char *argv[])
{
	a->path = NULL;
	a->image = NULL;
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
		die_arg("unknown option: %s", argv[i]);
	}

	if (argc - i != 2)
		die_arg("expected <path> <image> (use --help)");

	a->path = argv[i];
	a->image = argv[i + 1];

	a->imgfd = open(a->image, O_RDWR);
	if (a->imgfd < 0)
		die_errno("open");
}

static void cat_file(struct brkfs_ctx *ctx, uint32_t ino)
{
	struct brkfs_inode inode = { .i_ino = ino };
	uint32_t sz;

	read_inode(ctx, &inode);
	sz = inode.i_size;

	if (sz == 0) {
		touch_inode_atime(ctx, &inode);
		return;
	}

	uint32_t nblocks = div_round_up_u32(sz, ctx->block_size);
	uint8_t *blockbuf = xmalloc(ctx->block_size);

	for (uint32_t i = 0; i < nblocks; i++) {
		uint32_t bno = lookup_inode_bno(ctx, &inode, i);

		if (bno == 0)
			die_prog("file data missing at block %" PRIu32, i);

		read_block(ctx, bno, blockbuf);

		uint64_t off = (uint64_t)i * ctx->block_size;
		size_t chunk = ctx->block_size;

		if (off + chunk > (uint64_t)sz)
			chunk = (size_t)((uint64_t)sz - off);

		if (fwrite(blockbuf, 1, chunk, stdout) != chunk)
			die_prog("write to stdout failed");
	}

	free(blockbuf);
	touch_inode_atime(ctx, &inode);

	if (fflush(stdout) != 0)
		die_errno("fflush");
}

int main(int argc, char *argv[])
{
	struct cat_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };
	struct brkfs_path path;
	uint32_t ino;

	set_prog_name(CAT_PROG);
	parse_args(&args, argc, argv);
	open_ctx(args.imgfd, &ctx);

	ino = resolve_path(&ctx, args.path, 0, &path);
	if (ino == 0)
		die_prog("no such file or directory: %s", args.path);
	if (path.is_dir)
		die_prog("not a regular file: %s", args.path);

	cat_file(&ctx, ino);

	close(args.imgfd);
	return EXIT_SUCCESS;
}
