#include "vmm.h"
#include "serial.h"
#include <string.h>
#include "pmm.h"


//redundant but looks cool
typedef uint8_t bool;
#define true 1
#define false 0


//this is the local pointer that will hold the lable
static uintptr_t *toptable;
static uintptr_t adress_toptable;
static bool is_initialized = false;





// FUTURE IMPROVEMENT:
// current init is good for trusted bootstub paging, but later we can harden it more.
// verify more of the inherited paging layout, not just the first pml4/pdpt path.
// confirm the inherited entries are the exact kind we expect, not only present/aligned.
// optionally sanity-check the next inherited level too and add a short serial debug msg.
// keep marking the vmm ready only after all expected boot paging checks pass.
// and also check if the pds are(just one large mapping) or do they point to pts


uint64_t vmm_init(uintptr_t *pgtables){

    is_initialized = false;
    toptable = NULL;
    adress_toptable = 0;
    


    if(pgtables == NULL ){
        return 0;

    }

    size_t allign_const = 4096;

    if((uintptr_t)pgtables % allign_const != 0 ){

        return 0;
    }


    //first entry in top pml4 table
    uintptr_t first_entry = *pgtables;

    //check if the adress is non zero and check the present flag bit[0]
    if((first_entry==0) || (first_entry & 1) == 0){
        return 0;
    }

    //pdpt adress(removing the flags form the entry (low 12 bits))
    uintptr_t first_pdpt_adress = first_entry & ~0xFFF;

    if(first_pdpt_adress==0 || (first_pdpt_adress % allign_const) != 0){
        return 0;
    }


    uintptr_t first_pdpt_entry =  * (uintptr_t *) first_pdpt_adress;

    if((first_pdpt_entry == 0) || (first_pdpt_entry & 1) == 0){

        return 0;
    }


    //parse the asm pointer to a global pointer only after validation passes
    toptable = pgtables;
    adress_toptable = (uintptr_t)toptable;

    is_initialized = true;



    return 1;



}


//this struct is the output of the walker funciton
typedef struct walker_out{

    bool success;
    uintptr_t *final_entry_pointer;
    uint64_t final_page_level;
    uintptr_t physical_base_adress;
    uint64_t page_size;
    
}walker_out;

//we will treat these as trasntion private functiuons the main puropuse is a phase 1 vmm which switches from the old 
// bootstrap tables to new ones, them some of these functions might be updated or even reimplemnbted
//IMPORTANT: i will comment the part when the transtion is complete,there will be a clear ass comment saying 
//(THE TRANSITION IS DONE).
//bcz of identity mapping we can treat physical adresses as virtuial adresses but when we switch from identity map
//we cant do that anymore some logic needs to be chamged idfk what yet atp i jsutw ant to kill myself
//idfk why im doing this to myslef maybe i hate myself idfk.
//page walker, this is a helper function that itterates overpages it follows a virtuyal adress 
//and itterates over page levels adn checks wether its mapped and were its mapped and at what level is the mapping at
 static walker_out the_walking_pager(uintptr_t *root_table, uintptr_t virtual_adress){


    walker_out result = {0};




    if(root_table == NULL){

        return result;
    }

    //this is an index in the table size meaning from 0 to 511
    //every 9th bit in the virtual adress gives you the index in the table which it refers to
    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;

    uintptr_t *curr = root_table;

    

    uintptr_t pml4_entry = curr[index_in_pml4];
    uintptr_t *pml4_entry_adress = &curr[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        return result;
    }


    uintptr_t pdpt_adress = pml4_entry & ~0xFFF;

    curr = (uintptr_t *)pdpt_adress;

    uintptr_t pdpt_entry = curr[index_in_pdpt];
    uintptr_t *pdpt_entry_adress = &curr[index_in_pdpt];

    if(((pdpt_entry & 1) == 0) || pdpt_entry == 0){

        return result;
    }



    uint64_t pdpt_page_size_bit = (pdpt_entry & (1ULL << 7));

    if(pdpt_page_size_bit != 0){

        result.success = true;
        result.final_entry_pointer = pdpt_entry_adress;
        result.final_page_level = 2;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 1048576; //1gib in kib
        uintptr_t physical_base = final_entry_value & ~((1ULL << 30) - 1);
        result.physical_base_adress = physical_base;

        return result;

    }
    

    uintptr_t pd_adress = pdpt_entry & ~0xFFF;


    curr = (uintptr_t *)pd_adress;

    uintptr_t pd_entry = curr[index_in_pd];
    uintptr_t *pd_entry_adress = &curr[index_in_pd];
    

    if(((pd_entry & 1) == 0) || pd_entry == 0){

        return result;
    }

     uint64_t pd_page_size_bit = (pd_entry & (1ULL << 7));


      if(pd_page_size_bit != 0){

        result.success = true;
        result.final_entry_pointer = pd_entry_adress;
        result.final_page_level = 3;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 2048; //2mib in kib
        uintptr_t physical_base = final_entry_value & ~((1ULL << 21) - 1);
        result.physical_base_adress = physical_base;

                return result;

    }

    uintptr_t pt_adress = pd_entry & ~0xFFF;

    curr = (uintptr_t *)pt_adress;

    uintptr_t pt_entry = curr[index_in_pt];
    uintptr_t *pt_entry_adress = &curr[index_in_pt];

    if(((pt_entry & 1) == 0) || pt_entry == 0){

                return result;
    }


   
        result.success = true;
        result.final_entry_pointer = pt_entry_adress;
        result.final_page_level = 4;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 4; 
        uintptr_t physical_base = final_entry_value & ~((1ULL << 12) - 1);
        result.physical_base_adress = physical_base;

    return result;

 }


