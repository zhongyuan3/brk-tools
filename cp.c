#include "brkfs.h"
#include "commands.h"
#include "common.h"
#include <dirent.h>
#include <errno.h>
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

struct cp_args {
	const char *src;
	const char *dst;
	const char *image;
	bool recursive;
	bool parents;
	int imgfd;
};

struct host_entry {
	char *name;
};

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: %s [options] <source> <destination> <image>\n\n"
		"Copy a host file or directory into a brkfs image.\n\n"
		"Options:\n"
		"  -r, --recursive   Copy directories recursively\n"
		"  -p, --parents     Create missing destination parent directories\n"
		"      --help        Show this help and exit\n",
		get_prog_name());
	exit(exit_code);
}

static void parse_args(struct cp_args *a, int argc, char *argv[])
{
	a->src = NULL;
	a->dst = NULL;
	a->image = NULL;
	a->recursive = false;
	a->parents = false;
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
		if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--recursive")) {
			a->recursive = true;
			continue;
		}
		if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--parents")) {
			a->parents = true;
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

static void read_exact(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t r = read(fd, p, len);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			die_errno("read");
		}
		if (r == 0)
			die_prog("short read from source file");
		p += r;
		len -= (size_t)r;
	}
}

static char *join_path(const char *dir, const char *name)
{
	size_t dlen = strlen(dir);
	size_t nlen = strlen(name);
	char *path = xmalloc(dlen + 1 + nlen + 1);

	memcpy(path, dir, dlen);
	path[dlen] = '/';
	memcpy(path + dlen + 1, name, nlen + 1);
	return path;
}

static void copy_host_file(struct brkfs_ctx *ctx, int hostfd,
			   uint32_t parent_ino, const char *name,
			   const struct stat *st)
{
	uint64_t sz = (uint64_t)st->st_size;
	uint64_t max = max_inode_file_bytes(ctx);
	uint64_t nblocks;
	struct brkfs_inode inode = { 0 };

	if (sz > max)
		die_prog("file too large (max %" PRIu64 " bytes)", max);

	if (lookup_dir_entry(ctx, parent_ino, name) != 0)
		die_prog("destination already exists: %s", name);

	alloc_inode(ctx, &inode, S_IFREG | (st->st_mode & 07777), 0, 0);

	nblocks = sz > 0 ? div_round_up_u64(sz, ctx->block_size) : 0;

	for (uint64_t i = 0; i < nblocks; i++) {
		uint8_t *blockbuf = xmalloc(ctx->block_size);
		uint64_t off = i * ctx->block_size;
		size_t chunk = ctx->block_size;

		if (off + chunk > sz)
			chunk = (size_t)(sz - off);

		memset(blockbuf, 0, ctx->block_size);
		read_exact(hostfd, blockbuf, chunk);

		uint32_t bno = alloc_data(ctx);

		assign_inode_bno(ctx, &inode, i, bno);
		write_block(ctx, bno, blockbuf);
		free(blockbuf);
	}

	inode.i_size = (uint32_t)sz;
	write_inode(ctx, &inode);
	add_dir_entry(ctx, parent_ino, name, DT_REG, inode.i_ino);
}

static int entry_cmp(const void *ap, const void *bp)
{
	const struct host_entry *a = ap;
	const struct host_entry *b = bp;

	return strcmp(a->name, b->name);
}

static void free_entries(struct host_entry *entries, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(entries[i].name);
	free(entries);
}

static struct host_entry *read_dir_entries(const char *path, size_t *count)
{
	DIR *dir = opendir(path);
	struct host_entry *entries = NULL;
	size_t n = 0;
	size_t cap = 0;
	struct dirent *de;

	if (dir == NULL)
		die_errno("opendir");

