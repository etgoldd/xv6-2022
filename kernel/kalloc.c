// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

typedef struct {
  struct spinlock lock;
  struct run *freelist;
} kmem_cpu;

void freerange_cpu(void *pa_start, void *pa_end, kmem_cpu kmem);

static kmem_cpu kmems[NCPU]; 

kmem_cpu*
get_cpu_kmem() {
  int cpu_id;
  kmem_cpu* kmem;
  push_off();
  cpu_id = cpuid();
  kmem = kmems + cpu_id;
  pop_off();
  return kmem;
}


int get_cpuid() {
  int cpu_id;
  push_off();
  cpu_id = cpuid();
  pop_off();
  return cpu_id;
}


void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    initlock(&(kmems[i].lock), "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}


void
kfree_cpu(void *pa, kmem_cpu *kmem)
{
  struct run *r;
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  
  acquire(&(kmem->lock));
  r->next = kmem->freelist;
  kmem->freelist = r;
  
  release(&(kmem->lock));
} 

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  kmem_cpu* kmem;
  kmem = get_cpu_kmem();

  kfree_cpu(pa, kmem);
  
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  kmem_cpu *kmem;
  kmem = get_cpu_kmem();
  push_off();
  acquire(&(kmem->lock));
  r = kmem->freelist;

  if (r) {
    kmem->freelist = r->next;
    release(&(kmem->lock));

  }
  else {
    release(&(kmem->lock));

    int cur_cpu = get_cpuid();
    for (int i = 1; i < NCPU; i++) {
      kmem_cpu *other_kmem = kmems +((i + cur_cpu) % NCPU);
      acquire(&(other_kmem->lock));
      if (other_kmem->freelist == 0) {
        release(&(other_kmem->lock));
        continue;
      }
      r = other_kmem->freelist;
      other_kmem->freelist = r->next;
      
      int freelist_length = 0;
      struct run *p = other_kmem->freelist;
      while (p != 0) {
        p = p->next;
        freelist_length++;
      }
      if (freelist_length > 1) {
      kmem->freelist = other_kmem->freelist;

        p = kmem->freelist;
        if (freelist_length > 1){
          
        }
        for (int page_index = 0; page_index < freelist_length / 2 && p->next != 0; page_index++) {
          p = p->next;
        }
        other_kmem->freelist = p->next;
        p->next = 0;
      }
      release(&(other_kmem->lock));
      break;
    }
  }
  pop_off();
 
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}


void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  kmem_cpu *kmem;
  kmem = get_cpu_kmem();
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    kfree_cpu(p, kmem);
  }
  
}
