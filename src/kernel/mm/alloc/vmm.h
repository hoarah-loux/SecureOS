#pragma once
#include <stdint.h>
#include <stddef.h>

//this is the header file for our vmm, the public api, most of the returns are just for success/fail


void vmm_init(uintptr_t *pgtables); 


//pretty obvious what this does, but incase ur a nutcase we are mapping a virtual page to a physical adress here
uint64_t vmm_map_page(uintptr_t virtual_adress, uintptr_t physical_adress);

//also pretty obvious, but also incase u are a nutcase, yk what ill just assume anyone reading this is a nutcase.
//this will unmap a page from the physical adress, in case u are wondering why we are not passing any physical adress
//here its bcz we want to keep abstarction clean and it will be handled internally cuz we surley will move from bitmap
//to linked list in the pmm
uint64_t vmm_unmap_page(uintptr_t virtual_adress);


//NOT INTENDED FOR A NUTCASE
//the main diff between the two above and two below is that the two above dont free the physical frame, the physical
//frame is still amrked as used in the pmm in the two above and the data is also still there not overwritten
//the two below completley remove everythiong from the frame
//IN ALLOC U DONT HAVE THE FRAME, IN MAP U DO
uint64_t vmm_alloc_page(uintptr_t virtual_adress);

uint64_t vmm_free_page(uintptr_t virtual_adress);


//this function checks if a given virtual adress is mapped or not 
uint64_t vmm_query_page(uintptr_t virtual_adress);

//this changes the mapping of a physical adress to a new virtual adress
void switch_table_page(uintptr_t *page_table);


// THIS CREATES A PAGE TABLE WOOOWWW. and returns a pointer to its adress
uintptr_t* vmm_create_page_table(void);

//this takes a pointer to a page adress and DESTROYS it woooowwww
void vmm_destroy_page_table(uintptr_t *page_table);


//the further functions here are non essentials but make our vmm much more powerful


//we will define consts for permission flags, this func will give/take permissions from a page
uint64_t vmm_protect_page(uintptr_t virtual_adress, uint16_t permission_flags);

uint64_t vmm_map_region(uintptr_t start_virtual_adress, uintptr_t start_physical_adress, uint64_t num_of_pages);

uint64_t vmm_unmap_region(uintptr_t start_virtual_adress, uint64_t num_of_pages);

uint64_t vmm_alloc_region(uintptr_t virtual_adress, uint64_t num_of_pages);

uint64_t vmm_free_region(uintptr_t virtual_adress, uint64_t num_of_pages);

//these 4 functions serve the same purpouse of the ones above but for ranges and they have the same diff 

//name is very self explanatory
uintptr_t* vmm_clone_page_table(uintptr_t *source_page_table);

//same shit
uintptr_t* vmm_get_active_page_table(void);

//idk what this does we will get back later
void vmm_invalidate_tlb(uintptr_t virtual_adress);


