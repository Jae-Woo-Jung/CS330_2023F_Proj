#include <stats.h>

unsigned long gettime (void) {
	return __gettime();
}

int memstat (void) {
	return __memstat();
}