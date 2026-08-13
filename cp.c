#include "brkfs.h"
#include "common.h"
#include <dirent.h>
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

#define CP_PROG "cp.brkfs"

struct cp_args {
	const char *src;
	const char *dst;
	const char *image;
	bool recursive;
	int imgfd;
};

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: " CP_PROG
		" [options] <source> <destination> <image>\n\n"
		"Copy a single regular file from the host into a brkfs image.\n\n"
		"Options:\n"
		"  -r, --recursive   Create missing parent directories on the image\n"
		"      --help        Show this help and exit\n");
	exit(exit_code);
}

static void parse_args(struct cp_args *a, int argc, char *argv[])
{
	a->src = NULL;
	a->dst = NULL;
	a->image = NULL;
	a->recursive = false;
	a->imgfd = -1;

	if (argc < 2)
		print_help(EXIT_FAILURE);

	int i = 1;
	for (; i < argc; i++) {
		if (argv[i][0] != '-') {
			break;
		}
		if (!strcmp(argv[i], "--help")) {
			print_help(EXIT_SUCCESS);
		}
		if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--recursive")) {
			a->recursive = true;
			continue;
		}
		die_arg("unknown option: %s", argv[i]);
	}

	if (argc - i != 3)
		die_arg("expected <source> <destination> <image> "
			"(use --help)");

	a->src = argv[i];
	a->dst = argv[i + 1];
	a->image = argv[i + 2];

	a->imgfd = open(a->image, O_RDWR);
	if (a->imgfd < 0)
		die_errno("open");
}

static void copy_host_file(struct brkfs_ctx *ctx, int hostfd,
			   uint32_t parent_ino, const char *name)
{
	struct stat st;

	if (fstat(hostfd, &st) < 0)
		die_errno("fstat");

	if (!S_ISREG(st.st_mode))
		die_prog("source is not a regular file");

	uint64_t sz = (uint64_t)st.st_size;
	uint64_t max = max_inode_file_bytes(ctx);

	if (sz > max)
		die_prog("file too large (max %" PRIu64 " bytes)", max);

	if (sz > UINT32_MAX)
		die_prog("file too large");

	struct brkfs_inode inode = { 0 };

	alloc_inode(ctx, &inode, S_IFREG | 0644, 0, 0);

	uint32_t nblocks = 0;

	if (sz > 0)
		nblocks = (uint32_t)((sz + ctx->block_size - 1) /
				     ctx->block_size);

	uint64_t off = 0;

	for (uint32_t i = 0; i < nblocks; i++) {
		uint8_t *blockbuf = xmalloc(ctx->block_size);
		size_t left = (size_t)(sz - off);
		size_t chunk = left < ctx->block_size ? left : ctx->block_size;

		memset(blockbuf, 0, ctx->block_size);
		if (chunk > 0) {
			ssize_t rd = read(hostfd, blockbuf, chunk);

			if (rd < 0)
				die_errno("read");
			if ((size_t)rd != chunk)
				die_prog("short read from source file");
		}

		uint32_t bno = alloc_data(ctx);

		assign_inode_bno(ctx, &inode, i, bno);
		write_block(ctx, bno, blockbuf);
		free(blockbuf);
		off += chunk;
	}

	inode.i_size = (uint32_t)sz;
	write_inode(ctx, &inode);
	add_dir_entry(ctx, parent_ino, name, DT_REG, inode.i_ino);
}

int main(int argc, char *argv[])
{
	struct cp_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };
	struct brkfs_path path;
	int hostfd;
	uint32_t existing;

	set_prog_name(CP_PROG);
	parse_args(&args, argc, argv);
	open_ctx(args.imgfd, &ctx);

	hostfd = open(args.src, O_RDONLY);
	if (hostfd < 0)
		die_errno("open");

	unsigned flags = BRKFS_PATH_HINT_RECURSIVE;

	if (args.recursive)
		flags |= BRKFS_PATH_CREATE_PARENTS;

	existing = resolve_path(&ctx, args.dst, flags, &path);

	if (path.basename[0] == '\0')
		die_prog("invalid destination path");

	if (existing != 0)
		die_prog("destination already exists: %s", path.basename);

	copy_host_file(&ctx, hostfd, path.parent_ino, path.basename);

	close(hostfd);
	close(args.imgfd);

	return EXIT_SUCCESS;
}
