#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define NUM_OF_DIRECT_POINTER 120
#define NUM_OF_INDIRECT_POINTER 4
#define INDIRECT_POINTERS_PRE_SECTOR BLOCK_SECTOR_SIZE / sizeof(block_sector_t)  // should be 128

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct_pointer[NUM_OF_DIRECT_POINTER];   // each pointer points to one sector of file data
    block_sector_t indirect_pointer[NUM_OF_INDIRECT_POINTER];
    block_sector_t double_indirect_pointer;

    off_t length;                       /* File size in bytes. include the last byte for EOF*/
    bool is_dir;                    /* True if inode is a directory */
    unsigned magic;                     /* Magic number. */
  };

  struct inode_indirect_pointer {
   block_sector_t sector_ptr[INDIRECT_POINTERS_PRE_SECTOR];  // each element contains the sector number pointed by corresponding pointer
 };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
    struct lock lock_inode;
  };

static bool inode_allocate(struct inode_disk *inoded, off_t length);
static void inode_deallocate(struct inode_disk *inoded);


static block_sector_t _byte_to_sector(const struct inode_disk * inoded, off_t sector_idx);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  bool lock_held = lock_held_by_current_thread (&inode->lock_inode);
  if (!lock_held) lock_acquire(&inode->lock_inode);
  block_sector_t ret;
  if (pos < inode->data.length) { // < because last byte is terminator
     ret = _byte_to_sector(&inode->data, pos/BLOCK_SECTOR_SIZE);
 }
  else ret = -1;

  if (!lock_held) lock_release(&inode->lock_inode);
  return ret;
}

/**
   sector_idx: the offset from the start of the inode data in unit of sector
   Assumes sector_idx is within the allocated data sectors of the file (checked in byte_to_sector)
*/
static block_sector_t _byte_to_sector(const struct inode_disk * inoded, off_t sector_idx) {
   off_t sector_limit = NUM_OF_DIRECT_POINTER * 1;   // the maximum number of sectors can be used for file; starting with the number of direct pointers, will grow
   // times 1 is just an indication that each pointer points to 1 sector
   off_t cur_base = 0;
   if (sector_idx < sector_limit) {  // must be less, since equal means it starts at the end the sector limit, so its data is in the next sector
      return inoded->direct_pointer[sector_idx];  // returns the sector num of the sector we want
   }
   cur_base = sector_limit;
   // now extend to indirect pointers
   for (int i=0; i<NUM_OF_INDIRECT_POINTER; i++) {
      sector_limit += INDIRECT_POINTERS_PRE_SECTOR * 1;  // each indirect pointer creates INDIRECT_POINTERS_PRE_SECTOR number of sectors available for file data
      if (sector_idx < sector_limit) {
         struct inode_indirect_pointer *indptr = malloc(sizeof(struct inode_indirect_pointer));
         block_read (fs_device, inoded->indirect_pointer[i], indptr);
         off_t indirect_offset = sector_idx - cur_base;  // the nth pointer in the region pointed by this indirect pointer
         block_sector_t ret = indptr->sector_ptr[indirect_offset]; // returned is the sector num of the file data sector
         free(indptr);
         return ret;
      }
      cur_base = sector_limit;
   }

   // now move on to double indirect pointer
   sector_limit += INDIRECT_POINTERS_PRE_SECTOR * 1 * INDIRECT_POINTERS_PRE_SECTOR * 1;
   if (sector_idx < sector_limit) {
      struct inode_indirect_pointer *level1ptr = malloc(sizeof(struct inode_indirect_pointer));
      struct inode_indirect_pointer *level2ptr = malloc(sizeof(struct inode_indirect_pointer));
      block_read (fs_device, inoded->double_indirect_pointer, level1ptr);
      off_t first_level_off = (sector_idx - cur_base) / INDIRECT_POINTERS_PRE_SECTOR;
      block_read (fs_device, level1ptr->sector_ptr[first_level_off], level2ptr);
      off_t second_level_off = (sector_idx - cur_base) % INDIRECT_POINTERS_PRE_SECTOR;
      block_sector_t ret = level2ptr->sector_ptr[second_level_off];
      free(level1ptr);
      free(level2ptr);
      return ret;
   }

   NOT_REACHED ();
   return -1;
}




