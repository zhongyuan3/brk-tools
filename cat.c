#include "brkfs.h"
#include "common.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CAT_PROG "cat.brkfs"
#define CAT_PATH_MAX 4096

struct cat_args {
	const char *path;
	const char *img;
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
	a->img = NULL;
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
		die_argf("unknown option: %s", argv[i]);
	}

	if (argc - i != 2)
		die_argf("expected <path> <image> (use --help)");

	a->path = argv[i];
	a->img = argv[i + 1];

	a->imgfd = open(a->img, O_RDONLY);
	if (a->imgfd < 0)
		die_errno("open");
}

static void init_volume(int imgfd, struct brkfs_volume *vol)
{
	struct brkfs_super_block sb;

	read_super(imgfd, &sb);

	if (sb.s_magic != BRKFS_MAGIC)
		die_prog("invalid super block magic number");

	vol->imgfd = imgfd;
	vol->bs = sb.s_blocksize;
	vol->is = sb.s_inode_size;
	vol->blocks = sb.s_blocks_count;
	vol->inodes = sb.s_inodes_count;

	uint32_t bits_per_block = vol->bs * 8;
	uint32_t i_per_block = vol->bs / vol->is;

	vol->ibmap_bno = sb.s_inode_bitmap;
	vol->ibmap_blocks = div_round_up_u32(sb.s_inodes_count, bits_per_block);
	vol->ibmap_bits = sb.s_inodes_count;
	vol->dbmap_bno = sb.s_data_block_bitmap;
	vol->dbmap_blocks =
		div_round_up_u32(sb.s_data_blocks_count, bits_per_block);
	vol->dbmap_bits = sb.s_data_blocks_count;
	vol->itable_bno = sb.s_inode_table;
	vol->itable_blocks = div_round_up_u32(sb.s_inodes_count, i_per_block);
	vol->dfirst_bno = sb.s_first_data_block;
	vol->dblocks = sb.s_data_blocks_count;
}

static bool inode_is_dir(struct brkfs_volume *vol, uint32_t ino)
{
	struct brkfs_inode inode = { .i_ino = ino };

	read_inode(vol, &inode);
	return (inode.i_mode & S_IFMT) == S_IFDIR;
}

static uint32_t resolve_file_ino(struct brkfs_volume *vol, const char *path)
{
	char buf[CAT_PATH_MAX];
	size_t len = strlen(path);

	if (len == 0 || len >= sizeof(buf))
		die_prog("invalid path");

	memcpy(buf, path, len + 1);

	char *s = buf;
	while (*s == '/')
		s++;

	if (*s == '\0')
		die_prog("path must name a file, not a directory");

	char *slash = strrchr(s, '/');
	uint32_t parent = BRKFS_ROOT_INO;
	const char *base;

	if (!slash) {
		base = s;
	} else {
		*slash = '\0';
		base = slash + 1;
		if (*base == '\0')
			die_prog("invalid path (trailing '/')");

		if (*s != '\0') {
			char *save = NULL;
			char *tok = strtok_r(s, "/", &save);

			for (; tok != NULL; tok = strtok_r(NULL, "/", &save)) {
				if (strlen(tok) > BRKFS_NAME_LEN)
					die_prog("path component too long: %s",
						 tok);

				uint32_t ch = dir_lookup(vol, parent, tok);

				if (ch == 0)
					die_prog(
						"no such file or directory: %s",
						path);
				if (!inode_is_dir(vol, ch))
					die_prog("not a directory: %s", path);
				parent = ch;
			}
		}
	}

	if (strlen(base) > BRKFS_NAME_LEN)
		die_prog("name too long");

	uint32_t ino = dir_lookup(vol, parent, base);

	if (ino == 0)
		die_prog("no such file or directory: %s", path);

	struct brkfs_inode fi = { .i_ino = ino };

	read_inode(vol, &fi);
	if ((fi.i_mode & S_IFMT) != S_IFREG)
		die_prog("not a regular file: %s", path);

	return ino;
}

static void cat_file(struct brkfs_volume *vol, uint32_t ino)
{
	struct brkfs_inode inode = { .i_ino = ino };
	uint32_t sz;

	read_inode(vol, &inode);
	sz = inode.i_size;

	if (sz == 0)
		return;

	uint32_t nblks = div_round_up_u32(sz, vol->bs);
	uint8_t *blockbuf = xmalloc(vol->bs);

	for (uint32_t i = 0; i < nblks; i++) {
		uint32_t bno = inode_lookup_bno(vol, &inode, i);

		if (bno == 0)
			die_prog("file data missing at block %" PRIu32, i);

		read_block(vol, bno, blockbuf);

		uint64_t off = (uint64_t)i * vol->bs;
		size_t chunk = vol->bs;

		if (off + chunk > (uint64_t)sz)
			chunk = (size_t)((uint64_t)sz - off);

		if (fwrite(blockbuf, 1, chunk, stdout) != chunk)
			die_prog("write to stdout failed");
	}

	free(blockbuf);

	if (fflush(stdout) != 0)
		die_errno("fflush");
}

int main(int argc, char *argv[])
{
	struct cat_args args = { 0 };
	struct brkfs_volume vol = { 0 };

	set_prog_name(CAT_PROG);
	parse_args(&args, argc, argv);
	init_volume(args.imgfd, &vol);

	uint32_t ino = resolve_file_ino(&vol, args.path);

	cat_file(&vol, ino);

	close(args.imgfd);
	return EXIT_SUCCESS;
}
