#ifndef FCNTL_H
#define FCNTL_H

/* ============================================================================
 * FILE ACCESS MODES (Required - Select exactly one using O_ACCMODE)
 * ============================================================================ */

#define O_RDONLY        00000000    /* Open for reading only */
#define O_WRONLY        00000001    /* Open for writing only */
#define O_RDWR          00000002    /* Open for reading and writing */
#define O_ACCMODE       00000003    /* Mask for file access modes */

/* ============================================================================
 * FILE CREATION FLAGS (Used with open/create APIs)
 * ============================================================================ */

#define O_CREAT         00000100    /* Create file if it does not exist */
#define O_EXCL          00000200    /* Fail if file already exists with O_CREAT */
#define O_NOCTTY        00000400    /* Do not assign controlling terminal */
#define O_TRUNC         00001000    /* Truncate file length to 0 if it exists */
#define O_APPEND        00002000    /* Set append mode; writes always occur at EOF */
#define O_NONBLOCK      00004000    /* Non-blocking I/O mode */
#define O_NDELAY        O_NONBLOCK  /* Alias for O_NONBLOCK */

/* ============================================================================
 * ADVANCED STATUS & EXECUTION FLAGS
 * ============================================================================ */

#define O_SYNC          00010000    /* Synchronous writes (data + metadata) */
#define FASYNC          00020000    /* Signal pgrp when I/O is possible */
#define O_DIRECT        00040000    /* Direct disk access; bypass page cache */
#define O_LARGEFILE     00100000    /* Allow opening large files (>2GB on 32-bit systems) */
#define O_DIRECTORY     00200000    /* Must be a directory; fail if regular file */
#define O_NOFOLLOW      00400000    /* Do not follow symbolic links */
#define O_NOATIME       01000000    /* Do not update last access time (st_atime) */
#define O_CLOEXEC       02000000    /* Set Close-on-Exec flag automatically */
#define O_DSYNC         00010000    /* Synchronous data writes (data only) */
#define O_PATH          010000000   /* Obtain file descriptor for path operations only */
#define O_TMPFILE       020200000   /* Create an unnamed temporary file */

#endif /* FCNTL_H */