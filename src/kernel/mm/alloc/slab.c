#include <slab.h>


static uintptr_t heap_base;
static uintptr_t heap_limit;
static size_t heap_used;
static size_t heap_free;  
static uintptr_t heap_curr;
static size_t heap_size_;

static const size_t slab_sizes[]= {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

#define NUM_SLAB_CLASSES (sizeof(slab_sizes) / sizeof(slab_sizes[0]))
#define SLAB_CHUNCK_SIZE  4096
#define OBJ_HEADER_SIZE sizeof(void*)


typedef struct free_node{

     struct free_node *next;

}free_node;

typedef struct slab{
    
    struct slab *next;
    free_node *free_list;
    size_t obj_size;
    size_t free_count;
    size_t total_count;


}slab;

typedef struct slab_cache{

    size_t obj_size;
    slab *slabs_list;

}slab_cache;


static slab_cache cache_list[NUM_SLAB_CLASSES]; 




uint64_t slab_init(uintptr_t *heap_start, size_t heap_size) {

    if(heap_start == NULL || heap_size == 0) {
        // Invalid heap parameters, handle error as needed
        return 0;
    }

    heap_base = (uintptr_t)heap_start;
    heap_limit = heap_base + heap_size;
    heap_used = 0;
    heap_free = heap_size;
    heap_curr = heap_base;
    heap_size_ = heap_size;

    for(int i = 0; i < NUM_SLAB_CLASSES; i++){

        cache_list[i].obj_size = slab_sizes[i];
        cache_list[i].slabs_list = NULL;


    }



    return 1;


}

slab* slab_new(slab_cache *cache){

    if(cache == NULL){return NULL;}
    size_t obj_size = cache->obj_size;

    if(obj_size < sizeof(free_node)){return NULL;}


    if((heap_curr + SLAB_CHUNCK_SIZE) > heap_limit){return NULL;}//no space in heap

    uintptr_t slab_start = heap_curr;
    slab *s = (slab*)slab_start;

    uintptr_t obj_base = slab_start + sizeof(*s);

    size_t align = sizeof(void*);
    obj_base = (obj_base + (align - 1)) & ~(align - 1);

    size_t usable_bytes = SLAB_CHUNCK_SIZE - (obj_base - slab_start);
    size_t slot_size = OBJ_HEADER_SIZE + obj_size;
    slot_size = (slot_size + (align - 1)) & ~(align - 1);
    size_t total = usable_bytes / slot_size;
    
    if(total == 0){return NULL;}

    s->next = NULL;
    s->obj_size = obj_size;
    s->total_count = total;
    s->free_count = total;
    s->free_list = NULL;

    //free list lifo
    for(int i = 0; i < total; i++){

    uintptr_t header_addr = obj_base + i * slot_size;

    *(slab**)header_addr = s;

    uintptr_t object_addr = header_addr + OBJ_HEADER_SIZE;

    free_node *node = (free_node*)object_addr;
        node->next = s->free_list;
        s->free_list = node;

    }


    heap_curr += SLAB_CHUNCK_SIZE;
    heap_used += SLAB_CHUNCK_SIZE;
    heap_free -= SLAB_CHUNCK_SIZE;


    return s;



}


void* slab_alloc(size_t size){

    if(size == 0){return NULL;}

    int count = sizeof(cache_list)/sizeof(cache_list[0]);

    slab_cache *cache_used = NULL;


    for(int i = 0; i < count; i++){

        if(cache_list[i].obj_size >= size){

            cache_used = &cache_list[i];
            break;

        }

    }

    if(cache_used == NULL){return NULL;}


    if(cache_used->slabs_list == NULL){

        slab *s =  slab_new(cache_used);
        if(s == NULL){return NULL;}
        cache_used->slabs_list  = s;


    }

    slab *s= cache_used->slabs_list;


    hell:
    
    if(s->free_count > 0){


        free_node *obj = s->free_list;
        if(obj == NULL){return NULL;}
        s->free_count -= 1;

        s->free_list = obj->next;

        return obj;
        
    }else if(s->free_count <= 0){

        
        if(s->next == NULL){

            s->next = slab_new(cache_used);
            if(s->next == NULL){return NULL;}
            s = s->next;

        goto hell;

        }

        s = s->next;

        goto hell; 



    }
    

}

// returns an object to its owner slab in the slabs free list 
uint64_t slab_free(void *obj){


    if(obj == NULL){return 1;} // noop

    uintptr_t header_adress = (uintptr_t)obj - OBJ_HEADER_SIZE;
    slab *s = *(slab**)header_adress;

    if(s == NULL){return 0;}

    uintptr_t slab_base = (uintptr_t)s;
    size_t align = sizeof(void*);

    uintptr_t data_start = slab_base + ((sizeof(*s) + (align - 1)) & ~(align - 1));

    uintptr_t slab_end = slab_base + SLAB_CHUNCK_SIZE;
    size_t slot_size = OBJ_HEADER_SIZE + s->obj_size;

    slot_size = (slot_size + (align - 1)) & ~(align - 1);

    if((uintptr_t)obj < data_start || (uintptr_t)obj >= slab_end){return 0;} //not in slab

    if (((uintptr_t)obj - (data_start + OBJ_HEADER_SIZE)) % slot_size != 0){return 0;}
    free_node *node = (free_node *)obj;
    node->next = s->free_list;
    s->free_list = node;
    s->free_count++;

    return 1;

    


    
}