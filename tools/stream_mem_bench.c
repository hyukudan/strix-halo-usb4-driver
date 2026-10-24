// SPDX-License-Identifier: GPL-2.0-only
// Host-memory bandwidth over one /dev/tbstreamN, as used for the README
// figures (written for this lab on 2026-10-15; transfer loop unchanged).
// usage: stream_mem_bench send|recv DEVICE BYTES BLOCK_BYTES VERIFY [START_GATE]
//   BYTES total per stream, BLOCK_BYTES per write()/read() (1..64 MiB),
//   VERIFY 1 makes the receiver check every byte against the sender's
//   position-dependent pattern (64-bit words of (offset/8) ^ constant).
// Prints READY, then one JSON line. The receiver's clock starts at its first
// read and excludes those first bytes ("first_bytes_excluded"). Exit status:
// START_GATE: optional file; prints READY after open, then waits at most30s for
// it to exist before starting the transfer clock. No gate preserves old behavior.
// USB4_BENCH_NONBLOCK=1: opt-in bounded read/write retry loops, without poll().
// USB4_BENCH_EAGAIN_PAUSE_NS=N (with NONBLOCK, 1..100000): spin N ns after each
// EAGAIN before the next call (diagnostic: lower descriptor poll rate). Unset = old loop.
// Compile with USB4_BENCH_DIAGNOSTIC for syscall counters/histogram; that
// separate binary is diagnostic only. Default builds have no counter writes.
// 2 usage, 3 alloc, 4 open, 5 I/O error, 6 early EOF, 7 bad chunks, 8 gate error.
// docs/BENCHMARKS.md describes a complete, reproducible measurement.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void fill(unsigned char *buf,size_t size,uint64_t offset){
 if(!((offset|size)&7)){
  uint64_t *words=(uint64_t *)buf;
  for(size_t i=0;i<size/8;i++)words[i]=((offset/8)+i)^UINT64_C(0x9e3779b97f4a7c15);
 }else{
  for(size_t i=0;i<size;i++){uint64_t at=offset+i,v=(at/8)^UINT64_C(0x9e3779b97f4a7c15);buf[i]=(unsigned char)(v>>(8*(at%8)));}
 }
}
int main(int argc,char **argv){
 if(argc!=6&&argc!=7){fprintf(stderr,"usage: bench send|recv device bytes block_bytes verify [start_gate]\n");return 2;}
 int send=!strcmp(argv[1],"send"),verify=atoi(argv[5]);
 const char *nb=getenv("USB4_BENCH_NONBLOCK");int nonblock=nb&&!strcmp(nb,"1");
 const char *pz=getenv("USB4_BENCH_EAGAIN_PAUSE_NS");long pause_ns=pz?strtol(pz,0,10):0;
 if(pz&&(pause_ns<1||pause_ns>100000||!nonblock)){fprintf(stderr,"USB4_BENCH_EAGAIN_PAUSE_NS needs NONBLOCK and 1..100000\n");return 2;}
 uint64_t total=strtoull(argv[3],0,0),done=0,first=0,errors=0;
 size_t block=strtoul(argv[4],0,0);if(!block||block>(64u<<20)||!total)return 2;
 unsigned char *buf=malloc(block),*pattern=malloc(block+256);if(!buf||!pattern)return 3;
 int fd=open(argv[2],(send?O_WRONLY:O_RDONLY)|(nonblock?O_NONBLOCK:0));if(fd<0){perror("open");return 4;}
 printf("READY %s\n",argv[1]);fflush(stdout);
 if(argc==7){
  double until=now()+30;const struct timespec pause={0,1000000};
  while(access(argv[6],F_OK)){
   if(errno!=ENOENT){perror("start gate");close(fd);return 8;}
   if(now()>=until){fprintf(stderr,"start gate timed out\n");close(fd);return 8;}
   nanosleep(&pause,NULL);
  }
 }
 double start=now(),cpu_start=(double)clock()/CLOCKS_PER_SEC;
 double progress_deadline=start+20;
 uint64_t prepared_base=UINT64_MAX;size_t prepared_size=0;
#ifdef USB4_BENCH_DIAGNOSTIC
 uint64_t io_calls=0,io_eagain=0,io_eintr=0,io_positive=0,io_short=0,io_bytes=0,io_bins[6]={0};
#endif
 while(done<total){
  size_t want=total-done<block?(size_t)(total-done):block;
  size_t pos=(size_t)(done%block);
  if(send){
   uint64_t base=done-pos;
   if(base!=prepared_base){prepared_base=base;prepared_size=total-base<block?(size_t)(total-base):block;fill(pattern,prepared_size,base);}
   want=prepared_size-pos;
  }
  ssize_t n=send?write(fd,pattern+pos,want):read(fd,buf,want);
#ifdef USB4_BENCH_DIAGNOSTIC
  io_calls++;
  if(n<0){if(errno==EAGAIN)io_eagain++;else if(errno==EINTR)io_eintr++;}
  if(n>0){
   unsigned int bin=n<=4096?0:n<=16384?1:n<=65536?2:n<=262144?3:n<=1048576?4:5;
   io_positive++;io_bytes+=(uint64_t)n;io_short+=(size_t)n<want;io_bins[bin]++;
  }
#endif
  if(n<0){
   if(errno==EINTR)continue;
   if(nonblock&&errno==EAGAIN){
    if(now()>=progress_deadline){fprintf(stderr,"transfer no-progress timeout\n");return 5;}
#if defined(__x86_64__)
    __asm__ __volatile__("pause");
#endif
    if(pause_ns){double until=now()+pause_ns*1e-9;while(now()<until){
#if defined(__x86_64__)
     __asm__ __volatile__("pause");
#endif
    }}
    continue;
   }
   perror("transfer");return 5;
  }
  if(!n){fprintf(stderr,"unexpected EOF at %llu\n",(unsigned long long)done);return 6;}
  if(!send && verify){fill(pattern,(size_t)n,done);if(memcmp(buf,pattern,(size_t)n))errors++;}
  done+=(uint64_t)n;
  if(nonblock)progress_deadline=now()+20;
  if(!send&&!first){first=(uint64_t)n;start=now();cpu_start=(double)clock()/CLOCKS_PER_SEC;}
 }
 double end=now(),elapsed=end-start,cpu=(double)clock()/CLOCKS_PER_SEC-cpu_start;
 printf("{\"mode\":\"%s\",\"bytes\":%llu,\"seconds\":%.6f,\"gbps\":%.6f,\"cpu_seconds\":%.6f,\"verification\":%d,\"bad_chunks\":%llu,\"start_monotonic\":%.9f,\"end_monotonic\":%.9f,\"first_bytes_excluded\":%llu",argv[1],(unsigned long long)done,elapsed,(done-first)*8e-9/elapsed,cpu,verify,(unsigned long long)errors,start,end,(unsigned long long)first);
#ifdef USB4_BENCH_DIAGNOSTIC
 printf(",\"io_stats\":{\"scope\":\"complete transfer including first bytes\",\"calls\":%llu,\"eagain\":%llu,\"eintr\":%llu,\"positive\":%llu,\"short\":%llu,\"bytes\":%llu,\"bytes_le_4k_16k_64k_256k_1m_over_1m\":[",(unsigned long long)io_calls,(unsigned long long)io_eagain,(unsigned long long)io_eintr,(unsigned long long)io_positive,(unsigned long long)io_short,(unsigned long long)io_bytes);
 for(unsigned int i=0;i<6;i++)printf("%s%llu",i?",":"",(unsigned long long)io_bins[i]);
 printf("]}");
#endif
 puts("}");
 close(fd);free(buf);free(pattern);return errors?7:0;
}
