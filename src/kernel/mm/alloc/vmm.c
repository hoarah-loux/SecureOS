#include "vmm.h"

//redundant but looks cool
#define LOW_BASE 0x100000

//all these are virtual addrs

#define PHYS_MAP_BASE 0xFFFF800000000000ULL //slot 256 in the pml4
#define HIGHER_HALF_BASE_ 0xFFFF800000000000ULL // slot 256 in the pml4
#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000 //slot 511 in the pml4
uint64_t MAX_PHYS; //first 4gib
uint64_t BUDDY_MAX_PHYS;// all ram


extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];


//this is the local pointer that will hold the lable
static uintptr_t *toptable;
static uintptr_t adress_toptable;
static bool is_initialized = false;


//VERY IMPORTANT:
// after the transition is done all inputs are physical and outputs are virtual.....





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
    MAX_PHYS = 0x100000000UL;
    BUDDY_MAX_PHYS = pmm_max_phys_addr();//update this later
    


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

static uintptr_t align_up_u(uintptr_t value, uintptr_t alignment){
    if(alignment == 0){return value;}

    uintptr_t mask = alignment - 1;
    if((alignment & mask) != 0){return value;}
    if(value > (UINTPTR_MAX - mask)){return 0;}

    return (value + mask) & ~mask;
}

static uintptr_t align_down_u(uintptr_t value, uintptr_t alignment){
    if(alignment == 0){return value;}

    uintptr_t mask = alignment - 1;
    if((alignment & mask) != 0){return value;}

    return value & ~mask;
}

static size_t align_size_up(size_t value, size_t alignment){
    if(alignment == 0){return value;}

    size_t mask = alignment - 1;
    if((alignment & mask) != 0){return value;}
    if(value > (SIZE_MAX - mask)){return 0;}

    return (value + mask) & ~mask;
}




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

    result.pml4_entry = pml4_entry;

    uintptr_t pdpt_adress = pml4_entry & ~0xFFF;

    curr = (uintptr_t *)pdpt_adress;

    uintptr_t pdpt_entry = curr[index_in_pdpt];
    uintptr_t *pdpt_entry_adress = &curr[index_in_pdpt];
    result.pdpt_entry = pdpt_entry;

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
    result.pd_entry = pd_entry;
    

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
    result.pt_entry = pt_entry;

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




// since any new page created isnt a part of the kernel image so we will have to reserve frames for it in the pmm
// they are created at runtime so they arent marked as used so we ned to do that 


//Right now this works because your low 4 GiB is identity-mapped.
//Later, if that stops being true, this helper will need a different way to get a usable virtual 
//pointer to the new table page.
//So the exact thing that changes is:

//the physical address returned by PMM stays the same kind of value
//but the usable C pointer to touch that page must come from a physical-to-virtual translation step


//VERY IMPORTANT:::::::
//before transition the bitmap is used to get usbale pointers bcz the bitmap is
//capped to 4gib, after the transition we use the buddy normally bcz its not capped.
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

static uint64_t strip_flags_pt(uintptr_t entry) {
const uint64_t ADDR_MASK_4K = 0x000FFFFFFFFFF000ULL;
return (uint64_t)entry & ~ADDR_MASK_4K;
}

static uint64_t strip_flags_non_pt(uintptr_t entry) {
const uint64_t ADDR_MASK_4K = 0x000FFFFFFFFFF000ULL;
const uint64_t BIT_PS = (1ULL << 7); // PS for PDE/PDPTE link entries
const uint64_t BIT_PAT_LARGE = (1ULL << 12); // PAT for 2MiB/1GiB leaf format
uint64_t flags = (uint64_t)entry & ~ADDR_MASK_4K;
flags &= ~(BIT_PS | BIT_PAT_LARGE);
return flags;
}




