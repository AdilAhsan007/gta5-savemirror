/* Host-only stubs (Linux) so the backup logic can be exercised off-console. */
#include <sys/file.h>
#include <sys/statfs.h>
#define KERNEL_ADDRESS_PRISON0 0
#define KERNEL_ADDRESS_ROOTVNODE 0
static inline int kernel_set_ucred_uid(int p,int u){(void)p;(void)u;return 0;}
#define kernel_set_ucred_ruid kernel_set_ucred_uid
#define kernel_set_ucred_svuid kernel_set_ucred_uid
#define kernel_set_ucred_rgid kernel_set_ucred_uid
#define kernel_set_ucred_svgid kernel_set_ucred_uid
static inline int kernel_set_ucred_prison(int p,long v){(void)p;(void)v;return 0;}
static inline long kernel_getlong(long a){(void)a;return 0;}
static inline int kernel_set_proc_rootdir(int p,long v){(void)p;(void)v;return 0;}
#define kernel_set_proc_jaildir kernel_set_proc_rootdir
