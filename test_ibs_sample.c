#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <linux/perf_event.h>

/* ibs_sample.c's report printer writes to outfile, normally defined in
 * wspy.c -- this test doesn't include wspy.c, so provide it directly (same
 * convention as test_ibs.c). */
FILE *outfile;

#include "ibs_sample.c"

// ---- pure decode tests: hand-built word arrays, no ring/mmap involved ----

static void test_decode_op_dc_miss_and_tlb(void){
  struct ibs_sample_state st;
  uint64_t words[5] = {0,0,0,0,0};

  printf("Testing ibs_sample_decode_op: dc_miss/dc_l1tlb_miss bits...\n");
  memset(&st,0,sizeof(st));
  words[2] = (1ULL << 37); // IbsOpData.op_brn_ret
  words[4] = (1ULL << 7) | (1ULL << 2); // IbsOpData3.dc_miss, dc_l1tlb_miss

  ibs_sample_decode_op(words,5,&st);

  assert(st.op_count == 1);
  assert(st.op_brn_ret_count == 1);
  assert(st.op_brn_misp_count == 0);
  assert(st.op_dc_miss_count == 1);
  assert(st.op_dc_l1tlb_miss_count == 1);
  assert(st.op_dc_l2tlb_miss_count == 0);
  printf("PASS: ibs_sample_decode_op dc_miss/tlb\n");
}

static void test_decode_op_brn_misp_requires_brn_ret(void){
  struct ibs_sample_state st;
  uint64_t words[5] = {0,0,0,0,0};

  printf("Testing ibs_sample_decode_op: op_brn_misp only counted when op_brn_ret is set...\n");
  memset(&st,0,sizeof(st));
  words[2] = (1ULL << 36); // op_brn_misp set, op_brn_ret NOT set

  ibs_sample_decode_op(words,5,&st);

  assert(st.op_count == 1);
  assert(st.op_brn_ret_count == 0);
  assert(st.op_brn_misp_count == 0); // must not count a misprediction bit that isn't architecturally valid
  printf("PASS: ibs_sample_decode_op brn_misp gating\n");
}

static void test_decode_op_short_record(void){
  struct ibs_sample_state st;
  uint64_t words[1] = {0};

  printf("Testing ibs_sample_decode_op: record shorter than IbsOpData3's offset...\n");
  memset(&st,0,sizeof(st));

  ibs_sample_decode_op(words,1,&st);

  assert(st.op_count == 1); // still counted as a seen op sample
  assert(st.op_brn_ret_count == 0);
  assert(st.op_dc_miss_count == 0); // never read past nwords
  printf("PASS: ibs_sample_decode_op short record\n");
}

static void test_decode_fetch_bits(void){
  struct ibs_sample_state st;
  uint64_t words[1];

  printf("Testing ibs_sample_decode_fetch: ic_miss/l1tlb_miss/l2tlb_miss bits...\n");
  memset(&st,0,sizeof(st));
  words[0] = (1ULL << 51) | (1ULL << 56); // IbsFetchCtl.ic_miss, l2tlb_miss

  ibs_sample_decode_fetch(words,1,&st);

  assert(st.fetch_count == 1);
  assert(st.fetch_ic_miss_count == 1);
  assert(st.fetch_l1tlb_miss_count == 0);
  assert(st.fetch_l2tlb_miss_count == 1);
  printf("PASS: ibs_sample_decode_fetch bits\n");
}

static void test_decode_null_safe(void){
  printf("Testing ibs_sample_decode_op/fetch: NULL state doesn't crash...\n");
  ibs_sample_decode_op(NULL,0,NULL);
  ibs_sample_decode_fetch(NULL,0,NULL);
  printf("PASS: decode NULL-safety\n");
}

// ---- ring-buffer drain tests: a malloc'd fake mmap page + data region, no
// real mmap()/perf fd involved -- decode is pure bit-twiddling and the ring
// walk only depends on the perf_event_mmap_page header fields, both
// reproducible without real hardware. ----

struct fake_ring {
  void *buf;
  struct perf_event_mmap_page *meta;
  uint8_t *data_base;
  uint64_t data_size;
};

// Same wraparound-aware copy as ibs_sample.c's ring_read(), but for writing
// -- only the test harness needs to construct ring contents; real wspy only
// ever consumes them (the kernel is the only writer in production).
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

