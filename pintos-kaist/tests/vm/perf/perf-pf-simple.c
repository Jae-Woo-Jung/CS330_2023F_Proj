/* Checks prefetch implementation */
/* Simple sequential scan */

#include <string.h>
#include <stdint.h>
#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"
#include <stats.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)
#define ONE_MB (1 << 20) // 1MB

#define CHUNK_SIZE (16 * ONE_MB)
#define PAGE_COUNT (CHUNK_SIZE / PAGE_SIZE)

static char big_chunks[CHUNK_SIZE];

void
test_main (void) 
{
    size_t i;
    char *mem;
    unsigned long start, end;
    
    start = gettime();

    for (i = 0 ; i < PAGE_COUNT; i++) {
        if (!(i % 512))
            msg ("write sparsely over page %zu", i);
        mem = (big_chunks + (i * PAGE_SIZE));
        *mem = (char)i;
    }

    end = gettime();

    for (i = 0 ; i < PAGE_COUNT; i++) {
        mem = (big_chunks + (i * PAGE_SIZE));
        if ((char)i != *mem) {
		    fail ("data is inconsistent");
        }
        if (!(i % 512))
            msg ("check consistency in page %zu", i);
    }
    
    msg ("Time elapsed : %ld ms, Memory used : %d frames", (end - start) * 10, memstat());
    msg ("test done");

    return;
}
