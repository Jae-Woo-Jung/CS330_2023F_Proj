#include <stats.h>

unsigned long __gettime (void) {
	return *((unsigned long *) VA_STAT_REGION);
}

int __memstat (void) {
	return *((int *) VA_STAT_REGION + 8);
}