// Writes one PERF_RECORD_SAMPLE record (header + raw_size + 4-byte caps +
// nwords regs) at ring-relative cursor, matching the wire format
// ibs_sample_drain() expects (see ibs_sample.h). Returns the cursor
// advanced past this record.
static uint64_t write_sample_record(struct fake_ring *fr,uint64_t cursor,const uint64_t *words,int nwords){
  struct perf_event_header hdr;
  uint32_t raw_size = (uint32_t)(sizeof(uint32_t) + (uint64_t)nwords*sizeof(uint64_t));
  uint32_t caps = 0;
  uint32_t record_size = (uint32_t)(sizeof(hdr)+sizeof(raw_size)+raw_size);

  hdr.type = PERF_RECORD_SAMPLE;
  hdr.misc = 0;
  hdr.size = (uint16_t)record_size;

  ring_write(fr->data_base,fr->data_size,cursor,&hdr,sizeof(hdr));
  ring_write(fr->data_base,fr->data_size,cursor+sizeof(hdr),&raw_size,sizeof(raw_size));
  ring_write(fr->data_base,fr->data_size,cursor+sizeof(hdr)+sizeof(raw_size),&caps,sizeof(caps));
  ring_write(fr->data_base,fr->data_size,cursor+sizeof(hdr)+sizeof(raw_size)+sizeof(caps),
	     words,(uint64_t)nwords*sizeof(uint64_t));

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

static void test_drain_single_op_record(void){
  struct fake_ring fr = make_fake_ring(256,0);
  uint64_t words[5] = {0,0,(1ULL<<37)|(1ULL<<36),0,(1ULL<<7)};
  uint64_t cursor;
  struct ibs_sample_state st;

  printf("Testing ibs_sample_drain: single record, no wraparound...\n");
  cursor = write_sample_record(&fr,0,words,5);
  fr.meta->data_head = cursor;

  memset(&st,0,sizeof(st));
  st.ring_base = fr.buf;
  st.ring_len = 4096+256;
  st.is_op = 1;

  ibs_sample_drain(&st);

  assert(st.drained == 1);
  assert(st.samples_seen == 1);
  assert(st.op_count == 1);
  assert(st.op_brn_ret_count == 1);
  assert(st.op_brn_misp_count == 1);
  assert(st.op_dc_miss_count == 1);
  assert(fr.meta->data_tail == cursor);

  free(fr.buf);
  printf("PASS: ibs_sample_drain single record\n");
}

static void test_drain_wraparound_record(void){
  uint64_t data_size = 64;
  uint64_t start = 40; // record is 56 bytes (8 hdr + 4 raw_size + 4 caps + 40 regs) -- starting
                       // at 40 in a 64-byte ring wraps the regs read across the boundary
  struct fake_ring fr = make_fake_ring(data_size,start);
  uint64_t words[5] = {0,0,(1ULL<<37),0,(1ULL<<7)|(1ULL<<3)};
  uint64_t cursor;
  struct ibs_sample_state st;

  printf("Testing ibs_sample_drain: record straddling the ring wraparound point...\n");
  cursor = write_sample_record(&fr,start,words,5);
  assert(cursor == start + 56);
  fr.meta->data_head = cursor; // data_tail stays at `start` -- one unread record pending

  memset(&st,0,sizeof(st));
  st.ring_base = fr.buf;
  st.ring_len = 4096+data_size;
  st.is_op = 1;

  ibs_sample_drain(&st);

  assert(st.samples_seen == 1);
  assert(st.op_count == 1);
  assert(st.op_brn_ret_count == 1);
  assert(st.op_dc_miss_count == 1);
  assert(st.op_dc_l2tlb_miss_count == 1);
  assert(fr.meta->data_tail == cursor);

  free(fr.buf);
  printf("PASS: ibs_sample_drain wraparound\n");
}

static void test_drain_lost_record(void){
  struct fake_ring fr = make_fake_ring(128,0);
  uint64_t cursor;
  struct ibs_sample_state st;

  printf("Testing ibs_sample_drain: PERF_RECORD_LOST accounting...\n");
  cursor = write_lost_record(&fr,0,7);
  fr.meta->data_head = cursor;

  memset(&st,0,sizeof(st));
  st.ring_base = fr.buf;
  st.ring_len = 4096+128;
  st.is_op = 1;

  ibs_sample_drain(&st);

  assert(st.samples_lost == 7);
  assert(st.samples_seen == 0);

  free(fr.buf);
  printf("PASS: ibs_sample_drain PERF_RECORD_LOST\n");
}

static void test_drain_idempotent(void){
  struct fake_ring fr = make_fake_ring(128,0);
  uint64_t words[5] = {0,0,(1ULL<<37),0,(1ULL<<7)};
  uint64_t cursor;
  struct ibs_sample_state st;

  printf("Testing ibs_sample_drain: a second call doesn't double-count...\n");
  cursor = write_sample_record(&fr,0,words,5);
  fr.meta->data_head = cursor;

  memset(&st,0,sizeof(st));
  st.ring_base = fr.buf;
  st.ring_len = 4096+128;
  st.is_op = 1;

  ibs_sample_drain(&st);
  ibs_sample_drain(&st);

  assert(st.op_count == 1);

  free(fr.buf);
  printf("PASS: ibs_sample_drain idempotent\n");
}

static void test_drain_null_safe(void){
  struct ibs_sample_state st;

  printf("Testing ibs_sample_drain: NULL/unmapped state doesn't crash...\n");
  ibs_sample_drain(NULL);

  memset(&st,0,sizeof(st));
  ibs_sample_drain(&st); // ring_base == NULL -> no-op
  assert(st.drained == 0);

  printf("PASS: ibs_sample_drain NULL-safety\n");
}

// Host-independent smoke test: exercises ibs_sample_counter_group() without
// assuming AMD IBS is actually present (mirrors test_ibs.c's own
// host-independent ibs_counter_group() coverage).
static void test_sample_counter_group_smoke(void){
  struct counter_group *cgroup;

  printf("Testing ibs_sample_counter_group: doesn't crash regardless of host IBS support...\n");
  cgroup = ibs_sample_counter_group("ibs_sample",NULL);
  if (cgroup){
    assert(cgroup->ncounters >= 1 && cgroup->ncounters <= 2);
    assert(cgroup->mask == COUNTER_IBS_SAMPLE);
  }
  printf("PASS: ibs_sample_counter_group smoke test\n");
}

int main(void){
  test_decode_op_dc_miss_and_tlb();
  test_decode_op_brn_misp_requires_brn_ret();
  test_decode_op_short_record();
  test_decode_fetch_bits();
  test_decode_null_safe();
  test_drain_single_op_record();
  test_drain_wraparound_record();
  test_drain_lost_record();
  test_drain_idempotent();
  test_drain_null_safe();
  test_sample_counter_group_smoke();

  printf("\nAll test_ibs_sample tests passed.\n");
  return 0;
}
