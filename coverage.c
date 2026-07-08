/*
 * coverage.c - counter capability discovery + coverage reporting, described
 * in coverage.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wspy.h"
#include "coverage.h"

int coverage_requested = 0;
int coverage_measured = 0;
struct coverage_entry *coverage_entries = NULL;
static struct coverage_entry **coverage_entries_tail = &coverage_entries;

void coverage_reset(void){
  struct coverage_entry *e;

  while (coverage_entries){
    e = coverage_entries;
    coverage_entries = e->next;
    free(e->group_label);
    free(e->counter_label);
    free(e);
  }
  coverage_entries_tail = &coverage_entries;
  coverage_requested = 0;
  coverage_measured = 0;
}

void coverage_note(const char *group_label,const char *counter_label,int available,int open_errno){
  struct coverage_entry *e = calloc(1,sizeof(*e));

  e->group_label = strdup(group_label);
  e->counter_label = strdup(counter_label);
  e->available = available;
  e->open_errno = available ? 0 : open_errno;
  e->next = NULL;
  *coverage_entries_tail = e;
  coverage_entries_tail = &e->next;

  coverage_requested++;
  if (available) coverage_measured++;
}

void print_counter_coverage(enum output_format oformat){
  struct coverage_entry *e;

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"counters_measured,counters_requested,");
    return;
  }
  if (oformat == PRINT_CSV){
    fprintf(outfile,"%d,%d,",coverage_measured,coverage_requested);
    return;
  }
  if (coverage_requested == 0) return;
  fprintf(outfile,"counter coverage     %d/%d measured\n",coverage_measured,coverage_requested);
  for (e = coverage_entries; e; e = e->next){
    if (!e->available){
      fprintf(outfile,"  unavailable         %s: %s (errno=%d - %s)\n",
	      e->group_label,e->counter_label,e->open_errno,strerror(e->open_errno));
    }
  }
}

void print_capability_report(void){
  struct coverage_entry *e;

  fprintf(outfile,"counter capability report: %d/%d available\n",coverage_measured,coverage_requested);
  for (e = coverage_entries; e; e = e->next){
    if (e->available){
      fprintf(outfile,"  available           %-12s %s\n",e->group_label,e->counter_label);
    } else {
      fprintf(outfile,"  unavailable         %-12s %s (errno=%d - %s)\n",
	      e->group_label,e->counter_label,e->open_errno,strerror(e->open_errno));
    }
  }
}
