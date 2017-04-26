/* Copyright (c) 2017, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include "or.h"

#include "config.h"
#include "conscache.h"
#include "storagedir.h"

#define CCE_MAGIC 0x17162253

/**
 * A consensus_cache_entry_t is a reference-counted handle to an
 * item in a consensus_cache_t.  It can be mmapped into RAM, or not,
 * depending whether it's currently in use.
 */
struct consensus_cache_entry_t {
  uint32_t magic; /**< Must be set to CCE_MAGIC */
  HANDLE_ENTRY(consensus_cache_entry, consensus_cache_entry_t);
  int32_t refcnt; /**< Reference count. */
  unsigned can_remove : 1; /**< If true, we want to delete this file. */
  /** If true, we intend to unmap this file as soon as we're done with it. */
  unsigned release_aggressively : 1;

  /** Filename for this object within the storage_dir_t */
  char *fname;
  /** Labels associated with this object. Immutable once the object
   * is created. */
  config_line_t *labels;
  /** Pointer to the cache that includes this entry (if any). */
  consensus_cache_t *in_cache;

  /** Since what time has this object been mapped into RAM, but with the cache
   * being the only having a reference to it? */
  time_t unused_since;
  /** mmaped contents of the underlying file.  May be NULL */
  tor_mmap_t *map;
  /** Length of the body within <b>map</b>. */
  size_t bodylen;
  /** Pointer to the body within <b>map</b>. */
  const uint8_t *body;
};

/**
 * A consensus_cache_t holds a directory full of labeled items.
 */
struct consensus_cache_t {
  /** Underling storage_dir_t to handle persistence */
  storage_dir_t *dir;
  /** List of all the entries in the directory. */
  smartlist_t *entries;
};

static void consensus_cache_clear(consensus_cache_t *cache);
static void consensus_cache_rescan(consensus_cache_t *);
static void consensus_cache_entry_map(consensus_cache_t *,
                                      consensus_cache_entry_t *);
static void consensus_cache_entry_unmap(consensus_cache_entry_t *ent);

/**
 * Helper: Open a consensus cache in subdirectory <b>subdir</b> of the
 * data directory, to hold up to <b>max_entries</b> of data.
 */
consensus_cache_t *
consensus_cache_open(const char *subdir, int max_entries)
{
  consensus_cache_t *cache = tor_malloc_zero(sizeof(consensus_cache_t));
  char *directory = get_datadir_fname(subdir);
  cache->dir = storage_dir_new(directory, max_entries);
  tor_free(directory);
  if (!cache->dir) {
    tor_free(cache);
    return NULL;
  }

  consensus_cache_rescan(cache);
  return cache;
}

/**
 * Tell the sandbox (if any) configured by <b>cfg</b> to allow the
 * operations that <b>cache</b> will need.
 */
int
consensus_cache_register_with_sandbox(consensus_cache_t *cache,
                                      struct sandbox_cfg_elem **cfg)
{
  return storage_dir_register_with_sandbox(cache->dir, cfg);
}

/**
 * Helper: clear all entries from <b>cache</b> (but do not delete
 * any that aren't marked for removal
 */
static void
consensus_cache_clear(consensus_cache_t *cache)
{
  consensus_cache_delete_pending(cache, 0);

  SMARTLIST_FOREACH_BEGIN(cache->entries, consensus_cache_entry_t *, ent) {
    ent->in_cache = NULL;
    consensus_cache_entry_decref(ent);
  } SMARTLIST_FOREACH_END(ent);
  smartlist_free(cache->entries);
  cache->entries = NULL;
}

/**
 * Drop all storage held by <b>cache</b>.
 */
void
consensus_cache_free(consensus_cache_t *cache)
{
  if (! cache)
    return;

  if (cache->entries) {
    consensus_cache_clear(cache);
  }
  storage_dir_free(cache->dir);
  tor_free(cache);
}

