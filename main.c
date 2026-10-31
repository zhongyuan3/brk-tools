#include "commands.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

#define BRKFS_PROG "brkfs"

static noreturn void print_help(int exit_code)
{
	fprintf(stderr,
		"Usage: " BRKFS_PROG " <command> [options]\n\n"
		"Commands:\n"
		"  mkfs   [options] <image>          Format an image\n"
		"  layout [options] <image>          Show the layout mkfs would create\n"
		"  info   <image>                    Show the layout of an existing image\n"
		"  ls     [options] [<path>] <image> List a directory\n"
		"  cat    <path> <image>             Print a regular file\n"
		"  cp     [options] <source> <destination> <image>\n"
		"                                    Copy a host file into an image\n"
		"  check  [options] <image>          Check inode, block and directory consistency\n"
		"\n"
		"Run '" BRKFS_PROG
		" <command> --help' for command-specific options.\n"
		"\n"
		"General options:\n"
		"  -h, --help    Show this help message and exit\n"
		"      --version Show version information and exit\n");
	exit(exit_code);
}

int main(int argc, char *argv[])
{
	const char *cmd;

	set_prog_name(BRKFS_PROG);

	if (argc < 2)
		print_help(EXIT_FAILURE);

	cmd = argv[1];

	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") ||
	    !strcmp(cmd, "--help"))
		print_help(EXIT_SUCCESS);
	if (!strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
		printf(BRKFS_PROG " %s\n", BRKFS_VERSION);
		return EXIT_SUCCESS;
	}
	if (!strcmp(cmd, "mkfs")) {
		set_prog_name(BRKFS_PROG " mkfs");
		return cmd_mkfs(argc - 1, argv + 1);
	}
	if (!strcmp(cmd, "layout")) {
		set_prog_name(BRKFS_PROG " layout");
		return cmd_layout(argc - 1, argv + 1);
	}
	if (!strcmp(cmd, "info")) {
		set_prog_name(BRKFS_PROG " info");
		return cmd_info(argc - 1, argv + 1);
	}
	if (!strcmp(cmd, "ls")) {
		set_prog_name(BRKFS_PROG " ls");
		return cmd_ls(argc - 1, argv + 1);
	}
	if (!strcmp(cmd, "cat")) {
		set_prog_name(BRKFS_PROG " cat");
		return cmd_cat(argc - 1, argv + 1);
	}
	if (!strcmp(cmd, "cp")) {
		set_prog_name(BRKFS_PROG " cp");
		return cmd_cp(argc - 1, argv + 1);
	}
	if (!strcmp(cmd, "check")) {
		set_prog_name(BRKFS_PROG " check");
		return cmd_check(argc - 1, argv + 1);
	}

	fprintf(stderr, "%s: unknown command: %s\n\n", BRKFS_PROG, cmd);
	print_help(EXIT_FAILURE);
}
