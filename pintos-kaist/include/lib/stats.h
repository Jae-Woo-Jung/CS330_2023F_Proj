#ifndef __LIB_STATS_H
#define __LIB_STATS_H

#define VA_STAT_REGION (0x4000)

unsigned long __gettime (void);
int __memstat (void);

unsigned long gettime (void);
int memstat (void);

#endif