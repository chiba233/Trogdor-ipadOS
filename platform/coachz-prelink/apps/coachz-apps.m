/* SPDX-License-Identifier: MIT
 * Local bootstrap and trust-cache bridge for the explicitly enabled Coachz image.
 * No kernel exploit, network download or device identity is used here.
 */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <errno.h>
#include <archive.h>
#include <archive_entry.h>

extern char **environ;
static NSString *const base = @"/private/var/ngpu/owner-apps";
static NSMutableSet<NSData *> *records;

static uint32_t be32(const void *p) { uint32_t x; memcpy(&x,p,4); return OSSwapBigToHostInt32(x); }
static uint32_t le32(const void *p) { uint32_t x; memcpy(&x,p,4); return OSSwapLittleToHostInt32(x); }

/* Parse each CodeDirectory, including alternate hashes, within its own Mach-O slice. */
static BOOL hashes(NSData *data, unsigned level)
{
    if (level > 2 || data.length < 4) return NO;
    const uint8_t *b = data.bytes; size_t n = data.length;
    if (be32(b) == FAT_MAGIC || be32(b) == FAT_MAGIC_64) {
        BOOL wide = be32(b) == FAT_MAGIC_64; uint32_t count = n >= 8 ? be32(b+4) : 0;
        size_t stride = wide ? 32 : 20;
        if (!count || count > 64 || 8+stride*count > n) return NO;
        BOOL found = NO;
        for (unsigned i=0;i<count;i++) {
            const uint8_t *a=b+8+stride*i;
            uint64_t off=be32(a+8), bytes=be32(a+12);
            if (wide) {off=(off<<32)|bytes; bytes=((uint64_t)be32(a+16)<<32)|be32(a+20);}
            if (off>n || bytes>n-off) return NO;
            if (be32(a) == CPU_TYPE_ARM64) found |= hashes([data subdataWithRange:NSMakeRange(off,bytes)],level+1);
        }
        return found;
    }
    if (le32(b)!=MH_MAGIC_64 || n<sizeof(struct mach_header_64)) return NO;
    uint32_t count=le32(b+16), commands=le32(b+20); size_t at=32;
    if (commands>n-at || count>commands/8) return NO;
    for (unsigned i=0;i<count;i++) {
        if (at+8>32+commands) return NO;
        uint32_t cmd=le32(b+at), len=le32(b+at+4);
        if (len<8 || len>32+commands-at) return NO;
        if (cmd==LC_CODE_SIGNATURE && len>=16) {
            size_t off=le32(b+at+8), bytes=le32(b+at+12);
            if (off>n || bytes>n-off || bytes<12) return NO;
            const uint8_t *s=b+off; uint32_t total=be32(s+4), entries=be32(s+8);
            if (be32(s)!=0xfade0cc0 || total>bytes || total<12 || entries>(total-12)/8) return NO;
            BOOL found=NO;
            for (unsigned j=0;j<entries;j++) {
                uint32_t cd=be32(s+16+j*8);
                if (cd>total || total-cd<44 || be32(s+cd)!=0xfade0c02) continue;
                uint32_t size=be32(s+cd+4); uint8_t type=s[cd+37], hash[CC_SHA512_DIGEST_LENGTH]={0}, record[22]={0};
                if (size<44 || size>total-cd) return NO;
                if (type==1) CC_SHA1(s+cd,size,hash);
                else if (type==2 || type==3) CC_SHA256(s+cd,size,hash);
                else if (type==4) CC_SHA384(s+cd,size,hash);
                else continue;
                memcpy(record,hash,20); record[20]=type;
                [records addObject:[NSData dataWithBytes:record length:22]]; found=YES;
            }
            return found;
        }
        at+=len;
    }
    return NO;
}

