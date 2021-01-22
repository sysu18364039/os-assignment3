// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define NAMELEN 8

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];


char lock_name[NCPU][NAMELEN];

void
kinit()
{
  int i;
  // one lock one name and initialize each lock
  for (i = 0; i < NCPU; i++){
     /* wirte "'kmem' + i" to lockname[i], the size
        of lockname[i] is NAMELEN */
     snprintf(lock_name[i], NAMELEN, "kmem%d", i);
     initlock(&kmem[i].lock, lock_name[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  
  // disable interrupts to avoid deadlock
  push_off();
  // get this core's number
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if(!r){
    int i;
    int id_n;
    /** 
     * the CPU[id]'s freelist is empty, 
     * search and "steal" part of the other CPU's free list
     */
    for (i = 1; i < NCPU; i++){
       id_n = (id + i) % NCPU;
       acquire(&kmem[id_n].lock);
       r = kmem[id_n].freelist;
       if(r){
         kmem[id_n].freelist = r->next;
         release(&kmem[id_n].lock);
         break;
       }
       release(&kmem[id_n].lock);
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  pop_off();

  return (void*)r;
}
