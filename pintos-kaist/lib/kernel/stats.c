#include <stats.h>

#include "../stats.h"
#include "devices/timer.h"
#include "threads/palloc.h"
#include "threads/thread.h"

static void *stat_region = NULL;

unsigned long __gettime (void) {
	return timer_ticks();
}

int __memstat (void) {
	return memstat_get_peak();
}

void init_stat_region (void) {
    stat_region = palloc_get_page(PAL_ASSERT | PAL_ZERO);
}

void mmap_stat_region (uint64_t *pml4) {
    ASSERT (pml4_set_page(pml4, VA_STAT_REGION, stat_region, false));
}

void munmap_stat_region (uint64_t *pml4) {
    pml4_clear_page(pml4, VA_STAT_REGION);
}

void update_stat (void) {
    *((unsigned long *) stat_region) = __gettime();
    *((int *) stat_region + 8) = __memstat();
}

