static spilt_out spilt_page(uintptr_t *root_table, uintptr_t virtual_adress, uint64_t desired_spilt_lvl, uint64_t caller_flags){

    spilt_out result = {0};

    uintptr_t *last_new_table_ptr = NULL;
    uintptr_t *last_old_table_ptr = NULL;

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
                if (((caller_flags & BIT_PRESENT)) == 0 || (caller_flags & BIT_PS_OR_PTE_PAT) || (caller_flags & ADDR_MASK_4K)) {
                    result.error_code = INVALID_FLAGS_FOR_NP_CASE;
                    return result;
                }

                free_page_frame_out new_page = free_frame_for_new_page_structure_and_create_it2();

                if(!new_page.success){return result;}

                if(walker_result.final_entry_pointer == NULL){return result;}
                if(walker_result.final_page_walked_ptr == NULL){return result;}

                last_new_table_ptr = new_page.pgtable_pointer;
                last_old_table_ptr = walker_result.final_page_walked_ptr;

                if(caller_flags == 0){result.error_code = NO_FLAG_PROVIDED_FOR_NP_CASE; return result;}

                *walker_result.final_entry_pointer = (new_page.physical_adress & ADDR_MASK_4K) | (caller_flags & ~ADDR_MASK_4K);


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
                    {
                        uint64_t link_flags = (old_leaf_entry & ~ADDR_MASK_4K) & ~BIT_PS_OR_PTE_PAT;
                        *walker_result.final_entry_pointer = (new_page.physical_adress & ADDR_MASK_4K) | link_flags;
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
