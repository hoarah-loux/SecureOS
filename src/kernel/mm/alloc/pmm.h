// api for the pmm, the pmm(pyhiscal mem allocator) basically 
// allings mem into 4kib frames and checks if this memory is free
// or reserved since we are using a bitmap, used=1, free=0
// we are mapping for the low 4gib of memory
#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

// this fucntion is the initializer function for the pmm 
// it takes a pointer to the mb2 info to fertch the mem map
// the ptr has to be 32 bits bcz this is what grub hands to us
// we only need the value of the pointer so we dont *mb2_info_ptr
void pmm_init(uint32_t mb2_info_ptr,uint8_t _kernel_start, uint8_t _kernel_end);


// this is 64 bit since we are dealing with memory directly now
// this function returns the physical adress of the frame
// dont be mislead we are not returning a ptr
// its just a holder that can hold the size of an adress also 64 bits
// RETURNS ZERO IN SERIAL IF WE ARE OUT OF BOUNDS
uintptr_t pmm_alloc_frame(void); 

uintptr_t pmm_alloc_frame_range(uintptr_t start_addr, uintptr_t end_addr);



// this function frees memory that needs to be freed
// thats if they were allocated before
uintptr_t pmm_free_frame(uintptr_t adress_to_free);

uintptr_t pmm_free_range(uintptr_t start_addr, uintptr_t end_addr);

// these three functions are self explanatory please just read the name
// read READDDD
uint64_t pmm_total_frame_count(size_t bitmap_size);

uint64_t pmm_free_frame_count(size_t bitmap_size);

uint64_t pmm_used_frame_count(size_t bitmap_size);

// this function returns the highest physical adress manged by this pmm 
// (the limit)
// not this will be added later for dynamic allocation not useful now since we are working witha fixed size, YETTTT!!!!!
uint64_t pmm_max_phys_addr(void);




