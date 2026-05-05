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
#define CP_PATH_MAX 4096

struct cp_args {
	const char *src;
	const char *dst;
	const char *img;
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
	a->img = NULL;
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
		die_argf("unknown option: %s", argv[i]);
	}

	if (argc - i != 3)
		die_argf("expected <source> <destination> <image> "
			 "(use --help)");

	a->src = argv[i];
	a->dst = argv[i + 1];
	a->img = argv[i + 2];

	a->imgfd = open(a->img, O_RDWR);
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

/*
 * Returns parent directory inode number for the last path component,
 * and copies the final component (new file name) into basename[].
 */
static uint32_t resolve_parent(struct brkfs_volume *vol, const char *dst,
			       bool recursive, char *basename)
{
	char buf[CP_PATH_MAX];
	size_t len = strlen(dst);

	if (len == 0 || len >= sizeof(buf))
		die_prog("invalid destination path");

	memcpy(buf, dst, len + 1);

	char *s = buf;
	while (*s == '/')
		s++;

	if (*s == '\0')
		die_prog("empty destination path");

	char *slash = strrchr(s, '/');

	if (!slash) {
		if (strlen(s) > BRKFS_NAME_LEN)
			die_prog("name too long");
		memcpy(basename, s, strlen(s) + 1);
		return BRKFS_ROOT_INO;
	}

	*slash = '\0';
	const char *base = slash + 1;

	if (*base == '\0')
		die_prog("invalid destination path (trailing '/')");

	if (strlen(base) > BRKFS_NAME_LEN)
		die_prog("name too long");

	memcpy(basename, base, strlen(base) + 1);

	if (*s == '\0')
		return BRKFS_ROOT_INO;

	char *save = NULL;
	char *tok = strtok_r(s, "/", &save);
	uint32_t cur = BRKFS_ROOT_INO;

	for (; tok != NULL; tok = strtok_r(NULL, "/", &save)) {
		if (strlen(tok) > BRKFS_NAME_LEN)
			die_prog("path component too long: %s", tok);

		uint32_t ch = dir_lookup(vol, cur, tok);

		if (ch == 0) {
			if (!recursive)
				die_prog("missing directory '%s' "
					 "(use -r or --recursive to create "
					 "parents)",
					 tok);
			struct brkfs_inode newdir;

			alloc_dir_inode(vol, &newdir, 0, 0);
			dir_add_entry(vol, cur, tok, DT_DIR, newdir.i_ino);
			cur = newdir.i_ino;
		} else {
			if (!inode_is_dir(vol, ch))
				die_prog("not a directory: %s", tok);
			cur = ch;
		}
	}

	return cur;
}

static void copy_host_file(struct brkfs_volume *vol, int hostfd,
			   uint32_t parent_ino, const char *name)
{
	struct stat st;

	if (fstat(hostfd, &st) < 0)
		die_errno("fstat");

	if (!S_ISREG(st.st_mode))
		die_prog("source is not a regular file");

	uint64_t sz = (uint64_t)st.st_size;
	uint64_t max = inode_max_file_bytes(vol);

	if (sz > max)
		die_prog("file too large (max %" PRIu64 " bytes)", max);

	if (sz > UINT32_MAX)
		die_prog("file too large");

	if (dir_lookup(vol, parent_ino, name) != 0)
		die_prog("destination already exists: %s", name);

	struct brkfs_inode file = { 0 };

	alloc_inode(vol, &file, S_IFREG | 0644, 0, 0);

	uint32_t nblk = 0;

	if (sz > 0)
		nblk = (uint32_t)((sz + vol->bs - 1) / vol->bs);

	uint64_t off = 0;

	for (uint32_t i = 0; i < nblk; i++) {
		uint8_t *blockbuf = xmalloc(vol->bs);
		size_t left = (size_t)(sz - off);
		size_t chunk = left < vol->bs ? left : vol->bs;

		memset(blockbuf, 0, vol->bs);
		if (chunk > 0) {
			ssize_t rd = read(hostfd, blockbuf, chunk);

			if (rd < 0)
				die_errno("read");
			if ((size_t)rd != chunk)
				die_prog("short read from source file");
		}

		uint32_t bno = alloc_data(vol);

		inode_assign_bno(vol, &file, i, bno);
		write_block(vol, bno, blockbuf);
		free(blockbuf);
		off += chunk;
	}

	file.i_size = (uint32_t)sz;
	write_inode(vol, &file);
	dir_add_entry(vol, parent_ino, name, DT_REG, file.i_ino);
}

int main(int argc, char *argv[])
{
	struct cp_args args = { 0 };
	struct brkfs_volume vol = { 0 };
	char basename[BRKFS_NAME_LEN + 1];
	int hostfd = -1;

	set_prog_name(CP_PROG);
	parse_args(&args, argc, argv);
	init_volume(args.imgfd, &vol);

	hostfd = open(args.src, O_RDONLY);
	if (hostfd < 0)
		die_errno("open");

	uint32_t parent =
		resolve_parent(&vol, args.dst, args.recursive, basename);
	copy_host_file(&vol, hostfd, parent, basename);

	close(hostfd);
	close(args.imgfd);

	return EXIT_SUCCESS;
}
