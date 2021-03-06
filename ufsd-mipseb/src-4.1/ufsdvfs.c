/*++

Module Name:

    ufsdvfs.c

Abstract:

    This module implements VFS entry points for
    UFSD-based Linux filesystem driver.

Author:

    Ahdrey Shedel

Revision History:

    27/12/2002 - Andrey Shedel - Created

    Since 29/07/2005 - Alexander Mamaev

--*/

//
// This field is updated by SVN
//
static const char s_FileVer[] = "$Id: ufsdvfs.c 214002 2013-10-08 14:08:41Z shura $";

//
// Tune ufsdvfs.c
//

//#define UFSD_COUNT_CONTAINED        "Use unix semantics for dir->i_nlink"
//#define UFSD_USE_ASM_DIV64          "Use built-in macros do_div in <asm/div64.h> instead of __udivdi3"
#define UFSD_READAHEAD_PAGES        8
// NOTE: Kernel's utf8 does not support U+10000 (see utf8_mbtowc for details and note that 'typedef _u16 wchar_t;' )
//#define UFSD_BUILTINT_UTF8          "Use builtin utf8 code page"
#ifdef UFSD_DEBUG
#define UFSD_DEBUG_ALLOC            "Track memory allocation/deallocation"
#endif
// Activate this define to test readdir
//#define UFSD_EMULATE_SMALL_READDIR_BUFFER 10

#ifndef UFSD_SMART_DIRTY_SEC
  #define UFSD_SMART_DIRTY_SEC  5
#endif

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/nls.h>
#include <asm/uaccess.h>
#if defined HAVE_LINUX_BACKING_DEV_H && HAVE_LINUX_BACKING_DEV_H
#include <linux/backing-dev.h>
#endif
#if defined HAVE_LINUX_SMP_LOCK_H && HAVE_LINUX_SMP_LOCK_H
#include <linux/smp_lock.h>
#endif
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/writeback.h>

#include "config.h"

#if defined HAVE_LINUX_PAGEVEC_H && HAVE_LINUX_PAGEVEC_H
  #include <linux/pagevec.h>
  #if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN && defined AOP_FLAG_NOFS
    #define UFSD_DELAY_ALLOC  "use delay block allocation"
    #define UFSD_RED_ZONE   0x100
  #endif
#endif

#if defined HAVE_LINUX_NAMEI_H && HAVE_LINUX_NAMEI_H
  #include <linux/namei.h>
#endif

#if defined UFSD_USE_ASM_DIV64
  #include <asm/div64.h> // this file defines macros 'do_div'
#endif

#if defined HAVE_LINUX_BUFFER_HEAD_H && HAVE_LINUX_BUFFER_HEAD_H
  #include <linux/buffer_head.h>
#endif

#if defined HAVE_LINUX_UIO_H && HAVE_LINUX_UIO_H
  #include <linux/uio.h>
#endif

#if defined HAVE_LINUX_STATFS_H && HAVE_LINUX_STATFS_H
  #include <linux/statfs.h>
#endif

#if defined HAVE_LINUX_VERMAGIC_H && HAVE_LINUX_VERMAGIC_H
  #include <linux/vermagic.h>
#endif

#if defined HAVE_LINUX_MPAGE_H && HAVE_LINUX_MPAGE_H
  #include <linux/mpage.h>
#endif

#if defined HAVE_LINUX_BLKDEV_H && HAVE_LINUX_BLKDEV_H
  #include <linux/blkdev.h>
#elif defined HAVE_LINUX_BLK_H && HAVE_LINUX_BLK_H
  #include <linux/blk.h>
#endif

#if defined HAVE_LINUX_DELAY_H && HAVE_LINUX_DELAY_H
  #include <linux/delay.h> // jiffies_to_msecs
#endif

#if defined HAVE_LINUX_IOBUF_H && HAVE_LINUX_IOBUF_H
  #include <linux/iobuf.h>
#endif

#if defined HAVE_LINUX_LOCKS_H && HAVE_LINUX_LOCKS_H
  #include <linux/locks.h>
#endif

#if defined HAVE_LINUX_EXPORTFS_H && HAVE_LINUX_EXPORTFS_H
  #include <linux/exportfs.h>
#endif

#if defined HAVE_LINUX_FS_STRUCT_H && HAVE_LINUX_FS_STRUCT_H
  #include <linux/fs_struct.h>
#endif

#if defined HAVE_LINUX_PROC_NS_H && HAVE_LINUX_PROC_NS_H
  #include <linux/proc_ns.h>
#endif

#if defined HAVE_LINUX_AIO_H && HAVE_LINUX_AIO_H
  #include <linux/aio.h>
#endif

//#define physmem totalram_pages

#if defined CONFIG_FS_POSIX_ACL \
  && ( (defined HAVE_DECL_POSIX_ACL_FROM_XATTR && HAVE_DECL_POSIX_ACL_FROM_XATTR)\
    || (defined HAVE_DECL_POSIX_ACL_TO_XATTR_V2 && HAVE_DECL_POSIX_ACL_TO_XATTR_V2) )
  #include <linux/posix_acl_xattr.h>
  #define UFSD_USE_XATTR              "Include code to support xattr and acl"
#endif

#if defined HAVE_DECL_MODE_TYPE_MODE_T && HAVE_DECL_MODE_TYPE_MODE_T
  #define posix_acl_mode mode_t
#elif defined HAVE_DECL_MODE_TYPE_UMODE_T && HAVE_DECL_MODE_TYPE_UMODE_T
  #define posix_acl_mode umode_t
#else
  #define posix_acl_mode umode_t
#endif

#ifndef UFSD_USE_FLUSH_THREAD
  #if !(defined HAVE_STRUCT_SUPER_OPERATIONS_WRITE_SUPER && HAVE_STRUCT_SUPER_OPERATIONS_WRITE_SUPER)
    #define UFSD_USE_FLUSH_THREAD "Use thread to flush periodically"
  #endif
#endif

#if defined UFSD_USE_FLUSH_THREAD && !(defined HAVE_LINUX_FREEZER_H && HAVE_LINUX_FREEZER_H)
  #undef UFSD_USE_FLUSH_THREAD
#endif

//
// Default trace level for many functions in this module
//
#define Dbg  UFSD_LEVEL_VFS

#define UFSD_PACKAGE_STAMP " " "lke_8.9.0_22"

//
// Used to trace driver version
//
static const char s_DriverVer[] = PACKAGE_VERSION
#ifdef PACKAGE_TAG
   " " PACKAGE_TAG
#else
   UFSD_PACKAGE_STAMP
#endif
#if defined CONFIG_LBD | defined CONFIG_LBDAF
  ", LBD=ON"
#else
  ", LBD=OFF"
#endif
#if defined UFSD_DELAY_ALLOC
  ", delalloc"
#endif
#if defined UFSD_USE_XATTR
  ", acl"
#endif
#if !defined UFSD_NO_USE_IOCTL
  ", ioctl"
#endif
#ifndef UFSD_DISABLE_UGM
  ", ugm"
#endif
#ifdef UFSD_CHECK_BDI
  ", bdi"
#endif
#ifdef UFSD_USE_FLUSH_THREAD
  ", sd2"
#else
  ", sd"
#endif
#ifdef WRITE_FLUSH_FUA
  ", fua"
#elif defined WRITE_BARRIER
  ", wb"
#else
  ", nb"
#endif
#ifdef UFSD_USE_BUILTIN_ZEROING
  ", bz"
#endif
#ifdef UFSD_DEBUG
  ", debug"
#elif defined UFSD_TRACE
  ", tr"
#endif
  ;

#if !(defined HAVE_SECTOR_T_KERNEL) || !HAVE_SECTOR_T_KERNEL
 typedef long sector_t;
#endif

#include "ufsdapi.h"
#include "vfsdebug.h"

#if defined HAVE_LINUX_MUTEX_H && HAVE_LINUX_MUTEX_H
  #include <linux/mutex.h>
#endif

#if defined HAVE_LINUX_MUTEX_H && HAVE_LINUX_MUTEX_H && !(defined CONFIG_DEBUG_LOCK_ALLOC && CONFIG_DEBUG_LOCK_ALLOC)
  // Use mutex if possible
  #define Mutex                    mutex
  #define Mutex_init(lock)         mutex_init(lock)
  #define Mutex_lock(lock)         mutex_lock(lock)
  #define Mutex_trylock(lock)      mutex_trylock(lock)
  #define Mutex_unlock(lock)       mutex_unlock(lock)
  #define Mutex_destroy(lock)      mutex_destroy(lock)
#else
  // Use semaphore if mutex is not available
  #define Mutex                    semaphore
  // Don't 'cause may be G.P.L. #define Mutex_init(lock)         sema_init(lock, 1)
  #define Mutex_init(lock)         *lock = (struct semaphore) __SEMAPHORE_INITIALIZER(*lock, 1)
  #define Mutex_lock(lock)         down(lock)
  #define Mutex_trylock(lock)      !down_trylock(lock)
  #define Mutex_unlock(lock)       up(lock)
  #define Mutex_destroy(lock)      do { } while ((void) 0,0)
#endif


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
  #define STATIC_CONST static const
#else
  #define STATIC_CONST static
#endif

// 2.4 kernel
#ifndef __user
  #define __user
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
  static inline u32 new_encode_dev(dev_t dev){
    unsigned major = MAJOR(dev);
    unsigned minor = MINOR(dev);
    return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
  }
  static inline dev_t new_decode_dev(u32 dev){
    unsigned major = (dev & 0xfff00) >> 8;
    unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);
    return MKDEV(major, minor);
  }
#endif

#ifndef MS_POSIXACL
  #define MS_POSIXACL (1<<16)
#endif

#ifndef BDEVNAME_SIZE
  #define BDEVNAME_SIZE 32
#endif

#if !(defined HAVE_STRUCT_SUPER_BLOCK_S_FS_INFO && HAVE_STRUCT_SUPER_BLOCK_S_FS_INFO)
  #define s_fs_info u.generic_sbp
#endif

#if (defined CONFIG_NLS | defined CONFIG_NLS_MODULE) & !defined UFSD_BUILTINT_UTF8
  #define UFSD_USE_NLS  "Use nls functions instead of builtin utf8 to convert strings"
#endif

#if defined HAVE_STRUCT_SUPER_OPERATIONS_ALLOC_INODE && HAVE_STRUCT_SUPER_OPERATIONS_ALLOC_INODE
  #define UFSD_BIG_UNODE  "inode is a part of unode"
#endif

#if defined HAVE_KMEM_CACHE && HAVE_KMEM_CACHE && defined HAVE_KMEM_CACHE_NOT_DIFF && HAVE_KMEM_CACHE_NOT_DIFF
  #define u_kmem_cache struct kmem_cache
#elif defined HAVE_KMEM_CACHE_T && HAVE_KMEM_CACHE_T
  // Previous kernel uses 'typedef struct kmem_cache_s kmem_cache_t'
  #define u_kmem_cache kmem_cache_t
#else
  // The latest kernel uses 'struct kmem_cache'
  #define u_kmem_cache struct kmem_cache
#endif

#ifndef SLAB_MEM_SPREAD
  #define SLAB_MEM_SPREAD 0
#endif

#ifndef ACL_NOT_CACHED
 #define ACL_NOT_CACHED ((void *)(-1))
#endif

#if  !(defined HAVE_DECL_KMEM_CACHE_CREATE_V1 && HAVE_DECL_KMEM_CACHE_CREATE_V1)\
  && !(defined HAVE_DECL_KMEM_CACHE_CREATE_V2 && HAVE_DECL_KMEM_CACHE_CREATE_V2)\
  && !(defined HAVE_DECL_KMEM_CACHE_CREATE_V3 && HAVE_DECL_KMEM_CACHE_CREATE_V3)\
  && !(defined HAVE_DECL_KMEM_CACHE_CREATE_V4 && HAVE_DECL_KMEM_CACHE_CREATE_V4)
#error "Unknown version of kmem_cache_create"
#endif

#if defined HAVE_DECL_KMAP_ATOMIC_V1 && HAVE_DECL_KMAP_ATOMIC_V1
  #define atomic_kmap(p)    kmap_atomic( (p), KM_USER0 )
  #define atomic_kunmap(p)  kunmap_atomic( (p), KM_USER0 )
#else
  #define atomic_kmap(p)    kmap_atomic( (p) )
  #define atomic_kunmap(p)  kunmap_atomic( (p) )
#endif

#if !( defined HAVE_DECL_FILE_INODE && HAVE_DECL_FILE_INODE )
  #define file_inode(x) (x)->f_path.dentry->d_inode
#endif

//
// This function returns UFSD's handle for 'inode'
//
// UFSD_FILE* UFSD_FH( IN struct inode* inode );
//
#define UFSD_FH(i)      (UFSD_U(i)->ufile)

#define UFSD_SB(sb)     ((usuper*)(sb)->s_fs_info)
#define UFSD_VOLUME(sb) UFSD_SB(sb)->Ufsd

#define UFSD_STATE_DA_ALLOC_CLOSE 0x00000010 // Alloc DA blks on close

//
// In memory ufsd inode
//
typedef struct unode {
#ifdef UFSD_BIG_UNODE
  struct inode  i;
#endif
  spinlock_t    block_lock;

#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
  // inode does not contain cached values of acl/default_acl.  use own acl cache
	struct posix_acl* acl;
	struct posix_acl* default_acl;
#endif

  //
  // init_once initialize members ['i' - 'ufile')
  // ufsd_alloc_inode resets members ['ufile' end)
  // Do not move 'ufile' member
  //
  UFSD_FILE*    ufile;

  sector_t      Vbn, Lbn, Len;        // saved one fragment

  loff_t        mmu;
  unsigned      atime, ctime, mtime;  // saved on-disk times in seconds

  char          set_mode;
  char          sparse;
  char          compr;
  char          encrypt;
  char          xattr;
  char          flags;

#ifdef UFSD_DELAY_ALLOC
  unsigned      i_state_flags;      // See UFSD_STATE_XXX
  sector_t      i_reserved_data_blocks;
  sector_t      i_reserved_meta_blocks;
#endif

} unode;

#ifdef UFSD_BIG_UNODE
  //
  // This function returns 'unode' for 'inode'
  //
  // struct unode* UFSD_U( IN struct inode* inode );
  //
  #ifdef UFSD_DEBUG
    struct unode* UFSD_U( IN struct inode* inode ) {
      return container_of((inode), struct unode, i);
    }
  #else
    #define UFSD_U(inode)   (container_of((inode), struct unode, i))
  #endif

#else

  #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
    #define UFSD_U(i)      (*(struct unode**)&((i)->i_private))
  #else
    #define UFSD_U(i)      (*(struct unode**)&((i)->u.generic_ip))
  #endif

#endif

//
// Private superblock structure.
// Stored in super_block.s_fs_info
//
typedef struct usuper {
    UINT64            MaxBlock;
    UINT64            Eod;          // End of directory
    UFSD_VOLUME*      Ufsd;
#if defined HAVE_DECL_FST_GETSB_V2 && HAVE_DECL_FST_GETSB_V2
    struct vfsmount*  VfsMnt;
    char              MntBuffer[32];
#endif
    struct Mutex      ApiMutex;
    struct Mutex      NoCaseMutex;
    mount_options     options;
#ifdef UFSD_CHECK_BDI
    struct backing_dev_info*  bdi;
#endif
#ifdef UFSD_USE_XATTR
    void*             Xbuffer;
    size_t            BytesPerXBuffer;
#endif

#ifdef UFSD_HFS
    struct buffer_head* TailBh;
#endif

#ifdef UFSD_USE_FLUSH_THREAD
    rwlock_t            StateLock;     // Protect the various scalars
    wait_queue_head_t   WaitDoneFlush;
    wait_queue_head_t   WaitExitFlush;
    struct task_struct* FlushTask;     // Pointer to the current flush thread for this volume
    struct timer_list   FlushTimer;    // The timer used to wakeup the flush thread
    unsigned char       ExitFlushThread; // Used to exit from flush thread
    unsigned char       bDirty;
#endif
    unsigned long       LastDirty;

#if defined CONFIG_PROC_FS
    struct proc_dir_entry*  procdir;
#endif
    TRACE_ONLY( struct sysinfo    SysInfo; )
    spinlock_t        ddt_lock;     // DoDelayedTasks lock
    struct list_head  clear_list;   // List of inodes to clear

    #define RW_BUFFER_SIZE  (4*PAGE_SIZE)
    void*             rw_buffer;    // RW_BUFFER_SIZE
    unsigned int      ReadAheadBlocks;

#ifdef UFSD_TRACE
    size_t            nDelClear;      // Delayed clear
    size_t            nWrittenBlocks; // Count of written blocks
    size_t            nReadBlocks;    // Count of read blocks
    size_t            nWrittenBlocksNa; // Count of written not aligned blocks
    size_t            nReadBlocksNa;    // Count of read not aligned blocks
    size_t            nPinBlocks;     // Count of pinned blocks
    size_t            nUnpinBlocks;   // Count of unpinned blocks
    size_t            nMappedBh;      // Count of mapped buffers
    size_t            nMappedMem;     // Count of mapped buffers
    size_t            nUnMapped;      // Count of mapped buffers
    size_t            nHashCalls;     // Count of ufsd_name_hash calls
    size_t            nCompareCalls;  // Count of ufsd_compare calls

#ifdef UFSD_DEBUG
    // Internal profiler
    size_t            bdread_cnt;
    size_t            bdread_ticks;
    size_t            bdwrite_cnt;
    size_t            bdwrite_ticks;
    size_t            bdmap_cnt;
    size_t            bdmap_ticks;
    size_t            bdsetdirty_cnt;
    size_t            bdsetdirty_ticks;
    size_t            writepages_cnt;
    size_t            writepages_ticks;
    size_t            get_block_cnt;
    size_t            get_block_ticks;
    size_t            write_begin_cnt;
    size_t            write_begin_ticks;
    size_t            write_end_cnt;
    size_t            write_end_ticks;
    size_t            write_inode_cnt;
    size_t            write_inode_ticks;
    size_t            da_writepages_cnt;
    size_t            da_writepages_ticks;
    size_t            da_write_begin_cnt;
    size_t            da_write_begin_ticks;
    size_t            da_write_end_cnt;
    size_t            da_write_end_ticks;
#endif
#endif

    unsigned char     BlkBits;      // Log2(BytesPerBlock)
    unsigned char     SctBits;      // Log2(BytesPerSector)

#if !(defined HAVE_STRUCT_SUPER_BLOCK_S_ID && HAVE_STRUCT_SUPER_BLOCK_S_ID)
    char             s_id[BDEVNAME_SIZE];
#endif

    atomic_t          VFlush;       // Need volume flush

#ifdef UFSD_DELAY_ALLOC
    atomic_long_t     FreeBlocks; // UINT64?
    atomic_long_t     DirtyBlocks;
    TRACE_ONLY( int   DoNotTraceNoSpc; )
#endif

} usuper;

typedef struct {
  struct list_head  list;
  UFSD_FILE*        file;
} delay_task;


#ifdef UFSD_TRACE
  #define LockUfsd(s)     _LockUfsd( s, __func__ )
  #define TryLockUfsd(s)  _TryLockUfsd( s, __func__ )
  #define UnlockUfsd(s)   _UnlockUfsd( s, __func__ )
  static unsigned long WaitMutex, StartJiffies;
#else
  #define LockUfsd(s)     _LockUfsd( s )
  #define TryLockUfsd(s)  _TryLockUfsd( s )
  #define UnlockUfsd(s)   _UnlockUfsd( s )
#endif

#ifdef UFSD_DEBUG
  #define ProfileEnter(s,name)    \
    s->name##_cnt += 1;           \
    s->name##_ticks -= jiffies

  #define ProfileLeave(s,name)    \
    s->name##_ticks += jiffies
#else
  #define ProfileEnter(s,name)
  #define ProfileLeave(s,name)
#endif


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
  #define MODULE_BASE_ADDRESS  __this_module.module_core
#else
  #define MODULE_BASE_ADDRESS  &UFSD_CurrentTime //&__this_module
  #define get_seconds() xtime.tv_sec
#endif

#ifndef current_fsuid
  #define current_fsuid() (current->fsuid)
#endif

#ifndef current_fsgid
  #define current_fsgid() (current->fsgid)
#endif

#ifndef current_uid
  #define current_uid() (current->uid)
#endif

#ifndef current_gid
  #define current_gid() (current->gid)
#endif

#ifndef is_owner_or_cap
  #define is_owner_or_cap(i) ( ( uid_eq(current_fsuid(), (i)->i_uid) ) || capable(CAP_FOWNER) )
#endif

//
// assert tv_sec is the first member of type time_t
//
typedef char AssertTvSecOff [0 == offsetof(struct timespec, tv_sec)? 1 : -1];
typedef char AssertTvSecSz [sizeof(time_t) == sizeof(((struct timespec*)NULL)->tv_sec)? 1 : -1];

#define TIMESPEC_SECONDS(t) (*(time_t*)(t))

#define _100ns2seconds        10000000UL
#define SecondsToStartOf1970  0x00000002B6109100ULL
// How many seconds since 1970 till 1980
#define Seconds1970To1980     0x12CEA600

///////////////////////////////////////////////////////////
// UFSD_CurrentTime (GMT time)
//
// This function returns the number of 100 nanoseconds since 1601
///////////////////////////////////////////////////////////
UINT64 UFSDAPI_CALL
UFSD_CurrentTime(
    IN int PosixTime
    )
{
  UINT64 NtTime;
  time_t sec = get_seconds();

  if ( PosixTime )
    return sec;

  // 10^7 units of 100 nanoseconds in one second
  NtTime = _100ns2seconds * (sec + SecondsToStartOf1970);

#ifdef UFSD_PROFILE
  // Internal profiler uses this function to measure
  // time differences. The resolution of 1 second seems is too poor
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
  NtTime += CURRENT_TIME.tv_nsec/100;
#else
  NtTime += xtime.tv_usec/100000;
#endif
#endif
  return NtTime;
}


///////////////////////////////////////////////////////////
// UFSD_TimePosix2Nt
//
// Convert posix time (seconds from 1970) to Nt time (100 nseconds from 1601)
///////////////////////////////////////////////////////////
static inline
UINT64
UFSD_TimePosix2Nt(
    IN time_t PosixTime
    )
{
  return (SecondsToStartOf1970 + PosixTime)*_100ns2seconds;
}


///////////////////////////////////////////////////////////
// UFSD_TimeNt2Posix
//
// Convert Nt time (100 nseconds from 1601) to posix time (seconds from 1970)
///////////////////////////////////////////////////////////
static inline time_t
UFSD_TimeNt2Posix(
    IN UINT64 NtTime
    )
{
#if defined UFSD_USE_ASM_DIV64
  UINT64 seconds = NtTime;
  // WARNING: do_div changes its first argument(!)
  (void)do_div( seconds, _100ns2seconds );
  seconds -= SecondsToStartOf1970;
#else
  UINT64 seconds = NtTime / _100ns2seconds - SecondsToStartOf1970;
#endif

  assert( seconds > 0 );

  return (time_t)seconds;
}


///////////////////////////////////////////////////////////
// UFSD_TimeT2UfsdPosix
//
// Used to convert timespec to posix time
///////////////////////////////////////////////////////////
UINT64
UFSDAPI_CALL
UFSD_TimeT2UfsdPosix(
    IN const void* time
    )
{
  return *(time_t*)time;
}


///////////////////////////////////////////////////////////
// UFSD_TimeT2UfsdNt
//
// Used to convert timespec to Nt time
///////////////////////////////////////////////////////////
UINT64
UFSDAPI_CALL
UFSD_TimeT2UfsdNt(
    IN const void* time
    )
{
  return UFSD_TimePosix2Nt( *(time_t*)time );
}


///////////////////////////////////////////////////////////
// UfsdTimes2Inode
//
//
///////////////////////////////////////////////////////////
static inline void
UfsdTimes2Inode(
    IN usuper*        sbi,
    IN unode*         u,
    IN struct inode*  i,
    IN const UfsdFileInfo* Info
    )
{
  if ( sbi->options.posixtime ) {
    u->atime = TIMESPEC_SECONDS( &i->i_atime )  = Info->atime;
    u->ctime = TIMESPEC_SECONDS( &i->i_ctime )  = Info->ctime;
    u->mtime = TIMESPEC_SECONDS( &i->i_mtime )  = Info->mtime;
  } else {
    u->atime = TIMESPEC_SECONDS( &i->i_atime )  = UFSD_TimeNt2Posix( Info->atime );
    u->ctime = TIMESPEC_SECONDS( &i->i_ctime )  = UFSD_TimeNt2Posix( Info->ctime );
    u->mtime = TIMESPEC_SECONDS( &i->i_mtime )  = UFSD_TimeNt2Posix( Info->mtime );
  }
}


#if !(defined HAVE_DECL_TIMESPEC_COMPARE  && HAVE_DECL_TIMESPEC_COMPARE)
static inline int timespec_compare(const struct timespec *lhs, const struct timespec *rhs){
  if ( lhs->tv_sec < rhs->tv_sec )
    return -1;
  if ( lhs->tv_sec > rhs->tv_sec )
    return 1;
  return lhs->tv_nsec - rhs->tv_nsec;
}
#endif


#if !(defined HAVE_DECL_JIFFIES_TO_MSECS && HAVE_DECL_JIFFIES_TO_MSECS)
// Convert jiffies to milliseconds
static inline unsigned int jiffies_to_msecs(const unsigned long j)
{
#if HZ <= 1000 && !(1000 % HZ)
  return (1000 / HZ) * j;
#elif HZ > 1000 && !(HZ % 1000)
  return (j + (HZ / 1000) - 1)/(HZ / 1000);
#else
  return (j * 1000) / HZ;
#endif
}
#endif

//
// Implement mising fucntions
//
#if !(defined HAVE_DECL_SET_NLINK && HAVE_DECL_SET_NLINK)
static inline void set_nlink(struct inode* i, unsigned int nlink){ i->i_nlink = nlink; }
#endif
#if !(defined HAVE_DECL_DROP_NLINK && HAVE_DECL_DROP_NLINK)
static inline void drop_nlink(struct inode* i){ i->i_nlink--; }
#endif
#if !(defined HAVE_DECL_INC_NLINK && HAVE_DECL_INC_NLINK)
static inline void inc_nlink(struct inode* i){ i->i_nlink++; }
#endif

//
// Simple wrapper for 'writeback_inodes_sb_if_idle'
//
#if defined HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V1 && HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V1
  #define Writeback_inodes_sb_if_idle(s) writeback_inodes_sb_if_idle( (s) )
#elif defined HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V2 && HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V2
  #define Writeback_inodes_sb_if_idle(s) writeback_inodes_sb_if_idle( (s), WB_REASON_FREE_MORE_MEM )
#endif


//
// Memory allocation routines.
// Debug version of memory allocation/deallocation routine performs
// detection of memory leak/overwrite
//
#if defined UFSD_DEBUG_ALLOC

typedef struct _MEMBLOCK_HEAD {
    struct list_head Link;
    unsigned int  AllocatedSize;
    unsigned int  Seq;
    unsigned int  DataSize;
    unsigned char Barrier[64 - 3*sizeof(int) - sizeof(struct list_head)];

  /*
     Offset  0x40
     |---------------------|
     | Requested memory of |
     |   size 'DataSize'   |
     |---------------------|
  */
  //unsigned char barrier2[64 - 3*sizeof(int) - sizeof(struct list_head)];

} MEMBLOCK_HEAD;

static size_t TotalKmallocs;
static size_t TotalVmallocs;
static size_t UsedMemMax;
static size_t TotalAllocs;
static size_t TotalAllocBlocks;
static size_t TotalAllocSequence;
static size_t MemMaxRequest;
static LIST_HEAD(TotalAllocHead);
static struct Mutex MemMutex;


///////////////////////////////////////////////////////////
// TraceMemReport
//
// Helper function to trace memory usage information
///////////////////////////////////////////////////////////
static void
TraceMemReport(
    IN int OnExit
    )
{
  size_t Mb = UsedMemMax/(1024*1024);
  size_t Kb = (UsedMemMax%(1024*1024)) / 1024;
  size_t b  = UsedMemMax%1024;
  if ( 0 != Mb ) {
    DebugTrace(0, UFSD_LEVEL_ERROR, ("Memory report: Peak usage %Zu.%03Zu Mb (%Zu bytes), kmalloc %Zu, vmalloc %Zu\n",
                  Mb, Kb, UsedMemMax, TotalKmallocs, TotalVmallocs ) );
  } else {
    DebugTrace(0, UFSD_LEVEL_ERROR, ("Memory report: Peak usage %Zu.%03Zu Kb (%Zu bytes),  kmalloc %Zu, vmalloc %Zu\n",
                  Kb, b, UsedMemMax, TotalKmallocs, TotalVmallocs ) );
  }
  DebugTrace(0, UFSD_LEVEL_ERROR, ("%s:  %Zu bytes in %Zu blocks, Max request %Zu bytes\n",
                OnExit? "Leak":"Total allocated", TotalAllocs, TotalAllocBlocks, MemMaxRequest ) );
}


///////////////////////////////////////////////////////////
// UFSD_HeapAlloc
//
// Debug version of memory allocation routine
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
UFSD_HeapAlloc(
    IN size_t Size
    )
{
  MEMBLOCK_HEAD* head;
  int Use_kmalloc;
  // Overhead includes private information and two barriers to check overwriting
  const size_t TheOverhead = sizeof(MEMBLOCK_HEAD) + sizeof(head->Barrier);
  size_t AllocatedSize = Size + TheOverhead;

  if ( AllocatedSize <= PAGE_SIZE ) {
    Use_kmalloc = 1;
    // size_t align
    AllocatedSize = (AllocatedSize + sizeof(size_t)-1) & ~(sizeof(size_t)-1);
    head = (MEMBLOCK_HEAD*)kmalloc( AllocatedSize, GFP_NOFS );
  } else {
    Use_kmalloc = 0;
    AllocatedSize = PAGE_ALIGN(AllocatedSize);
    head = (MEMBLOCK_HEAD*)vmalloc( AllocatedSize );
    assert( (size_t)head >= VMALLOC_START && (size_t)head < VMALLOC_END );
#ifdef UFSD_DEBUG
    if ( (size_t)head < VMALLOC_START || (size_t)head >= VMALLOC_END )
      _UFSDTrace( "vmalloc(%Zu) returns %p. Must be in range [%lx, %lx)\n", AllocatedSize, head, (long)VMALLOC_START, (long)VMALLOC_END );
#endif
  }

  assert(NULL != head);
  if ( NULL == head ) {
    DebugTrace(0, UFSD_LEVEL_ERROR, ("HeapAlloc(%Zu) failed\n", Size));
    return NULL;
  }
  assert(0 == (AllocatedSize & 1U));

  Mutex_lock( &MemMutex );

  // Fill head private fields
  head->AllocatedSize = Use_kmalloc? AllocatedSize : (AllocatedSize | 1);
  head->DataSize      = Size;
  list_add( &head->Link, &TotalAllocHead );
  head->Seq           = ++TotalAllocSequence;

  //
  // fills two barriers to check memory overwriting
  //
  memset( &head->Barrier[0], 0xde, sizeof(head->Barrier) );
  memset( Add2Ptr( head + 1, Size), 0xed, sizeof(head->Barrier) );

  //
  // Update statistics
  //
  Use_kmalloc? ++TotalKmallocs : ++TotalVmallocs;
  TotalAllocs    += Size;
  if( TotalAllocs > UsedMemMax )
    UsedMemMax = TotalAllocs;
  TotalAllocBlocks += 1;
  if ( Size > MemMaxRequest )
    MemMaxRequest = Size;

  DebugTrace(0, UFSD_LEVEL_MEMMNGR, ("alloc(%Zu) -> %p%s, seq=%u\n",
                Size, head+1, Use_kmalloc? "" : "(v)", head->Seq));

  Mutex_unlock( &MemMutex );
  return head + 1;
}


///////////////////////////////////////////////////////////
// UFSD_HeapFree
//
// Debug version of memory deallocation routine
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_HeapFree(
    IN void* Pointer
    )
{
  MEMBLOCK_HEAD* block;

  if ( NULL == Pointer )
    return;

  Mutex_lock( &MemMutex );

#if 1
  // Fast but unsafe find
  block = (MEMBLOCK_HEAD*)Pointer - 1;
#else
  // Safe but slow find
  {
    struct list_head* pos;
    list_for_each( pos, &TotalAllocHead )
    {
      block = list_entry( pos, MEMBLOCK_HEAD, Link );
      if ( Pointer == (void*)(block + 1) )
        goto Found;
    }
  }
  assert( !"failed to find block" );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("HeapFree(%p) failed to find block\n", Pointer ));
  Mutex_unlock( &MemMutex );
  return;
Found:
#endif

  // Verify barrier
  {
    unsigned char* p;
    size_t i = sizeof(block->Barrier);
    char* Err = NULL;
    for ( p = &block->Barrier[0]; 0 != i; p++, i-- ) {
      if ( *p != 0xde ) {
        Err = "head";
BadNews:
        TRACE_ONLY( UFSD_TraceLevel = -1; )
        DebugTrace(0, UFSD_LEVEL_ERROR, ("**** Allocated %u seq %u DataSize %u\n",
                   block->AllocatedSize, block->Seq, block->DataSize ));
        DebugTrace(0, UFSD_LEVEL_ERROR, ("**** HeapFree(%p) %s barrier failed at 0x%Zx\n", Pointer, Err, PtrOffset( block, p ) ));
        UFSDAPI_DumpMemory( block, 512 );
        TRACE_ONLY( UFSD_TraceLevel = 0; )
        BUG_ON(1);
      }
    }

    i = sizeof(block->Barrier);
    for ( p = Add2Ptr( block + 1, block->DataSize ); 0 != i; p++, i-- ) {
      if ( *p != 0xed ) {
        Err = "tail";
        goto BadNews;
      }
    }
  }

  list_del( &block->Link );

  //
  // Update statistics
  //
  TotalAllocs -= block->DataSize;
  TotalAllocBlocks -= 1;
  Mutex_unlock( &MemMutex );
  DebugTrace(0, UFSD_LEVEL_MEMMNGR, ("free(%p, %u) seq=%u\n", block + 1, block->DataSize, block->Seq));

  memset( block + 1, 0xcc, block->DataSize );

  // declaration of vfree and kfree differs!
  if ( block->AllocatedSize & 1U )
    vfree( block );
  else
    kfree( block );
}

#else

///////////////////////////////////////////////////////////
// UFSD_HeapAlloc
//
// Release version of memory allocation routine
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
UFSD_HeapAlloc(
    IN size_t Size
    )
{
  void* ptr;
  if ( Size <= PAGE_SIZE ) {
    ptr = kmalloc(Size, GFP_NOFS);
  } else {
    ptr = vmalloc( Size );
    assert( (size_t)ptr >= VMALLOC_START && (size_t)ptr < VMALLOC_END );
  }

  assert(NULL != ptr);
  if ( NULL == ptr ) {
    DebugTrace(0, UFSD_LEVEL_ERROR, ("alloc(%Zu) failed\n", Size));
    return NULL;
  }

  DebugTrace(0, UFSD_LEVEL_MEMMNGR, ("alloc(%Zu) -> %p%s\n", Size, ptr, Size <= PAGE_SIZE?"" : "(v)" ));
  return ptr;
}


///////////////////////////////////////////////////////////
// UFSD_HeapFree
//
// Release version of memory deallocation routine
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_HeapFree(
    IN void* ptr
    )
{
  if ( NULL != ptr ) {
    DebugTrace(0, UFSD_LEVEL_MEMMNGR, ("HeapFree(%p)\n", ptr));
    if ( (size_t)ptr >= VMALLOC_START && (size_t)ptr < VMALLOC_END ) {
      // This memory was allocated via vmalloc
      vfree(ptr);
    } else {
      // This memory was allocated via kmalloc
      kfree(ptr);
    }
  }
}

#endif // #ifndef UFSD_DEBUG_ALLOC


//
// 'kmem_cache_create' too often crashes at least in 2.6.22
//
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
  #define UFSD_USE_KMEM_CACHE
#endif

///////////////////////////////////////////////////////////
// UFSD_CacheCreate
//
//
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
UFSD_CacheCreate(
    IN const char*  Name,
    IN size_t       size
    )
{
#ifdef UFSD_USE_KMEM_CACHE
  u_kmem_cache* cache = cache = kmem_cache_create( Name, size, 0, SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|SLAB_PANIC, NULL
#if defined HAVE_DECL_KMEM_CACHE_CREATE_V1 && HAVE_DECL_KMEM_CACHE_CREATE_V1
                                          , NULL
#endif
                                          );
#else
  void* cache = (void*)size;
  UNREFERENCED_PARAMETER( Name );
#endif

  DebugTrace(0, Dbg, ("Cache create: \"%s\" (%Zx) -> %p\n", Name, size, cache ) );
  return cache;
}


///////////////////////////////////////////////////////////
// UFSD_CacheDestroy
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_CacheDestroy(
    IN void* Cache
    )
{
  DebugTrace(0, Dbg, ("Cache destroy: %p \n", Cache ) );
#ifdef UFSD_USE_KMEM_CACHE
  kmem_cache_destroy( (u_kmem_cache*)Cache );
#else
  UNREFERENCED_PARAMETER( Cache );
#endif
}


///////////////////////////////////////////////////////////
// UFSD_CacheAlloc
//
//
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
UFSD_CacheAlloc(
    IN void*  Cache,
    IN int    bZero
    )
{
#ifdef UFSD_USE_KMEM_CACHE
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
  void* p = kmem_cache_alloc( (u_kmem_cache*)Cache, bZero? (__GFP_ZERO | GFP_KERNEL) : GFP_KERNEL );
#elif defined HAVE_DECL_KMEM_CACHE_ZALLOC && HAVE_DECL_KMEM_CACHE_ZALLOC
  void* p = (bZero? kmem_cache_zalloc : kmem_cache_alloc)( (u_kmem_cache*)Cache, GFP_KERNEL );
#else
  void* p = kmem_cache_alloc( (u_kmem_cache*)Cache, GFP_KERNEL );
  if ( NULL != p && bZero )
    memset( p, 0, kmem_cache_size( (u_kmem_cache*)Cache ) );
#endif
#else
  void* p = kmalloc( (size_t)Cache, GFP_KERNEL );
  if ( NULL != p && bZero )
    memset( p, 0, (size_t)Cache );
#endif
  DebugTrace(0, UFSD_LEVEL_MEMMNGR, ("CacheAlloc(%p)->%p\n", Cache, p ) );
  return p;
}


///////////////////////////////////////////////////////////
// UFSD_CacheFree
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_CacheFree(
    IN void* Cache,
    IN void* p
    )
{
  DebugTrace(0, UFSD_LEVEL_MEMMNGR, ("CacheFree(%p,%p)\n", Cache, p ) );
#ifdef UFSD_USE_KMEM_CACHE
  kmem_cache_free( (u_kmem_cache*)Cache, p );
#else
  UNREFERENCED_PARAMETER( Cache );
  kfree( p );
#endif
}


//
// NLS support routines requiring
// access to kernel-dependent nls_table structure.
//

///////////////////////////////////////////////////////////
// UFSD_BCSToUni
//
// Converts multibyte string to UNICODE string
// Returns the length of destination string in wide symbols
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
UFSD_BCSToUni(
    OUT unsigned short*       ws,       // Destination UNICODE string
    IN  int                   max_out,  // Maximum UNICODE characters in ws
    IN  const unsigned char*  s,        // Source BCS string
    IN  int                   len,      // The length of BCS strings in bytes
    IN  struct nls_table*     nls       // Code pages
    )
{
#ifdef UFSD_USE_NLS
  int ret   = 0;
  int len0  = len;

  for ( ;; ) {

    int charlen;
    wchar_t wc;

    if ( len <= 0 || 0 == *s )
      return ret; // The only correct way to exit

    if ( max_out <= 0 ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("A2U: too little output buffer\n" ) );
      return ret;
    }

    wc      = *ws;
    charlen = nls->char2uni( s, len, &wc );

    if ( charlen <= 0 ){
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("char2uni (%s) failed:\n", nls->charset ) );
      printk( KERN_NOTICE  QUOTED_UFSD_DEVICE": %s failed to convert '%.*s' to unicode. Pos %d, chars %x %x %x\n",
              nls->charset, len0, s - (len0-len), len0-len, (int)s[0], len > 1? (int)s[1] : 0, len > 2? (int)s[2] : 0 );
      return 0;
    }

    *ws++    = (unsigned short)wc;
    ret    += 1;
    max_out -= 1;
    len     -= charlen;
    s       += charlen;
  }

#else

  UNREFERENCED_PARAMETER( max_out );
  UNREFERENCED_PARAMETER( s );
  UNREFERENCED_PARAMETER( len );
  UNREFERENCED_PARAMETER( nls );
  *ws = 0;
  return 0;

#endif
}


///////////////////////////////////////////////////////////
// UFSD_UniToBCS
//
// Converts UNICODE string to multibyte
// Returns the length of destination string in chars
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
UFSD_UniToBCS(
    OUT unsigned char         *s,        // Destination BCS string
    IN  int                   max_out,  // Maximum bytes in BCS string
    IN  const unsigned short  *ws,       // Source UNICODE string
    IN  int                   len,      // The length of UNICODE string
    IN  struct nls_table      *nls       // Code pages
   )
{
#ifdef UFSD_USE_NLS
  unsigned char *s0 = s;

  for ( ;; ) {

    int charlen;

    if ( len <= 0 || 0 == *ws )
      return (int)(s - s0); // The only correct way to exit

    if ( max_out <= 0 ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("U2A: too little output buffer\n" ) );
      return (int)(s - s0);
    }

    charlen = nls->uni2char( *ws, s, max_out );
    if ( charlen <= 0 ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("uni2char (%s) failed:\n", nls->charset ));
      assert( !"U2A: failed to convert" );
      printk( KERN_NOTICE  QUOTED_UFSD_DEVICE": %s failed to convert from unicode. Pos %d, chars %x %x %x\n",
              nls->charset, (int)(s-s0), (unsigned)ws[0], len > 1? (unsigned)ws[1] : 0, len > 2? (unsigned)ws[2] : 0 );
      return 0;
    }

    ws      += 1;
    len     -= 1;
    max_out -= charlen;
    s       += charlen;
  }

#else

  UNREFERENCED_PARAMETER( max_out );
  UNREFERENCED_PARAMETER( ws );
  UNREFERENCED_PARAMETER( len );
  UNREFERENCED_PARAMETER( nls );
  *s = 0;
  return 0;

#endif
}


#ifdef UFSD_USE_NLS
///////////////////////////////////////////////////////////
// UFSD_unload_nls
//
//
///////////////////////////////////////////////////////////
static void
UFSD_unload_nls(
    IN mount_options* opts
    )
{
  int cp;
  for ( cp = 0; cp < opts->nls_count; cp++ ){
    if ( NULL != opts->nls[cp] )
      unload_nls( opts->nls[cp] );
    opts->nls[cp] = NULL;
  }
  opts->nls_count = 0;
}
#else
  #define UFSD_unload_nls( o )
#endif // #ifdef UFSD_USE_NLS


//
// Device IO functions.
//

#if defined HAVE_DECL_KDEV_T_S_DEV && HAVE_DECL_KDEV_T_S_DEV
  #define sb_dev(sb) ((sb)->s_dev)
  #define bh_dev(bh) ((bh)->b_dev)
#else
  #define sb_dev(sb) ((sb)->s_bdev)
  #define bh_dev(bh) ((bh)->b_bdev)
#endif

#if !(defined HAVE_DECL___BRELSE & HAVE_DECL___BRELSE)
  #define __brelse  brelse
#endif

#if !(defined HAVE_DECL___BREAD & HAVE_DECL___BREAD)
  #define __bread   bread
#endif

#if !(defined HAVE_DECL___GETBLK & HAVE_DECL___GETBLK)
  #define __getblk  getblk
#endif

#if !(defined HAVE_DECL_SET_BUFFER_UPTODATE & HAVE_DECL_SET_BUFFER_UPTODATE)
  #define set_buffer_uptodate(bh) mark_buffer_uptodate(bh, 1);
#endif

#if !(defined HAVE_DECL_SET_BUFFER_ORDERED & HAVE_DECL_SET_BUFFER_ORDERED)
  #define set_buffer_ordered( bh )
  #define clear_buffer_ordered( bh )
#endif

#if !(defined HAVE_DECL___BREADAHEAD & HAVE_DECL___BREADAHEAD)
static inline void __breadahead(dev_t dev, UINT64 block, int size)
{
  struct buffer_head *bh = getblk(dev, block, size);
  ll_rw_block(READA, 1, &bh);
  brelse(bh);
}
#endif

#if !defined PageUptodate && defined Page_Uptodate
  #define PageUptodate(x) Page_Uptodate(x)
#endif

#ifndef page_buffers
  #define page_has_buffers(page) (NULL != page->buffers)
  #define page_buffers(page) page->buffers
#endif

#ifdef UFSD_HFS
///////////////////////////////////////////////////////////
// bh_tail
//
// Get buffer_head for tail
///////////////////////////////////////////////////////////
struct buffer_head*
bh_tail(
    IN struct super_block* sb,
    IN size_t              Bytes2Skip
    )
{
  usuper* sbi = UFSD_SB( sb );
  struct buffer_head* bh = sbi->TailBh;
  if ( NULL == bh ) {
    sector_t TailBlock = ((sbi->MaxBlock << sb->s_blocksize_bits) + Bytes2Skip) >> 9;
    struct page* page = alloc_page( GFP_KERNEL | __GFP_ZERO );
    if ( NULL == page )
      return NULL;
    bh = alloc_buffer_head( GFP_NOFS );
    if ( NULL == bh ) {
out:
      __free_page( page );
      return NULL;
    }

    bh->b_state = 0;
    init_buffer( bh, end_buffer_read_sync, NULL );
    atomic_set( &bh->b_count, 2 );
    set_bh_page( bh, page, Bytes2Skip );
    bh->b_size    = 512;
    bh->b_bdev    = sb_dev(sb);
    bh->b_blocknr = TailBlock;
    set_buffer_mapped( bh );
    lock_buffer( bh );
    submit_bh( READ, bh );
    wait_on_buffer( bh );
    if ( !buffer_uptodate( bh ) ) {
      brelse( bh );
      goto out;
    }
    assert( 1 == atomic_read( &bh->b_count ) );
    sbi->TailBh = bh;
//    DebugTrace(0, 0, ("bh_tail\n"));
  }
  get_bh( bh );
  return bh;
}
#endif


///////////////////////////////////////////////////////////
// UFSD_BdRead
//
// Read data from block device
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
UFSD_BdRead(
    IN  struct super_block* sb,
    IN  UINT64  Offset,
    IN  size_t  Bytes,
    OUT void*   Buffer
   )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb_dev(sb))
  //
  usuper*   sbi         = UFSD_SB( sb );
  sector_t  DevBlock    = (sector_t)(Offset >> sb->s_blocksize_bits);
  size_t    Bytes2Skip  = (size_t)Offset & (sb->s_blocksize - 1); // Offset % sb->s_blocksize

  int ReadAhead     = 0;
  int RaBlocks      = sbi->ReadAheadBlocks;
  sector_t RaBlock  = DevBlock;
  unsigned err      = 0;

  DebugTrace(+1, UFSD_LEVEL_IO, ("BdRead: %p, %"PSCT"x, %Zx, %p\n", sb, DevBlock, Bytes, Buffer));

  ProfileEnter( sbi, bdread );

  while ( 0 != Bytes ) {

    size_t ToRead;
    struct buffer_head* bh;

    while( ReadAhead++ < RaBlocks && RaBlock < sbi->MaxBlock )
      __breadahead( sb_dev(sb), RaBlock++, sb->s_blocksize );
    ReadAhead -= 1;

#ifdef UFSD_HFS
    if ( DevBlock == sbi->MaxBlock ) {
      assert( 512 == Bytes );
      bh = bh_tail( sb, Bytes2Skip );
      Bytes2Skip = 0;
    } else
#endif
    {
      TRACE_ONLY( if ( 0 != Bytes2Skip || Bytes < sb->s_blocksize ) sbi->nReadBlocksNa += 1; )

      bh = __bread( sb_dev(sb), DevBlock, sb->s_blocksize );
    }

    if ( NULL == bh ) {
      assert( !"BdRead: failed to map block" );
      printk( KERN_CRIT QUOTED_UFSD_DEVICE ":failed to read block 0x%"PSCT"x (max=%llx)\n", DevBlock, sbi->MaxBlock);
      DebugTrace(0, UFSD_LEVEL_ERROR, ("BdRead: failed to map block starting from %"PSCT"x, %llx\n", DevBlock, sbi->MaxBlock));
      err = -EIO;
      goto out;
    }

    TRACE_ONLY( sbi->nReadBlocks += 1; )

    ToRead = sb->s_blocksize - Bytes2Skip;
    if ( ToRead > Bytes )
      ToRead = Bytes;

#if !defined UFSD_AVOID_COPY_PAGE && defined HAVE_DECL_COPY_PAGE && HAVE_DECL_COPY_PAGE
    if ( likely(PAGE_SIZE == ToRead) )
    {
      assert( 0 == Bytes2Skip );
      assert( 0 == ((size_t)Buffer & 0x3f) );
      copy_page( Buffer, bh->b_data );
    }
    else
#endif
      memcpy( Buffer, bh->b_data + Bytes2Skip, ToRead );

    __brelse( bh );

    Buffer      = Add2Ptr( Buffer, ToRead );
    DevBlock   += 1;
    Bytes      -= ToRead;
    Bytes2Skip  = 0;
  }

out:
  ProfileLeave( sbi, bdread );

#ifdef UFSD_TRACE
  if ( UFSD_TraceLevel & UFSD_LEVEL_IO )
    UFSD_TraceInc( -1 );
#endif
//  DebugTrace(-1, UFSD_LEVEL_IO, ("BdRead -> ok\n"));
  return err;
}


///////////////////////////////////////////////////////////
// BdSync
//
//
///////////////////////////////////////////////////////////
static inline int
BdSync(
    IN struct super_block*  sb,
    IN struct buffer_head*  bh,
    IN size_t Wait
    )
{
  int err = 0;
  if ( Wait ) {
#ifdef WRITE_FLUSH_FUA
    if ( Wait & UFSD_RW_WAIT_BARRIER ) {
      err = __sync_dirty_buffer( bh, WRITE_SYNC | WRITE_FLUSH_FUA );
      if ( 0 != err ) {
        DebugTrace(0, UFSD_LEVEL_ERROR, ("BdSync: \"__sync_dirty_buffer( bh, WRITE_SYNC | WRITE_FLUSH_FUA )\" failed -> %d\n", err ));
      }
    } else {
      err = sync_dirty_buffer( bh );
      if ( 0 != err ) {
        DebugTrace(0, UFSD_LEVEL_ERROR, ("BdSync: \"sync_dirty_buffer( bh )\" failed -> %d\n", err ));
      }
    }
#elif defined WRITE_BARRIER
    if ( Wait & UFSD_RW_WAIT_BARRIER )
      set_buffer_ordered( bh );

    err = sync_dirty_buffer( bh );

    if ( Wait & UFSD_RW_WAIT_BARRIER )
      clear_buffer_ordered( bh );

    if ( 0 != err ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("BdSync: %s \"sync_dirty_buffer( bh )\" failed -> %d\n", Wait & UFSD_RW_WAIT_BARRIER? "ordered":"noordered", err ));
    }
#else
    ll_rw_block( WRITE, 1, &bh ); // TODO: do SG IO.
    wait_on_buffer( bh );
#endif
  }

#ifdef WRITE_BARRIER
  if ( -EOPNOTSUPP == err && (Wait & UFSD_RW_WAIT_BARRIER) ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": disabling barriers on %s - not supported\n", UFSD_BdGetName( sb ) );
    UFSD_SB( sb )->options.nobarrier = 1;

    // And try again, without the barrier
    set_buffer_uptodate( bh );
    set_buffer_dirty( bh );
    err = sync_dirty_buffer( bh );

    if ( 0 != err ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("BdSync: nobarrier \"sync_dirty_buffer( bh )\" failed -> %d\n", err ));
    }
  }
#else
  UNREFERENCED_PARAMETER( sb );
#endif

  return err;
}


///////////////////////////////////////////////////////////
// UFSD_BdWrite
//
// Write data to block device
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
UFSD_BdWrite(
    IN struct super_block*  sb,
    IN UINT64       Offset,
    IN size_t       Bytes,
    IN const void*  Buffer,
    IN size_t       Wait
   )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb_dev(sb))
  //
  usuper*   sbi         = UFSD_SB( sb );
  sector_t  DevBlock    = (sector_t)(Offset >> sb->s_blocksize_bits);
  size_t    Bytes2Skip  = (size_t)Offset & (sb->s_blocksize - 1); // Offset % sb->s_blocksize
  unsigned  err         = 0;
  if ( !Wait && FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    Wait = UFSD_RW_WAIT_SYNC;

  DebugTrace(+1, UFSD_LEVEL_IO, ("BdWrite: %p, %"PSCT"x, %Zx, %p%s\n", sb, DevBlock, Bytes, Buffer, Wait?", wait":""));

  ProfileEnter( sbi, bdwrite );

  while ( 0 != Bytes ) {

    size_t ToWrite;
    struct buffer_head* bh;

#ifdef UFSD_HFS
    if ( DevBlock == sbi->MaxBlock ) {
      assert( Bytes == 512 )
      bh = bh_tail( sb, Bytes2Skip );
      Bytes2Skip = 0;
    } else
#endif
    {
      TRACE_ONLY( if ( 0 != Bytes2Skip || Bytes < sb->s_blocksize ) sbi->nWrittenBlocksNa += 1; )

      bh = ( 0 != Bytes2Skip || Bytes < sb->s_blocksize ? __bread : __getblk )( sb_dev(sb), DevBlock, sb->s_blocksize );
    }

    if ( NULL == bh ) {
      assert( !"BdWrite: failed to map block" );
      printk( KERN_CRIT QUOTED_UFSD_DEVICE ":failed to write block 0x%"PSCT"x (max=%llx)\n", DevBlock, sbi->MaxBlock );
      DebugTrace(0, UFSD_LEVEL_ERROR, ("BdWrite: failed to map block starting from %"PSCT"x, %llx\n", DevBlock, sbi->MaxBlock));
      err = -EIO;
      goto out;
    }

    if ( buffer_locked( bh ) )
      __wait_on_buffer( bh );

    ToWrite = sb->s_blocksize - Bytes2Skip;
    if ( ToWrite > Bytes )
      ToWrite = Bytes;

    if ( unlikely( PIN_BUFFER == Buffer ) ) {
      //
      // Process Pin request
      //
      TRACE_ONLY( sbi->nPinBlocks += 1; )
      goto Next;  // skip __brelse

    } else if ( unlikely( UNPIN_BUFFER == Buffer ) ) {
      //
      // Process Unpin request
      //
      assert( atomic_read( &bh->b_count ) >= 2 );
      TRACE_ONLY( sbi->nUnpinBlocks += 1; )
      __brelse( bh ); // double __brelse

    } else {

      //
      // Update buffer with user data
      //
      lock_buffer( bh );
      if ( unlikely( MINUS_ONE_BUFFER == Buffer ) )
        memset( bh->b_data + Bytes2Skip, -1, ToWrite );
      else {
#if !defined UFSD_AVOID_COPY_PAGE && defined HAVE_DECL_COPY_PAGE && HAVE_DECL_COPY_PAGE
        if ( likely(PAGE_SIZE == ToWrite) )
        {
          assert( 0 == Bytes2Skip );
          assert( 0 == ((size_t)Buffer & 0x3f) );
          copy_page( bh->b_data, (void*)Buffer ); // copy_page requires source page as non const!
        }
        else
#endif
          memcpy( bh->b_data + Bytes2Skip, Buffer, ToWrite );
        Buffer  = Add2Ptr( Buffer, ToWrite );
      }
      set_buffer_uptodate( bh );
      mark_buffer_dirty( bh );
      unlock_buffer( bh );

      TRACE_ONLY( sbi->nWrittenBlocks += 1; )

      if ( Wait ) {
#ifdef UFSD_DEBUG
        if ( !(UFSD_TraceLevel & UFSD_LEVEL_IO) )
          DebugTrace(0, UFSD_LEVEL_VFS, ("BdWrite(wait)\n"));
#endif

        if ( sbi->options.nobarrier )
          Wait &= ~UFSD_RW_WAIT_BARRIER;

        err = BdSync( sb, bh, Wait );

        if ( 0 != err ) {
          assert( !"BdWrite: failed to write block" );
          DebugTrace(0, UFSD_LEVEL_ERROR, ("BdWrite: failed to write block starting from %"PSCT"x, %llx, error=%d\n", DevBlock, sbi->MaxBlock, err));
          __brelse( bh );
          goto out;
        }
      }
    }

    __brelse( bh );

Next:
    DevBlock    += 1;
    Bytes       -= ToWrite;
    Bytes2Skip   = 0;
  }

out:
  ProfileLeave( sbi, bdwrite );

#ifdef UFSD_TRACE
  if ( UFSD_TraceLevel & UFSD_LEVEL_IO )
    UFSD_TraceInc( -1 );
#endif
//  DebugTrace(-1, UFSD_LEVEL_IO, ("BdWrite -> ok\n"));
  return err;
  UNREFERENCED_PARAMETER( sbi );
}


///////////////////////////////////////////////////////////
// UFSD_BdMap
//
//
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
UFSD_BdMap(
    IN  struct super_block* sb,
    IN  UINT64  Offset,
    IN  size_t  Bytes,
    IN  size_t  Flags,
    OUT struct buffer_head** Bcb,
    OUT void**  Mem
    )
{
  struct buffer_head* bh;
#if defined UFSD_TRACE || defined UFSD_HFS || !defined UFSD_USE_BUILTIN_ZEROING
  usuper* sbi = UFSD_SB( sb );
#endif
  unsigned int BlockSize  = sb->s_blocksize;
  sector_t  DevBlock      = (sector_t)(Offset >> sb->s_blocksize_bits);
  size_t Bytes2Skip       = (size_t)(Offset & (BlockSize - 1)); // Offset % sb->s_blocksize
  TRACE_ONLY( const char* hint );
  TRACE_ONLY( const char* hint2 = "" );

  if ( Bytes2Skip + Bytes > BlockSize ) {
    DebugTrace(0, UFSD_LEVEL_ERROR, ("BdMap: [%llx %Zx] overlaps block boundary %x\n", Offset, Bytes, BlockSize));
    return -EINVAL;
  }

  ProfileEnter( sbi, bdmap );

#ifdef UFSD_HFS
  if ( DevBlock == sbi->MaxBlock ) {
    assert( Bytes == 512 )
    bh = bh_tail( sb, Bytes2Skip );
    Bytes2Skip = 0;
    TRACE_ONLY( hint = "tail " );
  } else
#endif
  {
    if ( 0 == Bytes2Skip && Bytes == BlockSize && FlagOn( Flags, UFSD_RW_MAP_NO_READ ) ) {
      TRACE_ONLY( hint = "g " );
      bh = __getblk( sb_dev(sb), DevBlock, BlockSize );
      if ( NULL != bh )
        set_buffer_uptodate( bh );
    } else {
      //
      // Do not readahead implicitly. cause BdZero
      //
#ifndef UFSD_USE_BUILTIN_ZEROING
      int RaBlocks      = sbi->ReadAheadBlocks;
      sector_t RaBlock  = DevBlock;
      while( 0 != RaBlocks-- ) {
        if ( RaBlock >= sbi->MaxBlock )
          break;
        __breadahead( sb_dev(sb), RaBlock++, BlockSize );
      }
#endif

      TRACE_ONLY( hint = "b " );
      bh = __bread( sb_dev(sb), DevBlock, BlockSize );
    }
  }

  ProfileLeave( sbi, bdmap );

  if ( NULL == bh ) {
    assert( !"BdMap: failed to map block" );
    DebugTrace(0, UFSD_LEVEL_ERROR, ("BdMap: failed to map block %"PSCT"x, max = %llx\n", DevBlock, sbi->MaxBlock));
    return -EIO;
  }

  if ( buffer_locked( bh ) ) {
    TRACE_ONLY( hint2 = " w"; )
    __wait_on_buffer( bh );
  }

  DebugTrace(0, UFSD_LEVEL_IO, ("BdMap: %p, %"PSCT"x, %Zx, %s%s%s -> %p (%d)\n", sb, DevBlock, Bytes,
              hint, buffer_dirty(bh)?"d":"c", hint2, bh, atomic_read( &bh->b_count ) ));

  //
  // Return pointer into page
  //
  *Mem = Add2Ptr( bh->b_data, Bytes2Skip );
  *Bcb = bh;
  DEBUG_ONLY( sbi->nMappedBh += 1; )
  return 0;
}


///////////////////////////////////////////////////////////
// UFSD_BdUnMap
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_BdUnMap(
#ifdef UFSD_DEBUG
    IN struct super_block* sb,
#endif
    IN struct buffer_head* bh
    )
{
  assert( NULL != bh );

  DebugTrace(0, UFSD_LEVEL_IO, ("BdUnMap: %"PSCT"x,%s %d\n", bh->b_blocknr, buffer_dirty(bh)?"d":"c", atomic_read( &bh->b_count ) - 1 ));
  __brelse( bh );

  DEBUG_ONLY( UFSD_SB( sb )->nUnMapped += 1; )
}


///////////////////////////////////////////////////////////
// UFSD_BdSetDirty
//
//
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
UFSD_BdSetDirty(
    IN struct super_block* sb,
    IN struct buffer_head* bh,
    IN size_t   Wait
    )
{
  int err = 0;
  usuper* sbi = UFSD_SB( sb );

  if ( !Wait && FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    Wait = UFSD_RW_WAIT_SYNC;

  if ( Wait && sbi->options.nobarrier )
    Wait &= ~UFSD_RW_WAIT_BARRIER;

  assert( NULL != bh );

  DebugTrace(0, UFSD_LEVEL_IO, ("BdDirty: %"PSCT"x,%s %d\n", bh->b_blocknr, buffer_dirty(bh)?"d":"c", atomic_read( &bh->b_count ) ));
  set_buffer_uptodate( bh );
  mark_buffer_dirty( bh );

  if ( Wait ) {
    ProfileEnter( sbi, bdsetdirty );
    err = BdSync( sb, bh, Wait );
    ProfileLeave( sbi, bdsetdirty );
  }

  return err;
}


///////////////////////////////////////////////////////////
// UFSD_BdLockBuffer
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_BdLockBuffer(
    IN struct buffer_head* bh
    )
{
  assert( NULL != bh );
  assert( !buffer_locked( bh ) );
  lock_buffer( bh );
}


///////////////////////////////////////////////////////////
// UFSD_BdUnLockBuffer
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_BdUnLockBuffer(
    IN struct buffer_head* bh
    )
{
  assert( NULL != bh );
  assert( buffer_locked( bh ) );
  set_buffer_uptodate( bh );
  unlock_buffer( bh );
}


///////////////////////////////////////////////////////////
// UFSD_BdDiscard
//
// Issue a discard request (trim for SSD)
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
UFSD_BdDiscard(
    IN struct super_block*  sb,
    IN UINT64 Offset,
    IN size_t Bytes
    )
{
#if defined HAVE_DECL_BLKDEV_ISSUE_DISCARD && HAVE_DECL_BLKDEV_ISSUE_DISCARD
  int err;
  DebugTrace(+1, UFSD_LEVEL_IO, ("BdDiscard: %p, %llx, %Zx\n", sb, Offset, Bytes));
  err = blkdev_issue_discard( sb->s_bdev, Offset >> 9, Bytes >> 9, GFP_NOFS, 0 );
  if ( -EOPNOTSUPP == err ) {
    DebugTrace(-1, UFSD_LEVEL_IO, ("BdDiscard -> not supported\n"));
    return -EOPNOTSUPP;
  }

  if ( 0 != err ) {
    DebugTrace(-1, UFSD_LEVEL_IO, ("BdDiscard -> failed %d\n", err));
  } else {
#ifdef UFSD_TRACE
    if ( UFSD_TraceLevel & UFSD_LEVEL_IO )
      UFSD_TraceInc( -1 );
#endif
  }

  return err;
#else
  UNREFERENCED_PARAMETER( sb );
  UNREFERENCED_PARAMETER( Offset );
  UNREFERENCED_PARAMETER( Bytes );

  DebugTrace(0, UFSD_LEVEL_IO, ("BdDiscard -> not supported\n"));
  return -EOPNOTSUPP;
#endif
}

#ifdef UFSD_USE_BUILTIN_ZEROING

#if !(defined HAVE_DECL_BLKDEV_ISSUE_ZEROOUT && HAVE_DECL_BLKDEV_ISSUE_ZEROOUT)
//
// Stolen from block/blk-lib.c
//

struct bio_batch {
  atomic_t          done;
  unsigned long     flags;
  struct completion *wait;
};

#if defined HAVE_DECL_BIO_END_V2 && HAVE_DECL_BIO_END_V2
static void bio_batch_end_io( struct bio *bio, int err )
{
  struct bio_batch *bb = bio->bi_private;
  if ( err && EOPNOTSUPP != err )
    clear_bit( BIO_UPTODATE, &bb->flags );
  if ( atomic_dec_and_test( &bb->done ) )
    complete( bb->wait );
  bio_put( bio );
}
#elif defined HAVE_DECL_BIO_END_V1 && HAVE_DECL_BIO_END_V1
static int bio_batch_end_io( struct bio *bio, unsigned int bytes, int err )
{
  struct bio_batch *bb = bio->bi_private;
	if ( bio->bi_size )
		return 1;
  if ( err && EOPNOTSUPP != err )
    clear_bit( BIO_UPTODATE, &bb->flags );
  if ( atomic_dec_and_test( &bb->done ) )
    complete( bb->wait );
  bio_put( bio );
  return 0;
}
#endif
#endif // #if !(defined HAVE_DECL_BLKDEV_ISSUE_ZEROOUT && HAVE_DECL_BLKDEV_ISSUE_ZEROOUT)

///////////////////////////////////////////////////////////
// UFSD_BdZero
//
// Helper function to zero blocks in block device
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
UFSD_BdZero(
    IN struct super_block* sb,
    IN UINT64 Offset,
    IN size_t Bytes
    )
{
  int err;

#if defined HAVE_DECL_BLKDEV_ISSUE_ZEROOUT && HAVE_DECL_BLKDEV_ISSUE_ZEROOUT
  //
  // 2.6.35+
  //
  DebugTrace(+1, UFSD_LEVEL_IO, ("BdZero: %p, %llx, %Zx\n", sb, Offset, Bytes));
  err = blkdev_issue_zeroout( sb->s_bdev, Offset >> 9, Bytes >> 9, GFP_NOFS
#ifdef BLKDEV_IFL_WAIT
                              ,  BLKDEV_IFL_WAIT | BLKDEV_IFL_BARRIER
#endif
                             ,false);
#else
  sector_t  sector    = Offset >> 9;
  sector_t  nr_sects  = Bytes >> 9;
  struct bio_batch bb;
  struct page* zero;
#ifdef DECLARE_COMPLETION_ONSTACK
  DECLARE_COMPLETION_ONSTACK( wait );
#else
  DECLARE_COMPLETION( wait );
#endif

#if 1
  zero = alloc_page( GFP_KERNEL | __GFP_ZERO );
  if ( NULL == zero )
    return -ENOMEM;
#else
  // Don't use ZERO_PAGE(0). Sometimes G.P.L., sometimes not exported ...
  zero = ZERO_PAGE(0);
#endif

  DebugTrace(+1, UFSD_LEVEL_IO, ("BdZero: %p, %llx, %Zx\n", sb, Offset, Bytes));

  atomic_set( &bb.done, 1 );
  err       = 0;
  bb.flags  = 1 << BIO_UPTODATE;
  bb.wait   = &wait;

  while ( 0 != nr_sects ) {

    struct bio* bio = bio_alloc( GFP_NOFS, min(nr_sects, (sector_t)BIO_MAX_PAGES) );
    if ( !bio ) {
      err = -ENOMEM;
      break;
    }

    bio->bi_sector  = sector;
    bio->bi_bdev    = sb->s_bdev;
    bio->bi_end_io  = bio_batch_end_io;
    bio->bi_private = &bb;

    while ( 0 != nr_sects ) {
      unsigned int sz = min((sector_t) PAGE_SIZE , nr_sects << 9 );
      int ok = bio_add_page( bio, zero, sz, 0 );
      DebugTrace(0, UFSD_LEVEL_IO, ("added %d\n", ok));
      nr_sects -= ok >> 9;
      sector   += ok >> 9;
      if ( ok < sz )
        break;
    }
    atomic_inc( &bb.done );
    DebugTrace(0, UFSD_LEVEL_IO, ("bio: %"PSCT"x, sz=%x\n", bio->bi_sector, bio->bi_size));
    submit_bio( WRITE, bio );
  }

  // Wait for bios in-flight
  if ( !atomic_dec_and_test( &bb.done ) )
    wait_for_completion( &wait );

  if ( !test_bit( BIO_UPTODATE, &bb.flags ) )
    err = -EIO; // One of bios in the batch was completed with error

  __free_page( zero );
#endif

#ifdef UFSD_TRACE
  if ( 0 != err ) {
    DebugTrace(-1, UFSD_LEVEL_IO, ("zero failed: err=%d\n", err));
  } else if ( UFSD_TraceLevel & UFSD_LEVEL_IO )
    UFSD_TraceInc( -1 );
#endif
  return err;
}
#endif // #ifdef UFSD_USE_BUILTIN_ZEROING


///////////////////////////////////////////////////////////
// UFSD_BdSetBlockSize
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_BdSetBlockSize(
    IN struct super_block* sb,
    IN unsigned int        BytesPerBlock
    )
{
  usuper* sbi = UFSD_SB( sb );
  //
  // Log2 of block size
  //
  sbi->BlkBits = blksize_bits( BytesPerBlock );

  if ( BytesPerBlock <= PAGE_SIZE ) {
    UINT64 BytesPerDev    = sb->s_bdev->bd_inode->i_size;
    unsigned int RaBytes  = sbi->ReadAheadBlocks << sb->s_blocksize_bits;
    sb_set_blocksize( sb, BytesPerBlock );
    sbi->ReadAheadBlocks  = RaBytes >> sb->s_blocksize_bits;
    sbi->MaxBlock         = BytesPerDev >> sb->s_blocksize_bits;
    DebugTrace(0, Dbg, ("BdSetBlockSize %x\n", BytesPerBlock ));
  } else {
    DebugTrace(0, Dbg, ("BdSetBlockSize %x -> %lx\n", BytesPerBlock, sb->s_blocksize ));
  }
}


///////////////////////////////////////////////////////////
// UFSD_BdIsReadonly
//
// Returns !0 for readonly media
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
UFSD_BdIsReadonly(
    IN struct super_block* sb
    )
{
  return FlagOn( sb->s_flags, MS_RDONLY );
}


///////////////////////////////////////////////////////////
// UFSD_BdGetName
//
// Returns the name of block device
///////////////////////////////////////////////////////////
const char*
UFSDAPI_CALL
UFSD_BdGetName(
    IN struct super_block* sb
    )
{
#if defined HAVE_STRUCT_SUPER_BLOCK_S_ID && HAVE_STRUCT_SUPER_BLOCK_S_ID
  return sb->s_id;
#else
  return UFSD_SB(sb)->s_id;
#endif
}


#if !(defined HAVE_DECL_SYNC_BLOCKDEV && HAVE_DECL_SYNC_BLOCKDEV)
  #define sync_blockdev fsync_no_super
#endif


///////////////////////////////////////////////////////////
// UFSD_BdFlush
//
//
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
UFSD_BdFlush(
    IN struct super_block* sb,
    IN size_t Wait
    )
{
#if defined WRITE_FLUSH_FUA | defined WRITE_BARRIER
  if ( 0 == Wait && 0 == UFSD_SB( sb )->options.nobarrier )
    return 0;
#endif

  DebugTrace(0, Dbg, ("BdFlush (%s)\n", UFSD_BdGetName( sb ) ));

  return sync_blockdev( sb_dev(sb) );
}


///////////////////////////////////////////////////////////
// DoDelayedTasks
//
// This function is called under locked ApiMutex
///////////////////////////////////////////////////////////
static void
DoDelayedTasks(
    IN usuper* sbi
    )
{
  unsigned int cnt;
  delay_task* task;
  int VFlush = atomic_read( &sbi->VFlush );

  if ( 0 != VFlush || ( sbi->options.sync && UFSDAPI_IsVolumeDirty( sbi->Ufsd ) ) ){
    UFSDAPI_VolumeFlush( sbi->Ufsd, 2 == VFlush );
    atomic_set( &sbi->VFlush, 0 );
  }

  //
  // Do delayed clear
  //
  for ( cnt = 0; ; cnt++ ) {
    UFSD_FILE* file;
    spin_lock( &sbi->ddt_lock );
    if ( list_empty( &sbi->clear_list ) ) {
      task = NULL;
    } else {
      task = list_entry( sbi->clear_list.next, delay_task, list );
      list_del( &task->list );
    }
    spin_unlock( &sbi->ddt_lock );

    if ( NULL == task )
      break;

    file = task->file;
    assert( NULL != file );

    UFSDAPI_FileClose( sbi->Ufsd, file );
    kfree( task );
  }

  if ( 0 != cnt ){
    DebugTrace( 0, Dbg, ("DoDelayedTasks: clear=%u\n", cnt ) );
  }
}


///////////////////////////////////////////////////////////
// LockUfsd
//
//
///////////////////////////////////////////////////////////
static void
_LockUfsd(
    IN usuper* sb
#ifdef UFSD_TRACE
    , IN const char* Hint
#endif
    )
{
  TRACE_ONLY( unsigned long dT; )
  TRACE_ONLY( unsigned long T0; )
  TRACE_ONLY( if ( UFSD_TraceLevel & UFSD_LEVEL_SEMA ) si_meminfo( &sb->SysInfo ); )
  DebugTrace(+1, UFSD_LEVEL_SEMA, ("%u: %lx %lx \"%s\" %s (+), ... ",
              jiffies_to_msecs(jiffies-StartJiffies),
              sb->SysInfo.freeram, sb->SysInfo.bufferram,
              current->comm, Hint));

  TRACE_ONLY( T0 = jiffies; )

  Mutex_lock( &sb->ApiMutex );
#ifdef UFSD_TRACE
  dT         = jiffies - T0;
  WaitMutex += dT;
  if ( 0 == dT ) {
    DebugTrace(0, UFSD_LEVEL_SEMA, ("OK\n"));
  } else {
    DebugTrace(0, UFSD_LEVEL_SEMA, ("OKw %u\n", jiffies_to_msecs(dT)));
  }
#endif

  //
  // Perform any delayed tasks
  //
  DoDelayedTasks( sb );
}


///////////////////////////////////////////////////////////
// TryLockUfsd
//
// Returns 0 if mutex is locked
///////////////////////////////////////////////////////////
static int
_TryLockUfsd(
    IN usuper* sb
#ifdef UFSD_TRACE
    , IN const char* Hint
#endif
    )
{
  int ok = Mutex_trylock( &sb->ApiMutex );
  TRACE_ONLY( if ( UFSD_TraceLevel & UFSD_LEVEL_SEMA ) si_meminfo( &sb->SysInfo ); )

  assert( 0 == ok || 1 == ok );

  DebugTrace(ok, UFSD_LEVEL_SEMA, ("%u: %lx %lx \"%s\" %s %s\n",
              jiffies_to_msecs(jiffies-StartJiffies),
              sb->SysInfo.freeram, sb->SysInfo.bufferram,
              current->comm, Hint, ok? "(+)" : "-> wait"));

  if ( ok ) {
    //
    // Perform any delayed tasks
    //
    DoDelayedTasks( sb );
  }

  return !ok;
}


///////////////////////////////////////////////////////////
// UnlockUfsd
//
//
///////////////////////////////////////////////////////////
static void
_UnlockUfsd(
    IN usuper* sb
#ifdef UFSD_TRACE
    , IN const char* Hint
#endif
    )
{
  TRACE_ONLY( if ( UFSD_TraceLevel & UFSD_LEVEL_SEMA ) si_meminfo( &sb->SysInfo ); )

  //
  // Perform any delayed tasks
  //
  DoDelayedTasks( sb );

  DebugTrace(-1, UFSD_LEVEL_SEMA, ("%u: %lx %lx \"%s\" %s (-)\n",
              jiffies_to_msecs(jiffies-StartJiffies),
              sb->SysInfo.freeram, sb->SysInfo.bufferram,
              current->comm, Hint));

  Mutex_unlock( &sb->ApiMutex );
}


//
// Parameter structure for
// iget4 call to be passed as 'opaque'.
//

typedef struct ufsd_iget4_param {
  UfsdCreate*           Create;
  UfsdFileInfo          Info;
  UFSD_FILE*            fh;
  int                   subdir_count;
  const unsigned char*  name;
  size_t                name_len;
} ufsd_iget4_param;


#if defined HAVE_DECL_READDIR_V1 && HAVE_DECL_READDIR_V1

  #define READDIR_DECLARE_ARG struct file* file, void* dirent, filldir_t filldir
  #define READDIR_POS         file->f_pos
  #define READDIR_FILL(Name, NameLen, pos, ino, dt) filldir( dirent, Name, NameLen, pos, ino, dt )
  #define iterate             readdir

#elif defined HAVE_DECL_READDIR_V2 && HAVE_DECL_READDIR_V2

  #define READDIR_DECLARE_ARG struct file* file, struct dir_context* ctx
  #define READDIR_POS         ctx->pos
  #define READDIR_FILL(Name, NameLen, pos, ino, dt) (ctx->pos=pos, !dir_emit( ctx, Name, NameLen, ino, dt ))

#else
  #error "Unknown readdir"
#endif


///////////////////////////////////////////////////////////
// ufsd_readdir
//
// file_operations::readdir
//
// This routine is a callback used to fill readdir() buffer.
//  file - Directory pointer.
//    'f_pos' member contains position to start scan from.
//
//  dirent, filldir - data to be passed to
//    'filldir()' helper
///////////////////////////////////////////////////////////
static int
ufsd_readdir(
    READDIR_DECLARE_ARG
    )
{
  struct inode* i = file_inode(file);
  unode* u     = UFSD_U( i );
  usuper* sbi  = UFSD_SB( i->i_sb );
  UINT64 pos      = READDIR_POS;
  UFSD_SEARCH* DirScan = NULL;
#ifdef UFSD_EMULATE_SMALL_READDIR_BUFFER
  size_t cnt = 0;
#endif

  if ( pos >= sbi->Eod ) {
    DebugTrace(0, Dbg, ("readdir: r=%lx,%llx -> no more\n", i->i_ino, pos ));
    return 0;
  }

  DebugTrace(+1, Dbg, ("readdir: %p, r=%lx, %llx\n", file, i->i_ino, pos ));

  LockUfsd( sbi );

  if ( 0 == UFSDAPI_FindOpen( sbi->Ufsd, u->ufile, pos, &DirScan ) ) {

    size_t  ino;
    int     is_dir;
    char*   Name;
    size_t  NameLen;
    int nfsd  = 0 == strcmp( "nfsd", current->comm );

    //
    // Enumerate UFSD's direntries
    //
    while ( 0 == UFSDAPI_FindGet( DirScan, &pos, &Name, &NameLen, &is_dir, &ino ) ) {

      int fd;

      if ( nfsd )
        UnlockUfsd( sbi );

#ifdef UFSD_EMULATE_SMALL_READDIR_BUFFER
      if ( ++cnt > UFSD_EMULATE_SMALL_READDIR_BUFFER )
        break;
#endif

      //
      // Unfortunately nfsd callback function opens file which in turn calls 'LockUfsd'
      // Linux's mutex does not allow recursive locks
      //
      fd = READDIR_FILL( Name, NameLen, pos, (ino_t)ino, is_dir? DT_DIR : DT_REG );

      if ( nfsd )
        LockUfsd( sbi );

      if ( fd )
        break;
    }

    UFSDAPI_FindClose( DirScan );
  }

  UnlockUfsd( sbi );

  //
  // Save position and return
  //
  READDIR_POS = pos;
  file->f_version = i->i_version;
#if defined HAVE_DECL_UPDATE_ATIME && HAVE_DECL_UPDATE_ATIME
  update_atime( i );
#endif

  DebugTrace(-1, Dbg, ("readdir -> 0 (next=%x)\n", (unsigned)file->f_pos));
  return 0;
}


///////////////////////////////////////////////////////////
// LazyOpen
//
// Assumed LockUfsd()
// Returns 0 if OK
///////////////////////////////////////////////////////////
static int
LazyOpen(
    IN usuper*        sbi,
    IN struct inode*  i
    )
{
  UfsdFileInfo Info;
  unode* u = UFSD_U(i);

  if ( NULL != u->ufile )
    return 0;

  if ( 0 == UFSDAPI_FileOpenById( sbi->Ufsd, i->i_ino, &u->ufile, &Info ) ) {
    assert( NULL != u->ufile );
    assert( i->i_ino == Info.Id );

    i->i_size   = Info.size;
    i->i_blocks = Info.asize >> 9;

    if ( S_ISDIR( i->i_mode ) != Info.is_dir ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("Incorrect dir/file of inode r=%lx\n", i->i_ino ));
    } else {
      if ( !Info.is_dir ) {
        set_nlink( i, Info.link_count );
        u->sparse   = Info.is_sparse;
        u->compr    = Info.is_compr;
        u->encrypt  = Info.is_encrypt;
        u->mmu      = Info.vsize;
      }
      u->xattr  = Info.is_xattr;
      return 0;
    }
  }

  make_bad_inode( i );
  return -ENOENT;
}


#ifdef UFSD_DELAY_ALLOC

#ifdef UFSD_DEBUG
///////////////////////////////////////////////////////////
// TraceFreeSpace
//
//
///////////////////////////////////////////////////////////
static void
TraceFreeSpace(
    IN usuper* sbi,
    IN char*   hint
    )
{
  DebugTrace(0, Dbg, ("%s: dirty=%lx,free=%lx\n", hint, atomic_long_read( &sbi->DirtyBlocks ), atomic_long_read( &sbi->FreeBlocks ) ));
}
#else
  #define TraceFreeSpace(sbi,hint)
#endif


///////////////////////////////////////////////////////////
// FreeSpaceCallBack
//
//
///////////////////////////////////////////////////////////
static void
FreeSpaceCallBack(
    IN size_t Lcn,
    IN size_t Len,
    IN int    AsUsed,
    IN void*  Arg
    )
{
  usuper* sbi = (usuper*)Arg;
  UNREFERENCED_PARAMETER( Lcn );
  if ( AsUsed )
  {
    atomic_long_sub( Len, &sbi->FreeBlocks );
  }
  else
  {
    atomic_long_add( Len, &sbi->FreeBlocks );
  }
  assert( sbi->DoNotTraceNoSpc || atomic_long_read( &sbi->FreeBlocks ) >= atomic_long_read( &sbi->DirtyBlocks ) );
}


///////////////////////////////////////////////////////////
// ufsd_alloc_da_blocks
//
// Force all delayed allocation blocks to be allocated for a given inode
///////////////////////////////////////////////////////////
static int
ufsd_alloc_da_blocks(
    IN struct inode* i
    )
{
  unode* u = UFSD_U( i );

  if ( 0 == u->i_reserved_data_blocks && 0 == u->i_reserved_meta_blocks )
    return 0;

  DebugTrace( 0, Dbg, ("alloc_da_blocks: r=%lx, %"PSCT"x+%"PSCT"x\n", i->i_ino, u->i_reserved_data_blocks, u->i_reserved_meta_blocks) );

  // filemap_flush(), will map the blocks, and start the I/O, but not actually wait for the I/O to complete.
  return filemap_flush( i->i_mapping );
}


///////////////////////////////////////////////////////////
// ufsd_da_release_space
//
// Reduce the count of reserved blocks
///////////////////////////////////////////////////////////
static void
ufsd_da_release_space(
    IN struct inode*  i,
    IN unsigned       blocks
    )
{
  usuper* sbi = UFSD_SB( i->i_sb );
  unode* u    = UFSD_U( i );

  assert( 0 != blocks );

  spin_lock( &u->block_lock );

  assert( blocks <= u->i_reserved_data_blocks );

  if ( unlikely( blocks > u->i_reserved_data_blocks ) ) {
    WARN_ON(1);
    blocks = u->i_reserved_data_blocks;
  }
  u->i_reserved_data_blocks -= blocks;
  atomic_long_sub( blocks, &sbi->DirtyBlocks );

  if ( 0 == u->i_reserved_data_blocks ) {
    //
    // We can release all of the reserved metadata blocks
    // only when we have written all of the delayed allocation blocks.
    //
    atomic_long_sub( u->i_reserved_meta_blocks, &sbi->DirtyBlocks );
    u->i_reserved_meta_blocks   = 0;
  }

  spin_unlock( &u->block_lock );
}


///////////////////////////////////////////////////////////
// ufsd_getattr
//
// inode_operations::getattr
///////////////////////////////////////////////////////////
static int
ufsd_getattr(
    IN struct vfsmount* mnt,
    IN struct dentry*   de,
    OUT struct kstat*   stat
    )
{
  struct inode* i = de->d_inode;
  generic_fillattr( i, stat );
  // Correct allocated blocks
  stat->blocks += UFSD_U(i)->i_reserved_data_blocks << (i->i_sb->s_blocksize_bits-9);
  return 0;
}

#endif // #ifdef UFSD_DELAY_ALLOC


#ifdef UFSD_NTFS
///////////////////////////////////////////////////////////
// IsStream
//
// Helper function returns non zero if filesystem supports streams and
// 'file' is stream handler
///////////////////////////////////////////////////////////
static inline const unsigned char*
IsStream(
    IN struct file* file
    )
{
#if 0
  // Nobody should use 'file->private_data'
  return file->private_data;
#else
  // Safe check
  if ( NULL != file->private_data ) {
    const unsigned char* r = file->private_data;
    int d = (int)(r - file->f_path.dentry->d_name.name);
    if ( 0 < d && d <= file->f_path.dentry->d_name.len )
      return r;
  }
  return NULL;
#endif
}
#else
  #define IsStream(f) NULL
#endif


///////////////////////////////////////////////////////////
// ufsd_file_open
//
// file_operations::open
///////////////////////////////////////////////////////////
static int
ufsd_file_open(
    IN struct inode*  i,
    IN struct file*   file
    )
{
  usuper* sbi     = UFSD_SB( i->i_sb );
  unode* u        = UFSD_U( i );
  struct qstr* s  = &file->f_path.dentry->d_name;
  TRACE_ONLY( const char* hint=""; )
  int err;

  assert( file->f_mapping == i->i_mapping && "Check kernel config!" );
  DebugTrace(+1, Dbg, ("file_open: r=%lx, c=%x, l=%x, f=%p, fl=o%o%s%s, %.*s\n",
                i->i_ino, atomic_read( &i->i_count ), i->i_nlink,
                file, file->f_flags,
                FlagOn( file->f_flags, O_DIRECT )?",d":"", FlagOn( file->f_flags, O_APPEND )?",a":"",
                (int)s->len, s->name ));

  LockUfsd( sbi );
  err = LazyOpen( sbi, i );
  UnlockUfsd( sbi );

  if ( 0 != err ) {
    DebugTrace(-1, Dbg, ("file_open -> failed\n"));
    return err;
  }

  if ( (u->compr || u->encrypt) && FlagOn( file->f_flags, O_DIRECT ) ) {
    DebugTrace(-1, Dbg, ("file_open -> failed to open compressed file with O_DIRECT\n"));
    return -ENOTBLK;
  }

  assert( NULL == file->private_data );
  if ( 0 != sbi->options.delim ) {
    char* p = strchr( s->name, sbi->options.delim );
    if ( NULL != p ){
      igrab( i );
      dget( file->f_path.dentry );
      file->private_data = p + 1;
      assert( IsStream( file ) );
      TRACE_ONLY( hint="(stream)"; )
    }
  }

  if ( i->i_nlink <= 1 ) {
    DebugTrace(-1, Dbg, ("file_open%s -> ok%s\n", hint, u->compr?", c" : ""));
  } else {
    DebugTrace(-1, Dbg, ("file_open%s -> l=%x%s\n", hint, i->i_nlink, u->compr?", c" : "" ));
  }

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_file_release
//
// file_operations::release
///////////////////////////////////////////////////////////
static int
ufsd_file_release(
    IN struct inode*  i,
    IN struct file*   file
    )
{
  TRACE_ONLY( const char* hint=""; )
  if ( IsStream( file ) ) {
    dput( file->f_path.dentry );
    iput( i );
    TRACE_ONLY( hint="(stream)"; )
  } else {
#ifdef UFSD_DELAY_ALLOC
    unode* u = UFSD_U( i );
    if ( FlagOn( u->i_state_flags, UFSD_STATE_DA_ALLOC_CLOSE ) ) {
      ufsd_alloc_da_blocks( i );
      ClearFlag( u->i_state_flags, UFSD_STATE_DA_ALLOC_CLOSE );
    }

#if 0
    // if we are the last writer on the inode, drop the block reservation
    if ( FlagOn( file->f_mode, FMODE_WRITE )
      && 1 == atomic_read( &i->i_writecount )
      && 0 == u->i_reserved_data_blocks )
    {
//      down_write( &u->i_data_sem);
//      ufsd_discard_preallocations( i );
//      up_write( &u->i_data_sem );
    }
#endif
#endif
  }

  DebugTrace(0, Dbg, ("file_release%s: r=%lx, %p\n", hint, i->i_ino, file ));

  return 0;
}


#ifndef UFSD_NO_USE_IOCTL

#ifndef VFAT_IOCTL_GET_VOLUME_ID
  #define VFAT_IOCTL_GET_VOLUME_ID  _IOR('r', 0x12, __u32)
#endif

///////////////////////////////////////////////////////////
// ufsd_ioctl
//
// file_operations::ioctl
///////////////////////////////////////////////////////////
#if defined HAVE_STRUCT_FILE_OPERATIONS_IOCTL && HAVE_STRUCT_FILE_OPERATIONS_IOCTL
static int
ufsd_ioctl(
    IN struct inode*  i,
    IN struct file*   file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
#else
#define ioctl unlocked_ioctl
static long
ufsd_ioctl(
    IN struct file*   file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
  struct inode* i = file_inode( file );
#endif
  int err;
  unsigned insize = 0, osize = 0;
  size_t BytesReturned;
  usuper* sbi  = UFSD_SB( i->i_sb );

  UfsdFileInfo Info;

  DebugTrace(+1, Dbg,("ioctl: ('%.*s'), r=%lx, m=%o, f=%p, %08x, %lu\n",
                       (int)file->f_path.dentry->d_name.len, file->f_path.dentry->d_name.name,
                       i->i_ino, i->i_mode, file, cmd, arg));

  if ( VFAT_IOCTL_GET_VOLUME_ID == cmd ) {
    //
    // Special code
    //
    err = UFSDAPI_QueryVolumeId( sbi->Ufsd );
    DebugTrace(-1, Dbg, ("ioctl (VFAT_IOCTL_GET_VOLUME_ID ) -> %x\n", (unsigned)err));
    return err;
  }

  switch( cmd ) {
  case UFSD_IOC_SETVALID:
  case UFSD_IOC_SETCLUMP:
  case UFSD_IOC_SETTIMES:
  case UFSD_IOC_GETTIMES:
  case UFSD_IOC_SETATTR:
  case UFSD_IOC_GETATTR:
  case UFSD_IOC_GETMEMUSE:
  case UFSD_IOC_GETVOLINFO:
    break;
  default:
    DebugTrace(-1, Dbg, ("ioctl -> '-ENOTTY'\n"));
    return -ENOTTY;
  }

#if !defined access_ok && (defined HAVE_DECL_VERIFY_AREA && HAVE_DECL_VERIFY_AREA)
  #define access_ok !verify_area
#endif

  if ( _IOC_DIR( cmd ) & _IOC_WRITE ) {
    insize  = _IOC_SIZE( cmd );
    if ( !access_ok( VERIFY_READ, (__user void*)arg, insize ) ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("ioctl: invalid input buffer.\n"));
      err = -EFAULT;
      goto out;
    }
  } else if (_IOC_DIR( cmd ) & _IOC_READ ) {
    osize   = _IOC_SIZE( cmd );
    if ( !access_ok( VERIFY_WRITE, (__user void*)arg, osize ) ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("ioctl: invalid output buffer.\n"));
      err = -EFAULT;
      goto out;
    }
  }

  assert( NULL != i );
  assert( NULL != UFSD_VOLUME(i->i_sb) );

  //
  // And call the library.
  //
  LockUfsd( sbi );

  err = UFSDAPI_IoControl( sbi->Ufsd, UFSD_FH( i ), _IOC_NR(cmd), (void*)arg, insize, (void*)arg, osize, &BytesReturned, &Info );

  if ( 0 == err ) {
    switch( cmd ) {
    case UFSD_IOC_SETTIMES:
      UfsdTimes2Inode( sbi, UFSD_U( i ), i, &Info );
      mark_inode_dirty( i );
      break;
    case UFSD_IOC_SETVALID:
      UFSD_U( i )->mmu = Info.vsize;
      // no break here!
    case UFSD_IOC_SETATTR:
      mark_inode_dirty( i );
      break;
    }
  }

  UnlockUfsd( sbi );

  //
  // Translate possible UFSD IoControl errors (see u_errors.h):
  //
  switch( (unsigned)err ) {
  case 0:                       err = 0; break;           // OK
  case ERR_NOTIMPLEMENTED:      err = -ENOSYS; break;     // Function not implemented
  case ERR_INSUFFICIENT_BUFFER: err = -ENODATA; break;    // No data available
  case ERR_MORE_DATA:           err = -EOVERFLOW; break;  // Value too large for defined data type
  default:                      err = -EINVAL;
  }

out:

  DebugTrace(-1, Dbg, ("ioctl -> %d\n", err));
  return err;
}


#ifdef CONFIG_COMPAT
#include <linux/compat.h>
///////////////////////////////////////////////////////////
// ufsd_compat_ioctl
//
// 32 application -> 64 bit driver
///////////////////////////////////////////////////////////
static long
ufsd_compat_ioctl(
    IN struct file*   file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
  switch( cmd ) {
  case UFSD_IOC32_GETMEMUSE:
    cmd = UFSD_IOC_GETMEMUSE;
    break;
  }

  return ufsd_ioctl(
#if defined HAVE_STRUCT_FILE_OPERATIONS_IOCTL && HAVE_STRUCT_FILE_OPERATIONS_IOCTL
    file_inode( file ),
#endif
    file, cmd, (unsigned long)compat_ptr(arg)
    );
}
#endif // #ifdef CONFIG_COMPAT
#endif // #ifndef UFSD_NO_USE_IOCTL


#if !defined HAVE_DECL_GENERIC_FILE_FSYNC || !HAVE_DECL_GENERIC_FILE_FSYNC
  #define generic_file_fsync  file_fsync
#endif

STATIC_CONST struct file_operations ufsd_dir_operations = {
  .llseek   = generic_file_llseek,
  .read     = generic_read_dir,
  .iterate  = ufsd_readdir,
  .fsync    = generic_file_fsync,
  .open     = ufsd_file_open,
  .release  = ufsd_file_release,
#ifndef UFSD_NO_USE_IOCTL
  .ioctl    = ufsd_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl	= ufsd_compat_ioctl,
#endif
#endif
};


///////////////////////////////////////////////////////////
// ufsd_compare_hlp
//
// Helper function for both version of 'ufsd_compare'
// Returns 0 if names equal, +1 if not equal, -1 if UFSD required
///////////////////////////////////////////////////////////
static inline int
ufsd_compare_hlp(
    IN const char*  n1,
    IN unsigned int l1,
    IN const char*  n2,
    IN unsigned int l2
    )
{
  unsigned int len = min( l1, l2 );
  while( 0 != len-- ){
    unsigned char c1 = *n1++, c2 = *n2++;
    if ( (c1 >= 0x80 || c2 >= 0x80) && c1 != c2 )
      return -1; // Requires UFSD
    if ( 'a' <= c1 && c1 <= 'z' )
      c1 -= 'a'-'A';  // c1 &= ~0x20
    if ( 'a' <= c2 && c2 <= 'z' )
      c2 -= 'a'-'A';  // c2 &= ~0x20

    if ( c1 != c2 )
      return 1; // not equal
  }
  return l1 == l2? 0 : 1;
}


///////////////////////////////////////////////////////////
// ufsd_name_hash_hlp
//
// Helper function for both version of 'ufsd_name_hash'
///////////////////////////////////////////////////////////
static inline unsigned int
ufsd_name_hash_hlp(
    IN const char*  n,
    IN unsigned int len,
    OUT int*        err
    )
{
  unsigned int hash = 0;

  while( 0 != len-- ) {
    unsigned int c = *n++;
    if ( c >= 0x80 ) {
      *err = -1;  // Requires UFSD
      return 0;
    }

    if ( 'a' <= c && c <= 'z' )
      c -= 'a'-'A'; // simple upcase

    hash = (hash + (c << 4) + (c >> 4)) * 11;
  }

  *err = 0;
  return hash;
}


#if defined HAVE_DECL_DHASH_V1 && HAVE_DECL_DHASH_V1
///////////////////////////////////////////////////////////
// ufsd_compare
//
// dentry_operations::d_compare
///////////////////////////////////////////////////////////
static int
ufsd_compare(
    IN struct dentry*  de,
    IN struct qstr*    name1,
    IN struct qstr*    name2
    )
{
  int ret;

  // Custom compare used to support case-insensitive scan.
  // Should return zero on name match.
  assert(NULL != de->d_inode);

  DEBUG_ONLY( UFSD_SB(de->d_inode->i_sb)->nCompareCalls += 1; )

  ret = ufsd_compare_hlp( name1->name, name1->len, name2->name, name2->len );
  if ( ret < 0 ) {
    usuper* sbi  = UFSD_SB(de->d_inode->i_sb);
    Mutex_lock( &sbi->NoCaseMutex );
    ret = !UFSDAPI_NamesEqual( sbi->Ufsd, name1->name, name1->len, name2->name, name2->len );
    Mutex_unlock( &sbi->NoCaseMutex );
  }
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_name_hash
//
// dentry_operations::d_hash
///////////////////////////////////////////////////////////
static int
ufsd_name_hash(
    IN struct dentry*  de,
    IN struct qstr*    name
    )
{
  int err;
  DEBUG_ONLY( UFSD_SB(de->d_inode->i_sb)->nHashCalls += 1; )

  name->hash = ufsd_name_hash_hlp( name->name, name->len, &err );
  if ( err ) {
    usuper* sbi  = UFSD_SB(de->d_inode->i_sb);
    Mutex_lock( &sbi->NoCaseMutex );
    name->hash = UFSDAPI_NameHash( sbi->Ufsd, name->name, name->len );
    Mutex_unlock( &sbi->NoCaseMutex );
  }

  return 0;
}

#elif ( defined HAVE_DECL_DHASH_V2 && HAVE_DECL_DHASH_V2 ) || ( defined HAVE_DECL_DHASH_V3 && HAVE_DECL_DHASH_V3 )

///////////////////////////////////////////////////////////
// ufsd_compare
//
// dentry_operations::d_compare
///////////////////////////////////////////////////////////
static int
ufsd_compare(
    IN const struct dentry* parent,
#if defined HAVE_DECL_DCOMPARE_V2 && HAVE_DECL_DCOMPARE_V2
    IN const struct inode * iparent,
#endif
    IN const struct dentry* de,
#if defined HAVE_DECL_DCOMPARE_V2 && HAVE_DECL_DCOMPARE_V2
    IN const struct inode * i,
#endif
    IN unsigned int         len,
    IN const char*          str,
    IN const struct qstr*   name
    )
{
  int ret;

//  DebugTrace(0, Dbg, ("ufsd_compare: %p %p %p %p %*.s %.*s\n", parent, iparent, de, i, len, str, name->len, name->name ));

  // Custom compare used to support case-insensitive scan.
  // Should return zero on name match.
  //
  // NOTE: do not use 'i' cause it can be NULL (3.6.6+)
  //
#if defined HAVE_DECL_DCOMPARE_V2 && HAVE_DECL_DCOMPARE_V2
  assert( NULL != parent && NULL != iparent && parent->d_inode == iparent );
#elif defined HAVE_DECL_DCOMPARE_V3 && HAVE_DECL_DCOMPARE_V3
  assert( NULL != parent );
#endif

  DEBUG_ONLY( UFSD_SB( parent->d_inode->i_sb )->nCompareCalls += 1; )

  ret = ufsd_compare_hlp( name->name, name->len, str, len );
  if ( ret < 0 ) {
    usuper* sbi  = UFSD_SB( parent->d_inode->i_sb );
    Mutex_lock( &sbi->NoCaseMutex );
    ret = !UFSDAPI_NamesEqual( sbi->Ufsd, name->name, name->len, str, len );
    Mutex_unlock( &sbi->NoCaseMutex );
  }

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_name_hash
//
// dentry_operations::d_hash
///////////////////////////////////////////////////////////
static int
ufsd_name_hash(
    IN const struct dentry* de,
#if defined HAVE_DECL_DHASH_V2 && HAVE_DECL_DHASH_V2
    IN const struct inode*  i,
#endif
    IN struct qstr*         name
    )
{
  int err;
  DEBUG_ONLY( UFSD_SB(de->d_inode->i_sb)->nHashCalls += 1; )

  name->hash = ufsd_name_hash_hlp( name->name, name->len, &err );
  if ( err ) {
    usuper* sbi  = UFSD_SB(de->d_inode->i_sb);
    Mutex_lock( &sbi->NoCaseMutex );
    name->hash = UFSDAPI_NameHash( sbi->Ufsd, name->name, name->len );
    Mutex_unlock( &sbi->NoCaseMutex );
  }

  return 0;
}

#else

#error "unknown dentry_operations.d_hash"

#endif


static struct dentry_operations ufsd_dop = {
  // case insensitive (nocase=1)
  .d_hash       = ufsd_name_hash,
  .d_compare    = ufsd_compare,
};


// Compatible implementation of iget4 for kernel 2.6+
#if !(defined HAVE_DECL_IGET4 && HAVE_DECL_IGET4)
static void ufsd_read_inode2 (struct inode * i, void* p);
///////////////////////////////////////////////////////////
// iget4
//
//
///////////////////////////////////////////////////////////
static inline struct inode*
iget4(
    IN struct super_block *sb,
    IN unsigned long ino,
    IN void* unused,
    IN void *ctxt
    )
{
  struct inode* i = iget_locked( sb, ino );
  if ( NULL != i && FlagOn( i->i_state, I_NEW ) ) {
    ufsd_read_inode2( i, ctxt );
    unlock_new_inode( i );
  }
  return i;
}
#endif

// Forward declaration
static int
ufsd_create_or_open (
    IN struct inode*        dir,
    IN OUT struct dentry*   de,
    IN UfsdCreate*          cr,
    OUT struct inode**      inode
    );

#ifdef UFSD_USE_XATTR
static int
ufsd_acl_chmod(
    IN struct inode* i
    );
#endif

#if ( defined HAVE_DECL_INOP_CREATE_V3 && HAVE_DECL_INOP_CREATE_V3 || defined HAVE_DECL_INOP_CREATE_V4 && HAVE_DECL_INOP_CREATE_V4 )
  typedef umode_t  Umode_t;
#else
  typedef int      Umode_t;
#endif

///////////////////////////////////////////////////////////
// ufsd_create
//
// create/open use the same helper.
// inode_operations::create
///////////////////////////////////////////////////////////
static int
ufsd_create(
    IN struct inode*   dir,
    IN struct dentry*  de,
    IN Umode_t         mode
#if (defined HAVE_DECL_INOP_CREATE_V2 && HAVE_DECL_INOP_CREATE_V2) || (defined HAVE_DECL_INOP_CREATE_V3 && HAVE_DECL_INOP_CREATE_V3)
    , struct nameidata * nd
#elif defined HAVE_DECL_INOP_CREATE_V4 && HAVE_DECL_INOP_CREATE_V4
    , bool nd
#endif
    )
{
  int err;
  struct inode* i = NULL;

  UfsdCreate  cr;

  cr.lnk  = NULL;
  cr.data = NULL;
  cr.len  = 0;
  cr.mode = mode;

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( !err )
    d_instantiate( de, i );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_mkdir
//
// inode_operations::mkdir
///////////////////////////////////////////////////////////
static int
ufsd_mkdir(
    IN struct inode*  dir,
    IN struct dentry* de,
    IN Umode_t        mode
    )
{
  int err;
  struct inode* i = NULL;
  UfsdCreate  cr;

  cr.lnk  = NULL;
  cr.data = NULL;
  cr.len  = 0;
  cr.mode = mode | S_IFDIR;

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( !err )
    d_instantiate( de, i );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_unlink
//
// inode_operations::unlink
// inode_operations::rmdir
///////////////////////////////////////////////////////////
static int
ufsd_unlink(
    IN struct inode*  dir,
    IN struct dentry* de
    )
{
  int err;
  struct inode* i   = de->d_inode;
  usuper* sbi       = UFSD_SB( i->i_sb );
  struct qstr* s    = &de->d_name;
  unsigned char* p  = 0 == sbi->options.delim? NULL : strchr( s->name, sbi->options.delim );
  char *sname;
  int flen, slen;

  if ( NULL == p ) {
    flen  = s->len;
    sname = NULL;
    slen  = 0;
  } else {
    flen  = p - s->name;
    sname = p + 1;
    slen  = s->name + s->len - p - 1;
  }

  DebugTrace(+1, Dbg, ("unlink: r=%lx, ('%.*s'), r=%lx, c=%x, l=%x\n",
              dir->i_ino, (int)s->len, s->name,
              i->i_ino, atomic_read( &i->i_count ), i->i_nlink));

  LockUfsd( sbi );

  err = UFSDAPI_Unlink( sbi->Ufsd, UFSD_FH(dir), s->name, flen, sname, slen, UFSD_FH(i) );

  UnlockUfsd( sbi );

  switch( err ) {
  case ERR_DIRNOTEMPTY:
    DebugTrace(-1, Dbg, ("unlink -> ENOTEMPTY\n"));
    return -ENOTEMPTY;
  case ERR_NOSPC:
    DebugTrace(-1, Dbg, ("unlink -> ENOSPC\n"));
    return -ENOSPC;
  }

  if ( 0 != err ) {
    make_bad_inode( i );
    DebugTrace(-1, Dbg, ("unlink -> EACCES\n"));
    return -EACCES;
  }

  if ( NULL == sname ) {
    drop_nlink( i );

    // Mark dir as requiring resync.
    dir->i_version += 1;
    TIMESPEC_SECONDS( &dir->i_mtime )  = TIMESPEC_SECONDS( &dir->i_ctime ) = get_seconds();
    dir->i_size   = UFSDAPI_GetDirSize( UFSD_FH(dir) );
    dir->i_blocks = dir->i_size >> 9;
    mark_inode_dirty( dir );
    i->i_ctime    = dir->i_ctime;
    mark_inode_dirty( i );
  }

#ifdef UFSD_COUNT_CONTAINED
  if ( S_ISDIR( i->i_mode ) ) {
    assert(dir->i_nlink > 0);
    dir->i_nlink -= 1;
    mark_inode_dirty( dir );
  }
#endif

  DebugTrace(-1, Dbg, ("unlink -> %d\n", 0));
  return 0;
}

#if !(defined HAVE_DECL_I_SIZE_READ & HAVE_DECL_I_SIZE_READ)
 #define i_size_read(i) (i)->i_size
#endif

#if !(defined HAVE_DECL_I_SIZE_WRITE & HAVE_DECL_I_SIZE_WRITE)
 #define i_size_write(i,p) (i)->i_size = (p)
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)

 #define set_buffer_new(bh)       set_bit( BH_New, &(bh)->b_state )
 #define clear_buffer_new(bh)     clear_bit( BH_New, &(bh)->b_state )
 #define set_buffer_dirty(bh)     set_bit( BH_Dirty, &(bh)->b_state )
 #define clear_buffer_dirty(bh)   clear_bit( BH_Dirty, &(bh)->b_state )
 #define set_buffer_mapped(bh)    set_bit( BH_Mapped, &(bh)->b_state )
 #define clear_buffer_mapped(bh)  clear_bit( BH_Mapped, &(bh)->b_state )
 #define clear_buffer_req(bh)     clear_bit( BH_Req, &(bh)->b_state )

#ifdef buffer_delay
 #define set_buffer_delay(bh)     set_bit( BH_Delay, &(bh)->b_state )
 #define clear_buffer_delay(bh)   clear_bit( BH_Delay, &(bh)->b_state )
#else
 #define set_buffer_delay(bh)
 #define clear_buffer_delay(bh)
 #define buffer_delay(bh)   0
#endif

#endif


#define UFSD_FLAG_DIRECT_IO         0x00000001
#define UFSD_FLAG_PREPARE           0x00000002
#define UFSD_FLAG_WRITEPAGE         0x00000004
#define UFSD_FLAG_BMAP              0x00000008
#define UFSD_FLAG_NOALLOC           0x00000010

// Caller is from the delayed allocation writeout path,
// so set the magic i_delalloc_reserve_flag after taking the
// inode allocation semaphore for
#define UFSD_GET_BLOCKS_DELALLOC_RESERVE 0x0004


// Maximum number of blocks we map for direct IO at once
#define DIO_MAX_BLOCKS 4096


///////////////////////////////////////////////////////////
// ufsd_get_block_flags
//
// This function is a callback for many 'general' functions
//
// It translates logical block in a host for the mapping to device block number.
// Here we have the following parameters:
// sb->s_blocksize             - min( BytesPerBlock, PAGE_SIZE )
// sb->s_blocksize_bits        - log2( sb->s_blocksize )
// block_size(sb_dev(i->i_sb)) - min( BytesPerBlock, PAGE_SIZE )
// i->i_blksize                - min( BytesPerBlock, PAGE_SIZE )
// i->i_blkbits                - log2( i->i_blksize )
//
///////////////////////////////////////////////////////////
static int
ufsd_get_block_flags(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create,
    IN int                  ufsd_flags
    )
{
  usuper* sbi             = UFSD_SB( i->i_sb );
  unode* u                = UFSD_U( i );
  UINT64 isize            = i_size_read( i );
  unsigned blkbits        = i->i_blkbits;
  unsigned int blocksize  = 1 << blkbits;
  UINT64 Vbo              = (UINT64)iblock << blkbits;
  sector_t Lbn = -1, Len  = 0;
  int err = 0, bNew = 0, ufsd_locked = 0;
  TRACE_ONLY( const char* hint1 = ""; )
  TRACE_ONLY( const char* hint2 = ""; )
  size_t max_blocks = bh->b_size >> blkbits;
  size_t bh_size;
  int dirty = 0;

  if ( 0 == max_blocks )
    max_blocks = 1;

  bh_size = max_blocks << blkbits;

  assert( i->i_sb->s_blocksize_bits >= sbi->SctBits );
  assert( blkbits >= sbi->SctBits );
  assert( blkbits == i->i_sb->s_blocksize_bits );

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
  assert( !buffer_mapped( bh ) );
#else
  //
  // Clear buffer delay. Very important for 2.4
  //
  clear_buffer_delay( bh );
#endif

  DebugTrace(+1, UFSD_LEVEL_VFS_GETBLK, ("get_block: r=%lx b=%"PSCT"x,%Zx, %s, %x\n", i->i_ino, iblock, max_blocks, create? "w" : "r", ufsd_flags ));

  if ( NULL == u->ufile ) {
    DebugTrace(-1, UFSD_LEVEL_VFS_GETBLK, ("get_block -> not opened\n"));
    return -EBADF;
  }

  ProfileEnter( sbi, get_block );

  //
  // Check cached info
  //
  spin_lock( &u->block_lock );
  if ( u->Vbn <= iblock && iblock < u->Vbn + u->Len ) {
    sector_t dVbn = iblock - u->Vbn;
    if ( -1 != u->Lbn ) {
      Lbn = u->Lbn + dVbn;
      Len = u->Len - dVbn;
//      DebugTrace(0, UFSD_LEVEL_VFS_GETBLK, ("cache [%"PSCT"x, %"PSCT"x) => %"PSCT"x\n", u->Vbn, u->Vbn + u->Len, u->Lbn));
      TRACE_ONLY( hint1 = ", cache"; )
    } else if ( !create ) {
//      Lbn = -1;
      Len = u->Len - dVbn;
//      DebugTrace(0, UFSD_LEVEL_VFS_GETBLK, ("cache [%"PSCT"x, %"PSCT"x) => -1\n", u->Vbn, u->Vbn + u->Len));
      TRACE_ONLY( hint1 = ", cache sparse"; )
    }
  }
  spin_unlock( &u->block_lock );

  //
  // Call UFSD library
  //
  if ( 0 == Len && (create || Vbo < isize) ) {
    mapinfo Map;

    LockUfsd( sbi );
    ufsd_locked = 1;

    assert( -1 == Lbn );

    err = UFSDAPI_FileMap( u->ufile, Vbo, bh_size, create ? UFSD_MAP_VBO_CREATE : 0, &Map );

    if ( 0 == err && Map.Len > 0 ) {

      long long dVbo = Vbo - Map.Vbo;
      assert( 0 <= dVbo && dVbo <= Map.Len );

      UnlockUfsd( sbi );
      ufsd_locked = 0;

      // Update cache
      spin_lock( &u->block_lock );

      if ( MINUS_ONE_ULL == Map.Lbo ) {
        u->Lbn  = -1;
      } else {
        u->Lbn  = Map.Lbo >> blkbits;
        Lbn     = (Map.Lbo + dVbo) >> blkbits;
      }

      u->Vbn  = Map.Vbo >> blkbits;
      u->Len  = Map.Len >> blkbits;
      Len     = (Map.Len - dVbo) >> blkbits;

      spin_unlock( &u->block_lock );

      DebugTrace(0, UFSD_LEVEL_VFS_GETBLK, ("cache: [%"PSCT"x, %"PSCT"x) => %"PSCT"x\n", u->Vbn, u->Vbn + u->Len, u->Lbn));

      assert( 0 != Len );

      if ( 0 == Len )
        Len = 1;

      bNew = FlagOn( Map.Flags, UFSD_MAP_LBO_NEW );
      if ( create )
        i->i_blocks = Map.Alloc >> 9;
    }
  }

  if ( 0 == err && Len > 0 ) {
    //
    // Process mapped blocks
    //
    UINT64 mmu;
    if ( -1 != Lbn )
      set_buffer_mapped( bh );

    if ( Len > max_blocks )
      Len = max_blocks;

    bh_size = Len << blkbits;
    mmu     = Vbo + bh_size;

    if ( create ) {
      if ( bNew || Vbo >= isize ) {
        TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds();
        dirty = 1;
        set_buffer_new( bh );
      } else if ( Vbo >= u->mmu ) {
        set_buffer_new( bh );
      }

      if ( mmu > u->mmu ) {
        dirty = 1;
        u->mmu = mmu;
      }

    } else if ( -1 == Lbn ) {
      ;
    } else if ( u->sparse ) {
      ;
    } else if ( Vbo >= u->mmu ) {
      TRACE_ONLY( hint2 = ", > valid"; )
      clear_buffer_mapped( bh );
    } else if ( mmu > u->mmu && u->mmu < isize ) {
      Len = (u->mmu - Vbo + blocksize - 1) >> blkbits;
      if ( 0 == Len )
        Len = 1;
      bh_size = Len << blkbits;
      TRACE_ONLY( hint2 = ", truncate at valid"; )
    } else if ( ufsd_flags & UFSD_FLAG_BMAP ) {
      ;
    }

  } else if ( 0 != err ) {
    err = ERR_NOSPC == err? -ENOSPC : -EINVAL;
#ifdef UFSD_DELAY_ALLOC
  } else if ( (UFSD_FLAG_NOALLOC|UFSD_FLAG_PREPARE) == ufsd_flags ) {
    err = 0;
#endif
  } else if ( ufsd_flags & UFSD_FLAG_BMAP ) {
    // Can not use bh->b_page
    err = -ENOTBLK;
  } else if ( ufsd_flags & UFSD_FLAG_DIRECT_IO ) {

    //
    // To perform direct I/O file should have allocations
    // No error if read request is out of valid size
    //
    if ( create || Vbo < isize ) {
      // -ENOTBLK is a special error code
      // which allows to handle the remaining part of the request by buffered I/O
      //
//      err = -EINVAL;
      err = -ENOTBLK;
    } else {
      bh_size = blocksize;
      err = 0;
    }

  } else if ( u->sparse && Vbo >= PAGE_SIZE ) {
    // nothing to do with sparse files
#ifdef UFSD_NTFS
  } else if ( !create || PageUptodate( bh->b_page ) ) {

    char* kaddr, *data;
    unsigned off;
    size_t tmp;

    assert( ufsd_locked );

    //
    // File does not have allocation of this Vsn = Vbo >> sbi->SctBits
    // Read/Write file synchronously
    // ( if UFSD deals with NTFS then we have compressed/sparsed/resident file )
    //
    kaddr = kmap( bh->b_page );
    off   = (unsigned)(Vbo - (bh->b_page->index << PAGE_CACHE_SHIFT));
    data  = kaddr + off;
    if ( off + bh_size > PAGE_CACHE_SIZE )
      bh_size = PAGE_CACHE_SIZE - off;

    Lbn = -1;

    if ( !create ) {

      if ( !ufsd_locked ) {
        LockUfsd( sbi );
        ufsd_locked = 1;
      }

      //
      // Read file via UFSD -> UFSD_BdRead
      //
      DebugTrace(0, UFSD_LEVEL_VFS_GETBLK, ("get_block: use ufsd to read file %llx, %Zx\n", Vbo, bh_size ));

      err = UFSDAPI_FileRead( sbi->Ufsd, u->ufile, NULL, 0, Vbo, bh_size, data, &tmp );

      if ( 0 == err && tmp < bh_size )
        memset( data + tmp, 0, bh_size - tmp );
      DEBUG_ONLY( if ( IsZero( data, tmp ) ) hint2 = ", zero"; )

      flush_dcache_page( bh->b_page );

    } else {

#if defined HAVE_DECL_ASO_WRITEPAGE_V2 && HAVE_DECL_ASO_WRITEPAGE_V2
      if ( ufsd_flags & UFSD_FLAG_WRITEPAGE ) {
        //
        // Write file via UFSD -> UFSD_BdWrite
        // Check to make sure that we are not extending the file
        //
        size_t towrite = Vbo + bh_size > isize ? (size_t)(isize - Vbo) : bh_size;

        if ( !ufsd_locked ) {
          LockUfsd( sbi );
          ufsd_locked = 1;
        }

//        DebugTrace(0, UFSD_LEVEL_VFS_GETBLK, ("get_block: use ufsd to write file: 0x%llx, 0x%Zx%s\n", Vbo, towrite, IsZero(data, towrite)?", zero":"" ));

        err = UFSDAPI_FileWrite( sbi->Ufsd, u->ufile, NULL, 0, Vbo, towrite, data, &tmp );
        if ( 0 == err && u->mmu < Vbo + tmp ) {
          u->mmu = Vbo + tmp;
          dirty = 1;
        }
      }
#endif
      clear_buffer_dirty( bh );
      set_buffer_delay( bh );
    }

    if ( 0 == err ) {
      set_buffer_mapped( bh );    // need to fix_page
      set_buffer_uptodate( bh );
    }

    kunmap( bh->b_page );
#endif // #ifdef UFSD_NTFS

  } else {

    //
    // block is not yet allocated on disk
    //
    TRACE_ONLY( hint2 = ", not allocated"; )

    //
    // Special sign
    //
    if ( ufsd_flags & UFSD_FLAG_PREPARE ) {
      set_buffer_mapped( bh );    // need to fix_page
      set_buffer_uptodate( bh );
      set_buffer_delay( bh );
      bh_size = blocksize;
    }
    err = 0;

  }

#ifdef UFSD_DELAY_ALLOC
  if ( 0 == err && !buffer_mapped( bh ) && (UFSD_FLAG_NOALLOC|UFSD_FLAG_PREPARE) == ufsd_flags ) {

    sector_t md_needed = iblock >> 10;
    long d;

    assert( !buffer_delay( bh ) );

    spin_lock( &u->block_lock );
    d = md_needed - u->i_reserved_meta_blocks;
    if ( d < 0 )
      d = 0;

    //
    // Check for free space: we cannot afford to run out of free blocks
    //
    if ( atomic_long_read( &sbi->FreeBlocks ) < atomic_long_read( &sbi->DirtyBlocks ) + d + 1 + UFSD_RED_ZONE ) {
      TraceFreeSpace( sbi, "nospc!" );
      err = -ENOSPC;
    } else {
      atomic_long_add( d + 1, &sbi->DirtyBlocks );
      u->i_reserved_data_blocks += 1;
      u->i_reserved_meta_blocks += d;
    }

    spin_unlock( &u->block_lock );

    if ( 0 == err ) {
      Lbn = -2;
      set_buffer_mapped( bh );
      set_buffer_new( bh );
      set_buffer_delay( bh );
    }
  }
#endif

  if ( ufsd_locked )
    UnlockUfsd( sbi );

  if ( 0 != err ){
    DebugTrace(-1, UFSD_LEVEL_VFS_GETBLK, ("get_block failed -> %d\n", err ));
  } else {
    //
    // Setup bh
    //
    bh_dev(bh)  = sb_dev(i->i_sb);
    bh->b_size  = bh_size;

    if ( buffer_mapped( bh ) ) {
      bh->b_blocknr = Lbn;
#ifdef UFSD_DELAY_ALLOC
      if ( -2 == Lbn ) {
        DebugTrace(-1, UFSD_LEVEL_VFS_GETBLK, ("get_block -> b=%"PSCT"x,%Zx (delay) %lx, mm=%llx%s%s\n",
                              iblock, bh_size>>i->i_blkbits,
                              bh->b_state, (UINT64)u->mmu, hint1, hint2 ));
      } else
#endif
      if ( -1 != Lbn ) {
        DebugTrace(-1, UFSD_LEVEL_VFS_GETBLK, ("get_block -> b=%"PSCT"x,%Zx => %"PSCT"x,%lx, mm=%llx%s%s\n",
                              iblock, bh_size>>i->i_blkbits, Lbn,
                              bh->b_state, (UINT64)u->mmu, hint1, hint2 ));
      } else {
        DebugTrace(-1, UFSD_LEVEL_VFS_GETBLK, ("get_block -> b=%"PSCT"x,%Zx (to fix) %lx, mm=%llx%s%s\n",
                              iblock, bh_size>>i->i_blkbits,
                              bh->b_state, (UINT64)u->mmu, hint1, hint2 ));
      }
    } else {
      bh->b_blocknr = -1;
      DebugTrace(-1, UFSD_LEVEL_VFS_GETBLK, ("get_block -> b=%"PSCT"x,%Zx (nomap) %lx, mm=%llx%s%s\n",
                            iblock, bh_size>>i->i_blkbits,
                            bh->b_state, (UINT64)u->mmu, hint1, hint2 ));
    }

    if ( dirty )
      mark_inode_dirty( i );
  }
  ProfileLeave( sbi, get_block );
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_get_block_bmap
//
// This function is a callback for ufsd_truncate/ufsd_bmap
///////////////////////////////////////////////////////////
static int
ufsd_get_block_bmap(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  return ufsd_get_block_flags( i, iblock, bh, create, UFSD_FLAG_BMAP );
}


#if !(defined HAVE_DECL_TRUNCATE_SETSIZE && HAVE_DECL_TRUNCATE_SETSIZE)
///////////////////////////////////////////////////////////
// ufsd_truncate
//
// inode_operations::truncate
///////////////////////////////////////////////////////////
static void
ufsd_truncate(
    IN struct inode* i
    )
{
  UINT64 asize;
  UINT64 isize  = i_size_read( i );
  usuper* sbi   = UFSD_SB( i->i_sb );
  unode* u      = UFSD_U( i );
  TRACE_ONLY( const char* hint = ""; )

  DebugTrace(+1, Dbg, ("truncate(r=%lx) -> %llx\n", i->i_ino, isize ) );

#ifdef UFSD_DELAY_ALLOC
  if ( 0 == isize )
    SetFlag( u->i_state_flags, UFSD_STATE_DA_ALLOC_CLOSE );
#endif

  block_truncate_page( i->i_mapping, isize, ufsd_get_block_bmap );

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  if ( 0 == LazyOpen( sbi, i )
    && 0 == UFSDAPI_FileSetSize( u->ufile, isize, NULL, &asize ) )
  {
    i->i_blocks = asize >> 9;
    if ( u->mmu > isize )
      u->mmu  = isize;
  }

  //
  // 'LazyOpen' may change 'i->i_size'
  // Always restore correct value
  //
  i->i_size = isize;

  UnlockUfsd( sbi );

  //
  // Update cache info
  //
  spin_lock( &u->block_lock );
  if ( 0 != u->Len ) {
    long long dVbn = ((isize + (1u<<i->i_blkbits) - 1) >> i->i_blkbits) - u->Vbn;
    if ( dVbn <= 0 ) {
      u->Vbn  = 0;
      u->Lbn  = 0;
      u->Len  = 0;
      TRACE_ONLY( hint = " reset"; )
    } else if ( dVbn < u->Len ) {
      u->Len = dVbn;
      TRACE_ONLY( hint = " truncated"; )
    }
  }
  spin_unlock( &u->block_lock );

  DebugTrace(-1, Dbg, ("truncate -> ok, cache: [%"PSCT"x, %"PSCT"x) => %"PSCT"x%s\n",
                       u->Vbn, u->Vbn + u->Len, u->Lbn, hint ));
}
#endif

///////////////////////////////////////////////////////////
// ufsd_setattr
//
// inode_operations::setattr
///////////////////////////////////////////////////////////
static int
ufsd_setattr(
    IN struct dentry*  de,
    IN struct iattr*   attr
    )
{
  UINT64 isize, ia_size;
  struct inode* i = de->d_inode;
  unode* u        = UFSD_U( i );
  int err;
  int dirty = 0;
  unsigned int ia_valid = attr->ia_valid;

  DebugTrace(+1, Dbg, ("setattr(%x): r=%lx, uid=%d,gid=%d,m=%o,sz=%llx,%llx\n",
                        ia_valid, i->i_ino, i->i_uid.val, i->i_gid.val, i->i_mode,
                        u->mmu, (UINT64)i_size_read(i) ));

  err = inode_change_ok( i, attr );
  if ( err ) {
#ifdef UFSD_DEBUG
    unsigned int fs_uid   = current_fsuid();
    DebugTrace(0, Dbg, ("inode_change_ok failed: \"%s\" current_fsuid=%d, ia_valid=%x\n", current->comm, fs_uid, ia_valid ));
    if ( ia_valid & ATTR_UID )
      DebugTrace(0, Dbg, ("new uid=%d, capable(CAP_CHOWN)=%d\n", attr->ia_uid, capable(CAP_CHOWN) ));

    if ( ia_valid & ATTR_GID )
      DebugTrace(0, Dbg, ("new gid=%d, in_group_p=%d, capable(CAP_CHOWN)=%d\n", attr->ia_gid, in_group_p(attr->ia_gid), capable(CAP_CHOWN) ));

    if ( ia_valid & ATTR_MODE )
      DebugTrace(0, Dbg, ("new mode=%o, is_owner_or_cap=%d\n", (unsigned)attr->ia_mode, (int)is_owner_or_cap(i) ));

#ifndef ATTR_TIMES_SET
  #define ATTR_TIMES_SET  (1 << 16)
#endif
    if ( ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET) )
      DebugTrace(0, Dbg, ("new times, is_owner_or_cap=%d\n", (int)is_owner_or_cap(i) ));
#endif
    goto out;
  }

  if ( (ia_valid & ATTR_SIZE) && ( isize = i->i_size ) != ( ia_size = attr->ia_size ) ) {
    UINT64 asize;
    int Up = ia_size >= isize;

    if ( u->encrypt ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("setattr: attempt to resize encrypted file\n" ) );
      err = -ENOSYS;
      goto out;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)
    inode_dio_wait( i );
#endif

#if defined HAVE_DECL_TRUNCATE_SETSIZE && HAVE_DECL_TRUNCATE_SETSIZE
    //
    // 2.6.36++ way
    //
    {
      TRACE_ONLY( const char* hint = Up? "expand":"truncate"; )
      usuper* sbi   = UFSD_SB( i->i_sb );
      DebugTrace(+1, Dbg, ("%s: %llx -> %llx%s\n", hint, isize, ia_size, u->sparse?" ,sp" : "" ) );

      if ( !Up ) {
#ifdef UFSD_DELAY_ALLOC
        if ( 0 == ia_size )
          SetFlag( u->i_state_flags, UFSD_STATE_DA_ALLOC_CLOSE );
#endif
//        block_truncate_page( i->i_mapping, isize, ufsd_get_block_bmap );
      }

      LockUfsd( sbi );

      err = LazyOpen( sbi, i );

      if ( 0 == err ) {

        err = UFSDAPI_FileSetSize( u->ufile, ia_size, NULL, &asize );

        if ( 0 == err ) {
          if ( u->mmu > ia_size )
            u->mmu  = ia_size;
          i->i_blocks = asize >> 9;
          dirty = 1;
        } else
          // If no free space we should return -EFBIG or -EINVAL
          //err = ERR_NOSPC == err? -ENOSPC : -EINVAL;
          err = -EINVAL;
      }

      UnlockUfsd( sbi );

      if ( 0 != err ) {
        DebugTrace(-1, Dbg, ("%s failed -> %d\n", hint, err ) );
        goto out;
      }

      //
      // Always restore original i->i_size for 'truncate_setsize' (may be change in LazyOpen)
      //
      i->i_size = isize;

      truncate_setsize( i, ia_size );

      if ( !Up ) {
        //
        // Update cache info
        //
        spin_lock( &u->block_lock );
        if ( 0 != u->Len ) {
          long long dVbn = ((ia_size + (1u<<i->i_blkbits) - 1) >> i->i_blkbits) - u->Vbn;
          if ( dVbn <= 0 ) {
            u->Vbn  = 0;
            u->Lbn  = 0;
            u->Len  = 0;
          } else if ( dVbn < u->Len ) {
            u->Len = dVbn;
          }
        }
        spin_unlock( &u->block_lock );
      }
      DebugTrace(-1, Dbg, ("%s -> ok, mmu=%llx\n", hint, (UINT64)u->mmu ) );
    }
#else
    //
    // 2.6.36-- way
    //
    if ( Up ) {
      usuper* sbi   = UFSD_SB( i->i_sb );
      DebugTrace(+1, Dbg, ("expand: %llx -> %llx%s\n", isize, ia_size, u->sparse?" ,sp" : "" ) );

      LockUfsd( sbi );

      err = LazyOpen( sbi, i );

      if ( 0 == err ) {

        err = UFSDAPI_FileSetSize( u->ufile, ia_size, NULL, &asize );

        if ( 0 == err ) {
          i->i_size   = ia_size;
          i->i_blocks = asize >> 9;
          dirty = 1;
        } else
          // If no free space we should return -EFBIG or -EINVAL
          //err = ERR_NOSPC == err? -ENOSPC : -EINVAL;
          err = -EINVAL;
      }

      UnlockUfsd( sbi );

      if ( 0 == err ) {
        DebugTrace(-1, Dbg, ("expand -> ok, mmu=%llx\n", (UINT64)u->mmu ) );
      } else {
        DebugTrace(-1, Dbg, ("expand failed -> %d\n", err ) );
        goto out;
      }
    } else {
      DebugTrace(0, Dbg, ("vmtruncate: %llx -> %llx\n", isize, ia_size ));
      //err = vmtruncate( i, ia_size );
	err = inode_newsize_ok(i, attr->ia_size);
      if ( err ) {
        DebugTrace(0, Dbg, ("vmtruncate failed %d\n", err ));
        goto out;
      }
	
	truncate_setsize(i, attr->ia_size);
	
      assert( ia_size == i->i_size );
      dirty = 1;
    }
#endif

    if ( dirty )
      TIMESPEC_SECONDS( &i->i_mtime ) = TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds();
  }

  //
  // Do smart setattr_copy
  //
  if ( (ia_valid & ATTR_UID) && !uid_eq(i->i_uid, attr->ia_uid) ) {
    dirty = 1;
    i->i_uid = attr->ia_uid;
    u->set_mode = 1;
  }
  if ( (ia_valid & ATTR_GID) && !gid_eq(i->i_gid , attr->ia_gid) ) {
    dirty = 1;
    i->i_gid = attr->ia_gid;
    u->set_mode = 1;
  }
  if ( (ia_valid & ATTR_ATIME) && 0 != timespec_compare( &i->i_atime, &attr->ia_atime ) ) {
    dirty = 1;
    i->i_atime = attr->ia_atime;
  }
  if ( (ia_valid & ATTR_MTIME) && 0 != timespec_compare( &i->i_mtime, &attr->ia_mtime ) ) {
    dirty = 1;
    i->i_mtime = attr->ia_mtime;
  }
  if ( (ia_valid & ATTR_CTIME) && 0 != timespec_compare( &i->i_ctime, &attr->ia_ctime ) ) {
    dirty = 1;
    i->i_ctime = attr->ia_ctime;
  }
  if ( ia_valid & ATTR_MODE ) {
    umode_t mode = attr->ia_mode;
    if (!in_group_p(i->i_gid) && !capable(CAP_FSETID))
      mode &= ~S_ISGID;

    if ( i->i_mode != mode ) {
      dirty = 1;
      i->i_mode = mode;
      u->set_mode = 1;
    }

#ifdef UFSD_USE_XATTR
    err = ufsd_acl_chmod( i );
    if ( err )
      goto out;
#endif
  }

//  DropPages( i->i_mapping );
out:
  if ( dirty )
    mark_inode_dirty( i );

  DebugTrace(-1, Dbg, ("setattr -> %d, uid=%d,gid=%d,m=%o,sz=%llx,%llx%s\n", err,
                        i->i_uid.val, i->i_gid.val, i->i_mode,
                        u->mmu, (UINT64)i_size_read(i), FlagOn(i->i_state, I_DIRTY)?",d":"" ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_rename
//
// inode_operations::rename
///////////////////////////////////////////////////////////
static int
ufsd_rename(
    IN struct inode*  odir,
    IN struct dentry* ode,
    IN struct inode*  ndir,
    IN struct dentry* nde
    )
{
  int err, force_da_alloc = 0;
  usuper* sbi = UFSD_SB( odir->i_sb );

  assert( NULL != UFSD_FH( odir ) );

  DebugTrace(+1, Dbg, ("rename: r=%lx, %p('%.*s') => r=%lx, %p('%.*s')\n",
                      odir->i_ino, ode,
                      (int)ode->d_name.len, ode->d_name.name,
                      ndir->i_ino, nde,
                      (int)nde->d_name.len, nde->d_name.name ));

  //
  // If the target already exists, delete it first.
  // I will not unwind it on move failure. Although it's a weak point
  // it's better to not have it implemented then trying to create
  // a complex workaround.
  //
  if ( NULL != nde->d_inode ) {

    DebugTrace(0, Dbg, ("rename: deleting existing target %p (r=%lx)\n", nde->d_inode, nde->d_inode->i_ino));

    dget( nde );
    err = ufsd_unlink( ndir, nde );
    dput( nde );
    if ( err ) {
      DebugTrace(-1, Dbg, ("rename -> failed to unlink target, %d\n", err));
      return err;
    }
    force_da_alloc = 1;
  }

  LockUfsd( sbi );

  // Call UFSD library
  err = UFSDAPI_FileMove( sbi->Ufsd, UFSD_FH( odir ), UFSD_FH( ndir ), UFSD_FH( ode->d_inode ),
                          ode->d_name.name, ode->d_name.len,
                          nde->d_name.name, nde->d_name.len );
  // Translate UFSD errors
  switch( err ) {
  case 0: break;
  case ERR_NOFILEEXISTS: err = -ENOENT; break;
  case ERR_FILEEXISTS: err = -EEXIST; break;
  case ERR_NOSPC: err = -ENOSPC; break;
  default: err = -EPERM;
  }

  UnlockUfsd( sbi );

  if ( 0 != err ) {
    DebugTrace(-1, Dbg, ("rename -> failed, %d\n", err));
    return err;
  }


  // Mark dir as requiring resync.
  odir->i_version += 1;
  TIMESPEC_SECONDS( &odir->i_ctime ) = TIMESPEC_SECONDS( &odir->i_mtime ) = get_seconds();
  mark_inode_dirty( odir );
  mark_inode_dirty( ndir );
  odir->i_size   = UFSDAPI_GetDirSize( UFSD_FH( odir ) );
  odir->i_blocks = odir->i_size >> 9;

  if ( ndir != odir ) {

    ndir->i_version += 1;
    ndir->i_mtime  = ndir->i_ctime = odir->i_ctime;
    ndir->i_size   = UFSDAPI_GetDirSize( UFSD_FH( ndir ) );
    ndir->i_blocks = ndir->i_size >> 9;

#ifdef UFSD_COUNT_CONTAINED
    if ( S_ISDIR( ode->d_inode->i_mode ) ) {
      assert(odir->i_nlink > 0);
      odir->i_nlink -= 1;
      ndir->i_nlink += 1;
    }
#endif
  }

  if ( NULL != ode->d_inode ) {
    ode->d_inode->i_ctime = odir->i_ctime;
    mark_inode_dirty( ode->d_inode );
#ifdef UFSD_DELAY_ALLOC
  if ( force_da_alloc )
    ufsd_alloc_da_blocks( ode->d_inode );
#endif
  }

  DebugTrace(-1, Dbg, ("rename -> %d\n", err));
  return 0;
}


#ifdef UFSD_USE_XATTR

#if defined HAVE_DECL_POSIX_ACL_TO_XATTR_V2 && HAVE_DECL_POSIX_ACL_TO_XATTR_V2
#if defined HAVE_LINUX_PROC_NS_H && HAVE_LINUX_PROC_NS_H
  #include <linux/proc_ns.h>
#endif
#include <linux/user_namespace.h>
  // Wait for 'init_user_ns' to be non G.P.L.
  struct user_namespace user_ns = {
    .uid_map    = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
    .gid_map    = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
    .projid_map = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
#if defined HAVE_STRUCT_USER_NAMESPACE_COUNT && HAVE_STRUCT_USER_NAMESPACE_COUNT
    .count = ATOMIC_INIT(3),
#else
    .kref = { .refcount = ATOMIC_INIT(3), },
#endif
    .owner = GLOBAL_ROOT_UID,
    .group = GLOBAL_ROOT_GID,
#if defined HAVE_STRUCT_USER_NAMESPACE_PROC_INUM && HAVE_STRUCT_USER_NAMESPACE_PROC_INUM
    .proc_inum = PROC_USER_INIT_INO,
#endif
  };
  #define Posix_acl_to_xattr( acl, buffer, size )   posix_acl_to_xattr( &user_ns, acl, buffer, size )
  #define Posix_acl_from_xattr( value, size )       posix_acl_from_xattr( &user_ns, value, size )
#else
  #define Posix_acl_to_xattr( acl, buffer, size )   posix_acl_to_xattr( acl, buffer, size )
  #define Posix_acl_from_xattr( value, size )       posix_acl_from_xattr( value, size )
#endif


///////////////////////////////////////////////////////////
// ufsd_listxattr
//
// inode_operations::listxattr
//
// Copy a list of attribute names into the buffer
// provided, or compute the buffer size required.
// Buffer is NULL to compute the size of the buffer required.
//
// Returns a negative error number on failure, or the number of bytes
// used / required on success.
///////////////////////////////////////////////////////////
ssize_t
static ufsd_listxattr(
    IN  struct dentry*  de,
    OUT char*           buffer,
    IN  size_t          size
    )
{
  struct inode* i = de->d_inode;
  unode* u        = UFSD_U( i );
  usuper* sbi     = UFSD_SB( i->i_sb );
  ssize_t ret;

  DebugTrace(+1, Dbg, ("listxattr: r=%lx, %p, %Zu\n", i->i_ino, buffer, size ));

  LockUfsd( sbi );

  ret = LazyOpen( sbi, i );

  if ( 0 == ret ) {
    switch( UFSDAPI_ListXAttr( sbi->Ufsd, u->ufile, buffer, size, (size_t*)&ret ) ){
    case 0                  : break; // Ok
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    case ERR_MORE_DATA      : ret = -ERANGE; break;
    default                 : ret = -EINVAL;
    }
  }

  UnlockUfsd( sbi );

  DebugTrace(-1, Dbg, ("listxattr -> %Zd\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_getxattr
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_getxattr(
    IN  struct inode* i,
    IN  const char*   name,
    OUT void*         value,
    IN  size_t        size,
    OUT size_t*       required
    )
{
  unode* u    = UFSD_U( i );
  usuper* sbi = UFSD_SB( i->i_sb );
  int ret;
  size_t len;
  if ( NULL != u->ufile && !u->xattr )
    return -ENODATA;

  DebugTrace(+1, Dbg, ("getxattr: r=%lx, \"%s\", %p, %Zu\n", i->i_ino, name, value, size ));

  if ( NULL == required )
    LockUfsd( sbi );

  ret = LazyOpen( sbi, i );

  if ( 0 == ret ) {
    switch( UFSDAPI_GetXAttr( sbi->Ufsd, u->ufile,
                              name, strlen(name), value, size, &len ) ) {
    case 0                  : ret = (int)len; break;  // Ok
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    case ERR_MORE_DATA      : ret = -ERANGE; if ( NULL != required ) *required = len; break;
    case ERR_NOFILEEXISTS   : ret = -ENODATA; break;
    default                 : ret = -EINVAL;
    }
  }

  if ( NULL == required )
    UnlockUfsd( sbi );

  DebugTrace(-1, Dbg, ("getxattr -> %d\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_setxattr
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_setxattr(
    IN struct inode*  i,
    IN const char*    name,
    IN const void*    value,
    IN size_t         size,
    IN int            flags
    )
{
  unode* u    = UFSD_U( i );
  usuper* sbi = UFSD_SB( i->i_sb );
  int ret;

  DebugTrace(+1, Dbg, ("setxattr: r=%lx \"%s\", %p, %Zu, %d\n",
                        i->i_ino, name, value, size, flags ));

  LockUfsd( sbi );

  ret = LazyOpen( sbi, i );

  if ( 0 == ret ) {
    switch( UFSDAPI_SetXAttr( sbi->Ufsd, u->ufile,
                              name, strlen(name), value, size,
                              0 != (flags & XATTR_CREATE),
                              0 != (flags & XATTR_REPLACE) ) ) {
    case 0:
      // Check if we delete the last xattr ( 0 == size && XATTR_REPLACE == flags && no xattrs )
      u->xattr = 0 != size
        || XATTR_REPLACE != flags
        || 0 != UFSDAPI_ListXAttr( sbi->Ufsd, u->ufile, NULL, 0, &size )
        || 0 != size;
      if ( 0 == u->xattr ) {
        DebugTrace(0, Dbg, ("setxattr: (removed last extended attribute)\n" ));
      }
      ret = 0;
      break;  // Ok
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    case ERR_NOFILEEXISTS   : ret = -ENODATA; break;
    default                 : ret = -EINVAL;
    }
  }

  UnlockUfsd( sbi );

  DebugTrace(-1, Dbg, ("setxattr -> %d\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_posix_acl_release
//
//
///////////////////////////////////////////////////////////
static inline void
ufsd_posix_acl_release(
    IN struct posix_acl *acl
    )
{
  assert( NULL != acl );
  if ( atomic_dec_and_test( &acl->a_refcount ) )
    kfree( acl );
}


///////////////////////////////////////////////////////////
// ufsd_get_acl
//
// inode_operations::get_acl
// inode->i_mutex: don't care
///////////////////////////////////////////////////////////
static struct posix_acl*
ufsd_get_acl(
    IN struct inode*  i,
    IN int            type
    )
{
  const char* name;
  struct posix_acl* acl, **p;
  size_t req;
  int ret;
  usuper* sbi = UFSD_SB( i->i_sb );
  unode* u    = UFSD_U( i );

  assert( sbi->options.acl );

	switch ( type ) {
#if defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL
	case ACL_TYPE_ACCESS:   p = &i->i_acl; break;
	case ACL_TYPE_DEFAULT:  p = &i->i_default_acl; break;
#else
	case ACL_TYPE_ACCESS:   p = &u->acl; break;
	case ACL_TYPE_DEFAULT:  p = &u->default_acl; break;
#endif
	default:  return ERR_PTR(-EINVAL);
	}

  //
  // Check cached value of 'acl' and 'default_acl'
  //
  spin_lock( &i->i_lock );
	acl = *p;
  if ( ACL_NOT_CACHED != acl )
    acl = posix_acl_dup( acl );
  else if ( NULL != u->ufile && !u->xattr )
    acl = NULL;
  spin_unlock( &i->i_lock );

  if ( ACL_NOT_CACHED != acl )
    return acl;

  //
  // Possible values of 'type' was already checked above
  //
  name = ACL_TYPE_ACCESS == type? POSIX_ACL_XATTR_ACCESS : POSIX_ACL_XATTR_DEFAULT;

  LockUfsd( sbi );

  //
  // Get the size of extended attribute
  //
  ret = ufsd_getxattr( i, name, sbi->Xbuffer, sbi->BytesPerXBuffer, &req );

  if ( (ret > 0 && NULL == sbi->Xbuffer) || -ERANGE == ret ) {

    //
    // Allocate/Reallocate buffer and read again
    //
    if ( NULL != sbi->Xbuffer ) {
      assert( -ERANGE == ret );
      kfree( sbi->Xbuffer );
    }

    if ( ret > 0 )
      req = ret;

    sbi->Xbuffer = kmalloc( req, GFP_KERNEL );
    if ( NULL != sbi->Xbuffer ) {
      sbi->BytesPerXBuffer = req;

      //
      // Read the extended attribute.
      //
      ret = ufsd_getxattr( i, name, sbi->Xbuffer, sbi->BytesPerXBuffer, &req );
      assert( ret > 0 );

    } else {
      ret = -ENOMEM;
      sbi->BytesPerXBuffer = 0;
    }
  }

  UnlockUfsd( sbi );

  //
  // Translate extended attribute to acl
  //
  if ( ret > 0 ) {
    acl = Posix_acl_from_xattr( sbi->Xbuffer, ret );
    if ( !IS_ERR( acl ) ) {
	    struct posix_acl *old;
      spin_lock( &i->i_lock );
      old = *p;
      *p  = posix_acl_dup( acl );
      spin_unlock( &i->i_lock );
	    if ( ACL_NOT_CACHED != old )
		    ufsd_posix_acl_release( old );
    }
  } else {
    acl = -ENODATA == ret || -ENOSYS == ret ? NULL : ERR_PTR( ret );
  }

  return acl;
}


///////////////////////////////////////////////////////////
// ufsd_set_acl
//
///////////////////////////////////////////////////////////
static int
ufsd_set_acl(
    IN struct inode*      i,
    IN int                type,
    IN struct posix_acl*  acl
    )
{
  const char* name;
  void* value = NULL;
  size_t size = 0;
  int err     = 0;

  if ( S_ISLNK( i->i_mode ) )
    return -EOPNOTSUPP;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;

  switch( type ) {
    case ACL_TYPE_ACCESS:
      if ( acl ) {
        posix_acl_mode mode = i->i_mode;
        err = posix_acl_equiv_mode( acl, &mode );
        if ( err < 0 )
          return err;

        i->i_mode = mode;
        mark_inode_dirty( i );
        if ( 0 == err )
          acl = NULL;
      }
      name = POSIX_ACL_XATTR_ACCESS;
      break;

    case ACL_TYPE_DEFAULT:
      if ( !S_ISDIR(i->i_mode) )
        return acl ? -EACCES : 0;
      name = POSIX_ACL_XATTR_DEFAULT;
      break;

    default:
      return -EINVAL;
  }

  if ( NULL != acl ) {
    size  = posix_acl_xattr_size( acl->a_count );
    value = kmalloc( size, GFP_KERNEL );
    if ( NULL == value )
      return -ENOMEM;

    err = Posix_acl_to_xattr( acl, value, size );
  }

  if ( 0 != err ) {
    err = ufsd_setxattr( i, name, value, size, 0 );
    if ( 0 == err ) {
#if defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL
      struct posix_acl** p  = ACL_TYPE_ACCESS == type? &i->i_acl : &i->i_default_acl;
#else
      unode* u = UFSD_U(i);
      struct posix_acl** p  = ACL_TYPE_ACCESS == type? &u->acl : &u->default_acl;
#endif
	    struct posix_acl* old;
      spin_lock( &i->i_lock );
      old = *p;
      *p  = posix_acl_dup( acl );
      spin_unlock( &i->i_lock );
	    if ( ACL_NOT_CACHED != old )
		    ufsd_posix_acl_release( old );
    }
  }

  kfree( value );

  return err;
}


#if defined HAVE_DECL_GENERIC_PERMISSION_V3 && HAVE_DECL_GENERIC_PERMISSION_V3

///////////////////////////////////////////////////////////
// inode_operations::permission = generic_permission
//
// generic_permission does not require 'check_acl'
///////////////////////////////////////////////////////////
#define ufsd_permission generic_permission

#else

///////////////////////////////////////////////////////////
// ufsd_check_acl
//
//
///////////////////////////////////////////////////////////
static int
ufsd_check_acl(
    IN struct inode*  i,
    IN int            mask
#ifdef IPERM_FLAG_RCU
    , IN unsigned int flags
#endif
    )
{
  int err;
  struct posix_acl *acl;

  assert( UFSD_SB( i->i_sb )->options.acl );

#ifdef IPERM_FLAG_RCU
  if ( flags & IPERM_FLAG_RCU ) {
    if ( !negative_cached_acl( i, ACL_TYPE_ACCESS ) )
      return -ECHILD;
    return -EAGAIN;
  }
#endif

  acl = ufsd_get_acl( i, ACL_TYPE_ACCESS );
  if ( IS_ERR(acl) )
    return PTR_ERR(acl);

  if ( NULL == acl )
    return -EAGAIN;

  //
  // Trace acl
  //
#if 0//def UFSD_DEBUG
  {
    int n;
    for ( n = 0; n < acl->a_count; n++ ) {
      DebugTrace(0, Dbg, ("e_tag=%x, e_perm=%x e_id=%x\n", (unsigned)acl->a_entries[n].e_tag, (unsigned)acl->a_entries[n].e_perm, (unsigned)acl->a_entries[n].e_id ));
    }
  }
#endif

  err = posix_acl_permission( i, acl, mask );
  ufsd_posix_acl_release( acl );

  DebugTrace(0, Dbg, ("check_acl (r=%lx, m=%o) -> %d\n", i->i_ino, mask, err) );

  return err;
}


#ifdef IPERM_FLAG_RCU
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode*  i,
    IN int            mask,
    IN unsigned int   flag
    )
{
  return generic_permission( i, mask, flag, ufsd_check_acl );
}
#else
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode*  i,
    IN int            mask
#if defined HAVE_DECL_INOP_PERMISSION_V1 & HAVE_DECL_INOP_PERMISSION_V1
    , IN struct nameidata* nd __attribute__((__unused__))
#endif
#if defined HAVE_DECL_INOP_PERMISSION_V2 & HAVE_DECL_INOP_PERMISSION_V2
    , IN unsigned int ui __attribute__((__unused__))
#endif
    )
{
  return generic_permission( i, mask, ufsd_check_acl );
}
#endif // #ifdef IPERM_FLAG_RCU
#endif // #if defined HAVE_DECL_GENERIC_PERMISSION_V3 && HAVE_DECL_GENERIC_PERMISSION_V3


///////////////////////////////////////////////////////////
// ufsd_acl_chmod
//
//
///////////////////////////////////////////////////////////
static int
ufsd_acl_chmod(
    IN struct inode* i
    )
{
  struct posix_acl *acl;
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;

  if ( S_ISLNK( i->i_mode ) )
    return -EOPNOTSUPP;

  DebugTrace(+1, Dbg, ("acl_chmod r=%lx\n", i->i_ino));

  acl = ufsd_get_acl( i, ACL_TYPE_ACCESS );
  if ( IS_ERR(acl) || !acl )
    err = PTR_ERR(acl);
  else {
#if defined HAVE_DECL_POSIX_ACL_CHMOD && HAVE_DECL_POSIX_ACL_CHMOD
    err = posix_acl_chmod( &acl, GFP_KERNEL, i->i_mode );
    if ( err )
       return err;
    err = ufsd_set_acl( i, ACL_TYPE_ACCESS, acl );
    ufsd_posix_acl_release( acl );
#else
    struct posix_acl* clone = posix_acl_clone( acl, GFP_KERNEL );
    ufsd_posix_acl_release( acl );
    if ( NULL == clone )
      err = -ENOMEM;
    else {
      err = posix_acl_chmod_masq( clone, i->i_mode );
      if ( 0 == err )
        err = ufsd_set_acl( i, ACL_TYPE_ACCESS, clone );
      ufsd_posix_acl_release( clone );
    }
#endif
  }

  DebugTrace(-1, Dbg, ("acl_chmod -> %d\n", err));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_get_acl
//
// Helper function for ufsd_xattr_acl_access_get/ufsd_xattr_acl_default_get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_get_acl(
    IN struct inode*  i,
    IN int            type,
    OUT void*         buffer,
    IN size_t         size
    )
{
  struct posix_acl* acl;
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return -EOPNOTSUPP;

  acl = ufsd_get_acl( i, type );
  if ( IS_ERR(acl) )
    return PTR_ERR(acl);

  if ( NULL == acl )
    return -ENODATA;

  err = Posix_acl_to_xattr( acl, buffer, size );
  ufsd_posix_acl_release( acl );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_set_acl
//
// Helper function for ufsd_xattr_acl_access_set/ufsd_xattr_acl_default_set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_set_acl(
    IN struct inode*  i,
    IN int            type,
    IN const void*    value,
    IN size_t         size
    )
{
  struct posix_acl* acl;
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return -EOPNOTSUPP;

  if ( !is_owner_or_cap( i ) )
    return -EPERM;

  if ( NULL != value ) {
    acl = Posix_acl_from_xattr( value, size );
    if ( IS_ERR(acl) )
      return PTR_ERR(acl);

    if ( NULL != acl ) {
      err = posix_acl_valid( acl );
      if ( err )
        goto release_and_out;
    }
  } else
    acl = NULL;

  err = ufsd_set_acl( i, type, acl );

release_and_out:
  ufsd_posix_acl_release( acl );
  return err;
}


#if defined HAVE_DECL_XATTR_HANDLER_V2 & HAVE_DECL_XATTR_HANDLER_V2

  #define DECLARE_XATTR_LIST_ARG  struct dentry* de, char* list, size_t list_size, const char* name, size_t name_len, int handler_flags
  #define DECLARE_XATTR_GET_ARG   struct dentry* de, const char* name, void* buffer, size_t size, int handler_flags
  #define DECLARE_XATTR_SET_ARG   struct dentry* de, const char* name, const void* value, size_t size, int flags, int handler_flags
  #define DECLARE_XATTR_INODE     struct inode* i = de->d_inode;

#else

  #define DECLARE_XATTR_LIST_ARG  struct inode* i, char* list, size_t list_size, const char* name, size_t name_len
  #define DECLARE_XATTR_GET_ARG   struct inode* i, const char* name, void* buffer, size_t size
  #define DECLARE_XATTR_SET_ARG   struct inode* i, const char* name, const void* value, size_t size, int flags
  #define DECLARE_XATTR_INODE

#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_acl_access_list
//
// ufsd_xattr_acl_access_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_acl_access_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  DECLARE_XATTR_INODE
  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;
  if ( NULL != list && list_size >= sizeof(POSIX_ACL_XATTR_ACCESS) )
    memcpy( list, POSIX_ACL_XATTR_ACCESS, sizeof(POSIX_ACL_XATTR_ACCESS) );
  return sizeof(POSIX_ACL_XATTR_ACCESS);
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_access_get
//
// ufsd_xattr_acl_access_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_access_get(
    DECLARE_XATTR_GET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace(+1, Dbg, ("acl_access_get: r=%lx, \"%s\", sz=%u\n", i->i_ino, name, (unsigned)size ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_get_acl( i, ACL_TYPE_ACCESS, buffer, size );

  DebugTrace(-1, Dbg, ("acl_access_get -> %d\n", ret ));

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_access_set
//
// ufsd_xattr_acl_access_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_access_set(
    DECLARE_XATTR_SET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace(+1, Dbg, ("acl_access_set: r=%lx, \"%s\", sz=%u, fl=%d\n", i->i_ino, name, (unsigned)size, flags ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_set_acl( i, ACL_TYPE_ACCESS, value, size );

  DebugTrace(-1, Dbg, ("acl_access_set -> %d\n", ret ));

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_default_list
//
// ufsd_xattr_acl_default_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_acl_default_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  DECLARE_XATTR_INODE
  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;
  if ( NULL != list && list_size >= sizeof(POSIX_ACL_XATTR_DEFAULT) )
    memcpy( list, POSIX_ACL_XATTR_DEFAULT, sizeof(POSIX_ACL_XATTR_DEFAULT) );
  return sizeof(POSIX_ACL_XATTR_DEFAULT);
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_default_get
//
// ufsd_xattr_acl_default_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_default_get(
    DECLARE_XATTR_GET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace(+1, Dbg, ("acl_default_get: r=%lx, \"%s\", sz=%u\n", i->i_ino, name, (unsigned)size ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_get_acl( i, ACL_TYPE_DEFAULT, buffer, size );

  DebugTrace(-1, Dbg, ("acl_default_get -> %d\n", ret ));

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_default_set
//
// ufsd_xattr_acl_default_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_default_set(
    DECLARE_XATTR_SET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace(+1, Dbg, ("acl_default_set: r=%lx, \"%s\", sz=%u, fl=%d\n", i->i_ino, name, (unsigned)size, flags ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_set_acl( i, ACL_TYPE_DEFAULT, value, size );

  DebugTrace(-1, Dbg, ("acl_default_set -> %d\n", ret ));

  return ret;
}

#ifndef XATTR_USER_PREFIX_LEN
  #define XATTR_USER_PREFIX "user."
  #define XATTR_USER_PREFIX_LEN (sizeof (XATTR_USER_PREFIX) - 1)
#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_user_list
//
// ufsd_xattr_user_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_user_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  const size_t prefix_len = XATTR_USER_PREFIX_LEN;
  const size_t total_len = prefix_len + name_len + 1;
  DECLARE_XATTR_INODE

  if ( !UFSD_SB( i->i_sb )->options.user_xattr )
    return 0;

  if ( NULL != list && total_len <= list_size ) {
    memcpy( list, XATTR_USER_PREFIX, prefix_len );
    memcpy( list+prefix_len, name, name_len );
    list[prefix_len + name_len] = 0;
  }
  return total_len;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_user_get
//
// ufsd_xattr_user_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_user_get(
    DECLARE_XATTR_GET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  ret = 0 == name[0]
    ? -EINVAL
    : !UFSD_SB( i->i_sb )->options.user_xattr
    ? -EOPNOTSUPP
    : ufsd_getxattr( i, name - XATTR_USER_PREFIX_LEN, buffer, size, NULL );

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_user_set
//
// ufsd_xattr_user_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_user_set(
    DECLARE_XATTR_SET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]
    ? -EINVAL
    : !UFSD_SB( i->i_sb )->options.user_xattr
    ? -EOPNOTSUPP
    : ufsd_setxattr( i, name - XATTR_USER_PREFIX_LEN, value, size, flags );
}

#ifndef XATTR_TRUSTED_PREFIX_LEN
  #define XATTR_TRUSTED_PREFIX "trusted."
  #define XATTR_TRUSTED_PREFIX_LEN (sizeof (XATTR_TRUSTED_PREFIX) - 1)
#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_trusted_list
//
// ufsd_xattr_trusted_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_trusted_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  const int prefix_len    = XATTR_TRUSTED_PREFIX_LEN;
  const size_t total_len  = prefix_len + name_len + 1;

  if ( !capable( CAP_SYS_ADMIN ) )
    return 0;

  if ( NULL != list && total_len <= list_size ) {
    memcpy( list, XATTR_TRUSTED_PREFIX, prefix_len );
    memcpy( list+prefix_len, name, name_len );
    list[prefix_len + name_len] = 0;
  }
  return total_len;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_trusted_get
//
// ufsd_xattr_trusted_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_trusted_get(
    DECLARE_XATTR_GET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_getxattr( i, name - XATTR_TRUSTED_PREFIX_LEN, buffer, size, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_xattr_trusted_set
//
// ufsd_xattr_trusted_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_trusted_set(
    DECLARE_XATTR_SET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_setxattr( i, name - XATTR_TRUSTED_PREFIX_LEN, value, size, flags );
}

#ifndef XATTR_SECURITY_PREFIX_LEN
  #define XATTR_SECURITY_PREFIX "security."
  #define XATTR_SECURITY_PREFIX_LEN (sizeof (XATTR_SECURITY_PREFIX) - 1)
#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_security_list
//
// ufsd_xattr_security_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_security_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  const int prefix_len    = XATTR_SECURITY_PREFIX_LEN;
  const size_t total_len  = prefix_len + name_len + 1;

  if ( NULL != list && total_len <= list_size ) {
    memcpy( list, XATTR_SECURITY_PREFIX, prefix_len );
    memcpy( list+prefix_len, name, name_len );
    list[prefix_len + name_len] = 0;
  }
  return total_len;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_security_get
//
// ufsd_xattr_security_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_security_get(
    DECLARE_XATTR_GET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_getxattr( i, name - XATTR_SECURITY_PREFIX_LEN, buffer, size, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_xattr_security_set
//
// ufsd_xattr_security_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_security_set(
    DECLARE_XATTR_SET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_setxattr( i, name - XATTR_SECURITY_PREFIX_LEN, value, size, flags );
}


STATIC_CONST struct xattr_handler ufsd_xattr_acl_access_handler = {
  .prefix = POSIX_ACL_XATTR_ACCESS,
  .list   = ufsd_xattr_acl_access_list,
  .get    = ufsd_xattr_acl_access_get,
  .set    = ufsd_xattr_acl_access_set,
};


STATIC_CONST struct xattr_handler ufsd_xattr_acl_default_handler = {
  .prefix = POSIX_ACL_XATTR_DEFAULT,
  .list   = ufsd_xattr_acl_default_list,
  .get    = ufsd_xattr_acl_default_get,
  .set    = ufsd_xattr_acl_default_set,
};


STATIC_CONST struct xattr_handler ufsd_xattr_user_handler = {
  .prefix = XATTR_USER_PREFIX,
  .list   = ufsd_xattr_user_list,
  .get    = ufsd_xattr_user_get,
  .set    = ufsd_xattr_user_set,
};


STATIC_CONST struct xattr_handler ufsd_xattr_trusted_handler = {
  .prefix = XATTR_TRUSTED_PREFIX,
  .list   = ufsd_xattr_trusted_list,
  .get    = ufsd_xattr_trusted_get,
  .set    = ufsd_xattr_trusted_set,
};


STATIC_CONST struct xattr_handler ufsd_xattr_security_handler = {
  .prefix = XATTR_SECURITY_PREFIX,
  .list   = ufsd_xattr_security_list,
  .get    = ufsd_xattr_security_get,
  .set    = ufsd_xattr_security_set,
};


//
// xattr operations
// super_block::s_xattr
//
STATIC_CONST struct xattr_handler* ufsd_xattr_handlers[] = {
  &ufsd_xattr_user_handler,
  &ufsd_xattr_trusted_handler,
  &ufsd_xattr_acl_access_handler,
  &ufsd_xattr_acl_default_handler,
  &ufsd_xattr_security_handler,
  NULL
};

#endif // #ifdef UFSD_USE_XATTR

///////////////////////////////////////////////////////////
// ufsd_lookup
//
// inode_operations::lookup
//
//  This routine is a callback used to load inode for a
//  direntry when this direntry was not found in dcache.
//
// dir - container inode for this operation.
//
// dentry - On entry contains name of the entry to find.
//          On exit should contain inode loaded.
//
// Return:
// struct dentry* - direntry in case of one differs from one
//     passed to me. I return NULL to indicate original direntry has been used.
//     ERRP() can also be returned to indicate error condition.
//
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_lookup(
    IN struct inode*  dir,
    IN struct dentry* de
#if defined HAVE_DECL_INOP_LOOKUP_V2 && HAVE_DECL_INOP_LOOKUP_V2
  , IN struct nameidata * nd  __attribute__((__unused__))
#elif defined HAVE_DECL_INOP_LOOKUP_V3 && HAVE_DECL_INOP_LOOKUP_V3
  , IN unsigned int nd  __attribute__((__unused__))
#endif
    )
{
  struct inode* i = NULL;
  int err = ufsd_create_or_open( dir, de, NULL, &i );

#if defined HAVE_DECL_D_SPLICE_ALIAS && HAVE_DECL_D_SPLICE_ALIAS
  if ( NULL != i ) {
    if ( UFSD_SB( i->i_sb )->options.nocase ) {
      struct dentry* a = d_find_alias( i );
      if ( NULL != a ) {
        if ( IS_ROOT( a ) && (a->d_flags & DCACHE_DISCONNECTED) )
          dput( a );
        else {
          BUG_ON(d_unhashed( a ));
          if ( !S_ISDIR( i->i_mode ))
            d_move( a, de );
          iput( i );
          return a;
        }
      }
    }
    return d_splice_alias( i, de );
  }
#endif

  // ENOENT is expected and will be handled by the caller.
  // (a least on some old kernels).
  if ( err && -ENOENT != err ) {
    assert(NULL == i);
    return ERR_PTR(err);
  }

  d_add( de, i );
  return NULL;
}


///////////////////////////////////////////////////////////
// ufsd_link
//
// This function creates a hard link
// inode_operations::link
///////////////////////////////////////////////////////////
static int
ufsd_link(
    IN struct dentry* ode,
    IN struct inode*  dir,
    IN struct dentry* de
    )
{
  int err;
  struct inode* i;
  struct inode* oi = ode->d_inode;

  UfsdCreate  cr;

  cr.lnk  = UFSD_FH(oi);
  cr.data = NULL;
  cr.len  = 0;
  cr.mode = 0;

  assert( NULL != oi && NULL != UFSD_FH(oi) );
  assert( NULL != dir && NULL != UFSD_FH(dir) );
  assert( S_ISDIR( dir->i_mode ) );
  assert( dir->i_sb == oi->i_sb );

  DebugTrace(+1, Dbg, ("link: r=%lx \"%.*s\" => r=%lx /\"%.*s\"\n",
                        oi->i_ino, (int)ode->d_name.len, ode->d_name.name,
                        dir->i_ino, (int)de->d_name.len, de->d_name.name ));

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( 0 == err ) {
    //
    // Hard link is created
    //
    assert( i == oi );

    d_instantiate( de, i );
    inc_nlink( i );
  }

  DebugTrace(-1, Dbg, ("link -> %d\n", err ));

  return err;
}

static int
ufsd_symlink(
    IN struct inode*  dir,
    IN struct dentry* de,
    IN const char*    symname
    );

static int
ufsd_mknod(
    IN struct inode*  dir,
    IN struct dentry* de,
    IN Umode_t        mode,
    IN dev_t          rdev
    );


STATIC_CONST struct inode_operations ufsd_dir_inode_operations = {
  .lookup       = ufsd_lookup,
  .create       = ufsd_create,
  .link         = ufsd_link,
  .unlink       = ufsd_unlink,
  .symlink      = ufsd_symlink,
  .mkdir        = ufsd_mkdir,
  .rmdir        = ufsd_unlink,
  .mknod        = ufsd_mknod,
  .rename       = ufsd_rename,
  .setattr      = ufsd_setattr,
#ifdef UFSD_USE_XATTR
  .permission   = ufsd_permission,
  .setxattr     = generic_setxattr,
  .getxattr     = generic_getxattr,
  .listxattr    = ufsd_listxattr,
  .removexattr  = generic_removexattr,
#endif
#if defined UFSD_USE_XATTR && (defined HAVE_STRUCT_INODE_OPERATIONS_GET_ACL && HAVE_STRUCT_INODE_OPERATIONS_GET_ACL)
  .get_acl      = ufsd_get_acl,
#endif
};


STATIC_CONST struct inode_operations ufsd_special_inode_operations = {
#ifdef UFSD_USE_XATTR
  .permission   = ufsd_permission,
  .setxattr     = generic_setxattr,
  .getxattr     = generic_getxattr,
  .listxattr    = ufsd_listxattr,
  .removexattr  = generic_removexattr,
#endif
  .setattr      = ufsd_setattr,
#if defined UFSD_USE_XATTR && (defined HAVE_STRUCT_INODE_OPERATIONS_GET_ACL && HAVE_STRUCT_INODE_OPERATIONS_GET_ACL)
  .get_acl      = ufsd_get_acl,
#endif

};


///////////////////////////////////////////////////////////
// ufsd_mknod
//
//
///////////////////////////////////////////////////////////
static int
ufsd_mknod(
    IN struct inode*  dir,
    IN struct dentry* de,
    IN Umode_t        mode,
    IN dev_t          rdev
    )
{
  struct inode* i = NULL;
  int     err;
  unsigned int udev32;

  UfsdCreate  cr;

  cr.lnk  = NULL;
  cr.data = &udev32;
  cr.len  = sizeof(udev32);
  cr.mode = mode;

  DebugTrace(+1, Dbg, ("mknod m=%o\n", mode));

  udev32 = new_encode_dev( rdev );

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( 0 == err ) {
    init_special_inode( i, i->i_mode, rdev );
    i->i_op = &ufsd_special_inode_operations;
    mark_inode_dirty( i );
    d_instantiate( de, i );
  }

  DebugTrace(-1, Dbg, ("mknod -> %d\n", err));

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_file_read
//
// file_operations::read
///////////////////////////////////////////////////////////

/*
static ssize_t new_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
 {
         struct iovec iov = { .iov_base = buf, .iov_len = len };
         struct kiocb kiocb;
         struct iov_iter iter;
         ssize_t ret;
 
         init_sync_kiocb(&kiocb, filp);
         kiocb.ki_pos = *ppos;
         iov_iter_init(&iter, READ, &iov, 1, len);
 
         ret = filp->f_op->read_iter(&kiocb, &iter);
         BUG_ON(ret == -EIOCBQUEUED);
         *ppos = kiocb.ki_pos;
         return ret;
 }


ssize_t
ufsd_file_read(
    IN struct file* file,
    IN char __user* buf,
    IN size_t       count,
    OUT loff_t*     ppos
    )
{
  ssize_t ret;
  struct inode* i = file->f_path.dentry->d_inode;
  unode* u        = UFSD_U( i );
  const unsigned char* p  = IsStream( file );

  if ( NULL != p ) {

    // Read stream
    usuper* sbi     = UFSD_SB( i->i_sb );
    struct qstr* s  = &file->f_path.dentry->d_name;
    int len         = s->name + s->len - p;
    loff_t pos      = *ppos;

    DebugTrace(+1, Dbg, ("file_read(s): r=%lx (:%.*s), %llx, %Zx\n",
                          i->i_ino, len, p, (UINT64)pos, count ));

    //
    // Allocate buffer if not
    //
    if ( NULL == sbi->rw_buffer ) {
      sbi->rw_buffer = vmalloc( RW_BUFFER_SIZE );
      if ( NULL == sbi->rw_buffer ){
        ret = -ENOMEM;
        goto out;
      }
    }

    //
    // Read via sbi->rw_buffer
    //
    for ( ret = 0; ret < count; ) {
      size_t  read;
      unsigned long rem = ((unsigned long)pos) & ( RW_BUFFER_SIZE - 1 );
      size_t to_read = min_t(unsigned int, RW_BUFFER_SIZE - rem,  count - ret);
      C_ASSERT( sizeof(pos) == 8 );
      int err;

      LockUfsd( sbi );

      err = UFSDAPI_FileRead( sbi->Ufsd, u->ufile, p, len,
                              pos, to_read, sbi->rw_buffer, &read );

      if ( ERR_NOFILEEXISTS == err ) {
        err = -ENOENT;
      } else if ( 0 != err ) {
        err = -EIO;
      } else if ( 0 != copy_to_user( buf, sbi->rw_buffer, read ) ) {
        err = -EFAULT;
      }

      UnlockUfsd( sbi );

      if ( 0 != err ) {
        ret = err;
        goto out;
      }

      pos += read;
      buf += read;
      ret += read;

      if ( read != to_read )
        break;
    }

    *ppos = pos;

  } else {

    unsigned int  flags = file->f_flags;

    DebugTrace(+1, Dbg, ("file_read: r=%lx, %llx, %Zx, sz=%llx,%llx\n", i->i_ino, (UINT64)*ppos, count, u->mmu, i->i_size ));

Again:
#if defined HAVE_DECL_DO_SYNC_READ && HAVE_DECL_DO_SYNC_READ
    ret = do_sync_read( file, buf, count, ppos );
#elif defined HAVE_DECL_GENERIC_FILE_READ && HAVE_DECL_GENERIC_FILE_READ
    ret = generic_file_read( file, buf, count, ppos );
#else
    //#error "Unknown file_read version"
    ret = new_sync_write( file, buf, count, ppos );
#endif

    if ( -ENOTBLK == ret && FlagOn( file->f_flags, O_DIRECT ) ) {
      DebugTrace(0, Dbg, ("file_read: turn off O_DIRECT\n" ));
      file->f_flags &= ~O_DIRECT;
      goto Again;
    }

    file->f_flags = flags;
  }
out:
  if ( ret >= 0 ){
//    DropPages( i->i_mapping );
    DebugTrace(-1, Dbg, ("file_read -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_read -> error %d\n", (int)ret));
  }
  return ret;
}

*/

///////////////////////////////////////////////////////////
// ufsd_file_extend
//
// Extend non sparse file. Helper function
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_extend(
    IN struct inode*  i,
    IN UINT64         pos,
    IN size_t         len,
    IN int            append
    )
{
  ssize_t ret;
  usuper* sbi;
  sector_t blocks, iblock;
  unode* u;
  unsigned blkbits, blksize;
  UINT64 new_size, isize  = i_size_read( i );
  if ( append )
    pos = isize;

  new_size  = pos + len;

  if ( new_size <= isize )
    return 0;

  blkbits  = i->i_blkbits;
  blksize  = 1 << blkbits;

  if ( ((new_size + blksize - 1) >> blkbits) == ((isize + blksize - 1) >> blkbits) )
    return 0;

  if ( new_size > i->i_sb->s_maxbytes ) {
    DebugTrace(0, Dbg, ("extend: new size is too big. s_maxbytes=%llx\n", i->i_sb->s_maxbytes ) );
    return -EFBIG;
  }

  //
  // Check cached info (file may be preallocated)
  //
  iblock  = pos >> blkbits;
  u       = UFSD_U(i);

  spin_lock( &u->block_lock );
  blocks = NULL != u->ufile && -1 != u->Lbn && u->Vbn <= iblock && iblock < u->Vbn + u->Len
        ? u->Len - (iblock - u->Vbn)
        : 0;
  spin_unlock( &u->block_lock );

  if ( 0 != blocks && iblock + blocks >= ((new_size + (1u<<blkbits) - 1) >> blkbits) ) {
    DebugTrace(0, Dbg, ("preallocated in cache: [%"PSCT"x, %"PSCT"x) => %"PSCT"x\n", u->Vbn, u->Vbn + u->Len, u->Lbn));
    return 0;
  }

  sbi = UFSD_SB( i->i_sb );

#ifdef UFSD_DELAY_ALLOC
  if ( sbi->options.delalloc )
    return 0;
#endif

  LockUfsd( sbi );

  ret = LazyOpen( sbi, i );
  if ( 0 == ret ) {
    unode* u  = UFSD_U(i);
    mapinfo Map;

    switch( UFSDAPI_FileAllocate( u->ufile, pos, len, 0, &Map ) ) {
    case 0:
      ret = 0;
      if ( Map.Len > 0 ) {
        // Update cache
        spin_lock( &u->block_lock );
        u->Lbn  = Map.Lbo >> blkbits;
        u->Vbn  = Map.Vbo >> blkbits;
        u->Len  = Map.Len >> blkbits;
        spin_unlock( &u->block_lock );
        DebugTrace(0, Dbg, ("cache: [%"PSCT"x, %"PSCT"x) => %"PSCT"x\n", u->Vbn, u->Vbn + u->Len, u->Lbn));
      }
      i->i_blocks = Map.Alloc >> 9;

      if ( !append ) {
        i_size_write( i, new_size );
        TIMESPEC_SECONDS( &i->i_mtime ) = TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds();
        mark_inode_dirty( i );
      }
      break;

    case ERR_NOSPC          : ret = -ENOSPC; break;
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    default                 : ret = -EINVAL;
    }
  }

  UnlockUfsd( sbi );
  return ret;
}


#ifdef UFSD_CHECK_BDI
///////////////////////////////////////////////////////////
// IsBdiOk
//
// Returns 0 if bdi is removed
///////////////////////////////////////////////////////////
static int
IsBdiOk(
    IN struct super_block* sb
    )
{
#if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
  if ( UFSD_SB( sb )->bdi != sb->s_bdi )
#else
  if ( NULL == UFSD_SB( sb )->bdi->dev )
#endif
  {
    printk( KERN_CRIT QUOTED_UFSD_DEVICE": media removed\n" );
    return 0;
  }
  return 1;
}
#endif


///////////////////////////////////////////////////////////
// ufsd_file_write
//
// file_operations::write
///////////////////////////////////////////////////////////
/*
static ssize_t
ufsd_file_write(
    IN struct file* file,
    IN const char*  buf,
    IN size_t       count,
    IN OUT loff_t*  ppos
    )
{
  struct inode* i = file_inode( file );
  unode* u        = UFSD_U( i );
  UINT64 pos      = *ppos;
  usuper* sbi     = UFSD_SB( i->i_sb );
  ssize_t ret;
  const unsigned char* p  = IsStream( file );

  if ( u->encrypt ) {
    DebugTrace(0, UFSD_LEVEL_ERROR, ("file_write: r=%lx. Attempt to write to encrypted file\n", i->i_ino ));
    return -ENOSYS;
  }

#ifdef UFSD_CHECK_BDI
  if ( !IsBdiOk( i->i_sb ) )
    return -ENODEV;
#endif

  if ( NULL != p || (!sbi->options.delalloc && sbi->options.ntfs && 0 == i->i_size && count < 512 && ( FlagOn(file->f_flags,O_APPEND) || 0 == pos ) ) ) {

    // Write stream
    struct qstr* s  = &file->f_path.dentry->d_name;
    int len         = NULL == p? 0 : (s->name + s->len - p);

    DebugTrace(+1, Dbg, ("file_write(s): r=%lx (:%.*s), %llx, %Zx\n", i->i_ino, len, p, pos, count ));

    //
    // Allocate buffer if not
    //
    if ( NULL == sbi->rw_buffer ) {
      sbi->rw_buffer = vmalloc( RW_BUFFER_SIZE );
      if ( NULL == sbi->rw_buffer ){
        ret = -ENOMEM;
        goto out;
      }
    }

    if ( NULL == p && FlagOn(file->f_flags,O_APPEND) )
      pos = i->i_size;

    // Write as much as possible via sbi->rw_buffer
    for ( ret = 0; ret < count; ) {
      size_t written;
      unsigned long rem = ((unsigned long)pos) & ( RW_BUFFER_SIZE - 1 );
      size_t to_write = min_t(unsigned int,RW_BUFFER_SIZE - rem,count - ret);
      int err;

      LockUfsd( sbi );

      if ( copy_from_user( sbi->rw_buffer, buf, to_write ) ) {
        err = -EFAULT;
      } else {
        err = UFSDAPI_FileWrite( sbi->Ufsd, u->ufile, p, len,
                                 pos, to_write, sbi->rw_buffer, &written );
        if ( 0 != err )
          err = ERR_NOTIMPLEMENTED == err? -EOPNOTSUPP : ERR_NOSPC == err? -ENOSPC : -EINVAL;
      }

      UnlockUfsd( sbi );

      if ( 0 != err ) {
        if ( -EOPNOTSUPP == err && NULL == p ) {
#ifdef UFSD_TRACE
          if ( UFSD_TraceLevel & Dbg )
            UFSD_TraceInc( -1 );
#endif
          goto sync_write; // Operation requires resident->nonresident
        }
        ret = err;
        goto out;
      }

      pos += written;
      buf += written;
      ret += written;

      if ( written != to_write ) {
        ret = 0 == written? -EIO : written;
        break;
      }
    }

    if ( NULL == p && 0 != ret ) {
      i->i_size = u->mmu = ret;
      mark_inode_dirty( i );
    }

    *ppos = pos;
  } else {

sync_write:
    DebugTrace(+1, Dbg, ("file_write: r=%lx, %llx, %Zx, s=%llx,%llx\n", i->i_ino, pos, count, (UINT64)u->mmu, (UINT64)i_size_read( i ) ));

#if defined HAVE_DECL_DO_SYNC_WRITE && HAVE_DECL_DO_SYNC_WRITE
    //
    // 'do_sync_write' calls 'ufsd_file_aio_write'
    //
    ret = do_sync_write( file, buf, count, ppos );
#elif defined HAVE_DECL_GENERIC_FILE_WRITE && HAVE_DECL_GENERIC_FILE_WRITE

#if !(defined HAVE_STRUCT_FILE_OPERATIONS_AIO_WRITE && HAVE_STRUCT_FILE_OPERATIONS_AIO_WRITE)
    if ( !u->sparse && !u->compr ) {
      ret = ufsd_file_extend( i, pos, count, FlagOn( file->f_flags & O_APPEND ) );
      if ( 0 != ret )
        goto out;
    }
#endif

    ret = generic_file_write( file, buf, count, ppos );
#else
  #error "Unknown file_write version"
#endif
  }

out:

#ifdef Writeback_inodes_sb_if_idle
  if ( sbi->options.wb && count >= PAGE_SIZE  )
    Writeback_inodes_sb_if_idle( i->i_sb );
#endif

  if ( ret >= 0 ){
//    DropPages( i->i_mapping );
    DebugTrace(-1, Dbg, ("file_write -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_write -> error %d\n", (int)ret));
  }
  return ret;
}

*/

#if defined HAVE_DECL_FO_AIO_WRITE_V1 && HAVE_DECL_FO_AIO_WRITE_V1

///////////////////////////////////////////////////////////
// ufsd_file_aio_read
//
// file_operations::aio_read
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_read(
    IN struct kiocb*  iocb,
    IN char __user*   buf,
    IN size_t         count,
    IN loff_t         pos
    )
{
  ssize_t ret;
  //struct iov_iter iter;
  //struct inode* i = iocb->ki_filp->f_path.dentry->d_inode;
  struct inode* i = iocb->ki_filp->f_mapping->host;
  unode* u        = UFSD_U( i );

  DebugTrace(+1, Dbg, ("file_aio_read: r=%lx, %llx, %Zx\n", i->i_ino, (UINT64)pos, count ));

  if ( !u->sparse && !u->compr ){
    loff_t isize = i->i_size;
    if ( pos >= isize ) {
      DebugTrace(0, Dbg, ("file_aio_read: zero full buffer\n"));
//      ret = __clear_user( buf, count );
      ret = 0;
      goto out;
    }
    if ( pos >= u->mmu ) {
      loff_t tail = isize - pos;
      if ( count > tail )
        count = tail;
      DebugTrace(0, Dbg, ("file_aio_read: zero tail [%llx %llx)\n", (UINT64)pos, (UINT64)pos + count ));
      ret = count - __clear_user( buf, count );
      iocb->ki_pos = pos + ret;
      goto out;
    }
  }
//fix here
  ret = generic_file_aio_read( iocb, buf, count, pos );

out:
  if ( ret >= 0 ){
    DebugTrace(-1, Dbg, ("file_aio_read -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_aio_read -> error %d\n", (int)ret));
  }
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_file_aio_write
//
// file_operations::aio_write
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_write(
    IN struct kiocb*      iocb,
    IN const char __user* buf,
    IN size_t             len,
    IN loff_t             pos
    )
{
  ssize_t ret;
  // struct inode* i = iocb->ki_filp->f_path.dentry->d_inode;
  struct inode* i = iocb->ki_filp->f_mapping->host;
  unode* u        = UFSD_U( i );
  UINT64 Holder;

  DebugTrace(+1, Dbg, ("file_aio_write: r=%lx, %llx, count=%x\n", i->i_ino, (UINT64)pos, (unsigned)len ));

  //
  // Preallocate space for normal files
  //
  if ( !u->sparse && !u->compr ) {
    ret = ufsd_file_extend( i, pos, len, FlagOn( iocb->ki_filp->f_flags, O_APPEND ) );
    if ( 0 != ret )
      goto out;

    if ( unlikely( FlagOn( iocb->ki_filp->f_flags, O_DIRECT ) ) ) {
      UINT64 new_mmu = (iocb->ki_filp->f_flags & O_APPEND? i_size_read( i ) : pos) + len;
      if ( u->mmu < new_mmu )
        u->mmu = new_mmu;
    } else if ( 0 == u->flags && pos > u->mmu && len <= sizeof(Holder) && (Holder = 0, 0 == copy_from_user( &Holder, buf, len ) ) ) {
      // Optimize writing zero into non initialized area
      if ( 0 == Holder ) {
        DebugTrace(0, Dbg, ("file_aio_write (skip zeroing)\n"));
        ret = len;
        goto out;
      }
      DebugTrace(0, Dbg, ("file_aio_write (@%llx=%llx)\n", (UINT64)pos, Holder ));
    }
  }
//fix here
  ret = generic_file_aio_write( iocb, buf, len, pos );

out:
  if ( ret >= 0 ){
    DebugTrace(-1, Dbg, ("file_aio_write -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_aio_write -> error %d\n", (int)ret));
  }
  return ret;
}

#elif defined HAVE_DECL_FO_AIO_WRITE_V2 && HAVE_DECL_FO_AIO_WRITE_V2

///////////////////////////////////////////////////////////
// ufsd_file_aio_read
//
// file_operations::aio_read
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_read(
    IN struct kiocb*        iocb,
    IN const struct iovec*  iov,
    IN unsigned long        nr_segs,
    IN loff_t               pos
    )
{
  ssize_t ret;
  unsigned long seg;
  loff_t len      = iov_length( iov, nr_segs );
  //struct inode* i = iocb->ki_filp->f_path.dentry->d_inode;
  struct inode* i = iocb->ki_filp->f_mapping->host;
  unode* u        = UFSD_U( i );

  DebugTrace(+1, Dbg, ("file_aio_read: r=%lx, %llx, %llx\n", i->i_ino, (UINT64)pos, (UINT64)len ));

  if ( !u->sparse && !u->compr ){
    loff_t isize = i->i_size;
    if ( pos >= isize ) {
      DebugTrace(0, Dbg, ("file_aio_read: zero full buffer\n"));

//      for ( seg = 0; seg < nr_segs; seg++ )
//        ret = __clear_user( iov[seg].iov_base, iov[seg].iov_len );
      ret = 0;
      goto out;
    }
    if ( pos >= u->mmu ) {
      loff_t tail = isize - pos;
      if ( len > tail )
        len = tail;
      DebugTrace(0, Dbg, ("file_aio_read: zero tail [%llx %llx)\n", (UINT64)pos, (UINT64)pos + len ));

      for ( seg = 0; seg < nr_segs; seg++ )
        ret = __clear_user( iov[seg].iov_base, iov[seg].iov_len );

      iocb->ki_pos = pos + len;
      ret = len;
      goto out;
    }
  }

  ret = generic_file_aio_read( iocb, iov, nr_segs, pos );

out:
  if ( ret >= 0 ){
    DebugTrace(-1, Dbg, ("file_aio_read -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_aio_read -> error %d\n", (int)ret));
  }
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_file_aio_write
//
// file_operations::aio_write
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_write(
    IN struct kiocb*        iocb,
    IN const struct iovec*  iov,
    IN unsigned long        nr_segs,
    IN loff_t               pos
    )
{
  ssize_t ret;
  size_t len      = iov_length( iov, nr_segs );
  // struct inode* i = iocb->ki_filp->f_path.dentry->d_inode;
  struct inode* i = iocb->ki_filp->f_mapping->host;
  unode* u        = UFSD_U( i );
  UINT64 Holder;

  DebugTrace(+1, Dbg, ("file_aio_write: r=%lx, %llx, %Zx\n", i->i_ino, (UINT64)pos, len ));


  //
  // Preallocate space for normal files
  //
  if ( !u->sparse && !u->compr ) {
    ret = ufsd_file_extend( i, pos, len, iocb->ki_filp->f_flags & O_APPEND );
    if ( 0 != ret )
      goto out;

    if ( unlikely( FlagOn( iocb->ki_filp->f_flags, O_DIRECT ) ) ) {
      UINT64 new_mmu = (iocb->ki_filp->f_flags & O_APPEND? i_size_read( i ) : pos) + len;
      if ( u->mmu < new_mmu )
        u->mmu = new_mmu;
    } else if ( 0 == u->flags && pos > u->mmu && 1 == nr_segs && len <= sizeof(Holder) && (Holder = 0, 0 == copy_from_user( &Holder, iov->iov_base, len ) ) ) {
      // Optimize writing zero into non initialized area
      if ( 0 == Holder ) {
        ret = len;
        goto out;
      }
      DebugTrace(0, Dbg, ("file_aio_write (@%llx=%llx)\n", (UINT64)pos, Holder ));
    }
  }

  ret = generic_file_aio_write( iocb, iov, nr_segs, pos );

out:
  if ( ret >= 0 ){
    DebugTrace(-1, Dbg, ("file_aio_write -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_aio_write -> error %d\n", (int)ret));
  }
  return ret;
}

#endif // #if defined HAVE_DECL_FO_AIO_WRITE_V2 && HAVE_DECL_FO_AIO_WRITE_V2


#ifdef UFSD_NTFS
///////////////////////////////////////////////////////////
// ufsd_fix_page_buffers
//
//
///////////////////////////////////////////////////////////
static int
ufsd_fix_page_buffers(
    IN struct page* page,
    IN int          dirty
    )
{
  int ret = 0;
  if ( page_has_buffers( page ) ) {
    struct buffer_head* head = page_buffers( page );
    struct buffer_head* bh   = head;
    do {
      struct buffer_head* next = bh->b_this_page;
      if ( buffer_mapped( bh ) && -1 == bh->b_blocknr ) {
#ifdef UFSD_DEBUG
//        unsigned long b_state = bh->b_state;
#endif
        ret = 1;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
        mark_buffer_clean(bh);
        lock_buffer(bh);
        clear_buffer_mapped(bh);
        clear_buffer_req(bh);
        clear_buffer_new(bh);
        unlock_buffer(bh);
#else
        clear_buffer_mapped(bh);
        if ( dirty )
          set_buffer_dirty(bh);
        else
          clear_buffer_dirty(bh);
        set_buffer_delay(bh);

//        DebugTrace(0,Dbg,("fix buffer bh=%p,%lx=>%lx\n", bh, b_state, bh->b_state));
#endif
      }
      bh = next ;
    } while (bh != head);

    if ( 0 != ret )
      DebugTrace(0,Dbg,("fix page %p,%lx (%lx)\n", page, page->flags, page->index ));
  }

  return ret;
}
#endif // #ifdef UFSD_NTFS


///////////////////////////////////////////////////////////
// ufsd_file_mmap
//
// file_operations::mmap
///////////////////////////////////////////////////////////
static int
ufsd_file_mmap(
    IN struct file*           file,
    IN struct vm_area_struct* vma
    )
{
  int err;
  struct inode* i   = file_inode( file );
  unode* u          = UFSD_U( i );
  UINT64 from       = ((UINT64)vma->vm_pgoff << PAGE_SHIFT);
  unsigned long len = vma->vm_end - vma->vm_start;
  UINT64 isize   = i_size_read( i );
  UINT64 vsize   = from + len;

  assert( from < isize );
  if ( vsize > isize ) {
    len   = isize - from;
    vsize = isize;
  }

  DebugTrace(+1, Dbg, ("file_mmap: r=%lx %lx(%s%s), %llx, %lx s=%llx,%llx\n",
              i->i_ino, vma->vm_flags,
              (vma->vm_flags & VM_READ)?"r":"",
              (vma->vm_flags & VM_WRITE)?"w":"",
              from, len, u->mmu, isize ));
  if ( IsStream( file ) ) {
    err = -ENOSYS; // no mmap for streams
    goto out;
  }

  if ( (u->compr || u->encrypt) && (vma->vm_flags & VM_WRITE) ) {
    err = -ENOTBLK;
    goto out;
  }

  if ( !u->sparse && (vma->vm_flags & VM_WRITE) && u->mmu < vsize ) {

    loff_t pos              = vsize - 1;
    unsigned int flags      = file->f_flags;
    mm_segment_t old_limit  = get_fs();
    char zero = 0;

    DebugTrace(0, Dbg, ("file_mmap: zero range [%llx,%llx)\n", u->mmu, vsize ));

    u->flags       = 1;
    file->f_flags &= ~O_DIRECT;
    set_fs(KERNEL_DS);

    //err = ufsd_file_write( file, &zero, 1, &pos );
    err = do_sync_write( file, &zero, 1, &pos );

    set_fs(old_limit);
    file->f_flags = flags;
    u->flags      = 0;

    if ( 1 != err )
      goto out;
  }

  err = generic_file_mmap( file, vma );

out:
  DebugTrace(-1, Dbg, ("file_mmap -> %d\n", err) );

  return err;
}


#if defined HAVE_DECL_GENERIC_FILE_SENDFILE && HAVE_DECL_GENERIC_FILE_SENDFILE
///////////////////////////////////////////////////////////
// ufsd_file_sendfile
//
// file_operations::sendfile
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_sendfile(
    IN struct file* file,
    IN OUT loff_t*  ppos,
    IN size_t       count,
    IN read_actor_t actor,
    IN void*        target
    )
{
  ssize_t ret;
  DebugTrace(+1, Dbg, ("file_sendfile: r=%lx, %llx %Zx\n", file_inode( file )->i_ino, (UINT64)*ppos, count ));

  ret = IsStream( file )
    ? -ENOSYS
    : generic_file_sendfile( file, ppos, count, actor, target );

  DebugTrace(-1, Dbg, ("file_sendfile -> %Zx\n", ret ));
  return ret;
}
#endif


#if defined HAVE_DECL_GENERIC_FILE_SPLICE_READ && HAVE_DECL_GENERIC_FILE_SPLICE_READ
///////////////////////////////////////////////////////////
// ufsd_file_splice_read
//
// file_operations::splice_read
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_splice_read(
    IN struct file* file,
    IN OUT loff_t*  ppos,
    IN struct pipe_inode_info*  pipe,
    IN size_t       len,
    IN unsigned int flags
    )
{
  ssize_t ret;
  DebugTrace(+1, Dbg, ("file_splice_read: r=%lx, %llx %Zx\n", file_inode( file )->i_ino, (UINT64)*ppos, len ));

  if ( IsStream( file ) ) {
    DebugTrace(-1, Dbg, ("file_splice_read failed to read stream -> -ENOSYS\n"));
    return -ENOSYS;
  }

  ret = generic_file_splice_read( file, ppos, pipe, len, flags );

  if ( ret >= 0 ){
    DebugTrace(-1, Dbg, ("file_splice_read -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_splice_read -> error %d\n", (int)ret));
  }
  return ret;
}
#endif

#if defined HAVE_DECL_GENERIC_FILE_SPLICE_WRITE && HAVE_DECL_GENERIC_FILE_SPLICE_WRITE
///////////////////////////////////////////////////////////
// ufsd_file_splice_write
//
// file_operations::splice_write
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_splice_write(
    IN struct pipe_inode_info*  pipe,
    IN struct file*   file,
    IN OUT loff_t*    ppos,
    IN size_t         len,
    IN unsigned int   flags
    )
{
  ssize_t ret;
  struct inode* i = file_inode( file );
  unode* u        = UFSD_U( i );

#ifdef UFSD_CHECK_BDI
  if ( !IsBdiOk( i->i_sb ) )
    return -ENODEV;
#endif

  DebugTrace(+1, Dbg, ("file_splice_write: r=%lx, %llx %Zx\n", i->i_ino, (UINT64)*ppos, len ));

  if ( IsStream( file ) ) {
    DebugTrace(-1, Dbg, ("file_splice_write failed to write stream -> -ENOSYS\n"));
    return -ENOSYS;
  }

  if ( !u->sparse && !u->compr ) {
    ret = ufsd_file_extend( i, *ppos, len, file->f_flags & O_APPEND );
    if ( 0 != ret )
      goto out;
  }
//fix here
  ret = generic_file_splice_write( pipe, file, ppos, len, flags );

out:
  if ( ret >= 0 ){
    DebugTrace(-1, Dbg, ("file_splice_write -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace(-1, Dbg, ("file_splice_write -> error %d\n", (int)ret));
  }
  return ret;
}
#endif


#if defined HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE && HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE
#include <linux/falloc.h>
///////////////////////////////////////////////////////////
// ufsd_fallocate
//
// inode_operations::fallocate
///////////////////////////////////////////////////////////
static long
ufsd_fallocate(
    IN struct inode*  i,
    IN int            mode,
    IN loff_t         offset,
    IN loff_t         len
    )
{
  long ret;
  usuper* sbi = UFSD_SB( i->i_sb );
  unode* u    = UFSD_U( i );

  DebugTrace(+1, Dbg, ("fallocate: r=%lx, %llx, %llx, %d\n", i->i_ino, (UINT64)offset, (UINT64)len, mode ));

  if ( !u->sparse ) {
    DebugTrace(-1, Dbg, ("fallocate -> notsupported\n" ));
    return -EOPNOTSUPP;
  }

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  ret = LazyOpen( sbi, i );

  if ( 0 == ret ) {
    int KeepSize = FlagOn( mode, FALLOC_FL_KEEP_SIZE );
    mapinfo Map;
    ret = UFSDAPI_FileAllocate( u->ufile, offset, len, KeepSize, &Map );
    switch( ret ) {
    case 0:
      i->i_blocks = Map.Alloc >> 9;
      offset += len;
      if ( !KeepSize && offset > i_size_read(i) )
        i_size_write( i, offset );
      break;
    case ERR_NOSPC          : ret = -ENOSPC; break;
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    default                 : ret = -EINVAL;
    }
  }

  UnlockUfsd( sbi );

  if ( 0 == ret ){
    DebugTrace(-1, Dbg, ("fallocate -> ok\n"));
  } else {
    DebugTrace(-1, Dbg, ("fallocate -> error %d\n", (int)ret));
  }
  return ret;
}
#endif // #if defined HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE && HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE


STATIC_CONST struct file_operations ufsd_file_operations = {
  .llseek   = generic_file_llseek,
  //.read     = ufsd_file_read,
  //.read     = new_sync_read,

#if defined HAVE_STRUCT_FILE_OPERATIONS_AIO_READ && HAVE_STRUCT_FILE_OPERATIONS_AIO_READ
  .aio_read = ufsd_file_aio_read,
#endif
  .mmap     = ufsd_file_mmap,
  .open     = ufsd_file_open,
  .release  = ufsd_file_release,
  .fsync    = generic_file_fsync,
//  .write    = ufsd_file_write,
  //.write    = new_sync_write,
#if defined HAVE_STRUCT_FILE_OPERATIONS_AIO_WRITE && HAVE_STRUCT_FILE_OPERATIONS_AIO_WRITE
  .aio_write = ufsd_file_aio_write,
#endif
#ifndef UFSD_NO_USE_IOCTL
  .ioctl    = ufsd_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl	= ufsd_compat_ioctl,
#endif
#endif
#if defined HAVE_DECL_GENERIC_FILE_SENDFILE && HAVE_DECL_GENERIC_FILE_SENDFILE
  .sendfile     = ufsd_file_sendfile,
#endif
#if defined HAVE_DECL_GENERIC_FILE_SPLICE_READ && HAVE_DECL_GENERIC_FILE_SPLICE_READ
  .splice_read  = ufsd_file_splice_read,
#endif
#if defined HAVE_DECL_GENERIC_FILE_SPLICE_WRITE && HAVE_DECL_GENERIC_FILE_SPLICE_WRITE
  .splice_write = ufsd_file_splice_write,
#endif
};

STATIC_CONST struct inode_operations ufsd_file_inode_operations = {
#if !(defined HAVE_DECL_TRUNCATE_SETSIZE && HAVE_DECL_TRUNCATE_SETSIZE)
  .truncate     = ufsd_truncate,
#endif
  .setattr      = ufsd_setattr,
#ifdef UFSD_DELAY_ALLOC
  .getattr      = ufsd_getattr,
#endif
#ifdef UFSD_USE_XATTR
  .permission   = ufsd_permission,
  .setxattr     = generic_setxattr,
  .getxattr     = generic_getxattr,
  .listxattr    = ufsd_listxattr,
  .removexattr  = generic_removexattr,
#endif
#if defined HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE && HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE
  .fallocate    = ufsd_fallocate,
#endif
#if defined UFSD_USE_XATTR && (defined HAVE_STRUCT_INODE_OPERATIONS_GET_ACL && HAVE_STRUCT_INODE_OPERATIONS_GET_ACL)
  .get_acl      = ufsd_get_acl,
#endif

};


///////////////////////////////////////////////////////////
// ufsd_readlink_hlp
//
// helper for  ufsd_readlink and ufsd_follow_link
///////////////////////////////////////////////////////////
static int
ufsd_readlink_hlp(
    IN usuper*        sbi,
    IN struct inode*  i,
    IN char*          kaddr,
    IN int            buflen
    )
{
  int len;
  char* p   = kaddr;
  char* l   = kaddr + buflen;
  unode* u  = UFSD_U(i);

  //
  // Call library code to read link
  //
  LockUfsd( sbi );

  len = LazyOpen( sbi, i );
  if ( 0 == len )
    len = UFSDAPI_ReadLink( sbi->Ufsd, u->ufile, kaddr, buflen );

  UnlockUfsd( sbi );

  if ( 0 != len )
    return -EFAULT;

  // safe strlen
  while( 0 != *p && p <= l )
    p += 1;
  len = (int)(p-kaddr);

#if defined HAVE_DECL_FST_GETSB_V2 && HAVE_DECL_FST_GETSB_V2
  //
  // Assume that link points to the same volume
  // and convert strings
  // C:\\Users => /mnt/ntfs/Users
  //
  if ( len > 3
    && 'a' <= (kaddr[0] | 0x20)
    && (kaddr[0] | 0x20) <= 'z'
    && ':' == kaddr[1]
    && '\\' == kaddr[2]
    && NULL != sbi->VfsMnt ) {

    char* MntPath;
#if defined HAVE_DECL_D_PATH_V1 && HAVE_DECL_D_PATH_V1
    MntPath       = d_path( sbi->VfsMnt->mnt_root, sbi->VfsMnt, sbi->MntBuffer, sizeof(sbi->MntBuffer) - 1 );
#elif defined HAVE_DECL_D_PATH_V2 && HAVE_DECL_D_PATH_V2
    struct path path;
    path.dentry   = sbi->VfsMnt->mnt_root;
    path.mnt      = sbi->VfsMnt;
    MntPath       = d_path( &path, sbi->MntBuffer, sizeof(sbi->MntBuffer) - 1 );
#else
#error d_path unknown
#endif

    if ( !IS_ERR(MntPath) ) {
//      DebugTrace(0, Dbg,("mount path %s\n", MntPath ));
      // Add last slash
      int MntPathLen = strlen( MntPath );
      MntPath[MntPathLen++] = '/';

      if ( MntPathLen + len - 3 < buflen ) {
        p = kaddr + MntPathLen;
        memmove( p, kaddr + 3, len - 3 );
        memcpy( kaddr, MntPath, MntPathLen );
        len += MntPathLen - 3;
        // Convert slashes
        l = kaddr + len;
        while( ++p < l ){
          if ( '\\' == *p )
            *p = '/';
        }
        *p = 0;
      }
    }
  }
#endif

  return len;
}


///////////////////////////////////////////////////////////
// ufsd_readlink
//
// inode_operations::readlink
///////////////////////////////////////////////////////////
static int
ufsd_readlink(
    IN struct dentry* de,
    OUT char __user*  buffer,
    IN int            buflen
    )
{
  int err;
  char* kaddr;
  struct inode* i = de->d_inode;
  usuper* sbi     = UFSD_SB( i->i_sb );

  DebugTrace(+1, Dbg, ("readlink: '%.*s'\n", (int)de->d_name.len, de->d_name.name ));

  if ( sbi->options.utf8link ) {
    err = generic_readlink( de, buffer, buflen );
    if ( err > 0 ) {
      DebugTrace(-1, Dbg, ("readlink -> ok '%.*s'\n", buflen, buffer ));
    } else {
      DebugTrace(-1, Dbg, ("readlink failed (%d)\n", err ));
    }
    return err;
  }

  kaddr = UFSD_HeapAlloc( buflen );
  if ( NULL == kaddr ) {
    DebugTrace(-1, Dbg, ("readlink: HeapAlloc failed to allocate %d bytes\n", buflen ));
    return -ENOMEM;
  }

  //
  // Call helper function that reads symlink into buffer
  //
  err = ufsd_readlink_hlp( sbi, i, kaddr, buflen );

  if ( err > 0 ) {
    DebugTrace(-1, Dbg, ("readlink: '%.*s' -> '%.*s'\n",
                (int)de->d_name.len, de->d_name.name, err, kaddr ));
  } else {
    DebugTrace(-1, Dbg, ("readlink: UFSDAPI_ReadLink failed %d\n", err ));
    err = -EFAULT;
  }

  if ( err > 0 && 0 != copy_to_user( buffer, kaddr, err ) )
    err = -EFAULT;
  UFSD_HeapFree( kaddr );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_follow_link
//
// inode_operations::follow_link
///////////////////////////////////////////////////////////
static void*
ufsd_follow_link(
    IN struct dentry*     de,
    IN struct nameidata*  nd
    )
{
  void* ret;
  struct inode* i = de->d_inode;
  usuper* sbi     = UFSD_SB( i->i_sb );

  DebugTrace(+1, Dbg, ("follow_link: '%.*s'\n", (int)de->d_name.len, de->d_name.name ));

  if ( sbi->options.utf8link )
    ret = page_follow_link_light( de, nd );
  else {
    ret = kmalloc(PAGE_SIZE, GFP_NOFS);
    //
    // Call helper function that reads symlink into buffer
    //
    if ( NULL != ret && ufsd_readlink_hlp( sbi, i, ret, PAGE_SIZE ) > 0 ) {
#if defined HAVE_DECL_ND_SET_LINK && HAVE_DECL_ND_SET_LINK
      nd_set_link( nd, (char*)ret );
#else
      nd->saved_names[nd->depth] = (char*)ret;
#endif
    }
  }

  if ( !IS_ERR( ret ) ) {
    DebugTrace(-1, Dbg, ("follow_link -> %p '%.*s'\n", ret, (int)PAGE_SIZE, (char*)ret ));
  } else {
    DebugTrace(-1, Dbg, ("follow_link failed (%p)\n", ret ));
  }

  return ret;
}


#if defined HAVE_DECL_PAGE_PUT_LINK_V1 && HAVE_DECL_PAGE_PUT_LINK_V1

///////////////////////////////////////////////////////////
// ufsd_put_link
//
// inode_operations::put_link
///////////////////////////////////////////////////////////
static void
ufsd_put_link(
    IN struct dentry*     de,
    IN struct nameidata*  nd,
    IN void*              cookie
    )
{
  if ( UFSD_SB( de->d_inode->i_sb )->options.utf8link )
    page_put_link( de, nd, cookie );
  else
    kfree( cookie );
}

STATIC_CONST struct inode_operations ufsd_link_inode_operations = {
  .readlink    = ufsd_readlink,
  .follow_link = ufsd_follow_link,
  .put_link    = ufsd_put_link,
};

#else

///////////////////////////////////////////////////////////
// ufsd_follow_link2 (old version of follow_link)
//
// inode_operations::follow_link
///////////////////////////////////////////////////////////
static int
ufsd_follow_link2(
    IN struct dentry*     de,
    IN struct nameidata*  nd
    )
{
  void* r = ufsd_follow_link( de, nd );
  return NULL == r? -EBADF : 0;
}


///////////////////////////////////////////////////////////
// ufsd_put_link (old version of put_link)
//
// inode_operations::put_link
///////////////////////////////////////////////////////////
static void
ufsd_put_link(
    IN struct dentry*     de,
    IN struct nameidata*  nd
    )
{
  if ( UFSD_SB( de->d_inode->i_sb )->options.utf8link )
    page_put_link( de, nd );
  else
    kfree( nd_get_link(nd) );
}


STATIC_CONST struct inode_operations ufsd_link_inode_operations = {
  .readlink    = ufsd_readlink,
  .follow_link = ufsd_follow_link2,
  .put_link    = ufsd_put_link,
};

#endif


///////////////////////////////////////////////////////////
// ufsd_get_block
//
// This function is a callback for many 'general' functions
///////////////////////////////////////////////////////////
static int
ufsd_get_block(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  return ufsd_get_block_flags( i, iblock, bh, create, 0 );
}


#if defined HAVE_DECL_ASO_WRITEPAGE_V2 && HAVE_DECL_ASO_WRITEPAGE_V2

///////////////////////////////////////////////////////////
// ufsd_get_block_writepage
//
// This function is a callback for ufsd_writepage/ufsd_writepages
///////////////////////////////////////////////////////////
static int
ufsd_get_block_writepage(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  assert( PageUptodate( bh->b_page ) );
  return ufsd_get_block_flags( i, iblock, bh, create, UFSD_FLAG_WRITEPAGE );
}
#endif


#if defined HAVE_DECL_ASO_WRITEPAGE_V2 && HAVE_DECL_ASO_WRITEPAGE_V2

///////////////////////////////////////////////////////////
// ufsd_writepage
//
// address_space_operations::writepage
///////////////////////////////////////////////////////////
static int
ufsd_writepage(
    IN struct page* page,
    IN struct writeback_control*  wbc
    )
{
  DebugTrace(0, Dbg, ("writepage: r=%lx o=%llx\n", page->mapping->host->i_ino, (UINT64)page->index << PAGE_CACHE_SHIFT) );
#ifdef UFSD_NTFS
  ufsd_fix_page_buffers( page, 1 );
#endif
  return block_write_full_page( page, ufsd_get_block_writepage, wbc );
}

#else

///////////////////////////////////////////////////////////
// ufsd_writepage
//
// address_space_operations::writepage
///////////////////////////////////////////////////////////
static int
ufsd_writepage(
    IN struct page* page
    )
{
  int err = 0;
  struct inode* i = page->mapping->host;
  sector_t block;
  struct buffer_head *bh, *head;
  int unlock = 0, uptodate;
  int submitted     = 0;
  UINT64 i_size     = i_size_read(i);
  loff_t end_index  = i_size >> PAGE_CACHE_SHIFT;
  unsigned tail;

  BUG_ON(!PageLocked(page));
  assert( PageUptodate(page) );

  DebugTrace(+1, Dbg, ("writepage: r=%lx p=%p, o=%llx, s=%llx\n", i->i_ino, page, (UINT64)page->index << PAGE_CACHE_SHIFT, i_size) );

  if ( page->index < end_index ){

  } else if ( page->index >= end_index + 1 || 0 == (tail = (i_size & (PAGE_CACHE_SIZE-1))) ){
    UnlockPage( page );
    DebugTrace(-1, Dbg, ("_writepage out of file\n") );
    return 0;
  } else {
    void* kaddr = atomic_kmap( page );
    assert( page->index == end_index && 0 != tail );
    memset(kaddr + tail, 0, PAGE_CACHE_SIZE - tail);
    flush_dcache_page(page);
    atomic_kunmap( kaddr );
    DebugTrace(0, Dbg, ("writepage: last page\n") );
  }

  if ( NULL == page->buffers ) {
    create_empty_buffers(page, i->i_dev, 1 << i->i_blkbits);
    if ( PageUptodate(page) ) {
      head  = page->buffers;
      bh    = head;
      do {
        set_buffer_uptodate(bh);
      } while ((bh = bh->b_this_page) != head);
    }
  }
  head  = page->buffers;
  bh    = head;
  block = page->index << (PAGE_CACHE_SHIFT - i->i_blkbits);

  //
  // Stage 1: make sure we have all the buffers mapped!
  //
  do {
    if ( !(buffer_mapped(bh) && -1 != bh->b_blocknr) && buffer_uptodate(bh) ) {

      err = ufsd_get_block( i, block, bh, 1 );
      clear_buffer_delay( bh );

      if ( 0 != err ) {
        //
        // ENOSPC, or some other error.  We may already have added some
        // blocks to the file, so we need to write these out to avoid
        // exposing stale data.
        //
        ClearPageUptodate(page);
        bh     = head;
        unlock = 1;
        // Recovery: lock and submit the mapped buffers
        do {
          if ( buffer_mapped(bh) && -1 != bh->b_blocknr ) {
            lock_buffer(bh);
            set_buffer_async_io(bh);
            assert( buffer_mapped(bh) );
            unlock = 0;
          } else {
            clear_buffer_dirty( bh );
          }
        } while ((bh = bh->b_this_page) != head);

        assert( 0 == submitted );
        do {
          struct buffer_head *next = bh->b_this_page;
          if ( buffer_async( bh ) ) {
            set_buffer_uptodate(bh);
            clear_buffer_dirty(bh);
            submit_bh(WRITE, bh);
            submitted += 1;
          }
          bh = next;
        } while (bh != head);
        goto done;
      }

      if ( buffer_new(bh) ) {
        struct buffer_head *old_bh =
          get_hash_table(bh->b_dev, bh->b_blocknr, bh->b_size);
        if ( NULL != old_bh) {
          mark_buffer_clean(old_bh);
          wait_on_buffer(old_bh);
          clear_bit(BH_Req, &old_bh->b_state);
          __brelse(old_bh);
        }
        clear_buffer_new(bh);
      }
    }
    block += 1;
  } while ((bh = bh->b_this_page) != head);

  //
  // Stage 2: lock the buffers, mark them clean
  //
  do {
    clear_buffer_delay( bh );
    if ( !(buffer_mapped(bh) && -1 != bh->b_blocknr) )
      continue;

    assert( buffer_mapped(bh) );
    lock_buffer(bh);
    set_buffer_async_io(bh);
    set_buffer_uptodate(bh);
    clear_buffer_dirty(bh);
  } while ((bh = bh->b_this_page) != head);

  //
  // Stage 3: submit the IO
  //
  assert( 0 == submitted );
  do {
    struct buffer_head *next = bh->b_this_page;
    if ( buffer_async( bh ) ) {
      assert( buffer_mapped( bh ) );
      submit_bh(WRITE, bh);
      submitted += 1;
    }
    bh = next;
  } while ( bh != head );

done:

  DebugTrace(0, Dbg, ("writepage: submitted %d\n", submitted ));

  uptodate = 1;
  if ( 0 == submitted ) {
    //
    // The page was marked dirty, but the buffers were
    // clean.  Someone wrote them back by hand with
    // ll_rw_block/submit_bh.  A rare case.
    //
    do {
      if ( !buffer_uptodate(bh) ) {
        uptodate = 0;
        break;
      }
    } while ((bh = bh->b_this_page) != head);

    unlock = 1;
  }

  //
  // Done - end_buffer_io_async will unlock
  //
  if ( uptodate )
    SetPageUptodate( page );

  if ( unlock )
    UnlockPage( page );

#if defined HAVE_DECL_WAKEUP_PAGE_WAITERS && HAVE_DECL_WAKEUP_PAGE_WAITERS
  wakeup_page_waiters( page );
#endif

  DebugTrace(-1, Dbg, ("writepage -> %d\n", err ));
  return err;
}

#endif // HAVE_DECL_ASO_WRITEPAGE_V2


///////////////////////////////////////////////////////////
// ufsd_get_block_prep
//
// This function is a callback for ufsd_prepare_write
///////////////////////////////////////////////////////////
static int
ufsd_get_block_prep(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  return ufsd_get_block_flags( i, iblock, bh, create, UFSD_FLAG_PREPARE );
}


#if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN

///////////////////////////////////////////////////////////
// ufsd_write_begin
//
// address_space_operations::write_begin
///////////////////////////////////////////////////////////
static int
ufsd_write_begin(
    IN struct file*   file,
    IN struct address_space *mapping,
    IN loff_t         pos,
    IN unsigned       len,
    IN unsigned       flags,
    OUT struct page** pagep,
    OUT void**        fsdata
    )
{
  int err;
  struct inode* i = mapping->host;
  unode* u        = UFSD_U( i );

  DebugTrace(+1, UFSD_LEVEL_VFS_WBWE, ("write_begin: r=%lx pos=%llx,%x fl=%x s=%llx,%llx%s\n",
                        i->i_ino, pos, len, flags, u->mmu, i_size_read( i ), u->sparse?",sp":"" ));

  ProfileEnter( UFSD_SB(i->i_sb), write_begin );

  *pagep = NULL;
  *fsdata = NULL;
  assert( NULL == file || !IsStream( file ) );

  if ( u->encrypt ) {
    err = -ENOSYS;
  } else if ( u->sparse || u->compr ) {
#if defined HAVE_DECL_BLOCK_WRITE_BEGIN_V1 && HAVE_DECL_BLOCK_WRITE_BEGIN_V1
    err = block_write_begin( file, mapping, pos, len, flags, pagep, fsdata, ufsd_get_block_prep );
#else
    err = block_write_begin( mapping, pos, len, flags, pagep, ufsd_get_block_prep );
#endif
  } else {
    err = cont_write_begin( file, mapping, pos, len, flags, pagep, fsdata, ufsd_get_block_prep, &u->mmu );
  }

  ProfileLeave( UFSD_SB(i->i_sb), write_begin );

  DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, ("write_begin: -> %d\n", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_write_end
//
// address_space_operations::write_end
///////////////////////////////////////////////////////////
static int
ufsd_write_end(
    IN struct file* file,
    IN struct address_space* mapping,
    IN loff_t       pos,
    IN unsigned     len,
    IN unsigned     copied,
    IN struct page* page,
    IN void*        fsdata
    )
{
  int err;
  struct inode* i = page->mapping->host;
  unode* u        = UFSD_U( i );

  assert( copied <= len );
  assert( page->index == (pos >> PAGE_CACHE_SHIFT) );

  DebugTrace(+1, UFSD_LEVEL_VFS_WBWE, ("write_end: r=%lx pos=%llx,%x,%x s=%llx,%llx\n",
                        i->i_ino, pos, len, copied, u->mmu, i->i_size ));

  ProfileEnter( UFSD_SB(i->i_sb), write_end );

  assert( NULL == file || !IsStream( file ) );

#ifdef UFSD_NTFS
  if ( ufsd_fix_page_buffers( page, 0 ) ) {

    UINT64 isize  = i->i_size;
    usuper* sbi   = UFSD_SB( i->i_sb );

    //
    // File does not have allocation of this Vsn = Offset >> sbi->SctBits
    // Read/Write file synchronously
    // ( if UFSD deals with NTFS then we have compressed/sparsed/resident file )
    //
    unsigned from = (unsigned)(pos & (PAGE_CACHE_SIZE - 1));
    unsigned to   = from + copied;
    char* kaddr   = kmap(page);
    size_t tmp;

    assert( !u->sparse );

    //
    // Call UFSD library
    //
    LockUfsd( sbi );

    //
    // Write file via UFSD -> UFSD_BdWrite
    // Check to make sure that we are not extending the file
    //
//    DebugTrace(0, Dbg, ("write_end: use ufsd to write file: %llx, %x\n", pos, copied ));

    err = UFSDAPI_FileWrite( sbi->Ufsd, u->ufile, NULL, 0, pos, copied, kaddr + from, &tmp );
    assert( tmp == copied );

    //
    // Check results
    //
    if ( 0 != err ) {
      SetPageError( page );
      err = ERR_NOTIMPLEMENTED == err? -EOPNOTSUPP : ERR_NOSPC == err? -ENOSPC : -EINVAL;
    } else {
      if ( try_to_free_buffers( page ) && 0 == from ) {
        if ( pos + copied > isize ){
          memset( kaddr + to, 0, PAGE_CACHE_SIZE - to );
          flush_dcache_page( page );
          SetPageUptodate( page );
        } else if ( copied >= PAGE_CACHE_SIZE )
          SetPageUptodate( page );
      }
      err = tmp;
    }

    UnlockUfsd( sbi );
    kunmap( page );

    TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds();
    mark_inode_dirty( i );

  } else
#endif // #ifdef UFSD_NTFS
  {
    // Use generic function
    err = block_write_end( file, mapping, pos, len, copied, page, fsdata );
  }

  assert( err >= 0 );

  if ( err >= 0 ) {
    pos += err;

    if ( pos > i->i_size ) {
      i_size_write( i, pos );
      mark_inode_dirty( i );
    }

    if( pos > u->mmu ) {
      u->mmu = pos;
      mark_inode_dirty( i );
    }
  }

  unlock_page(page);
  page_cache_release(page);

  ProfileLeave( UFSD_SB(i->i_sb), write_end );

  DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, (err > 0? "write_end: -> %x s=%llx,%llx\n" : "write_end: -> %d s=%llx,%llx\n", err, u->mmu, i->i_size) );
  return err;
}

#else // #if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN

#ifndef UnlockPage
  #define UnlockPage unlock_page
#endif

///////////////////////////////////////////////////////////
// cont_prepare_write64
//
// stolen from fs/buffer.c
//
// unsigned long* bytes is changed to loff_t* bytes
///////////////////////////////////////////////////////////
static int
cont_prepare_write64(
    IN struct page*   page,
    IN unsigned       offset,
    IN unsigned       to,
    IN get_block_t*   get_block,
    IN OUT loff_t*    bytes
    )
{
  struct address_space *mapping = page->mapping;
  struct inode* i = mapping->host;
  struct page *new_page;
  unsigned long pgpos;
  int err;
  unsigned zerofrom;
  unsigned blocksize = 1 << i->i_blkbits;
  char *kaddr;
  loff_t mmu;

  while( page->index > (pgpos = *bytes>>PAGE_CACHE_SHIFT) ) {
    err = -ENOMEM;
//    DebugTrace(0, 0, ("cont64: grab page %lx\n", pgpos) );
    new_page = grab_cache_page( mapping, pgpos );
    if ( NULL == new_page ) {
      DebugTrace(0, 0, ("no memory for page at %lx\n", pgpos) );
      goto out;
    }
    // we might sleep
    if ( *bytes>>PAGE_CACHE_SHIFT != pgpos ) {
      DebugTrace(0, 0, ("sleep: mmu=%llx page=%lx\n", (UINT64)*bytes, pgpos) );
      UnlockPage( new_page );
      page_cache_release(new_page);
      continue;
    }
    zerofrom  = *bytes & ~PAGE_CACHE_MASK;
    if ( zerofrom & (blocksize-1) ) {
      *bytes |= (blocksize-1);
      (*bytes)++;
    }
//    DebugTrace(0, 0, ("cont64: prepare_write: %x,mmu=%llx\n", zerofrom,(UINT64)*bytes) );

    err = block_prepare_write( new_page, zerofrom, PAGE_CACHE_SIZE, get_block );
    if ( 0 != err ) {
      DebugTrace(0, 0, ("out_unmap: page=%lx\n", pgpos) );
      goto out_unmap;
    }
//    DebugTrace(0, UFSD_LEVEL_VFS_WBWE, ("cont64: commit_write: p=%lx, %x,mmu=%llx\n", new_page->index, zerofrom,(UINT64)*bytes) );
    kaddr = kmap_atomic(new_page, KM_USER0);
    memset(kaddr+zerofrom, 0, PAGE_CACHE_SIZE-zerofrom);
    flush_dcache_page(new_page);
    kunmap_atomic(kaddr, KM_USER0);
    generic_commit_write( NULL, new_page, zerofrom, PAGE_CACHE_SIZE );
    UnlockPage( new_page );
    page_cache_release(new_page);

    mmu = ((loff_t)new_page->index << PAGE_CACHE_SHIFT) + PAGE_CACHE_SIZE;
    if ( *bytes < mmu )
      *bytes = mmu;
  }

  if ( page->index < pgpos ) {
    // completely inside the area
    zerofrom = offset;
  } else {
    // page covers the boundary, find the boundary offset
    zerofrom = *bytes & ~PAGE_CACHE_MASK;

    // if we will expand the thing last block will be filled
    if ( to > zerofrom && (zerofrom & (blocksize-1)) ) {
      *bytes |= (blocksize-1);
      (*bytes)++;
    }

    // starting below the boundary? Nothing to zero out
    if ( offset <= zerofrom )
      zerofrom = offset;
  }

//  DebugTrace(0, 0, ("cont64: prepare_write: page=%lx, [%x,%x), mmu=%llx\n", page->index,
//                    zerofrom, to, (UINT64)*bytes ));

  err = block_prepare_write( page, zerofrom, to, get_block );
  if ( 0 != err ) {
    DebugTrace(0, 0, ("cont64: block_prepare_write failed %d\n", err) );
    goto out;
  }

  // Update *bytes
  mmu = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;
  if ( *bytes < mmu ) {
//    DebugTrace(0, UFSD_LEVEL_VFS_WBWE, ("cont64: update mmu: %llx => %llx\n", (UINT64)*bytes, (UINT64)mmu ) );
    *bytes = mmu;
  }

  if ( zerofrom < offset ) {
    kaddr = kmap_atomic(page, KM_USER0);
    memset(kaddr+zerofrom, 0, offset-zerofrom);
    flush_dcache_page(page);
    kunmap_atomic(kaddr, KM_USER0); // WAS (???): block_commit_write calls kunmap
    DebugTrace(0, UFSD_LEVEL_VFS_WBWE, ("cont64: zero tail p=%lx, [%x,%x), mmu=%llx\n", page->index,
                       zerofrom, offset, (UINT64)*bytes ));
    generic_commit_write( NULL, page, zerofrom, offset );
  }
  return 0;

out_unmap:
  UnlockPage( new_page );
  page_cache_release( new_page );
out:
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_prepare_write
//
// address_space_operations::prepare_write
///////////////////////////////////////////////////////////
static int
ufsd_prepare_write(
    IN struct file* file  __attribute__((__unused__)),
    IN struct page* page,
    IN unsigned     from,
    IN unsigned     to
    )
{
  int err;
  struct inode* i = page->mapping->host;
  unode* u        = UFSD_U( i );
  UINT64 mmu, off = (UINT64)page->index << PAGE_CACHE_SHIFT;

  DebugTrace(+1, UFSD_LEVEL_VFS_WBWE, ("prepare_write: r=%lx p=%p,%lx o=%llx from %x to %x s=(%llx,%llx)\n",
                        i->i_ino, page, page->flags, off, from, to, u->mmu, (UINT64)i_size_read( i )));

  assert( NULL == file || !IsStream( file ) );

  if ( u->encrypt ) {
    err = -ENOSYS;
  } else if ( u->sparse ) {
    mmu = i->i_size;
    err = block_prepare_write( page, from, to, ufsd_get_block_prep );
  } else {
    mmu = u->mmu;
    err = cont_prepare_write64( page, from, to, ufsd_get_block_prep, &u->mmu );
  }

  if ( 0 != err ) {
    DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, ("prepare_write -> error %d\n", err));
    return err;
  }

  if ( !u->sparse ) {
    unsigned z;

    if ( off + to > mmu ) {
      z = to;
      goto ZeroTail;
    }

    if ( off <= mmu && mmu < off + PAGE_CACHE_SIZE ) {
      z  = mmu & (PAGE_CACHE_SIZE - 1);
ZeroTail:
      assert( z <= PAGE_CACHE_SIZE );
      if ( z < PAGE_CACHE_SIZE ) {
        DebugTrace(0, Dbg, ("zero tail [%llx %llx)\n", off + z, off + PAGE_CACHE_SIZE) );
        memset( kmap(page) + z, 0, PAGE_CACHE_SIZE - z );
        flush_dcache_page( page );
        kunmap( page );
      }
    }
  }

  //
  // Update ondisk sizes
  //
  off += to;
  if ( off > i->i_size )
    mark_inode_dirty( i );

  if ( off > u->mmu ) {
    u->mmu = off;
    mark_inode_dirty( i );
  }

  DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, ("prepare_write: -> 0, p=%lx,s=%llx,%llx\n", page->flags, u->mmu, (UINT64)i->i_size) );

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_commit_write
//
// address_space_operations::commit_write
///////////////////////////////////////////////////////////
static int
ufsd_commit_write(
    IN struct file* file,
    IN struct page* page,
    IN unsigned     from,
    IN unsigned     to
    )
{
  int err;
  DebugTrace(+1, UFSD_LEVEL_VFS_WBWE, ("commit_write: r=%lx p=%p,%lx off=%llx len=%x,s=%llx,%llx\n",
            page->mapping->host->i_ino, page, page->flags, ((UINT64)page->index << PAGE_CACHE_SHIFT) + from,
            to-from, UFSD_U(page->mapping->host)->mmu, (UINT64)page->mapping->host->i_size));

  assert( NULL == file || !IsStream( file ) );

#ifdef UFSD_NTFS
  if ( ufsd_fix_page_buffers( page, 0 ) ) {
    //
    // File does not have allocation of this Vsn = Offset >> sbi->SctBits
    // Read/Write file synchronously
    // ( if UFSD deals with NTFS then we have compressed/sparsed/resident file )
    //
    struct inode* i = page->mapping->host;
    usuper* sbi     = UFSD_SB( i->i_sb );
    unode* u        = UFSD_U( i );
    UINT64 pos      = ((UINT64)page->index << PAGE_CACHE_SHIFT) + from;
    unsigned len    = to - from;
    char* kaddr     = kmap(page);
    size_t tmp;

    //
    // Call UFSD library
    //
    LockUfsd( sbi );

    //
    // Write file via UFSD -> UFSD_BdWrite
    //
//    DebugTrace(0, UFSD_LEVEL_VFS_WBWE, ("commit_write: use ufsd to write file: %llx, %x\n", pos, len ));

    err = UFSDAPI_FileWrite( sbi->Ufsd, u->ufile, NULL, 0, pos, len, kaddr + from, &tmp );
    assert( 0 != err || tmp == len );

    pos += len;
    if ( pos > i->i_size )
      i_size_write( i, pos );

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
    //
    // Check results
    //
    if ( 0 == err && try_to_free_buffers( page ) && 0 == from ) {
      if ( pos > i->i_size ) {
        memset( kaddr + to, 0, PAGE_CACHE_SIZE - to );
        flush_dcache_page( page );
        SetPageUptodate( page );
      } else if ( len >= PAGE_CACHE_SIZE )
        SetPageUptodate( page );
    }
#endif

    UnlockUfsd( sbi );

    kunmap( page );

    TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds();
    mark_inode_dirty( i );

  } else
#endif // #ifdef UFSD_NTFS
  {
    // Use generic function
    err = generic_commit_write( file, page, from, to );
  }

  DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, ("commit_write: -> %d, p=%lx,s=%llx,%llx\n", err,
              page->flags, UFSD_U(page->mapping->host)->mmu, (UINT64)page->mapping->host->i_size) );
  return err;
}

#endif // #if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN


///////////////////////////////////////////////////////////
// ufsd_readpage
//
// address_space_operations::readpage
///////////////////////////////////////////////////////////
static int
ufsd_readpage(
    IN struct file *file __attribute__((__unused__)) ,
    IN struct page *page
    )
{
  int err;
#if defined HAVE_DECL_MPAGE_READPAGE && HAVE_DECL_MPAGE_READPAGE
  DebugTrace(+1, Dbg, ("readpage: r=%lx %llx\n", page->mapping->host->i_ino, (UINT64)page->index << PAGE_CACHE_SHIFT ));
  err = mpage_readpage( page, ufsd_get_block );
#else
  struct inode* i = page->mapping->host;
  sector_t block, lblock;
  struct buffer_head *bh, *head;
  unsigned int blocksize, blocks;
  int j, submitted = 0;
  err = 0;

  DebugTrace(+1, Dbg, ("readpage: r=%lx p=%p, %llx\n", i->i_ino, page, (UINT64)page->index << PAGE_CACHE_SHIFT ));

  assert( NULL == file || !IsStream( file ) );

  BUG_ON(!PageLocked(page));
  blocksize = 1 << i->i_blkbits;
  if ( NULL == page->buffers )
    create_empty_buffers( page, i->i_dev, blocksize );

  head    = page->buffers;
  blocks  = PAGE_CACHE_SIZE >> i->i_blkbits;
  block   = page->index << (PAGE_CACHE_SHIFT - i->i_blkbits);
  lblock  = (i->i_size+blocksize-1) >> i->i_blkbits;
  bh      = head;
  j       = 0;

  //
  // Stage 1: Map the buffers
  //
  do {
    if ( !buffer_uptodate( bh ) && !(buffer_mapped( bh ) && -1 != bh->b_blocknr) ) {
      if ( block < lblock
        && 0 != ufsd_get_block( i, block, bh, 0 ) ) {
          SetPageError( page );
      }

      if ( buffer_uptodate( bh ) ) {
//        map_buffer_to_page(page, bh, block);
        goto confused;
      } else {
        char* d = atomic_kmap( page );
        memset( d + j*blocksize, 0, blocksize );
        flush_dcache_page( page );
        atomic_kunmap( d );
        set_buffer_uptodate( bh );
      }
    }
  } while ( j++, block++, (bh = bh->b_this_page) != head);

  //
  // Stage 2: lock the buffers
  //
  do {
    if ( !buffer_mapped( bh ) || -1 == bh->b_blocknr ) {
      assert( buffer_uptodate( bh ) );
      continue;
    }

    assert( buffer_mapped( bh ) );
    lock_buffer( bh );
    set_buffer_async_io( bh );
  } while ((bh = bh->b_this_page) != head);

  //
  // Stage 3: submit the IO
  //
  assert( 0 == submitted );
  do {
    struct buffer_head *next = bh->b_this_page;
    if ( buffer_async( bh ) ) {
      assert( buffer_mapped( bh ) );
      submit_bh(READ, bh);
      submitted += 1;
    }
    bh = next;
  } while ( bh != head );

  DebugTrace(0, Dbg, ("readpage: submitted %u\n", submitted ));

  if ( 0 == submitted ) {
confused:
    //
    // All buffers are uptodate - we can set the page uptodate
    // as well. But not if get_block() returned an error.
    //
    if ( !PageError( page ) )
      SetPageUptodate( page );
    UnlockPage( page );
  } else {
    //
    // end_buffer_io_async will unlock
    //
#if defined HAVE_DECL_WAKEUP_PAGE_WAITERS && HAVE_DECL_WAKEUP_PAGE_WAITERS
    wakeup_page_waiters( page );
#endif
  }

#endif
  DebugTrace(-1, Dbg, ("readpage -> %d\n", err ));
  return err;
}


#if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_READPAGES && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_READPAGES
///////////////////////////////////////////////////////////
// ufsd_readpages
//
// address_space_operations::readpages
///////////////////////////////////////////////////////////
static int
ufsd_readpages(
    IN struct file*           file __attribute__((__unused__)) ,
    IN struct address_space*  mapping,
    IN struct list_head*      pages,
    IN unsigned               nr_pages
    )
{
  int err;
  DebugTrace(+1, Dbg, ("readpages r=%lx (%u)\n", mapping->host->i_ino, nr_pages));
  assert( NULL == file || !IsStream( file ) );
  err = mpage_readpages( mapping, pages, nr_pages, ufsd_get_block );
  DebugTrace(-1, Dbg, ("readpages -> %d\n", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_writepages
//
// address_space_operations::writepages
///////////////////////////////////////////////////////////
static int
ufsd_writepages(
    IN struct address_space*      m,
    IN struct writeback_control*  w
    )
{
  int err;
  struct inode* i = m->host;
  TRACE_ONLY( int nr; )
  ProfileEnter( UFSD_SB(i->i_sb), writepages );

//  w->nr_to_write *= 8;

  // Save current 'nr_to_write' to show the number of written pages after 'mpage_writepages'
  TRACE_ONLY( nr = w->nr_to_write; )

  DebugTrace(+1, Dbg, ("%u: writepages r=%lx, %ld \"%s\"\n", jiffies_to_msecs(jiffies-StartJiffies), i->i_ino, w->nr_to_write, current->comm ));
  err = mpage_writepages( m, w, UFSD_U(i)->compr? NULL : ufsd_get_block_writepage );
  DebugTrace(-1, Dbg, ("%u: writepages -> %d, %ld\n", jiffies_to_msecs(jiffies-StartJiffies), err, nr - w->nr_to_write ));

  ProfileLeave( UFSD_SB(i->i_sb), writepages );
  return err;
}

#endif


#ifdef UFSD_DELAY_ALLOC

#if !(defined HAVE_DECL___BLOCK_WRITE_BEGIN & HAVE_DECL___BLOCK_WRITE_BEGIN)
  #define __block_write_begin block_prepare_write
#endif

#define MPAGE_DA_EXTENT_TAIL 0x01

#define BH_FLAGS ( (1 << BH_Uptodate) | (1 << BH_Mapped) | (1 << BH_Delay) )


//
// For delayed allocation tracking
//
typedef struct {
  struct inode* inode;
  sector_t      b_blocknr;     // start block number of extent
  size_t        b_size;        // size of extent
  unsigned long b_state;       // state of the extent
  unsigned long first_page;    // extent of pages
  unsigned long next_page;     // extent of pages
  struct writeback_control* wbc;
  int io_done;
  int pages_written;
  int retval;
} mpage_da_data;


///////////////////////////////////////////////////////////
// walk_page_buffers
//
//
///////////////////////////////////////////////////////////
static int
walk_page_buffers(
    IN struct buffer_head* head,
    IN unsigned from,
    IN unsigned to,
    IN int *partial,
    IN int (*fn)(struct buffer_head *bh)
    )
{
  struct buffer_head *bh  = head;
  unsigned block_start    = 0;
  unsigned blocksize      = head->b_size;

  do {
    struct buffer_head* next = bh->b_this_page;
    unsigned block_end       = block_start + blocksize;

    if ( from < block_end && block_start < to ) {
      int err = (*fn)( bh );
      if ( err )
        return err;
    } else if ( NULL != partial && !buffer_uptodate( bh ) )
      *partial = 1;

    bh = next;
    block_start = block_end;
  } while( bh != head );

  return 0;
}


///////////////////////////////////////////////////////////
// noalloc_get_block_write
//
// This function is used as a standard get_block_t callback function
// when there is no desire to allocate any blocks.  It is used as a
// callback function for block_write_begin() and block_write_full_page().
// These functions should only try to map a single block at a time.
//
// Since this function doesn't do block allocations even if the caller
// requests it by passing in create=1, it is critically important that
// any caller checks to make sure that any buffer heads are returned
// by this function are either all already mapped or marked for
// delayed allocation before calling  block_write_full_page().  Otherwise,
// b_blocknr could be left unitialized, and the page write functions will
// be taken by surprise.
///////////////////////////////////////////////////////////
static int
noalloc_get_block_write(
    IN struct inode*  i,
    IN sector_t       iblock,
    IN struct buffer_head* bh,
    IN int            create
    )
{
  int err;
  UNREFERENCED_PARAMETER( create );

//  DebugTrace(+1, Dbg, ("noalloc_get_block_write\n"));
  err = ufsd_get_block_flags( i, iblock, bh, 0, UFSD_FLAG_NOALLOC );
//  DebugTrace(-1, Dbg, ("noalloc_get_block_write\n"));

  return err;
}


///////////////////////////////////////////////////////////
// mpage_da_submit_io
//
// Walks through extent of pages and try to write them with writepage() call back
//
// By the time mpage_da_submit_io() is called we expect all blocks
// to be allocated. this may be wrong if allocation failed.
//
// As pages are already locked by write_cache_pages(), we can't use it
///////////////////////////////////////////////////////////
static int
mpage_da_submit_io(
    IN mpage_da_data* mpd,
    IN sector_t       vbn,
    IN sector_t       lbn,
    IN sector_t       len // may be 0
    )
{
  struct pagevec pvec;
  int ret = 0, err;
  struct inode* i = mpd->inode;
  struct address_space* m = i->i_mapping;
  loff_t size = i_size_read(i);
  unsigned int head, block_start;
  struct buffer_head *bh, *page_bufs = NULL;
  sector_t Lbn = 0, Vbn = 0;

  unsigned long index = mpd->first_page;
  unsigned long end   = mpd->next_page - 1;

  BUG_ON(mpd->next_page <= mpd->first_page);

  DebugTrace(+1, Dbg, ("da_submit_io: r=%lx, [%lx %lx)\n", i->i_ino, index, end+1 ));

  //
  // We need to start from the first_page to the next_page - 1
  // to make sure we also write the mapped dirty buffer_heads.
  // If we look at mpd->b_blocknr we would only be looking
  // at the currently mapped buffer_heads.
  //
  pagevec_init( &pvec, 0 );

  while ( index <= end ) {
    int j, pages = pagevec_lookup( &pvec, m, index, PAGEVEC_SIZE );
    if ( 0 == pages )
      break;
    for ( j = 0; j < pages; j++ ) {
      int commit_write = 0, redirty_page = 0;
      struct page *page = pvec.pages[j];

      index = page->index;
      if ( index > end )
        break;

      head = index == (size >> PAGE_CACHE_SHIFT)? (size & ~PAGE_CACHE_MASK) : PAGE_CACHE_SIZE;

      if ( len ) {
        Vbn = index << (PAGE_CACHE_SHIFT - i->i_blkbits);
        Lbn = lbn + (Vbn - vbn);
      }
      index += 1;

      BUG_ON( !PageLocked( page ) );
      BUG_ON( PageWriteback( page ) );

      //
      // If the page does not have buffers (for whatever reason), try to create them using
      // __block_write_begin. If this fails, redirty the page and move on.
      //
      if ( !page_has_buffers( page ) ) {
        if ( __block_write_begin( page, 0, head, noalloc_get_block_write ) ) {
redirty_page:
          redirty_page_for_writepage( mpd->wbc, page );
          unlock_page( page );
          continue;
        }
        commit_write = 1;
      }

      bh = page_bufs = page_buffers( page );
      block_start = 0;
      do {
        if ( !bh )
          goto redirty_page;
        if ( len && Vbn >= vbn && Vbn <= vbn + len - 1 ) {
          if ( buffer_delay( bh ) ) {
            clear_buffer_delay( bh );
            bh->b_blocknr = Lbn;
          }

          if ( buffer_mapped( bh ) )
            BUG_ON( bh->b_blocknr != Lbn );
        }

        // redirty page if block allocation undone
        if ( buffer_delay( bh ) )
          redirty_page = 1;
        bh = bh->b_this_page;
        block_start += bh->b_size;
        Vbn += 1;
        Lbn += 1;
      } while ( bh != page_bufs );

      if ( redirty_page )
        goto redirty_page;

      if ( commit_write )
        // mark the buffer_heads as dirty & uptodate
        block_commit_write( page, 0, head );

      err = block_write_full_page( page, noalloc_get_block_write, mpd->wbc );

      if ( !err )
        mpd->pages_written += 1;

      //
      // In error case, we have to continue because remaining pages are still locked
      //
      if ( 0 == ret )
        ret = err;
    }
    pagevec_release( &pvec );
  }

  DebugTrace(-1, Dbg, ("da_submit_io -> %d\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_da_block_invalidatepages
//
//
///////////////////////////////////////////////////////////
static void
ufsd_da_block_invalidatepages(
    IN mpage_da_data* mpd,
    IN sector_t       vbn,
    IN long           len
    )
{
  struct pagevec pvec;
  struct inode* i         = mpd->inode;
  struct address_space* m = i->i_mapping;
  pgoff_t index = vbn >> (PAGE_CACHE_SHIFT - i->i_blkbits);
  pgoff_t end   = (vbn + len - 1) >> (PAGE_CACHE_SHIFT - i->i_blkbits);

  while ( index <= end ) {
    int j, pages = pagevec_lookup( &pvec, m, index, PAGEVEC_SIZE );
    if ( 0 == pages )
      break;

    for ( j = 0; j < pages; j++ ) {
      struct page *page = pvec.pages[j];
      if ( page->index > end )
        break;
      BUG_ON(!PageLocked( page ) );
      BUG_ON(PageWriteback( page ) );
#if defined HAVE_DECL_ASO_INVALIDATEPAGE_V1 && HAVE_DECL_ASO_INVALIDATEPAGE_V1
      block_invalidatepage( page, 0 );
#elif defined HAVE_DECL_ASO_INVALIDATEPAGE_V2 && HAVE_DECL_ASO_INVALIDATEPAGE_V2
      block_invalidatepage( page, 0, PAGE_CACHE_SIZE );
#else
#error "Unknown block_invalidatepage"
#endif
      ClearPageUptodate( page );
      unlock_page( page );
    }
    index = pvec.pages[pages - 1]->index + 1;
    pagevec_release( &pvec );
  }
}


///////////////////////////////////////////////////////////
// mpage_da_map_and_submit
//
// Go through given space, map them if necessary, and then submit them for I/O
// The function skips space we know is already mapped to disk blocks.
///////////////////////////////////////////////////////////
static int
mpage_da_map_and_submit(
    IN mpage_da_data* mpd
    )
{
  int err, get_blocks_flags;
  struct buffer_head bh;
  struct inode* i     = mpd->inode;
  sector_t next       = mpd->b_blocknr;
  unsigned blkbits    = i->i_blkbits;
  sector_t vbn = 0, lbn = 0, len = 0;
  TRACE_ONLY( const char* hint; )

  DebugTrace(+1, Dbg, ("da_map_and_submit: r=%lx, [%lx %lx)\n", i->i_ino, mpd->first_page, mpd->next_page ));

  //
  // If the blocks are mapped already, or we couldn't accumulate
  // any blocks, then proceed immediately to the submission stage.
  //
  if ( 0 == mpd->b_size
    || ( (mpd->b_state & (1 << BH_Mapped)) && !(mpd->b_state & (1 << BH_Delay)) ) )
  {
    TRACE_ONLY( hint = "0"; )
    err = 0;
  }
  else
  {
    //
    // Call ufsd_get_block_flags() to allocate any delayed allocation blocks
    //
    bh.b_state = 0;
    bh.b_size  = mpd->b_size;

    get_blocks_flags = 0;
//  if ( mpd->b_state & (1 << BH_Delay) )
//    get_blocks_flags |= UFSD_GET_BLOCKS_DELALLOC_RESERVE;

    err = ufsd_get_block_flags( i, next, &bh, 1, get_blocks_flags );

    if ( err ) {
      //
      // If get block returns EAGAIN or ENOSPC and there
      // appears to be free blocks we will call
      // ufsd_writepage() for all of the pages which will
      // just redirty the pages.
      //
      if ( -EAGAIN == err ) {
        TRACE_ONLY( hint = "eagain"; )
      } else if ( -ENOSPC == err ) {//&& ufsd_count_free_blocks( sb ) ) {
        mpd->retval = err;
        TRACE_ONLY( printk( KERN_CRIT" no free space\n" ); )
        TRACE_ONLY( hint = "nospc"; )
      } else {
        TRACE_ONLY( printk( KERN_CRIT" invalidate pages\n" ); )

        // invalidate all the pages
        ufsd_da_block_invalidatepages( mpd, next, mpd->b_size >> blkbits );
        TRACE_ONLY( hint = "invalidate"; )
      }
    }
    else
    {
      vbn = next;
      lbn = bh.b_blocknr;
      len = bh.b_size >> blkbits;

      if ( mpd->b_state & (1 << BH_Delay) )
        ufsd_da_release_space( i, len );

      if ( buffer_new( &bh ) ) {
        struct block_device* bdev = i->i_sb->s_bdev;
        int j;

        for ( j = 0; j < len; j++ )
          unmap_underlying_metadata( bdev, bh.b_blocknr + j );
      }

      TRACE_ONLY( hint = "ok"; )
    }
  }

  mpage_da_submit_io( mpd, vbn, lbn, len );
  mpd->io_done = 1;

  DebugTrace(-1, Dbg, ("da_map_and_submit(%s) -> %d\n", hint, err ));
  return err;
}


///////////////////////////////////////////////////////////
// mpage_add_bh_to_extent
//
// Function is used to collect contig. blocks in same state
///////////////////////////////////////////////////////////
static void
mpage_add_bh_to_extent(
    IN OUT mpage_da_data* mpd,
    IN sector_t           vbn,
    IN size_t             b_size,
    IN unsigned long      b_state
    )
{
  //
  // First block in the extent
  //
  if ( 0 == mpd->b_size ) {
    mpd->b_blocknr  = vbn;
    mpd->b_size     = b_size;
    mpd->b_state    = b_state & BH_FLAGS;
  } else {
    sector_t next = mpd->b_blocknr + (mpd->b_size >> mpd->inode->i_blkbits);

    //
    // Can we merge the block to our big extent?
    //
    if ( vbn == next && (b_state & BH_FLAGS) == mpd->b_state ) {
      mpd->b_size += b_size;
    } else {
      //
      // We couldn't merge the block to our extent, so we
      // need to flush current  extent and start new one
      //
      mpage_da_map_and_submit( mpd );
    }
  }
}


///////////////////////////////////////////////////////////
// ufsd_bh_delay
//
//
///////////////////////////////////////////////////////////
static int
ufsd_bh_delay(
    struct buffer_head *bh
    )
{
  return buffer_delay( bh ) && buffer_dirty( bh );
}


///////////////////////////////////////////////////////////
// __mpage_da_writepage
//
// The function finds extents of pages and scan them for all blocks
///////////////////////////////////////////////////////////
static int
__mpage_da_writepage(
    IN struct page*   page,
    IN struct writeback_control* wbc,
    OUT mpage_da_data* mpd
    )
{
  struct inode* i = mpd->inode;
  sector_t vbn;

//  DebugTrace(+1, Dbg, ("mpage_da_writepage: r=%lx p=%lx\n", i->i_ino, page->index ));

  //
  // Can we merge this page to current extent?
  //
  if ( mpd->next_page != page->index ) {
    //
    // Nope, we can't. So, we map non-allocated blocks and start IO on them
    //
    if ( mpd->next_page != mpd->first_page ) {
      mpage_da_map_and_submit( mpd );
      //
      // skip rest of the page in the page_vec
      //
      redirty_page_for_writepage( wbc, page );
      unlock_page( page );
//      DebugTrace(-1, Dbg, ("mpage_da_writepage -> tail\n" ));
      return MPAGE_DA_EXTENT_TAIL;
    }

    //
    // Start next extent of pages ...
    //
    mpd->first_page = page->index;
    mpd->b_size     = 0;
    mpd->b_state    = 0;
    mpd->b_blocknr  = 0;
  }

  mpd->next_page  = page->index + 1;
  vbn             = (sector_t) page->index << (PAGE_CACHE_SHIFT - i->i_blkbits);

  if ( !page_has_buffers( page ) ) {
//    DebugTrace(0, Dbg, ("no buffers for page %lx\n", page->index ));
    mpage_add_bh_to_extent( mpd, vbn, PAGE_CACHE_SIZE, (1 << BH_Dirty) | (1 << BH_Uptodate) );
    if ( mpd->io_done ) {
//      DebugTrace(-1, Dbg, ("mpage_da_writepage -> tail\n" ));
      return MPAGE_DA_EXTENT_TAIL;
    }
  } else {
    //
    // Page with regular buffer heads, just add all dirty ones
    //
    struct buffer_head* head  = page_buffers( page );
    struct buffer_head *bh    = head;

    do {
      BUG_ON( buffer_locked(bh) );
      //
      // We need to try to allocate unmapped blocks in the same page.
      // Otherwise we won't make progress with the page in UFSD_writepage
      //
      if ( ufsd_bh_delay( bh ) ) {
        mpage_add_bh_to_extent( mpd, vbn, bh->b_size, bh->b_state );
        if ( mpd->io_done ) {
//          DebugTrace(-1, Dbg, ("mpage_da_writepage -> tail\n" ));
          return MPAGE_DA_EXTENT_TAIL;
        }
      } else if ( buffer_dirty( bh ) && buffer_mapped( bh ) ) {
//        DebugTrace(0, Dbg, ("mapped + dirty\n" ));
        //
        // mapped dirty buffer. We need to update the b_state because we look at
        // b_state in mpage_da_map_blocks. We don't update b_size because if we find an
        // unmapped buffer_head later we need to use the b_state flag of that buffer_head.
        //
        if ( 0 == mpd->b_size )
          mpd->b_state = bh->b_state & BH_FLAGS;
      }
      vbn += 1;
      bh = bh->b_this_page;
    } while ( bh != head );
  }

//  DebugTrace(-1, Dbg, ("mpage_da_writepage -> 0\n" ));
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_da_writepage
//
// address_space_operations::writepage
///////////////////////////////////////////////////////////
static int
ufsd_da_writepage(
    IN struct page* page,
    IN struct writeback_control*  wbc
    )
{
  struct buffer_head* head;
  int ret, commit_write = 0;
  struct inode* i = page->mapping->host;
  loff_t size     = i_size_read( i );
  unsigned int len = page->index == (size >> PAGE_CACHE_SHIFT)? (size & ~PAGE_CACHE_MASK) : PAGE_CACHE_SIZE;

  DebugTrace(+1, Dbg, ("da_writepage: r=%lx o=%llx\n", i->i_ino, (UINT64)page->index << PAGE_CACHE_SHIFT ));

  //
  // If the page does not have buffers (for whatever reason),
  // try to create them using __block_write_begin.  If this
  // fails, redirty the page and move on.
  //
  if ( !page_has_buffers( page ) ) {
    if (__block_write_begin( page, 0, len, noalloc_get_block_write) ) {
redirty_page:
      redirty_page_for_writepage( wbc, page );
      unlock_page(page);
      DebugTrace(-1, Dbg, ("da_writepage: -> 0, redirty\n" ));
      return 0;
    }
    commit_write = 1;
  }

  head = page_buffers( page );
  if ( walk_page_buffers( head, 0, len, NULL, ufsd_bh_delay ) ) {
    //
    // We don't want to do block allocation, so redirty
    // the page and return.  We may reach here when we do
    // a journal commit via journal_submit_inode_data_buffers.
    // We can also reach here via shrink_page_list
    //
    goto redirty_page;
  }

  // now mark the buffer_heads as dirty and uptodate
  if ( commit_write )
    block_commit_write( page, 0, len );

  ret = block_write_full_page( page, noalloc_get_block_write, wbc );

  DebugTrace(-1, Dbg, ("da_writepage -> %d\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_num_dirty_pages
//
// Return the number of contiguous dirty pages in a given inode starting at page 'idx'
///////////////////////////////////////////////////////////
static pgoff_t
ufsd_num_dirty_pages(
    IN struct inode*  i,
    IN pgoff_t        idx,
    IN unsigned int   max_pages
    )
{
  struct address_space* m = i->i_mapping;
  struct pagevec pvec;
  int done = 0;
  pgoff_t num = 0;

  if ( 0 == max_pages )
    return 0;

//  DebugTrace(+1, Dbg, ("num_dirty_pages: r=%lx i=%lx, max=%x\n", i->i_ino, idx, max_pages ));

  pagevec_init( &pvec, 0 );

  do {
    pgoff_t index   = idx;
    int j, nr_pages = pagevec_lookup_tag( &pvec, m, &index, PAGECACHE_TAG_DIRTY, (pgoff_t)PAGEVEC_SIZE );
    if ( 0 == nr_pages )
      break;

    for ( j = 0; j < nr_pages; j++ ) {
      struct page* page = pvec.pages[j];

      lock_page( page );

      if ( unlikely( page->mapping != m )
        || !PageDirty( page )
        || PageWriteback( page )
        || page->index != idx )
      {
        done = 1;
      } else if ( page_has_buffers( page ) ) {
        struct buffer_head* head  = page_buffers( page );
        struct buffer_head* bh    = head;
        do {
          if ( !buffer_delay( bh ) ) {
            done = 1;
            break;
          }
          bh = bh->b_this_page;
        } while ( bh != head );
      }

      unlock_page( page );

      idx += 1;
      num += 1;
      if ( num >= max_pages )
        done = 1;

      if ( done )
        break;
    }
    pagevec_release( &pvec );
  } while( !done );

//  DebugTrace(-1, Dbg, ("num_dirty_pages -> %lx\n", num ));
  return num;
}


///////////////////////////////////////////////////////////
// write_cache_pages_da
//
// Walk the list of dirty pages of the given address space and call
// the callback function (which usually writes the pages).
//
// This is a forked version of write_cache_pages() in mm/page-writeback.c
//  Differences:
//    Range cyclic is ignored.
//    no_nrwrite_index_update is always presumed true
///////////////////////////////////////////////////////////
static int
write_cache_pages_da(
    IN struct address_space*      m,
    IN struct writeback_control*  wbc,
    IN mpage_da_data*             mpd,
    OUT pgoff_t*                  done_index
    )
{
  struct pagevec pvec;
  int ret = 0, done = 0;
  pgoff_t index     = wbc->range_start >> PAGE_CACHE_SHIFT;
  pgoff_t end       = wbc->range_end >> PAGE_CACHE_SHIFT; // Inclusive
  long nr_to_write  = wbc->nr_to_write;
#ifdef PAGECACHE_TAG_TOWRITE
  int tag           = WB_SYNC_ALL == wbc->sync_mode? PAGECACHE_TAG_TOWRITE : PAGECACHE_TAG_DIRTY;
#else
  int tag           = PAGECACHE_TAG_DIRTY;
#endif

  DebugTrace(+1, Dbg, ("write_cache_pages_da: r=%lx, [%lx %lx], sync=%s\n", m->host->i_ino, index, end, WB_SYNC_ALL == wbc->sync_mode?"all" : "!all" ));

  pagevec_init( &pvec, 0 );

  *done_index = index;

  while ( index <= end ) {
    int j, pages = pagevec_lookup_tag( &pvec, m, &index, tag, min(end - index, (pgoff_t)PAGEVEC_SIZE-1) + 1 );

    if ( 0 == pages )
      break;

    for ( j = 0; j < pages; j++ ) {
      struct page* page = pvec.pages[j];

      if ( page->index > end ) {
        DebugTrace(0, Dbg, ("page->index > end\n"));
        done = 1;
        break;
      }

      *done_index = page->index + 1;

      lock_page( page );

      //
      // Page truncated or invalidated. We can freely skip it
      // then, even for data integrity operations: the page
      // has disappeared concurrently, so there could be no
      // real expectation of this data interity operation
      // even if there is now a new, dirty page at the same
      // pagecache address.
      //
      if ( unlikely( page->mapping != m ) ) {
        DebugTrace(0, Dbg, ("page->mapping != m\n"));
unlock_and_continue:
        unlock_page( page );
        continue;
      }

      if ( !PageDirty( page ) ) {
        DebugTrace(0, Dbg, ("!dirty\n"));
        goto unlock_and_continue; // someone wrote it for us
      }

      if ( PageWriteback( page ) ) {
        if ( WB_SYNC_NONE == wbc->sync_mode ) {
          DebugTrace(0, Dbg, ("sync_none\n"));
          goto unlock_and_continue;
        }
        wait_on_page_writeback( page );
      }

      BUG_ON( PageWriteback( page ) );

      if ( !clear_page_dirty_for_io( page ) ) {
        DebugTrace(0, Dbg, ("clear_page_dirty_for_io\n"));
        goto unlock_and_continue;
      }

      ret = __mpage_da_writepage( page, wbc, mpd );

      if ( unlikely( AOP_WRITEPAGE_ACTIVATE == ret ) ) {
        unlock_page( page );
        ret = 0;
      } else if ( 0 != ret ) {
        done = 1;
        break;
      }

      if ( nr_to_write > 0 && 0 == --nr_to_write && WB_SYNC_NONE == wbc->sync_mode ) {
        done = 1;
        break;
      }
    }
    pagevec_release( &pvec );
    cond_resched();
    if ( done )
      break;
  }

  DebugTrace(-1, Dbg, ("write_cache_pages_da -> %d\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_da_writepages
//
// address_space_operations::writepages
///////////////////////////////////////////////////////////
static int
ufsd_da_writepages(
    IN struct address_space*      m,
    IN struct writeback_control*  wbc
    )
{
  mpage_da_data mpd;
  struct inode* i     = m->host;
  int pages_written   = 0;
  long pages_skipped  = wbc->pages_skipped;
  loff_t range_start  = wbc->range_start;
  const unsigned int max_pages = 0xFFFFFFF0;
  int range_whole, range_cyclic, cycled = 1, io_done = 0;
  int err = 0;
  long desired_nr_to_write, nr_to_writebump = 0;
  pgoff_t index, end, done_index = 0;
  DEBUG_ONLY( struct usuper* sbi = UFSD_SB( i->i_sb ); )

  ProfileEnter( sbi, da_writepages );

  DebugTrace(+1, Dbg, ("da_writepages r=%lx, %lx \"%s\", sync=%d, res=%"PSCT"x\n",
                        i->i_ino, wbc->nr_to_write, current->comm, (int)wbc->sync_mode,
                        UFSD_U( i )->i_reserved_data_blocks ));

  //
  // No pages to write? This is mainly a kludge to avoid starting
  // a transaction for special inodes like journal inode on last iput()
  // because that could violate lock ordering on umount
  //
  if ( !m->nrpages || !mapping_tagged( m, PAGECACHE_TAG_DIRTY ) ) {
    DebugTrace(-1, Dbg, ("da_writepages -> 0. no pages to write\n" ));
    return 0;
  }

  TraceFreeSpace( sbi, "wp" );

  range_whole = 0 == wbc->range_start && LLONG_MAX == wbc->range_end;

  range_cyclic = wbc->range_cyclic;
  if ( wbc->range_cyclic ) {
    index = m->writeback_index;
    if ( index )
      cycled = 0;
    wbc->range_start  = index << PAGE_CACHE_SHIFT;
    wbc->range_end    = LLONG_MAX;
    wbc->range_cyclic = 0;
    DebugTrace(0, Dbg, ("da_writepages cyclic from %lx\n", index ));
    end = -1;
  } else {
    index = wbc->range_start >> PAGE_CACHE_SHIFT;
    end   = wbc->range_end >> PAGE_CACHE_SHIFT;
    DebugTrace(0, Dbg, ("da_writepages range [%lx %lx]\n", index, end ));
  }

//  max_pages = sbi->s_max_writeback_mb_bump << (20 - PAGE_CACHE_SHIFT);
  desired_nr_to_write = range_cyclic || !range_whole
    ? ufsd_num_dirty_pages( i, index, max_pages )
    : LONG_MAX == wbc->nr_to_write
    ? wbc->nr_to_write
    : wbc->nr_to_write * 8;

  if ( desired_nr_to_write > max_pages )
    desired_nr_to_write = max_pages;

  if ( wbc->nr_to_write < desired_nr_to_write ) {
    nr_to_writebump  = desired_nr_to_write - wbc->nr_to_write;
    wbc->nr_to_write = desired_nr_to_write;
  }

//  DebugTrace(0, Dbg, ("nr_to_write = %lx\n", wbc->nr_to_write ));

  mpd.wbc   = wbc;
  mpd.inode = m->host;

retry:
#if defined HAVE_DECL_TAG_PAGES_FOR_WRITEBACK && HAVE_DECL_TAG_PAGES_FOR_WRITEBACK
  if ( WB_SYNC_ALL == wbc->sync_mode )
    tag_pages_for_writeback( m, index, end );
#endif

  while ( wbc->nr_to_write > 0 ) {

    mpd.b_size        = 0;
    mpd.b_state       = 0;
    mpd.b_blocknr     = 0;
    mpd.first_page    = 0;
    mpd.next_page     = 0;
    mpd.io_done       = 0;
    mpd.pages_written = 0;
    mpd.retval        = 0;
    err = write_cache_pages_da( m, wbc, &mpd, &done_index );

    //
    // If we have a contiguous extent of pages and we haven't done the I/O yet, map the blocks and submit them for I/O.
    //
    if ( !mpd.io_done && mpd.next_page != mpd.first_page ) {
      err = mpage_da_map_and_submit( &mpd );
      if ( 0 == err )
        err = MPAGE_DA_EXTENT_TAIL;
    }

    wbc->nr_to_write -= mpd.pages_written;

    if ( MPAGE_DA_EXTENT_TAIL == err ) {
      //
      // got one extent now try with rest of the pages
      //
      pages_written     += mpd.pages_written;
      wbc->pages_skipped = pages_skipped;
      err     = 0;
      io_done = 1;
    } else if ( err ) {
      printk( KERN_CRIT QUOTED_UFSD_DEVICE": This should not happen leaving with nr_to_write = %ld ret = %d", wbc->nr_to_write, err );
      nr_to_writebump = wbc->nr_to_write;
      goto out_writepages;
    } else if ( wbc->nr_to_write ) {
      //
      // There is no more writeout needed or we requested for a noblocking writeout
      // and we found the device congested
      //
      break;
    }
  }

  if ( !io_done && !cycled ) {
    cycled  = 1;
    index   = 0;
    wbc->range_start  = 0;
    wbc->range_end    = m->writeback_index - 1;
    DebugTrace(0, Dbg, ("retry writepages\n" ));
    goto retry;
  }

  assert( pages_skipped == wbc->pages_skipped );
  if ( pages_skipped != wbc->pages_skipped )
    printk( KERN_CRIT QUOTED_UFSD_DEVICE": This should not happen leaving with nr_to_write = %ld ret = %d", wbc->nr_to_write, err );

  wbc->range_cyclic = range_cyclic;
  //
  // Set the writeback_index so that range_cyclic mode will write it back later
  //
  if ( range_cyclic || (range_whole && wbc->nr_to_write > 0) )
    m->writeback_index = done_index;

out_writepages:
  wbc->nr_to_write -= nr_to_writebump;
  wbc->range_start  = range_start;

  DebugTrace(-1, Dbg, ("da_writepages -> %d, %lx\n", err, wbc->nr_to_write ));

  ProfileLeave( sbi, da_writepages );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_da_get_block_prep
//
// It will either return mapped block or reserve space for a single block.
//
// For delayed buffer_head we have BH_Mapped, BH_New, BH_Delay set.
// We also have b_blocknr = -2 and b_bdev initialized properly
///////////////////////////////////////////////////////////
static int
ufsd_da_get_block_prep(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  BUG_ON( 0 == create );
  BUG_ON( bh->b_size != i->i_sb->s_blocksize );

  return ufsd_get_block_flags( i, iblock, bh, 0, UFSD_FLAG_NOALLOC|UFSD_FLAG_PREPARE );
}


///////////////////////////////////////////////////////////
// ufsd_da_write_begin
//
// address_space_operations::write_begin
///////////////////////////////////////////////////////////
static int
ufsd_da_write_begin(
    IN struct file*           file,
    IN struct address_space*  mapping,
    IN loff_t                 pos,
    IN unsigned               len,
    IN unsigned               flags,
    OUT struct page**         pagep,
    OUT void**                fsdata
    )
{
  int err;
  struct inode* i = mapping->host;
  unode* u        = UFSD_U( i );
  usuper* sbi     = UFSD_SB( i->i_sb );
  long dBlocks    = atomic_long_read( &sbi->DirtyBlocks );
  long fBlocks    = atomic_long_read( &sbi->FreeBlocks );
  get_block_t* get_block;

  DebugTrace(+1, UFSD_LEVEL_VFS_WBWE, ("da_write_begin: r=%lx pos=%llx,%x fl=%x s=%llx,%llx%s\n",
                        i->i_ino, pos, len, flags, u->mmu, i_size_read( i ), u->sparse?",sp":"" ));

  *pagep = NULL;
  if ( u->encrypt ) {
    err = -ENOSYS;
    goto out;
  }

  if ( fBlocks < dBlocks + dBlocks/2 || fBlocks < dBlocks + UFSD_RED_ZONE ) {
    //
    // Free block count is less than 150% of dirty blocks
    //
    TraceFreeSpace( sbi, "turn off delalloc" );
    err = ufsd_alloc_da_blocks( i );
    if ( 0 != err ) {
      DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, ("da_write_begin-> %d, failed to allocate da blocks\n", err ));
      return err;
    }

    get_block = &ufsd_get_block_prep;
  } else {
    get_block = &ufsd_da_get_block_prep;
  }

#ifdef Writeback_inodes_sb_if_idle
  //
  // Even if we don't switch but are nearing capacity,
  // start pushing delalloc when 1/2 of free blocks are dirty.
  //
  if ( fBlocks < 2 * dBlocks )
    Writeback_inodes_sb_if_idle( i->i_sb );
#endif

  ProfileEnter( sbi, da_write_begin );

  flags |= AOP_FLAG_NOFS;

  assert( NULL == file || !IsStream( file ) );
  *fsdata = NULL;

  if ( u->sparse || u->compr ) {
#if defined HAVE_DECL_BLOCK_WRITE_BEGIN_V1 && HAVE_DECL_BLOCK_WRITE_BEGIN_V1
    err = block_write_begin( file, mapping, pos, len, flags, pagep, fsdata, get_block );
#else
    err = block_write_begin( mapping, pos, len, flags, pagep, get_block );
#endif
  } else {
    err = cont_write_begin( file, mapping, pos, len, flags, pagep, fsdata, get_block, &u->mmu );
  }

  ProfileLeave( sbi, da_write_begin );

out:
  DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, ("da_write_begin: -> %d\n", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_da_write_end
//
// address_space_operations::write_end
///////////////////////////////////////////////////////////
static int
ufsd_da_write_end(
    IN struct file* file,
    IN struct address_space* mapping,
    IN loff_t       pos,
    IN unsigned     len,
    IN unsigned     copied,
    IN struct page* page,
    IN void*        fsdata
    )
{
  int err = 0;
  struct inode* i = page->mapping->host;
  unode* u        = UFSD_U( i );
//  UINT64 isize = i->i_size;

  DebugTrace(+1, UFSD_LEVEL_VFS_WBWE, ("da_write_end: r=%lx pos=%llx,%x,%x s=%llx,%llx\n",
                        i->i_ino, pos, len, copied, u->mmu, i->i_size ));

  ProfileEnter( UFSD_SB(i->i_sb), da_write_end );

  // Use generic function
  err = block_write_end( file, mapping, pos, len, copied, page, fsdata );

  if ( err >= 0 ) {
    pos += err;

    if ( pos > i->i_size ) {
      i_size_write( i, pos );
      mark_inode_dirty( i );
    }

    if( pos > u->mmu ) {
      u->mmu = pos;
      mark_inode_dirty( i );
    }
  }

  unlock_page( page );
  page_cache_release( page );

  ProfileLeave( UFSD_SB(i->i_sb), da_write_end );

  DebugTrace(-1, UFSD_LEVEL_VFS_WBWE, (err > 0? "da_write_end: -> %x s=%llx,%llx\n" : "write_end: -> %d s=%llx,%llx\n", err, u->mmu, i->i_size) );
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_da_invalidatepage
//
// address_space_operations::invalidatepage
// Drop reserved blocks
///////////////////////////////////////////////////////////
static void
ufsd_da_invalidatepage(
    IN struct page*   page,
#if defined HAVE_DECL_ASO_INVALIDATEPAGE_V1 && HAVE_DECL_ASO_INVALIDATEPAGE_V1
    IN unsigned long  offset
#elif defined HAVE_DECL_ASO_INVALIDATEPAGE_V2 && HAVE_DECL_ASO_INVALIDATEPAGE_V2
    IN unsigned int   offset,
    IN unsigned int   len
#endif
    )
{
  BUG_ON(!PageLocked(page));

  if ( page_has_buffers( page ) ) {
    unsigned int to_release   = 0;
    unsigned int curr_off     = 0;
    struct buffer_head* head  = page_buffers(page);
    struct buffer_head* bh    = head;

    do {
      unsigned int next_off = curr_off + bh->b_size;

      if ( offset <= curr_off && buffer_delay(bh) ) {
        to_release += 1;
        clear_buffer_delay( bh );
      }
      curr_off = next_off;
      bh = bh->b_this_page;
    } while ( bh != head );

    if ( to_release )
      ufsd_da_release_space( page->mapping->host, to_release );
  }

  // If it's a full truncate we just forget about the pending dirtying
  if ( 0 == offset )
    ClearPageChecked( page );
#if defined HAVE_DECL_ASO_INVALIDATEPAGE_V1 && HAVE_DECL_ASO_INVALIDATEPAGE_V1
  block_invalidatepage( page, offset );
#elif defined HAVE_DECL_ASO_INVALIDATEPAGE_V2 && HAVE_DECL_ASO_INVALIDATEPAGE_V2
  block_invalidatepage( page, offset, len );
#else
#error "Unknown block_invalidatepage"
#endif
}

#endif // #ifdef UFSD_DELAY_ALLOC


///////////////////////////////////////////////////////////
// ufsd_bmap
//
// address_space_operations::bmap
///////////////////////////////////////////////////////////
#if  defined HAVE_DECL_BMAP_V1 && HAVE_DECL_BMAP_V1
static int
ufsd_bmap(
    IN struct address_space*  mapping,
    IN long     block
    )
{
  int ret;
#elif defined HAVE_DECL_BMAP_V2 && HAVE_DECL_BMAP_V2
static sector_t
ufsd_bmap(
    IN struct address_space*  mapping,
    IN sector_t block
    )
{
  sector_t ret;
#endif
  DebugTrace(+1, Dbg, ("bmap (%x)\n", (unsigned)block ));
  ret = generic_block_bmap( mapping, block, ufsd_get_block_bmap );
  DebugTrace(-1, Dbg, ("bmap -> %x\n", (unsigned)ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_get_block_io
//
// This function is a callback for ufsd_direct_IO
///////////////////////////////////////////////////////////
static int
ufsd_get_block_io(
    IN struct inode*        i,
    IN sector_t             iblock,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0) && LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
    IN unsigned long        max_blocks __attribute__(( __unused__ )),
#endif
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  return ufsd_get_block_flags( i, iblock, bh, create, UFSD_FLAG_DIRECT_IO );
}

#if defined HAVE_DECL_ASO_DIRECTIO_V2 && HAVE_DECL_ASO_DIRECTIO_V2

///////////////////////////////////////////////////////////
// ufsd_direct_IO
//
// address_space_operations::direct_IO
///////////////////////////////////////////////////////////
static ssize_t
ufsd_direct_IO(
    IN int                 rw,
    IN struct kiocb*       iocb,
    IN const struct iovec* iov,
    IN loff_t              offset,
    IN unsigned long       nr_segs
    )
{
  struct inode* i = iocb->ki_filp->f_mapping->host;
  ssize_t ret;

  DebugTrace(+1, Dbg, ("direct_IO: %s, %llx, %lu s=%llx,%llx\n",
              (rw&WRITE)? "w":"r", offset, nr_segs, (UINT64)(UFSD_U( i )->mmu), i->i_size ));

#if defined HAVE_DECL_BLOCKDEV_DIRECT_IO_V1 && HAVE_DECL_BLOCKDEV_DIRECT_IO_V1
  ret = blockdev_direct_IO( rw, iocb, i, i->i_sb->s_bdev, iov,
                            offset, ufsd_get_block_io, NULL );
#elif defined HAVE_DECL_BLOCKDEV_DIRECT_IO_V2 && HAVE_DECL_BLOCKDEV_DIRECT_IO_V2
  ret = blockdev_direct_IO( rw, iocb, i, iov, offset, ufsd_get_block_io );
#elif defined HAVE_DECL_BLOCKDEV_DIRECT_IO_V3 && HAVE_DECL_BLOCKDEV_DIRECT_IO_V3
ssize_t ufsd_direct_IO(int rw, struct kiocb * iocb, struct iov_iter *iter, loff_t offset)
{
	struct inode * i = iocb->ki_filp->f_mapping->host;
	return blockdev_direct_IO(rw, iocb, i, iter, offset, ufsd_get_block_io);
}
#elif defined HAVE_DECL_BLOCKDEV_DIRECT_IO_V4 && HAVE_DECL_BLOCKDEV_DIRECT_IO_V4
ssize_t ufsd_direct_IO(struct kiocb * iocb, struct iov_iter *iter, loff_t offset)
{
	struct inode * i = iocb->ki_filp->f_mapping->host;
	return blockdev_direct_IO(iocb, i, iter, offset, ufsd_get_block_io);
}
#else
  #error "Unknown type blockdev_direct_IO"
#endif

  if ( ret > 0 ) {
    DebugTrace(-1, Dbg, ("direct_IO -> %Zx\n", ret ));
  } else {
    DebugTrace(-1, Dbg, ("direct_IO -> %d\n", (int)ret ));
  }
  return ret;
}

#else

///////////////////////////////////////////////////////////
// ufsd_direct_IO
//
// address_space_operations::direct_IO
///////////////////////////////////////////////////////////
static int
ufsd_direct_IO(
    IN int            rw,
    IN struct inode*  i,
    IN struct kiobuf* iobuf,
    IN unsigned long  blocknr,
    IN int            blocksize
    )
{
  int ret;
  DebugTrace(+1, Dbg, ("direct_IO: %s, r=%lx, %lu, s=%llx,%llx\n",
              (rw&WRITE)? "w":"r", i->i_ino, blocknr, (UINT64)(UFSD_U( i )->mmu), (UINT64)i->i_size ));

  ret = generic_direct_IO( rw, i, iobuf, blocknr, blocksize, ufsd_get_block_io );

  DebugTrace(-1, Dbg, (ret > 0? "direct_IO -> %x\n" : "direct_IO -> %d\n", ret ));
  return ret;
}

#endif//HAVE_DECL_ASO_DIRECTIO_V2


//
// Address space operations
//
STATIC_CONST struct address_space_operations ufsd_aops = {
  .writepage      = ufsd_writepage,
  .readpage       = ufsd_readpage,
#if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_SYNC_PAGE && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_SYNC_PAGE
  .sync_page      = block_sync_page,
#endif
#if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_WRITE_BEGIN
  .write_begin    = ufsd_write_begin,
  .write_end      = ufsd_write_end,
#else
  .prepare_write  = ufsd_prepare_write,
  .commit_write   = ufsd_commit_write,
#endif
#if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_READPAGES && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_READPAGES
  .readpages      = ufsd_readpages,
  .writepages     = ufsd_writepages,
#endif
  .bmap           = ufsd_bmap,
  .direct_IO      = ufsd_direct_IO,
#if defined HAVE_STRUCT_ADDRESS_SPACE_IS_PARTIALLY_UPTODATE && HAVE_STRUCT_ADDRESS_SPACE_IS_PARTIALLY_UPTODATE
  .is_partially_uptodate  = block_is_partially_uptodate,
#endif
#if defined HAVE_STRUCT_ADDRESS_SPACE_ERROR_REMOVE_PAGE && HAVE_STRUCT_ADDRESS_SPACE_ERROR_REMOVE_PAGE
  .error_remove_page  = generic_error_remove_page,
#endif
};


#ifdef UFSD_DELAY_ALLOC
//
// Address space operations for delay allocation
//
STATIC_CONST struct address_space_operations ufsd_da_aops = {
  .writepage      = ufsd_da_writepage,
  .readpage       = ufsd_readpage,
#if defined HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_SYNC_PAGE && HAVE_STRUCT_ADDRESS_SPACE_OPERATIONS_SYNC_PAGE
  .sync_page      = block_sync_page,
#endif
  .write_begin    = ufsd_da_write_begin,
  .write_end      = ufsd_da_write_end,
  .readpages      = ufsd_readpages,
  .writepages     = ufsd_da_writepages,
  .bmap           = ufsd_bmap,
  .invalidatepage = ufsd_da_invalidatepage,
  .direct_IO      = ufsd_direct_IO,
#if defined HAVE_STRUCT_ADDRESS_SPACE_IS_PARTIALLY_UPTODATE && HAVE_STRUCT_ADDRESS_SPACE_IS_PARTIALLY_UPTODATE
  .is_partially_uptodate  = block_is_partially_uptodate,
#endif
#if defined HAVE_STRUCT_ADDRESS_SPACE_ERROR_REMOVE_PAGE && HAVE_STRUCT_ADDRESS_SPACE_ERROR_REMOVE_PAGE
  .error_remove_page  = generic_error_remove_page,
#endif
};
#endif


#ifdef UFSD_BIG_UNODE

static u_kmem_cache* unode_cachep;

///////////////////////////////////////////////////////////
// ufsd_alloc_inode
//
// super_operations::alloc_inode
///////////////////////////////////////////////////////////
static struct inode*
ufsd_alloc_inode(
    IN struct super_block *sb
    )
{
  unode* u = kmem_cache_alloc( unode_cachep, GFP_KERNEL );
  if ( NULL == u )
    return NULL;

  //
  // NOTE: explicitly zero all unode members from 'ufile' until the end of struct
  //
  memset( &u->ufile, 0, sizeof(unode) - offsetof(unode,ufile) );

#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
  assert( ACL_NOT_CACHED == u->acl );
  assert( ACL_NOT_CACHED == u->default_acl );
#endif

  return &u->i;
}


///////////////////////////////////////////////////////////
// ufsd_destroy_inode
//
// super_operations::destroy_inode
///////////////////////////////////////////////////////////
static void
ufsd_destroy_inode(
    IN struct inode* i
    )
{
  kmem_cache_free( unode_cachep, UFSD_U( i ) );
}


///////////////////////////////////////////////////////////
// init_once
//
//
///////////////////////////////////////////////////////////
static void
init_once(
#if ( defined HAVE_DECL_KMEM_CACHE_CREATE_V1 && HAVE_DECL_KMEM_CACHE_CREATE_V1 ) \
 || ( defined HAVE_DECL_KMEM_CACHE_CREATE_V2 && HAVE_DECL_KMEM_CACHE_CREATE_V2 )
    IN void*          foo,
    IN u_kmem_cache*  cachep __attribute__((__unused__)),
    IN unsigned long  flags
#elif defined HAVE_DECL_KMEM_CACHE_CREATE_V3 && HAVE_DECL_KMEM_CACHE_CREATE_V3
    IN u_kmem_cache*  cachep __attribute__((__unused__)),
    IN void*          foo
#elif defined HAVE_DECL_KMEM_CACHE_CREATE_V4 && HAVE_DECL_KMEM_CACHE_CREATE_V4
    IN void*          foo
#endif
    )
{
  unode* u = (unode *)foo;

  //
  // NOTE: once init unode members from start to 'ufile'
  //
#if defined SLAB_CTOR_CONSTRUCTOR && defined SLAB_CTOR_VERIFY
  if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) == SLAB_CTOR_CONSTRUCTOR)
#endif
  {
#if defined HAVE_DECL_INODE_INIT_ONCE && HAVE_DECL_INODE_INIT_ONCE
    inode_init_once( &u->i );
#else
    struct inode* i = &u->i;
    memset( i, 0, sizeof( *i ) );
    init_waitqueue_head( &i->i_wait );
    INIT_LIST_HEAD( &i->i_hash );
    INIT_LIST_HEAD( &i->i_data.clean_pages );
    INIT_LIST_HEAD( &i->i_data.dirty_pages );
    INIT_LIST_HEAD( &i->i_data.locked_pages );
    INIT_LIST_HEAD( &i->i_dentry );
    INIT_LIST_HEAD( &i->i_dirty_buffers );
    INIT_LIST_HEAD( &i->i_dirty_data_buffers );
    INIT_LIST_HEAD( &i->i_devices );
    sema_init( &i->i_sem, 1 );
    sema_init( &i->i_zombie, 1 );
    spin_lock_init( &->i_data.i_shared_lock );
#endif
    spin_lock_init( &u->block_lock );
#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
    u->acl = u->default_acl = ACL_NOT_CACHED;
#endif
  }
}

#endif // #ifdef UFSD_BIG_UNODE


///////////////////////////////////////////////////////////
// ufsd_symlink
//
// inode_operations::symlink
///////////////////////////////////////////////////////////
static int
ufsd_symlink(
    IN struct inode*  dir,
    IN struct dentry* de,
    IN const char*    symname
    )
{
  usuper* sbi = UFSD_SB( dir->i_sb );
  struct inode* i = NULL;
  int err;

  UfsdCreate  cr;

  cr.lnk  = NULL;
  cr.data = symname;
  cr.len  = strlen(symname) + 1;
  cr.mode = S_IFLNK;

  DebugTrace(+1, Dbg, ("symlink: r=%lx /\"%.*s\" => \"%s\"\n",
              dir->i_ino, (int)de->d_name.len, de->d_name.name, symname ));

  if ( cr.len > dir->i_sb->s_blocksize ) {
    DebugTrace(0, Dbg, ("symlink name is too long\n" ));
    err = -ENAMETOOLONG;
    goto out;
  }

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( 0 == err ) {

    assert( NULL != i && NULL != UFSD_FH(i) );
    i->i_op = &ufsd_link_inode_operations;

    if ( sbi->options.utf8link )
      err = page_symlink( i, symname, cr.len );

    if ( 0 == err ) {
      i->i_mode &= ~(S_IFDIR | S_IFREG);
      i->i_mode |= S_IFLNK;
      mark_inode_dirty( i );
      d_instantiate( de, i );
    } else {
      drop_nlink( i );
      mark_inode_dirty( i );
      iput( i );
    }
  }

out:
  DebugTrace(-1, Dbg, ("symlink -> %d\n", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_read_inode2
//
// read_inode2() callback
// 'opaque' is passed from iget4()
///////////////////////////////////////////////////////////
static void
ufsd_read_inode2(
    IN struct inode*  i,
    IN OUT void*      param
    )
{
  unode* u                = UFSD_U( i );
  ufsd_iget4_param* p     = (ufsd_iget4_param*)param;
  const UfsdCreate* cr    = p->Create;
  struct super_block* sb  = i->i_sb;
  usuper* sbi             = UFSD_SB( sb );
  int check_special       = 0;
  mode_t mode;

#ifndef UFSD_BIG_UNODE
  if ( NULL == u ) {
    u = UFSD_HeapAlloc( sizeof(unode) );
    if ( NULL == u ) {
      printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to allocate %u bytes\n", (unsigned)sizeof(unode) );
      return;
    }
    memset( u, 0, sizeof(unode) );
    UFSD_U( i ) = u;
    spin_lock_init( &u->block_lock );
  }
#endif

  //
  // Next members are set at this point:
  //
  // i->i_sb    = sb;
  // i->i_dev   = sb->s_dev;
  // i->i_blkbits = sb->s_blocksize_bits;
  // i->i_ino   = Info.Id;
  // i->i_flags = 0;
  //
  assert( i->i_ino == p->Info.Id );
//  assert( NULL == p->lnk );
  assert( 1 == atomic_read( &i->i_count ) );

  i->i_op = NULL;

  //
  // Setup 'uid' and 'gid'
  //
  i->i_uid.val  = unlikely(sbi->options.uid)? sbi->options.fs_uid : cr? cr->uid : p->Info.is_ugm? p->Info.uid : sbi->options.fs_uid;
  i->i_gid.val  = unlikely(sbi->options.gid)? sbi->options.fs_gid : cr? cr->gid : p->Info.is_ugm? p->Info.gid : sbi->options.fs_gid;

  //
  // Setup 'mode'
  //
  if ( p->Info.is_dir ) {
    if ( sbi->options.dmask ) {
      // use mount options "dmask" or "umask"
      mode = S_IRWXUGO & sbi->options.fs_dmask;
    } else if ( NULL != cr ) {
      mode = cr->mode;
      check_special = 1;
    } else if ( p->Info.is_ugm ) {
      // no mount options "dmask"/"umask" and fs supports "ugm"
      mode     = p->Info.mode;
      check_special = 1;
    } else if ( NULL == sb->s_root ) {
      // Read root inode while mounting
      mode = S_IRWXUGO;
    } else {
      // by default ~(current->fs->umask)
      mode = S_IRWXUGO & sbi->options.fs_dmask;
    }
  } else {
    if ( sbi->options.fmask ) {
      // use mount options "fmask" or "umask"
      mode = S_IRWXUGO & sbi->options.fs_fmask;
    } else if ( NULL != cr ) {
      mode = cr->mode;
      check_special = 1;
    } else if ( p->Info.is_ugm ) {
      // no mount options "fmask"/"umask" and fs supports "ugm"
      mode     = p->Info.mode;
      check_special = 1;
    } else {
      // by default ~(current->fs->umask)
      mode = S_IRWXUGO & sbi->options.fs_fmask;
    }
  }

  i->i_mode = mode;
  if ( check_special && ( S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode) ) ) {
    init_special_inode( i, mode, new_decode_dev( p->Info.udev ) );
    i->i_op = &ufsd_special_inode_operations;
  } else {
    assert( NULL == cr || !p->Info.is_ugm || cr->mode == p->Info.mode );
  }

  i->i_version  = 0;
  i->i_generation = p->Info.generation; // Used by NFS
  UfsdTimes2Inode( sbi, u, i, &p->Info );
  i->i_size     = p->Info.size;

  //
  // Setup unode
  //
  u->sparse   = p->Info.is_sparse;
  u->compr    = p->Info.is_compr;
  u->encrypt  = p->Info.is_encrypt;
  u->xattr    = p->Info.is_xattr;
  u->mmu      = p->Info.vsize;
  BUG_ON( 0 != u->Len );


  // NOTE: i_blocks is measured in 512 byte blocks !
  i->i_blocks = p->Info.asize >> 9;

  if ( NULL != i->i_op ) {
    ;
  } else if ( p->Info.is_dir ) {
    // dot and dot-dot should be included in count but was not included
    // in enumeration.
    assert( 1 == p->Info.link_count ); // Usually a hard link to directories are disabled
#ifdef UFSD_COUNT_CONTAINED
    set_nlink( i, p->Info.link_count + p->subdir_count + 1 );
#else
    set_nlink( i, 1 );
#endif
    i->i_op   = &ufsd_dir_inode_operations;
    i->i_fop  = &ufsd_dir_operations;
    i->i_mode |= S_IFDIR;
  } else {
    set_nlink( i, p->Info.link_count );
    i->i_op     = &ufsd_file_inode_operations;
    i->i_fop    = &ufsd_file_operations;
#ifdef UFSD_DELAY_ALLOC
    if ( sbi->options.delalloc )
      i->i_mapping->a_ops = &ufsd_da_aops;
    else
#endif
    i->i_mapping->a_ops = &ufsd_aops;
    i->i_mode |= S_IFREG;
  }

  if ( p->Info.is_readonly )
    i->i_mode &= ~S_IWUGO;

  if ( p->Info.is_link ) {
    i->i_mode &= ~(S_IFDIR | S_IFREG);
    i->i_mode |= S_IFLNK;
    i->i_op    = &ufsd_link_inode_operations;
    i->i_fop   = NULL;
  }

  if ( p->Info.is_system && sbi->options.sys_immutable
    && !( S_ISFIFO(i->i_mode) || S_ISSOCK(i->i_mode) || S_ISLNK(i->i_mode) ) )
  {
    DebugTrace( 0, 0, ("Set inode r=%lx immutable\n", i->i_ino) );
    i->i_flags |= S_IMMUTABLE;
  }
  else
    i->i_flags &= ~S_IMMUTABLE;
#ifdef S_PRIVATE
  i->i_flags |= S_PRIVATE;  // Hack?
#endif

#if defined HAVE_STRUCT_INODE_I_BLKSIZE && HAVE_STRUCT_INODE_I_BLKSIZE
  // Do not touch this member.
  // 'du' works incorrectly if uncomment
//  i->i_blksize = sb->s_blocksize;
#endif

  u->ufile  = p->fh;
  p->fh     = NULL;
}


///////////////////////////////////////////////////////////
// ufsd_create_or_open
//
//  This routine is a callback used to load or create inode for a
//  direntry when this direntry was not found in dcache or direct
//  request for create or mkdir is being served.
///////////////////////////////////////////////////////////
static int
ufsd_create_or_open(
    IN struct inode*        dir,
    IN OUT struct dentry*   de,
    IN UfsdCreate*          cr,
    OUT struct inode**      inode
    )
{
  ufsd_iget4_param param;
  struct inode* i = NULL;
  usuper* sbi = UFSD_SB( dir->i_sb );
  int err = -ENOENT;
  unsigned char* p = 0 == sbi->options.delim? NULL : strchr( de->d_name.name, sbi->options.delim );
  TRACE_ONLY( const char* hint = NULL==cr?"open":S_ISDIR(cr->mode)?"mkdir":cr->lnk?"link":S_ISLNK(cr->mode)?"symlink":cr->data?"mknode":"create"; )

  param.Create        = cr;
  param.subdir_count  = 0;
  param.name          = de->d_name.name;
  param.name_len      = NULL == p? de->d_name.len : p - de->d_name.name;
#if !(defined HAVE_STRUCT_SUPER_BLOCK_S_D_OP && HAVE_STRUCT_SUPER_BLOCK_S_D_OP)
  de->d_op = sbi->options.nocase? &ufsd_dop : NULL;
#endif

  DebugTrace(+1, Dbg, ("%s: r=%lx '%s' m=%o\n", hint, dir->i_ino, de->d_name.name, NULL == cr? 0u : (unsigned)cr->mode ));
//  DebugTrace(+1, Dbg, ("%s: %p '%.*s'\n", hint, dir, (int)param.name_len, param.name));

  //
  // The rest to be set in this routine
  // follows the attempt to open the file.
  //
  LockUfsd( sbi );

  if ( NULL != dir && 0 != LazyOpen( sbi, dir ) ) {
    // Failed to open parent directory
    goto Exit;
  }

  if ( NULL != cr ) {
    cr->uid = current_fsuid().val;
    if ( !(dir->i_mode & S_ISGID) )
      cr->gid = current_fsgid().val;
    else {
      cr->gid = dir->i_gid.val;
      if ( S_ISDIR(cr->mode) )
        cr->mode |= S_ISGID;
    }

    cr->mode &= ~current->fs->umask;

#ifdef UFSD_DELAY_ALLOC
    if ( sbi->options.delalloc
      && atomic_long_read( &sbi->FreeBlocks ) < atomic_long_read( &sbi->DirtyBlocks ) + 1 + UFSD_RED_ZONE ) {

      TraceFreeSpace( sbi, "nospc!" );
      err = -ENOSPC;
      goto Exit;
    }
#endif
  }

  err = UFSDAPI_FileOpen( sbi->Ufsd, UFSD_FH( dir ), param.name, param.name_len,
                          cr,
#ifdef UFSD_COUNT_CONTAINED
                          &param.subdir_count,
#else
                          NULL,
#endif
                          &param.fh, &param.Info );

  switch( err ) {
  case 0: break;
  case ERR_BADNAME_LEN: err = -ENAMETOOLONG; goto Exit;
  case ERR_NOTIMPLEMENTED: err = -ENOSYS; goto Exit;
  case ERR_WPROTECT:  err = -EROFS; goto Exit;
  case ERR_NOSPC:  err = -ENOSPC; goto Exit;
  default:  err = -ENOENT; goto Exit;
  }

  assert( NULL == cr || NULL != param.fh );
  assert( NULL != dir || param.Info.is_dir ); // root must be directory

  //
  // Load and init inode
  // iget4 calls ufsd_read_inode2 for new nodes
  // if node was not loaded then param.fh will be copied into UFSD_FH(inode)
  // and original param.fh will be zeroed
  // if node is already loaded then param.fh will not be changed
  // and we must to close it
  //
  i = iget4( dir->i_sb, param.Info.Id, NULL, &param );

  if ( NULL != param.fh ){
    // inode was already opened
    if ( NULL == i ) {
      DebugTrace(0, Dbg, ("assert: i=NULL, new=%p\n", param.fh ));
    } else {
      DebugTrace(0, Dbg, ("assert: i=%p, l=%x, old=%p, new=%p\n", i, i->i_nlink, UFSD_FH(i), param.fh ));
    }
    // UFSD handle was not used. Close it
    UFSDAPI_FileClose( sbi->Ufsd, param.fh );
  }

  if ( NULL != i ) {
    assert( NULL == cr || NULL != UFSD_FH(i) );
    // OK
    err = 0;

    if ( NULL != cr ) {
      assert( NULL != dir );
#ifdef UFSD_COUNT_CONTAINED
      if ( S_ISDIR ( i->i_mode ) )
        inc_nlink( dir );
#endif
      TIMESPEC_SECONDS( &dir->i_mtime ) = TIMESPEC_SECONDS( &dir->i_ctime ) = get_seconds();
      // Mark dir as requiring resync.
      dir->i_version += 1;
      dir->i_size   = UFSDAPI_GetDirSize( UFSD_FH(dir) );
      dir->i_blocks = dir->i_size >> 9;

      mark_inode_dirty( dir );

      if ( NULL != cr->lnk ){
        i->i_ctime = dir->i_ctime;
      }
#ifdef UFSD_USE_XATTR
      else if ( !sbi->options.acl )
        ;//i->i_mode &= ~current->fs->umask;
      else {
        struct posix_acl* acl;

        UnlockUfsd( sbi );

        acl = ufsd_get_acl( dir, ACL_TYPE_DEFAULT );
        if ( IS_ERR(acl) )
          err = PTR_ERR(acl);
        else if ( NULL == acl )
          ;//i->i_mode &= ~current->fs->umask;
        else {
          posix_acl_mode mode;
          if ( !S_ISDIR( i->i_mode ) || 0 == ( err = ufsd_set_acl( i, ACL_TYPE_DEFAULT, acl ) ) ) {
#if defined HAVE_DECL_POSIX_ACL_CREATE && HAVE_DECL_POSIX_ACL_CREATE
            err = posix_acl_create( &acl, GFP_KERNEL, &mode );
            if ( err >= 0 ) {
              i->i_mode = mode;
              if ( err > 0 )
                err = ufsd_set_acl( i, ACL_TYPE_ACCESS, acl );
            }
#else
            struct posix_acl* clone = posix_acl_clone( acl, GFP_KERNEL );
            if ( NULL == clone )
              err = -ENOMEM;
            else {
              mode  = i->i_mode;
              err   = posix_acl_create_masq( clone, &mode );
              if ( err >= 0 ) {
                i->i_mode = mode;
                if ( err > 0 )
                  err = ufsd_set_acl( i, ACL_TYPE_ACCESS, clone );
              }
              ufsd_posix_acl_release( clone );
            }
#endif
          }
          ufsd_posix_acl_release( acl );
        }
        //
        // Skip UnlockUfsd
        //
        err = 0; // ignore any errors?
        goto Exit1;
      }
#endif
    }
  }

Exit:
  UnlockUfsd( sbi );
#ifdef UFSD_USE_XATTR
Exit1:
#endif

  if ( 0 == err ) {
    DebugTrace(-1, Dbg, ("%s -> i=%p de=%p h=%p r=%Zx l=%x m=%o%s\n",
                         hint, i, de, UFSD_FH(i),
                         param.Info.Id, i->i_nlink, i->i_mode, param.Info.is_sparse?",sp":param.Info.is_compr?",c":""));
  } else {
    DebugTrace(-1, Dbg, ("%s failed %d\n", hint, err ));
  }

  *inode = i;
  return err;
}

#ifdef UFSD_TRACE
///////////////////////////////////////////////////////////
// ParseTraceLevel
//
// parses string for trace level
///////////////////////////////////////////////////////////
static void
ParseTraceLevel(
    IN const char* v
    )
{
  if ( NULL == v || 0 == v[0] )
    UFSD_TraceLevel = UFSD_LEVEL_DEFAULT;
  else if ( 0 == strcmp( v, "all" ) )
    UFSD_TraceLevel = UFSD_LEVEL_STR_ALL;
  else if ( 0 == strcmp( v, "vfs" ) )
    UFSD_TraceLevel = UFSD_LEVEL_STR_VFS;
  else if ( 0 == strcmp( v, "lib" ) )
    UFSD_TraceLevel = UFSD_LEVEL_STR_LIB;
  else if ( 0 == strcmp( v, "mid" ) )
    UFSD_TraceLevel = UFSD_LEVEL_STR_MID;
  else if ( 0 == strcmp( v, "io" ) )
    UFSD_TraceLevel = UFSD_LEVEL_IO;
  else
    UFSD_TraceLevel = simple_strtoul( v, NULL, 16 );
  DebugTrace(0, UFSD_LEVEL_ALWAYS, ("%s: trace mask set to %08lx\n", v, UFSD_TraceLevel));
}
#endif

#if defined CONFIG_PROC_FS

static struct proc_dir_entry* proc_info_root = NULL;
#define PROC_FS_UFSD_NAME "fs/ufsd"

#if !( defined HAVE_DECL_PDE_DATA && HAVE_DECL_PDE_DATA )
  #define PDE_DATA(X) PDE(X)->data
#endif

///////////////////////////////////////////////////////////
// ufsd_proc_dev_version_show
//
// /proc/fs/ufsd/version
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_version_show(
    IN struct seq_file* m,
    IN void*            o
    )
{
  seq_printf( m, "%s%s\ndriver (%s) loaded at %p, sizeof(inode)=%u\n",
              UFSDAPI_LibraryVersion( NULL ), s_FileVer, s_DriverVer, __this_module.module_core, (unsigned)sizeof(struct inode) );

#ifdef UFSD_DEBUG_ALLOC
  {
    size_t Mb = UsedMemMax/(1024*1024);
    size_t Kb = (UsedMemMax%(1024*1024)) / 1024;
    size_t b  = UsedMemMax%1024;
    if ( 0 != Mb ) {
      seq_printf( m, "Memory report: Peak usage %Zu.%03Zu Mb (%Zu bytes), kmalloc %Zu, vmalloc %Zu\n",
                  Mb, Kb, UsedMemMax, TotalKmallocs, TotalVmallocs );
    } else {
      seq_printf( m, "Memory report: Peak usage %Zu.%03Zu Kb (%Zu bytes), kmalloc %Zu, vmalloc %Zu\n",
                  Kb, b, UsedMemMax, TotalKmallocs, TotalVmallocs );
    }
    seq_printf( m, "Total allocated:  %Zu bytes in %Zu blocks, Max request %Zu bytes\n",
                  TotalAllocs, TotalAllocBlocks, MemMaxRequest );
  }
#endif

  return 0;
}

static int ufsd_proc_dev_version_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_version_show, NULL );
}

static const struct file_operations ufsd_proc_dev_version_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_version_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_dirty_show
//
// /proc/fs/ufsd/<dev>/dirty
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_dirty_show(
    IN struct seq_file* m,
    IN void*            o
    )
{
  struct super_block* sb = m->private;
#ifdef UFSD_USE_FLUSH_THREAD
  seq_printf( m, "%u", (unsigned)UFSD_SB( sb )->bDirty );
#else
  seq_printf( m, "%u", (unsigned)sb->s_dirt );
#endif
  return 0;
}

static int ufsd_proc_dev_dirty_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_dirty_show, PDE_DATA(inode) );
}

static const struct file_operations ufsd_proc_dev_dirty_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_dirty_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_volinfo
//
// /proc/fs/ufsd/<dev>/volinfo
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_volinfo(
    IN struct seq_file* m,
    IN void*            o
    )
{
  usuper* sbi = UFSD_SB( (struct super_block*)(m->private) );

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  UFSDAPI_TraceVolumeInfo( sbi->Ufsd, m, &seq_printf );

  UnlockUfsd( sbi );
  return 0;
}

static int ufsd_proc_dev_volinfo_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_volinfo, PDE_DATA(inode) );
}

static const struct file_operations ufsd_proc_dev_volinfo_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_volinfo_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_label_show
//
// /proc/fs/ufsd/<dev>/label
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_label_show(
    OUT struct seq_file*  m,
    IN void*              o
    )
{
  usuper* sbi = UFSD_SB( (struct super_block*)(m->private) );
  char* Label = (char *)alloc_page( GFP_KERNEL | __GFP_ZERO );
	if ( !Label )
		return -ENOMEM;

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  UFSDAPI_QueryVolumeInfo( sbi->Ufsd, NULL, Label, PAGE_SIZE, NULL );

  UnlockUfsd( sbi );

  DebugTrace(0, Dbg, ("read_label: %s\n", Label ) );

  seq_printf( m, "%s\n", Label );

  __free_page( (struct page*)Label );
  return 0;
}

static int ufsd_proc_dev_label_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_label_show, PDE_DATA(inode) );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_label_write
//
// /proc/fs/ufsd/<dev>/label
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_label_write(
    IN struct file* file,
    IN const char __user * buffer,
    IN size_t       count,
    IN OUT loff_t * ppos
    )
{
  struct super_block* sb = PDE_DATA( file_inode( file ) );
  usuper* sbi = UFSD_SB( sb );
  ssize_t ret = count < PAGE_SIZE? count : PAGE_SIZE;
  char* Label = (char *)alloc_page( GFP_KERNEL | __GFP_ZERO );
	if ( !Label )
		return -ENOMEM;

  if ( copy_from_user( Label, buffer, count ) ) {
    ret = -EFAULT;
  } else {
    // Remove last '\n'
    while( ret > 0 && '\n' == Label[ret-1] )
      ret -= 1;
    // Set last zero
    Label[ret] = 0;

    DebugTrace(0, Dbg, ("write_label: %s\n", Label ) );

    //
    // Call UFSD library
    //
    LockUfsd( sbi );

    ret = UFSDAPI_SetVolumeInfo( sbi->Ufsd, Label, ret );

    UnlockUfsd( sbi );

    if ( 0 == ret ){
      ret   = count; // Ok
      *ppos += count;
    } else {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("write_label failed: %x\n", (unsigned)ret ) );
      ret = -EINVAL;
    }
  }
  __free_page( (struct page*)Label );
  return ret;
}

static const struct file_operations ufsd_proc_dev_label_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_label_open,
  .write    = ufsd_proc_dev_label_write,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_tune_show
//
// /proc/fs/ufsd/<dev>/tune
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_tune_show(
    IN struct seq_file* m,
    IN void*            o
    )
{
  usuper* sbi = UFSD_SB( (struct super_block*)(m->private) );
  UfsdVolumeTune vt;

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  if ( 0 != UFSDAPI_QueryVolumeTune( sbi->Ufsd, &vt ) )
    vt.DirAge = vt.JnlRam = 0;

  UnlockUfsd( sbi );

  seq_printf( m, "Ra=%u DirAge=%u JnlRam=%u", sbi->ReadAheadBlocks, vt.DirAge, vt.JnlRam );
  return 0;
}

static int ufsd_proc_dev_tune_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_tune_show, PDE_DATA( inode ) );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_tune_write
//
// /proc/fs/ufsd/<dev>/tune
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_tune_write(
    IN struct file* file,
    IN const char __user * buffer,
    IN size_t       count,
    IN OUT loff_t * ppos
    )
{
  struct super_block* sb = PDE_DATA(file_inode(file));
  usuper* sbi = UFSD_SB( sb );
  ssize_t ret = count < PAGE_SIZE? count : PAGE_SIZE;
  char* Tune  = (char *)alloc_page( GFP_KERNEL | __GFP_ZERO );
	if ( !Tune )
		return -ENOMEM;

  //
  // Copy buffer into kernel memory
  //
  if ( 0 != copy_from_user( Tune, buffer, count ) ) {
    ret = -EINVAL;
  } else {
    unsigned int NewReadAhead;
    UfsdVolumeTune vt;
    int Parsed = sscanf( Tune, "Ra=%u DirAge=%u JnlRam=%u", &NewReadAhead, &vt.DirAge, &vt.JnlRam );
    if ( Parsed < 1 ) {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("failed to parse tune buffer \"%s\"\n", Tune) );
      ret = -EINVAL;
    } else {
      sbi->ReadAheadBlocks = NewReadAhead;

      if ( Parsed >= 3 ) {
        //
        // Call UFSD library
        //
        LockUfsd( sbi );

        ret = UFSDAPI_SetVolumeTune( sbi->Ufsd, &vt );

        UnlockUfsd( sbi );
      } else {
        ret = 0;
      }
    }

    if ( 0 == ret ){
      ret = count; // Ok
      *ppos += count;
    } else {
      DebugTrace(0, UFSD_LEVEL_ERROR, ("write_tune failed: %x\n", (unsigned)ret ) );
      ret = -EINVAL;
    }
  }

  __free_page( (struct page*)Tune );
  return ret;
}

static const struct file_operations ufsd_proc_dev_tune_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_tune_open,
  .write    = ufsd_proc_dev_tune_write,
};


#ifdef UFSD_TRACE

///////////////////////////////////////////////////////////
// ufsd_proc_dev_trace_show
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_trace_show(
    IN struct seq_file* m,
    IN void*            o
    )
{
  const char* hint;
  switch( UFSD_TraceLevel ) {
  case UFSD_LEVEL_STR_ALL:  hint = "all"; break;
  case UFSD_LEVEL_STR_VFS:  hint = "vfs"; break;
  case UFSD_LEVEL_STR_LIB:  hint = "lib"; break;
  case UFSD_LEVEL_STR_MID:  hint = "mid"; break;
  default:
    seq_printf( m, "%lx\n", UFSD_TraceLevel );
    return 0;
  }
  seq_printf( m, "%s\n", hint );
  return 0;
}

static int ufsd_proc_dev_trace_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_trace_show, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_trace_write
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_trace_write(
    IN struct file* file,
    IN const char __user * buffer,
    IN size_t       count,
    IN OUT loff_t * ppos
    )
{
  //
  // Copy buffer into kernel memory
  //
  char kbuffer[16];
  size_t len = count;
  if ( len > sizeof(kbuffer)-1 )
    len = sizeof(kbuffer)-1;

  if ( 0 != copy_from_user( kbuffer, buffer, len ) )
    return -EINVAL;

  // Remove last '\n'
  while( len > 0 && '\n' == kbuffer[len-1] )
    len -= 1;

  // Set last zero
  kbuffer[len] = 0;

  ParseTraceLevel( kbuffer );
  *ppos += count;
  return count;
}


static const struct file_operations ufsd_proc_dev_trace_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_trace_open,
  .write    = ufsd_proc_dev_trace_write,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_log_show
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_log_show(
    IN struct seq_file* m,
    IN void*            o
    )
{
  seq_printf( m, "%s\n", ufsd_trace_file );
  return 0;
}

static int ufsd_proc_dev_log_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_log_show, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_log_write
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_log_write(
    IN struct file* file,
    IN const char __user * buffer,
    IN size_t       count,
    IN OUT loff_t*  ppos
    )
{
  //
  // Copy buffer into kernel memory
  //
  char kbuffer[sizeof(ufsd_trace_file)];
  size_t len = count;
  if ( len > sizeof(kbuffer)-1 )
    len = sizeof(kbuffer)-1;

  if ( 0 != copy_from_user( kbuffer, buffer, len ) )
    return -EINVAL;

  // Remove last '\n'
  while( len > 0 && '\n' == kbuffer[len-1] )
    len -= 1;

  // Set last zero
  kbuffer[len] = 0;

  if ( 0 != strcmp( ufsd_trace_file, kbuffer ) ) {
    memcpy( ufsd_trace_file, kbuffer, len + 1 );
    CloseTrace();
  }

  *ppos += count;
  return count;
}

static const struct file_operations ufsd_proc_dev_log_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_log_open,
  .write    = ufsd_proc_dev_log_write,
};

#endif // #ifdef UFSD_TRACE

typedef struct {
  const char   name[8];
  const struct file_operations* fops;
  unsigned int mode;
} ufsd_proc_entries;

static const ufsd_proc_entries ProcInfoEntries[] = {
  { "dirty",    &ufsd_proc_dev_dirty_fops   , S_IFREG | S_IRUGO },
  { "label",    &ufsd_proc_dev_label_fops   , S_IFREG | S_IRUGO | S_IWUGO },
  { "tune",     &ufsd_proc_dev_tune_fops    , S_IFREG | S_IRUGO | S_IWUGO },
  { "volinfo",  &ufsd_proc_dev_volinfo_fops , S_IFREG | S_IRUGO },
};

static const ufsd_proc_entries ProcRootEntries[] = {
  { "version",  &ufsd_proc_dev_version_fops , S_IFREG | S_IRUGO },
#ifdef UFSD_TRACE
  { "trace",    &ufsd_proc_dev_trace_fops   , S_IFREG | S_IRUGO | S_IWUGO },
  { "log",      &ufsd_proc_dev_log_fops     , S_IFREG | S_IRUGO | S_IWUGO },
#endif
};


///////////////////////////////////////////////////////////
// Proc_mkdir
//
// Helper function to reduce chaos
///////////////////////////////////////////////////////////
static struct proc_dir_entry*
Proc_mkdir( const char* name, struct proc_dir_entry* parent )
{
  struct proc_dir_entry* e = proc_mkdir( name, parent );
#if defined HAVE_STRUCT_PROC_DIR_ENTRY_OWNER && HAVE_STRUCT_PROC_DIR_ENTRY_OWNER
  if ( NULL != e )
    e->owner = THIS_MODULE;
#endif
  return e;
}


///////////////////////////////////////////////////////////
// CreateProcEntries
//
//
///////////////////////////////////////////////////////////
static const char*
CreateProcEntries(
    IN const ufsd_proc_entries* e,
    IN unsigned int             count,
    IN struct proc_dir_entry*   parent,
    IN void*                    data
    )
{
  for ( ; 0 != count--; e++ ) {
#if defined HAVE_DECL_PROC_CREATE_DATA && HAVE_DECL_PROC_CREATE_DATA
    if ( NULL == proc_create_data( e->name, e->mode, parent, e->fops, data ) )
      return e->name;
#else
    // 2.6.22 -
	  struct proc_dir_entry* de = create_proc_entry( e->name, e->mode, parent );
    if ( NULL == de )
      return e->name;
    de->data = data;
    de->proc_fops = e->fops;
#endif
  }
  return NULL;
}


///////////////////////////////////////////////////////////
// RemoveProcEntries
//
//
///////////////////////////////////////////////////////////
static void
RemoveProcEntries(
    IN const ufsd_proc_entries* e,
    IN unsigned int             count,
    IN struct proc_dir_entry*   parent
    )
{
  for ( ; 0 != count--; e++ )
    remove_proc_entry( e->name, parent );
}


///////////////////////////////////////////////////////////
// ufsd_proc_info_create
//
// creates /proc/fs/ufsd/<dev>
// Called from 'ufsd_read_super'
///////////////////////////////////////////////////////////
static void
ufsd_proc_info_create(
    IN struct super_block* sb
    )
{
  if ( NULL != proc_info_root ) {
    const char* dev   = UFSD_BdGetName( sb );
    struct proc_dir_entry* e = Proc_mkdir( dev, proc_info_root );
    const char* hint  = NULL == e? "" : CreateProcEntries( ProcInfoEntries, ARRAY_SIZE( ProcInfoEntries ), e, sb );
    if ( NULL != hint )
      printk( KERN_NOTICE QUOTED_UFSD_DEVICE": cannot create /proc/"PROC_FS_UFSD_NAME"/%s/%s", dev, hint );
    UFSD_SB( sb )->procdir = e;
  }
}


///////////////////////////////////////////////////////////
// ufsd_proc_info_delete
//
// deletes /proc/fs/ufsd/<dev>
// Called from 'ufsd_put_super'
///////////////////////////////////////////////////////////
static void
ufsd_proc_info_delete(
    IN struct super_block* sb
    )
{
  usuper* sbi = UFSD_SB( sb );

  if ( NULL != sbi->procdir )
    RemoveProcEntries( ProcInfoEntries, ARRAY_SIZE( ProcInfoEntries ), sbi->procdir );

  if ( NULL != proc_info_root )
    remove_proc_entry( UFSD_BdGetName( sb ), proc_info_root );
  sbi->procdir = NULL;
}


///////////////////////////////////////////////////////////
// ufsd_proc_create
//
// creates "/proc/fs/ufsd"
// Called from 'ufsd_init'
///////////////////////////////////////////////////////////
static void
ufsd_proc_create( void )
{
  struct proc_dir_entry* e = Proc_mkdir( PROC_FS_UFSD_NAME, NULL );
  const char* hint = NULL == e? "" : CreateProcEntries( ProcRootEntries, ARRAY_SIZE( ProcRootEntries), e, NULL );
  if ( NULL != hint )
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": cannot create /proc/"PROC_FS_UFSD_NAME"/%s\n", hint );
  proc_info_root = e;
}


///////////////////////////////////////////////////////////
// ufsd_proc_delete
//
// deletes "/proc/fs/ufsd"
// Called from 'ufsd_exit'
///////////////////////////////////////////////////////////
static void
ufsd_proc_delete( void )
{
  if ( NULL != proc_info_root ) {
    RemoveProcEntries( ProcRootEntries, ARRAY_SIZE( ProcRootEntries), proc_info_root );
    proc_info_root = NULL;
    remove_proc_entry( PROC_FS_UFSD_NAME, NULL );
  }
}

#else

  #define ufsd_proc_info_create( s )
  #define ufsd_proc_info_delete( s )
  #define ufsd_proc_create()
  #define ufsd_proc_delete()

#endif // #if defined CONFIG_PROC_FS


///////////////////////////////////////////////////////////
// ufsd_put_super
//
// super_operations::put_super
// Drop the volume handle.
///////////////////////////////////////////////////////////
static void
ufsd_put_super(
    IN struct super_block * sb
    )
{
  usuper* sbi = UFSD_SB( sb );
  DebugTrace(+1, Dbg, ("put_super: %p (%s)\n", sb, UFSD_BdGetName(sb)));

  //
  // Perform any delayed tasks
  //
  DoDelayedTasks( sbi );

#ifdef UFSD_USE_FLUSH_THREAD
  //
  // Stop flush thread
  //
  write_lock( &sbi->StateLock );
  sbi->ExitFlushThread = 1;

  while ( NULL != sbi->FlushTask ) {
    wake_up( &sbi->WaitExitFlush );
    write_unlock( &sbi->StateLock );
    wait_event( sbi->WaitDoneFlush, NULL == sbi->FlushTask );
    write_lock( &sbi->StateLock );
  }
  write_unlock( &sbi->StateLock );
#endif

  // Remove /proc/fs/ufsd/..
  ufsd_proc_info_delete( sb );

  UFSDAPI_VolumeFree( sbi->Ufsd );

#ifdef UFSD_HFS
  if ( NULL != sbi->TailBh ) {
    struct buffer_head* bh = sbi->TailBh;
    assert( 1 == atomic_read( &bh->b_count ) );
    __free_page( bh->b_page );
    __brelse( bh );
  }
#endif

  UFSD_unload_nls( &sbi->options );
  if ( NULL != sbi->rw_buffer )
    vfree( sbi->rw_buffer );

#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
  Mutex_destroy( &sbi->ApiMutex );
  Mutex_destroy( &sbi->NoCaseMutex );
#endif

#if !defined UFSD_TRACE_SILENT && defined UFSD_DEBUG
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Delayed clear %Zu\n", sbi->nDelClear ));
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Read %Zu, Written %Zu\n", sbi->nReadBlocks, sbi->nWrittenBlocks ));
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("ReadNa %Zu, WrittenNa %Zu\n", sbi->nReadBlocksNa, sbi->nWrittenBlocksNa ));
  assert( sbi->nPinBlocks == sbi->nUnpinBlocks );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Pinned %Zu, Unpinned %Zu\n", sbi->nPinBlocks, sbi->nUnpinBlocks ));
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Mapped: %Zu + %Zu - %Zu\n", sbi->nMappedBh, sbi->nMappedMem, sbi->nUnMapped ));
  assert( sbi->nMappedBh + sbi->nMappedMem == sbi->nUnMapped );
  if ( 0 != sbi->nCompareCalls )
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("ufsd_compare %Zu\n", (ssize_t)sbi->nCompareCalls ));
  if ( 0 != sbi->nHashCalls )
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("ufsd_name_hash %Zu\n", (ssize_t)sbi->nHashCalls ));

  DebugTrace(0, UFSD_LEVEL_ERROR, ("bdread        : %Zu, %u msec\n", sbi->bdread_cnt, jiffies_to_msecs( sbi->bdread_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("bdwrite       : %Zu, %u msec\n", sbi->bdwrite_cnt, jiffies_to_msecs( sbi->bdwrite_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("bdmap         : %Zu, %u msec\n", sbi->bdmap_cnt, jiffies_to_msecs( sbi->bdmap_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("bdsetdirty    : %Zu, %u msec\n", sbi->bdsetdirty_cnt, jiffies_to_msecs( sbi->bdsetdirty_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("get_block     : %Zu, %u msec\n", sbi->get_block_cnt, jiffies_to_msecs( sbi->get_block_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("write_begin   : %Zu, %u msec\n", sbi->write_begin_cnt, jiffies_to_msecs( sbi->write_begin_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("write_end     : %Zu, %u msec\n", sbi->write_end_cnt, jiffies_to_msecs( sbi->write_end_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("writepages    : %Zu, %u msec\n", sbi->writepages_cnt, jiffies_to_msecs( sbi->writepages_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("da_writepages : %Zu, %u msec\n", sbi->da_writepages_cnt, jiffies_to_msecs( sbi->da_writepages_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("da_write_begin: %Zu, %u msec\n", sbi->da_write_begin_cnt, jiffies_to_msecs( sbi->da_write_begin_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("da_write_end  : %Zu, %u msec\n", sbi->da_write_end_cnt, jiffies_to_msecs( sbi->da_write_end_ticks ) ) );
  DebugTrace(0, UFSD_LEVEL_ERROR, ("write_inode   : %Zu, %u msec\n", sbi->write_inode_cnt, jiffies_to_msecs( sbi->write_inode_ticks ) ) );
#endif //#if !defined UFSD_TRACE_SILENT && defined UFSD_DEBUG

#ifdef UFSD_USE_XATTR
  if ( NULL != sbi->Xbuffer )
    kfree( sbi->Xbuffer );
#endif

  UFSD_HeapFree( sbi );
  sb->s_fs_info = NULL;
  assert( NULL == UFSD_SB( sb ) );

  sync_blockdev( sb_dev(sb) );

  DebugTrace(-1, Dbg, ("put_super ->\n"));
}


///////////////////////////////////////////////////////////
// ufsd_write_inode
//
// super_operations::write_inode
///////////////////////////////////////////////////////////
static
#if defined HAVE_DECL_SO_WRITE_INODE_V1 && HAVE_DECL_SO_WRITE_INODE_V1
void ufsd_write_inode(
    IN struct inode* i,
    IN int sync __attribute__((__unused__)))
#elif defined HAVE_DECL_SO_WRITE_INODE_V2 && HAVE_DECL_SO_WRITE_INODE_V2
int ufsd_write_inode(
    IN struct inode* i,
    IN int sync __attribute__((__unused__)))
#elif defined HAVE_DECL_SO_WRITE_INODE_V3 && HAVE_DECL_SO_WRITE_INODE_V3
int ufsd_write_inode(
    IN struct inode* i,
    IN struct writeback_control *wbc __attribute__((__unused__)))
#else
#error "Unknown type ufsd_write_inode"
#endif
{
  unode* u    = UFSD_U( i );
  usuper* sbi = UFSD_SB( i->i_sb );
  int flushed = 0;

  DebugTrace(+1, Dbg, ("write_inode: r=%lx, h=%p\n", i->i_ino, u->ufile));
  ProfileEnter( sbi, write_inode );

  if ( i->i_state & (I_CLEAR | I_FREEING) ) {
    assert( !"try to flush clear node" );
    DebugTrace(0, Dbg, ("write_inode: try to flush clear node\n"));
  } else if ( NULL == u->ufile ){
    DebugTrace(0, Dbg, ("write_inode: no ufsd handle for this inode\n"));
  } else {

    if ( !TryLockUfsd( sbi ) ){

      //
      // Do not flush while write_begin/write_end in progress
      //
#if defined HAVE_STRUCT_INODE_I_MUTEX && HAVE_STRUCT_INODE_I_MUTEX
      if ( mutex_trylock( &i->i_mutex ) )
#endif
      {
        UINT64 isize = i_size_read(i);
        UFSD_FILE* file;
        spin_lock( &sbi->ddt_lock );
        file = u->ufile;
        spin_unlock( &sbi->ddt_lock );

        if ( u->sparse )
          u->mmu = isize;

        if ( NULL != file ) {
          UFSDAPI_FileFlush( sbi->Ufsd, file, isize, u->mmu,
                             TIMESPEC_SECONDS( &i->i_atime ) == u->atime? NULL : &i->i_atime,
                             TIMESPEC_SECONDS( &i->i_mtime ) == u->mtime? NULL : &i->i_mtime,
                             TIMESPEC_SECONDS( &i->i_ctime ) == u->ctime? NULL : &i->i_ctime,
                             &i->i_gid.val, &i->i_uid.val, u->set_mode? &i->i_mode : NULL );
          u->atime = TIMESPEC_SECONDS( &i->i_atime );
          u->mtime = TIMESPEC_SECONDS( &i->i_mtime );
          u->ctime = TIMESPEC_SECONDS( &i->i_ctime );
        }
        flushed = 1;
        u->set_mode = 0;
#if defined HAVE_STRUCT_INODE_I_MUTEX && HAVE_STRUCT_INODE_I_MUTEX
        mutex_unlock( &i->i_mutex );
#endif
      }
      UnlockUfsd( sbi );
    }

    if ( !flushed )
      mark_inode_dirty( i );
  }

  ProfileLeave( sbi, write_inode );

  DebugTrace(-1, Dbg, ("write_inode ->%s\n", flushed? "":" (d)"));
#if defined HAVE_DECL_SO_WRITE_INODE_V1 && HAVE_DECL_SO_WRITE_INODE_V1
  return;
#else
  return 0;
#endif
}


///////////////////////////////////////////////////////////
// ufsd_sync_volume
//
// super_operations::sync_fs
///////////////////////////////////////////////////////////
static int
ufsd_sync_volume(
    IN struct super_block * sb,
    IN int wait
    )
{
  usuper* sbi = UFSD_SB( sb );
  DebugTrace(+1, Dbg, ("sync_volume: %p (%s)%s\n", sb, UFSD_BdGetName(sb), wait? ",w":""));

#ifndef UFSD_USE_FLUSH_THREAD
  sb->s_dirt = 0;
#else
  sbi->bDirty = 0;
#endif

#ifdef UFSD_SMART_TRACE
  printk( "<4>ufsd: sync_volume:+\n" );
#endif

  if ( !TryLockUfsd( sbi ) ){

    UFSDAPI_VolumeFlush( sbi->Ufsd, wait );
    UnlockUfsd( sbi );

  } else {

    //
    // Do volume flush later
    //
    atomic_set( &sbi->VFlush, wait? 2 : 1 );
  }

#ifdef UFSD_SMART_TRACE
  printk( "<4>ufsd: sync_volume:-\n" );
#endif

  DebugTrace(-1, Dbg, ("sync_volume ->\n"));
  return 0;
}

#ifdef UFSD_USE_FLUSH_THREAD

#include <linux/freezer.h>
#include <linux/kthread.h>

///////////////////////////////////////////////////////////
// AddTimer
//
// Helper function to add timer UFSD_SMART_DIRTY_SEC after last dirty
///////////////////////////////////////////////////////////
static inline void
AddTimer(
    IN usuper* sbi
    )
{
  mod_timer( &sbi->FlushTimer, HZ + sbi->LastDirty + msecs_to_jiffies( UFSD_SMART_DIRTY_SEC * 1000 ) );
}


///////////////////////////////////////////////////////////
// flush_timer_fn
//
// Timer function
///////////////////////////////////////////////////////////
static void
flush_timer_fn(
    IN unsigned long data
    )
{
  usuper* sbi = (usuper*)data;

  if ( !sbi->bDirty ) {
    // Do not wake up flush thread
  } else {
    long dj = jiffies - sbi->LastDirty;
    if ( dj <= 0 || jiffies_to_msecs( dj ) < UFSD_SMART_DIRTY_SEC * 1000 ) {
      // Do not wake up flush thread
      // Sleep for another period
      AddTimer( sbi );
    } else if ( NULL != sbi->FlushTask ) {
      //
      // Volume is dirty and there are no writes last UFSD_SMART_DIRTY_SEC
      // Wake up flush thread
      //
      wake_up_process( sbi->FlushTask );
    }
  }
}


///////////////////////////////////////////////////////////
// ufsd_flush_thread
//
// 'dirty_writeback_interval'
///////////////////////////////////////////////////////////
static int
ufsd_flush_thread(
    IN void* arg
    )
{
  struct super_block* sb = arg;
  usuper* sbi = UFSD_SB( sb );
#ifdef UFSD_TRACE
  unsigned long j0, j1, j_a = 0, j_s = 0, cnt = 0;
#endif

  // Record that the flush thread is running
  sbi->FlushTask = current;

  //
  // Set up an interval timer which can be used to trigger a flush wakeup after the flush interval expires
  //
  setup_timer( &sbi->FlushTimer, flush_timer_fn, (unsigned long)sbi );

  wake_up( &sbi->WaitDoneFlush );

  //
  // And now, wait forever for flush wakeup events
  //
  write_lock( &sbi->StateLock );

  TRACE_ONLY( j0 = jiffies; )

  for ( ;; ) {
    if ( sbi->ExitFlushThread ) {
      write_unlock( &sbi->StateLock );
      del_timer_sync( &sbi->FlushTimer );
      sbi->FlushTask = NULL;
      wake_up( &sbi->WaitDoneFlush );
      DebugTrace(0, Dbg, ("flush_thread exiting: active %u, sleep %u, cycles %lu\n", jiffies_to_msecs( j_a ), jiffies_to_msecs( j_s ), cnt ));
      return 0;
    }

    if ( sbi->bDirty ) {
      long dj = jiffies - sbi->LastDirty;
      unsigned int dt;
      TRACE_ONLY( const char* hint;  )
      TRACE_ONLY( dt = 0; )

      DebugTrace(+1, Dbg, ("flush_thread: %p (%s)\n", sb, UFSD_BdGetName(sb)));

      if ( dj <= 0 || (dt = jiffies_to_msecs( dj )) < UFSD_SMART_DIRTY_SEC * 1000 ) {
        TRACE_ONLY( hint = "skip"; )
        AddTimer( sbi );
      } else {
        sbi->bDirty = 0;
#ifdef UFSD_SMART_TRACE
        printk( "<4>ufsd: flush_thread:+\n" );
#endif
        write_unlock( &sbi->StateLock );
        if ( !TryLockUfsd( sbi ) ){
          UFSDAPI_VolumeFlush( sbi->Ufsd, 1 );
          UnlockUfsd( sbi );
          TRACE_ONLY( hint = "flushed"; )
        } else {
          //
          // Do volume flush later
          //
          atomic_set( &sbi->VFlush, 1 );
          TRACE_ONLY( hint = "delay"; )
        }
        write_lock( &sbi->StateLock );
#ifdef UFSD_SMART_TRACE
        printk( "<4>ufsd: flush_thread:-\n" );
#endif
      }

      DebugTrace(-1, Dbg, ("flush_thread -> %s, %u\n", hint, dt));
    }

    wake_up( &sbi->WaitDoneFlush );

    TRACE_ONLY( cnt += 1; )
    TRACE_ONLY( j1 = jiffies; )
    TRACE_ONLY( j_a += j1 - j0; )
    TRACE_ONLY( j0 = j1; )

    if ( freezing( current ) ) {
      DebugTrace(0, Dbg, ("now suspending flush_thread\n" ));
      write_unlock( &sbi->StateLock );
#if defined HAVE_DECL_REFRIGERATOR && HAVE_DECL_REFRIGERATOR
      refrigerator();
#else
      try_to_freeze();
#endif
      write_lock( &sbi->StateLock );

    } else if ( !sbi->ExitFlushThread ) {

      DEFINE_WAIT( wait );
      prepare_to_wait( &sbi->WaitExitFlush, &wait, TASK_INTERRUPTIBLE );
      write_unlock( &sbi->StateLock );

      schedule();

      TRACE_ONLY( j1 = jiffies; )
      TRACE_ONLY( j_s += j1 - j0; )
      TRACE_ONLY( j0 = j1; )

      write_lock( &sbi->StateLock );
      finish_wait( &sbi->WaitExitFlush, &wait );
    }
  }
}

#else

///////////////////////////////////////////////////////////
// ufsd_write_super
//
// super_operations::write_super
///////////////////////////////////////////////////////////
static void
ufsd_write_super(
    IN struct super_block * sb
    )
{
  usuper* sbi = UFSD_SB( sb );
  long dj = jiffies - sbi->LastDirty;
  unsigned int dt;
  TRACE_ONLY( const char* hint;  )
  TRACE_ONLY( dt = 0; )
  DebugTrace(+1, Dbg, ("write_super: %p (%s)\n", sb, UFSD_BdGetName(sb)));

  if ( dj <= 0 || (dt = jiffies_to_msecs( dj )) < UFSD_SMART_DIRTY_SEC * 1000 ) {
    TRACE_ONLY( hint = "skip"; )
  } else {
    // Clear 's_dirt' to avoid next calls
    sb->s_dirt  = 0;
#ifdef UFSD_SMART_TRACE
    printk( "<4>ufsd: write_super:+\n" );
#endif
    if ( !TryLockUfsd( sbi ) ){

      UFSDAPI_VolumeFlush( sbi->Ufsd, 0 );
      UnlockUfsd( sbi );
      TRACE_ONLY( hint = "flushed"; )

    } else {

      //
      // Do volume flush later
      //
      atomic_set( &sbi->VFlush, 1 );
      TRACE_ONLY( hint = "delay"; )
    }
#ifdef UFSD_SMART_TRACE
    printk( "<4>ufsd: write_super:-\n" );
#endif
  }

  DebugTrace(-1, Dbg, ("write_super -> %s, %u\n", hint, dt));
}

#endif // #ifdef UFSD_USE_FLUSH_THREAD


///////////////////////////////////////////////////////////
// UFSD_OnSetDirty
//
// Callback function. Called when volume becomes dirty
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
UFSD_OnSetDirty(
    IN void* Arg
    )
{
  struct super_block* sb = Arg;
  usuper* sbi = UFSD_SB( sb );

  assert( !(sb->s_flags & MS_RDONLY) )

#ifdef UFSD_USE_FLUSH_THREAD
  write_lock( &sbi->StateLock );
  sbi->LastDirty = jiffies;
  if ( !sbi->bDirty ) {
#ifdef UFSD_SMART_TRACE
    printk( "<4>ufsd: UFSD_OnSetDirty()\n" );
#endif
    DebugTrace(0, Dbg, ("UFSD_OnSetDirty()\n" ));
    sbi->bDirty = 1;
  }
  AddTimer( sbi );
  write_unlock( &sbi->StateLock );
#else
#ifdef UFSD_SMART_TRACE
  if ( !sb->s_dirt )
    printk( "<4>ufsd: UFSD_OnSetDirty()\n" );
#endif
  sb->s_dirt      = 1;
  sbi->LastDirty  = jiffies;
#endif
}


#if defined HAVE_DECL_KSTATFS && HAVE_DECL_KSTATFS
#define kstatfs statfs
#endif

///////////////////////////////////////////////////////////
// ufsd_statfs
//
// super_operations::statfs
///////////////////////////////////////////////////////////
static int
ufsd_statfs(
#if defined HAVE_DECL_SO_STATFS_V1 && HAVE_DECL_SO_STATFS_V1
    IN  struct super_block* sb,
    OUT struct kstatfs*     buf
#elif defined HAVE_DECL_SO_STATFS_V2 && HAVE_DECL_SO_STATFS_V2
    IN struct dentry*       de,
    OUT struct kstatfs*     buf
#elif defined HAVE_DECL_SO_STATFS_V3 && HAVE_DECL_SO_STATFS_V3
    IN struct super_block*  sb,
    OUT struct statfs*      buf
#endif
    )
{
#if defined HAVE_DECL_SO_STATFS_V2 && HAVE_DECL_SO_STATFS_V2
  struct super_block* sb = de->d_sb;
#endif
  usuper* sbi = UFSD_SB( sb );
  struct UfsdVolumeInfo Info;
  UINT64 FreeBlocks;
  DebugTrace(+1, Dbg, ("statfs: %p (%s), %p\n", sb, UFSD_BdGetName(sb), buf));
  LockUfsd( sbi );

  UFSDAPI_QueryVolumeInfo( sbi->Ufsd, &Info, NULL, 0, &FreeBlocks );

#ifdef UFSD_DELAY_ALLOC
  assert( !sbi->options.delalloc || atomic_long_read( &sbi->FreeBlocks ) == FreeBlocks );
//  DebugTrace(0, Dbg, ("dirty blocks: %lx\n", atomic_long_read( &sbi->DirtyBlocks )));
  FreeBlocks -= atomic_long_read( &sbi->DirtyBlocks );
#endif

  UnlockUfsd( sbi );

  buf->f_type   = Info.FsSignature;
  buf->f_bsize  = Info.BytesPerBlock;
  buf->f_blocks = Info.TotalBlocks;
  buf->f_bfree  = FreeBlocks;
  buf->f_bavail = buf->f_bfree;
  buf->f_files  = 0;
  buf->f_ffree  = 0;
  buf->f_namelen= Info.NameLength;

  DebugTrace(-1, Dbg, ("statfs -> free=%llx\n", FreeBlocks));
  //TRACE_ONLY(show_buffers();)
#if defined UFSD_DEBUG_ALLOC & !defined UFSD_TRACE_SILENT
  TraceMemReport( 0 );
#endif
  return 0;
}

// Forward declaration
static int
ufsd_parse_options(
    char **options,
    mount_options *opts
    );


///////////////////////////////////////////////////////////
// ufsd_remount
//
// super_operations::remount_fs
///////////////////////////////////////////////////////////
static int
ufsd_remount(
    IN struct super_block* sb,
    IN int*   flags,
    IN char*  data
    )
{
  mount_options opts_saved;
  char*  options = data;
  int err = -EINVAL;
  int NeedParse = NULL != data && 0 != data[0];
  int Ro = *flags & MS_RDONLY;
  struct UfsdVolumeInfo Info;
  usuper* sbi = UFSD_SB( sb );
  C_ASSERT( sizeof(sbi->options) == sizeof(opts_saved) );
  const char* DevName = UFSD_BdGetName( sb );

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  DebugTrace(+1, Dbg, ("remount %s, %lx, options '%s'\n", DevName, sb->s_flags, NULL == options? "(null)" : options));

  if ( (sb->s_flags & MS_RDONLY) && !Ro && sbi->options.journal >= JOURNAL_STATUS_NEED_REPLAY ) {
    DebugTrace(0, Dbg, ("remount \"%s\": ro -> rw + jnl\n", DevName ));
    printk( KERN_WARNING QUOTED_UFSD_DEVICE ": Couldn't remount \"%s\" rw because journal is not replayed."
            " Please umount/remount instead\n", DevName );
    NeedParse = 0;
    goto Exit;
  }

  if ( NeedParse ) {

    // Save current options
    memcpy( &opts_saved, &sbi->options, sizeof(opts_saved) );

    // Parse options passed in command 'mount'
    memset( &sbi->options, 0, sizeof(opts_saved) );

    if ( !ufsd_parse_options( &options, &sbi->options ) ) {
      DebugTrace(0, Dbg, ("remount: failed to remount %s, bad options '%s'\n", DevName, options));
      goto Exit;
    }
  }

  *flags |= MS_NODIRATIME | (sbi->options.noatime? MS_NOATIME : 0);

  if ( !Ro
    && ( 0 != UFSDAPI_QueryVolumeInfo( sbi->Ufsd, &Info, NULL, 0, NULL )
      || 0 != Info.Dirty )
    && !sbi->options.force ) {
    //
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": volume is dirty and \"force\" flag is not set\n" );
    goto Exit;
  }

  err = UFSDAPI_VolumeReMount( sbi->Ufsd, Ro, &sbi->options );
  if ( 0 != err ) {
    DebugTrace(0, Dbg, ("remount: failed to remount %s, UFSDAPI_VolumeReMount failed %x\n", DevName, (unsigned)err ));
    err = -EINVAL;
    goto Exit;
  }

  if ( NeedParse ) {
    // unload original nls
    UFSD_unload_nls( &opts_saved );
  }

#if defined HAVE_STRUCT_SUPER_BLOCK_S_D_OP && HAVE_STRUCT_SUPER_BLOCK_S_D_OP
  sb->s_d_op = sbi->options.nocase? &ufsd_dop : NULL;
#endif

#if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
  if ( sbi->options.raKb )
    sb->s_bdi->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#else
  if ( sbi->options.raKb )
    sb->s_bdev->bd_inode_backing_dev_info->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#endif

  if ( Ro )
    sb->s_flags |= MS_RDONLY;
  else
    sb->s_flags &= ~MS_RDONLY;

  //
  // Save 'sync' flag
  //
  if ( FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    sbi->options.sync = 1;

Exit:

  if ( 0 != err && NeedParse ) {
    // unload new nls
    UFSD_unload_nls( &sbi->options );
    // Restore original options
    memcpy( &sbi->options, &opts_saved, sizeof(opts_saved) );
  }

  UnlockUfsd( sbi );

  if ( 0 == err ) {
    DebugTrace(-1, Dbg, ("remount -> ok\n"));
  } else {
    DebugTrace(-1, Dbg, ("remount failed: %d\n", err));
  }

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_evict_inode
//
// super_operations::evict_inode/clear_inode
///////////////////////////////////////////////////////////
static void
ufsd_evict_inode(
    IN struct inode* i
    )
{
  usuper* sbi = UFSD_SB( i->i_sb );
  unode* u    = UFSD_U( i );
  UFSD_FILE* file;
  TRACE_ONLY( int d = 0; )

  DebugTrace(+1, Dbg, ("evict_inode: r=%lx, h=%p, c=%u\n", i->i_ino, u->ufile, atomic_read(&i->i_count) ));

#if defined HAVE_STRUCT_SUPER_OPERATIONS_EVICT_INODE && HAVE_STRUCT_SUPER_OPERATIONS_EVICT_INODE
  if ( i->i_data.nrpages )
    truncate_inode_pages( &i->i_data, 0 );
#if defined HAVE_DECL_END_WRITEBACK && HAVE_DECL_END_WRITEBACK
  end_writeback( i );
#elif defined HAVE_DECL_CLEAR_INODE && HAVE_DECL_CLEAR_INODE
  //In kernel 3.5 end_writeback renamed to clear_inode
  clear_inode( i );
#else
#error "end_writeback or clear_inode not defined"
#endif
#else
  #define evict_inode clear_inode
#endif

  if ( NULL == sbi ){
    DebugTrace(0, Dbg, ("evict_inode: forgotten inode\n") );
  }
#ifndef UFSD_BIG_UNODE
  else if ( NULL == u ) {
    DebugTrace(0, Dbg, ("evict_inode: uninitialized inode\n") );
  }
#endif
  else if ( NULL == u->ufile ){
    ;
  } else if ( !TryLockUfsd( sbi ) ) {

    spin_lock( &sbi->ddt_lock );
    file = u->ufile;
    u->ufile = NULL;
    spin_unlock( &sbi->ddt_lock );

#ifndef UFSD_BIG_UNODE
    UFSD_HeapFree( u );
    UFSD_U(i) = NULL;
#endif

    UFSDAPI_FileClose( sbi->Ufsd, file );

    UnlockUfsd( sbi );

  } else {

    int is_dir = S_ISDIR( i->i_mode );

    spin_lock( &sbi->ddt_lock );
    file = u->ufile;
    u->ufile = NULL;
    spin_unlock( &sbi->ddt_lock );

    if ( NULL != file ) {
      delay_task* task = (delay_task*)kmalloc( sizeof(delay_task), GFP_NOFS );
      if ( NULL == task ){
        //
        // UFSD must correct close unclosed files in the end of the session
        //
      } else {

        //
        // Add this inode to internal list to clear later
        //
        task->file = file;
        spin_lock( &sbi->ddt_lock );
        if ( is_dir )
          list_add_tail( &task->list, &sbi->clear_list );
        else
          list_add( &task->list, &sbi->clear_list );
        spin_unlock( &sbi->ddt_lock );
        TRACE_ONLY( sbi->nDelClear += 1; )
        TRACE_ONLY(d = 1;)
#ifndef UFSD_BIG_UNODE
        UFSD_HeapFree( u );
        UFSD_U(i) = NULL;
#endif
      }
    }
  }

#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
  if ( ACL_NOT_CACHED != u->acl ) {
    ufsd_posix_acl_release( u->acl );
    u->acl = ACL_NOT_CACHED;
  }
  if ( ACL_NOT_CACHED != u->default_acl ) {
    ufsd_posix_acl_release( u->default_acl );
    u->default_acl  = ACL_NOT_CACHED;
  }
#endif

  DebugTrace(-1, Dbg, ("evict_inode ->%s\n", d? " (d)" : "") );
}


///////////////////////////////////////////////////////////
// ufsd_show_options
//
// super_operations::show_options
///////////////////////////////////////////////////////////
static int
ufsd_show_options(
    IN struct seq_file* seq,
#if defined HAVE_DECL_SO_SHOW_OPTIONS_V2 && HAVE_DECL_SO_SHOW_OPTIONS_V2
    IN struct dentry*   dnt
#else
    IN struct vfsmount* vfs
#endif
    )
{
#if defined HAVE_DECL_SO_SHOW_OPTIONS_V2 && HAVE_DECL_SO_SHOW_OPTIONS_V2
  usuper* sbi = UFSD_SB( dnt->d_sb );
#else
  usuper* sbi = UFSD_SB( vfs->mnt_sb );
#endif

  mount_options* opts = &sbi->options;
//  TRACE_ONLY( char* buf = seq->buf + seq->count; )

//  DebugTrace(+1, Dbg, ("show_options: %p\n", sbi));

#ifdef UFSD_USE_NLS
  {
    int cp;
    for ( cp = 0; cp < opts->nls_count; cp++ ) {
      struct nls_table* nls = opts->nls[cp];
      if ( NULL != nls )
        seq_printf( seq, ",nls=%s", nls->charset );
      else
        seq_printf( seq, ",nls=utf8" );
    }
  }
#endif

  if ( opts->uid )
    seq_printf( seq, ",uid=%d", opts->fs_uid );
  if ( opts->gid )
    seq_printf( seq, ",gid=%d", opts->fs_gid );
  if ( opts->fmask )
    seq_printf( seq, ",fmask=%o", (int)(unsigned short)~opts->fs_fmask );
  if ( opts->dmask )
    seq_printf( seq, ",dmask=%o", (int)(unsigned short)~opts->fs_dmask );
  if ( opts->clumpKb )
    seq_printf( seq, ",clump=%u", opts->clumpKb );
  if ( opts->showmeta )
    seq_printf( seq, ",showmeta" );
  if ( opts->sys_immutable )
    seq_printf( seq, ",sys_immutable" );
  if ( opts->nocase )
    seq_printf( seq, ",nocase" );
  if ( opts->noatime )
    seq_printf( seq, ",noatime" );
  if ( opts->bestcompr )
    seq_printf( seq, ",bestcompr" );
  if ( opts->sparse )
    seq_printf( seq, ",sparse" );
  if ( opts->force )
    seq_printf( seq, ",force" );
  if ( opts->nohidden )
    seq_printf( seq, ",nohidden" );
  if ( opts->user_xattr )
    seq_printf( seq, ",user_xattr" );
  if ( opts->acl )
    seq_printf( seq, ",acl" );
  if ( opts->nolazy )
    seq_printf( seq, ",nolazy" );
#ifdef UFSD_DELAY_ALLOC
  if ( opts->delalloc )
    seq_printf( seq, ",delalloc" );
#endif
  if ( opts->nojnl )
    seq_printf( seq, ",nojnl" );
  if ( opts->wb )
    seq_printf( seq, ",wb=1" );
  if ( opts->raKb ) {
    if ( 0 == (opts->raKb&0x3ff) )
      seq_printf( seq, ",ra=%uM", opts->raKb>>10 );
    else
      seq_printf( seq, ",ra=%u", opts->raKb );
  }

//  DebugTrace(-1, Dbg, ("show_options -> \"%s\"\n", buf));
  return 0;
}


//
// Volume operations
// super_block::s_op
//
STATIC_CONST struct super_operations ufsd_sops = {
#ifdef UFSD_BIG_UNODE
  .alloc_inode    = ufsd_alloc_inode,
  .destroy_inode  = ufsd_destroy_inode,
#endif
#if defined HAVE_STRUCT_SUPER_OPERATIONS_READ_INODE2 && HAVE_STRUCT_SUPER_OPERATIONS_READ_INODE2
  .read_inode2    = ufsd_read_inode2,
#endif
  .put_super      = ufsd_put_super,
  .statfs         = ufsd_statfs,
  .remount_fs     = ufsd_remount,
#ifndef UFSD_USE_FLUSH_THREAD
  .write_super    = ufsd_write_super,
#endif
  .sync_fs        = ufsd_sync_volume,
  .write_inode    = ufsd_write_inode,
  .evict_inode    = ufsd_evict_inode,
  .show_options   = ufsd_show_options,
};


#if defined HAVE_STRUCT_EXPORT_OPERATIONS && HAVE_STRUCT_EXPORT_OPERATIONS\
 && defined HAVE_STRUCT_SUPER_BLOCK_S_EXPORT_OP && HAVE_STRUCT_SUPER_BLOCK_S_EXPORT_OP

#if defined HAVE_DECL_D_ALLOC_ANON && HAVE_DECL_D_ALLOC_ANON
  #define d_alloc_ufsd  d_alloc_anon
#elif defined HAVE_DECL_D_OBTAIN_ALIAS && HAVE_DECL_D_OBTAIN_ALIAS
  #define d_alloc_ufsd  d_obtain_alias
#else
  #warning "Disabling export operations due to unknown version of d_alloc_anon/d_obtain_alias"
#endif

#endif

#ifdef d_alloc_ufsd

///////////////////////////////////////////////////////////
// ufsd_get_name
//
// dentry - the directory in which to find a name
// name   - a pointer to a %NAME_MAX+1 char buffer to store the name
// child  - the dentry for the child directory.
//
//
// Get the name of child entry by its ino
// export_operations::get_name
///////////////////////////////////////////////////////////
static int
ufsd_get_name(
    IN struct dentry* de,
    OUT char*         name,
    IN struct dentry* ch
    )
{
  int err;
  struct inode* i_p   = de->d_inode;
  struct inode* i_ch  = ch->d_inode;
  usuper* sbi = UFSD_SB( i_ch->i_sb );

  DebugTrace(+1, Dbg, ("get_name: r=%lx=%p('%.*s'), r=%lx=%p('%.*s')\n",
              i_p->i_ino, de, (int)de->d_name.len, de->d_name.name,
              i_ch->i_ino, ch, (int)ch->d_name.len, ch->d_name.name ));

  //
  // Reset returned value
  //
  name[0] = 0;

  //
  // Lock UFSD
  //
  LockUfsd( sbi );

  err = 0 == LazyOpen( sbi, i_ch )
     && 0 == UFSDAPI_FileGetName( sbi->Ufsd, UFSD_FH(i_ch), i_p->i_ino, name, NAME_MAX )
     ? 0
     : -ENOENT;

  UnlockUfsd( sbi );

  DebugTrace(-1, Dbg, ("get_name -> %d (%s)\n", err, name ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_get_parent
//
// export_operations::get_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_get_parent(
    IN struct dentry* ch
    )
{
  ufsd_iget4_param param;
  struct inode* i_ch  = ch->d_inode;
  usuper* sbi         = UFSD_SB( i_ch->i_sb );
  struct inode* i     = NULL;
  struct dentry* de;
  int err;

  DebugTrace(+1, Dbg, ("get_parent: r=%lx,%p('%.*s')\n", i_ch->i_ino, ch, (int)ch->d_name.len, ch->d_name.name));

  param.subdir_count = 0;

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  if ( 0 == LazyOpen( sbi, i_ch )
    && 0 == UFSDAPI_FileGetParent( sbi->Ufsd, UFSD_FH(i_ch), &param.fh, &param.Info ) ) {

    assert( NULL != param.fh );

    i = iget4( i_ch->i_sb, param.Info.Id, NULL, &param );

    if ( NULL == i ) {
      err = -ENOMEM;
      DebugTrace(0, Dbg, ("get_parent: -> No memory for new inode\n" ));
    } else {
      assert( NULL != UFSD_FH(i) );
      // OK
      err = 0;
    }

    if ( NULL != param.fh ){
      // UFSD handle was not used. Close it
      UFSDAPI_FileClose( sbi->Ufsd, param.fh );
      param.fh = NULL;
      if ( 0 == err ){
        DebugTrace(0, Dbg, ("get_parent: r=%lx already loaded\n", i->i_ino ));
      }
    }
  } else {
    //
    // No parent for given inode
    //
    err = -ENOENT;
  }

  UnlockUfsd( sbi );

  if ( 0 != err ){
    DebugTrace(-1, Dbg, ("get_parent -> error %d\n", err ));
    return ERR_PTR(err);
  }

  DebugTrace(0, Dbg, ("get_parent -> OK, r=%lx h=%p id=%Zx l=%x\n",
                      i->i_ino, UFSD_FH(i), param.Info.Id, i->i_nlink));

  // Finally get a dentry for the parent directory and return it.
  de = d_alloc_ufsd( i );

  if ( unlikely( IS_ERR( de ) ) ) {
    iput( i );
    DebugTrace(-1, Dbg, ("get_parent: -> No memory for dentry\n" ));
    return ERR_PTR(-ENOMEM);
  }

  DebugTrace(-1, Dbg, ("get_parent -> %p('%.*s'))\n", de, (int)de->d_name.len, de->d_name.name));
  return de;
}



#ifdef UFSD_EXFAT
///////////////////////////////////////////////////////////
// ufsd_encode_fh
//
// stores in the file handle fragment 'fh' (using at most 'max_len' bytes)
// information that can be used by 'decode_fh' to recover the file refered
// to by the 'struct dentry* de'
//
// export_operations::encode_fh
///////////////////////////////////////////////////////////
static int
ufsd_encode_fh(
#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
    IN struct dentry* de,
#elif defined HAVE_DECL_ENCODE_FH_V2 && HAVE_DECL_ENCODE_FH_V2
    IN struct inode*  i,
#else
#error Unknown ufsd_encode_fh
#endif
    IN __u32*         fh,
    IN OUT int*       max_len,
#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
    IN int            connectable
#elif defined HAVE_DECL_ENCODE_FH_V2 && HAVE_DECL_ENCODE_FH_V2
    IN struct inode * connectable
#endif
    )
{
  int type;
#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
  struct inode* i = de->d_inode;
#endif
  usuper* sbi     = UFSD_SB( i->i_sb );
  UNREFERENCED_PARAMETER( connectable ); // Always assumed to be true

#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
  DebugTrace(+1, Dbg, ("encode_fh: r=%lx, %p('%.*s'), %x\n",
              i->i_ino, de, (int)de->d_name.len, de->d_name.name, *max_len ));
#else
  DebugTrace(+1, Dbg, ("encode_fh: r=%lx, %x\n",
              i->i_ino, *max_len ));
#endif

  LockUfsd( sbi );

  if ( 0 != LazyOpen( sbi, i ) )
    type = -ENOENT;
  else {
    type = UFSDAPI_EncodeFH( sbi->Ufsd, UFSD_FH(i), fh, max_len );
    if ( type < 0 )
      type = 255; // no room
  }

  UnlockUfsd( sbi );

  DebugTrace(-1, Dbg, ("encode_fh -> %d,%d\n", type, *max_len) );

  return type;
}


///////////////////////////////////////////////////////////
// ufsd_decode_fh
//
// Helper function for export (inverse function to ufsd_encode_fh)
///////////////////////////////////////////////////////////
static struct inode*
ufsd_decode_fh(
    IN struct super_block* sb,
    IN const void*  fh,
    IN unsigned     fh_len,
    IN const int*   fh_type,
    IN int          parent
    )
{
  int err;
  ufsd_iget4_param param;
  struct inode* i = NULL;
  usuper* sbi     = UFSD_SB( sb );

  DebugTrace(+1, Dbg, ("decode_fh: sb=%p %d,%d,%d\n", sb, NULL == fh_type? 0 : *fh_type, fh_len, parent));

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  if ( 0 != UFSDAPI_DecodeFH( sbi->Ufsd, fh, fh_len, fh_type, parent, &param.fh, &param.Info ) )
    err = -ENOENT;
  else {
    err = 0;

    i = iget4( sb, param.Info.Id, NULL, &param );

    if ( NULL == i ) {
      err = -ENOMEM;
      DebugTrace(0, Dbg, ("decode_fh: -> No memory for new inode\n" ));
    }

    if ( NULL != param.fh ){
      // UFSD handle was not used. Close it
      UFSDAPI_FileClose( sbi->Ufsd, param.fh );
      if ( 0 == err ){
        DebugTrace(0, Dbg, ("decode_fh: i=%p,r=%lx already loaded\n", i, i->i_ino ));
      }
    }
  }

  UnlockUfsd( sbi );

  if ( 0 != err ) {
    DebugTrace(-1, Dbg, ("decode_fh -> %d\n", err ));
    return ERR_PTR(err);
  }

  DebugTrace(-1, Dbg, ("decode_fh -> r=%lx h=%p r=%lx l=%x m=%o\n", i->i_ino, UFSD_FH(i), i->i_ino, i->i_nlink, i->i_mode ));

  return i;
}


///////////////////////////////////////////////////////////
// ufsd_encode_get_dentry
//
// encode_export_operations::get_dentry
///////////////////////////////////////////////////////////
#if defined HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY
static struct dentry*
ufsd_encode_get_dentry(
    IN struct super_block* sb,
    IN void* fh
    )
{
  struct dentry* de;
  struct inode* i;

  DebugTrace(+1, Dbg, ("get_dentry: sb=%p\n", sb ));

  i   = ufsd_decode_fh( sb, fh, 2, NULL, 0 );
  de  = IS_ERR(i)? (struct dentry*)i : d_alloc_ufsd( i );

  DebugTrace(-1, Dbg, ("get_dentry -> %p\n", de ));

  return de;
}
#endif // #if defined HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY


#if defined HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY

///////////////////////////////////////////////////////////
// ufsd_encode_fh_to_dentry
//
// encode_export_operations::fh_to_dentry
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_encode_fh_to_dentry(
    IN struct super_block* sb,
    IN struct fid* fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry* de;
  struct inode* i;

  DebugTrace(+1, Dbg, ("fh_to_dentry: sb=%p,r=%x,gen=%x\n", sb, fid->i32.ino, fid->i32.gen ));

  i   = ufsd_decode_fh( sb, fid, fh_len, &fh_type, 0 );
  de  = IS_ERR(i)? (struct dentry*)i : d_alloc_ufsd( i );

  DebugTrace(-1, Dbg, ("fh_to_dentry -> %p\n", de ));

  return de;
}


///////////////////////////////////////////////////////////
// ufsd_encode_fh_to_parent
//
// encode_export_operations::fh_to_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_encode_fh_to_parent(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry* de;
  struct inode* i;

  DebugTrace(+1, Dbg, ("fh_to_parent: sb=%p,r=%x,gen=%x\n", sb, fid->i32.parent_ino, fh_len > 3 ? fid->i32.parent_gen : 0 ));

  i  = ufsd_decode_fh( sb, fid, fh_len, &fh_type, 1 );
  de = IS_ERR(i)? (struct dentry*)i : d_alloc_ufsd( i );

  DebugTrace(-1, Dbg, ("fh_to_parent -> %p\n", de ));

  return de;
}

#endif // #if defined HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY


//
// NFS operations.
// super_block::s_export_op
//
STATIC_CONST struct export_operations ufsd_encode_export_op = {
  .encode_fh  = ufsd_encode_fh,
  .get_name   = ufsd_get_name,
  .get_parent = ufsd_get_parent,
#if defined HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY
  .get_dentry = ufsd_encode_get_dentry,
#endif
#if defined HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY
  .fh_to_dentry = ufsd_encode_fh_to_dentry,
  .fh_to_parent = ufsd_encode_fh_to_parent,
#endif
};

#endif // #ifdef UFSD_EXFAT


///////////////////////////////////////////////////////////
// ufsd_nfs_get_inode
//
// Helper function for export
///////////////////////////////////////////////////////////
static struct inode*
ufsd_nfs_get_inode(
    IN struct super_block* sb,
    IN u64 ino,
    IN u32 gen
    )
{
  int err;
  ufsd_iget4_param param;
  struct inode* i = NULL;
  usuper* sbi     = UFSD_SB( sb );

  DebugTrace(+1, Dbg, ("nfs_get_inode: sb=%p,r=%x,gen=%x\n", sb, (unsigned)ino, gen ));

  //
  // Call UFSD library
  //
  LockUfsd( sbi );

  err = UFSDAPI_FileOpenById( sbi->Ufsd, ino, &param.fh, &param.Info );

  if ( 0 == err ) {
    i = iget4( sb, param.Info.Id, NULL, &param );

    if ( NULL == i ) {
      err = -ENOMEM;
      DebugTrace(0, Dbg, ("nfs_get_inode: -> No memory for new inode\n" ));
    } else if ( gen && i->i_generation != gen ) {
      // we didn't find the right inode...
      DebugTrace(0, Dbg, ("nfs_get_inode: -> invalid generation\n" ));
      iput( i );
      err = -ESTALE;
    }

    if ( NULL != param.fh ){
      // UFSD handle was not used. Close it
      UFSDAPI_FileClose( sbi->Ufsd, param.fh );
      if ( 0 == err ){
        DebugTrace(0, Dbg, ("nfs_get_inode: r=%lx already loaded\n", i->i_ino ));
      }
    }
  }

  UnlockUfsd( sbi );

  if ( 0 != err ) {
    DebugTrace(-1, Dbg, ("nfs_get_inode: -> error %d\n", err ));
    return ERR_PTR(err);
  }

  DebugTrace(-1, Dbg, ("nfs_get_inode -> r=%lx\n", i->i_ino ));

  return i;
}


///////////////////////////////////////////////////////////
// ufsd_get_dentry
//
// export_operations::get_dentry
///////////////////////////////////////////////////////////
#if defined HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY
static struct dentry*
ufsd_get_dentry(
    IN struct super_block* sb,
    IN void* fh
    )
{
  struct dentry* de;
  struct inode* i;
  unsigned int ino  = ((unsigned int*)fh)[0];
  unsigned int gen  = ((unsigned int*)fh)[1];

  DebugTrace(+1, Dbg, ("get_dentry: sb=%p,r=%x,gen=%x\n", sb, ino, gen ));

  i  = ufsd_nfs_get_inode( sb, ino, gen );
  de = IS_ERR(i)? (struct dentry*)i : d_alloc_ufsd( i );

  DebugTrace(-1, Dbg, ("get_dentry -> %p\n", de ));

  return de;
}
#endif // #if defined HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY


#if defined HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY

///////////////////////////////////////////////////////////
// ufsd_fh_to_dentry
//
// export_operations::fh_to_dentry
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_fh_to_dentry(
    IN struct super_block* sb,
    IN struct fid* fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry* de = NULL;

  DebugTrace(+1, Dbg, ("fh_to_dentry: sb=%p,r=%x,gen=%x\n", sb, fid->i32.ino, fid->i32.gen ));

  if ( fh_len >= 2 && ( FILEID_INO32_GEN == fh_type || FILEID_INO32_GEN_PARENT == fh_type ) )
  {
    struct inode* i = ufsd_nfs_get_inode( sb, fid->i32.ino, fid->i32.gen );
    de = IS_ERR(i)? (struct dentry*)i : d_alloc_ufsd( i );
  }

  DebugTrace(-1, Dbg, ("fh_to_dentry -> %p\n", de ));

  return de;
}


///////////////////////////////////////////////////////////
// ufsd_fh_to_parent
//
// export_operations::fh_to_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_fh_to_parent(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry* de = NULL;

  DebugTrace(+1, Dbg, ("fh_to_parent: sb=%p,r=%x,gen=%x\n", sb, fid->i32.parent_ino, fh_len > 3 ? fid->i32.parent_gen : 0 ));

  if ( fh_len > 2 && FILEID_INO32_GEN_PARENT == fh_type )
  {
    struct inode* i = ufsd_nfs_get_inode( sb, fid->i32.parent_ino, fh_len > 3 ? fid->i32.parent_gen : 0 );
    de = IS_ERR( i )? (struct dentry*)i : d_alloc_ufsd( i );
  }

  DebugTrace(-1, Dbg, ("fh_to_parent -> %p\n", de ));

  return de;
}

#endif // #if defined HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY


//
// NFS operations.
// super_block::s_export_op
//
STATIC_CONST struct export_operations ufsd_export_op = {
  .get_name   = ufsd_get_name,
  .get_parent = ufsd_get_parent,
#if defined HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_GET_DENTRY
  .get_dentry = ufsd_get_dentry,
#endif
#if defined HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY && HAVE_STRUCT_EXPORT_OPERATIONS_FH_TO_DENTRY
  .fh_to_dentry = ufsd_fh_to_dentry,
  .fh_to_parent = ufsd_fh_to_parent,
#endif
};

#endif // #ifdef d_alloc_ufsd


#ifdef UFSD_USE_NLS
///////////////////////////////////////////////////////////
// ufsd_add_nls
//
//
///////////////////////////////////////////////////////////
static int
ufsd_add_nls(
    IN OUT mount_options* opts,
    IN struct nls_table*  nls
    )
{
  int cp;
  if ( NULL == nls )
    return -1; // error

  for ( cp = 0; cp < opts->nls_count; cp++ ) {
    if ( 0 == strcmp( opts->nls[cp]->charset, nls->charset ) )
      return 0;
  }

  if ( opts->nls_count >= sizeof(opts->nls)/sizeof(opts->nls[0]) )
    return -1; // error

  opts->nls[opts->nls_count] = nls;
  opts->nls_count += 1;
  return 0; // ok
}
#endif


static const char s_Options[][16] = {
  "nocase",           // 0
  "uid",              // 1
  "gid",              // 2
  "umask",            // 3
  "fmask",            // 4
  "dmask",            // 5
  "trace",            // 6
  "log",              // 7
  "sys_immutable",    // 8
  "quiet",            // 9
  "noatime",          // 10
  "bestcompr",        // 11
  "showmeta",         // 12
  "nobuf",            // 13
  "sparse",           // 14
  "codepage",         // 15
  "nls",              // 16
  "iocharset",        // 17
  "force",            // 18
  "nohidden",         // 19
  "clump",            // 20
  "bias",             // 21
  "user_xattr",       // 22
  "acl",              // 23
  "chkcnv",           // 24
  "cycle",            // 25
  "delim",            // 26
  "nolazy",           // 27
  "delalloc",         // 28
  "nojnl",            // 29
  "wb",               // 30
  "ra",               // 31
};


///////////////////////////////////////////////////////////
// ufsd_parse_options
//
// Parse options.
// Helper function for read_super
// Returns 0 if error
///////////////////////////////////////////////////////////
static int
ufsd_parse_options(
    IN OUT char **options,
    OUT mount_options *opts
    )
{
  char *t,*v,*delim,**ret_opt=options;
  int i;
  char c;
  unsigned long tmp;

#ifdef UFSD_USE_NLS
  char nls_name[50];
  struct nls_table* nls;
  int cp;
  assert( 0 == opts->nls_count );
#endif

  assert( NULL != current->fs );

  opts->fs_uid   = current_uid().val;
  opts->fs_gid   = current_gid().val;

  opts->fs_fmask = opts->fs_dmask = NULL == current->fs
    ? -1
#if defined HAVE_DECL_CURRENT_UMASK && HAVE_DECL_CURRENT_UMASK
    : ~(current_umask());
#else
    : ~(current->fs->umask);
#endif
  if ( NULL == options || NULL == options[0] || 0 == options[0][0] )
    goto Ok;

  while ( NULL != ( t = strsep( options, "," ) ) ) {

    DebugTrace(0, Dbg, (" parse_options: %s\n", t));

    // Save current pointer to "=" delimiter
    // It will be used to restore current option
    // to print in correct form in log message
    v = delim = strchr( t, '=' );
    if ( NULL != v )
      *v++ = 0;

    for ( i = 0; i < sizeof(s_Options)/sizeof(s_Options[0]); i++ ) {
      if ( 0 == strcmp( t, s_Options[i] ) )
        break;
    }

    switch( i ) {
      case 0:   // "nocase"
      case 22:  // "user_xattr"
      case 23:  // "acl"
      case 27:  // "nolazy"
      case 28:  // "delalloc"
      case 29:  // "nojnl"
      case 30:  // "wb="
        // Support both forms: 'nocase' and 'nocase=0/1'
        if ( NULL == v || 0 == v[0] )
          c = 1;  // parse short form "nocase"
        else if ( 0 == v[1] && '0' <= v[0] && v[0] <= '9' )
          c = (char)(v[0] - '0'); // parse wide form "nocase=X", where X=0,1,..,9
        else
          goto Err;
        switch( i ) {
          case 0:   opts->nocase = c; break;
          case 22:  opts->user_xattr = c; break;
          case 23:  opts->acl = c; break;
          case 27:  opts->nolazy = c; break;
          case 28:  opts->delalloc = c; break;
          case 29:  opts->nojnl = c; break;
          case 30:  opts->wb = c; break;
        }
        break;
      case 1:   // "uid"
      case 2:   // "gid"
      case 21:  // "bias"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = simple_strtoul( v, &v, 0 );
        if ( 0 != v[0] ) goto Err;
        switch( i ) {
        case 1: opts->fs_uid = tmp; opts->uid = 1; break;
        case 2: opts->fs_gid = tmp; opts->gid = 1; break;
        case 21: opts->bias = tmp; break;
        }
        break;
      case 3: // "umask"
      case 4: // "fmask"
      case 5: // "dmask"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = ~simple_strtoul( v, &v, 8 );
        if ( 0 != v[0] ) goto Err;
        switch( i ) {
        case 3: opts->fs_fmask = opts->fs_dmask = tmp; opts->fmask = opts->dmask = 1; break;
        case 4: opts->fs_fmask = tmp; opts->fmask = 1; break;
        case 5: opts->fs_dmask = tmp; opts->dmask = 1; break;
        }
        break;
      case 20:  // "clump"
      case 31:  // "ra"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = simple_strtoul( v, &v, 0 );
        if ( 0 == v[0] || 'K' == v[0] )
          ;
        else if ( 'M' == *v )
          tmp *= 1024;
        else
          goto Err;
        switch( i ) {
        case 20: opts->clumpKb = tmp; break;
        case 31: opts->raKb = tmp; break;
        }
        break;
#ifdef UFSD_TRACE
      case 6: // "trace"
        ParseTraceLevel( v );
        break;
      case 7: // "log"
        if ( NULL == v ) goto Err;
        strncpy( ufsd_trace_file, v, sizeof(ufsd_trace_file) );
        ufsd_trace_file[sizeof(ufsd_trace_file)-1] = 0;
        DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("%s", UFSDAPI_LibraryVersion( NULL ) ) );
        DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("%s%s\n", s_FileVer, s_DriverVer ) );
        DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("Module address %p\n", MODULE_BASE_ADDRESS ));
        DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("Kernel version %d.%d.%d\n", LINUX_VERSION_CODE>>16,
                                            (LINUX_VERSION_CODE>>8)&0xFF, LINUX_VERSION_CODE&0xFF ));
        DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("sizeof(inode)=%u\n", (unsigned)sizeof(struct inode) ) );
        break;
      case 25:  // "cycle"
        // Support both forms: 'cycle' and 'cycle=256'
        if ( NULL == v || 0 == v[0] )
          tmp = 1;
        else {
          tmp = simple_strtoul( v, &v, 0 );
          if ( 'K' == *v )
            tmp *= 1024;
          else if ( 'M' == *v )
            tmp *= 1024*1024;
        }
        UFSD_CycleMB = (tmp + 1024*1024 - 1) >> 20;
        break;
#else
      case 6:   // trace
      case 7:   // log
      case 25:  // cycle
        break;
#endif
      case 8: // "sys_immutable"
        if ( NULL != v ) goto Err;
        opts->sys_immutable = 1;
        break;
      case 9: // "quiet"
        break;
      case 10: // "noatime"
        if ( NULL != v ) goto Err;
        opts->noatime = 1;
        break;
      case 11: // "bestcompr"
        if ( NULL != v ) goto Err;
        opts->bestcompr = 1;
        break;
      case 12: // "showmeta"
        if ( NULL != v ) goto Err;
        opts->showmeta = 1;
        break;
      case 13: // "nobuf"
        break;
      case 14: // "sparse"
        if ( NULL != v ) goto Err;
        opts->sparse = 1;
        break;
#ifdef UFSD_USE_NLS
      case 15: // "codepage"
        if ( NULL == v || 0 == v[0] ) goto Err;
        sprintf( nls_name, "cp%d", (int)simple_strtoul( v, &v, 0 ) );
        if ( 0 != v[0] ) goto Err;
        v = nls_name;
        // no break here!!
      case 16: // "nls"
      case 17: // "iocharset"
        // Add this nls into array of codepages
        if ( NULL == v || 0 == v[0] || ufsd_add_nls( opts, load_nls(v) ) )
          goto Err;
#else
      case 15: // "codepage"
      case 16: // "nls"
      case 17: // "iocharset"
        // Ignore any nls related options
#endif
        break;
      case 18: // "force"
        if ( NULL != v ) goto Err;
        opts->force = 1;
        break;
      case 19: // "nohidden"
        if ( NULL != v ) goto Err;
        opts->nohidden = 1;
        break;
      case 24: // "chkcnv"
        if ( NULL != v ) goto Err;
        opts->chkcnv = 1;
        break;
      case 26:  // "delim=':'
        if ( NULL == v || 0 == v[0] )
          opts->delim = 0;
        else if ( 0 == v[1] )
          opts->delim = v[0];
        else
          goto Err;
        DebugTrace(0, Dbg, ("delim=%c (0x%x)\n", opts->delim, (unsigned)opts->delim ));
        break;
      default:
Err:
        // Return error options
        *ret_opt = t;
    }

    // Restore options string
    if ( NULL != delim )
      delim[0] = '=';

    // Restore full string
    if ( NULL != *options )
      (*options)[-1] = ',';

    if ( *ret_opt == t )
      return 0; // error
  }

Ok:
#ifdef UFSD_USE_NLS
  //
  // Load default nls if no nls related options
  //
  if ( 0 == opts->nls_count ){
    struct nls_table* nls_def = load_nls_default();
    if ( NULL != nls_def && 0 != memcmp( nls_def->charset, "utf8", sizeof("utf8") ) ) {
#ifndef UFSD_TRACE_SILENT
      DebugTrace(0, Dbg, ("default nls %s\n", nls_def->charset ));
//      printk( KERN_NOTICE QUOTED_UFSD_DEVICE": default nls %s\n", nls_def->charset );
#endif
      ufsd_add_nls( opts, nls_def );
    }
  } else {
    //
    // Remove kernel utf8 and use builtin utf8
    //
    for ( cp = 0; cp < opts->nls_count; cp++ ) {
      nls = opts->nls[cp];
      if ( 0 == memcmp( nls->charset, "utf8", sizeof("utf8") ) ) {
#ifndef UFSD_TRACE_SILENT
        DebugTrace(0, Dbg, ("unload kernel utf8\n"));
//        printk( KERN_NOTICE QUOTED_UFSD_DEVICE": use builtin utf8 instead of kernel utf8\n" );
#endif
        unload_nls( nls );
        opts->nls[cp] = NULL;
      } else {
#ifndef UFSD_TRACE_SILENT
        DebugTrace(0, Dbg, ("loaded nls %s\n", nls->charset ));
        printk( KERN_NOTICE QUOTED_UFSD_DEVICE": loaded nls %s\n", nls->charset );
#endif
      }
    }
  }
#endif

  //
  // If no nls then use builtin utf8
  //
  if ( 0 == opts->nls_count ) {
#ifndef UFSD_TRACE_SILENT
    DebugTrace(0, Dbg, ("use builtin utf8\n" ));
//    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": use builtin utf8\n" );
#endif
    opts->nls_count = 1;
    opts->nls[0]    = NULL;
  }

#ifndef UFSD_USE_XATTR
  opts->acl = opts->user_xattr = 0;
#endif

#ifndef UFSD_TRACE_SILENT
  if ( opts->delalloc ) {
#ifndef UFSD_DELAY_ALLOC
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore delalloc 'cause not supported\n" );
    opts->delalloc = 0;
#elif !defined Writeback_inodes_sb_if_idle
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": delalloc may work incorrect due to old kernel\n" );
    DebugTrace(0, UFSD_LEVEL_ALWAYS, (" delalloc may work incorrect due to old kernel\n"));
#endif
  }
#endif

  if ( opts->wb ) {
#ifdef Writeback_inodes_sb_if_idle
    if ( opts->delalloc ) {
      printk( KERN_ERR QUOTED_UFSD_DEVICE": \"delalloc\" and \"wb=1\" are not compatible\n" );
      return 0; // error
    }
#else
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore \"wb=1\" 'cause not supported\n" );
    opts->wb = 0;
#endif
  }

#if !(defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI)
  if ( opts->raKb ){
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore \"ra\" option 'cause not supported\n" );
    opts->raKb = 0;
  }
#endif

  return 1; // ok
}


///////////////////////////////////////////////////////////
// ufsd_read_super
//
// This routine is a callback used to recognize and
// initialize superblock using this filesystem driver.
//
// sb - Superblock structure. On entry sb->s_dev is set to device,
//     sb->s_flags contains requested mount mode.
//     On exit this structure should have initialized root directory
//     inode and superblock callbacks.
//
// data - mount options in a string form.
//
// silent - non-zero if no messages should be displayed.
//
// Return: struct super_block* - 'sb' on success, NULL on failure.
//
///////////////////////////////////////////////////////////
static int
ufsd_read_super(
    IN OUT struct super_block* sb,
    IN void*  data,
    IN int    silent
    )
{
  UFSD_VOLUME* Volume = NULL;
  int err = -EINVAL; // default error
  UfsdVolumeInfo  Info;
  usuper* sbi = NULL;
  struct inode* i = NULL;
  char* options = (char*)data;
  ufsd_iget4_param param;
  struct sysinfo  SysInfo;
  const char* DevName;
  UINT64 BytesPerSb = sb->s_bdev->bd_inode->i_size;
  UINT64 FreeBlocks = 0;
#if defined HAVE_DECL_BDEV_HARDSECT_SIZE && HAVE_DECL_BDEV_HARDSECT_SIZE
  unsigned int BytesPerSector = bdev_hardsect_size(sb_dev(sb));
#elif defined HAVE_DECL_GET_HARDSECT_SIZE && HAVE_DECL_GET_HARDSECT_SIZE
  unsigned int BytesPerSector = get_hardsect_size(sb_dev(sb));
#elif defined HAVE_DECL_BDEV_LOGICAL_BLOCK_SIZE && HAVE_DECL_BDEV_LOGICAL_BLOCK_SIZE
  unsigned int BytesPerSector = bdev_logical_block_size(sb_dev(sb));
#else
#error "UFSD_BdGetSectorSize"
#endif

  TRACE_ONLY( const char* hint = ""; )

  C_ASSERT( sizeof(i->i_ino) == sizeof(param.Info.Id) );

  sbi = UFSD_HeapAlloc( sizeof(usuper) );
  assert(NULL != sbi);
  if ( NULL == sbi )
    return -ENOMEM;

  memset( sbi, 0, sizeof(usuper) );
  Mutex_init( &sbi->ApiMutex );
  spin_lock_init( &sbi->ddt_lock );
  Mutex_init( &sbi->NoCaseMutex );
  INIT_LIST_HEAD( &sbi->clear_list );
#ifdef UFSD_USE_FLUSH_THREAD
  rwlock_init( &sbi->StateLock );
  init_waitqueue_head( &sbi->WaitDoneFlush );
  init_waitqueue_head( &sbi->WaitExitFlush );
#endif

#if defined HAVE_STRUCT_SUPER_BLOCK_S_ID && HAVE_STRUCT_SUPER_BLOCK_S_ID
  DevName = sb->s_id;
#elif defined HAVE_DECL_KDEV_T_S_DEV && HAVE_DECL_KDEV_T_S_DEV
  strncpy( sbi->s_id, bdevname( sb->s_dev ), min( sizeof(sbi->s_id), (size_t)BDEVNAME_SIZE ) );
  DevName = sbi->s_id;
#else
  bdevname( sb->s_bdev, sbi->s_id );
  DevName = sbi->s_id;
#endif

  //
  // Check for size
  //
  if ( BytesPerSb <= 10*PAGE_SIZE ) {
    printk(KERN_WARNING QUOTED_UFSD_DEVICE": %s: the volume size (0x%llx bytes) is too small to keep any fs\n", DevName, BytesPerSb );
    TRACE_ONLY( hint = "too small"; )
    goto ExitInc;
  }

  //
  // Parse options.
  //
  if ( !ufsd_parse_options( &options, &sbi->options ) ) {
    printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to mount %s. bad option '%s'\n", DevName, options );
    TRACE_ONLY( hint = "bad options"; )
    goto ExitInc;
  }

  DebugTrace( +1, Dbg, ("read_super(%s), %u: %p %lx, %s, %s\n", DevName, jiffies_to_msecs(jiffies-StartJiffies),
                        sb, sb->s_flags, (char*)data,  silent ? "silent" : "verbose"));

  si_meminfo( &SysInfo );
  assert( PAGE_SIZE == SysInfo.mem_unit );
  DebugTrace( 0, Dbg, ("Pages: total=%lx, free=%lx, buff=%lx, unit=%x\n",
                        SysInfo.totalram, SysInfo.freeram,
                        SysInfo.bufferram, SysInfo.mem_unit ));

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,0)
  DebugTrace( 0, Dbg, ("Page flags: lck=%x, err=%x, ref=%x, upt=%x, drt=%x, wrb=%x\n",
                        1u<<PG_locked, 1u<<PG_error, 1u<<PG_referenced, 1u<<PG_uptodate, 1u<<PG_dirty, 1u<<PG_writeback ));
  DebugTrace( 0, Dbg, ("Buff flags: upt=%x, drt=%x, lck=%x, map=%x, new=%x, del=%x\n",
                        1u<<BH_Uptodate, 1u<<BH_Dirty, 1u<<BH_Lock, 1u<<BH_Mapped, 1u<<BH_New, 1u<<BH_Delay ));
#else
  DebugTrace( 0, Dbg, ("Page flags: lck=%x, err=%x, ref=%x, upt=%x, drt=%x\n",
                        1u<<PG_locked, 1u<<PG_error, 1u<<PG_referenced, 1u<<PG_uptodate, 1u<<PG_dirty ));
  DebugTrace( 0, Dbg, ("Buff flags: upt=%x, drt=%x, lck=%x, map=%x, new=%x\n",
                        1u<<BH_Uptodate, 1u<<BH_Dirty, 1u<<BH_Lock, 1u<<BH_Mapped, 1u<<BH_New ));
#endif

#if defined HAVE_STRUCT_SUPER_BLOCK_S_D_OP && HAVE_STRUCT_SUPER_BLOCK_S_D_OP
  sb->s_d_op = sbi->options.nocase? &ufsd_dop : NULL;
#endif

  //
  // Save 'sync' flag
  //
  if ( FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    sbi->options.sync = 1;

  sb_set_blocksize( sb, PAGE_SIZE );
  sbi->MaxBlock = BytesPerSb >> PAGE_SHIFT;

  //
  // Log2 of sector size
  //
  sbi->SctBits = blksize_bits( BytesPerSector );

  //
  // set s_fs_info to access options in BdRead/BdWrite
  //
  sb->s_fs_info = sbi;

  //
  // Set default readahead pages
  //
#ifdef UFSD_READAHEAD_PAGES
  sbi->ReadAheadBlocks = UFSD_READAHEAD_PAGES;
#endif

  //
  // 'dev' member of superblock set to device in question.
  // At exit in case of filesystem been
  // successfully recognized next members of superblock should be set:
  // 's_magic'    - filesystem magic nr
  // 's_maxbytes' - maximal file size for this filesystem.
  //
  DebugTrace( 0, Dbg, ("%s: size = 0x%llx*0x%x >= 0x%llx*0x%lx\n",
                        DevName, BytesPerSb>>sbi->SctBits, BytesPerSector, sbi->MaxBlock, PAGE_SIZE ));

  err = UFSDAPI_VolumeMount( sb, BytesPerSector, &BytesPerSb, &sbi->options, &Volume, SysInfo.totalram, SysInfo.mem_unit );

  if ( 0 != err ) {
    if ( ERR_NEED_REPLAY == err ) {
      printk( KERN_ERR QUOTED_UFSD_DEVICE": unable to replay native journal on %s\n", DevName);
    } else {
      if (!silent)
        printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to mount %s\n", DevName);
      TRACE_ONLY( hint = "unknown fs"; )
    }
    err = -EINVAL;
    goto Exit;
  }

  sb->s_flags = (sb->s_flags & ~MS_POSIXACL) | MS_NODIRATIME
            | (sbi->options.noatime? MS_NOATIME : 0)
            | (sbi->options.acl? MS_POSIXACL : 0);

  //
  // At this point filesystem has been recognized.
  // Let's query for it's capabilities.
  //
  UFSDAPI_QueryVolumeInfo( Volume, &Info, NULL, 0, sbi->options.delalloc? &FreeBlocks : NULL );

  if ( Info.ReadOnly && !FlagOn( sb->s_flags, MS_RDONLY ) ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": No write support. Marking filesystem read-only\n");
    sb->s_flags |= MS_RDONLY;
  }

  //
  // Check for dirty flag
  //
  if ( !FlagOn( sb->s_flags, MS_RDONLY ) && Info.Dirty && !sbi->options.force ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": volume is dirty and \"force\" flag is not set\n" );
    TRACE_ONLY( hint = "no \"force\" and dirty"; )
    err = -1000; // Return special value to detect no 'force'
    goto Exit;
  }

  //
  // Set maximum file size and 'end of directory'
  //
  sb->s_maxbytes  = Info.FileSizeMax;
  sbi->Eod        = Info.Eod;
#if defined HAVE_STRUCT_SUPER_BLOCK_S_TIME_GRAN && HAVE_STRUCT_SUPER_BLOCK_S_TIME_GRAN
  sb->s_time_gran = 1000000000; // 1 sec
#endif

  //
  // Update logical sector size.
  // If journaled Hfs+ consists of more than 2^31 physical sectors
  //
  sbi->SctBits = blksize_bits( Info.BytesPerSector );

#ifdef UFSD_DELAY_ALLOC
//  sbi->s_max_writeback_mb_bump = 128;
#endif

  //
  // At this point I know enough to allocate my root.
  //
  sb->s_magic       = Info.FsSignature;
  sb->s_op          = &ufsd_sops;
#ifdef d_alloc_ufsd
  // NFS support
  sb->s_export_op   = &ufsd_export_op;
#ifdef UFSD_EXFAT
  if ( Info.NeedEncode )
    sb->s_export_op = &ufsd_encode_export_op;
#endif
#endif

#ifdef UFSD_USE_XATTR
  sb->s_xattr       = (__typeof__( sb->s_xattr )) ufsd_xattr_handlers;
#endif
  sbi->Ufsd         = Volume;
  assert(UFSD_SB( sb ) == sbi);
  assert(UFSD_VOLUME(sb) == Volume);

  param.subdir_count = 0;
  if ( 0 == UFSDAPI_FileOpen( Volume, NULL, "/", 1, NULL,
#ifdef UFSD_COUNT_CONTAINED
                              &param.subdir_count,
#else
                              NULL,
#endif
                              &param.fh, &param.Info ) ) {
    param.Create = NULL;
    i = iget4( sb, param.Info.Id, NULL, &param );
  }

  if ( NULL == i ) {
    printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to open root on %s\n", DevName );
    TRACE_ONLY( hint = "open root"; )
    err = -EINVAL;
    goto Exit;
  }

  // Always clear S_IMMUTABLE
  i->i_flags &= ~S_IMMUTABLE;
#if defined HAVE_DECL_D_MAKE_ROOT && HAVE_DECL_D_MAKE_ROOT
  sb->s_root = d_make_root( i );
#else
  sb->s_root = d_alloc_root( i );
#endif

  if ( NULL == sb->s_root ) {
    iput( i );
    printk( KERN_ERR QUOTED_UFSD_DEVICE": No memory for root entry\n" );
    TRACE_ONLY( hint = "no memory"; )
    // Not necessary to close root_ufsd
    goto Exit;
  }

#ifdef UFSD_DELAY_ALLOC
  // Should we call atomic_set( &sbi->DirtyBlocks, 0 )
  assert( 0 == atomic_long_read( &sbi->DirtyBlocks ) );
  atomic_long_set( &sbi->FreeBlocks, FreeBlocks );

  TRACE_ONLY( if ( FreeBlocks < UFSD_RED_ZONE ) sbi->DoNotTraceNoSpc = 1; )

  UFSDAPI_SetFreeSpaceCallBack( Volume, &FreeSpaceCallBack, sbi );
#endif

  // Create /proc/fs/ufsd/..
  ufsd_proc_info_create( sb );

#if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
  if ( sbi->options.raKb )
    sb->s_bdi->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#else
  if ( sbi->options.raKb )
    sb->s_bdev->bd_inode_backing_dev_info->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#endif

#ifdef UFSD_CHECK_BDI
  // Save current bdi to check for media suprise remove
  #if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
  sbi->bdi = sb->s_bdi;
  #else
  sbi->bdi = blk_get_backing_dev_info( sb->s_bdev );
  #endif
#endif

#ifdef UFSD_USE_FLUSH_THREAD
  //
  // Start flush thread.
  // To simplify remount logic do it for read-only volumes too
  //
  {
    void* p = kthread_run( ufsd_flush_thread, sb, "ufsd_%s", DevName );
    if ( IS_ERR( p ) ) {
//      printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to create flush thread\n" );
      err = PTR_ERR( p );
      goto Exit;
    }

    wait_event( sbi->WaitDoneFlush, NULL != sbi->FlushTask );
  }
#endif

  //
  // Done.
  //
  DebugTrace(-1, Dbg, ("read_super(%s), %u -> sb=%p,i=%p,r=%lx,uid=%d,gid=%d,m=%o\n", DevName, jiffies_to_msecs(jiffies-StartJiffies), sb, i,
                        i->i_ino, i->i_uid.val, i->i_gid.val, i->i_mode ));

  return 0;

ExitInc:
#ifdef UFSD_TRACE
  if ( UFSD_TraceLevel & Dbg )
    UFSD_TraceInc( +1 ); // compensate the last 'DebugTrace(-1, ... )'
#endif

Exit:
  //
  // Free resources allocated in this function
  //
  if ( NULL != Volume )
    UFSDAPI_VolumeFree( Volume );

  assert( NULL != sbi );
#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
  Mutex_destroy( &sbi->ApiMutex );
  Mutex_destroy( &sbi->NoCaseMutex );
#endif
  UFSD_unload_nls( &sbi->options );

  // NOTE: 'DevName' may point into 'sbi->s_id'
  DebugTrace(-1, Dbg, ("read_super failed to mount %s: %s ->%d\n", DevName, hint, err));

  UFSD_HeapFree( sbi );
  sb->s_fs_info = NULL;

  return err;
}


#if defined HAVE_DECL_DECLARE_FSTYPE_DEV && HAVE_DECL_DECLARE_FSTYPE_DEV

static struct super_block*
ufsd_read_super_2_4( struct super_block* sb, void*  data, int silent ) {
  int err = ufsd_read_super( sb, data, silent );
  return 0 == err? sb : ERR_PTR( err );
}


// 2.4
static DECLARE_FSTYPE_DEV( ufsd_fs_type, QUOTED_UFSD_DEVICE, ufsd_read_super_2_4 );

#else

#if defined HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT && HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT

// 2.6.38+
static struct dentry*
ufsd_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data){
  return mount_bdev(fs_type, flags, dev_name, data, ufsd_read_super);
}

#else

#if defined HAVE_DECL_FST_GETSB_V2 && HAVE_DECL_FST_GETSB_V2
///////////////////////////////////////////////////////////
// ufsd_get_sb
//
// [2.6.18 - 2.6.38]
///////////////////////////////////////////////////////////
static int
ufsd_get_sb(
    IN struct file_system_type* fs_type,
    IN int                      flags,
    IN const char*              dev_name,
    IN void*                    data,
    IN struct vfsmount*         mnt
    )
{
  int err = get_sb_bdev( fs_type, flags, dev_name, data, ufsd_read_super, mnt );
  if ( 0 == err ) {
    // Save mount path to correct ntfs symlinks (see ufsd_readlink_hlp)
    usuper* sbi = UFSD_SB( mnt->mnt_sb );
    sbi->VfsMnt = mnt;
  }
  return err;
}

#else

///////////////////////////////////////////////////////////
// ufsd_get_sb
//
// 2,6,18-
///////////////////////////////////////////////////////////
static struct super_block *
ufsd_get_sb(
    IN struct file_system_type* fs_type,
    IN int                      flags,
    IN const char*              dev_name,
    IN void*                    data
    )
{
  return get_sb_bdev(fs_type, flags, dev_name, data, ufsd_read_super );
}

#endif //defined HAVE_DECL_FST_GETSB_V2 && HAVE_DECL_FST_GETSB_V2

#endif //defined HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT && HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT

static struct file_system_type ufsd_fs_type = {
    .owner      = THIS_MODULE,
    .name       = QUOTED_UFSD_DEVICE,
#if defined HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT && HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT
    .mount      = ufsd_mount,
#else
    .get_sb     = ufsd_get_sb,
#endif
    .kill_sb    = kill_block_super,
    .fs_flags   = FS_REQUIRES_DEV,
};

#endif //defined HAVE_DECL_DECLARE_FSTYPE_DEV && HAVE_DECL_DECLARE_FSTYPE_DEV


#ifdef UFSD_DEBUG_ALLOC
///////////////////////////////////////////////////////////
// ufsdp_make_tag_string
//
// local debug support routine.
///////////////////////////////////////////////////////////
static const unsigned char*
ufsdp_make_tag_string(
    IN const unsigned char* p,
    OUT unsigned char*      tag
    )
{
  int i;
  for (i = 0; i < 4; i++, p++)
    tag[i] = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == ' '
      ? *p : '.';
  tag[4] = 0;
  return tag;
}
#endif //UFSD_DEBUG_ALLOC


///////////////////////////////////////////////////////////
// ufsd_init
//
// module init function
///////////////////////////////////////////////////////////
static int
__init ufsd_init(void)
{
  int ret;
  int EndianError;
#ifdef UFSD_DEBUG_ALLOC
  TotalKmallocs=0;
  TotalVmallocs=0;
  UsedMemMax=0;
  TotalAllocs=0;
  TotalAllocBlocks=0;
  TotalAllocSequence=0;
  MemMaxRequest=0;
  Mutex_init( &MemMutex );
#endif

#ifdef UFSD_DEBUG
  WaitMutex=0;
  StartJiffies=jiffies;
#endif

  TRACE_ONLY( ParseTraceLevel( ufsd_trace_level ) );

#ifndef UFSD_TRACE_SILENT
  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": driver (%s) loaded at %p\n%s", s_DriverVer, MODULE_BASE_ADDRESS, UFSDAPI_LibraryVersion( &EndianError ) );
#else
  UFSDAPI_LibraryVersion( &EndianError );
#endif

  if ( EndianError )
    return -EINVAL;

#ifdef UFSD_EXFAT
  //
  // exfat stores dates relative 1980
  // 'get_seconds' returns seconds since 1970
  // Check current date
  if ( get_seconds() < Seconds1970To1980 )
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": exfat can't store dates before Jan 1, 1980. Please update current date\n" );
#endif

  ufsd_proc_create();

#ifdef UFSD_BIG_UNODE
  unode_cachep = kmem_cache_create( QUOTED_UFSD_DEVICE "_unode_cache", sizeof(unode), 0,
                                    SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|SLAB_PANIC, init_once
#if defined HAVE_DECL_KMEM_CACHE_CREATE_V1 && HAVE_DECL_KMEM_CACHE_CREATE_V1
                                    , NULL
#endif
                                    );
#endif

  //
  // Allow UFSD to init globals
  //
  ret = UFSDAPI_main( 0 );
  if ( 0 == ret ) {
    //
    // Finally register filesystem
    //
    ret = register_filesystem( &ufsd_fs_type );
    if ( 0 == ret )
      return 0; // Ok
  }

  //
  // Deinit UFSD globals
  //
  UFSDAPI_main( 1 );

#ifdef UFSD_BIG_UNODE
  kmem_cache_destroy( unode_cachep );
#endif

  // remove /proc/fs/ufsd
  ufsd_proc_delete();

//  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ufsd_init failed %d\n", ret );

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_exit
//
// module exit function
///////////////////////////////////////////////////////////
static void
__exit ufsd_exit(void)
{
#ifdef UFSD_DEBUG_ALLOC
  struct list_head* pos, *pos2;
#endif
  // remove /proc/fs/ufsd
  ufsd_proc_delete();

  unregister_filesystem( &ufsd_fs_type );

  //
  // Deinit UFSD globals
  //
  UFSDAPI_main( 1 );

#ifdef UFSD_BIG_UNODE
  kmem_cache_destroy( unode_cachep );
#endif
#ifndef UFSD_TRACE_SILENT
  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": driver unloaded\n" );
#endif
#ifdef UFSD_DEBUG_ALLOC
  assert(0 == TotalAllocs);
  TraceMemReport( 1 );
  list_for_each_safe( pos, pos2, &TotalAllocHead )
  {
    MEMBLOCK_HEAD* block = list_entry( pos, MEMBLOCK_HEAD, Link );
    unsigned char* p = (unsigned char*)(block+1);
    unsigned char tag[5];
    DebugTrace(0, UFSD_LEVEL_ERROR,
           ("block %p, seq=%u, %u bytes, tag '%s': '%02x %02x %02x %02x %02x %02x %02x %02x'\n",
          p, block->Seq, block->DataSize,
          ufsdp_make_tag_string(p, tag),
          p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]));

    // Don't: (( block->AllocatedSize & 1U)? vfree : kfree)( block );
    // 'cause declaration of vfree and kfree differs!
    if ( block->AllocatedSize & 1U )
      vfree( block );
    else
      kfree( block );
  }
  DebugTrace(0, UFSD_LEVEL_ERROR, ("inuse = %u msec, wait = %u msec, HZ=%u\n", jiffies_to_msecs( jiffies - StartJiffies ), jiffies_to_msecs( WaitMutex ), (unsigned)HZ ));
#endif
  CloseTrace();
}

//
// And now the modules code and kernel interface.
//
MODULE_DESCRIPTION("Paragon " QUOTED_UFSD_DEVICE " driver");
MODULE_AUTHOR("Andrey Shedel & Alexander Mamaev");
MODULE_LICENSE("GPL");

#ifdef UFSD_TRACE
module_param_string(trace, ufsd_trace_level, sizeof(ufsd_trace_level), S_IRUGO);
MODULE_PARM_DESC(trace, " trace level for ufsd module");
module_param_string(log,ufsd_trace_file,sizeof(ufsd_trace_file), S_IRUGO);
MODULE_PARM_DESC(log, " ufsd log file, default is system log");
module_param_named(cycle, UFSD_CycleMB, ulong, S_IRUGO);
MODULE_PARM_DESC(cycle, " the size of cycle log in MB, default is 0");
#endif


#if defined HAVE_DECL_EXPORT_NO_SYMBOLS && HAVE_DECL_EXPORT_NO_SYMBOLS
EXPORT_NO_SYMBOLS;
#endif

module_init(ufsd_init)
module_exit(ufsd_exit)
