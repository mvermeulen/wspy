#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <linux/perf_event.h>

/* symbol_sample.c's mmap/free paths use warning() (error.h), normally
 * defined in wspy.c -- this test doesn't include wspy.c, so provide it
 * directly (same convention as test_ibs_sample.c). */
FILE *outfile;

#include "symbol_sample.c"

// ---- pure histogram tests: no ring/mmap involved ----

static void test_record_addr_new_and_repeat(void){
  struct symbol_sample_state st;

  printf("Testing symbol_sample_record_addr: new address, then a repeat...\n");
  memset(&st,0,sizeof(st));
  st.addrs_cap = SYMBOL_SAMPLE_INITIAL_CAPACITY;
  st.addrs = calloc((size_t)st.addrs_cap,sizeof(*st.addrs));

  symbol_sample_record_addr(&st,0x1000);
  assert(st.naddrs == 1);
  assert(st.addrs[0].addr == 0x1000);
  assert(st.addrs[0].count == 1);

  symbol_sample_record_addr(&st,0x1000);
  assert(st.naddrs == 1); // same address -- count bumps, no new bucket
  assert(st.addrs[0].count == 2);

  symbol_sample_record_addr(&st,0x2000);
  assert(st.naddrs == 2);
  assert(st.addrs[1].addr == 0x2000);
  assert(st.addrs[1].count == 1);

  free(st.addrs);
  printf("PASS: symbol_sample_record_addr new/repeat\n");
}

static void test_record_addr_grows_past_initial_capacity(void){
  struct symbol_sample_state st;
  int i;
  int n = SYMBOL_SAMPLE_INITIAL_CAPACITY*2 + 3; // force at least one doubling

  printf("Testing symbol_sample_record_addr: grows past initial capacity...\n");
  memset(&st,0,sizeof(st));
  st.addrs_cap = SYMBOL_SAMPLE_INITIAL_CAPACITY;
  st.addrs = calloc((size_t)st.addrs_cap,sizeof(*st.addrs));

  for (i=0;i<n;i++) symbol_sample_record_addr(&st,0x1000+(uint64_t)i*8);

  assert(st.naddrs == n);
  assert(st.addrs_cap >= n);
  for (i=0;i<n;i++){
    assert(st.addrs[i].addr == 0x1000+(uint64_t)i*8);
    assert(st.addrs[i].count == 1);
  }

  free(st.addrs);
  printf("PASS: symbol_sample_record_addr grows past initial capacity\n");
}

static void test_record_addr_null_safe(void){
  printf("Testing symbol_sample_record_addr: NULL state doesn't crash...\n");
  symbol_sample_record_addr(NULL,0x1234);
  printf("PASS: symbol_sample_record_addr NULL-safety\n");
}

// ---- ring-buffer drain tests: a malloc'd fake mmap page + data region, no
// real mmap()/perf fd involved -- same harness pattern as
// test_ibs_sample.c's fake_ring, but PERF_SAMPLE_IP's payload is just one
// bare u64 (the instruction pointer), not IBS's raw_size+caps+regs[]
// layout. ----

struct fake_ring {
  void *buf;
  struct perf_event_mmap_page *meta;
  uint8_t *data_base;
  uint64_t data_size;
};

static void ring_write(uint8_t *data_base,uint64_t data_size,uint64_t offset,const void *src,uint64_t len){
  uint64_t pos = offset % data_size;
  uint64_t first = data_size - pos;

  if (first >= len){
    memcpy(data_base+pos,src,len);
  } else {
    memcpy(data_base+pos,src,first);
    memcpy(data_base,(const uint8_t *)src+first,len-first);
  }
}

static struct fake_ring make_fake_ring(uint64_t data_size,uint64_t start_cursor){
  struct fake_ring fr;
  size_t page_size = 4096;
  size_t total = page_size + (size_t)data_size;

  fr.buf = calloc(1,total);
  assert(fr.buf != NULL);
  fr.meta = (struct perf_event_mmap_page *)fr.buf;
  fr.meta->data_offset = page_size;
  fr.meta->data_size = data_size;
  fr.meta->data_head = start_cursor;
  fr.meta->data_tail = start_cursor;
  fr.data_base = (uint8_t *)fr.buf + page_size;
  fr.data_size = data_size;
  return fr;
}

static uint64_t write_ip_record(struct fake_ring *fr,uint64_t cursor,uint64_t ip){
  struct perf_event_header hdr;
  uint32_t record_size = (uint32_t)(sizeof(hdr)+sizeof(ip));

  hdr.type = PERF_RECORD_SAMPLE;
  hdr.misc = 0;
  hdr.size = (uint16_t)record_size;

  ring_write(fr->data_base,fr->data_size,cursor,&hdr,sizeof(hdr));
  ring_write(fr->data_base,fr->data_size,cursor+sizeof(hdr),&ip,sizeof(ip));

  return cursor + record_size;
}

static uint64_t write_lost_record(struct fake_ring *fr,uint64_t cursor,uint64_t lost_count){
  struct perf_event_header hdr;
  uint64_t fields[2] = {0,lost_count};
  uint32_t record_size = (uint32_t)(sizeof(hdr)+sizeof(fields));

  hdr.type = PERF_RECORD_LOST;
  hdr.misc = 0;
  hdr.size = (uint16_t)record_size;

  ring_write(fr->data_base,fr->data_size,cursor,&hdr,sizeof(hdr));
  ring_write(fr->data_base,fr->data_size,cursor+sizeof(hdr),fields,sizeof(fields));

  return cursor + record_size;
}

