//page walker, this is a helper function that itterates overpages it follows a virtuyal adress 
//and itterates over page levels adn checks wether its mapped and were its mapped and at what level is the mapping at
 static uintptr_t the_walking_pager(uintptr_t *root_table, uint64_t virtual_adress){

    uintptr_t final_entry_pointer;
    uintptr_t final_page_level;


    if(root_table == NULL){

        return 0;
    }

    //this is an index in the table size meaning from 0 to 511
    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;

    uintptr_t *curr = root_table;

    

    uintptr_t pml4_entry = curr[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        return 0;
    }


    uintptr_t pdpt_adress = pml4_entry & ~0xFFF;

    curr = (uintptr_t *)pdpt_adress;

    uintptr_t pdpt_entry = curr[index_in_pdpt];

    if(((pdpt_entry & 1) == 0) || pdpt_entry == 0){

        return 0;
    }



    uint64_t pdpt_page_size_bit = (pdpt_entry & (1ULL << 7));

    if(pdpt_page_size_bit != 0){

        final_entry_pointer = pdpt_entry;

        return 1;

    }
    

    uintptr_t pd_adress = pdpt_entry & ~0xFFF;


    curr = (uintptr_t *)pd_adress;

    uintptr_t pd_entry = curr[index_in_pd];

    if(((pd_entry & 1) == 0) || pd_entry == 0){

        return 0;
    }

     uint64_t pd_page_size_bit = (pd_entry & (1ULL << 7));


      if(pd_page_size_bit != 0){

        final_entry_pointer = pd_entry;

                return 1;

    }

    uintptr_t pt_adress = pd_entry & ~0xFFF;

    curr = (uintptr_t *)pt_adress;

    uintptr_t pt_entry = curr[index_in_pt];

    if(((pt_entry & 1) == 0) || pt_entry == 0){

                return 0;
    }


   

        final_entry_pointer = pt_entry;

    return 1;

 }











//page walker, this is a helper function that itterates overpages it follows a virtuyal adress 
//and itterates over page levels adn checks wether its mapped and were its mapped and at what level is the mapping at
 static uintptr_t the_walking_pager(uintptr_t *root_table, uint64_t virtual_adress){

    uintptr_t final_entry_pointer;
    uintptr_t final_page_level;


    if(root_table == NULL){

        return 0;
    }

    //this is an index in the table size meaning from 0 to 511
    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;

    uintptr_t *curr = root_table;

    

    uintptr_t pml4_entry = curr[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        return 1;
    }


    uintptr_t pdpt_adress = pml4_entry & ~0xFFF;

    curr = (uintptr_t *)pdpt_adress;

    uintptr_t pdpt_entry = curr[index_in_pdpt];

    if((pdpt_entry & 1 == 0) || pdpt_entry == 0){

        return 1;
    }



    uint64_t pdpt_page_size_bit = (pdpt_entry & (1ULL << 7));

    if(pdpt_page_size_bit != 0){

        final_entry_pointer = pdpt_entry;

        return 0;

    }
    

    uintptr_t pd_adress = pdpt_entry & ~0xFFF;


    curr = (uintptr_t *)pd_adress;

    uintptr_t pd_entry = curr[index_in_pd];

     if((pd_entry & 1 == 0) || pd_entry == 0){

        return 1;
    }

     uint64_t pd_page_size_bit = (pd_entry & (1ULL << 7));


      if(pd_page_size_bit != 0){

        final_entry_pointer = pd_entry;

        return 0;

    }

    uintptr_t pt_adress = pd_entry & ~0xFFF;

    curr = (uintptr_t *)pt_adress;

    uintptr_t pt_entry = curr[index_in_pt];

      if((pt_entry & 1 == 0) || pt_entry == 0){

        return 1;
    }


   

        final_entry_pointer = pt_entry;

        return 0;






 static uint64_t the_walking_pager(uintptr_t *root_table, uintptr_t virtual_adress){

    if(root_table == NULL){

        return 0;
    }

    //this is an index in the table size meaning from 0 to 511
    uint64_t index_in_pml4 = (virtual_adress >> 39) & 0x1FF;
    uint64_t index_in_pdpt = (virtual_adress >> 30) & 0x1FF;
    uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
    uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;

    uintptr_t *curr = root_table;

    

    uintptr_t pml4_entry = curr[index_in_pml4];

    if((pml4_entry & 1) == 0 || pml4_entry == 0){

        return 0;
    }


    uintptr_t pdpt_adress = pml4_entry & ~0xFFF;

    curr = (uintptr_t *)pdpt_adress;

    uintptr_t pdpt_entry = curr[index_in_pdpt];

    if(pdpt_entry == 0 || ((pdpt_entry & 1) == 0)){

        return 0;
    }



    uint64_t pdpt_page_size_bit = (pdpt_entry & (1ULL << 7));

    if(pdpt_page_size_bit != 0){

        return 1;

    }
    

    uintptr_t pd_adress = pdpt_entry & ~0xFFF;


    curr = (uintptr_t *)pd_adress;

    uintptr_t pd_entry = curr[index_in_pd];

     if(pd_entry == 0 || ((pd_entry & 1) == 0)){

        return 0;
    }

     uint64_t pd_page_size_bit = (pd_entry & (1ULL << 7));


      if(pd_page_size_bit != 0){

                return 1;

    }

    uintptr_t pt_adress = pd_entry & ~0xFFF;

    curr = (uintptr_t *)pt_adress;

    uintptr_t pt_entry = curr[index_in_pt];

            if(pt_entry == 0 || ((pt_entry & 1) == 0)){

                return 0;
    }


                return 1;


}