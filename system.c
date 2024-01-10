/*
 * system.c - system-wide status
 */
#include "wspy.h"
#include "error.h"

// system state
struct system_state {
  double load;
  struct cpustat { double value, last_read, prev_read; } cpu;
} system_state;

// read system-wide state
void read_system(void){
}

void print_system(enum output_format oformat){
  switch(oformat){
  case PRINT_CSV_HEADER:
    fprintf(outfile,"load,cpu,");
    break;
  case PRINT_CSV:
    fprintf(outfile,"%4.2f,%3.0f%%,",
	    system_state.load,
	    system_state.cpu.value);
    break;
  case PRINT_NORMAL:
    fprintf(outfile,"load = %4.2f\n",system_state.load);
    fprintf(outfile,"cpu = %4.2f\n",system_state.cpu.value);
  }
}
