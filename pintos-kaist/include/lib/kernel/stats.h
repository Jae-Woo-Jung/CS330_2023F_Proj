#ifndef __LIB_KERNEL_STATS_H
#define __LIB_KERNSL_STATS_H

void init_stat_region (void);
void mmap_stat_region (uint64_t *);
void munmap_stat_region (uint64_t *);
void update_stat (void);

#endif