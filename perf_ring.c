/*
 * perf_ring.c - see perf_ring.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <linux/perf_event.h>
#include "error.h"
#include "perf_ring.h"

void *perf_ring_mmap(int fd,int data_pages,const char *label,size_t *ring_len_out){
  long page_size;
  size_t ring_len;
  void *base;

  page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) page_size = 4096;
  ring_len = (size_t)page_size * (1 + (size_t)data_pages);

  base = mmap(NULL,ring_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if (base == MAP_FAILED){
    warning("unable to mmap %s sampling ring buffer, errno=%d - %s\n",label,errno,strerror(errno));
    return NULL;
  }
  *ring_len_out = ring_len;
  return base;
}

void perf_ring_unmap(void *ring_base,size_t ring_len){
  if (ring_base) munmap(ring_base,ring_len);
}

// Copies len bytes starting at the ring-relative byte offset "offset" (a
// monotonically increasing stream position, per the perf mmap ABI -- not
// yet reduced mod data_size) out of the ring's data region into dest,
// handling the wraparound case where the read straddles the end of the
// physical buffer.
static void ring_read(const uint8_t *data_base,uint64_t data_size,uint64_t offset,void *dest,uint64_t len){
  uint64_t pos = offset % data_size;
  uint64_t first = data_size - pos;

  if (first >= len){
    memcpy(dest,data_base+pos,len);
  } else {
    memcpy(dest,data_base+pos,first);
    memcpy((uint8_t *)dest+first,data_base,len-first);
  }
}

uint64_t perf_ring_drain(void *ring_base,size_t ring_len,perf_ring_sample_cb cb,void *ctx,uint64_t *lost_count){
  struct perf_event_mmap_page *meta;
  uint8_t *data_base;
  uint64_t data_size;
  uint64_t head,tail;
  uint64_t samples_seen = 0;
  long page_size;

  if (!ring_base) return 0;

  meta = (struct perf_event_mmap_page *)ring_base;
  if (meta->data_size){
    data_base = (uint8_t *)ring_base + meta->data_offset;
    data_size = meta->data_size;
  } else {
    // Pre-4.1 kernel fallback: data region is exactly the pages after the
    // one mandatory header page, in the same single mmap() call.
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    data_base = (uint8_t *)ring_base + page_size;
    data_size = (uint64_t)ring_len - (uint64_t)page_size;
  }

  head = meta->data_head;
  __sync_synchronize(); // perf mmap ABI: must read data_head before the data it bounds
  tail = meta->data_tail;

  while (tail < head){
    struct perf_event_header hdr;

    if (head - tail < sizeof(hdr)) break; // partial header at the end -- nothing more to read
    ring_read(data_base,data_size,tail,&hdr,sizeof(hdr));
    if (hdr.size < sizeof(hdr)) break; // malformed record -- stop rather than loop forever

    if (hdr.type == PERF_RECORD_SAMPLE){
      uint32_t payload_len = hdr.size - sizeof(hdr);
      uint8_t payload[PERF_RING_MAX_PAYLOAD];
      uint32_t copy_len = payload_len > PERF_RING_MAX_PAYLOAD ? PERF_RING_MAX_PAYLOAD : payload_len;

      samples_seen++;
      if (copy_len > 0) ring_read(data_base,data_size,tail+sizeof(hdr),payload,copy_len);
      if (cb) cb(payload,copy_len,ctx);
    } else if (hdr.type == PERF_RECORD_LOST){
      // struct { perf_event_header; u64 id; u64 lost; } -- see perf_event.h
      uint64_t lost_fields[2] = {0,0};
      if (hdr.size >= sizeof(hdr)+sizeof(lost_fields))
	ring_read(data_base,data_size,tail+sizeof(hdr),lost_fields,sizeof(lost_fields));
      if (lost_count) *lost_count += lost_fields[1];
    }

    tail += hdr.size;
  }

  meta->data_tail = tail;
  __sync_synchronize(); // perf mmap ABI: must publish data_tail after we're done reading

  return samples_seen;
}
