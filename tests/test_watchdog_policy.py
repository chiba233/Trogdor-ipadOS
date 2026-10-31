"""Run the actual policy helper against files that survive a second boot."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-watchdog-persistent.patch'
SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int vnode_t;
typedef int vfs_context_t;
#define NULLVP (-1)
#define VREG 1
#define UIO_READ 0
#define UIO_WRITE 1
#define UIO_SYSSPACE 0
#define IO_UNIT 0
#define IO_SYNC 0
struct vnode_attr { unsigned va_uid, va_mode; uint64_t va_data_size; };
#define VATTR_INIT(a) memset(a, 0, sizeof(*(a)))
#define VATTR_WANTED(a, f) ((void)(a))
static int vfs_context_kernel(void) { return 0; }
static int vfs_context_ucred(int c) { return c; }
static int vfs_context_proc(int c) { return c; }
static int vnode_open(const char *p,int flags,int mode,int unused,int *v,int c) {
    (void)c;(void)unused;int access=flags&FWRITE?O_WRONLY:O_RDONLY;
    *v=open(p,(flags&~(FREAD|FWRITE))|access,mode);return *v<0?errno:0;
}
static int vnode_close(int v,int flags,int c) { (void)flags;(void)c;return close(v)?errno:0; }
static int vnode_vtype(int v) { struct stat s;return !fstat(v,&s)&&S_ISREG(s.st_mode)?VREG:0; }
static int vnode_getattr(int v,struct vnode_attr *a,int c) {
    (void)c;struct stat s;if(fstat(v,&s))return errno;
    /* The harness's owner represents the kernel's root credential. */
    a->va_uid=s.st_uid==getuid()?0:1;a->va_mode=s.st_mode;a->va_data_size=s.st_size;return 0;
}
static int vn_rdwr(int operation,int v,char *bytes,int n,int offset,int space,int flags,int cred,int *residual,int proc) {
    (void)space;(void)flags;(void)cred;(void)proc;
    ssize_t done=operation==UIO_READ?pread(v,bytes,n,offset):pwrite(v,bytes,n,offset);
    if(done<0)return errno;if(residual)*residual=n-(int)done;return done==n?0:EIO;
}
'''
MAIN = r'''
int main(void) {
    const char policy[]="watchdogd disabled";
    assert(!coachz_watchdog_policy_file("disabled.plist",policy,sizeof(policy)-1));
    struct stat before,after;assert(!stat("disabled.plist",&before));
    assert(!coachz_watchdog_policy_file("disabled.plist",policy,sizeof(policy)-1));
    assert(!stat("disabled.plist",&after)&&before.st_ino==after.st_ino);
    assert(!coachz_watchdog_policy_file("disabled.migrated","",0));
    assert(!coachz_watchdog_policy_file("disabled.migrated","",0));
    assert(coachz_watchdog_policy_file("disabled.plist","different contents",sizeof(policy)-1)==EINVAL);
    char bytes[sizeof(policy)]={};int fd=open("disabled.plist",O_RDONLY);
    assert(read(fd,bytes,sizeof(bytes))==sizeof(policy)-1&&!memcmp(bytes,policy,sizeof(policy)-1));close(fd);
    assert(!chmod("disabled.plist",0666));
    assert(coachz_watchdog_policy_file("disabled.plist",policy,sizeof(policy)-1)==EACCES);
    return 0;
}
'''


class WatchdogPolicyTests(unittest.TestCase):
    def test_existing_persistent_policy_is_verified_without_rewriting(self):
        lines = PATCH.read_text().splitlines(True)
        helper = ''.join(line[1:] for line in lines if line[:1] in (' ', '+') and not line.startswith('+++'))
        with tempfile.TemporaryDirectory() as scratch:
            folder = Path(scratch)
            source = folder / 'policy.c'
            source.write_text(SHIM + helper + MAIN)
            binary = folder / 'policy-test'
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], cwd=folder, check=True)