/**
 * Write <b>datalen</b> bytes of data at <b>data</b> into the <b>cache</b>,
 * labeling that data with <b>labels</b>.  On failure, return NULL. On
 * success, return a newly created consensus_cache_entry_t.
 *
 * The returned value will be owned by the cache, and you will have a
 * reference to it.  Call consensus_cache_entry_decref() when you are
 * done with it.
 *
 * The provided <b>labels</b> MUST have distinct keys: if they don't,
 * this API does not specify which values (if any) for the duplicate keys
 * will be considered.
 */
consensus_cache_entry_t *
consensus_cache_add(consensus_cache_t *cache,
                    const config_line_t *labels,
                    const uint8_t *data,
                    size_t datalen)
{
  char *fname = NULL;
  int r = storage_dir_save_labeled_to_file(cache->dir,
                                            labels, data, datalen, &fname);
  if (r < 0 || fname == NULL) {
    return NULL;
  }
  consensus_cache_entry_t *ent =
    tor_malloc_zero(sizeof(consensus_cache_entry_t));
  ent->magic = CCE_MAGIC;
  ent->fname = fname;
  ent->labels = config_lines_dup(labels);
  ent->in_cache = cache;
  ent->unused_since = TIME_MAX;
  smartlist_add(cache->entries, ent);
  /* Start the reference count at 2: the caller owns one copy, and the
   * cache owns another.
   */
  ent->refcnt = 2;

  return ent;
}

/**
 * Given a <b>cache</b>, return some entry for which <b>key</b>=<b>value</b>.
 * Return NULL if no such entry exists.
 *
 * Does not adjust reference counts.
 */
consensus_cache_entry_t *
consensus_cache_find_first(consensus_cache_t *cache,
                           const char *key,
                           const char *value)
{
  smartlist_t *tmp = smartlist_new();
  consensus_cache_find_all(tmp, cache, key, value);
  consensus_cache_entry_t *ent = NULL;
  if (smartlist_len(tmp))
    ent = smartlist_get(tmp, 0);
  smartlist_free(tmp);
  return ent;
}

/**
 * Given a <b>cache</b>, add every entry to <b>out<b> for which
 * <b>key</b>=<b>value</b>.  If <b>key</b> is NULL, add every entry.
 *
 * Do not add any entry that has been marked for removal.
 *
 * Does not adjust reference counts.
 */
void
consensus_cache_find_all(smartlist_t *out,
                         consensus_cache_t *cache,
                         const char *key,
                         const char *value)
{
  SMARTLIST_FOREACH_BEGIN(cache->entries, consensus_cache_entry_t *, ent) {
    if (ent->can_remove == 1) {
      /* We want to delete this; pretend it isn't there. */
      continue;
    }
    if (! key) {
      smartlist_add(out, ent);
      continue;
    }
    const char *found_val = consensus_cache_entry_get_value(ent, key);
    if (found_val && !strcmp(value, found_val)) {
      smartlist_add(out, ent);
    }
  } SMARTLIST_FOREACH_END(ent);
}

/**
 * Given a list of consensus_cache_entry_t, remove all those entries
 * that do not have <b>key</b>=<b>value</b> in their labels.
 *
 * Does not adjust reference counts.
 */
void
consensus_cache_filter_list(smartlist_t *lst,
                            const char *key,
                            const char *value)
{
  if (BUG(lst == NULL))
    return; // LCOV_EXCL_LINE
  if (key == NULL)
    return;
  SMARTLIST_FOREACH_BEGIN(lst, consensus_cache_entry_t *, ent) {
    const char *found_val = consensus_cache_entry_get_value(ent, key);
    if (! found_val || strcmp(value, found_val)) {
      SMARTLIST_DEL_CURRENT(lst, ent);
    }
  } SMARTLIST_FOREACH_END(ent);
}

/**
 * If <b>ent</b> has a label with the given <b>key</b>, return its
 * value.  Otherwise return NULL.
 *
 * The return value is only guaranteed to be valid for as long as you
 * hold a reference to <b>ent</b>.
 */
const char *
consensus_cache_entry_get_value(const consensus_cache_entry_t *ent,
                                const char *key)
{
  const config_line_t *match = config_line_find(ent->labels, key);
  if (match)
    return match->value;
  else
    return NULL;
}