//this function returns the exact physical adress to a virtual adress not a range or a frame, THE EXACT ONE
uintptr_t vmm_query_page(uintptr_t virtual_adress){

    if(is_initialized == false){
        return 0;
    }

    walker_out walker_result = the_walking_pager(toptable, virtual_adress);

    if(walker_result.success == 0){
        return 0;
    }

    uintptr_t physical_base_adress = walker_result.physical_base_adress;
    uint64_t final_page_level = walker_result.final_page_level;

    if(final_page_level == 2){

        uint64_t offset = virtual_adress & ((1ULL << 30) - 1);
        uintptr_t exact_physical_adress = offset + physical_base_adress;

        return exact_physical_adress;

    }

    if(final_page_level == 3){

        uint64_t offset = virtual_adress & ((1ULL << 21) - 1);
        uintptr_t exact_physical_adress = offset + physical_base_adress;

        return exact_physical_adress;


    }


    if(final_page_level == 4){

        uint64_t offset = virtual_adress & ((1ULL << 12) - 1);
        uintptr_t exact_physical_adress = offset + physical_base_adress;

        return exact_physical_adress;
    }


    return 0;


}


typedef struct free_page_frame_out{

    bool success;
    uintptr_t *pgtable_pointer;
    uintptr_t physical_adress;


}free_page_frame_out;

// since any new page created isnt a part of the kernel image so we will have to reserve frames for it in the pmm
// they are created at runtime so they arent marked as used so we ned to do that 


//Right now this works because your low 4 GiB is identity-mapped.
//Later, if that stops being true, this helper will need a different way to get a usable virtual 
//pointer to the new table page.
//So the exact thing that changes is:

//the physical address returned by PMM stays the same kind of value
//but the usable C pointer to touch that page must come from a physical-to-virtual translation step

static free_page_frame_out free_frame_for_new_page_structure_and_create_it(void){

    free_page_frame_out result = {0};

    if(is_initialized == false){

        return result;
    }

    uintptr_t free_frame = pmm_alloc_frame();

    if(free_frame == 0){

        return result;
    }

    result.physical_adress = free_frame;
    uintptr_t *free_frame_ptr = (uintptr_t *) free_frame;
    result.pgtable_pointer = free_frame_ptr;
    size_t byte_counter = 0;
    uint8_t *free_frame_byte_ptr = (uint8_t *)free_frame_ptr;

    while(byte_counter < 4096){
       
        *free_frame_byte_ptr = 0;

        free_frame_byte_ptr++;
        byte_counter++;

    }

    result.success = true;

    return result;

   

}


//TRANSITIONAL CODE
static free_page_frame_out vmm_create_root_table(void){

    free_page_frame_out page_metadata = {0};

    if(is_initialized == false){return page_metadata;}

    page_metadata = free_frame_for_new_page_structure_and_create_it();

    if(page_metadata.success == false){return page_metadata;}

    return page_metadata;


}


