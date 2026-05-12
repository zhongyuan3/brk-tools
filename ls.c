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
#define LS_PATH_MAX 4096

struct ls_args {
	const char *path;
	const char *img;
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
	a->img = NULL;
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
					die_argf("unknown option: %s", argv[i]);
			}
			continue;
		}
		die_argf("unknown option: %s", argv[i]);
	}

	if (argc - i == 1) {
		a->img = argv[i];
	} else if (argc - i == 2) {
		a->path = argv[i];
		a->img = argv[i + 1];
	} else {
		die_argf("expected [<path>] <image> (use --help)");
	}

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

static uint32_t resolve_dir_path(struct brkfs_volume *vol, const char *path)
{
	char buf[LS_PATH_MAX];
	size_t len = strlen(path);

	if (len >= sizeof(buf))
		die_prog("path too long");

	if (len == 0)
		return BRKFS_ROOT_INO;

	memcpy(buf, path, len + 1);

	char *s = buf;
	while (*s == '/')
		s++;

	if (*s == '\0')
		return BRKFS_ROOT_INO;

	char *save = NULL;
	char *tok = strtok_r(s, "/", &save);
	uint32_t cur = BRKFS_ROOT_INO;

	for (; tok != NULL; tok = strtok_r(NULL, "/", &save)) {
		if (strlen(tok) > BRKFS_NAME_LEN)
			die_prog("path component too long: %s", tok);

		uint32_t ch = dir_lookup(vol, cur, tok);

		if (ch == 0)
			die_prog("no such file or directory: %s", path);
		if (!inode_is_dir(vol, ch))
			die_prog("not a directory: %s", path);
		cur = ch;
	}

	return cur;
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

static void list_dir(struct brkfs_volume *vol, uint32_t dir_ino, bool long_fmt,
		     bool all)
{
	struct brkfs_inode di = { .i_ino = dir_ino };

	read_inode(vol, &di);

	if ((di.i_mode & S_IFMT) != S_IFDIR)
		die_prog("inode %" PRIu32 " is not a directory", dir_ino);

	if (di.i_size == 0) {
		inode_touch_atime(vol, &di);
		if (long_fmt)
			printf("total 0\n");
		return;
	}

	uint64_t nblks = (uint64_t)di.i_size / vol->bs;
	uint8_t *blockbuf = xmalloc(vol->bs);
	struct ls_dent *ents = NULL;
	size_t nent = 0;
	size_t cap = 0;

	for (uint64_t bi = 0; bi < nblks; bi++) {
		uint32_t bno = inode_lookup_bno(vol, &di, bi);

		if (bno == 0)
			continue;

		read_block(vol, bno, blockbuf);
		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)blockbuf;
		struct brkfs_dir_entry *end =
			(struct brkfs_dir_entry *)(blockbuf + vol->bs);
		uint32_t el = vol->bs;

		for (; (uint8_t *)e < (uint8_t *)end;
		     e = (struct brkfs_dir_entry *)((uint8_t *)e + el)) {
			el = e->entry_len;
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

			read_inode(vol, &chi);
			mode_str(mstr, chi.i_mode);
			printf("%8u %s %8u %s\n", d->ino, mstr, chi.i_size,
			       d->name);
		} else {
			printf("%s\n", d->name);
		}
	}

	free(ents);
	inode_touch_atime(vol, &di);
}

int main(int argc, char *argv[])
{
	struct ls_args args = { 0 };
	struct brkfs_volume vol = { 0 };

	set_prog_name(LS_PROG);
	parse_args(&args, argc, argv);
	init_volume(args.imgfd, &vol);

	uint32_t dir_ino = resolve_dir_path(&vol, args.path);

	list_dir(&vol, dir_ino, args.long_fmt, args.all);

	close(args.imgfd);
	return EXIT_SUCCESS;
}
