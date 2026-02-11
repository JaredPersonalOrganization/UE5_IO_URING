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


#if __has_include(<linux/nvme_ioctl.h>)
#include <linux/nvme_ioctl.h>

#ifndef NVME_URING_CMD_IO	
#define NVME_URING_CMD_IO _IOWR('N', 0x80, struct nvme_uring_cmd)
// Assumes that if we don't have the macro, we likely don't have the structure.
struct nvme_uring_cmd {
	__u8	opcode;
	__u8	flags;
	__u16	rsvd1;
	__u32	nsid;
	__u32	cdw2;
	__u32	cdw3;
	__u64	metadata;
	__u64	addr;
	__u32	metadata_len;
	__u32	data_len;
	__u32	cdw10;
	__u32	cdw11;
	__u32	cdw12;
	__u32	cdw13;
	__u32	cdw14;
	__u32	cdw15;
	__u32	timeout_ms;
	__u32   rsvd2;
};

enum {
	NVME_RW_LR			= 1 << 15,
	NVME_RW_FUA			= 1 << 14,
	NVME_RW_APPEND_PIREMAP		= 1 << 9,
	NVME_RW_DSM_FREQ_UNSPEC		= 0,
	NVME_RW_DSM_FREQ_TYPICAL	= 1,
	NVME_RW_DSM_FREQ_RARE		= 2,
	NVME_RW_DSM_FREQ_READS		= 3,
	NVME_RW_DSM_FREQ_WRITES		= 4,
	NVME_RW_DSM_FREQ_RW		= 5,
	NVME_RW_DSM_FREQ_ONCE		= 6,
	NVME_RW_DSM_FREQ_PREFETCH	= 7,
	NVME_RW_DSM_FREQ_TEMP		= 8,
	NVME_RW_DSM_LATENCY_NONE	= 0 << 4,
	NVME_RW_DSM_LATENCY_IDLE	= 1 << 4,
	NVME_RW_DSM_LATENCY_NORM	= 2 << 4,
	NVME_RW_DSM_LATENCY_LOW		= 3 << 4,
	NVME_RW_DSM_SEQ_REQ		= 1 << 6,
	NVME_RW_DSM_COMPRESSED		= 1 << 7,
	NVME_RW_PRINFO_PRCHK_REF	= 1 << 10,
	NVME_RW_PRINFO_PRCHK_APP	= 1 << 11,
	NVME_RW_PRINFO_PRCHK_GUARD	= 1 << 12,
	NVME_RW_PRINFO_PRACT		= 1 << 13,
	NVME_RW_DTYPE_STREAMS		= 1 << 4,
	NVME_WZ_DEAC			= 1 << 9,
};

#endif 

#ifndef NVME_URING_CMD_IO_VEC
#define NVME_URING_CMD_IO_VEC	_IOWR('N', 0x81, struct nvme_uring_cmd)
#endif

#ifndef NVME_URING_CMD_ADMIN
#define NVME_URING_CMD_ADMIN	_IOWR('N', 0x82, struct nvme_uring_cmd)
#endif 

#ifndef NVME_URING_CMD_ADMIN_VEC
#define NVME_URING_CMD_ADMIN_VEC _IOWR('N', 0x83, s
#endif

#else
struct nvme_uring_cmd {
	__u8	opcode;
	__u8	flags;
	__u16	rsvd1;
	__u32	nsid;
	__u32	cdw2;
	__u32	cdw3;
	__u64	metadata;
	__u64	addr;
	__u32	metadata_len;
	__u32	data_len;
	__u32	cdw10;
	__u32	cdw11;
	__u32	cdw12;
	__u32	cdw13;
	__u32	cdw14;
	__u32	cdw15;
	__u32	timeout_ms;
	__u32   rsvd2;
};

#define NVME_URING_CMD_IO	_IOWR('N', 0x80, struct nvme_uring_cmd)
#define NVME_URING_CMD_IO_VEC	_IOWR('N', 0x81, struct nvme_uring_cmd)
#define NVME_URING_CMD_ADMIN	_IOWR('N', 0x82, struct nvme_uring_cmd)
#define NVME_URING_CMD_ADMIN_VEC _IOWR('N', 0x83, struct nvme_uring_cmd)

#endif

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


#if __has_include(<linux/fiemap.h>)
#include <linux/fiemap.h>
#else
/**
* struct fiemap_extent - description of one fiemap extent
* @fe_logical: byte offset of the extent in the file
* @fe_physical: byte offset of extent on disk
* @fe_length: length in bytes for this extent
* @fe_flags: FIEMAP_EXTENT_* flags for this extent
*/
struct fiemap_extent {
	__u64 fe_logical;
	__u64 fe_physical;
	__u64 fe_length;
	/* private: */
	__u64 fe_reserved64[2];
	/* public: */
	__u32 fe_flags;
	/* private: */
	__u32 fe_reserved[3];
};

/**
 * struct fiemap - file extent mappings
 * @fm_start: byte offset (inclusive) at which to start mapping (in)
 * @fm_length: logical length of mapping which userspace wants (in)
 * @fm_flags: FIEMAP_FLAG_* flags for request (in/out)
 * @fm_mapped_extents: number of extents that were mapped (out)
 * @fm_extent_count: size of fm_extents array (in)
 * @fm_extents: array of mapped extents (out)
 */
struct fiemap {
	__u64 fm_start;
	__u64 fm_length;
	__u32 fm_flags;
	__u32 fm_mapped_extents;
	__u32 fm_extent_count;
	/* private: */
	__u32 fm_reserved;
	/* public: */
	struct fiemap_extent fm_extents[];
};


#endif


#define NVME_CMD_READ 0x02

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