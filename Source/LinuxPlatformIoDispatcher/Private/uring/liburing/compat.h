/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_COMPAT_H
#define LIBURING_COMPAT_H

// It would have been so much more ideal to use PreBuildSteps to generate this file....

#ifndef __has_include
#error "Missing __has_include"
#endif

// figure out if we have BLOCK_URING_CMD_DISCARD
#if __has_include(<linux/blkdev.h>)
#include <linux/blkdev.h>
#endif

#if defined(BLOCK_URING_CMD_DISCARD)
#define HAS_BLOCK_SUPPORT 1
#else
#define HAS_BLOCK_SUPPORT 0
#endif

#if !HAS_BLOCK_SUPPORT
#include <linux/ioctl.h>
#ifndef BLOCK_URING_CMD_DISCARD
#define BLOCK_URING_CMD_DISCARD _IO(0x12, 0)
#endif
#endif


// Check for __kernel_rwf_t
#if __has_include(<linux/fs.h>)
#include <linux/fs.h>
#else
typedef int __kernel_rwf_t; 
#endif


// check for __kernel_timespec
#if __has_include("linux/time_types.h")
#include <linux/time_types.h>
#else 
struct __kernel_timespec {
	int64_t		tv_sec;
	long long	tv_nsec;
};
#endif

#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1

// check for open_how
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#else 
struct open_how {
	uint64_t	flags;
	uint64_t	mode;
	uint64_t	resolve;
};	
#endif


#if !__has_include(<linux/futex.h>)
#include <inttypes.h>

#define FUTEX_32	2
#define FUTEX_WAITV_MAX	128

struct futex_waitv {
	uint64_t	val;
	uint64_t	uaddr;
	uint32_t	flags;
	uint32_t	__reserved;
};
#endif

#if !__has_include("sys/wait.h")
typedef enum
{
	P_ALL,		/* Wait for any child.  */
	P_PID,		/* Wait for specified process.  */
	P_PGID		/* Wait for members of process group.  */
  } idtype_t;
#endif
 
#include <endian.h>

#ifndef __USE_ATFILE
#define __USE_ATFILE
#endif

#ifndef __USE_XOPEN_EXTENDED
#define __USE_XOPEN_EXTENDED
#endif

#endif