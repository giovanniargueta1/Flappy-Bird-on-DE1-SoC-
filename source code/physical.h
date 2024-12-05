#ifndef PHYSICAL_H_
#define PHYSICAL_H_

#include <unistd.h>  // For sysconf()

#define PAGE_ALIGN(addr) ((addr) & ~(sysconf(_SC_PAGE_SIZE) - 1))

// Function prototypes
int open_physical(int fd);
void close_physical(int fd);
void* map_physical(int fd, unsigned int base, unsigned int span);
int unmap_physical(void* virtual_base, unsigned int span);

#endif /* PHYSICAL_H_ */