static struct symbol_sample_state make_state(struct fake_ring *fr,uint64_t ring_len){
  struct symbol_sample_state st;

  memset(&st,0,sizeof(st));
  st.ring_base = fr->buf;
  st.ring_len = ring_len;
  st.addrs_cap = SYMBOL_SAMPLE_INITIAL_CAPACITY;
  st.addrs = calloc((size_t)st.addrs_cap,sizeof(*st.addrs));
  return st;
}

static void test_drain_multiple_records_same_and_different_ip(void){
  struct fake_ring fr = make_fake_ring(256,0);
  uint64_t cursor = 0;
  struct symbol_sample_state st;

  printf("Testing symbol_sample_drain: several records, repeated + distinct IPs...\n");
  cursor = write_ip_record(&fr,cursor,0xdeadbeef);
  cursor = write_ip_record(&fr,cursor,0xdeadbeef);
  cursor = write_ip_record(&fr,cursor,0xc0ffee);
  fr.meta->data_head = cursor;

  st = make_state(&fr,4096+256);
  symbol_sample_drain(&st);

  assert(st.drained == 1);
  assert(st.samples_seen == 3);
  assert(st.naddrs == 2);
  assert(st.addrs[0].addr == 0xdeadbeef && st.addrs[0].count == 2);
  assert(st.addrs[1].addr == 0xc0ffee && st.addrs[1].count == 1);
  assert(fr.meta->data_tail == cursor);

  free(st.addrs);
  free(fr.buf);
  printf("PASS: symbol_sample_drain multiple records\n");
}

static void test_drain_wraparound_record(void){
  uint64_t data_size = 32;
  uint64_t start = 24; // record is 16 bytes (8 hdr + 8 ip) -- starting at 24
                       // in a 32-byte ring wraps the ip read across the boundary
  struct fake_ring fr = make_fake_ring(data_size,start);
  uint64_t cursor;
  struct symbol_sample_state st;

  printf("Testing symbol_sample_drain: record straddling the ring wraparound point...\n");
  cursor = write_ip_record(&fr,start,0x123456789abcdef0ULL);
  assert(cursor == start + 16);
  fr.meta->data_head = cursor;

  st = make_state(&fr,4096+data_size);
  symbol_sample_drain(&st);

  assert(st.samples_seen == 1);
  assert(st.naddrs == 1);
  assert(st.addrs[0].addr == 0x123456789abcdef0ULL);
  assert(fr.meta->data_tail == cursor);

  free(st.addrs);
  free(fr.buf);
  printf("PASS: symbol_sample_drain wraparound\n");
}

static void test_drain_lost_record(void){
  struct fake_ring fr = make_fake_ring(128,0);
  uint64_t cursor;
  struct symbol_sample_state st;

  printf("Testing symbol_sample_drain: PERF_RECORD_LOST accounting...\n");
  cursor = write_lost_record(&fr,0,7);
  fr.meta->data_head = cursor;

  st = make_state(&fr,4096+128);
  symbol_sample_drain(&st);

  assert(st.samples_lost == 7);
  assert(st.samples_seen == 0);
  assert(st.naddrs == 0);

  free(st.addrs);
  free(fr.buf);
  printf("PASS: symbol_sample_drain PERF_RECORD_LOST\n");
}

static void test_drain_idempotent(void){
  struct fake_ring fr = make_fake_ring(128,0);
  uint64_t cursor;
  struct symbol_sample_state st;

  printf("Testing symbol_sample_drain: a second call doesn't double-count...\n");
  cursor = write_ip_record(&fr,0,0xabc);
  fr.meta->data_head = cursor;

  st = make_state(&fr,4096+128);
  symbol_sample_drain(&st);
  symbol_sample_drain(&st);

  assert(st.samples_seen == 1);
  assert(st.naddrs == 1);
  assert(st.addrs[0].count == 1);

  free(st.addrs);
  free(fr.buf);
  printf("PASS: symbol_sample_drain idempotent\n");
}

static void test_drain_null_safe(void){
  struct symbol_sample_state st;

  printf("Testing symbol_sample_drain: NULL/unmapped state doesn't crash...\n");
  symbol_sample_drain(NULL);

  memset(&st,0,sizeof(st));
  symbol_sample_drain(&st); // ring_base == NULL -> no-op
  assert(st.drained == 0);

  printf("PASS: symbol_sample_drain NULL-safety\n");
}

// Host-independent smoke test: exercises symbol_sample_mmap()/_free() on an
// invalid fd without assuming any real perf_event_open() fd is available
// (mirrors test_ibs_sample.c's own host-independent counter_group smoke
// test convention).
static void test_mmap_invalid_fd_smoke(void){
  struct symbol_sample_state *state;

  printf("Testing symbol_sample_mmap: invalid fd degrades to NULL, not a crash...\n");
  state = symbol_sample_mmap(-1);
  assert(state == NULL);
  symbol_sample_free(state); // NULL-safe
  printf("PASS: symbol_sample_mmap invalid fd smoke test\n");
}

int main(void){
  test_record_addr_new_and_repeat();
  test_record_addr_grows_past_initial_capacity();
  test_record_addr_null_safe();
  test_drain_multiple_records_same_and_different_ip();
  test_drain_wraparound_record();
  test_drain_lost_record();
  test_drain_idempotent();
  test_drain_null_safe();
  test_mmap_invalid_fd_smoke();

  printf("\nAll test_symbol_sample tests passed.\n");
  return 0;
}
