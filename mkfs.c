#include "brkfs.h"
#include "commands.h"
#include "common.h"
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MKFS_DEFAULT_BLOCK_SIZE BRKFS_MIN_BLOCK_SIZE
#define MKFS_DEFAULT_INODE_SIZE BRKFS_MIN_INODE_SIZE
#define MKFS_DEFAULT_INODES_COUNT 256

enum tool_mode {
	TOOL_MKFS,
	TOOL_LAYOUT,
};

struct mkfs_args {
	const char *image;
	uint32_t block_size;
	uint32_t inode_size;
	uint32_t block_count;
	uint32_t inode_count;
	char label[64];
	int imgfd;
};

static noreturn void print_help(enum tool_mode mode, int exit_code)
{
	const char *what = mode == TOOL_LAYOUT ?
				   "Show the layout mkfs would create "
				   "without modifying the image." :
				   "Format an image as a brkfs filesystem.";

	fprintf(stderr,
		"Usage: %s [options] <image>\n\n"
		"%s\n\n"
		"Options:\n"
		"\t-bs <size>      Block size (at least %d bytes, must be a multiple of %d, maximum %d bytes)\n"
		"\t--count <count> Blocks count\n"
		"\t-I <count>      Inodes count\n"
		"\t-i <size>       Inode size (at least %d bytes, must be power of 2, maximum %d bytes)\n"
		"\t-L <label>      Volume label (at most %d characters)\n"
		"\t--help          Show this help message and exit\n"
		"\t--version       Show version information and exit\n",
		get_prog_name(), what, BRKFS_MIN_BLOCK_SIZE,
		BRKFS_MIN_BLOCK_SIZE, BRKFS_MAX_BLOCK_SIZE,
		BRKFS_MIN_INODE_SIZE, BRKFS_MAX_INODE_SIZE,
		(int)sizeof(((struct mkfs_args *)0)->label) - 1);
	exit(exit_code);
}

static noreturn void print_version(void)
{
	printf("%s %s\n", get_prog_name(), BRKFS_VERSION);
	exit(EXIT_SUCCESS);
}

static void parse_args(struct mkfs_args *args, int argc, char *argv[],
		       enum tool_mode mode, int open_flags)
{
	args->block_size = MKFS_DEFAULT_BLOCK_SIZE;
	args->inode_size = MKFS_DEFAULT_INODE_SIZE;
	args->block_count = 0;
	args->inode_count = MKFS_DEFAULT_INODES_COUNT;
	args->label[0] = '\0';
	args->image = NULL;
	args->imgfd = -1;

	if (argc < 2)
		print_help(mode, EXIT_FAILURE);

	int i = 1;

	for (; i < argc; i++) {
		if (argv[i][0] != '-') {
			break;
		} else if (!strcmp(argv[i], "--help")) {
			print_help(mode, EXIT_SUCCESS);
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
		} else if (!strcmp(argv[i], "-L") ||
			   !strcmp(argv[i], "--label")) {
			if (i + 1 >= argc)
				die_arg("missing label for -L");
			if (strlen(argv[i + 1]) >= sizeof(args->label))
				die_arg("label too long: %s", argv[i + 1]);
			strcpy(args->label, argv[++i]);
		} else {
			die_arg("unknown option: %s", argv[i]);
		}
	}

	if (i >= argc)
		die_arg("missing image");

	args->image = argv[i++];
	if (i != argc)
		die_arg("unexpected argument: %s", argv[i]);

	args->imgfd = open(args->image, open_flags);
	if (args->imgfd < 0)
		die_errno("open");
}

static void validate_args(struct mkfs_args *args)
{
	if (args->block_size < BRKFS_MIN_BLOCK_SIZE ||
	    args->block_size > BRKFS_MAX_BLOCK_SIZE ||
	    !is_pow_of_two(args->block_size))
		die_arg("invalid block size: %u", args->block_size);

	if (args->inode_size < BRKFS_MIN_INODE_SIZE ||
	    args->inode_size > BRKFS_MAX_INODE_SIZE ||
	    !is_pow_of_two(args->inode_size))
		die_arg("invalid inode size: %u", args->inode_size);
	if (args->inode_size < sizeof(struct brkfs_inode))
		die_arg("inode size %u is too small, need at least %zu",
			args->inode_size, sizeof(struct brkfs_inode));

	if (args->inode_count == 0)
		die_arg("invalid inode count: 0");

	struct stat st;
	if (fstat(args->imgfd, &st) < 0)
		die_errno("fstat");

	if (args->block_count == 0)
		args->block_count = st.st_size / args->block_size;

	uint64_t totsz = (uint64_t)args->block_size * args->block_count;
	if (totsz > (uint64_t)st.st_size)
		die_arg("image is too small: need %" PRIu64
			" bytes, got %" PRIu64 " bytes",
			(uint64_t)totsz, (uint64_t)st.st_size);
}

