# BRK Tools

Host-side tools for creating, inspecting and modifying [brkfs](https://github.com/zhongyuan3/brk) filesystem images.

`brkfs` is a multicall host binary used by the [BRK userland](https://github.com/zhongyuan3/brk-user) build to package user programs, configuration files and tests into a bootable root filesystem image.

## Layout

```text
main.c        command dispatch and top-level help
mkfs.c        mkfs and layout commands
info.c        info command
ls.c          ls command
cat.c         cat command
cp.c          cp command
check.c       filesystem consistency checker and repair
common.c      shared image I/O, path handling and error helpers
common.h      shared host-side declarations
brkfs.h       on-disk brkfs format definitions
commands.h    command entry points
```

`brkfs.h` is a manual copy of the layout definitions in the kernel header
`src/fs/brkfs/brkfs.h`.  Keep the two in sync when changing the on-disk format.
The on-disk format version is stored in the superblock and currently uses
CRC32C checksums for the superblock, inodes and directory blocks.

## Requirements

- A C11 host compiler (`cc` by default)
- A POSIX-like host environment
- No RISC-V cross compiler is required

## Build

```sh
make
make HOSTCC=clang
make clean
```

The resulting binary is `./brkfs`.

Conventional Commit hooks can be enabled with:

```sh
make hooks
```

## Usage

```text
brkfs <command> [options]
```

Show all commands:

```sh
./brkfs --help
./brkfs --version
```

| Command  | Purpose |
| -------- | ------- |
| `mkfs`   | Format an existing image as brkfs |
| `layout` | Show the layout `mkfs` would create without modifying the image |
| `info`   | Show the layout of an existing brkfs image |
| `ls`     | List a directory inside an image |
| `cat`    | Print a regular file from an image to stdout |
| `cp`     | Copy a host file or directory into an image |
| `check`  | Check inode, block and directory consistency; optionally repair |

Every command accepts `--help`:

```sh
./brkfs mkfs --help
./brkfs cp --help
```

### Create an image

`mkfs` formats an image that already has the required size.  A typical rootfs
workflow is:

```sh
dd if=/dev/zero of=rootfs.img bs=4096 count=8192
./brkfs mkfs -bs 4096 -I 1024 rootfs.img
```

Common `mkfs` options:

| Option | Description |
| ------ | ----------- |
| `-bs <size>` | Block size, 1024-4096 bytes and a multiple of 1024 |
| `--count <count>` | Number of blocks |
| `-I <count>` | Number of inodes |
| `-i <size>` | Inode size, 128-256 bytes and a power of two |
| `-L <label>` | Volume label |

`layout` accepts the same options as `mkfs` and prints the resulting layout
without writing anything.

### Inspect and modify an image

```sh
# Show the superblock and layout
./brkfs info rootfs.img

# List the root directory, or a directory path
./brkfs ls rootfs.img
./brkfs ls -l /usr rootfs.img
./brkfs ls -la / rootfs.img

# Print a regular file
./brkfs cat /etc/passwd rootfs.img

# Copy a host file into the image
./brkfs cp ./hello /bin/hello rootfs.img

# Copy a directory recursively and create destination parents as needed
./brkfs cp -r --parents ./etc /etc rootfs.img
```

Paths passed to `ls`, `cat` and `cp` are paths inside the image, not host
paths.

### Check and repair

```sh
./brkfs check rootfs.img
./brkfs check -v rootfs.img
./brkfs check --repair rootfs.img
```

`--repair` must only be used on an image that is not mounted by a running
kernel.

## Rootfs integration

The BRK userland `scripts/mkrootfs.sh` uses this tool to create the rootfs
image.  It calls `mkfs`, then `cp` for the FHS directory skeleton, binaries,
configuration files and tests.  Building a rootfs therefore requires both this
repository and the kernel/userland trees.

## License

This project is licensed under the MIT License.  See [LICENSE](LICENSE) for
details.
