#include "alloc.h"

int main(int argc, char* argv[]) {
    alloc_init(1048576);
    char* data = alloc(1123);
    char* more_data = alloc(128);
    free_mem(more_data);
    return 0;
}