#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pmm.h>
#include <heap.h>

typedef struct thread thread;


typedef struct vm_space vm_space;

extern uintptr_t k_root;
extern uintptr_t active_pml4_phys;
extern bool physmap_ready; // true once transition() has populated the physmap




#define PTE_RW  (1ULL << 1)   // writable
#define PTE_US  (1ULL << 2)   // user-accessible
#define PTE_NX  (1ULL << 63)  // non-executable

#define KERNEL_STACK_FLAGS (PTE_RW | PTE_NX)           // 0x8000000000000002ULL
#define USER_STACK_FLAGS   (PTE_RW | PTE_US | PTE_NX)

typedef struct fs_mmap_backing fs_mmap_backing;


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

typedef struct free_out{
    bool success;
    uintptr_t freed_pa;
    uintptr_t *v_freed_pa_ptr;
    uintptr_t freed_va;
    uint64_t error_code;

}free_out;

typedef enum object_type{
    FILE,
    ANON,


}object_type;

typedef struct vm_object{


        object_type type; //file, anon, hardware etc..
        uint64_t size_bytes;
        uint64_t flags; //shared, private, cow, shared, pinned,writeback
        uint64_t ref_count; // num of regions referencing this object, important for freeing the object and its resources when no longer used
        uintptr_t phys_base; //base of the physical memory backing this object, can be 0 for non-physically backed objects like swap or file backed objects
        fs_mmap_backing* backing_ctx; //pointer to file struct if file backed,and pointer to the actual hardware if hardware backed, can be NULL for anon 
        //this is a poinetr to an inode for example


        //add spinlock
        uintptr_t *ref_list_region;
        uintptr_t *ref_list_space;


}vm_object;


// we have multiple types of region lists and each has there own pointers

// global kenrel asset list accessed via  *next_k_g and  *prev_k_g;


//space lists

//kernel assets via space->kenrel asset list and use next prev normally
//normal list via space->region list next and prev normally


//threads
 
//kerneal assets via t->kernel assets and use next_t and prev_t normally
//normal list via t->region list and use next_t and prev_t normally
    

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

    // How many bytes starting at object_base_offset actually come from the
    // backing file. Meaningful only for FILE regions.
    //
    // A mapping is page-granular but a file's contribution to it is not. An
    // ELF PT_LOAD with p_memsz > p_filesz is the normal case: the bytes past
    // p_filesz are .bss and must read as zero, but they sit inside the same
    // region and usually inside the same page as real file content. Without
    // this, the fault handler reads a full page from the file and .bss comes
    // back as whatever bytes follow the segment on disk.
    //
    // populate_fault_frame() reads min(4096, file_valid_len - page_delta) and
    // zero-fills the rest of the page; a page starting at or past this reads
    // as entirely zero and touches the file not at all.
    //
    // mmap() defaults it to the mapping length, i.e. "all of it is file", so
    // a caller that does not care gets the old behaviour. The ELF loader
    // narrows it to p_filesz.
    size_t file_valid_len;
    uint64_t type; //stack. heap, whatever
    struct vm_region *next;
    struct vm_region *prev;
    struct vm_region *next_k_g;
    struct vm_region *prev_k_g;
    struct vm_region *next_t;
    struct vm_region *prev_t;
    uint64_t region_id;
    bool is_kernel_asset;
    vm_space *owner_space;
    


}vm_region;

typedef struct vm_space{

    uintptr_t pml4_phys;
    uintptr_t *pml4_phys_ptr;
    uintptr_t *pml4_ptr;
    vm_region *vm_region_list;
    vm_region *kernel_asset_list;
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

extern vm_space active_space;


// mmap() used to take a `mmap_req*`. It does not any more: the request had
// exactly six caller-chosen fields, which is exactly how many arguments the
// syscall ABI passes in registers (rdi/rsi/rdx/r10/r8/r9), so the struct only
// ever forced a ring3 caller to build one in memory for the kernel to take
// apart again. Passing the six directly removes that round trip and makes the
// call writable from assembly without laying out a struct.
//
// It also removes the seventh field, `thread *t`, which named the mapping's
// target. That one was never a caller's to choose - sys_mmap dereferenced
// whatever ring3 put there, an arbitrary kernel pointer read. The target is
// now a kernel-supplied parameter, so a user caller can name only addresses in
// its own address space and fds in its own table.

enum create_vm_space_error{

    PML4_ALLOC_FAIL,

};

typedef struct create_vm_space_out{

    bool success;
    vm_space *space;
    uint64_t error_code;

}create_vm_space_out;

enum region_types{

    STACK,
    ELF_SEGMENT,



};



enum map_flags_region{

   
    NONE   = 0,
    FIXED  = 1u << 0,
    TOPDOWN= 1u << 1,
    PRIVATE= 1u << 2,
    SHARED = 1u << 3,
    GROWDOWN = 1u << 4,
    KERNEL_ASSET = 1u << 5,



};




enum reserve_region_error{

    FIXED_OVERLAP_ERROR,
    TOPDOWN_ALLOC_ERROR,
    FIRSTFIT_NO_SPACE_ERROR,
    KERNEL_ASSET_ALLOCATION_ERROR,
    OVERFLOW_ERROR,


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
    thread *t;



}reserve_region_request;

typedef struct kernel_asset_list_g{

    vm_region *head;
    vm_region *tail;


}kernel_asset_list_g;



typedef struct map_region_out{
    
}map_region_out;

typedef struct search_region_list_out{

    vm_region *region;
    bool success;


}search_region_list_out;




//this is the header file for our vmm, the public api, most of the returns are just for success/fail


uint64_t vmm_init(uintptr_t *pgtables); 


//pretty obvious what this does, but incase ur a nutcase we are mapping a virtual page to a physical adress here
map_out vmm_map_page(uintptr_t virtual_adress,
    uintptr_t physical_adress, 
    uintptr_t *root_table, 
    uint64_t desired_map_lvl, 
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

free_out vmm_free_page(uintptr_t *root_table, uintptr_t virtual_adress);


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


create_vm_space_out create_vm_space();
reserve_region_out reserve_region(vm_space* space, reserve_region_request* req);
uint64_t destroy_vm_region(vm_space *space, vm_region *region);
uint64_t destroy_vm_space(vm_space *space);

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

search_region_list_out search_region_list(vm_space *space, uintptr_t va);

// Reserve a region in t's address space. Returns the base VA, or NULL.
//
// `t` is the target and is never taken from userspace - see the note above the
// deleted mmap_req. Pass file_descriptor == 0 for an anonymous mapping;
// anything else names an fd in t's own process fd table.
void *mmap(thread *t, void *addr, size_t len, uint64_t perms,
           uint64_t map_flags, uint64_t file_descriptor, uint64_t file_offset);

// SYS_mmap. Same six arguments in the same order, straight out of the syscall
// registers; the target thread is resolved from the scheduler, not the caller.
int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t perms,
                 uint64_t map_flags, uint64_t file_descriptor,
                 uint64_t file_offset);