/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
}

/*
Initializes an inode with LENGTH bytes of data and
writes the new inode to sector SECTOR on the file system
device.
Returns true if successful.
Returns false if memory or disk allocation fails.
*/
bool inode_create (block_sector_t sector, off_t length, bool is_dir) {
   struct inode_disk *disk_inode = NULL;
   bool success = false;

   ASSERT (length >= 0);

   /* If this assertion fails, the inode structure is not exactly
   one sector in size, and you should fix that. */
   ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

   disk_inode = calloc (1, sizeof *disk_inode);
   if (disk_inode != NULL) {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      if (inode_allocate(disk_inode, disk_inode->length)) {
         block_write (fs_device, sector, disk_inode);
         success = true;
      }
      free (disk_inode);
   }
   return success;
}


/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->lock_inode);
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
   lock_acquire (&inode->lock_inode);
   if (inode != NULL)
     inode->open_cnt++;
   lock_release (&inode->lock_inode);
   return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks.
*/
void inode_close (struct inode *inode) {
   /* Ignore null pointer. */
   if (inode == NULL) return;

   lock_acquire (&inode->lock_inode);
   inode->open_cnt--;
   lock_release (&inode->lock_inode);

   /* Release resources if this was the last opener. */
   if (inode->open_cnt == 0) {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed) {
         free_map_release (inode->sector, 1);
         inode_deallocate(&inode->data);
      }
      free (inode);
   }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}


/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
   {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      // if (sector_idx == (block_sector_t)-1)) break; don't need, will be resolved
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;  // this can be 0
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;  // this is never 0
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0) { // <=0 means min_left = inode_left <= 0, meaning reaching end of file (size must > 0)
        break;               // can be < 0 if seek was called
     }
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  if (bounce != NULL) free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.)
*/
off_t inode_write_at (struct inode *inode, const void *buffer_, off_t size, off_t offset) {
   const uint8_t *buffer = buffer_;
   off_t bytes_written = 0;
   uint8_t *bounce = NULL;

   if (inode->deny_write_cnt)
      return 0;

   while (size > 0)
   {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;  // this is never 0
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0) {  //  <=0 means min_left = inode_left <=0, meaning reaching end of file
         ASSERT(sector_idx == (block_sector_t)-1); // inside here means sector_idx == -1
         /*
         Extend when reached EOF
         Writing far beyond EOF can cause many blocks to be entirely zero.
         Some file systems allocate and write real data blocks for these implicitly
         zeroed blocks. Other file systems do not allocate these blocks at all until
         they are explicitly written. The latter file systems are said to support
         "sparse files." You may adopt either allocation strategy in your file system.
         We chose the former.
         */
         if (inode_allocate(&inode->data, offset+size)) {
            bool lock_held = lock_held_by_current_thread (&inode->lock_inode);
            if (!lock_held) lock_acquire(&inode->lock_inode);
            inode->data.length = offset + size;
            if (!lock_held) lock_release(&inode->lock_inode);
            block_write (fs_device, inode->sector, &inode->data);  // write the new inode information to sector
            // notice here is the only place besides inode_create that calls allocate
            // so only need to write inode_disk to sector here
            continue; // recalculate sector_idx and chuck size
         }
         else break; // error associate with allocation
      }

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
      {
         /* Write full sector directly to disk. */
         block_write (fs_device, sector_idx, buffer + bytes_written);
      }
      else
      {
         /* We need a bounce buffer. */
         if (bounce == NULL)
         {
            bounce = malloc (BLOCK_SECTOR_SIZE);
            if (bounce == NULL)
            break;
         }

         /* If the sector contains data before or after the chunk
         we're writing, then we need to read in the sector
         first.  Otherwise we start with a sector of all zeros. */
         if (sector_ofs > 0 || chunk_size < sector_left)
         block_read (fs_device, sector_idx, bounce);
         else  memset (bounce, 0, BLOCK_SECTOR_SIZE);
         memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
         block_write (fs_device, sector_idx, bounce);
      }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
   }
   if (bounce != NULL) free (bounce);

   return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
   lock_acquire (&inode->lock_inode);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->lock_inode);

}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_acquire (&inode->lock_inode);
  inode->deny_write_cnt--;
  lock_release (&inode->lock_inode);

}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}


