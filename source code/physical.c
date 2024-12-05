#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "physical.h"

int open_physical(int fd) {
    if (fd == -1)
        if ((fd = open("/dev/mem", (O_RDWR | O_SYNC))) == -1) {
            perror("ERROR: could not open \"/dev/mem\"...");
            return -1;
        }
    return fd;
}

void close_physical(int fd) {
    close(fd);
}

void* map_physical(int fd, unsigned int base, unsigned int span) {
    unsigned int page_aligned_base = PAGE_ALIGN(base);
    unsigned int offset = base - page_aligned_base;
    unsigned int corrected_span = span + offset; // Adjust span to include the offset
    // Debugging: Print the parameters passed to mmap
    printf("Debug: fd=%d, base=0x%X, span=%u, page_aligned_base=0x%X, offset=%u, corrected_span=%u\n",
           fd, base, span, page_aligned_base, offset, corrected_span);

    void* virtual_base = mmap(NULL, corrected_span + offset, (PROT_READ | PROT_WRITE), MAP_SHARED, fd, page_aligned_base);
    if (virtual_base == MAP_FAILED) {
        perror("ERROR: mmap() failed...");
        return NULL;
    }
    return (void*)((char*)virtual_base + offset);
}

int unmap_physical(void* virtual_base, unsigned int span) {
    unsigned int offset = (unsigned int)virtual_base % sysconf(_SC_PAGE_SIZE); // Calculate offset
    if (munmap((void*)((unsigned int)virtual_base - offset), span + offset) != 0) {
        perror("ERROR: munmap() failed...");
        return -1;
    }
    return 0;
}
