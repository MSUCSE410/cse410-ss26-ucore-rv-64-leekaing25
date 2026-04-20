#ifndef FCNTL_H
#define FCNTL_H

#include "types.h"

#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR 0x002
#define O_CREATE 0x200
#define O_TRUNC 0x400

#define DIR 0x040000
#define FILE 0x100000

typedef struct {
	uint64 dev;
	uint64 ino;
	// The user tests only need a simple directory-vs-file classification,
	// not full Unix permission bits.
	uint32 mode;
	// Exposes the kernel inode's hard-link count to user space.
	uint32 nlink;
	uint64 pad[7];
} Stat;

#endif // FCNIL_H
