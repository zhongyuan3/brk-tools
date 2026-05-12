#include "brkfs.h"
#include "common.h"
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BRKFS_MIN_INODE_SIZE 128
#define BRKFS_MAX_INODE_SIZE 256

static_assert(sizeof(struct brkfs_super_block) == BRKFS_SUPER_SIZE,
	      "brkfs_super_block must match BRKFS_SUPER_SIZE");
static_assert(sizeof(struct brkfs_inode) <= BRKFS_MAX_INODE_SIZE,
	      "brkfs_inode larger than BRKFS_MAX_INODE_SIZE");

#define MIN_BLOCK_SIZE BRKFS_SUPER_SIZE
#define MAX_BLOCK_SIZE 4096

#define DEFAULT_BLOCK_SIZE BRKFS_SUPER_SIZE
#define DEFAULT_INODE_SIZE BRKFS_MIN_INODE_SIZE
#define DEFAULT_INODES_COUNT 256

#define MKFS_PROG "mkfs.brkfs"

struct mkfs_args {
	const char *image;
	uint32_t bs;
	uint32_t is;
	uint32_t blocks;
	uint32_t inodes;
	int imgfd;
	bool layout;
};

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: " MKFS_PROG " [options] <image>\n\n"
		"Options:\n"
		"\t-bs <size>      Block size (at least %d bytes, must be a multiple of %d, maximum %d bytes)\n"
		"\t--count <count> Blocks count\n"
		"\t-I <count>      Inodes count\n"
		"\t-i <size>       Inode size (at least %d bytes, must be power of 2, maximum %d bytes)\n"
		"\t--layout        Show the layout after device is formatted and exit (no action is taken)\n"
		"\t--help          Show this help message and exit\n"
		"\t--version       Show version information and exit\n",
		MIN_BLOCK_SIZE, MIN_BLOCK_SIZE, MAX_BLOCK_SIZE,
		BRKFS_MIN_INODE_SIZE, BRKFS_MAX_INODE_SIZE);
	exit(exit_code);
}

static noreturn void print_version(void)
{
	printf(MKFS_PROG " 0.0.1\n");
	exit(EXIT_SUCCESS);
}

static void parse_args(struct mkfs_args *args, int argc, char *argv[])
{
	args->bs = DEFAULT_BLOCK_SIZE;
	args->is = DEFAULT_INODE_SIZE;
	args->blocks = 0;
	args->inodes = DEFAULT_INODES_COUNT;
	args->layout = false;
	args->image = NULL;
	args->imgfd = -1;

	if (argc < 2)
		print_help(EXIT_FAILURE);

	int i = 1;

	for (; i < argc; i++) {
		if (argv[i][0] != '-') {
			break;
		} else if (!strcmp(argv[i], "--help")) {
			print_help(EXIT_SUCCESS);
		} else if (!strcmp(argv[i], "--version")) {
			print_version();
		} else if (!strcmp(argv[i], "-bs")) {
			if (i + 1 >= argc)
				die_argf("missing size for -bs");
			args->bs = strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--count")) {
			if (i + 1 >= argc)
				die_argf("missing count for --count");
			args->blocks = strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-I")) {
			if (i + 1 >= argc)
				die_argf("missing count for -I");
			args->inodes = strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-i")) {
			if (i + 1 >= argc)
				die_argf("missing size for -i");
			args->is = strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--layout")) {
			args->layout = true;
		} else {
			die_argf("unknown option: %s", argv[i]);
		}
	}

	if (i >= argc)
		die_argf("missing image");

	args->image = argv[i++];

	args->imgfd = open(args->image, O_RDWR);
	if (args->imgfd < 0)
		die_errno("open");
}

static void validate_args(struct mkfs_args *args)
{
	if (args->bs < MIN_BLOCK_SIZE || args->bs % MIN_BLOCK_SIZE != 0 ||
	    args->bs > MAX_BLOCK_SIZE)
		die_argf("invalid block size: %u", args->bs);

	if (args->is < BRKFS_MIN_INODE_SIZE ||
	    args->is > BRKFS_MAX_INODE_SIZE || !is_pow_of_two_z(args->is))
		die_argf("invalid inode size: %u", args->is);
	if (args->is < sizeof(struct brkfs_inode))
		die_argf("inode size %u is too small, need at least %zu",
			 args->is, sizeof(struct brkfs_inode));

	struct stat st;
	if (fstat(args->imgfd, &st) < 0)
		die_errno("fstat");

	if (args->blocks == 0)
		args->blocks = st.st_size / args->bs;

	uint64_t totsz = args->bs * args->blocks;
	if (totsz > (uint64_t)st.st_size)
		die_argf("image is too small: need %" PRIu64
			 " bytes, got %" PRIu64 " bytes",
			 (uint64_t)totsz, (uint64_t)st.st_size);
}

