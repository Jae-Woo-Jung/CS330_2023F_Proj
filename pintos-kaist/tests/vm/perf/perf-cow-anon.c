/* Read from anonymous pages in a forked child multiple times
   and measure performance. */

#include <string.h>
#include <syscall.h>
#include <stdio.h>
#include <stdint.h>
#include <stats.h>
#include "tests/lib.h"
#include "tests/main.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT) // 4KB
#define ONE_MB (1 << 20) // 1MB

#define CHILD_CNT 8
#define CHUNK_SIZE (16 * ONE_MB)
#define PAGE_COUNT (CHUNK_SIZE / PAGE_SIZE)

static char big_chunks[CHUNK_SIZE];

void
test_main (void) {
    pid_t child[CHILD_CNT];
    size_t i, j;
    char *mem;
    unsigned long start, end;

    for (i = 0; i < PAGE_COUNT; i++) {
        mem = (big_chunks + (i * PAGE_SIZE));
        *mem = (char)i;
    }

    start = gettime();

    for (i = 0; i < CHILD_CNT; i++) {
        child[i] = fork("child");
        if (child[i] == 0) {
            for (j = 0; j < PAGE_COUNT; j++) {
                mem = (big_chunks + (j * PAGE_SIZE));
                if ((char)j != *mem)
		            fail ("data is inconsistent");
            }
            return;
        } 
        else {
            if (wait (child[i]) != 0) 
                fail ("child abnormally exited");
        }
    }

    end = gettime();

    for (i = 0; i < PAGE_COUNT; i++) {
        mem = (big_chunks + (i * PAGE_SIZE));
        if ((char)i != *mem)
		    fail ("data is inconsistent");
    }

    msg ("Time elapsed : %ld ms, Memory used : %d frames", (end - start) * 10, memstat());
    msg ("test done");

    return;
}
