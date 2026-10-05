/* TinyOS 裸机 shim：errno.h */
#ifndef TINYOS_SHIM_ERRNO_H
#define TINYOS_SHIM_ERRNO_H

extern int errno;
#define EPERM  1
#define ENOENT 2
#define EINTR  4
#define EIO    5
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EISDIR 21
#define ENOTDIR 20
#define EILSEQ  22

#endif
