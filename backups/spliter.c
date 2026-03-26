static uint64_t spilt_page(uintptr_t root_table_phys, uintptr_t virtual_adress) {
    if (root_table_phys == 0 || virtual_adress == 0) return 0;

    // walker expects a physical pointer, conversion done internally
    walker_out walker_result = the_walking_pager2((uintptr_t *)root_table_phys, virtual_adress);
    if (!walker_result.success) return 0;

    // Already split to 4KiB
    if (walker_result.final_page_level == 4) return 11;

    // ---- Split 2MiB -> 4KiB ----
    if (walker_result.final_page_level == 3) {
        if (virtual_adress % 0x200000 != 0) return 0; // alignment check

        uintptr_t original_entry = *(walker_result.final_entry_pointer);
        uintptr_t original_entry_nf = original_entry & ~0xFFF;
        uintptr_t original_entry_flags = original_entry & 0xFFF;

        // allocate new PT
        free_page_frame_out new_page = free_frame_for_new_page_structure_and_create_it2();
        if (!new_page.success) return 0;

        uintptr_t *new_page_vptr = new_page.pgtable_pointer; // virtual pointer

        for (size_t i = 0; i < 512; i++) {
            uintptr_t subpage_phys = original_entry_nf + (i * 0x1000);
            new_page_vptr[i] = subpage_phys | (original_entry_flags & ~(1ULL << 7));
        }

        // update parent PD entry
        *(walker_result.final_entry_pointer) = new_page.physical_adress | (original_entry_flags & ~(1ULL << 7));

        // convert new PT physical address to virtual pointer safely
        uintptr_t *new_pt_vptr = (uintptr_t *)phys_to_virt(new_page.physical_adress);
        if (new_pt_vptr == NULL) {
            pmm_free_frame(new_page.physical_adress);
            return 0;
        }

        uint64_t index_in_pt = (virtual_adress >> 12) & 0x1FF;
        walker_result.final_entry_pointer = &new_pt_vptr[index_in_pt];
        walker_result.final_page_level = 4;
        walker_result.page_size = 4096;
        walker_result.physical_base_adress = original_entry_nf + (index_in_pt * 0x1000);

        return 1;
    }

    // ---- Split 1GiB -> 2MiB (and recurse to 4KiB if needed) ----
    if (walker_result.final_page_level == 2) {
        if (virtual_adress % 0x40000000 != 0) return 0; // 1GiB alignment

        uintptr_t original_entry = *(walker_result.final_entry_pointer);
        uintptr_t original_entry_nf = original_entry & ~0xFFF;
        uintptr_t original_entry_flags = original_entry & 0xFFF;

        // allocate new PD (for 2MiB pages)
        free_page_frame_out new_pd = free_frame_for_new_page_structure_and_create_it2();
        if (!new_pd.success) return 0;

        uintptr_t *new_pd_vptr = new_pd.pgtable_pointer;
        for (size_t i = 0; i < 512; i++) {
            uintptr_t subpage_phys = original_entry_nf + (i * 0x200000); // 2MiB
            new_pd_vptr[i] = subpage_phys | (original_entry_flags | (1ULL << 7)); // large page
        }

        // update parent PDPT entry
        *(walker_result.final_entry_pointer) = new_pd.physical_adress | (original_entry_flags & ~(1ULL << 7));

        // index for this VA in new PD
        uint64_t index_in_pd = (virtual_adress >> 21) & 0x1FF;
        uintptr_t *target_pd_entry = &new_pd_vptr[index_in_pd];
        uintptr_t pd_entry_val = *target_pd_entry;

        walker_result.final_entry_pointer = target_pd_entry;
        walker_result.final_page_level = 3;
        walker_result.page_size = 0x200000;
        walker_result.physical_base_adress = original_entry_nf + (index_in_pd * 0x200000);

        // recursively split 2MiB -> 4KiB if needed
        if (pd_entry_val & (1ULL << 7)) {
            if (virtual_adress % 0x200000 != 0) return 0; // alignment check
            spilt_page(new_pd.physical_adress, virtual_adress);
        }

        return 1;
    }

    return 0;
}









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