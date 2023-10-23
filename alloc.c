/* Copyright (C) 2023. Shivashish Das*/
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alloc.h"
#ifndef _MSC_VER
#include <sys/mman.h>
#endif
/* This is a simple memory allocator meant for use in single threaded applications.
   First we get memory from the system allocator which is defined by the pool size
   Larger the pool size, more is the amount you can allocate before running out of memory.
   On linux, mmap() is used for memory allocation while on windows good old calloc() is used as the system allocator

   Some basic definitions:
   Block - A memory region always of size 16 bytes. This is the basic unit of allocation
   All allocations are made in multiples of blocks. If any allocation request is not a multiple of 16 bytes, we return memory of a size
   that is the closest multiple to 16 and greater than the user requested size.

   Metadata blocks - For every allocation we allocate two extra blocks. These two blocks hold data about the allocation itself
   and serve to prevent buffer overflows too. Check the comment in alloc() to find out more. For example suppose that if the user asks for 80 bytes
   i.e 80 / 16  = 5 blocks, we will allocate 7 blocks but the pointer passed to the user will point to the seconf block so the user is unable to access
   these blocks. They do sound like a waste of some bytes but help provide protection from buffer overflows

   Posioning -  This means that user code has overflown the buffer it was allocated. All memory allocated by alloc() is now invalid
   However this may also be caused by the user simply passing an invalid pointer to us i.e a pointer allocated by some other allocator etc.
   In this case the user can clear the poisoned state by calling clear_posion() but be absolutely sure as a user about this before doing so
*/
static uint8_t* mem = 0;
static uint8_t* bitmap = 0;
static uint64_t blocks = 0;
static uint8_t poison = 0;

// Always call this before anything else.
void alloc_init(uint64_t pool) {
    if (pool % 16 != 0) {
        int rem = pool % 16;
        pool += (16 - rem);
    }

#ifndef _MSC_VER
    mem = mmap(0, pool, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "alloc_init(): could not allocate heap\n");
        perror("mmap");
        return;
    }
#else 
    mem = calloc(pool, 1);
    if (!mem) {
        fprintf(stderr, "alloc_init(): could not allocate heap\n");
        exit(1);
    }
#endif
    // Each bitmap entry can represent 8 blocks and each block is 16 bytes
    // So space representable in one uint8_t is 16 * 8 = 128 bytes
    uint64_t sz = pool / 128; 
    if (sz == 0)
        sz = 1; // allocate at least one to keep track of small pools

#ifndef _MSC_VER
    bitmap =  mmap(0, sz , PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (bitmap == MAP_FAILED) {
        fprintf(stderr, "alloc_init(): could not allocate bitmap");
        munmap(mem, pool);
    }
#else
    bitmap = calloc(sz, 1);
     if (!bitmap) {
        fprintf(stderr, "alloc_init(): could not allocate bitmap\n");
        exit(1);
    }
#endif
    // Zero the entire bitmap
    memset(bitmap, 0, sz);
    blocks = pool / 16;
}

#define IS_FREE(blkid) (bitmap[blkid / 8] & (((uint8_t)1) << blkid)) == 0
#define MARK(blkid) (bitmap[blkid / 8] ^= ((((uint8_t)1) << blkid) - 1))

// Allocate sz bytes of memory. Caution: May allocate upto 15 bytes more than sz
void* alloc(uint64_t sz) {
    if (sz % 16 != 0) {
        int rem = sz % 16;
        sz += (16 - rem);
    }
    // Allocate two extra blocks 
    // First will be allocated at just behind the first user accessible block
    // This block will have the number of blocks allocated and a randomly generated magic number each 8 bytes long
    // The last block has the "magic number" present in the first block
    // If this magic number gets modified then when free() tries to free the memory 
    // Buffer overruns will be caught and this allocator gets poisoned i.e it can no longer allocate memory
    // This is because all blocks are laid out sequentially and if the user overruns the blocks allocated 
    // Then the user may have overwritten the contents of other blocks and it is not possible to estimate the damage caused
    // and data corrupted. All pointers to blocks allocated immediately become invalid and free() posions the allocator
    // This helps catch buffer overflows early on  
    uint64_t blk = (sz / 16) + 2;

    // if we are posioned, all allocation requests will fail 
    if (poison)
        return 0;
    // Loop through the entire bitmap. If a free block is found, check if there are at least blk free blocks after it.
    // If such a contigious group of blocks is found, take appropriate actions and return to user
    // Otherwise we have ran out of memory so inform the user about it
    for (uint64_t i = 0; i < blocks; i++) {
        if (IS_FREE(i)) {
            // Check for contigious free blocks
            for (uint64_t j = i; j < (i + blk); j++) {
                if (!IS_FREE(j))
                    goto next;
            }

            // Mark all free blocks
            for (uint64_t j = i; j < (i + blk + 1); j++) {
                MARK(j);
            }
            uint64_t* ptr = mem + (i * 16);
            *ptr = blk;

            // I needed a number which was large enough to occupy 8 bytes so rand() is not enough as in most cases RAND_MAX is only USHORT_MAX
            // Instead use time() which returns a 64 bit value and is almost guaranteed to be unique on every call to alloc()
            uint64_t magic = time(0);
            *(ptr + 1) = magic;

            // Store a magic number in the last block. For the reason see free_mem()
            ptr = mem + (i * 16) + ((blk - 1) * 16);
            *ptr = magic;
            *(ptr + 1) = magic;
            // Return the user a pointer which points to the region just above our metadata block
            return mem + ((i + 1) * 16);
        }
next:        
    }
    fprintf(stderr, "Pool has been exhausted...Cannot allocate more memory");
    return 0;
}

// Frees memory allocated by alloc()
void free_mem(void* data) {
    // First get the number of blocks allocated and magic from the metadata block (i.e the block right behind what alloc() returned)
    uint64_t* ptr = data;
    ptr -= 2;
    uint64_t blk = *ptr;
    ptr++;
    uint64_t magic = *ptr;

    // The magic is stored in the last block of the allocation
    // Compare the two magic values
    // If they are equal, this memory block was allocated by us and we can free this
    // Otherwise the buffer has been overflown which has overwritten the magic number or this was not allocated by alloc() and is not ours to deal with
    ptr = data + (blk - 2) * 16;
    if (magic != *ptr)  {
        // If the buffer has overflown then mark this allocator posioned.
        // You may change the poison back to 0 in your code but be careful and do this only if you know that the buffer was not overrun
        fprintf(stderr, "Invalid pointer or buffer overrun detected..Poisoning ourself");
        poison = 1;
        return;
    }
    uint64_t offset = ((uint8_t*) data) - mem;
    offset -= 16;
    offset /= 16;

    // Clear all bits representing this block so next call to alloc() can use this 
    for (uint64_t j = offset; j < offset + blk + 1; j++) {
        MARK(j);
    }
}

// Do not call this unless you are absolutely sure about the cause of poisoning
void clear_posion() {
    poison = 0;
}