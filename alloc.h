#ifndef __ALLOC_H__
#define __ALLOC_H__

void alloc_init(unsigned long pool);
void* alloc(unsigned long sz);
void free_mem(void* data);
void clear_posion();
#endif