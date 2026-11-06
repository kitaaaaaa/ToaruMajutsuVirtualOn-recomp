/* rt_import_shim: reached from an unresolved import's stub with its index in r10 and the guest's
 * System V arguments in place. The index becomes rt_unimplemented's seventh argument (on the stack).
 * Entry rsp % 16 == 8: the push makes it 0 for the call. */
    .text
    .globl rt_import_shim
rt_import_shim:
    push %r10
    call rt_unimplemented
    add $8, %rsp
    ret
