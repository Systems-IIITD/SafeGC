#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stddef.h>

void *mymalloc(size_t Size);
void printMemoryStats();
void runGC();

#endif
