#include "vmm.h"
#include "serial.h"
#include "pmm.h"


//redundant but looks cool
typedef uint8_t bool;
#define true 1
#define false 0
#define LOW_BASE 0x100000
#define HIGHER_HALF_BASE 0xFFFFFFFF80000000
uint64_t MAX_PHYS;

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];


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
    MAX_PHYS = pmm_total_map_bytes();
    


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
    uintptr_t *final_page_walked_ptr;
    bool table_here;
    uint64_t error_code; //10 for present bit = 0
    
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
//add flag checks

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

//need to add old flag preservation
static uint64_t copy_mapping_dynamic(uintptr_t *src_table, uintptr_t src_va, uintptr_t *dest_table, uintptr_t dest_va){

    if(is_initialized == false){return 0;}
    if(src_table == NULL){return 0;}
    if(dest_table == NULL){return 0;}

    walker_out walker_out = the_walking_pager(src_table, src_va);

    if(walker_out.success == false || walker_out.final_entry_pointer == NULL){return 0;}

    uint64_t final_walker_out_lvl = walker_out.final_page_level;

    uintptr_t source_final_entry_val = * walker_out.final_entry_pointer;

    //extract source flags
    uintptr_t source_phys = walker_out.physical_base_adress; 
    uint64_t src_flags = source_final_entry_val & 0xFFF;
    

    if(final_walker_out_lvl == 3){

        if(populate2m_into_table(dest_table, dest_va, source_phys, src_flags) == 0 ){return 0;}

        


        
    }else if(final_walker_out_lvl == 4){

        if(populate4k_into_table(dest_table, dest_va, source_phys, src_flags) == 0){return 0;}

        
        

        

    }else{

        return 0;
    }



    return 1;

}


//END EXCLUSIVE

static uint64_t copy_mapping_dynamic_over_range(
    uintptr_t *src_table, 
    uintptr_t src_start_va,
    uintptr_t src_end_va,
    uintptr_t *dest_table, 
    uintptr_t dest_start_va){


        if(is_initialized == false){return 0;}
        if(src_table == NULL){return 0;}
        if(dest_table == NULL){return 0;}

        if(src_start_va > src_end_va){return 0;}


        uintptr_t curr_src_va = src_start_va;
        uintptr_t curr_dest_va = dest_start_va;


        while(curr_src_va < src_end_va){
            walker_out page_metadata = the_walking_pager(src_table, curr_src_va);
            if(page_metadata.success == false){return 0;}
            uintptr_t step_size;

            uint64_t stop_lvl = page_metadata.final_page_level;

            //this check is mainly to move tthe VA in an alligned manor(how much we increment the VA per itter), sorry i dont know how to put into better words
            if(stop_lvl == 3){

                step_size = 2097152; //2mib in bytes

            }else if(stop_lvl == 4){

                step_size = 4096; //4kib in bytes

            }else{return 0;}

            if(src_end_va - curr_src_va < step_size){return 0;}
            if(step_size == 2097152){

                if(curr_src_va % 2097152 != 0){
                    return 0;
                }

                if(curr_dest_va % 2097152 != 0){

                    return 0;
                }

                if(copy_mapping_dynamic(src_table, curr_src_va, dest_table, curr_dest_va) == 0){
                    return 0;
                }

                if(curr_src_va + step_size < curr_src_va){return 0;}
                if(curr_dest_va + step_size < curr_dest_va){return 0;}

                curr_src_va += step_size;
                curr_dest_va += step_size;


            }else if(step_size == 4096){
                if(curr_src_va % 4096 != 0){
                    return 0;
                }

                if(curr_dest_va % 4096 != 0){return 0;}

                if(copy_mapping_dynamic(src_table, curr_src_va, dest_table, curr_dest_va) == 0){
                    return 0;
                }

                if(curr_src_va + step_size < curr_src_va){return 0;}
                if(curr_dest_va + step_size < curr_dest_va){return 0;}

                curr_src_va += step_size;
                curr_dest_va += step_size;



            }else{return 0;}

        }



        return 1;


}