static bool _inode_allocate(block_sector_t * ptr);
/**
   length: the TOTAL length of the file
*/
static bool inode_allocate(struct inode_disk *inoded, off_t length) {
   ASSERT (length >= 0);

   size_t num_of_sectors = bytes_to_sectors(length);  // number of sectors to be allocated (will decrease as we allocate)

   // direct pointers
   // n is the num of sectors we attempt to allocate in one trial
   int n = num_of_sectors < NUM_OF_DIRECT_POINTER ? num_of_sectors : NUM_OF_DIRECT_POINTER;
   for (int i=0; i < n; i++) {
      if (!_inode_allocate(&inoded->direct_pointer[i]))
         return false;
   }
   num_of_sectors -= n;
   if (num_of_sectors == 0) return true;  // done allocation

   // indirect pointers
   for (int i = 0; i < NUM_OF_INDIRECT_POINTER; i++) {
      if (!_inode_allocate(&inoded->indirect_pointer[i]))
         return false;
      // if the indirect pointer is already allocated, still possibly the next-level direct pointers are not pointing to meaningful sector.
      // read the sector storing all next-level direct pointers into local indptr
      struct inode_indirect_pointer *indptr = malloc(sizeof(struct inode_indirect_pointer));  // we implemented stack growth so hopefully this is fine
      block_read (fs_device, inoded->indirect_pointer[i], indptr);

      n = num_of_sectors < INDIRECT_POINTERS_PRE_SECTOR ? num_of_sectors : INDIRECT_POINTERS_PRE_SECTOR;
      // then start assigning data sector to those direct pointers
      for (off_t j=0; j<n; j++) {
         if (!_inode_allocate(&indptr->sector_ptr[j])) {
            free(indptr);
            return false;
         }
      }
      // then write content in local indptr to filesys
      block_write(fs_device, inoded->indirect_pointer[i], indptr);
      free(indptr);
      num_of_sectors -= n;
      if (num_of_sectors == 0) return true;  // done allocation
   }

   // double indirect pointer; only 1
   // double_ind_ptr -> level-1 ptr -> level-2 ptr -> data
   // allocate the double indirect pointer if not allocated yet
   if (!_inode_allocate(&inoded->double_indirect_pointer))
      return false;
   struct inode_indirect_pointer *level1ptr = malloc(sizeof(struct inode_indirect_pointer));
   struct inode_indirect_pointer *level2ptr = malloc(sizeof(struct inode_indirect_pointer));
   block_read (fs_device, inoded->double_indirect_pointer, level1ptr);

   // each element of level1ptr (sector pointer) still points to a sector full of pointers (may not yet be allocated)
   for (int i=0; i < INDIRECT_POINTERS_PRE_SECTOR; i++) {
      if (!_inode_allocate(&level1ptr->sector_ptr[i])) {
         free(level1ptr);
         free(level2ptr);
         return false;
      }
      block_read (fs_device, level1ptr->sector_ptr[i], level2ptr);
      // each element of level2ptr (sector ptr) points to a data sector
      // from now on see how many sectors we need for data (equivalent to how many level2ptr in this sector we need)
      n = num_of_sectors < INDIRECT_POINTERS_PRE_SECTOR ? num_of_sectors : INDIRECT_POINTERS_PRE_SECTOR;
      for (off_t j = 0; j < n; j++) {
         if (!_inode_allocate(&level2ptr->sector_ptr[j])) {
            free(level1ptr);
            free(level2ptr);
            return false;
         }
      }
      // then write content in local level2ptr to filesys
      block_write(fs_device, level1ptr->sector_ptr[i], level2ptr);
      num_of_sectors -= n;
      if (num_of_sectors == 0) {
         //  write content in local level1ptr to filesys (may be newly expanded sectors)
         block_write(fs_device, inoded->double_indirect_pointer, &level1ptr);
         free(level1ptr);
         free(level2ptr);
         return true;
      }
   }
   // num_of_sectors should reach 0 somewhere above
   // reaching here meaning the length is too large to fit in the allowed space
   NOT_REACHED();
   return false;
}

