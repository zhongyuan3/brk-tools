#include "brkfs.h"
#include "common.h"
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>

#define MKFS_MIN_INODE_SIZE 128
#define MKFS_MAX_INODE_SIZE 256

static_assert(sizeof(struct brkfs_super_block) == BRKFS_SUPER_SIZE,
	      "brkfs_super_block must match BRKFS_SUPER_SIZE");
static_assert(sizeof(struct brkfs_inode) <= MKFS_MAX_INODE_SIZE,
	      "brkfs_inode larger than MKFS_MAX_INODE_SIZE");

#define MKFS_MIN_BLOCK_SIZE BRKFS_SUPER_SIZE
#define MKFS_MAX_BLOCK_SIZE 4096

#define MKFS_DEFAULT_BLOCK_SIZE BRKFS_SUPER_SIZE
#define MKFS_DEFAULT_INODE_SIZE MKFS_MIN_INODE_SIZE
#define MKFS_DEFAULT_INODES_COUNT 256

#define MKFS_PROG "mkfs.brkfs"

struct mkfs_args {
	const char *image;
	uint32_t block_size;
	uint32_t inode_size;
	uint32_t block_count;
	uint32_t inode_count;
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
		MKFS_MIN_BLOCK_SIZE, MKFS_MIN_BLOCK_SIZE, MKFS_MAX_BLOCK_SIZE,
		MKFS_MIN_INODE_SIZE, MKFS_MAX_INODE_SIZE);
	exit(exit_code);
}

static noreturn void print_version(void)
{
	printf(MKFS_PROG " 0.0.1\n");
	exit(EXIT_SUCCESS);
}

static void parse_args(struct mkfs_args *args, int argc, char *argv[])
{
	args->block_size = MKFS_DEFAULT_BLOCK_SIZE;
	args->inode_size = MKFS_DEFAULT_INODE_SIZE;
	args->block_count = 0;
	args->inode_count = MKFS_DEFAULT_INODES_COUNT;
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
				die_arg("missing size for -bs");
			args->block_size =
				parse_u32_arg(argv[++i], "block size");
		} else if (!strcmp(argv[i], "--count")) {
			if (i + 1 >= argc)
				die_arg("missing count for --count");
			args->block_count =
				parse_u32_arg(argv[++i], "block count");
		} else if (!strcmp(argv[i], "-I")) {
			if (i + 1 >= argc)
				die_arg("missing count for -I");
			args->inode_count =
				parse_u32_arg(argv[++i], "inode count");
		} else if (!strcmp(argv[i], "-i")) {
			if (i + 1 >= argc)
				die_arg("missing size for -i");
			args->inode_size =
				parse_u32_arg(argv[++i], "inode size");
		} else if (!strcmp(argv[i], "--layout")) {
			args->layout = true;
		} else {
			die_arg("unknown option: %s", argv[i]);
		}
	}

	if (i >= argc)
		die_arg("missing image");

	args->image = argv[i++];

	args->imgfd = open(args->image, O_RDWR);
	if (args->imgfd < 0)
		die_errno("open");
}

static void validate_args(struct mkfs_args *args)
{
	if (args->block_size < MKFS_MIN_BLOCK_SIZE ||
	    args->block_size % MKFS_MIN_BLOCK_SIZE != 0 ||
	    args->block_size > MKFS_MAX_BLOCK_SIZE)
		die_arg("invalid block size: %u", args->block_size);

	if (args->inode_size < MKFS_MIN_INODE_SIZE ||
	    args->inode_size > MKFS_MAX_INODE_SIZE ||
	    !is_pow_of_two(args->inode_size))
		die_arg("invalid inode size: %u", args->inode_size);
	if (args->inode_size < sizeof(struct brkfs_inode))
		die_arg("inode size %u is too small, need at least %zu",
			args->inode_size, sizeof(struct brkfs_inode));

	struct stat st;
	if (fstat(args->imgfd, &st) < 0)
		die_errno("fstat");

	if (args->block_count == 0)
		args->block_count = st.st_size / args->block_size;

	uint64_t totsz = args->block_size * args->block_count;
	if (totsz > (uint64_t)st.st_size)
		die_arg("image is too small: need %" PRIu64
			" bytes, got %" PRIu64 " bytes",
			(uint64_t)totsz, (uint64_t)st.st_size);
}

