// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13
#define BUCKETSZ (NBUF / NBUCKETS)
// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;
struct {
  struct spinlock locks[NBUCKETS];
  struct buf buckets[NBUCKETS];
  struct buf bufs[NBUF];
} bcache;

int get_bindex(uint dev, uint blockno) {
  // Calculates the correct bucket for this block
  return (dev + blockno) % NBUCKETS;
}

void print_length(int index) {
  int x = 0;
  acquire(&bcache.locks[index]);
  struct buf *head = &bcache.buckets[index];
  for (struct buf *b = head->next; b != head; b = b->next) {
    x++;
  }
  release(&bcache.locks[index]);
}

void
binit(void)
{
  struct buf *b;
  struct buf *head;
  for (int i = 0; i < NBUCKETS; i++) {
    initlock(&(bcache.locks[i]),"bcache.bucket");
    head = &bcache.buckets[i];
    head->prev = head;
    head->next = head;
  }
  
  // Initialising the bufs
  // int offset = 0;
  for (b = bcache.bufs; b < bcache.bufs+NBUF; b++) {
    // b->dev = -1;
    // b->blockno = offset;
    // This gives the bufs values that will cycle through all the buckets
    int bindex = get_bindex(b->dev, b->blockno);
    head = &bcache.buckets[bindex];
    b->next = head->next;
    b->prev = head;
    initsleeplock(&b->lock, "buffer");
    head->next->prev = b;
    head->next = b;
    
    // offset++;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *head;
  // Is the block already cached?
  int bucket_index = get_bindex(dev, blockno);
  acquire(&bcache.locks[bucket_index]);
  head = &bcache.buckets[bucket_index];
  for(b = head->next; b != head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.locks[bucket_index]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Searching the current bucket first because its much easier
  int x = 0;
  for (b = head->prev; b != head; b = b->prev) {
    x++;
    if (b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      acquiresleep(&b->lock);
      release(&bcache.locks[bucket_index]);
      return b;
    }
  } 

  release(&bcache.locks[bucket_index]);
  for (int bindex = bucket_index + 1; bindex < bucket_index + NBUCKETS; bindex++) {
    int smaller = bindex % NBUCKETS, larger = bucket_index;
    // Avoiding deadlock by dropping all locks and reacquiring them
    if (bucket_index < (bindex % NBUCKETS)) {
      smaller = bucket_index;
      larger = bindex % NBUCKETS;
    }
    acquire(&bcache.locks[smaller]);
    acquire(&bcache.locks[larger]);
    head = &bcache.buckets[bindex % NBUCKETS];
    for (b = head->prev; b != head; b = b->prev) {
      if(b->refcnt == 0) {
        // Moving buckets
        b->prev->next = b->next;
        b->next->prev = b->prev;
        head = &bcache.buckets[bucket_index];
        b->next = head->next;
        b->prev = head;
        b->next->prev = b;
        head->next = b;
        
        // Taking the buffer
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        acquiresleep(&b->lock);
        release(&bcache.locks[larger]);
        release(&bcache.locks[smaller]);
        return b;
      }
    }
    release(&bcache.locks[larger]);
    release(&bcache.locks[smaller]);
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  int block_bindex = get_bindex(b->dev, b->blockno);
  acquire(&bcache.locks[block_bindex]);
  b->refcnt--;
  release(&bcache.locks[block_bindex]);
  releasesleep(&b->lock);
}

void
bpin(struct buf *b) {
  int block_bindex = get_bindex(b->dev, b->blockno);
  acquire(&bcache.locks[block_bindex]);
  b->refcnt++;
  release(&bcache.locks[block_bindex]);
}

void
bunpin(struct buf *b) {
  int block_bindex = get_bindex(b->dev, b->blockno);
  acquire(&bcache.locks[block_bindex]);
  b->refcnt--;
  release(&bcache.locks[block_bindex]);
}


