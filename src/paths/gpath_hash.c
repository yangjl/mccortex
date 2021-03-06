#include "global.h"
#include "gpath_hash.h"
#include "hash_mem.h"
#include "util.h"
#include "binary_seq.h"
#include "misc/city.h"

// Entry is [hkey:5][gpindex:5] = 10 bytes

// We compare with REHASH_LIMIT(16)*bucket_size(<255) = 4080
// so we need 12 bits to have 2^12 = 4096 possibilities

// (1-(1/(2^12)))^4080 = 0.369 = 37% of entries would have zero collisions
// (1-(1/(2^16)))^4080 = 0.939 = 94% of entries would have zero collisions

#define PATH_HASH_UNSET (0xffffffffff)
#define PATH_HASH_ENTRY_ASSIGNED(x) ((x).hkey != PATH_HASH_UNSET)

void gpath_hash_alloc(GPathHash *gphash, GPathStore *gpstore, size_t mem_in_bytes)
{
  size_t cap_entries; uint64_t num_bkts = 0; uint8_t bkt_size = 0;

  // Decide on hash table capacity based on how much memory we can use
  cap_entries = mem_in_bytes / sizeof(GPEntry);
  hash_table_cap(cap_entries, &num_bkts, &bkt_size);
  cap_entries = num_bkts * bkt_size;

  size_t bktlocks_mem = roundup_bits2bytes(num_bkts);
  size_t mem = cap_entries*sizeof(GPEntry) + bktlocks_mem;

  char num_bkts_str[100], bkt_size_str[100], cap_str[100], mem_str[100];
  ulong_to_str(num_bkts, num_bkts_str);
  ulong_to_str(bkt_size, bkt_size_str);
  ulong_to_str(cap_entries, cap_str);
  bytes_to_str(mem, 1, mem_str);
  status("[GPathHash] Allocating table with %s entries, using %s", cap_str, mem_str);
  status("[GPathHash]  number of buckets: %s, bucket size: %s", num_bkts_str, bkt_size_str);

  GPEntry *table = ctx_malloc(cap_entries * sizeof(GPEntry));
  uint8_t *bktlocks = ctx_calloc(bktlocks_mem, sizeof(uint8_t));
  uint8_t *bucket_nitems = ctx_calloc(num_bkts, sizeof(uint8_t));

  ctx_assert(num_bkts * bkt_size == cap_entries);
  ctx_assert(cap_entries > 0);
  ctx_assert(sizeof(GPEntry) == 10);

  // Table all set to 1 to indicate empty
  memset(table, 0xff, cap_entries * sizeof(GPEntry));

  GPathHash tmp = {.gpstore = gpstore, .table = table,
                  .num_of_buckets = num_bkts,
                  .bucket_size = bkt_size,
                  .capacity = cap_entries,
                  .mask = num_bkts - 1,
                  .num_entries = 0,
                  .bucket_nitems = bucket_nitems,
                  .bktlocks = bktlocks};

  memcpy(gphash, &tmp, sizeof(GPathHash));
}

void gpath_hash_dealloc(GPathHash *gphash)
{
  ctx_free(gphash->bucket_nitems);
  ctx_free(gphash->bktlocks);
  ctx_free(gphash->table);
  memset(gphash, 0, sizeof(GPathHash));
}

void gpath_hash_reset(GPathHash *gphash)
{
  gphash->num_entries = 0;
  memset(gphash->table, 0xff, gphash->capacity * sizeof(GPEntry));
}

void gpath_hash_print_stats(const GPathHash *gphash)
{
  char entries_str[50], cap_str[50];
  ulong_to_str(gphash->num_entries, entries_str);
  ulong_to_str(gphash->capacity, cap_str);
  status("[GPathHash] Paths: %s / %s occupancy [%.2f%%]",
         entries_str, cap_str,
         (100.0 * gphash->num_entries) / gphash->capacity);
}

