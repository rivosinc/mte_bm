Tool for experimenting with publicly availabe memory tagging hardware. This
is created to create a microbenchmark to evaluate value of elide tag check
mechanism in future memory tagging hardware. In short, elide tag check is a
mechanism using which memory access on a page marked as a tagged page in
page tables can be waived off from tag lookups and tag checks. Compiler can
use such hardware mechanism to insert such elision mechanism whenever deemed
safe on a memory access. Some example of such safe memory accesses could be
access to objects on stack, container objects on stack or objects in heap
whose entire lifetime and scope can be evaluated statically in compiler
passes. One way to elide tag checks can be by making sure the CPU elides
tag checks on stack pointer relative accesses and compiler making sure that
only local object to a function are the only objects accessed via stack
pointer register. However there can be situations where more mechanism may
be needed to elide tag checks (like register spilling due to register
pressure on compiler).

This tool (`mte_bm`) creates a benchmark to emulate `settag` operations on
memory chunks as if it were stack objects and have loads/stores on those
objects. Following methodology is followed:

1) Create a large memory granule array
2) Create an array which holds shuffled index values
3) Use these shuffled index values to index into memory granule array
4) set tag (or not) for the selected memory granule(s)
5) Perform load/stores into these memory granules

In order to model the behavior of ensuring that ld/st (step 5) on memory
granules are independent of retirement of set tag operations (step 4),
two things can be done:

- Perform set tag to selected memory granule

			OR
- Perform set tag to an entirely different memory granule

In case of former, set tag will operate as nop because underlying page
itself it not tagged in page table entries.

In case of latter, set tag will operate as a store but shouldn't block
subsequent ld/st to different memory granule.

Latter models a behavior more closer to settag and subsequent ld/st not
being stalled on retirement of settag operation.
