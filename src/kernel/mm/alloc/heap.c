#include "buddy.h"
#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "slab.h"
#include "vmm.c"

static uintptr_t heap_start_addr;
static uintptr_t heap_end_addr;
bool is_initialized = false;

static size_t kheap_size;

void kheap_init(void){

    heap_start_addr = (uintptr_t)__kheap_start;
    heap_end_addr = (uintptr_t)__kheap_end;


    if(heap_end_addr < heap_start_addr){

        is_initialized = false;
        return;
    }

    kheap_size = (size_t)heap_end_addr - (size_t) heap_start_addr;

    if(kheap_size == 0){ is_initialized = false; return;}

    if(!slab_init((uintptr_t *)heap_start_addr, kheap_size)){ is_initialized = false; return;}
    else{is_initialized = true;}    




}

void* kmalloc(size_t size){

    if(is_initialized == false){return NULL;}

    if(size == 0){return NULL;}

    //will fail for now, but more will happen once budy is integrated.
    if(size > 4096){return NULL;}

    void* mem = slab_alloc(size);

    if(mem == NULL){return NULL;}
    return mem;

}

void* kcalloc(size_t count, size_t size){

    if(is_initialized == false){return NULL;}

    if(count == 0 || size == 0){return NULL;}

    if(count > SIZE_MAX / size){return NULL;}

    size_t total = count * size;

    void* mem = kmalloc(total);

    if(mem == NULL){return NULL;}

    unsigned char *p = (unsigned char *)mem;

    // zero initialize the allocated memory
    for(size_t i = 0; i < total; i++){
        p[i] = 0;
    }

    mem = (void*)p;

    return mem;

}

uint64_t kfree(void* ptr){

    if(is_initialized == false){return 0;}

    if(ptr == NULL){return 0;} // noop

    return slab_free(ptr);

}

