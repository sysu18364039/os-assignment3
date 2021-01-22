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

// number of buckets
#define NBUCKET 13
// the length of lock name
#define LNAME_LEN 20

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
}bcache;

// each bucket has its lock
struct {
  struct spinlock lock;
  struct buf head;
}bucket[NBUCKET];

char bucket_lock_name[NBUCKET][LNAME_LEN];


void
binit(void)
{
  struct buf *b;
  int i;
  
  initlock(&bcache.lock, "bcache");
 
  for(i = 0; i < NBUCKET; i++){
     snprintf(bucket_lock_name[i], LNAME_LEN, "bcache.bucket%d", i);
     initlock(&bucket[i].lock, bucket_lock_name[i]);
     bucket[i].head.prev = &bucket[i].head;
     bucket[i].head.next = &bucket[i].head;   
  }

  
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
     b->next = bucket[0].head.next;
     b->prev = &bucket[0].head;
     initsleeplock(&b->lock, "buffer");
     bucket[0].head.next->prev = b;
     bucket[0].head.next = b;
     b->lu_time = -1;
     
     //printf("binit: b->lu_time = %d\n", b->lu_time);
     //printf("binit: b = %d", b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  //printf("bget: argu dev is %d blockno is %d\n", dev, blockno);

  int hash_n = blockno % NBUCKET;

  acquire(&bucket[hash_n].lock);

  // Is the block already cached?
  for(b = bucket[hash_n].head.next; b != &bucket[hash_n].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      //printf("bget: cache hit\n");
      b->refcnt++;
      release(&bucket[hash_n].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  release(&bucket[hash_n].lock);


  // Not cached.
  acquire(&bcache.lock);
  acquire(&bucket[hash_n].lock);

  //Maybe Other processes cache the block  
  for(b = bucket[hash_n].head.next; b != &bucket[hash_n].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket[hash_n].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  
  // Recycle the least recently used (LRU) unused buffer.
  struct buf *lru = 0;
  
  while(lru == 0){
    //printf("bget: lru = %d\n", lru);
    /* search the buffer whose lu_time is min and
     and refcnt == 0 */
    for(b = bcache.buf; b < bcache.buf + NBUF; b++){
      //printf("bget: b = %d\n", b);
      //printf("bget: b->refcnt = %d\n", b->refcnt);
      //printf("bget: b->lu_time = %d\n", b->lu_time);
      //printf("bget: b->blockno = %d\n", b->blockno);
      if(b->refcnt == 0 && (lru == 0 || b->lu_time < lru->lu_time)){
        lru = b;
        //printf("bget: lru to be b(b->blockno = %d)\n", b->blockno);
      }
    }
  
    if(lru){
      //printf("bget: in if(lru){...}\n");
      int lru_hash_n = lru->blockno % NBUCKET;
      if(lru_hash_n != hash_n){
        acquire(&bucket[lru_hash_n].lock);
      }
      // lru->refcnt may be changed if lru_hash_n != hash_n
      if(lru->refcnt != 0){
        lru = 0;
        if(lru_hash_n != hash_n){
	  release(&bucket[lru_hash_n].lock);
        }
        continue;
      }
      
      // lru move out from bucket[lru_hash_n]
      lru->next->prev = lru->prev;  
      lru->prev->next = lru->next;

      if(lru_hash_n != hash_n){
	release(&bucket[lru_hash_n].lock);
      }
      lru->dev = dev;
      lru->blockno = blockno;
      lru->valid = 0;
      lru->refcnt = 1;
      // move lru to the head of bucket[hash_n]' list
      lru->prev = &bucket[hash_n].head;
      lru->next = bucket[hash_n].head.next;
      bucket[hash_n].head.next->prev = lru;
      bucket[hash_n].head.next = lru;
      release(&bucket[hash_n].lock);
      release(&bcache.lock);
      acquiresleep(&lru->lock);
      return lru;
    }

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

  releasesleep(&b->lock);

  acquire(&bucket[b->blockno % NBUCKET].lock);
  b->refcnt--;
  //acquire(&tickslock);
  b->lu_time = ticks;
  //release(&tickslock);
  //printf("brelse: \n");
  release(&bucket[b->blockno % NBUCKET].lock);
}

void
bpin(struct buf *b) {
  acquire(&bucket[b->blockno % NBUCKET].lock);
  b->refcnt++;
  release(&bucket[b->blockno % NBUCKET].lock);
}

void
bunpin(struct buf *b) {
  acquire(&bucket[b->blockno % NBUCKET].lock);
  b->refcnt--;
  release(&bucket[b->blockno % NBUCKET].lock);
}


