#include "commands.h"
#include "common.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>

struct info_args {
	const char *image;
	int imgfd;
};

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: %s [options] <image>\n\n"
		"Show the on-disk layout of an existing brkfs image.\n\n"
		"Options:\n"
		"      --help  Show this help and exit\n",
		get_prog_name());
	exit(exit_code);
}

static void parse_args(struct info_args *a, int argc, char *argv[])
{
	a->image = NULL;
	a->imgfd = -1;

	if (argc < 2)
		print_help(EXIT_FAILURE);

	int i = 1;
	for (; i < argc; i++) {
		if (argv[i][0] != '-')
			break;
		if (!strcmp(argv[i], "--help"))
			print_help(EXIT_SUCCESS);
		die_arg("unknown option: %s", argv[i]);
	}

	if (argc - i != 1)
		die_arg("expected <image> (use --help)");

	a->image = argv[i];
	a->imgfd = open(a->image, O_RDONLY);
	if (a->imgfd < 0)
		die_errno("open");
}

int cmd_info(int argc, char *argv[])
{
	struct info_args args = { 0 };
	struct brkfs_ctx ctx = { 0 };

	parse_args(&args, argc, argv);
	open_ctx(args.imgfd, &ctx);
	print_layout(&ctx, "brkfs info");
	close(args.imgfd);

	return EXIT_SUCCESS;
}