static void init_volume(struct mkfs_args *args, struct brkfs_volume *vol)
{
	uint32_t bits_per_block = args->bs * 8;

	/* Blocks [0, s_blocks) overlap the boot area and super (bytes [0, SUPER_END_OFFSET)). */
	uint32_t s_blocks = div_round_up_u32(BRKFS_SUPER_END_OFFSET, args->bs);
	uint32_t i_bmap_bno = s_blocks;
	uint32_t i_bmap_bits = args->inodes;
	uint32_t i_bmap_blocks = div_round_up_u32(i_bmap_bits, bits_per_block);
	uint32_t i_per_block = args->bs / args->is;
	uint32_t i_table_blocks = div_round_up_u32(args->inodes, i_per_block);

	uint32_t d_bmap_bno = i_bmap_bno + i_bmap_blocks;

	uint32_t prefix = s_blocks + i_bmap_blocks + i_table_blocks;
	if (args->blocks <= prefix)
		die_prog("not enough blocks, try increasing -bs or --count");

	/*
	 * remainder = data bitmap blocks + allocatable data blocks, with
	 * d_bmap_blocks = ceil(d_blocks / bits_per_block).
	 */
	uint32_t remainder = args->blocks - prefix;
	uint32_t d_blocks = remainder;
	uint32_t d_bmap_blocks;

	for (;;) {
		d_bmap_blocks = div_round_up_u32(d_blocks, bits_per_block);
		if (d_blocks + d_bmap_blocks <= remainder)
			break;
		d_blocks = remainder - d_bmap_blocks;
	}

	if (d_blocks < 1)
		die_prog("not enough blocks, try increasing -bs or --count");

	uint32_t i_table_bno = d_bmap_bno + d_bmap_blocks;
	uint32_t d_first_bno = i_table_bno + i_table_blocks;
	uint32_t d_bmap_bits = d_blocks;

	vol->imgfd = args->imgfd;
	vol->bs = args->bs;
	vol->is = args->is;
	vol->blocks = args->blocks;
	vol->inodes = args->inodes;
	vol->ibmap_bno = i_bmap_bno;
	vol->ibmap_blocks = i_bmap_blocks;
	vol->ibmap_bits = i_bmap_bits;
	vol->dbmap_bno = d_bmap_bno;
	vol->dbmap_blocks = d_bmap_blocks;
	vol->dbmap_bits = d_bmap_bits;
	vol->itable_bno = i_table_bno;
	vol->itable_blocks = i_table_blocks;
	vol->dfirst_bno = d_first_bno;
	vol->dblocks = d_blocks;
}

static void fill_super(struct brkfs_super_block *sb, struct brkfs_volume *vol)
{
	sb->s_blocksize = vol->bs;
	sb->s_inode_bitmap = vol->ibmap_bno;
	sb->s_inodes_count = vol->inodes;
	sb->s_data_block_bitmap = vol->dbmap_bno;
	sb->s_data_blocks_count = vol->dblocks;
	sb->s_inode_table = vol->itable_bno;
	sb->s_first_data_block = vol->dfirst_bno;
	sb->s_magic = BRKFS_MAGIC;
	sb->s_inode_size = vol->is;
	sb->s_blocks_count = vol->blocks;
}

int main(int argc, char *argv[])
{
	struct mkfs_args args = { 0 };
	struct brkfs_volume vol = { 0 };
	struct brkfs_super_block sb = { 0 };

	set_prog_name(MKFS_PROG);

	parse_args(&args, argc, argv);
	validate_args(&args);
	init_volume(&args, &vol);

	if (args.layout) {
		print_layout(&vol);
		return EXIT_SUCCESS;
	}

	fill_super(&sb, &vol);
	write_super(&vol, &sb);

	struct brkfs_inode inode;
	alloc_dir_inode(&vol, &inode, 0, 0);
	if (inode.i_ino != BRKFS_ROOT_INO)
		die_prog("root inode number is not %u", BRKFS_ROOT_INO);

	return EXIT_SUCCESS;
}