/**
 * Return a pointer to the labels in <b>ent</b>.
 *
 * This pointer is only guaranteed to be valid for as long as you
 * hold a reference to <b>ent</b>.
 */
const config_line_t *
consensus_cache_entry_get_labels(const consensus_cache_entry_t *ent)
{
  return ent->labels;
}

/**
 * Increase the reference count of <b>ent</b>.
 */
void
consensus_cache_entry_incref(consensus_cache_entry_t *ent)
{
  if (BUG(ent->magic != CCE_MAGIC))
    return; // LCOV_EXCL_LINE
  ++ent->refcnt;
  ent->unused_since = TIME_MAX;
}

/**
 * Release a reference held to <b>ent</b>.
 *
 * If it was the last reference, ent will be freed. Therefore, you must not
 * use <b>ent</b> after calling this function.
 */
void
consensus_cache_entry_decref(consensus_cache_entry_t *ent)
{
  if (! ent)
    return;
  if (BUG(ent->refcnt <= 0))
    return; // LCOV_EXCL_LINE
  if (BUG(ent->magic != CCE_MAGIC))
    return; // LCOV_EXCL_LINE

  --ent->refcnt;

  if (ent->refcnt == 1 && ent->in_cache) {
    /* Only the cache has a reference: we don't need to keep the file
     * mapped */
    if (ent->map) {
      if (ent->release_aggressively) {
        consensus_cache_entry_unmap(ent);
      } else {
        ent->unused_since = approx_time();
      }
    }
    return;
  }

  if (ent->refcnt > 0)
    return;

  /* Refcount is zero; we can free it. */
  if (ent->map) {
    consensus_cache_entry_unmap(ent);
  }
  tor_free(ent->fname);
  config_free_lines(ent->labels);
  consensus_cache_entry_handles_clear(ent);
  memwipe(ent, 0, sizeof(consensus_cache_entry_t));
  tor_free(ent);
}

/**
 * Mark <b>ent</b> for deletion from the cache.  Deletion will not occur
 * until the cache is the only place that holds a reference to <b>ent</b>.
 */
void
consensus_cache_entry_mark_for_removal(consensus_cache_entry_t *ent)
{
  ent->can_remove = 1;
}

/**
 * Mark <b>ent</b> as the kind of entry that we don't need to keep mmap'd for
 * any longer than we're actually using it.
 */
void
consensus_cache_entry_mark_for_aggressive_release(consensus_cache_entry_t *ent)
{
  ent->release_aggressively = 1;
}

/**
 * Try to read the body of <b>ent</b> into memory if it isn't already
 * loaded.  On success, set *<b>body_out</b> to the body, *<b>sz_out</b>
 * to its size, and return 0.  On failure return -1.
 *
 * The resulting body pointer will only be valid for as long as you
 * hold a reference to <b>ent</b>.
 */
int
consensus_cache_entry_get_body(const consensus_cache_entry_t *ent,
                               const uint8_t **body_out,
                               size_t *sz_out)
{
  if (BUG(ent->magic != CCE_MAGIC))
    return -1; // LCOV_EXCL_LINE

  if (! ent->map) {
    if (! ent->in_cache)
      return -1;

    consensus_cache_entry_map((consensus_cache_t *)ent->in_cache,
                              (consensus_cache_entry_t *)ent);
    if (! ent->map) {
      return -1;
    }
  }

  *body_out = ent->body;
  *sz_out = ent->bodylen;
  return 0;
}

/**
 * Unmap every mmap'd element of <b>cache</b> that has been unused
 * since <b>cutoff</b>.
 */
void
consensus_cache_unmap_lazy(consensus_cache_t *cache, time_t cutoff)
{
  SMARTLIST_FOREACH_BEGIN(cache->entries, consensus_cache_entry_t *, ent) {
    tor_assert_nonfatal(ent->in_cache == cache);
    if (ent->refcnt > 1 || BUG(ent->in_cache == NULL)) {
      /* Somebody is using this entry right now */
      continue;
    }
    if (ent->unused_since > cutoff) {
      /* Has been unused only for a little while */
      continue;
    }
    if (ent->map == NULL) {
      /* Not actually mapped. */
      continue;
    }
    consensus_cache_entry_unmap(ent);
  } SMARTLIST_FOREACH_END(ent);
}