static uint64_t populate2m_into_table(uintptr_t *root_table, uintptr_t virtual_adress, uintptr_t physical_adress, uint64_t flags){

    if(is_initialized == false){return 0;}

    if(root_table == NULL){return 0;}

    if(virtual_adress % 2097152 != 0){return 0;}

    if(physical_adress % 2097152 != 0){return 0;}

    //add if not alligned align them.

    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t table_flags = (1ULL << 0) | (1ULL << 1);
    uintptr_t pml4_entry = root_table[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        free_page_frame_out new_pdpt_table_metadata = free_frame_for_new_page_structure_and_create_it();
        if(new_pdpt_table_metadata.success == false){return 0;}
        //u have to write to the actual table pointer not to the local variable this is very important.
        root_table[index_in_pml4] = new_pdpt_table_metadata.physical_adress | table_flags;
        pml4_entry = root_table[index_in_pml4];
        root_table = new_pdpt_table_metadata.pgtable_pointer;
    }else{

    root_table = (uintptr_t *)(pml4_entry & ~0xFFF);    }
    uintptr_t pdpt_entry = root_table[index_in_pdpt];
    if((pdpt_entry & 1) == 0 || pdpt_entry == 0){

        free_page_frame_out new_pd_table_metadata = free_frame_for_new_page_structure_and_create_it();
        if(new_pd_table_metadata.success == false){return 0;}
        root_table[index_in_pdpt] = new_pd_table_metadata.physical_adress | table_flags;
        pdpt_entry = root_table[index_in_pdpt];
        root_table = new_pd_table_metadata.pgtable_pointer;


    }else{

    if((pdpt_entry & (1ULL << 7)) != 0){return 0;} //if this pdpt entry is a 1gib large page we cannot descend into a pd from it here
    root_table = (uintptr_t *)(pdpt_entry & ~0xFFF); //strip the flags from the existing pdpt entry so root_table now points to the real pd page
    }

    uint64_t pdpt_page_size_bit = (pdpt_entry & (1ULL << 7));


    if(pdpt_page_size_bit != 0){
        return 0;
    }


    uintptr_t pd_entry = root_table[index_in_pd];

    if((pd_entry & 1) != 0 && pd_entry != 0){ //if a pd entry already exists here do not overwrite the mapping

        return 0;
    }

    root_table[index_in_pd] = physical_adress | flags | (1ULL << 0) | (1ULL << 7); //write the final 2mib pd entry using the physical base plus present plus large page bit

    return 1; //the full 2mib population path succeeded

    

}


static uint64_t populate4k_into_table(uintptr_t *root_table, uintptr_t virtual_adress, uintptr_t physical_adress, uint64_t flags ){

    if(is_initialized == false){return 0;}

    if(root_table == NULL){return 0;}

    if(virtual_adress % 4096 != 0){return 0;}

    if(physical_adress % 4096 != 0){return 0;}

    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF; //this is the pt index because the final 4k leaf lives in the pt level
    uint64_t table_flags = (1ULL << 0) | (1ULL << 1);

    uintptr_t pml4_entry = root_table[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        free_page_frame_out pdpt_metadata = free_frame_for_new_page_structure_and_create_it();
        if(pdpt_metadata.success == false){return 0;}
        root_table[index_in_pml4] = pdpt_metadata.physical_adress | table_flags;
        pml4_entry = root_table[index_in_pml4];
        root_table = pdpt_metadata.pgtable_pointer;





    }else{
        root_table = (uintptr_t *)(pml4_entry & ~0xFFF);

    }


    uintptr_t pdpt_entry = root_table[index_in_pdpt];

    if((pdpt_entry & 1) == 0 || pdpt_entry == 0){

        free_page_frame_out pd_metadata = free_frame_for_new_page_structure_and_create_it(); //allocate a pd page if the pdpt slot is empty
        if(pd_metadata.success == false){return 0;} //stop if the new pd page could not be created
        root_table[index_in_pdpt] = pd_metadata.physical_adress | table_flags; //link the new pd page into the real pdpt entry with table flags
        pdpt_entry = root_table[index_in_pdpt]; //refresh the local copy so it matches the real pdpt entry we just wrote
        root_table = pd_metadata.pgtable_pointer; //descend into the new pd page so the next level can be filled




    }else{
        if((pdpt_entry & (1ULL << 7)) != 0){return 0;} //if this pdpt entry is a 1gib large page we cannot descend into a pd from it here
        root_table = (uintptr_t *)(pdpt_entry & ~0xFFF); //strip the flags from the existing pdpt entry so root_table now points to the real pd page

    }

    uintptr_t pd_entry = root_table[index_in_pd]; //read the pd entry for this virtual address from the current pd page

    if((pd_entry & 1) == 0 || pd_entry == 0){

        free_page_frame_out pt_metadata = free_frame_for_new_page_structure_and_create_it(); //allocate a pt page if the pd slot is empty
        if(pt_metadata.success == false){return 0;} //stop if the new pt page could not be created
        root_table[index_in_pd] = pt_metadata.physical_adress | table_flags; //link the new pt page into the real pd entry with table flags
        pd_entry = root_table[index_in_pd]; //refresh the local copy so it matches the real pd entry we just wrote
        root_table = pt_metadata.pgtable_pointer; //descend into the new pt page so the final 4k leaf can be written




    }else{
        if((pd_entry & (1ULL << 7)) != 0){return 0;} //if this pd entry is a 2mib large page we cannot descend into a pt from it here
        root_table = (uintptr_t *)(pd_entry & ~0xFFF); //strip the flags from the existing pd entry so root_table now points to the real pt page

    }

    uintptr_t pt_entry = root_table[index_in_pt]; //read the final pt entry that will hold the 4k mapping

    if((pt_entry & 1) != 0 && pt_entry != 0){return 0;} //if a pt entry already exists here do not overwrite the mapping

    root_table[index_in_pt] = physical_adress | flags | (1ULL << 0); //write the final 4k pt entry using the physical base plus the present bit

    return 1; //the full 4k population path succeeded

}

