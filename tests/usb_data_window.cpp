#include "USBDataWindow.h"
#include <vector>
#include <cassert>
#include <fstream>
#include <cstdio>
#include <cstdlib>
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(i*8));}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
int main(int argc,char **argv){
 assert(argc==2);std::ifstream f(argv[1],std::ios::binary);
 std::vector<uint8_t> disk(67*512);f.read((char*)disk.data(),disk.size());assert(f.gcount()==(long)disk.size());
 auto original=disk;uint64_t last=coachz_le64(disk.data()+512+32),failure=UINT64_MAX,backupLast=last;
 auto read=[&](uint64_t at,uint32_t n,uint8_t *p){if(at==failure || n>32)return false;uint64_t index=at<34?at:at>=backupLast-32?at-(backupLast-32)+34:999;
   if(index>=67 || n>67-index || (at<34 && n>34-at))return false;memcpy(p,disk.data()+index*512,n*512);return true;};
 auto refresh=[&](){auto *h=disk.data()+512;put32(h+88,coachz_crc32(disk.data()+1024,16384));put32(h+16,0);put32(h+16,coachz_crc32(h,92));
   memcpy(disk.data()+34*512,disk.data()+1024,16384);auto *b=disk.data()+66*512;put32(b+88,coachz_le32(h+88));put32(b+16,0);put32(b+16,coachz_crc32(b,92));};
 std::vector<uint8_t> work(33792);CoachzUSBDataWindow w;
 auto valid=[&](){return coachz_data_window(backupLast+1,read,w,work.data(),work.size());};
 assert(valid()&&w.blocks==(8ull<<30)/512);uint64_t first=w.first,count=w.blocks;
 assert(w.contains(first,1)&&w.contains(first+count-1,1)&&w.contains(first,count));
 assert(!w.contains(first-1,1)&&!w.contains(first+count,1)&&!w.contains(first,count+1));
 assert(!w.contains(first+count-1,2)&&!w.contains(first,0)&&!w.contains(UINT64_MAX,2)&&!w.contains(first,UINT64_MAX));
 for(uint64_t at:{1ull,2ull,last,last-32}){failure=at;assert(!valid()&&!w.blocks);}failure=UINT64_MAX;
 for(unsigned offset:{512u,528u,1024u,34u*512,66u*512}){disk=original;disk[offset]^=1;bool okay=valid();if(okay||w.blocks)fprintf(stderr,"unexpected offset %u valid %d blocks %llu\n",offset,okay,(unsigned long long)w.blocks);assert(!okay&&!w.blocks);}
 disk=original;put64(disk.data()+1024+3*128+32,coachz_le64(disk.data()+1024+2*128+40));refresh();assert(!valid());
 disk=original;put64(disk.data()+1024+3*128+40,coachz_le64(disk.data()+1024+3*128+32)+(64ull<<20)/512-1);refresh();assert(!valid());
 disk=original;disk[1024+4*128]=1;refresh();assert(!valid());
 disk=original;assert(valid());assert(!coachz_data_window(last,read,w,work.data(),work.size()));
 // vboot GptModified: backup moves to the real drive end, primary alternate stays at the image end.
 disk=original;backupLast=last+999;auto *backup=disk.data()+66*512;
 put64(backup+24,backupLast);put64(backup+72,backupLast-32);
 disk[1024+54]^=0x10;refresh();assert(valid()&&w.first==first&&w.blocks==count);
 // The kernel log ring: past every partition, before the backup GPT, 1 MiB aligned, 64 MiB; refused without room or a valid GPT.
 CoachzUSBDataWindow log;
 assert(w.partitions_end>=w.first+w.blocks-1);
 assert(!coachz_log_window(w.partitions_end+1000,w,log)&&!log.blocks);
 uint64_t medium=w.partitions_end+(1ull<<30)/512;
 assert(coachz_log_window(medium,w,log)&&log.first>w.partitions_end&&log.first%2048==0&&log.blocks==(64ull<<20)/512&&log.first+log.blocks<=medium-33);
 CoachzUSBDataWindow none;assert(!coachz_log_window(medium,none,log));
 CoachzUSBDataWindow real=w;real.partitions_end=35559423;assert(coachz_log_window(250069680,real,log)&&log.first==249937920);
 puts("PASS: actual GPT validator, both CRC copies, failed reads, overlap/size/extra partition rejection and protected-sector/overflow windows and vboot backup relocation, and the log ring placement");
}