static uintptr_t cr3_switch_t(uintptr_t new_root_table_phys){

    if(new_root_table_phys % 4096 != 0){return 0;}

    uintptr_t old_table_phys;


    //this part will need to be wrapped if we are going to port to rspi
    asm volatile(

        "mov %%cr3, %0\n\t"
        "mov %1, %%cr3\n\t"
        :"=r"(old_table_phys)
        :"r"(new_root_table_phys)
        :"memory"

    );

return old_table_phys;

}



uint64_t transition(uintptr_t *old_root){

    

free_page_frame_out page_metdata = vmm_create_root_table();
if(page_metdata.success == false){return 0;}
uintptr_t *new_root = page_metdata.pgtable_pointer;
uintptr_t new_root_phys = page_metdata.physical_adress;

uintptr_t src_start_va =(uintptr_t)& _kernel_start;
uintptr_t src_end_va = (uintptr_t)&_kernel_end;

uintptr_t dest_va_start = HIGHER_HALF_BASE + (_kernel_start - LOW_BASE);


if (!copy_mapping_dynamic_over_range(old_root, src_start_va, src_end_va, new_root, dest_va_start)) return 0;

cr3_switch_t(new_root_phys);

toptable = new_root;
adress_toptable = new_root_phys;

return 1;


}


//TRANSITION IS DOEN

//if u still havent figured im not the best at spelling
//now with page based virtual mem mangagment we are only concerned with alloc free, map unmap pages, which is pretty simple



//this fucntion looks very unsafe idfk
static uintptr_t phys_to_virt(uintptr_t physical_adress){

    if(physical_adress == 0){return 0;}
    if (physical_adress < LOW_BASE || physical_adress >= MAX_PHYS){return 0;}
    
    uintptr_t va = physical_adress + (HIGHER_HALF_BASE - LOW_BASE);

    return va;


}

enum walker_error{

    ENTRY_NOT_PRESENT = 10,


};



static walker_out the_walking_pager2(uintptr_t *root_table, uintptr_t virtual_adress){

    //result.sucess = TRUE means we hit a large page this is important for later REALLY IMPORTANT.....

    walker_out result = {0};

    if(root_table == NULL){return result;}
    if(virtual_adress == 0){return result;}

    
    //this is an index in the table size meaning from 0 to 511
    //every 9th bit in the virtual adress gives you the index in the table which it refers to
    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;


    
    

    uintptr_t *curr = (uintptr_t *)phys_to_virt((uintptr_t)root_table);
    if(curr == NULL){return result;}

    

    uintptr_t pml4_entry = curr[index_in_pml4];
    uintptr_t *pml4_entry_adress = &curr[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){


        return result;
    }


    uintptr_t pdpt_adress = phys_to_virt((uintptr_t)(pml4_entry & ~0xFFF));

    curr = (uintptr_t *)pdpt_adress;
    if(curr == NULL){return result;}

    uintptr_t pdpt_entry = curr[index_in_pdpt];
    uintptr_t *pdpt_entry_adress = &curr[index_in_pdpt];

    if(((pdpt_entry & 1) == 0) || pdpt_entry == 0){
        result.error_code = ENTRY_NOT_PRESENT;
        result.final_page_level = 2;
        result.final_page_walked_ptr = curr;
        result.final_entry_pointer = pdpt_entry_adress;
        result.physical_base_adress = NULL;
        result.page_size = NULL;
        result.success = false;
        

        result.table_here = false;

        return result;
    }



    uint64_t pdpt_page_size_bit = (pdpt_entry & (1ULL << 7));

    if(pdpt_page_size_bit != 0){

        result.success = true;
        result.final_entry_pointer = pdpt_entry_adress;
        result.final_page_level = 2;
        result.final_page_walked_ptr = curr;
        result.table_here = true;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 1048576; //1gib in kib
        uintptr_t physical_base = final_entry_value & ~((1ULL << 30) - 1);
        result.physical_base_adress = physical_base;

        return result;

    }
    

    uintptr_t pd_adress = phys_to_virt((uintptr_t)(pdpt_entry & ~0xFFF)); 


    curr = (uintptr_t *)pd_adress;
    if(curr == NULL){return result;}

    uintptr_t pd_entry = curr[index_in_pd];
    uintptr_t *pd_entry_adress = &curr[index_in_pd];
    

    if(((pd_entry & 1) == 0) || pd_entry == 0){
        result.error_code = ENTRY_NOT_PRESENT;
        result.final_page_level = 3;
        result.final_page_walked_ptr = curr;
        result.final_entry_pointer = pd_entry_adress;
        result.physical_base_adress = NULL;
        result.page_size = NULL;
        result.success = false;
        

        result.table_here = false;

        return result;
    }

     uint64_t pd_page_size_bit = (pd_entry & (1ULL << 7));


      if(pd_page_size_bit != 0){

        result.success = true;
        result.final_entry_pointer = pd_entry_adress;
        result.final_page_level = 3;
        result.final_page_walked_ptr = curr;
        result.table_here = true;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 2048; //2mib in kib
        uintptr_t physical_base = final_entry_value & ~((1ULL << 21) - 1);
        result.physical_base_adress = physical_base;

                return result;

    }

    uintptr_t pt_adress = phys_to_virt((uintptr_t)(pd_entry & ~0xFFF)); 

    curr = (uintptr_t *)pt_adress;
    if(curr == NULL){return result;}

    uintptr_t pt_entry = curr[index_in_pt];
    uintptr_t *pt_entry_adress = &curr[index_in_pt];

    if(((pt_entry & 1) == 0) || pt_entry == 0){
        result.error_code = ENTRY_NOT_PRESENT;
        result.final_page_level = 4;
        result.final_page_walked_ptr = curr;
        result.final_entry_pointer = pt_entry_adress;
        result.physical_base_adress = NULL;
        result.page_size = NULL;
        result.success = false;
        

        result.table_here = false;

                return result;
    }


   
        result.success = true;
        result.final_entry_pointer = pt_entry_adress;
        result.final_page_level = 4;
        result.final_page_walked_ptr = curr;
        result.table_here = true;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 4; 
        uintptr_t physical_base = final_entry_value & ~((1ULL << 12) - 1);
        result.physical_base_adress = physical_base;

    return result;






}