/* helper function */
static bool _inode_allocate(block_sector_t * ptr) {
   static char zeros[BLOCK_SECTOR_SIZE];  // a sector of 0

   if (*ptr == 0) {  // not allocated
      // allocate sector for the pointers (the sector may be used for pointers or file data)
      if(! free_map_allocate (1, ptr))  // indirect_pointer[i] should now contain the sector #
         return false;                  // its content should be pointers to the actual data sector
      block_write(fs_device, *ptr, zeros);  // init to zeros
   }
   return true;
}

static void inode_deallocate(struct inode_disk *inoded) {
   ASSERT (inoded->length >= 0);

   size_t num_of_sectors = bytes_to_sectors(inoded->length);  // number of sectors to be allocated (will decrease as we allocate)

   // direct pointers
   int n = num_of_sectors < NUM_OF_DIRECT_POINTER ? num_of_sectors : NUM_OF_DIRECT_POINTER;
   for (int i=0; i < n; i++)
      free_map_release (inoded->direct_pointer[i], 1);
   num_of_sectors -= n;
   if (num_of_sectors == 0) return;

   // indirect pointers
   for (int i = 0; i < NUM_OF_INDIRECT_POINTER; i++) {
      free_map_release (inoded->indirect_pointer[i], 1); // shouldnt matter to do this first––not ereasing its content
      struct inode_indirect_pointer indptr;  // we implemented stack growth so hopefully this is fine
      block_read (fs_device, inoded->indirect_pointer[i], &indptr);

      n = num_of_sectors < INDIRECT_POINTERS_PRE_SECTOR ? num_of_sectors : INDIRECT_POINTERS_PRE_SECTOR;
      // then start assigning data sector to those direct pointers
      for (off_t j=0; j<n; j++)
         free_map_release (indptr.sector_ptr[j], 1);
      num_of_sectors -= n;
      if (num_of_sectors == 0) return;  // done
   }

   // double indirect pointer; only 1
   // double_ind_ptr -> level-1 ptr -> level-2 ptr -> data
   free_map_release (inoded->double_indirect_pointer, 1); // shouldnt matter to do this first––not ereasing its content
   struct inode_indirect_pointer level1ptr, level2ptr;
   block_read (fs_device, inoded->double_indirect_pointer, &level1ptr);

   for (int i=0; i < INDIRECT_POINTERS_PRE_SECTOR; i++) {
      free_map_release (level1ptr.sector_ptr[i], 1);
      block_read (fs_device, level1ptr.sector_ptr[i], &level2ptr);
      // each element of level2ptr (sector ptr) points to a data sector
      n = num_of_sectors < INDIRECT_POINTERS_PRE_SECTOR ? num_of_sectors : INDIRECT_POINTERS_PRE_SECTOR;
      for (off_t j = 0; j < n; j++)
         free_map_release (level2ptr.sector_ptr[j], 1);
      num_of_sectors -= n;
      if (num_of_sectors == 0) return;
   }

   NOT_REACHED();
}

bool inode_is_directory(struct inode *inode){
   return inode->data.is_dir;
}

bool inode_is_removed(struct inode *inode) {
   return inode->removed;
}