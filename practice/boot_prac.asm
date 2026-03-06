32 BITS

section .page_tables

pgtables:

    resb 7000



section .text
    cld 
    cli

    ;  create all tables with zeros

    mov edi, pgtables

    xor eax, eax

    mov ecx, 7128

    

    rep stosd

    mov edi, pgtables



;build pml4[0] entry

    mov eax, pgtables + 0x1000
    or eax, 0x00000003
    mov dword[edi], eax
    mov dword [edi+4],0

    ;now go to pdpt base
    mov edi, pgtables+ 0x1000

    ;now populate pdpt[0] entry
    mov eax, pgtables + 0x2000
    or eax, 0x00000003
    mov dword [edi], eax
    mov dword [edi+4],0

    ;since we need 4 pd tables we will populate 4 pdpt entries we already finished one lets get on with the second
    ;pdpt[1]
    mov eax, pgtables + 0x3000
    or eax, 0x00000003
    mov dword[edi+8],eax
    mov dword[edi+ 12], 0
    ;pdpd[2]
    mov eax, pgtables + 0x4000



  