//add is alligned helpers, and allign helpers..


static free_page_frame_out free_frame_for_new_page_structure_and_create_it2(void){

    free_page_frame_out result = {0};

    if(is_initialized == false){

        return result;
    }

    uintptr_t free_frame = pmm_alloc_frame();

    if(free_frame == 0){

        return result;
    }

    result.physical_adress = free_frame;
    uintptr_t *free_frame_ptr = (uintptr_t *) phys_to_virt(free_frame);
    if (free_frame_ptr == NULL){
        pmm_free_frame(free_frame);
        return result;
    }
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

//this function will need alot of revision and might explode later
// i think its kinda fixed rn //// shouyld add more allignmnt checka and null checks, and also return a struct
// okay fuck this im redoing the funciton


//this function will have to be overhauled againadd a aparam desired map lvl to split to
static uint64_t spilt_page(uintptr_t *root_table, uintptr_t virtual_adress){

    if(root_table == NULL){return 0;}
    if(virtual_adress == 0){return 0;}

    // root_table is a physical pointer; convert to virtual only when dereferencing

    walker_out walker_result = the_walking_pager2(root_table, virtual_adress);
    if(walker_result.success == false){return 0;}

    // Already split to 4KiB
    if(walker_result.final_page_level == 4){return 11;}

    // Split 2MiB -> 4KiB (already implemented)
    if(walker_result.final_page_level == 3){
        // 2MiB = 2097152 bytes
        if(virtual_adress % 2097152 != 0 ){return 0;}
        // walker_result.final_entry_pointer is a virtual pointer (from the_walking_pager2)
        uintptr_t original_entry = *(walker_result.final_entry_pointer);
        uintptr_t original_entry_nf = original_entry & ~0xFFF;
        uintptr_t original_entry_flags = original_entry & 0xFFF;
        free_page_frame_out new_page = free_frame_for_new_page_structure_and_create_it2();
        if(new_page.success == false){return 0;}
        uintptr_t *new_page_vptr = new_page.pgtable_pointer; // new_page.pgtable_pointer is a virtual pointer
        for(size_t i = 0; i < 512; i++){
            uintptr_t subpage_phys = original_entry_nf + (i * 0x1000);
            // Each entry: physical address of 4KiB subpage, with correct flags (large bit cleared)
            new_page_vptr[i] = subpage_phys | (original_entry_flags & ~(1ULL << 7));
        }
        // Update parent PD entry to point to new PT (physical address, flags, large bit cleared)
        // walker_result.final_entry_pointer is a virtual pointer, so this is correct
        *(walker_result.final_entry_pointer) = new_page.physical_adress | (original_entry_flags & ~(1ULL << 7));
        // Update walker_out to point to the new 4KiB entry for this VA
        uintptr_t *new_pt_vptr = (uintptr_t *)phys_to_virt(new_page.physical_adress);

        if (new_pt_vptr == NULL) { 
        pmm_free_frame(new_page.physical_adress); 
        return 0;} // new_page.physical_adress is a physical address


        uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;
        walker_result.final_entry_pointer = &new_pt_vptr[index_in_pt];
        walker_result.final_page_level = 4;
        walker_result.page_size = 4096;
        walker_result.physical_base_adress = (original_entry_nf + (index_in_pt * 0x1000));
        return 1;
    }

    // Split 1GiB -> 2MiB (and recursively to 4KiB if needed)
    if(walker_result.final_page_level == 2){
        // 1GiB = 1073741824 bytes, 2MiB = 2097152 bytes
        if(virtual_adress % 1073741824 != 0 ){return 0;}
        uintptr_t original_entry = *(walker_result.final_entry_pointer);
        uintptr_t original_entry_nf = original_entry & ~0xFFF;
        uintptr_t original_entry_flags = original_entry & 0xFFF;
        // Allocate a new PD (page directory) for 2MiB entries
        free_page_frame_out new_pd = free_frame_for_new_page_structure_and_create_it2();
        if(new_pd.success == false){return 0;}
        uintptr_t *new_pd_vptr = new_pd.pgtable_pointer;
        // Fill the new PD with 512 2MiB entries, mapping the 1GiB region
        for(size_t i = 0; i < 512; i++){
            uintptr_t subpage_phys = original_entry_nf + (i * 0x200000); // 2MiB per entry
            // Each entry: physical address of 2MiB subpage, with correct flags (large bit set)
            new_pd_vptr[i] = subpage_phys | (original_entry_flags | (1ULL << 7));
        }
        // Update parent PDPT entry to point to new PD (physical address, flags, large bit cleared)
        *(walker_result.final_entry_pointer) = new_pd.physical_adress | (original_entry_flags & ~(1ULL << 7));

        // Now, check if the relevant 2MiB entry is a large page (bit 7 set)
        uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
        uintptr_t *target_pd_entry = &new_pd_vptr[index_in_pd];
        uintptr_t pd_entry_val = *target_pd_entry;
        // Update walker_result to reflect the new 2MiB mapping
        walker_result.final_entry_pointer = target_pd_entry;
        walker_result.final_page_level = 3;
        walker_result.page_size = 2097152;
        walker_result.physical_base_adress = (original_entry_nf + (index_in_pd * 0x200000));

        if((pd_entry_val & (1ULL << 7)) != 0) {
            // The 2MiB entry is a large page, so recursively split it to 4KiB
            // Alignment check for 2MiB
            if((virtual_adress % 2097152) != 0) { return 0; }
            spilt_page((uintptr_t *)new_pd.physical_adress, virtual_adress);
            // After recursion, re-walk to update walker_result to the new 4KiB entry
            walker_out new_walker = the_walking_pager2(root_table, virtual_adress);
            if (!new_walker.success) { return 0; }
            walker_result = new_walker;
        }
        return 1;
    }

    return 0;

}







typedef struct page_structure{

    bool success;
    uintptr_t v_final_entry_ptr;
    uint64_t final_page_lvl;
    uintptr_t final_table_adress;
    uintptr_t v_final_table_adress;
    uint64_t error_code;


    
}page_structure;


// NOTE TO SELF:
// recheck the conditions, explicitly check for the diff between the present bit conditions and the large page conditions
// this might cause very silent bugs later.
// we mauy need to refactor this function to use walkers detailed outputs.

page_structure create_page_struct_dynamic(
    uintptr_t *root_table, 
    uint64_t map_lvl_desired, 
    uintptr_t virtual_adress, 
    uint64_t flags){

        page_structure result = {0};

        if(root_table == NULL){return result;}
        if(virtual_adress % map_lvl_desired != 0){return result;}


        uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
        uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
        uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
        uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;

        walker_out walker_result = the_walking_pager2(root_table, virtual_adress);
        //add check for large paeg bit
        if((walker_result.final_page_level != map_lvl_desired)
         && (walker_result.final_page_level < map_lvl_desired) 
         && (walker_result.success == true)){
             
            if(!spilt_page(root_table, virtual_adress)){return result;}
            result.final_page_lvl = map_lvl_desired;
            result.success = true;

            return result;


        }

        if((walker_result.table_here = false) && (walker_result.final_page_level != map_lvl_desired) && (walker_result.error_code == 10)){

            while((walker_result.error_code == ENTRY_NOT_PRESENT || walker_result.final_page_level != map_lvl_desired)){
                //uint64_t index;

                //if(walker_result.final_page_level == 2){
                    //index = index_in_pdpt;
                    
                //}else if(walker_result.final_page_level == 3){
                    //index = index_in_pd;
                
                //}else if(walker_result.final_page_level == 4){
                    //index = index_in_pt;
                //}

            free_page_frame_out new_page = free_frame_for_new_page_structure_and_create_it2();
            if (!new_page.success) return result;

            //walker_result.final_page_walked_ptr[index] = new_page.physical_adress | flags; this also works

            *walker_result.final_entry_pointer = new_page.physical_adress | flags;//this fucntion is no complete yet

            walker_result = the_walking_pager2(root_table, virtual_adress);

            }


        }

        



        

        

}

enum map_error_code{

    MAPPING_EXISTS_NORMAL = 10,
    MAPPING_EXISTS_NP_ENTRY = 11, // NP means not present
    UNDEFINED_ERROR = 69696969, // this should never happen (should haha)
    MAP_STRUCTURE_INCOMPLETE, // this is for when we create a new structure but the there is an 
                            // error with with the structure creator
    COELSENCE,


};

typedef struct map_out{
   bool success;
   uint64_t error_code;
   uintptr_t v_final_entry_ptr;
   uintptr_t physical_adress;


}map_out;

//also expand the splitter to take arbitrary split sizes (desired split lvl)
//make sure already mapped pages are handled correctly
//the split page behaviour will be added later allthough the helper exisits i dont belive its a 100% correct
// since i dont understand it very well
// now i will add the helpers since i spent 2 days fixing them
map_out vmm_map_page(
    uintptr_t virtual_adress,
    uintptr_t physical_adress, 
    uintptr_t *root_table, 
    uint64_t desired_map_lvl, // or mapping lvl desired
    uint64_t flags
    
                     ){

        map_out result = {0};

        if(root_table == NULL){return result;}

        uint64_t mapping_size;

        if(desired_map_lvl == 2){
            mapping_size = 1073741824;
        }else if(desired_map_lvl == 3){
            mapping_size = 2097152;
        }else if(desired_map_lvl == 4){
            mapping_size = 4096;
        }else{return result;}

        //add more allignmnen checks

        if(virtual_adress % mapping_size != 0 || physical_adress % mapping_size != 0){return result;}
        if(desired_map_lvl == 0){return result;}// u have to fix 5his
        if(flags == 0){return result;}// aslo this

        

        


        //first we will walk the table and find the mapping lvl we are at

        walker_out walker_result = the_walking_pager2(root_table, virtual_adress);
        if(walker_result.success == false && walker_result.error_code != ENTRY_NOT_PRESENT){return result;}


        uint64_t walk_stop_lvl = walker_result.final_page_level;

        if(walk_stop_lvl == desired_map_lvl){

            if(walker_result.success && walker_result.table_here){
            //this block is for the normal fail case, the mapping existis at the desired level no ifs or buts
                result.success = false;
                result.error_code = MAPPING_EXISTS_NORMAL;
                result.physical_adress = walker_result.physical_base_adress;
                result.v_final_entry_ptr = walker_result.final_entry_pointer;
                return result;

            }

            if(!walker_result.success
                && !walker_result.table_here
                && walker_result.error_code == ENTRY_NOT_PRESENT){

                    //this isnt  afail case bcz we have the exact entry to map at but its not present


                *walker_result.final_entry_pointer = physical_adress | flags; // dont froget to set the present bit flag


                result.success = true;
                
                result.physical_adress = physical_adress;
                result.v_final_entry_ptr = walker_result.final_entry_pointer;
                return result;   





            }

            
                // last case were we have the desired map lvl, but its not mapped yet. we stopped the walk at the correct lvl
                // but no mapping exists(this is only a note we dont need anotehr case)
            

            result.error_code = UNDEFINED_ERROR;
            return result;

        
            
        }


        if(walk_stop_lvl < desired_map_lvl){

            if(walker_result.success && walker_result.table_here){
            //this block is for the normal case, the mapping deosnt exist at the desired lvl and we need to split
            //large page bit (bit 7) is 1 

                if(!spilt_page(root_table, virtual_adress)){return result;}

                    walker_out new_walk = the_walking_pager2(root_table, virtual_adress);


                *new_walk.final_entry_pointer = physical_adress | flags;

                new_walk.physical_base_adress = physical_adress;

                result.physical_adress = new_walk.physical_base_adress;

                result.v_final_entry_ptr = new_walk.final_entry_pointer;
                

                result.success = true;

                return result;

            }

            if(!walker_result.success
                && !walker_result.table_here
                && walker_result.error_code == ENTRY_NOT_PRESENT){

                //this is the more chaotic case were the entry is not present, we create a new structure to the desired 
                //lvl and map it
                page_structure new_page_structure = create_page_struct_dynamic(root_table, desired_map_lvl, virtual_adress, flags);
                if(!new_page_structure.success){return result;}

                walker_out new_walk = the_walking_pager2(root_table, virtual_adress);
                if(new_walk.final_page_level != desired_map_lvl){
                    result.error_code = MAP_STRUCTURE_INCOMPLETE;
                    result.success = false;
                    return result;
                }
                

                *new_walk.final_entry_pointer = physical_adress | flags;

                new_walk.physical_base_adress = physical_adress;

                result.physical_adress = new_walk.physical_base_adress;

                result.v_final_entry_ptr = new_walk.final_entry_pointer;
                

                result.success = true;

                return result;

            }



        }


        if(walk_stop_lvl > desired_map_lvl){

            result.error_code = COELSENCE;

            return result;
        }

            







    result.error_code = UNDEFINED_ERROR;
            return result;








}

enum unmap_error{

    UNMAPPING_AT_PML4 = 10,

}unmap_error;

typedef struct unmap_out{
    bool success;
    uintptr_t unmapped_pa;
    uintptr_t *v_unmapped_pa_ptr;
    uintptr_t unmapped_va;
    uint64_t error_code;

}unmap_out;


unmap_out vmm_unmap_page(uintptr_t *root_table, uintptr_t virtual_adress){

    unmap_out result = {0};
    if(root_table == NULL){return result;}

    walker_out walker_result = the_walking_pager2(root_table, virtual_adress);
    if(!walker_result.success){return result;}

    if(walker_result.final_page_level == 1 && walker_result.success){// this case will never happen unless we have a major bug

        
        result.success = false;
        result.error_code = UNMAPPING_AT_PML4;
        return result;

    }
    
    if(walker_result.success){
    result.v_unmapped_pa_ptr = walker_result.final_entry_pointer;
    result.unmapped_pa = *walker_result.final_entry_pointer;
    result.unmapped_va = virtual_adress;
    result.success = true;
    *walker_result.final_entry_pointer = NULL;
    }

    

}

typedef struct map_region_out{
    
}map_region_out;


map_region_out vmm_map_region(){



}