static uint64_t populate2m_into_table(uintptr_t *root_table, uintptr_t virtual_adress, 
    uintptr_t physical_adress, uint64_t pml4_e_flags, uint64_t pdpt_e_flags, uint64_t pd_e_flags){

    if(is_initialized == false){return 0;}

    if(root_table == NULL){return 0;}

    if(virtual_adress % 2097152 != 0){return 0;}

    
    

    

    //add if not alligned align them.

    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    
    uintptr_t pml4_entry = root_table[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        free_page_frame_out new_pdpt_table_metadata = free_frame_for_new_page_structure_and_create_it();
        if(new_pdpt_table_metadata.success == false){return 0;}
        //u have to write to the actual table pointer not to the local variable this is very important.
        root_table[index_in_pml4] = new_pdpt_table_metadata.physical_adress | pml4_e_flags;
        pml4_entry = root_table[index_in_pml4];
        root_table = new_pdpt_table_metadata.pgtable_pointer;
    }else{

    root_table = (uintptr_t *)(pml4_entry & ~0xFFF);    }
    uintptr_t pdpt_entry = root_table[index_in_pdpt];
    if((pdpt_entry & 1) == 0 || pdpt_entry == 0){

        free_page_frame_out new_pd_table_metadata = free_frame_for_new_page_structure_and_create_it();
        if(new_pd_table_metadata.success == false){return 0;}
        root_table[index_in_pdpt] = new_pd_table_metadata.physical_adress | pdpt_e_flags;
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

    root_table[index_in_pd] = physical_adress | pd_e_flags; //write the final 2mib pd entry using the physical base plus present plus large page bit

    return 1; //the full 2mib population path succeeded

    

}
//add flag checks

static uint64_t populate4k_into_table(uintptr_t *root_table, uintptr_t virtual_adress, 
    uintptr_t physical_adress, uint64_t pml4_e_flags, uint64_t pdpt_e_flags, uint64_t pd_e_flags, uint64_t pt_e_flags){

    if(is_initialized == false){return 0;}

    if(root_table == NULL){return 0;}

    if(virtual_adress % 4096 != 0){return 0;}

    

    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF; //this is the pt index because the final 4k leaf lives in the pt level
    

    uintptr_t pml4_entry = root_table[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        free_page_frame_out pdpt_metadata = free_frame_for_new_page_structure_and_create_it();
        if(pdpt_metadata.success == false){return 0;}
        root_table[index_in_pml4] = pdpt_metadata.physical_adress | pml4_e_flags;
        pml4_entry = root_table[index_in_pml4];
        root_table = pdpt_metadata.pgtable_pointer;





    }else{
        root_table = (uintptr_t *)(pml4_entry & ~0xFFF);

    }


    uintptr_t pdpt_entry = root_table[index_in_pdpt];

    if((pdpt_entry & 1) == 0 || pdpt_entry == 0){

        free_page_frame_out pd_metadata = free_frame_for_new_page_structure_and_create_it(); //allocate a pd page if the pdpt slot is empty
        if(pd_metadata.success == false){return 0;} //stop if the new pd page could not be created
        root_table[index_in_pdpt] = pd_metadata.physical_adress | pdpt_e_flags; //link the new pd page into the real pdpt entry with table flags
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
        root_table[index_in_pd] = pt_metadata.physical_adress | pd_e_flags; //link the new pt page into the real pd entry with table flags
        pd_entry = root_table[index_in_pd]; //refresh the local copy so it matches the real pd entry we just wrote
        root_table = pt_metadata.pgtable_pointer; //descend into the new pt page so the final 4k leaf can be written




    }else{
        if((pd_entry & (1ULL << 7)) != 0){return 0;} //if this pd entry is a 2mib large page we cannot descend into a pt from it here
        root_table = (uintptr_t *)(pd_entry & ~0xFFF); //strip the flags from the existing pd entry so root_table now points to the real pt page

    }

    uintptr_t pt_entry = root_table[index_in_pt]; //read the final pt entry that will hold the 4k mapping

    if((pt_entry & 1) != 0 && pt_entry != 0){return 0;} //if a pt entry already exists here do not overwrite the mapping

    root_table[index_in_pt] = physical_adress | pt_e_flags; //write the final 4k pt entry using the physical base plus the present bit

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
    
    
    uint64_t pml4e_flags = strip_flags_non_pt(walker_out.pml4_entry);
    uint64_t pdpte_flags = strip_flags_non_pt(walker_out.pdpt_entry);
    uint64_t pde_flags = strip_flags_non_pt(walker_out.pd_entry);
    uint64_t pte_flags = strip_flags_pt(walker_out.pt_entry);
    

    if(final_walker_out_lvl == 3){

        if(populate2m_into_table(dest_table, dest_va, source_final_entry_val ,pml4e_flags, pdpte_flags, pde_flags ) == 0 ){return 0;}

        


        
    }else if(final_walker_out_lvl == 4){

        if(populate4k_into_table(dest_table, dest_va, source_final_entry_val, pml4e_flags, pdpte_flags, pde_flags, pte_flags) == 0){return 0;}

        
        

        

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

//this root is from the first 4gib (from the bitmap not hte buddy)
static uint64_t poplulate_phys_map(uintptr_t *new_root){

    if(new_root == NULL){return 0;}

    //we will round up the physmap this wont matter bcz
    //the pmm will never hand those frames this is for convinence

    uintptr_t aligned_end = 
    ((uintptr_t)BUDDY_MAX_PHYS + (2ULL * 1024 * 1024) - 1) & ~((2ULL * 1024 * 1024) - 1);

    //now we need to loop over all mem and call the helper
    //mapping all mem in 2mib pages.

    uint64_t step_size = 2097152ULL;
    
    for(uintptr_t pa = 0; pa < aligned_end; pa += step_size){

        uintptr_t va = PHYS_MAP_BASE + pa;
        uint64_t rw_and_kernel_flags_enable_and_present = (1ULL << 0) | (1ULL << 1);      // P | RW
        uint64_t rw_and_kernel_flags_enable_and_present_and_large_page =
        (1ULL << 0) | (1ULL << 1) | (1ULL << 7);                                       // P | RW | PS


        if(!populate2m_into_table(new_root,va, pa, rw_and_kernel_flags_enable_and_present, 
            rw_and_kernel_flags_enable_and_present, rw_and_kernel_flags_enable_and_present_and_large_page)){return 0;}


    }

    return 1;


}




static uintptr_t cr3_switch_t(uintptr_t new_root_table_phys){

    if(new_root_table_phys % 4096 != 0){return 0;}

    uintptr_t old_table_phys;


    //this part will need to be wrapped if we are going to port to rspi
    asm volatile("mov %%cr3, %0" : "=r "(old_table_phys) :: "memory");
    asm volatile("mov %0, %%cr3" :: "r"(new_root_table_phys) : "memory");

return old_table_phys;

}



uint64_t transition(uintptr_t *old_root){

    

    free_page_frame_out page_metdata = vmm_create_root_table();
    if(page_metdata.success == false) return 0;
    uintptr_t *new_root = page_metdata.pgtable_pointer;
    uintptr_t new_root_phys = page_metdata.physical_adress;

    uintptr_t src_start_va =((uintptr_t)&_kernel_start) & ~((uintptr_t)0x1FFFFF);
    uintptr_t src_end_va = (((uintptr_t)&_kernel_end) + 0x1FFFFF) & ~((uintptr_t)0x1FFFFF);

    //uintptr_t dest_va_start = KERNEL_IMAGE_BASE + (_kernel_start - LOW_BASE); check this shit

    if(!copy_mapping_dynamic_over_range(old_root,src_start_va, src_end_va, new_root, src_start_va)) return 0;
    if (!copy_mapping_dynamic_over_range(old_root, src_start_va, src_end_va, new_root, KERNEL_IMAGE_BASE)) return 0;
    if(!poplulate_phys_map(new_root)) return 0;

    cr3_switch_t(new_root_phys);

    toptable = new_root;
    adress_toptable = new_root_phys;

return 1;


}


//TRANSITION IS DOEN

//if u still havent figured im not the best at spelling
//now with page based virtual mem mangagment we are only concerned with alloc free, map unmap pages, which is pretty simple
static void invlpg(void *va) {
    asm volatile("invlpg (%0)" ::"r"(va) : "memory");
}


//this fucntion looks very unsafe idfk
//okay now this is fixed we are corrctly getting vas from the physmap
static uintptr_t phys_to_virt(uintptr_t physical_adress){

    if(physical_adress == 0){return 0;}
    if (physical_adress >= (uintptr_t)BUDDY_MAX_PHYS){return 0;}
    
    uintptr_t va = physical_adress + PHYS_MAP_BASE;
    if(va < PHYS_MAP_BASE){return 0;}

    return va;


}

// We are redefining constraints and sucess for this walker and refactoring the full next phase, for clarity and 
// debug ease cuz its a nightmare now.
// faliure is only when the walk actually fails for sm reason now we have also the stop reason which will make our life 
// much easier.
// ALL POINTERS RETURNED BY THIS FUNCTION ARE VIRTUAL....
static walker_out2 the_walking_pager2(uintptr_t *root_table, uintptr_t virtual_adress){


    const uint64_t ADDR_MASK_4K = 0x000FFFFFFFFFF000ULL;
    const uint64_t ADDR_MASK_2M = 0x000FFFFFFFE00000ULL;
    const uint64_t ADDR_MASK_1G = 0x000FFFFFC0000000ULL;
    const uint64_t BIT_PRESENT = 1ULL;
    const uint64_t BIT_PS_OR_PTE_PAT = (1ULL << 7);
    const uint64_t BIT_LARGE_PAT = (1ULL << 12);


    walker_out2 result = {0};

    if(root_table == NULL){return result;}
    
    if ((((int64_t)virtual_adress) >> 47) != 0 && (((int64_t)virtual_adress) >> 47) != -1) { return result; }
    //non canonical va check idfk what this shit is

    
    //this is an index in the table size meaning from 0 to 511
    //every 9th bit in the virtual adress gives you the index in the table which it refers to
    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;


    
    

    uintptr_t *curr = (uintptr_t *)phys_to_virt((uintptr_t)root_table);
    if(curr == NULL){return result;}
    uintptr_t *pml4_ptr = curr;

    

    uintptr_t pml4_entry = curr[index_in_pml4];
    uintptr_t *pml4_entry_adress = &curr[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){
        
        result.final_page_walked_ptr = curr;
        result.final_entry_pointer = pml4_entry_adress;
        result.physical_base_adress = (uintptr_t)NULL;
        result.page_size = (uint64_t)NULL;
        result.final_page_level = 1;
        result.success = true;
        result.error_code = STOP_AT_PML4;
        result.stop_reason =  NOT_PRESENT;


        return result;
    }


    uintptr_t pdpt_adress = phys_to_virt((uintptr_t)(pml4_entry & ADDR_MASK_4K));

    curr = (uintptr_t *)pdpt_adress;
    if(curr == NULL){return result;}
    uintptr_t *pdpt_ptr = curr;

    uintptr_t pdpt_entry = curr[index_in_pdpt];
    uintptr_t *pdpt_entry_adress = &curr[index_in_pdpt];

    if(((pdpt_entry & 1) == 0) || pdpt_entry == 0){
        result.stop_reason = NOT_PRESENT;
        result.final_page_level = 2;
        result.final_page_walked_ptr = curr;
        result.final_entry_pointer = pdpt_entry_adress;
        result.physical_base_adress = (uintptr_t)NULL;
        result.page_size = (uintptr_t)NULL;
        result.success = true;
        result.parent_flags = pml4_entry & ~ADDR_MASK_4K;
        result.parent_entry = pml4_entry;
        result.parent_table_ptr = pml4_ptr;
        result.parent_lvl = 1;
        

        

        return result;
    }



    uint64_t pdpt_page_size_bit = (pdpt_entry & (1ULL << 7));

    if(pdpt_page_size_bit != 0){

        result.success = true;
        result.final_entry_pointer = pdpt_entry_adress;
        result.final_page_level = 2;
        result.final_page_walked_ptr = curr;
        result.stop_reason = LARGE_PAGE;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 1073741824; //1gib in bytes
        uintptr_t physical_base = final_entry_value & ADDR_MASK_1G;
        result.physical_base_adress = physical_base;
        result.parent_flags = pml4_entry & ~ADDR_MASK_4K;
         result.parent_entry = pml4_entry;
        result.parent_table_ptr = pml4_ptr;
        result.parent_lvl = 1;

        return result;

    }
    

    uintptr_t pd_adress = phys_to_virt((uintptr_t)(pdpt_entry & ADDR_MASK_4K)); 


    curr = (uintptr_t *)pd_adress;
    if(curr == NULL){return result;}
    uintptr_t *pd_ptr = curr;

    uintptr_t pd_entry = curr[index_in_pd];
    uintptr_t *pd_entry_adress = &curr[index_in_pd];
    

    if(((pd_entry & 1) == 0) || pd_entry == 0){
        result.stop_reason = NOT_PRESENT;
        result.final_page_level = 3;
        result.final_page_walked_ptr = curr;
        result.final_entry_pointer = pd_entry_adress;
        result.physical_base_adress = (uintptr_t)NULL;
        result.page_size = (uint64_t)NULL;
        result.success = true;
        result.parent_flags = pdpt_entry & ~ADDR_MASK_4K;
         result.parent_entry = pdpt_entry;
        result.parent_table_ptr = pdpt_ptr;
        result.parent_lvl = 2;

        return result;
    }

     uint64_t pd_page_size_bit = (pd_entry & (1ULL << 7));


      if(pd_page_size_bit != 0){

        result.success = true;
        result.final_entry_pointer = pd_entry_adress;
        result.final_page_level = 3;
        result.final_page_walked_ptr = curr;
        result.stop_reason = LARGE_PAGE;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 2097152; //2mib in bytes
        uintptr_t physical_base = final_entry_value & ADDR_MASK_2M;
        result.physical_base_adress = physical_base;
        result.parent_flags = pdpt_entry & ~ADDR_MASK_4K;
         result.parent_entry = pdpt_entry;
        result.parent_table_ptr = pdpt_ptr;
        result.parent_lvl = 2;

                return result;

    }

    uintptr_t pt_adress = phys_to_virt((uintptr_t)(pd_entry & ADDR_MASK_4K)); 

    curr = (uintptr_t *)pt_adress;
    if(curr == NULL){return result;}


    uintptr_t pt_entry = curr[index_in_pt];
    uintptr_t *pt_entry_adress = &curr[index_in_pt];

    if(((pt_entry & 1) == 0) || pt_entry == 0){
        result.stop_reason = NOT_PRESENT;
        result.final_page_level = 4;
        result.final_page_walked_ptr = curr;
        result.final_entry_pointer = pt_entry_adress;
        result.physical_base_adress = (uintptr_t)NULL;
        result.page_size = (uint64_t)NULL;
        result.success = true;
        result.parent_flags = pd_entry & ~ADDR_MASK_4K;
        result.parent_entry = pd_entry;
        result.parent_table_ptr = pd_ptr;
        result.parent_lvl = 3;
        


                return result;
    }


   
        result.success = true;
        result.final_entry_pointer = pt_entry_adress;
        result.final_page_level = 4;
        result.final_page_walked_ptr = curr;
        result.stop_reason = WALK_COMPLETEST;

        uintptr_t final_entry_value = * result.final_entry_pointer;
        result.page_size = 4096; 
        uintptr_t physical_base = final_entry_value & ADDR_MASK_4K;
        result.physical_base_adress = physical_base;
        result.parent_flags = pd_entry & ~ADDR_MASK_4K;
          result.parent_entry = pd_entry;
        result.parent_table_ptr = pd_ptr;
        result.parent_lvl = 3;

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


uintptr_t vmm_query_page2(uintptr_t virtual_adress){

    if(is_initialized == false){
        return 0;
    }

    walker_out2 walker_result = the_walking_pager2(toptable, virtual_adress);

    if(walker_result.success == 0 ){
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




// fun fact i made a typo naming the function and im leaving it here as my trademark
// again ALL POINTERS RETURNED ARE VIRTUAL.




//this function will need alot of revision and might explode later
// i think its kinda fixed rn //// shouyld add more allignmnt checka and null checks, and also return a struct
// okay fuck this im redoing the funciton


//this function will have to be overhauled againadd a aparam desired map lvl to split to

//now im starting the overhaul lets see how ts goes

//f***ing finally fuck this function this has to be the most annoying thing i ever did in my life
// f*** THIS FUNCTION OKAY? FINALLY FINALLYYYYYYYYYYYYYYYYYYYYYYYYYY DONE........
//
// FLAG CONTRACT / ASSUMPTIONS (IMPORTANT):
// - This function is a *splitter* (it only creates intermediate tables or splits an existing large-page leaf).
// - NOT_PRESENT stop_reason:
//     - We allocate a new paging-structure page (new table) and link it into the current level.
//     - `caller_flags` here means "TABLE/LINK FLAGS" for a pointer-to-next-table entry.
//     - Required: PRESENT (bit 0) must be set.
//     - Forbidden: PS (bit 7 at PDPT/PD levels) must be 0 (this must NOT create a large-page leaf).
//     - Forbidden: address bits must be 0 (do not pass any physical address inside `caller_flags`).
//     - The address field for the link entry always comes from `new_page.physical_adress`.
// - LARGE_PAGE stop_reason:
//     - We are splitting an existing *present* large-page leaf (1GiB or 2MiB).
//     - Child leaf entries inherit the old leaf's flags to preserve the mapping semantics for the whole region.
//     - After split, the parent entry is rewritten into a table-pointer entry (PS cleared).
// - If you want to CHANGE permissions/caching/etc for the final mapping, do it when you write the final leaf
//   entry in the mapper (e.g. `vmm_map_page()`), not by changing flags for neighboring entries in the split.

//BUGS TO LOOK OUT FOR

//Flag inheritance issues:
//You copy too many raw bits
//No strict permission-only mask
//PAT/PS semantics leak across levels
//Depends too heavily on old entry format stability
//Alignment issues to improve:
//enforce parent base alignment to page size "childe_size = parent_size/512"
//validate allocated page table alignment (4K)
//ensure split range consistency (512 entries exact mapping)
//optionally validate walker-provided base consistency









static spilt_out spilt_page(uintptr_t *root_table, uintptr_t virtual_adress, uint64_t desired_spilt_lvl){

    spilt_out result = {0};

    uintptr_t *last_new_table_ptr = NULL;
    uintptr_t *last_old_table_ptr = NULL;


    // the pat is overlapping here will have to read more about this...
    const uint64_t ADDR_MASK_4K = 0x000FFFFFFFFFF000ULL;
    const uint64_t ADDR_MASK_2M = 0x000FFFFFFFE00000ULL;
    const uint64_t ADDR_MASK_1G = 0x000FFFFFC0000000ULL;
    const uint64_t BIT_PRESENT = 1ULL;
    const uint64_t BIT_PS_OR_PTE_PAT = (1ULL << 7);
    const uint64_t BIT_LARGE_PAT = (1ULL << 12);

    if(root_table == NULL){return result;}
    if(virtual_adress == 0 || virtual_adress % 4096 != 0){return result;}
    if ((((int64_t)virtual_adress) >> 47) != 0 && (((int64_t)virtual_adress) >> 47) != -1) { return result; }
    //non canonical va check idfk what this shit is
    if(desired_spilt_lvl > 4 || desired_spilt_lvl == 1 || desired_spilt_lvl == 0){return result;}


    walker_out2 walker_result = the_walking_pager2(root_table, virtual_adress);
    if(walker_result.success == false){return result;}

    if(walker_result.final_page_level == desired_spilt_lvl){

        result.noop = true;
        result.success = true;
        
        return result;
    }


    if(walker_result.final_page_level < desired_spilt_lvl){


        while(walker_result.final_page_level != desired_spilt_lvl){

            if(walker_result.stop_reason == NOT_PRESENT){

                // make the present bit hardcoded not provided by the caller ((caller_flags & BIT_PRESENT)) == 0
                // maybe cleaner only would it make much of a diff not really...
               

                free_page_frame_out new_page = free_frame_for_new_page_structure_and_create_it2();

                if(!new_page.success){return result;}

                if(walker_result.final_entry_pointer == NULL){return result;}
                if(walker_result.final_page_walked_ptr == NULL){return result;}

                last_new_table_ptr = new_page.pgtable_pointer;
                last_old_table_ptr = walker_result.final_page_walked_ptr;

                uint64_t inherited_flags = walker_result.parent_flags;

                uint64_t enforced_flags = inherited_flags;

                if(virtual_adress < HIGHER_HALF_BASE_){

                    uint64_t us_bit = (1ULL << 2);


                    enforced_flags = ((inherited_flags | BIT_PRESENT) | us_bit) & ~(BIT_PS_OR_PTE_PAT | BIT_LARGE_PAT);
                    
                }
                else{
                 enforced_flags = (inherited_flags | BIT_PRESENT) & ~(BIT_PS_OR_PTE_PAT | BIT_LARGE_PAT);
                }

                

                

                *walker_result.final_entry_pointer = (new_page.physical_adress & ADDR_MASK_4K) | enforced_flags;


                walker_result = the_walking_pager2(root_table,virtual_adress);
                if (!walker_result.success){ result.success = false; result.error_code = REWALK_FAILED; return result;}





            }else if(walker_result.stop_reason == LARGE_PAGE){

                    uint64_t child_size = walker_result.page_size/512;
                    if((child_size != 2097152) && (child_size != 4096)){return result;}

                    if(walker_result.final_entry_pointer == NULL){return result;}

                    uintptr_t old_leaf_entry = *walker_result.final_entry_pointer;
                    uint64_t leaf_addr_mask = 0;
                    if(walker_result.page_size == (1ULL << 30)){
                        leaf_addr_mask = ADDR_MASK_1G;
                    }else if(walker_result.page_size == (1ULL << 21)){
                        leaf_addr_mask = ADDR_MASK_2M;
                    }else{
                        return result;
                    }

                    uint64_t leaf_flags = old_leaf_entry & ~leaf_addr_mask;


                

                    free_page_frame_out new_page = free_frame_for_new_page_structure_and_create_it2();
                    if(!new_page.success){return result;}

                    if(walker_result.final_page_walked_ptr == NULL){return result;}

                    last_new_table_ptr = new_page.pgtable_pointer;
                    last_old_table_ptr = walker_result.final_page_walked_ptr;

                    uintptr_t parent_physical_base = walker_result.physical_base_adress; 

                    //KEEP AN EYE FOR THIS L;ATER IF WE WANT TO ACCES THE ZERO AREA

                    if (parent_physical_base == 0) {result.error_code = PARENT_PA_IS_0; return  result;}

                    for(uint32_t i = 0; i < 512; i++){

                        uint64_t flags = leaf_flags;

                        if(child_size == 4096){
                            uint64_t pat = (flags & BIT_LARGE_PAT) ? 1ULL : 0ULL;
                            flags &= ~(BIT_PS_OR_PTE_PAT | BIT_LARGE_PAT);
                            flags |= (pat << 7);
                        }

                        new_page.pgtable_pointer[i] = (parent_physical_base + i*child_size) | flags;

                        


                    }

                    //(2) old_leaf_entry & ~ADDR_MASK_4K

                    //For link flags, this assumes all address bits are masked correctly with 4K mask, which is usually fine because next-level tables are 4K aligned.

                    //Just verify no architecture-specific high bits sneak through.

                    //Minor thing.
                    {
                        uint64_t link_flags = (old_leaf_entry & ~ADDR_MASK_4K) & ~BIT_PS_OR_PTE_PAT;
                        *walker_result.final_entry_pointer = (new_page.physical_adress & ADDR_MASK_4K) | link_flags;
                        invlpg((void*)virtual_adress);
                    }


                 walker_result = the_walking_pager2(root_table,virtual_adress);

                 if (!walker_result.success){ result.success = false; result.error_code = REWALK_FAILED; return result;}





            }else{

                result.error_code = UNDEFINED_ERROR;    
                result.success = false;

                return result;
            }



        }

        result.success = true;
        result.new_table_ptr = last_new_table_ptr;
        result.old_table_ptr = last_old_table_ptr;
        return result;

    }

    if(walker_result.final_page_level > desired_spilt_lvl){
        result.success = true;
        result.noop = true;
        result.error_code = COELSENCE; 
        return result;}


        
        return result;
    

}











//also expand the splitter to take arbitrary split sizes (desired split lvl)
//make sure already mapped pages are handled correctly
// now i will add the helpers since i spent 2 days fixing them
// the contract of this function is that it will fail if anything is mapped even if a large page is here 
map_out vmm_map_page(
    uintptr_t virtual_adress,
    uintptr_t physical_adress, 
    uintptr_t *root_table, 
    uint64_t desired_map_lvl, 
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


        const uint64_t ADDR_MASK_4K = 0x000FFFFFFFFFF000ULL;
        const uint64_t ADDR_MASK_2M = 0x000FFFFFFFE00000ULL;
        const uint64_t ADDR_MASK_1G = 0x000FFFFFC0000000ULL;
        const uint64_t BIT_PRESENT = 1ULL;
        const uint64_t BIT_PS_OR_PTE_PAT = (1ULL << 7);
        const uint64_t BIT_LARGE_PAT = (1ULL << 12);

        //add more allignmnen checks

        if(virtual_adress % mapping_size != 0 || physical_adress % mapping_size != 0){return result;}
        if ((((int64_t)virtual_adress) >> 47) != 0 && (((int64_t)virtual_adress) >> 47) != -1) { return result; }
        if(desired_map_lvl == 0 || desired_map_lvl > 4 || desired_map_lvl == 1){return result;}
        if(flags == 0){return result;}// aslo this

        walker_out2 walker_result = the_walking_pager2(root_table, virtual_adress);
        if(walker_result.success == false){return result;}

        

        // case 1, correct lvl mapping exists already           (*walker_result.final_entry_pointer & BIT_PRESENT)!= 0
        if(walker_result.final_page_level == desired_map_lvl && walker_result.stop_reason != NOT_PRESENT) {

            result.error_code = MAPPING_EXISTS_AT_DESIRED_LVL; 
            result.success = false;
            return result;
        }

        // CASE 2, correct lvl no mapping, just fill.
        if(walker_result.final_page_level == desired_map_lvl && walker_result.stop_reason == NOT_PRESENT){


            uint64_t entry_flags = flags| BIT_PRESENT;



             if(desired_map_lvl != 4){
                entry_flags |= BIT_PS_OR_PTE_PAT;
            }

            *walker_result.final_entry_pointer = physical_adress | entry_flags;
            invlpg((void*)virtual_adress);


            result.success = true;
            result.v_final_entry_ptr = walker_result.final_entry_pointer;
            result.physical_adress = physical_adress;
            return result;


        }

        
        if(walker_result.final_page_level < desired_map_lvl){

            
            //if (((*walker_result.final_entry_pointer & BIT_PS_OR_PTE_PAT) != 0) &&
            //(walker_result.final_page_level != 4)
               // && ((*walker_result.final_entry_pointer &  BIT_PRESENT) != 0)) {

        // case 3: LARGE PAGE HIT CASE
        // mapping exists and wrong lvl, just split to lvl(update structure)
        if (walker_result.stop_reason == LARGE_PAGE) {

            uint64_t mask;
            uint64_t offset_mask;

            if (walker_result.final_page_level == 3) {
                mask = ADDR_MASK_2M;
                offset_mask = 0x1FFFFF;
            }
            else if (walker_result.final_page_level == 2) {
                mask = ADDR_MASK_1G;
                offset_mask = 0x3FFFFFFF;
            }
            else {
                return result;
            }

            uint64_t base = *walker_result.final_entry_pointer & mask;
            uint64_t actual_pa = base + (virtual_adress & offset_mask);

            //verifying that the split operation is applied to the correct VA→PA region instance so we dont silently split an unrelated region.
            //this will fuck up the accedintally hit region(or sector of large page) flags and wont map correctly we will corrupt memory in specific the page
            // table semantics not physical mem in short NOOOOOOO. (mapping identity corruption) (midlife crisis) imma kill myself soon tho..
            if (actual_pa != physical_adress) {

                result.error_code = CANT_REMAP_TO_ANOTHER_ADRESS;
                return result;
            }

            
            spilt_out new_mapping_structure = spilt_page(root_table, virtual_adress, desired_map_lvl);
            if(new_mapping_structure.success == false){ result.error_code = SPILT_ERROR; return result;}

            walker_out2 new_walk = the_walking_pager2(root_table, virtual_adress);
            if(new_walk.success == false || new_walk.final_page_level != desired_map_lvl){ result.error_code = REWALK_FAILED;   return result;}  
            
             
            uint64_t entry_flags = flags| BIT_PRESENT;

            // question here for later should the flags be determined by lvl desired or by the walker.

            
            // case 4:
            // are mapping exisits wrong lvl are we splitting to 4k? just for setting the 7th bit
            // case 5:
            // we are not going to 4k lvl so we dont set bit 7 to 0
            if(desired_map_lvl != 4){
                entry_flags |= BIT_PS_OR_PTE_PAT;
            }

            

            *new_walk.final_entry_pointer = physical_adress | entry_flags;

            result.v_final_entry_ptr = new_walk.final_entry_pointer;
            result.physical_adress = physical_adress;
            result.success = true;
            result.flags = entry_flags;
            return result;


        }

        // NOT PRESENT ENTRY HIT....
        // case 6:
        // wrong lvl no mapping present, create structure and map
        //if((*walker_result.final_entry_pointer & BIT_PRESENT) == 0){

        if(walker_result.stop_reason == NOT_PRESENT){

            spilt_out new_mapping_structure = spilt_page(root_table, virtual_adress, desired_map_lvl);
            if(new_mapping_structure.success == false){ result.error_code = SPILT_ERROR; return result;}

            walker_out2 new_walk = the_walking_pager2(root_table, virtual_adress);
            if(new_walk.success== false || new_walk.final_page_level != desired_map_lvl){  result.error_code = REWALK_FAILED;   return result;}

            uint64_t entry_flags = flags| BIT_PRESENT;



             if(desired_map_lvl != 4){
                entry_flags |= BIT_PS_OR_PTE_PAT;
            }

            


            *new_walk.final_entry_pointer = physical_adress | entry_flags;

            result.v_final_entry_ptr = new_walk.final_entry_pointer;
            result.physical_adress = physical_adress;
            result.success = true;
            result.flags = entry_flags;
            return result;

        }

    }
    //case 7:
    // wether mapped or unmapped this will fail cuz we dont support mergeing small pages to large ones yet....
    if(walker_result.final_page_level > desired_map_lvl){

        result.error_code = COELSENCE;
        result.success = false;
        return result;

    }

    result.error_code = UNEXPECTED_ERROR;
    result.success = false;
    return result;


}


unmap_out vmm_unmap_page(uintptr_t *root_table, uintptr_t virtual_adress){

    unmap_out result = {0};
    if(root_table == NULL){return result;}

    walker_out2 walker_result = the_walking_pager2(root_table, virtual_adress);
    if(!walker_result.success){return result;}

    if(walker_result.final_page_level == 1 && walker_result.success){// this case will never happen unless we have a major bug

        
        result.success = false;
        result.error_code = UNMAPPING_AT_PML4;
        return result;

    }
    
    if(walker_result.success && walker_result.stop_reason != NOT_PRESENT){
    result.v_unmapped_pa_ptr = walker_result.final_entry_pointer;
    result.unmapped_pa = *walker_result.final_entry_pointer;
    result.unmapped_va = virtual_adress;
    result.success = true;
    *walker_result.final_entry_pointer = (uintptr_t)NULL;
    invlpg((void*)virtual_adress);

    

    return result;
    }

    return result;

    

    

}



create_vm_space_out create_vm_space(uintptr_t *kernel_root){

    create_vm_space_out result = {0};
    if(kernel_root == NULL){return result;}
    
    free_page_frame_out new_pml4 = free_frame_for_new_page_structure_and_create_it2();
    if(!new_pml4.success){ result.error_code = PML4_ALLOC_FAIL; return result;}
    
    vm_space *new_space = kcalloc(1,sizeof(vm_space));

    if(new_space == NULL){
        pmm_free_frame(new_pml4.physical_adress);
        result.success = false;
        result.error_code = PML4_ALLOC_FAIL;
        return result;
    }


    new_space->is_kernel = false;
    new_space->pml4_phys = new_pml4.physical_adress;
    new_space->pml4_phys_ptr = new_pml4.pgtable_pointer;
    new_space->pml4_ptr = (uintptr_t *)phys_to_virt(new_pml4.physical_adress);
    
    //copy the kernel mappings
    //the trick here is that we are copiyng only the top pointers bcz
    //these already point to the rest of the hieratchy 
    //(amazing spelling i know)
    int ker_index = (HIGHER_HALF_BASE_ >> 39) & 0x1FF;

    //DONT FORGET !!!!!!!!!!!!!!!!!!!!!!!!!!!!
    //add flag enforcment for kernel pages, and user pages.
    //just set all low pages to US = 1, maybe here maybe in region idfk

    for(int i = ker_index; i <= 511; i++){

        new_pml4.pgtable_pointer[i] = kernel_root[i];
     

    }
    new_space->heap_region = NULL;
    new_space->stack_region = NULL;
    new_space->vm_region_list = NULL;

    new_space->kernel_base = KERNEL_IMAGE_BASE;//fill this 
    new_space->kernel_limit = UINTPTR_MAX; //and this too
    new_space->user_base = 0;
    new_space->user_limit = HIGHER_HALF_BASE_; //also fill this
    new_space->kernel_adress_space_base = HIGHER_HALF_BASE_;

    
    result.space = (vm_space *)new_space;
    result.success = true;

    return result;
    

    

}


//this is why secure os is th emost secure ever

reserve_region_out reserve_region(vm_space* space, reserve_region_request* req){

    reserve_region_out result = {0};
    if(space == NULL || req == NULL){return result;}
    if(req->size == 0){return result;}

    if ((req->map_flags & FIXED) && (req->map_flags & TOPDOWN)) return result;
    if ((req->map_flags & PRIVATE) && (req->map_flags & SHARED)) return result;

    size_t req_real_size = req->size;

    req->size = align_size_up(req->size, 4096);
    

    vm_region* region = kcalloc( 1, sizeof(vm_region));
    if(region == NULL) return result;

    
    //overflow guard
    uintptr_t legal_limit;
    uintptr_t legal_base;

    if(space->is_kernel == false){
        legal_limit = HIGHER_HALF_BASE_;
        legal_base = 0;

    }else{
        legal_limit = align_down_u(UINTPTR_MAX, 4096);
        legal_base = HIGHER_HALF_BASE_;

    }

    if(legal_limit < legal_base){kfree(region); return result;}
    size_t available_range = legal_limit - legal_base;
    if(req->size > available_range){kfree(region); return result;}

    uintptr_t min_end_from_base = legal_base + req->size;
    if(min_end_from_base < legal_base){kfree(region); return result;} // overflow guard
    if(legal_limit < min_end_from_base){kfree(region); return result;} // underflow guard

    if(req->map_flags & FIXED){

        if(req->hint_va >= legal_limit || req->hint_va < legal_base){kfree(region); return result;}
        if(req->size > (legal_limit - req->hint_va)){kfree(region); return result;}
        

        if(req->hint_va % 4096 != 0){
            kfree(region);
            return result;
        }
         
        
        region->va_start = req->hint_va;
        region->va_end = align_up_u((req->hint_va + req->size), 4096); 

        vm_region* general_cursor = space->vm_region_list;
        if(general_cursor == NULL){ goto insert_to_list;}

        while(general_cursor != NULL){

            if(((general_cursor->va_start) < (req->hint_va + req->size)) && (req->hint_va < (general_cursor->va_end))){
                
                result.success = false;
                result.error_code = FIXED_OVERLAP_ERROR; 
                kfree(region);
                return result;
            }
            
            general_cursor = general_cursor->next;
           

        }

        insert_to_list:

        vm_region* curr = space->vm_region_list;
        vm_region* prev = NULL;

        

        // Find sorted insertion point: prev < new <= curr
        while(curr != NULL && curr->va_start < region->va_start){
            prev = curr;
            curr = curr->next;
        }

        // Stitch new node between prev and curr
        region->prev = prev;
        region->next = curr;

        // If prev is NULL, insertion happened at the head
        if(prev == NULL){
            space->vm_region_list = region;
        }else{
            prev->next = region;
        }

        if(curr != NULL){
            curr->prev = region;
        }

        

        result.success = true;
        result.region_ptr = (vm_region *) region;
        result.space_ptr = (vm_space *) space;

        return result;


        

    }else{

        if(req->map_flags & TOPDOWN){

            vm_region* cursor2 = space->vm_region_list;

            uintptr_t prefered_start_va = align_down_u(legal_limit - req->size, 4096); 


            // best case first travesre to end of list and check if last element has a valid gap from its end to limit
            if(cursor2 == NULL){

                region->va_start = prefered_start_va;
                region->va_end = prefered_start_va + req->size;
                goto insert_to_list;


            }
            while(cursor2 != NULL){
                if(cursor2->next == NULL){

                    if(((cursor2->va_start) < (legal_limit)) && (prefered_start_va < (cursor2->va_end))){
                        vm_region* back_cursor = cursor2;


                        while(back_cursor != NULL){

                            if(back_cursor->prev == NULL){
                                if(back_cursor->va_start < legal_base){
                                    result.success = false;
                                    result.error_code = TOPDOWN_ALLOC_ERROR;
                                    kfree(region);
                                    return result;
                                } // underflow guard

                                uintptr_t min_topdown_end = legal_base + req->size;
                                if(min_topdown_end < legal_base){
                                    result.success = false;
                                    result.error_code = TOPDOWN_ALLOC_ERROR;
                                    kfree(region);
                                    return result;
                                } // overflow guard

                                size_t gap = back_cursor->va_start - legal_base;
                                if(gap >= req->size){
                                    region->va_end = back_cursor->va_start;
                                    region->va_start = (back_cursor->va_start - req->size);
                                    goto insert_to_list;
                                }else{

                                    result.success = false;
                                    result.error_code = TOPDOWN_ALLOC_ERROR;
                                    kfree(region);
                                    return result;
                                }

                            }else{

                                if(back_cursor->va_start < back_cursor->prev->va_end){
                                    result.success = false;
                                    result.error_code = TOPDOWN_ALLOC_ERROR;
                                    kfree(region);
                                    return result;
                                } // underflow guard

                                uintptr_t min_topdown_end = back_cursor->prev->va_end + req->size;
                                if(min_topdown_end < back_cursor->prev->va_end){
                                    result.success = false;
                                    result.error_code = TOPDOWN_ALLOC_ERROR;
                                    kfree(region);
                                    return result;
                                } // overflow guard

                                if((back_cursor->va_start - back_cursor->prev->va_end) < req->size ){
                                    //gap is too small

                                    back_cursor = back_cursor->prev;
                                }else{
                                    //found a gap
                                    region->va_start = (back_cursor->va_start - req->size);
                                    region->va_end = back_cursor->va_start;
                                    goto insert_to_list;


                                }
                            }

                        }

                        result.success = false;
                        result.error_code = TOPDOWN_ALLOC_ERROR;
                        kfree(region);
                        return result;


                    }else{

                        //no overlap
                        region->va_start = prefered_start_va;
                        region->va_end = prefered_start_va + req->size;
                        goto insert_to_list;

                    }
                    
                }

                cursor2 = cursor2->next;

             }

            result.success = false;
            result.error_code = TOPDOWN_ALLOC_ERROR;
            kfree(region);
            return result;

            
            
    
        

        








        }else{

            vm_region* index = space->vm_region_list;

            if (index != NULL){

                size_t gap_at_start = index->va_start - legal_base;
                if(gap_at_start >= req->size){
                    region->va_start = legal_base;
                    region->va_end = legal_base + req->size;
                    goto insert_to_list;

                }
            
                while(index != NULL){

                    if(index->next == NULL){

                        //verify if the gap at the end is enough
                        size_t gap_at_end = legal_limit - index->va_end;
                        if(gap_at_end < req->size){

                            result.success = false;
                            result.error_code = FIRSTFIT_NO_SPACE_ERROR;
                            kfree(region);
                            return result;


                        }

                        region->va_start = index->va_end;
                        region->va_end = index->va_end + req->size;

                        goto insert_to_list;
                    }

                    size_t gap = index->next->va_start - index->va_end;
                    if(gap >= req->size){

                        region->va_end = index->va_end + req->size;
                        region->va_start = index->va_end;
                        goto insert_to_list;


                    }

                    index = index->next;

                



                


                

                }

            }else{
            region->va_start = legal_base;
            region->va_end = legal_base + req->size;    
            goto insert_to_list;

            }

            result.success = false;
            result.error_code = FIRSTFIT_NO_SPACE_ERROR;
            kfree(region);
            return result;

            
        }


    }

}

// i would like to say a word from the bottom of my heart to this file,
// fuck you and fuck this os shit, idfk what got into my mind starting this,
// i understand now why terry davis lost his mind doing this shit or
// why a mentally ill person is the only person to think to create amth like this shit

