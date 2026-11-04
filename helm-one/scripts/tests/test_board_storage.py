#!/usr/bin/env python3
"""Host fault-injection tests of board source functions (no board I/O).

Run: python3 vendor/HelmOne/scripts/tests/test_board_storage.py
Requires a host C compiler. Source functions are extracted unchanged; OS failures
and board callbacks are injected by the harness. This does not emulate NuttX.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'boards/sf32lb52/helmone'


def functions(path, names):
    source = (BOARD / path).read_text()
    result = []
    for name in names:
        match = re.search(r'^(?:static )?(?:void|bool|int|size_t)\s+' + name + r'\([^;{]*\)\s*\{', source, re.M)
        if not match:
            raise ValueError(name)
        start = source.index('{', match.start())
        depth, end = 1, start + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        result.append(source[match.start():end])
    return '\n'.join(result)


COMMON = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
static int sync_error, close_error, rename_fail_at, rename_count;
static int write_error, write_prefix, write_calls, sleeps;
static const char *bad_stat;
static int test_fsync(int fd) {
  if (sync_error) {errno=sync_error; return -1;} return fsync(fd);
}
static int test_close(int fd) {
  int r=close(fd);
  if (close_error) {errno=close_error; return -1;} return r;
}
static int test_rename(const char *a, const char *b) {
  if (++rename_count==rename_fail_at) {errno=EIO; return -1;}
  return rename(a,b);
}
static int test_stat(const char *p, struct stat *st) {
  if (bad_stat && !strcmp(p,bad_stat)) {errno=EIO; return -1;}
  return stat(p,st);
}
static ssize_t test_write(int fd,const void *p,size_t n) {
  write_calls++;
  if (write_error && write_prefix<=0) {errno=write_error; return -1;}
  if (write_error && n>(size_t)write_prefix) n=write_prefix;
  ssize_t r=write(fd,p,n);
  if (write_error && r>0) write_prefix-=r;
  return r;
}
static void put_file(const char *p,const char *value) {
  int fd=open(p,O_CREAT|O_TRUNC|O_WRONLY,0600); assert(fd>=0);
  assert(write(fd,value,strlen(value))==(ssize_t)strlen(value)); assert(!close(fd));
}
static void expect_file(const char *p,const char *value) {
  char b[1024]; int fd=open(p,O_RDONLY); assert(fd>=0);
  ssize_t n=read(fd,b,sizeof(b)); assert(n==(ssize_t)strlen(value));
  assert(!memcmp(b,value,n)); close(fd);
}
static void myvendor_watchdog_hw_pet(void) {}
#define fsync test_fsync
#define close test_close
#define rename test_rename
#define stat test_stat
#define write test_write
'''
# Object-like stat macro also changes struct stat; use a function-like macro.
COMMON = COMMON.replace('#define stat test_stat', '#define stat(...) test_stat(__VA_ARGS__)')

TRANSFER = COMMON + r'''
#define XFER_PATH_MAX 256
struct xfer_stat {uint8_t type; uint32_t size,mtime;};
static int myvendor_fw_slot_on_commit(const char *p) {(void)p; return 0;}
''' + functions('services/transfer_backend/transfer_backend.c', [
    'put_le32', 'xfer_entry_skipped', 'xfer_stat', 'xfer_list_pack', 'xfer_write_commit'
]) + r'''
int main(void) {
  put_file("old","old");
  assert(xfer_write_commit(-1,"missing.part","old")==ENOENT);
  expect_file("old","old");
  put_file("new.part","new");
  rename_count=0; rename_fail_at=2;
  assert(xfer_write_commit(-1,"new.part","old")==EIO);
  expect_file("old","old"); expect_file("new.part","new");
  rename_count=0; rename_fail_at=1;
  assert(xfer_write_commit(-1,"new.part","old")==EIO);
  expect_file("old","old"); expect_file("new.part","new");
  rename_fail_at=0;
  int fd=open("new.part",O_WRONLY); sync_error=ENOSPC;
  assert(xfer_write_commit(fd,"new.part","old")==ENOSPC);
  assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF); sync_error=0;
  expect_file("old","old"); expect_file("new.part","new");
  fd=open("new.part",O_WRONLY); close_error=EIO;
  assert(xfer_write_commit(fd,"new.part","old")==EIO); close_error=0;
  expect_file("old","old");
  assert(!xfer_write_commit(-1,"new.part","old"));
  expect_file("old","new"); assert(access("old.bak",F_OK)!=0);
  put_file("new.part","next"); put_file("old.bak","backup");
  assert(xfer_write_commit(-1,"new.part","old")==EBUSY);
  expect_file("old.bak","backup"); expect_file("old","new");

  mkdir("long",0700); put_file("long/longfilename.gpx","");
  uint16_t cursor=0; bool last=false; uint8_t out[32];
  assert(xfer_list_pack("long",&cursor,out,12,&last)==-EMSGSIZE);
  assert(cursor==0);
  assert(xfer_list_pack("long",&cursor,out,sizeof(out),&last)>0 && last);

  mkdir("list",0700); put_file("list/a",""); put_file("list/b",""); put_file("list/c","");
  DIR *d=opendir("list"); struct dirent *e; char bad[300];
  while ((e=readdir(d)) && e->d_name[0]=='.') {}
  assert(e); snprintf(bad,sizeof(bad),"list/%s",e->d_name); closedir(d); bad_stat=bad;
  cursor=0; last=false; unsigned seen=0, pages=0;
  while (!last) {
    int n=xfer_list_pack("list",&cursor,out,11,&last); assert(n>=0);
    if (n) { unsigned bit=1u<<(out[10]-'a'); assert(!(seen&bit)); seen|=bit; }
    assert(++pages<5);
  }
  assert(cursor==3); assert(seen==(7u & ~(1u<<(bad[5]-'a'))));
  puts("PASS transfer: commit failures preserve files; fd closed; pagination progresses");
}
'''

GPX = COMMON + r'''
#define GPX_PORT_FLUSH_TRY 4
#define GPX_PORT_IO_BOUNCE 256
#define GPX_PORT_PATH_MAX 256
static void *gpx_port_malloc(size_t n) {return malloc(n);}
static void gpx_port_free(void *p) {free(p);}
static void gpx_port_sleep_ms(unsigned ms) {(void)ms; sleeps++;}
'''
header = (BOARD / 'domain/gpx/gpx_port.h').read_text()
GPX += re.search(r'typedef struct gpx_port_file\s*\{.*?\} gpx_port_file_t;', header, re.S).group()
GPX += functions('domain/gpx/gpx_port.c', [
    'gpx_port_file_clear_stream', 'gpx_port_file_free_stream',
    'gpx_port_sys_write_all', 'gpx_port_file_consume_w',
    'gpx_port_file_flush_once', 'gpx_port_file_commit',
    'gpx_port_file_close', 'gpx_port_file_write', 'gpx_port_file_write_str'
])
header = (BOARD / 'domain/gpx/gpx_writer.h').read_text()
GPX += re.search(r'typedef struct gpx_writer\s*\{.*?\} gpx_writer_t;', header, re.S).group()
GPX += functions('domain/gpx/gpx_writer.c', ['gpx_writer_close'])
GPX += r'''
static gpx_port_file_t make_file(const char *path, size_t cap) {
  gpx_port_file_t f={0}; f.fd=-1; f.wbuf=malloc(cap); f.wcap=cap;
  snprintf(f.path,sizeof(f.path),"%s",path); put_file(path,""); return f;
}
int main(void) {
  gpx_port_file_t f=make_file("gpx",8);
  assert(!gpx_port_file_write(&f,"123456",6));
  write_error=ENOSPC; write_prefix=3;
  assert(gpx_port_file_write(&f,"abcdef",6)==-ENOSPC);
  assert(f.wlen==3 && !memcmp(f.wbuf,"456",3));
  expect_file("gpx","123");
  write_error=0; assert(!gpx_port_file_write(&f,"abcdef",6));
  assert(!gpx_port_file_close(&f)); expect_file("gpx","123456abcdef");

  f=make_file("sync",8); assert(!gpx_port_file_write(&f,"hello",5));
  sync_error=EIO; assert(gpx_port_file_close(&f)==-EIO);
  assert(f.fd>=0 && f.wbuf && f.wlen==0);
  sync_error=0; assert(!gpx_port_file_close(&f)); expect_file("sync","hello");

  f=make_file("retry",8); assert(!gpx_port_file_write(&f,"test",4));
  write_error=EAGAIN; write_prefix=0; write_calls=0; sleeps=0;
  assert(gpx_port_file_close(&f)==-EAGAIN);
  assert(write_calls==4 && sleeps==3 && f.wlen==4);
  write_error=0; assert(!gpx_port_file_close(&f)); expect_file("retry","test");

  gpx_writer_t w={0}; w.file=make_file("footer",128); w.open=true;
  assert(!gpx_port_file_write(&w.file,"header\n",7));
  sync_error=EIO; assert(gpx_writer_close(&w)==-EIO);
  assert(w.open && w.footer_written); sync_error=0;
  assert(!gpx_writer_close(&w)); assert(!w.open);
  expect_file("footer","header\n</trkseg>\n</trk>\n</gpx>\n");

  f=make_file("grow",4); assert(!gpx_port_file_write(&f,"larger batch",12));
  assert(!gpx_port_file_close(&f)); expect_file("grow","larger batch");
  puts("PASS GPX: bounded retry; cache retention; partial write recovery; single footer");
}
'''

BLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define FS_IDLE 0
#define FS_REPLY 1
#define COMPANION_FS_WRITE_CLOSE 7
#define LOGI(...) ((void)0)
static struct {int lock,wfd,state,tid; uint32_t wbuf_len,w_expected; char path[32],tmp_path[32];} g_fs;
static int flush_error,commit_error,closed,ok,err,held;
static bool icon;
static bool path_is_notif_icon(const char *p) {(void)p; return icon;}
static bool path_is_gnss_eph(const char *p) {(void)p; return false;}
static uint32_t fs_now_ms(void) {return 0;}
static int wbuf_flush(void) {return flush_error;}
static int xfer_write_commit(int fd,const char *a,const char *b) {(void)a;(void)b; assert(fd==9); closed++; return commit_error;}
static void xfer_write_suspend(int fd) {assert(fd==9); closed++;}
static bool icon_png_ok(const char *p) {(void)p; return false;}
static int unlink(const char *p) {(void)p; return 0;}
static void release_lfs(void) {held=0;}
static void queue_err(int c,int t,int e) {(void)c;(void)t;err=e;}
static void queue_ok(int c,int t,const void *p,int n,bool l) {(void)c;(void)t;(void)p;(void)n;(void)l;ok++;}
static void fs_progress_ping(void) {}
static void pthread_mutex_lock(int *m) {(void)m;}
static void pthread_mutex_unlock(int *m) {(void)m;}
static void myvendor_gnss_eph_reload(void) {}
static int fsync(int fd) {(void)fd; return 0;}
'''
BLE += functions('services/ble_companion/companion_fs.c', ['fs_deferred_commit'])
BLE += r'''
int main(void) {
 for(int i=0;i<3;i++) {
  g_fs.wfd=9; held=1; closed=ok=err=0;
  flush_error=i==0?ENOSPC:0; commit_error=i==1?EIO:0;
  fs_deferred_commit(); assert(closed==1 && g_fs.wfd==-1 && held==0);
  assert(ok==(i==2)); assert(err==(i==0?ENOSPC:i==1?EIO:0));
 }
 g_fs.wfd=9; held=1; closed=ok=err=0; icon=true;
 flush_error=commit_error=0; fs_deferred_commit();
 assert(closed==1 && ok==0 && err==EIO);
 puts("PASS BLE: failed flush closes fd; final reply matches commit result; invalid icon rejected");
}
'''
# The forward declaration shares the function name: extraction must choose a definition.


RECORD = GPX[:GPX.index('static gpx_port_file_t make_file')] + r'''
typedef struct {
 bool recording,start_pending,stop_drain,paused,thread_alive,release_pending;
 int close_error, lock, cmd_q;
 gpx_writer_t writer;
} gpx_record_t;
typedef struct {int id;} gpx_record_cmd_msg_t;
#define GPX_RECORD_CMD_STOP 3
static int queued,tail_error;
static void gpx_port_mutex_lock(int *p) {(void)p;}
static void gpx_port_mutex_unlock(int *p) {(void)p;}
static int gpx_record_thread_spawn(gpx_record_t *r) {r->thread_alive=true; return 0;}
static int gpx_port_queue_push(int *q,const void *p) {(void)q;(void)p;queued++;return 0;}
static void gpx_record_wake(gpx_record_t *r) {(void)r;}
static int gpx_record_flush_pending_locked(gpx_record_t *r) {(void)r;return tail_error;}
'''
RECORD += functions('domain/gpx/gpx_record.c', [
 'gpx_record_close_session_locked','gpx_record_stop',
 'gpx_record_close_error','gpx_record_wait_close'])
RECORD += r'''
int main(void) {
 gpx_record_t r={0}; r.thread_alive=true;r.recording=true;r.writer.open=true;
 r.writer.file.fd=-1;r.writer.file.wbuf=malloc(128);r.writer.file.wcap=128;
 strcpy(r.writer.file.path,"record");put_file("record","");
 tail_error=-ENOSPC;
 assert(gpx_record_close_session_locked(&r)==-ENOSPC);
 assert(!r.recording && !r.stop_drain && r.writer.open && r.close_error==-ENOSPC);
 assert(gpx_record_wait_close(&r)==-ENOSPC);
 assert(!gpx_record_stop(&r) && r.stop_drain && queued==1);
 tail_error=0;
 assert(!gpx_record_close_session_locked(&r));
 assert(!r.writer.open && !r.close_error && !r.stop_drain);
 assert(!gpx_record_wait_close(&r));
 expect_file("record","</trkseg>\n</trk>\n</gpx>\n");
 r.stop_drain=true;assert(gpx_record_wait_close(&r)==-ETIMEDOUT);
 puts("PASS recorder: failed save can be retried; error clears; wait timeout");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='myvendor-storage-tests-') as temp:
        for name, source in [('transfer', TRANSFER), ('gpx', GPX), ('ble', BLE), ('record', RECORD)]:
            work = Path(temp) / name
            work.mkdir()
            c = work / 'test.c'
            c.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra',
                            '-Wno-unused-function', '-Wno-unused-variable',
                            str(c), '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], cwd=work, check=True)


if __name__ == '__main__':
    main()
