#include "vmm.h"
#include "serial.h"
#include <string.h>

//this is the label from our asm code
extern uintptr_t *pgtables;

//this is the local pointer that will hold the lable
static uintptr_t *toptable;




void vmm_init(uintptr_t *pgtables){

    //parse the asm pointer to a local pointer
    toptable = pgtables;

    



}