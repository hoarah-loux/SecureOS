#include "mm/alloc/vmm.h"
#include "mm/alloc/pmm.h"

vm_space active_space;

void page_fault_handler(uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip){


    if(error_code == 0x0){

        //a kernel read hit a page that was not present.

    }

    if(error_code == 0x1){

        //kernel write hit a page that was not present.
        
    }

    if(error_code == 0x2){

        // a kernel read hit a page that was present but violated protection.

    }

    if(error_code == 0x3){

        //a kernel write hit a page that was present but violated protection.
    }

    if(error_code == 0x4){

        //a user read hit a page that was not present.


    }

       if(error_code == 0x5){

        //a user write hit a page that was not present.


    }

       if(error_code == 0x6){

        //a user read hit a page that was present but violated protection


    }



       if(error_code == 0x7){

        //a user write hit a page that was present but violated protection


    }


    




    
}