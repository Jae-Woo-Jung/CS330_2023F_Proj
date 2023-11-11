/* Read from file-backed pages in a forked child multiple times
   and measure performance. */

#include <string.h>
#include <syscall.h>
#include <stdio.h>
#include <stdint.h>
#include <stats.h>
#include "tests/lib.h"
#include "tests/main.h"
#include "tests/vm/large.inc"

#define CHILD_CNT 8

void
test_main (void) {
    pid_t child[CHILD_CNT];
    size_t i;
    void *map;
    char *actual = (char *) 0x10000000;
    int handle;
    unsigned long start, end;

	CHECK ((handle = open ("large.txt")) > 1, "open \"large.txt\"");
	CHECK ((map = mmap (actual, sizeof(large), 0, handle, 0)) != MAP_FAILED, "mmap \"large.txt\"");

    CHECK (memcmp (map, large, strlen(large)) == 0, "check data consistency");

    start = gettime();

    for (i = 0; i < CHILD_CNT; i++) {
        child[i] = fork("child");
        if (child[i] == 0) {
            CHECK (memcmp (map, large, strlen(large)) == 0, "check data consistency");
            return;
        } else {
            if (wait (child[i]) != 0) 
                fail ("child abnormally exited");
        }
    }
    
    end = gettime();

    CHECK (memcmp (map, large, strlen(large)) == 0, "check data consistency");

    msg ("Time elapsed : %ld ms, Memory used : %d frames", (end - start) * 10, memstat());
    msg ("test done");

    return;
}