static void generate_uuid(uint8_t uuid[16])
{
	int fd = open("/dev/urandom", O_RDONLY);
	ssize_t n = -1;

	if (fd >= 0) {
		n = read(fd, uuid, 16);
		close(fd);
	}

	if (n != 16) {
		uint64_t seed = (uint64_t)time(NULL) ^
				((uint64_t)getpid() << 32);

		for (int i = 0; i < 16; i++) {
			seed = seed * 6364136223846793005ull + 1;
			uuid[i] = (uint8_t)(seed >> 56);
		}
	}

	uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40);
	uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);
}

static void init_ctx(struct mkfs_args *args, struct brkfs_ctx *ctx)
{
	uint32_t bits_per_block = args->block_size * 8;
	uint32_t usable_blocks;

	/* The last block is reserved for a backup superblock. */
	if (args->block_count < 2)
		die_prog("not enough blocks, try increasing --count");
	usable_blocks = args->block_count - 1;

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
	if (usable_blocks <= prefix)
		die_prog("not enough blocks, try increasing -bs or --count");

	/*
	 * remainder = data bitmap blocks + allocatable data blocks, with
	 * data_bitmap_blocks = ceil(data_block_count / bits_per_block).
	 */
	uint32_t remainder = usable_blocks - prefix;
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
	ctx->version = BRKFS_FORMAT_VERSION;
	ctx->features = 0;
	ctx->state = BRKFS_STATE_CLEAN;
	ctx->free_blocks = data_block_count;
	ctx->free_inodes = args->inode_count;
	ctx->backup_super_bno = args->block_count - 1;
	ctx->max_mnt_count = 30;
	ctx->mtime = (uint32_t)time(NULL);
	ctx->lastcheck = ctx->mtime;
	generate_uuid(ctx->uuid);
	memcpy(ctx->label, args->label, sizeof(ctx->label));
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

static void clear_metadata(struct brkfs_ctx *ctx)
{
	uint32_t start = ctx->inode_bitmap_bno;
	uint32_t end = ctx->inode_table_bno + ctx->inode_table_blocks;
	uint8_t *zero = xmalloc(ctx->block_size);

	memset(zero, 0, ctx->block_size);
	for (uint32_t bno = start; bno < end; bno++)
		write_block(ctx, bno, zero);
	free(zero);
}

int cmd_mkfs(int argc, char *argv[])
{
	struct mkfs_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };
	struct brkfs_super_block sb = { 0 };

	parse_args(&args, argc, argv, TOOL_MKFS, O_RDWR);
	validate_args(&args);
	init_ctx(&args, &ctx);

	/*
	 * The image may already contain a filesystem (or arbitrary data), so
	 * start from a clean slate; otherwise stale allocation bits and inode
	 * entries leak into the new filesystem.
	 */
	clear_metadata(&ctx);

	struct brkfs_inode inode;
	alloc_dir_inode(&ctx, &inode, 0777, BRKFS_ROOT_INO, 0);
	if (inode.i_ino != BRKFS_ROOT_INO)
		die_prog("root inode number is not %u", BRKFS_ROOT_INO);

	ctx_to_super(&ctx, &sb);
	write_super(&ctx, &sb);

	if (fsync(ctx.imgfd) < 0)
		die_errno("fsync");

	return EXIT_SUCCESS;
}

int cmd_layout(int argc, char *argv[])
{
	struct mkfs_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };

	parse_args(&args, argc, argv, TOOL_LAYOUT, O_RDONLY);
	validate_args(&args);
	init_ctx(&args, &ctx);
	print_layout(&ctx, "brkfs layout");
	close(args.imgfd);

	return EXIT_SUCCESS;
}