static inline bool _gphash_entries_match(const GPathSet *gpset, GPEntry entry,
                                         hkey_t hkey, GPathNew newgpath)
{
  ctx_assert(entry.hkey != PATH_HASH_UNSET);
  ctx_assert(entry.gpindex != PATH_HASH_UNSET);
  GPath epath = gpset->entries.data[entry.gpindex];
  return (hkey == entry.hkey && gpaths_are_equal(epath, newgpath));
}

// Use a bucket lock to find or add an entry
// Returns NULL if not found or inserted
static inline GPath* _find_or_add_in_bucket(GPathHash *gphash, uint64_t hash,
                                            hkey_t hkey, GPathNew newgpath,
                                            bool *found)
{
  const GPathSet *gpset = &gphash->gpstore->gpset;

  // Add GPath within a lock to ensure we do not add the same path more than
  // once
  bitlock_yield_acquire(gphash->bktlocks, hash);

  GPEntry *entry = gphash->table + hash * gphash->bucket_size;
  const GPEntry *end = entry + gphash->bucket_size;
  *found = false;

  for(; entry < end; entry++)
  {
    if(!PATH_HASH_ENTRY_ASSIGNED(*entry))
    {
      GPath *gpath = gpath_store_add_mt(gphash->gpstore, hkey, newgpath);
      *entry = (GPEntry){.hkey = hkey,
                         .gpindex = gpath - gpset->entries.data};
      __sync_fetch_and_add((volatile uint8_t*)&gphash->bucket_nitems[hash], 1);
      __sync_fetch_and_add((volatile size_t*)&gphash->num_entries, 1);
      bitlock_release(gphash->bktlocks, hash);
      return gpath;
    }
    else if(_gphash_entries_match(gpset, *entry, hkey, newgpath))
    {
      bitlock_release(gphash->bktlocks, hash);
      *found = true;
      return gpset->entries.data + entry->gpindex;
    }
  }

  bitlock_release(gphash->bktlocks, hash);

  return NULL;
}

// Lock free search bucket for match.
// We can traverse a full bucket without acquiring the lock first because
// items are added but never removed from the hash. This allows us to remove
// the use of locks and improve performance.
// Returns NULL if not found
static inline GPath* _find_in_bucket(const GPathHash *gphash, uint64_t hash,
                                     hkey_t hkey, GPathNew newgpath)
{
  const GPathSet *gpset = &gphash->gpstore->gpset;
  const GPEntry *entry = gphash->table + hash * gphash->bucket_size;
  const GPEntry *end = entry + gphash->bucket_size;

  for(; entry < end; entry++)
    if(_gphash_entries_match(gpset, *entry, hkey, newgpath))
      return gpset->entries.data + entry->gpindex;

  return NULL;
}

// Returns NULL if out of memory
// Thread Safe: uses bucket level locks
GPath* gpath_hash_find_or_insert_mt(GPathHash *gphash,
                                    hkey_t hkey, GPathNew newgpath,
                                    bool *found)
{
  ctx_assert(newgpath.seq != NULL);
  ctx_assert(gphash->table != NULL);
  ctx_assert(hkey < PATH_HASH_UNSET);

  *found = false;

  size_t i, mem = (newgpath.num_juncs+3)/4;
  uint64_t hash = hkey;
  GPath *gpath = NULL;

  for(i = 0; i < REHASH_LIMIT; i++)
  {
    hash = CityHash64WithSeeds((const char*)newgpath.seq, mem, hash, i);
    hash &= gphash->mask;

    uint8_t bucket_fill = *(volatile uint8_t *)&gphash->bucket_nitems[hash];

    if(bucket_fill < gphash->bucket_size)
      gpath = _find_or_add_in_bucket(gphash, hash, hkey, newgpath, found);
    else {
      gpath = _find_in_bucket(gphash, hash, hkey, newgpath);
      *found = (gpath != NULL);
    }

    if(gpath != NULL) return gpath;
  }

  // Out of space
  die("[GPathHash] Out of memory (%zu / %zu occupancy [%.2f%%])",
      (size_t)gphash->num_entries, (size_t)gphash->capacity,
      (100.0 * gphash->num_entries) / gphash->capacity);
}
