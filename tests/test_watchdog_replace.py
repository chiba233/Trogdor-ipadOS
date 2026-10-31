"""Run the policy helper with the replace revision against a differing persistent file."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_watchdog_policy import PATCH, SHIM

ROOT = Path(__file__).resolve().parents[1]
REPLACE = ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-watchdog-replace.patch'
EXTRA = r'''
#include <stdlib.h>
#include <sys/param.h>
#define Z_WAITOK 0
#define Z_NOFAIL 0
#define kalloc_data(n, f) malloc(n)
#define kfree_data(p, n) free(p)
#define VATTR_SET(a, f, v) ((a)->f = (v))
static int vnode_setattr(int v,struct vnode_attr *a,int c) { (void)c;return fchmod(v,a->va_mode)?errno:0; }
'''
MAIN = r'''
static int same(const char *path,const char *bytes,int length) {
    char buffer[256]={};int fd=open(path,O_RDONLY);if(fd<0)return 0;
    int n=(int)read(fd,buffer,sizeof(buffer));close(fd);return n==length&&!memcmp(buffer,bytes,length);
}
int main(void) {
    const char policy[]="watchdogd disabled";const char other[]="rewritten by launchd, longer";
    int fd=open("disabled.plist",O_CREAT|O_WRONLY,0644);assert(write(fd,other,sizeof(other)-1)==sizeof(other)-1);close(fd);
    assert(!coachz_watchdog_policy_file("disabled.plist",policy,sizeof(policy)-1));
    assert(same("disabled.plist",policy,sizeof(policy)-1));
    assert(same("disabled.plist.coachz-prev",other,sizeof(other)-1));
    struct stat before,after;assert(!stat("disabled.plist",&before));
    assert(!coachz_watchdog_policy_file("disabled.plist",policy,sizeof(policy)-1));
    assert(!stat("disabled.plist",&after)&&before.st_ino==after.st_ino);
    assert(!chmod("disabled.plist",0666));
    assert(!coachz_watchdog_policy_file("disabled.plist",policy,sizeof(policy)-1));
    assert(!stat("disabled.plist",&after)&&(after.st_mode&07777)==0644);
    fd=open("disabled.migrated",O_CREAT|O_WRONLY,0644);assert(write(fd,"x",1)==1);close(fd);
    assert(!coachz_watchdog_policy_file("disabled.migrated","",0));
    assert(!stat("disabled.migrated",&after)&&after.st_size==0);
    assert(same("disabled.migrated.coachz-prev","x",1));
    return 0;
}
'''


class WatchdogReplaceTests(unittest.TestCase):
    def test_differing_policy_is_kept_aside_and_reinstalled(self):
        lines = PATCH.read_text().splitlines(True)
        helper = ''.join(line[1:] for line in lines if line[:1] in (' ', '+') and not line.startswith('+++'))
        with tempfile.TemporaryDirectory() as scratch:
            folder = Path(scratch)
            source = folder / 'bsd/kern/kern_exec.c'
            source.parent.mkdir(parents=True)
            # The revision's last hunk ends inside the following function; supply its opening lines.
            following = ''.join(line[1:] for line in REPLACE.read_text().splitlines(True)[-3:])
            source.write_text(helper + '\nstatic int\n' + following)
            subprocess.run(['git', 'apply', str(REPLACE)], cwd=folder, check=True)
            revised = source.read_text()
            revised = revised[:revised.rindex('static int\ncoachz_watchdog_policy_prepare')]
            program = folder / 'policy.c'
            program.write_text(SHIM.replace('unsigned va_uid, va_mode;', 'unsigned va_uid, va_gid, va_mode;') + EXTRA + revised + MAIN)
            binary = folder / 'policy-test'
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            str(program), '-o', str(binary)], check=True)
            work = folder / 'data'
            work.mkdir()
            subprocess.run([str(binary)], cwd=work, check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
