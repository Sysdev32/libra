#ifndef FCNTL_H
#define FCNTL_H

/* ============================================================================
 * INTERNAL FLAG VALUES (Matches newlib/BSD)
 * ============================================================================ */

#define _FREAD      0x0001
#define _FWRITE     0x0002
#define _FAPPEND    0x0008
#define _FASYNC     0x0040
#define _FCREAT     0x0200
#define _FTRUNC     0x0400
#define _FEXCL      0x0800
#define _FNBIO      0x1000
#define _FSYNC      0x2000
#define _FNONBLOCK  0x4000
#define _FNOCTTY    0x8000
#define _FNOINHERIT 0x40000
#define _FDIRECT    0x80000
#define _FNOFOLLOW  0x100000
#define _FDIRECTORY 0x200000
#define _FEXECSRCH  0x400000

/* ============================================================================
 * ACCESS MODES
 * ============================================================================ */

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   (O_RDONLY | O_WRONLY | O_RDWR)

/* ============================================================================
 * OPEN FLAGS
 * ============================================================================ */

#define O_APPEND    _FAPPEND
#define O_CREAT     _FCREAT
#define O_TRUNC     _FTRUNC
#define O_EXCL      _FEXCL
#define O_SYNC      _FSYNC
#define O_DSYNC     _FSYNC
#define O_RSYNC     _FSYNC
#define O_NONBLOCK  _FNONBLOCK
#define O_NDELAY    _FNONBLOCK
#define O_NOCTTY    _FNOCTTY

#define O_CLOEXEC   _FNOINHERIT
#define O_NOFOLLOW  _FNOFOLLOW
#define O_DIRECTORY _FDIRECTORY
#define O_EXEC      _FEXECSRCH
#define O_SEARCH    _FEXECSRCH

#define O_DIRECT    _FDIRECT

/* ============================================================================
 * OPTIONAL FLAGS
 * ============================================================================ */

#define O_LARGEFILE 0
#define O_NOATIME   0
#define O_PATH      0
#define O_TMPFILE   0

/* ============================================================================
 * FD FLAGS
 * ============================================================================ */

#define FD_CLOEXEC 1

/* ============================================================================
 * fcntl() COMMANDS
 * ============================================================================ */

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETOWN        5
#define F_SETOWN        6
#define F_GETLK         7
#define F_SETLK         8
#define F_SETLKW        9
#define F_DUPFD_CLOEXEC 14

/* ============================================================================
 * Record locking
 * ============================================================================ */

#define F_RDLCK 1
#define F_WRLCK 2
#define F_UNLCK 3

/* ============================================================================
 * openat()
 * ============================================================================ */

#define AT_FDCWD -2

#define AT_EACCESS          0x0001
#define AT_SYMLINK_NOFOLLOW 0x0002
#define AT_SYMLINK_FOLLOW   0x0004
#define AT_REMOVEDIR        0x0008
#define AT_EMPTY_PATH       0x0010

#endif