static void trustTree(NSString *root)
{
    struct stat st;
    if (lstat(root.fileSystemRepresentation,&st)) return;
    if (S_ISREG(st.st_mode)) {
        if (st.st_size>=32 && st.st_size<512*1024*1024) {
            NSData *d=[NSData dataWithContentsOfFile:root options:NSDataReadingMappedIfSafe error:nil];
            if (d) (void)hashes(d,0);
        }
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    NSDirectoryEnumerator *it=[[NSFileManager defaultManager] enumeratorAtPath:root];
    for (NSString *relative in it) {
        NSString *path=[root stringByAppendingPathComponent:relative];
        if (!lstat(path.fileSystemRepresentation,&st) && S_ISREG(st.st_mode)) trustTree(path);
    }
}

/* A .deb's data archive or an IPA can be examined without running any content. */
static BOOL trustArchive(NSData *d, unsigned level)
{
    if (!d || level>2) return NO;
    struct archive *a=archive_read_new(); archive_read_support_filter_all(a); archive_read_support_format_all(a);
    if (archive_read_open_memory(a,d.bytes,d.length)!=ARCHIVE_OK) {archive_read_free(a); return NO;}
    struct archive_entry *e; int r;
    while ((r=archive_read_next_header(a,&e))==ARCHIVE_OK) {
        la_int64_t size=archive_entry_size(e);
        if (archive_entry_filetype(e)!=AE_IFREG || size<32 || size>512*1024*1024) {archive_read_data_skip(a); continue;}
        NSMutableData *f=[NSMutableData dataWithLength:(NSUInteger)size]; size_t off=0;
        while (off<f.length) {la_ssize_t got=archive_read_data(a,(uint8_t *)f.mutableBytes+off,f.length-off);if(got<=0) break;off+=(size_t)got;}
        if(off!=f.length) {archive_read_free(a);return NO;}
        if (!hashes(f,0)) {
            const char *name=archive_entry_pathname(e);
            if (name && !strncmp(name,"data.tar",8) && !trustArchive(f,level+1)) {archive_read_free(a);return NO;}
        }
    }
    archive_read_free(a); return r==ARCHIVE_EOF;
}

static int loadRecords(void)
{
    if (!records.count) return 0;
    if (records.count>32768) return E2BIG;
    NSArray *sorted=[records.allObjects sortedArrayUsingComparator:^NSComparisonResult(NSData *a,NSData *b){int r=memcmp(a.bytes,b.bytes,22);return r<0?NSOrderedAscending:r>0?NSOrderedDescending:NSOrderedSame;}];
    NSMutableData *tc=[NSMutableData dataWithLength:24]; uint32_t version=1,count=(uint32_t)sorted.count;
    memcpy(tc.mutableBytes,&version,4); memcpy((uint8_t *)tc.mutableBytes+20,&count,4);
    for (NSData *record in sorted) [tc appendData:record];
    uint8_t digest[32]; CC_SHA256((const uint8_t *)tc.bytes+24,(CC_LONG)tc.length-24,digest);
    memcpy((uint8_t *)tc.mutableBytes+4,digest,16);
    int ret=sysctlbyname("kern.coachz_trustcache",NULL,NULL,tc.mutableBytes,tc.length) ? errno : 0;
    fprintf(stderr,"COACHZ apps: trust entries %u status %d\n",count,ret);
    [records removeAllObjects]; return ret;
}

static int run(NSString *path, NSArray<NSString *> *args)
{
    char **av=calloc(args.count+2,sizeof(char *)); av[0]=(char *)path.fileSystemRepresentation;
    for(NSUInteger i=0;i<args.count;i++) av[i+1]=(char *)args[i].UTF8String;
    pid_t pid; int status=0,r=posix_spawn(&pid,av[0],NULL,NULL,av,environ); free(av);
    if(r) return r;
    while(waitpid(pid,&status,0)<0) if(errno!=EINTR) return errno;
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}

/* Only var/jb entries. Absolute names, traversal, devices and unexpected hard links are refused. */
static int unpack(NSString *file)
{
    struct archive *a=archive_read_new(),*out=archive_write_disk_new(); struct archive_entry *e;
    archive_read_support_filter_all(a); archive_read_support_format_tar(a);
    archive_write_disk_set_options(out,ARCHIVE_EXTRACT_PERM|ARCHIVE_EXTRACT_TIME|ARCHIVE_EXTRACT_OWNER|
        ARCHIVE_EXTRACT_SECURE_NODOTDOT|ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS|ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    int r=archive_read_open_filename(a,file.fileSystemRepresentation,65536),result=0;
    if(r!=ARCHIVE_OK) result=EIO;
    if(!result && chdir("/private")) result=errno;
    while(!result && (r=archive_read_next_header(a,&e))==ARCHIVE_OK) {
        NSString *name=@(archive_entry_pathname(e)); while([name hasPrefix:@"./"])name=[name substringFromIndex:2];
        if([name isEqualToString:@"var/"]||[name isEqualToString:@"var"]||!name.length) {archive_read_data_skip(a);continue;}
        mode_t type=archive_entry_filetype(e);
        if((![name isEqualToString:@"var/jb"] && ![name hasPrefix:@"var/jb/"]) || [name.pathComponents containsObject:@".."] ||
           (type!=AE_IFREG && type!=AE_IFDIR && type!=AE_IFLNK)) {result=EINVAL;break;}
        const char *hard=archive_entry_hardlink(e);
        if(hard) {NSString *h=@(hard);while([h hasPrefix:@"./"])h=[h substringFromIndex:2];
            if(![h hasPrefix:@"var/jb/"]||[h.pathComponents containsObject:@".."]) {result=EINVAL;break;}
            archive_entry_set_hardlink(e,h.UTF8String);}
        archive_entry_set_pathname(e,name.UTF8String);
        if(archive_write_header(out,e)<ARCHIVE_WARN) {result=EIO;break;}
        const void *block;size_t bytes;la_int64_t off;int got;
        while((got=archive_read_data_block(a,&block,&bytes,&off))==ARCHIVE_OK)
            if(archive_write_data_block(out,block,bytes,off)<ARCHIVE_WARN){result=EIO;break;}
        if(got!=ARCHIVE_EOF || archive_write_finish_entry(out)<ARCHIVE_WARN) result=EIO;
    }
    if(!result && r!=ARCHIVE_EOF) result=EIO;
    if(result) fprintf(stderr,"COACHZ apps: extraction status %d detail %s\n",result,archive_error_string(out)?:archive_error_string(a)?:"none");
    archive_write_free(out);archive_read_free(a);return result;
}

static int bootstrap(void)
{
    fprintf(stderr,"COACHZ apps: installer entered pid %d uid %u\n",getpid(),getuid());
    NSString *revision=[NSString stringWithContentsOfFile:[base stringByAppendingPathComponent:@"revision"] encoding:NSUTF8StringEncoding error:nil];
    if(!revision.length)return EINVAL;
    NSString *marker=@"/private/var/db/coachz-apps-revision";
    NSString *extracted=@"/private/var/db/coachz-apps-extracted";
    BOOL fresh=![revision isEqual:[NSString stringWithContentsOfFile:marker encoding:NSUTF8StringEncoding error:nil]];
    NSString *prior=[NSString stringWithContentsOfFile:extracted encoding:NSUTF8StringEncoding error:nil];
    /* A later image must migrate package metadata explicitly, never replace an installed package database. */
    if(prior.length && ![prior isEqual:revision]) {
        fprintf(stderr,"COACHZ apps: existing environment needs an explicit migration\n");return EEXIST;
    }
    if(!prior.length) {
        fprintf(stderr,"COACHZ apps: extraction starting\n");
        int r=unpack([base stringByAppendingPathComponent:@"environment.tar.gz"]);
        fprintf(stderr,"COACHZ apps: extraction finished status %d\n",r);if(r)return r;
        if(![revision writeToFile:extracted atomically:YES encoding:NSUTF8StringEncoding error:nil])return EIO;
    }
    fprintf(stderr,"COACHZ apps: installed code scan starting\n");
    trustTree(@"/var/jb"); trustTree(@"/var/containers/Bundle/Application");
    int r=loadRecords();if(r)return r;
    setenv("PATH","/var/jb/usr/bin:/var/jb/usr/sbin:/var/jb/bin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin",1);
    setenv("HOME","/var/root",1);setenv("NO_PASSWORD_PROMPT","1",1);
    if(fresh) {
        r=run(@"/var/jb/usr/libexec/firmware",@[]);fprintf(stderr,"COACHZ apps: firmware metadata %d\n",r);if(r)return r;
        r=run(@"/var/jb/usr/sbin/pwd_mkdb",@[@"-p",@"/var/jb/etc/master.passwd"]);if(r)return r;
        r=run(@"/var/jb/usr/bin/dpkg",@[@"--configure",@"-a"]);fprintf(stderr,"COACHZ apps: package configuration %d\n",r);if(r)return r;
    }
    /* lsd may appear late. Retry registration without restarting the system shell. */
    for(NSString *app in @[@"Sileo.app",@"TrollStoreLite.app"]) {
        int status=EAGAIN;
        for(unsigned i=0;i<12 && status;i++) {
            status=run(@"/var/jb/usr/bin/uicache",@[@"-p",[@"/var/jb/Applications" stringByAppendingPathComponent:app]]);
            if(status)sleep(5);
        }
        fprintf(stderr,"COACHZ apps: registration %s status %d\n",app.UTF8String,status);if(status)return status;
    }
    if(fresh && ![revision writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil])return EIO;
    fprintf(stderr,"COACHZ apps: bootstrap ready\n");
    return 0;
}

int main(int argc,char **argv)
{
    @autoreleasepool {
        NSString *name=[@(argv[0]) lastPathComponent];
        /* Read-only package/signature queries retain the original tools' permissions. */
        if(geteuid()!=0) {
            if([name isEqualToString:@"dpkg"])argv[0]="/var/jb/usr/bin/dpkg.coachz-real";
            else if([name isEqualToString:@"ldid"])argv[0]="/var/jb/usr/bin/ldid.coachz-real";
            else return EPERM;
            execv(argv[0],argv);return errno;
        }
        records=[NSMutableSet set];
        if([name isEqualToString:@"dpkg"]) {
            for(int i=1;i<argc;i++) if([@(argv[i]) hasSuffix:@".deb"])
                if(!trustArchive([NSData dataWithContentsOfFile:@(argv[i]) options:NSDataReadingMappedIfSafe error:nil],0))return EINVAL;
            int r=loadRecords();if(r)return r;
            argv[0]="/var/jb/usr/bin/dpkg.coachz-real";execv(argv[0],argv);return errno;
        }
        if([name isEqualToString:@"ldid"]) {
            NSMutableArray *args=[NSMutableArray array];for(int i=1;i<argc;i++)[args addObject:@(argv[i])];
            int r=run(@"/var/jb/usr/bin/ldid.coachz-real",args);if(r)return r;
            for(NSString *arg in args) if(![arg hasPrefix:@"-"])trustTree(arg);
            return loadRecords();
        }
        if(argc==2 && !strcmp(argv[1],"bootstrap"))return bootstrap();
        if(argc==3 && !strcmp(argv[1],"trust")){trustTree(@(argv[2]));return loadRecords();}
        return EINVAL;
    }
}