	while ((de = readdir(dir)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (strlen(de->d_name) > BRKFS_NAME_LEN)
			die_prog("name too long: %s", de->d_name);

		if (n == cap) {
			cap = cap ? cap * 2 : 32;
			entries = realloc(entries, cap * sizeof(*entries));
			if (entries == NULL)
				die_errno("realloc");
		}
		entries[n].name = strdup(de->d_name);
		if (entries[n].name == NULL)
			die_errno("strdup");
		n++;
	}
	closedir(dir);

	qsort(entries, n, sizeof(*entries), entry_cmp);
	*count = n;
	return entries;
}

static void copy_host_dir(struct brkfs_ctx *ctx, const char *src,
			  uint32_t parent_ino, const char *name,
			  const struct stat *st, bool recursive);

static void copy_dir_children(struct brkfs_ctx *ctx, const char *src,
			      uint32_t dir_ino)
{
	size_t n = 0;
	struct host_entry *entries = read_dir_entries(src, &n);

	for (size_t i = 0; i < n; i++) {
		char *child = join_path(src, entries[i].name);
		struct stat st;

		if (stat(child, &st) < 0)
			die_errno("stat");

		if (S_ISDIR(st.st_mode))
			copy_host_dir(ctx, child, dir_ino, entries[i].name, &st,
				      true);
		else if (S_ISREG(st.st_mode)) {
			int fd = open(child, O_RDONLY);

			if (fd < 0)
				die_errno("open");
			copy_host_file(ctx, fd, dir_ino, entries[i].name, &st);
			close(fd);
		} else {
			die_prog("unsupported source type: %s", child);
		}
		free(child);
	}

	free_entries(entries, n);
}

static void copy_host_dir(struct brkfs_ctx *ctx, const char *src,
			  uint32_t parent_ino, const char *name,
			  const struct stat *st, bool recursive)
{
	struct brkfs_inode inode = { 0 };

	if (!recursive)
		die_prog("source is a directory (use -r or --recursive)");

	if (lookup_dir_entry(ctx, parent_ino, name) != 0)
		die_prog("destination already exists: %s", name);

	alloc_dir_inode(ctx, &inode, st->st_mode & 07777, parent_ino, 0);
	add_dir_entry(ctx, parent_ino, name, DT_DIR, inode.i_ino);
	copy_dir_children(ctx, src, inode.i_ino);
}

int cmd_cp(int argc, char *argv[])
{
	struct cp_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };
	struct brkfs_super_block sb = { 0 };
	struct brkfs_path path;
	struct stat st;
	uint32_t existing;
	unsigned path_flags = 0;

	parse_args(&args, argc, argv);

	if (stat(args.src, &st) < 0)
		die_errno("stat");

	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		die_prog("source is not a regular file or directory");

	if (S_ISDIR(st.st_mode) && !args.recursive)
		die_prog("source is a directory (use -r or --recursive)");

	open_ctx(args.imgfd, &ctx);

	if (args.parents)
		path_flags |= BRKFS_PATH_CREATE_PARENTS;

	existing = resolve_path(&ctx, args.dst, path_flags, &path);

	if (S_ISDIR(st.st_mode)) {
		if (existing != 0) {
			if (!path.is_dir)
				die_prog("destination is not a directory: %s",
					 args.dst);
			copy_dir_children(&ctx, args.src, existing);
		} else {
			if (path.basename[0] == '\0')
				die_prog("invalid destination path");
			copy_host_dir(&ctx, args.src, path.parent_ino,
				      path.basename, &st, true);
		}
	} else {
		int hostfd;

		if (existing != 0)
			die_prog("destination already exists: %s",
				 path.basename);
		if (path.basename[0] == '\0')
			die_prog("invalid destination path");

		hostfd = open(args.src, O_RDONLY);
		if (hostfd < 0)
			die_errno("open");
		copy_host_file(&ctx, hostfd, path.parent_ino, path.basename,
			       &st);
		close(hostfd);
	}

	ctx_to_super(&ctx, &sb);
	write_super(&ctx, &sb);
	if (fsync(ctx.imgfd) < 0)
		die_errno("fsync");

	close(args.imgfd);

	return EXIT_SUCCESS;
}