static void init_ctx(struct mkfs_args *args, struct brkfs_ctx *ctx)
{
	uint32_t bits_per_block = args->block_size * 8;

	/* Blocks [0, super_blocks) overlap the boot area and super (bytes [0, SUPER_END_OFFSET)). */
	uint32_t super_blocks =
		div_round_up_u32(BRKFS_SUPER_END_OFFSET, args->block_size);
	uint32_t inode_bitmap_bno = super_blocks;
	uint32_t inode_bitmap_bits = args->inode_count;
	uint32_t inode_bitmap_blocks =
		div_round_up_u32(inode_bitmap_bits, bits_per_block);
	uint32_t inodes_per_block = args->block_size / args->inode_size;
	uint32_t inode_table_blocks =
		div_round_up_u32(args->inode_count, inodes_per_block);

	uint32_t data_bitmap_bno = inode_bitmap_bno + inode_bitmap_blocks;

	uint32_t prefix =
		super_blocks + inode_bitmap_blocks + inode_table_blocks;
	if (args->block_count <= prefix)
		die_prog("not enough blocks, try increasing -bs or --count");

	/*
	 * remainder = data bitmap blocks + allocatable data blocks, with
	 * data_bitmap_blocks = ceil(data_block_count / bits_per_block).
	 */
	uint32_t remainder = args->block_count - prefix;
	uint32_t data_block_count = remainder;
	uint32_t data_bitmap_blocks;

	for (;;) {
		data_bitmap_blocks =
			div_round_up_u32(data_block_count, bits_per_block);
		if (data_block_count + data_bitmap_blocks <= remainder)
			break;
		data_block_count = remainder - data_bitmap_blocks;
	}

	if (data_block_count < 1)
		die_prog("not enough blocks, try increasing -bs or --count");

	uint32_t inode_table_bno = data_bitmap_bno + data_bitmap_blocks;
	uint32_t first_data_bno = inode_table_bno + inode_table_blocks;
	uint32_t data_bitmap_bits = data_block_count;

	ctx->imgfd = args->imgfd;
	ctx->block_size = args->block_size;
	ctx->inode_size = args->inode_size;
	ctx->block_count = args->block_count;
	ctx->inode_count = args->inode_count;
	ctx->inode_bitmap_bno = inode_bitmap_bno;
	ctx->inode_bitmap_blocks = inode_bitmap_blocks;
	ctx->inode_bitmap_bits = inode_bitmap_bits;
	ctx->data_bitmap_bno = data_bitmap_bno;
	ctx->data_bitmap_blocks = data_bitmap_blocks;
	ctx->data_bitmap_bits = data_bitmap_bits;
	ctx->inode_table_bno = inode_table_bno;
	ctx->inode_table_blocks = inode_table_blocks;
	ctx->first_data_bno = first_data_bno;
	ctx->data_block_count = data_block_count;
}

int main(int argc, char *argv[])
{
	struct mkfs_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };
	struct brkfs_super_block sb = { 0 };

	set_prog_name(MKFS_PROG);

	parse_args(&args, argc, argv);
	validate_args(&args);
	init_ctx(&args, &ctx);

	if (args.layout) {
		print_layout(&ctx);
		return EXIT_SUCCESS;
	}

	ctx_to_super(&ctx, &sb);
	write_super(&ctx, &sb);

	struct brkfs_inode inode;
	alloc_dir_inode(&ctx, &inode, 0, 0);
	if (inode.i_ino != BRKFS_ROOT_INO)
		die_prog("root inode number is not %u", BRKFS_ROOT_INO);

	return EXIT_SUCCESS;
}