/**
 * Delete every element of <b>cache</b> has been marked with
 * consensus_cache_entry_mark_for_removal.  If <b>force</b> is false,
 * retain those entries which are not in use except by the cache.
 */
void
consensus_cache_delete_pending(consensus_cache_t *cache, int force)
{
  SMARTLIST_FOREACH_BEGIN(cache->entries, consensus_cache_entry_t *, ent) {
    tor_assert_nonfatal(ent->in_cache == cache);
    if (! force) {
      if (ent->refcnt > 1 || BUG(ent->in_cache == NULL)) {
        /* Somebody is using this entry right now */
        continue;
      }
    }
    if (ent->can_remove == 0) {
      /* Don't want to delete this. */
      continue;
    }
    if (BUG(ent->refcnt <= 0)) {
      continue; // LCOV_EXCL_LINE
    }

    SMARTLIST_DEL_CURRENT(cache->entries, ent);
    ent->in_cache = NULL;
    char *fname = tor_strdup(ent->fname); /* save a copy */
    consensus_cache_entry_decref(ent);
    storage_dir_remove_file(cache->dir, fname);
    tor_free(fname);
  } SMARTLIST_FOREACH_END(ent);
}

/**
 * Internal helper: rescan <b>cache</b> and rebuild its list of entries.
 */
static void
consensus_cache_rescan(consensus_cache_t *cache)
{
  if (cache->entries) {
    consensus_cache_clear(cache);
  }

  cache->entries = smartlist_new();
  const smartlist_t *fnames = storage_dir_list(cache->dir);
  SMARTLIST_FOREACH_BEGIN(fnames, const char *, fname) {
    tor_mmap_t *map = NULL;
    config_line_t *labels = NULL;
    const uint8_t *body;
    size_t bodylen;
    map = storage_dir_map_labeled(cache->dir, fname,
                                  &labels, &body, &bodylen);
    if (! map) {
      /* Can't load this; continue */
      log_warn(LD_FS, "Unable to map file %s from consensus cache: %s",
               escaped(fname), strerror(errno));
      continue;
    }
    consensus_cache_entry_t *ent =
      tor_malloc_zero(sizeof(consensus_cache_entry_t));
    ent->magic = CCE_MAGIC;
    ent->fname = tor_strdup(fname);
    ent->labels = labels;
    ent->refcnt = 1;
    ent->in_cache = cache;
    ent->unused_since = TIME_MAX;
    smartlist_add(cache->entries, ent);
    tor_munmap_file(map); /* don't actually need to keep this around */
  } SMARTLIST_FOREACH_END(fname);
}

/**
 * Make sure that <b>ent</b> is mapped into RAM.
 */
static void
consensus_cache_entry_map(consensus_cache_t *cache,
                          consensus_cache_entry_t *ent)
{
  if (ent->map)
    return;

  ent->map = storage_dir_map_labeled(cache->dir, ent->fname,
                                     NULL, &ent->body, &ent->bodylen);
  ent->unused_since = TIME_MAX;
}

/**
 * Unmap <b>ent</b> from RAM.
 *
 * Do not call this if something other than the cache is holding a reference
 * to <b>ent</b>
 */
static void
consensus_cache_entry_unmap(consensus_cache_entry_t *ent)
{
  ent->unused_since = TIME_MAX;
  if (!ent->map)
    return;

  tor_munmap_file(ent->map);
  ent->map = NULL;
  ent->body = NULL;
  ent->bodylen = 0;
  ent->unused_since = TIME_MAX;
}

HANDLE_IMPL(consensus_cache_entry, consensus_cache_entry_t, )

#ifdef TOR_UNIT_TESTS
/**
 * Testing only: Return true iff <b>ent</b> is mapped into memory.
 *
 * (In normal operation, this information is not exposed.)
 */
int
consensus_cache_entry_is_mapped(consensus_cache_entry_t *ent)
{
  if (ent->map) {
    tor_assert(ent->body);
    return 1;
  } else {
    tor_assert(!ent->body);
    return 0;
  }
}
#endif

