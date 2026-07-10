#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pmm.h"
#include "heap.h"





//this struct is the output of the walker funciton
typedef struct walker_out{

    bool success;
    uintptr_t *final_entry_pointer;
    uint64_t final_page_level;
    uintptr_t physical_base_adress;
    uint64_t page_size;
    uintptr_t *final_page_walked_ptr;
    bool table_here;
    uint64_t error_code; //10 for present bit = 0
    uintptr_t pml4_entry;
    uintptr_t pdpt_entry;
    uintptr_t pd_entry;
    uintptr_t pt_entry;
    
}walker_out;

typedef struct free_page_frame_out{

    bool success;
    uintptr_t *pgtable_pointer;
    uintptr_t physical_adress;


}free_page_frame_out;

enum walker_error{
    STOP_AT_PML4,
};

enum walker_stop_reson{
    NOT_PRESENT,
    LARGE_PAGE,
    WALK_COMPLETEST,

};

typedef struct walker_out2{

    bool success;
    uintptr_t *final_entry_pointer;
    uint64_t final_page_level;
    uintptr_t physical_base_adress;
    uint64_t page_size;
    uintptr_t *final_page_walked_ptr;
    uint64_t stop_reason;
    uint64_t parent_flags;
    uintptr_t *parent_table_ptr;
    uintptr_t parent_entry;
    uint64_t parent_lvl;
   
    uint64_t error_code;


}walker_out2;

enum spilt_error{
    
    NO_FLAG_PROVIDED_FOR_NP_CASE,
    INVALID_FLAGS_FOR_NP_CASE,
    COELSENCE, //coalesce i didnt know how to spell it
    UNDEFINED_ERROR,
    REWALK_FAILED,
    PARENT_PA_IS_0,


};

typedef struct spilt_out{

    bool success;
    uintptr_t *new_table_ptr;
    uintptr_t *old_table_ptr;
    uint64_t error_code;
    bool noop;

    
}spilt_out;

enum map_error{
    MAPPING_EXISTS_AT_DESIRED_LVL,
    MAPPING_EXISTS_AT_LARGE_PAGE,
    CANT_REMAP_TO_ANOTHER_ADRESS,
    SPILT_ERROR,
    REWALK_AFTER_SPLIT_ERROR,
    UNEXPECTED_ERROR,

};

typedef struct map_out{
   bool success;
   uint64_t error_code;
   uintptr_t* v_final_entry_ptr;
   uintptr_t physical_adress;
   uint64_t flags;


}map_out;

enum unmap_error{

    UNMAPPING_AT_PML4 = 10,

};

typedef struct unmap_out{
    bool success;
    uintptr_t unmapped_pa;
    uintptr_t *v_unmapped_pa_ptr;
    uintptr_t unmapped_va;
    uint64_t error_code;

}unmap_out;

typedef struct vm_object{


        uint64_t type; //file, anon, hardware etc..
        uint64_t size_bytes;
        uint64_t flags; //shared, private, cow, shared, pinned,writeback
        uint64_t ref_count; // num of regions referencing this object, important for freeing the object and its resources when no longer used
        uintptr_t phys_base; //base of the physical memory backing this object, can be 0 for non-physically backed objects like swap or file backed objects
        void* backing_ctx; //pointer to file struct if file backed,and pointer to the actual hardware if hardware backed, can be NULL for anon 
        //this is a poinetr to an inode for example


        //add spinlock
        uintptr_t *ref_list_region;
        uintptr_t *ref_list_space;


}vm_object;

typedef struct vm_region{

    uintptr_t va_start;
    uintptr_t va_end; //exclusive end
    uintptr_t va_start_guarded;
    uintptr_t va_end_guarded;
    size_t size_rounded;
    size_t size_bytes;
    uint64_t permissions;//page tbale permissions
    vm_object* object; //anon, file, hardware etc..
    uint32_t map_flags;//shared, private, fixed, grow down
    size_t object_base_offset;
    uint64_t type; //stack. heap, whatever
    struct vm_region *next;
    struct vm_region *prev;
    uint64_t region_id;
    


}vm_region;

typedef struct vm_space{

    uintptr_t pml4_phys;
    uintptr_t *pml4_phys_ptr;
    uintptr_t *pml4_ptr;
    vm_region *vm_region_list;
    //add spinlock later
    uintptr_t user_base;
    uintptr_t user_limit;
    uintptr_t kernel_base;
    uintptr_t kernel_limit;
    vm_region *stack_region;
    vm_region *heap_region;
    bool is_kernel;
    
    uintptr_t kernel_adress_space_base;


}vm_space;

enum create_vm_space_error{

    PML4_ALLOC_FAIL,

};

typedef struct create_vm_space_out{

    bool success;
    vm_space *space;
    uint64_t error_code;

}create_vm_space_out;



enum map_flags_region{

   
    NONE   = 0,
    FIXED  = 1u << 0,
    TOPDOWN= 1u << 1,
    PRIVATE= 1u << 2,
    SHARED = 1u << 3,
    GROWDOWN = 1u << 4,



};




enum reserve_region_error{

    FIXED_OVERLAP_ERROR,
    TOPDOWN_ALLOC_ERROR,
    FIRSTFIT_NO_SPACE_ERROR,


};

typedef struct reserve_region_out{
    bool success;
    vm_space* space_ptr;
    vm_region* region_ptr;
    uint64_t error_code;

}reserve_region_out;

typedef struct reserve_region_request{
    size_t size;
    uint64_t type;
    vm_object* object;
    uint64_t perms;
    uint64_t map_flags;
    uintptr_t hint_va;
    size_t backing_offset;



}reserve_region_request;



typedef struct map_region_out{
    
}map_region_out;




//this is the header file for our vmm, the public api, most of the returns are just for success/fail


uint64_t vmm_init(uintptr_t *pgtables); 


//pretty obvious what this does, but incase ur a nutcase we are mapping a virtual page to a physical adress here
map_out vmm_map_page(uintptr_t virtual_adress,
    uintptr_t physical_adress, 
    uintptr_t *root_table, 
    uint64_t mapping_size, 
    uint64_t flags );

//also pretty obvious, but also incase u are a nutcase, yk what ill just assume anyone reading this is a nutcase.
//this will unmap a page from the physical adress, in case u are wondering why we are not passing any physical adress
//here its bcz we want to keep abstarction clean and it will be handled internally cuz we surley will move from bitmap
//to linked list in the pmm
unmap_out vmm_unmap_page(uintptr_t *root_table, uintptr_t virtual_adress);

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

uint64_t transition(uintptr_t *old_root);

uint64_t vmm_query_page2(uintptr_t virtual_adress);

uintptr_t phys_to_virt(uintptr_t physical_adress);


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


