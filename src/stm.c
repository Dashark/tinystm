/*
 * File:
 *   stm.c
 * Author(s):
 *   Pascal Felber <pascal.felber@unine.ch>
 * Description:
 *   STM functions.
 *
 * Copyright (c) 2007-2009.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <sched.h>

#include "stm.h"

#include "atomic.h"
#include "gc.h"

/* ################################################################### *
 * DEFINES
 * ################################################################### */

#define COMPILE_TIME_ASSERT(pred)       switch (0) { case 0: case pred: ; }

/* Designs */
#define WRITE_BACK_ETL                  0
#define WRITE_BACK_CTL                  1
#define WRITE_THROUGH                   2

static const char *design_names[] = {
  /* 0 */ "WRITE-BACK (ETL)",
  /* 1 */ "WRITE-BACK (CTL)",
  /* 2 */ "WRITE-THROUGH"
};

#ifndef DESIGN
# define DESIGN                         WRITE_BACK_ETL
#endif /* ! DESIGN */

/* Contention managers */
#define CM_SUICIDE                      0
#define CM_DELAY                        1
#define CM_BACKOFF                      2
#define CM_PRIORITY                     3

static const char *cm_names[] = {
  /* 0 */ "SUICIDE",
  /* 1 */ "DELAY",
  /* 2 */ "BACKOFF",
  /* 3 */ "PRIORITY"
};

#ifndef CM
# define CM                             CM_SUICIDE
#endif /* ! CM */

#if DESIGN == WRITE_BACK_CTL && CM == CM_PRIORITY
# error "PRIORITY contention manager cannot be used with CTL design"
#endif /* DESIGN == WRITE_BACK_CTL && CM == CM_PRIORITY */

#if DESIGN != WRITE_BACK_ETL && defined(READ_LOCKED_DATA)
# error "READ_LOCKED_DATA can only be used with WB-ETL design"
#endif /* DESIGN != WRITE_BACK_ETL && defined(READ_LOCKED_DATA) */

#if defined(CONFLICT_TRACKING) && ! defined(EPOCH_GC)
# error "CONFLICT_TRACKING requires EPOCH_GC"
#endif /* defined(CONFLICT_TRACKING) && ! defined(EPOCH_GC) */

#if defined(READ_LOCKED_DATA) && ! defined(EPOCH_GC)
# error "READ_LOCKED_DATA requires EPOCH_GC"
#endif /* defined(READ_LOCKED_DATA) && ! defined(EPOCH_GC) */

#ifdef EXPLICIT_TX_PARAMETER
# define TX_RETURN                      return tx
# define TX_GET                         /* Nothing */
#else /* ! EXPLICIT_TX_PARAMETER */
# define TX_RETURN                      /* Nothing */
# define TX_GET                         stm_tx_t *tx = stm_get_tx()
#endif /* ! EXPLICIT_TX_PARAMETER */

#ifdef DEBUG2
# ifndef DEBUG
#  define DEBUG
# endif /* ! DEBUG */
#endif /* DEBUG2 */

#ifdef DEBUG
/* Note: stdio is thread-safe */
# define IO_FLUSH                       fflush(NULL)
# define PRINT_DEBUG(...)               printf(__VA_ARGS__); fflush(NULL)
#else /* ! DEBUG */
# define IO_FLUSH
# define PRINT_DEBUG(...)
#endif /* ! DEBUG */

#ifdef DEBUG2
# define PRINT_DEBUG2(...)              PRINT_DEBUG(__VA_ARGS__)
#else /* ! DEBUG2 */
# define PRINT_DEBUG2(...)
#endif /* ! DEBUG2 */

#ifndef RW_SET_SIZE
# define RW_SET_SIZE                    4096                /* Initial size of read/write sets */
#endif /* ! RW_SET_SIZE */

#ifndef LOCK_ARRAY_LOG_SIZE
# define LOCK_ARRAY_LOG_SIZE            20                  /* Size of lock array: 2^20 = 1M */
#endif /* LOCK_ARRAY_LOG_SIZE */

#ifndef LOCK_SHIFT_EXTRA
# define LOCK_SHIFT_EXTRA               2                   /* 2 extra shift */
#endif /* LOCK_SHIFT_EXTRA */

#if CM == CM_BACKOFF
# ifndef MIN_BACKOFF
#  define MIN_BACKOFF                   (1UL << 2)
# endif /* MIN_BACKOFF */
# ifndef MAX_BACKOFF
#  define MAX_BACKOFF                   (1UL << 31)
# endif /* MAX_BACKOFF */
#endif /* CM == CM_BACKOFF */

#if CM == CM_PRIORITY
# define VR_THRESHOLD                   "VR_THRESHOLD"
# ifndef VR_THRESHOLD_DEFAULT
#  define VR_THRESHOLD_DEFAULT          3
# endif /* VR_THRESHOLD_DEFAULT */
static int vr_threshold;
# define CM_THRESHOLD                   "CM_THRESHOLD"
# ifndef CM_THRESHOLD_DEFAULT
#  define CM_THRESHOLD_DEFAULT          0
# endif /* CM_THRESHOLD_DEFAULT */
static int cm_threshold;
#endif /* CM == CM_PRIORITY */

#define XSTR(s)                         STR(s)
#define STR(s)                          #s

/* ################################################################### *
 * COMPATIBILITY FUNCTIONS
 * ################################################################### */

#if CM == CM_PRIORITY

# if defined(__CYGWIN__)
/* WIN32 (CYGWIN) */
#  include <malloc.h>
inline int posix_memalign(void **memptr, size_t alignment, size_t size)
{
  if ((*memptr = memalign(alignment, size)) == NULL)
    return 1;
  return 0;
}
# elif defined(__APPLE__)
/* OS X */
inline int posix_memalign(void **memptr, size_t alignment, size_t size)
{
  if ((*memptr = valloc(size)) == NULL)
    return 1;
  /* Assume that alignment is a power of 2 */
  if (((stm_word_t)*memptr & (alignment - 1)) != 0) {
    free (*memptr);
    return 1;
  }
  return 0;
}
# endif /* defined(__APPLE__) */

#endif /* CM == CM_PRIORITY */

/* ################################################################### *
 * TYPES
 * ################################################################### */

enum {                                  /* Transaction status */
  TX_IDLE = 0,
  TX_ACTIVE = 1,
  TX_COMMITTED = 2,
  TX_ABORTED = 3
};

typedef struct r_entry {                /* Read set entry */
  stm_word_t version;                   /* Version read */
  volatile stm_word_t *lock;            /* Pointer to lock (for fast access) */
} r_entry_t;

typedef struct r_set {                  /* Read set */
  r_entry_t *entries;                   /* Array of entries */
  int nb_entries;                       /* Number of entries */
  int size;                             /* Size of array */
} r_set_t;

typedef struct w_entry {                /* Write set entry */
  union {                               /* For padding... */
    struct {
      volatile stm_word_t *addr;        /* Address written */
      stm_word_t value;                 /* New (write-back) or old (write-through) value */
      stm_word_t mask;                  /* Write mask */
      stm_word_t version;               /* Version overwritten */
      volatile stm_word_t *lock;        /* Pointer to lock (for fast access) */
#if defined(READ_LOCKED_DATA) || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH)
      struct stm_tx *tx;                /* Transaction owning the write set */
#endif /* defined(READ_LOCKED_DATA) || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH) */
#if DESIGN == WRITE_BACK_ETL
      struct w_entry *next;             /* Next address covered by same lock (if any) */
#else /* DESIGN != WRITE_BACK_ETL */
      int no_drop;                      /* Should we drop lock upon abort? */
#endif /* DESIGN != WRITE_BACK_ETL */
    };
#if DESIGN == WRITE_BACK_ETL && CM == CM_PRIORITY
    stm_word_t padding[8];              /* Padding (must be a multiple of 32 bytes) */
#endif /* DESIGN == WRITE_BACK_ETL && CM == CM_PRIORITY */
  };
} w_entry_t;

typedef struct w_set {                  /* Write set */
  w_entry_t *entries;                   /* Array of entries */
  int nb_entries;                       /* Number of entries */
  int size;                             /* Size of array */
#if DESIGN == WRITE_BACK_ETL
  int reallocate;                       /* Reallocate on next start */
#elif DESIGN == WRITE_BACK_CTL
  int nb_acquired;                      /* Number of locks acquired */
# ifdef USE_BLOOM_FILTER
  stm_word_t bloom;                     /* Same Bloom filter as in TL2 */
# endif /* USE_BLOOM_FILTER */
#endif /* DESIGN == WRITE_BACK_CTL */
} w_set_t;

#ifndef MAX_SPECIFIC
# define MAX_SPECIFIC                   16
#endif /* MAX_SPECIFIC */

typedef struct stm_tx {                 /* Transaction descriptor */
  stm_tx_attr_t *attr;                  /* Transaction attributes (user-specified) */
  stm_word_t status;                    /* Transaction status (not read by other threads) */
  stm_word_t start;                     /* Start timestamp */
  stm_word_t end;                       /* End timestamp (validity range) */
  r_set_t r_set;                        /* Read set */
  w_set_t w_set;                        /* Write set */
  sigjmp_buf env;                       /* Environment for setjmp/longjmp */
  sigjmp_buf *jmp;                      /* Pointer to environment (NULL when not using setjmp/longjmp) */
  int nesting;                          /* Nesting level */
  int ro;                               /* Is this execution read-only? */
  int can_extend;                       /* Can this transaction be extended? */
  void *data[MAX_SPECIFIC];             /* Transaction-specific data (fixed-size array for better speed) */
#ifdef READ_LOCKED_DATA
  stm_word_t id;                        /* Instance number of the transaction */
#endif /* READ_LOCKED_DATA */
#ifdef CONFLICT_TRACKING
  pthread_t thread_id;                  /* Thread identifier (immutable) */
#endif /* CONFLICT_TRACKING */
#if CM == CM_DELAY || CM == CM_PRIORITY
  volatile stm_word_t *c_lock;          /* Pointer to contented lock (cause of abort) */
#endif /* CM == CM_DELAY || CM == CM_PRIORITY */
#if CM == CM_BACKOFF
  unsigned long backoff;                /* Maximum backoff duration */
  unsigned long seed;                   /* RNG seed */
#endif /* CM == CM_BACKOFF */
#if CM == CM_PRIORITY
  int priority;                         /* Transaction priority */
  int visible_reads;                    /* Should we use visible reads? */
#endif /* CM == CM_PRIORITY */
#if CM == CM_PRIORITY || defined(INTERNAL_STATS)
  unsigned long retries;                /* Number of consecutive aborts (retries) */
#endif /* CM == CM_PRIORITY || defined(INTERNAL_STATS) */
#ifdef INTERNAL_STATS
  unsigned long aborts;                 /* Total number of aborts (cumulative) */
  unsigned long aborts_ro;              /* Aborts due to wrong read-only specification (cumulative) */
  unsigned long aborts_locked_read;     /* Aborts due to trying to read when locked (cumulative) */
  unsigned long aborts_locked_write;    /* Aborts due to trying to write when locked (cumulative) */
  unsigned long aborts_validate_read;   /* Aborts due to failed validation upon read (cumulative) */
  unsigned long aborts_validate_write;  /* Aborts due to failed validation upon write (cumulative) */
  unsigned long aborts_validate_commit; /* Aborts due to failed validation upon commit (cumulative) */
  unsigned long aborts_invalid_memory;  /* Aborts due to invalid memory access (cumulative) */
# if DESIGN == WRITE_BACK_ETL
  unsigned long aborts_reallocate;      /* Aborts due to write set reallocation (cumulative) */
# endif /* DESIGN == WRITE_BACK_ETL */
# ifdef ROLLOVER_CLOCK
  unsigned long aborts_rollover;        /* Aborts due to clock rolling over (cumulative) */
# endif /* ROLLOVER_CLOCK */
# ifdef READ_LOCKED_DATA
  unsigned long locked_reads_ok;        /* Successful reads of previous value */
  unsigned long locked_reads_failed;    /* Failed reads of previous value */
# endif /* READ_LOCKED_DATA */
  unsigned long max_retries;            /* Maximum number of consecutive aborts (retries) */
#endif /* INTERNAL_STATS */
//  char pad[64];
} stm_tx_t;

static int nb_specific = 0;             /* Number of specific slots used (<= MAX_SPECIFIC) */

/*
 * Transaction nesting is supported in a minimalist way (flat nesting):
 * - When a transaction is started in the context of another
 *   transaction, we simply increment a nesting counter but do not
 *   actually start a new transaction.
 * - The environment to be used for setjmp/longjmp is only returned when
 *   no transaction is active so that it is not overwritten by nested
 *   transactions. This allows for composability as the caller does not
 *   need to know whether it executes inside another transaction.
 * - The commit of a nested transaction simply decrements the nesting
 *   counter. Only the commit of the top-level transaction will actually
 *   carry through updates to shared memory.
 * - An abort of a nested transaction will rollback the top-level
 *   transaction and reset the nesting counter. The call to longjmp will
 *   restart execution before the top-level transaction.
 * Using nested transactions without setjmp/longjmp is not recommended
 * as one would need to explicitly jump back outside of the top-level
 * transaction upon abort of a nested transaction. This breaks
 * composability.
 */

/*
 * Reading from the previous version of locked addresses is implemented
 * by peeking into the write set of the transaction that owns the
 * lock. Each transaction has a unique identifier, updated even upon
 * retry. A special "commit" bit of this identifier is set upon commit,
 * right before writing the values from the redo log to shared memory. A
 * transaction can read a locked address if the identifier of the owner
 * does not change between before and after reading the value and
 * version, and it does not have the commit bit set.
 */

/* ################################################################### *
 * CALLBACKS
 * ################################################################### */

typedef struct cb_entry {               /* Callback entry */
  void (*f)(TXPARAMS void *);           /* Function */
  void *arg;                            /* Argument to be passed to function */
} cb_entry_t;

#define MAX_CB                          16

/* Declare as static arrays (vs. lists) to improve cache locality */
static cb_entry_t init_cb[MAX_CB];      /* Init thread callbacks */
static cb_entry_t exit_cb[MAX_CB];      /* Exit thread callbacks */
static cb_entry_t start_cb[MAX_CB];     /* Start callbacks */
static cb_entry_t commit_cb[MAX_CB];    /* Commit callbacks */
static cb_entry_t abort_cb[MAX_CB];     /* Abort callbacks */

static int nb_init_cb = 0;
static int nb_exit_cb = 0;
static int nb_start_cb = 0;
static int nb_commit_cb = 0;
static int nb_abort_cb = 0;

#ifdef CONFLICT_TRACKING
static void (*conflict_cb)(stm_tx_t *, stm_tx_t *) = NULL;
#endif /* CONFLICT_TRACKING */

/* ################################################################### *
 * THREAD-LOCAL
 * ################################################################### */

#ifdef TLS
static __thread stm_tx_t* thread_tx;
#else /* ! TLS */
static pthread_key_t thread_tx;
#endif /* ! TLS */

/* ################################################################### *
 * LOCKS
 * ################################################################### */

/*
 * A lock is a unsigned int of the size of a pointer.
 * The LSB is the lock bit. If it is set, this means:
 * - At least some covered memory addresses is being written.
 * - Write-back (ETL): all bits of the lock apart from the lock bit form
 *   a pointer that points to the write log entry holding the new
 *   value. Multiple values covered by the same log entry and orginized
 *   in a linked list in the write log.
 * - Write-through and write-back (CTL): all bits of the lock apart from
 *   the lock bit form a pointer that points to the transaction
 *   descriptor containing the write-set.
 * If the lock bit is not set, then:
 * - All covered memory addresses contain consistent values.
 * - Write-back (ETL and CTL): all bits of the lock besides the lock bit
 *   contain a version number (timestamp).
 * - Write-through: all bits of the lock besides the lock bit contain a
 *   version number.
 *   - The high order bits contain the commit time.
 *   - The low order bits contain an incarnation number (incremented
 *     upon abort while writing the covered memory addresses).
 * When using the PRIORITY contention manager, the format of locks is
 * slightly different. It is documented elsewhere.
 */

#define OWNED_MASK                      0x01                /* 1 bit */
#if CM == CM_PRIORITY
# define WAIT_MASK                      0x02                /* 1 bit */
# define PRIORITY_BITS                  3                   /* 3 bits */
# define PRIORITY_MAX                   ((1 << PRIORITY_BITS) - 1)
# define PRIORITY_MASK                  (PRIORITY_MAX << 2)
# define ALIGNMENT                      (1 << (PRIORITY_BITS + 2))
# define ALIGNMENT_MASK                 (ALIGNMENT - 1)
#endif /* CM == CM_PRIORITY */
#if DESIGN == WRITE_THROUGH
# define INCARNATION_BITS               3                   /* 3 bits */
# define INCARNATION_MAX                ((1 << INCARNATION_BITS) - 1)
# define INCARNATION_MASK               (INCARNATION_MAX << 1)
# define VERSION_MAX                    (~(stm_word_t)0 >> (1 + INCARNATION_BITS))
#else /* DESIGN != WRITE_THROUGH */
# define VERSION_MAX                    (~(stm_word_t)0 >> 1)
#endif /* DESIGN != WRITE_THROUGH */

#define LOCK_GET_OWNED(l)               (l & OWNED_MASK)
#if CM == CM_PRIORITY
# define LOCK_SET_ADDR(a, p)            (a | (p << 2) | OWNED_MASK)
# define LOCK_GET_WAIT(l)               (l & WAIT_MASK)
# define LOCK_GET_PRIORITY(l)           ((l & PRIORITY_MASK) >> 2)
# define LOCK_SET_PRIORITY_WAIT(l, p)   ((l & ~(stm_word_t)PRIORITY_MASK) | (p << 2) | WAIT_MASK)
# define LOCK_GET_ADDR(l)               (l & ~(stm_word_t)(OWNED_MASK | WAIT_MASK | PRIORITY_MASK))
#else /* CM != CM_PRIORITY */
# define LOCK_SET_ADDR(a)               (a | OWNED_MASK)    /* OWNED bit set */
# define LOCK_GET_ADDR(l)               (l & ~(stm_word_t)OWNED_MASK)
#endif /* CM != CM_PRIORITY */
#if DESIGN == WRITE_THROUGH
# define LOCK_GET_TIMESTAMP(l)          (l >> (1 + INCARNATION_BITS))
# define LOCK_SET_TIMESTAMP(t)          (t << (1 + INCARNATION_BITS))
# define LOCK_GET_INCARNATION(l)        ((l & INCARNATION_MASK) >> 1)
# define LOCK_SET_INCARNATION(i)        (i << 1)            /* OWNED bit not set */
# define LOCK_UPD_INCARNATION(l, i)     ((l & ~(stm_word_t)(INCARNATION_MASK | OWNED_MASK)) | LOCK_SET_INCARNATION(i))
#else /* DESIGN != WRITE_THROUGH */
# define LOCK_GET_TIMESTAMP(l)          (l >> 1)         /* Logical shift (unsigned) */
# define LOCK_SET_TIMESTAMP(t)          (t << 1)            /* OWNED bit not set */
#endif /* DESIGN != WRITE_THROUGH */
#define LOCK_UNIT                       (~(stm_word_t)0)

/*
 * We use the very same hash functions as TL2 for degenerate Bloom
 * filters on 32 bits.
 */
#ifdef USE_BLOOM_FILTER
# define FILTER_HASH(a)                 (((stm_word_t)a >> 2) ^ ((stm_word_t)a >> 5))
# define FILTER_BITS(a)                 (1 << (FILTER_HASH(a) & 0x1F))
#endif /* USE_BLOOM_FILTER */

/*
 * We use an array of locks and hash the address to find the location of the lock.
 * We try to avoid collisions as much as possible (two addresses covered by the same lock).
 */
#define LOCK_ARRAY_SIZE                 (1 << LOCK_ARRAY_LOG_SIZE)
#define LOCK_MASK                       (LOCK_ARRAY_SIZE - 1)
#define LOCK_SHIFT                      (((sizeof(stm_word_t) == 4) ? 2 : 3) + LOCK_SHIFT_EXTRA)
#define LOCK_IDX(a)                     (((stm_word_t)(a) >> LOCK_SHIFT) & LOCK_MASK)
#ifdef LOCK_IDX_SWAP
# if LOCK_ARRAY_LOG_SIZE < 16
#  error "LOCK_IDX_SWAP requires LOCK_ARRAY_LOG_SIZE to be at least 16"
# endif /* LOCK_ARRAY_LOG_SIZE < 16 */
# define GET_LOCK(a)                    (locks + lock_idx_swap(LOCK_IDX(a)))
#else /* ! LOCK_IDX_SWAP */
# define GET_LOCK(a)                    (locks + LOCK_IDX(a))
#endif /* ! LOCK_IDX_SWAP */

static volatile stm_word_t locks[LOCK_ARRAY_SIZE];

/* ################################################################### *
 * CLOCK
 * ################################################################### */

#ifdef CLOCK_IN_CACHE_LINE
/* At least twice a cache line (512 bytes to be on the safe side) */
static volatile stm_word_t gclock[1024 / sizeof(stm_word_t)];
# define CLOCK                          (gclock[512 / sizeof(stm_word_t)])
#else /* ! CLOCK_IN_CACHE_LINE */
static volatile stm_word_t gclock;
# define CLOCK                          (gclock)
#endif /* ! CLOCK_IN_CACHE_LINE */

#define GET_CLOCK                       (ATOMIC_LOAD_ACQ(&CLOCK))
#define FETCH_INC_CLOCK                 (ATOMIC_FETCH_INC_FULL(&CLOCK))

/* ################################################################### *
 * STATIC
 * ################################################################### */

/*
 * Returns the transaction descriptor for the CURRENT thread.
 */
static inline stm_tx_t *stm_get_tx()
{
#ifdef TLS
  return thread_tx;
#else /* ! TLS */
  return (stm_tx_t *)pthread_getspecific(thread_tx);
#endif /* ! TLS */
}

#ifdef LOCK_IDX_SWAP
/*
 * Compute index in lock table (swap bytes to avoid consecutive addresses to have neighboring locks).
 */
static inline unsigned int lock_idx_swap(unsigned int idx) {
  return (idx & ~(unsigned int)0xFFFF) | ((idx & 0x00FF) << 8) | ((idx & 0xFF00) >> 8);
}
#endif /* LOCK_IDX_SWAP */

#ifdef ROLLOVER_CLOCK
/*
 * We use a simple approach for clock roll-over:
 * - We maintain the count of (active) transactions using a counter
 *   protected by a mutex. This approach is not very efficient but the
 *   cost is quickly amortized because we only modify the counter when
 *   creating and deleting a transaction descriptor, which typically
 *   happens much less often than starting and committing a transaction.
 * - We detect overflows when reading the clock or when incrementing it.
 *   Upon overflow, we wait until all threads have blocked on a barrier.
 * - Threads can block on the barrier upon overflow when they (1) start
 *   a transaction, or (2) delete a transaction. This means that threads
 *   must ensure that they properly delete their transaction descriptor
 *   before performing any blocking operation outside of a transaction
 *   in order to guarantee liveness (our model prohibits blocking
 *   inside a transaction).
 */

pthread_mutex_t tx_count_mutex;
pthread_cond_t tx_reset;
int tx_count;
int tx_overflow;

/*
 * Enter new transactional thread.
 */
static inline void stm_rollover_enter(stm_tx_t *tx)
{
  PRINT_DEBUG("==> stm_rollover_enter(%p)\n", tx);

  pthread_mutex_lock(&tx_count_mutex);
  while (tx_overflow != 0)
    pthread_cond_wait(&tx_reset, &tx_count_mutex);
  /* One more (active) transaction */
  tx_count++;
  pthread_mutex_unlock(&tx_count_mutex);
}

/*
 * Exit transactional thread.
 */
static inline void stm_rollover_exit(stm_tx_t *tx)
{
  PRINT_DEBUG("==> stm_rollover_exit(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  pthread_mutex_lock(&tx_count_mutex);
  /* One less (active) transaction */
  tx_count--;
  assert(tx_count >= 0);
  /* Are all transactions stopped? */
  if (tx_overflow != 0 && tx_count == 0) {
    /* Yes: reset clock */
    memset((void *)locks, 0, LOCK_ARRAY_SIZE * sizeof(stm_word_t));
    CLOCK = 0;
    tx_overflow = 0;
# ifdef EPOCH_GC
    /* Reset GC */
    gc_reset();
# endif /* EPOCH_GC */
    /* Wake up all thread */
    pthread_cond_broadcast(&tx_reset);
  }
  pthread_mutex_unlock(&tx_count_mutex);
}

/*
 * Clock overflow.
 */
static inline void stm_overflow(stm_tx_t *tx)
{
  PRINT_DEBUG("==> stm_overflow(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  pthread_mutex_lock(&tx_count_mutex);
  /* Set overflow flag (might already be set) */
  tx_overflow = 1;
  /* One less (active) transaction */
  tx_count--;
  assert(tx_count >= 0);
  /* Are all transactions stopped? */
  if (tx_count == 0) {
    /* Yes: reset clock */
    memset((void *)locks, 0, LOCK_ARRAY_SIZE * sizeof(stm_word_t));
    CLOCK = 0;
    tx_overflow = 0;
# ifdef EPOCH_GC
    /* Reset GC */
    gc_reset();
# endif /* EPOCH_GC */
    /* Wake up all thread */
    pthread_cond_broadcast(&tx_reset);
  } else {
    /* No: wait for other transactions to stop */
    pthread_cond_wait(&tx_reset, &tx_count_mutex);
  }
  /* One more (active) transaction */
  tx_count++;
  pthread_mutex_unlock(&tx_count_mutex);
}
#endif /* ROLLOVER_CLOCK */

/*
 * Check if stripe has been read previously.
 */
static inline r_entry_t *stm_has_read(stm_tx_t *tx, volatile stm_word_t *lock)
{
  r_entry_t *r;
  int i;

  PRINT_DEBUG("==> stm_has_read(%p[%lu-%lu],%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, lock);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

  /* Look for read */
  r = tx->r_set.entries;
  for (i = tx->r_set.nb_entries; i > 0; i--, r++) {
    if (r->lock == lock) {
      /* Return first match*/
      return r;
    }
  }
  return NULL;
}

#if DESIGN == WRITE_BACK_CTL
/*
 * Check if address has been written previously.
 */
static inline w_entry_t *stm_has_written(stm_tx_t *tx, volatile stm_word_t *addr)
{
  w_entry_t *w;
  int i;
# ifdef USE_BLOOM_FILTER
  stm_word_t mask;
# endif /* USE_BLOOM_FILTER */

  PRINT_DEBUG("==> stm_has_written(%p[%lu-%lu],%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, addr);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

# ifdef USE_BLOOM_FILTER
  mask = FILTER_BITS(addr);
  if ((tx->w_set.bloom & mask) != mask)
    return NULL;
# endif /* USE_BLOOM_FILTER */

  /* Look for write */
  w = tx->w_set.entries;
  for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
    if (w->addr == addr) {
      return w;
    }
  }
  return NULL;
}
#endif /* DESIGN == WRITE_BACK_CTL */

/*
 * (Re)allocate read set entries.
 */
static inline void stm_allocate_rs_entries(stm_tx_t *tx, int extend)
{
  if (extend) {
    /* Extend read set */
    tx->r_set.size *= 2;
    PRINT_DEBUG2("==> reallocate read set (%p[%lu-%lu],%d)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, tx->r_set.size);
    if ((tx->r_set.entries = (r_entry_t *)realloc(tx->r_set.entries, tx->r_set.size * sizeof(r_entry_t))) == NULL) {
      perror("realloc");
      exit(1);
    }
  } else {
    /* Allocate read set */
    if ((tx->r_set.entries = (r_entry_t *)malloc(tx->r_set.size * sizeof(r_entry_t))) == NULL) {
      perror("malloc");
      exit(1);
    }
  }
}

/*
 * (Re)allocate write set entries.
 */
static inline void stm_allocate_ws_entries(stm_tx_t *tx, int extend)
{
#if defined(READ_LOCKED_DATA) || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH)
  int i, first = (extend ? tx->w_set.size : 0);
#endif /* defined(READ_LOCKED_DATA) || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH) */

  if (extend) {
    /* Extend write set */
    tx->w_set.size *= 2;
    PRINT_DEBUG("==> reallocate write set (%p[%lu-%lu],%d)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, tx->w_set.size);
    if ((tx->w_set.entries = (w_entry_t *)realloc(tx->w_set.entries, tx->w_set.size * sizeof(w_entry_t))) == NULL) {
      perror("realloc");
      exit(1);
    }
  } else {
    /* Allocate write set */
#if CM == CM_PRIORITY && DESIGN == WRITE_BACK_ETL
    if (posix_memalign((void **)&tx->w_set.entries, ALIGNMENT, tx->w_set.size * sizeof(w_entry_t)) != 0) {
      fprintf(stderr, "Error: cannot allocate aligned memory\n");
      exit(1);
    }
#else /* CM != CM_PRIORITY || DESIGN != WRITE_BACK_ETL */
    if ((tx->w_set.entries = (w_entry_t *)malloc(tx->w_set.size * sizeof(w_entry_t))) == NULL) {
      perror("malloc");
      exit(1);
    }
#endif /* CM != CM_PRIORITY || DESIGN != WRITE_BACK_ETL */
  }

#if defined(READ_LOCKED_DATA) || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH)
  /* Initialize fields */
  for (i = first; i < tx->w_set.size; i++)
    tx->w_set.entries[i].tx = tx;
#endif /* defined(READ_LOCKED_DATA) || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH) */
}

/*
 * Validate read set (check if all read addresses are still valid now).
 */
static inline int stm_validate(stm_tx_t *tx)
{
  r_entry_t *r;
  int i;
  stm_word_t l;

  PRINT_DEBUG("==> stm_validate(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

  /* Validate reads */
  r = tx->r_set.entries;
  for (i = tx->r_set.nb_entries; i > 0; i--, r++) {
    /* Read lock */
    l = ATOMIC_LOAD(r->lock);
    /* Unlocked and still the same version? */
    if (LOCK_GET_OWNED(l)) {
      /* Do we own the lock? */
#if DESIGN == WRITE_THROUGH
      if ((stm_tx_t *)LOCK_GET_ADDR(l) != tx)
#else /* DESIGN != WRITE_THROUGH */
      w_entry_t *w = (w_entry_t *)LOCK_GET_ADDR(l);
      /* Simply check if address falls inside our write set (avoids non-faulting load) */
      if (!(tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries))
#endif /* DESIGN != WRITE_THROUGH */
      {
        /* Locked by another transaction: cannot validate */
        return 0;
      }
      /* We own the lock: OK */
#if DESIGN == WRITE_BACK_CTL
      if (w->version != r->version) {
        /* Other version: cannot validate */
        return 0;
      }
#endif /* DESIGN == WRITE_BACK_CTL */
    } else {
      if (LOCK_GET_TIMESTAMP(l) != r->version) {
        /* Other version: cannot validate */
        return 0;
      }
      /* Same version: OK */
    }
  }
  return 1;
}

/*
 * Extend snapshot range.
 */
static inline int stm_extend(stm_tx_t *tx)
{
  stm_word_t now;

  PRINT_DEBUG("==> stm_extend(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

  /* Get current time */
  now = GET_CLOCK;
#ifdef ROLLOVER_CLOCK
  if (now >= VERSION_MAX) {
    /* Clock overflow */
    return 0;
  }
#endif /* ROLLOVER_CLOCK */
  /* Try to validate read set */
  if (stm_validate(tx)) {
    /* It works: we can extend until now */
    tx->end = now;
    return 1;
  }
  return 0;
}

/*
 * Rollback transaction.
 */
static inline void stm_rollback(stm_tx_t *tx)
{
  w_entry_t *w;
#if DESIGN != WRITE_BACK_CTL
  int i;
#endif /* DESIGN != WRITE_BACK_CTL */
#if DESIGN == WRITE_THROUGH
  stm_word_t t;
#endif /* DESIGN == WRITE_THROUGH */
#if CM == CM_BACKOFF
  unsigned long wait;
  volatile int j;
#endif /* CM == CM_BACKOFF */
#ifdef READ_LOCKED_DATA
  stm_word_t id;
#endif /* READ_LOCKED_DATA */

  PRINT_DEBUG("==> stm_rollback(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

#if DESIGN == WRITE_THROUGH
  t = 0;
  /* Undo writes and drop locks (traverse in reverse order) */
  w = tx->w_set.entries + tx->w_set.nb_entries;
  while (w != tx->w_set.entries) {
    w--;
    PRINT_DEBUG2("==> undo(t=%p[%lu-%lu],a=%p,d=%p-%lu,v=%lu,m=0x%lx)\n",
                 tx, (unsigned long)tx->start, (unsigned long)tx->end, w->addr, (void *)w->value, (unsigned long)w->value, (unsigned long)w->version, (unsigned long)w->mask);
    if (w->mask != 0)
      ATOMIC_STORE(w->addr, w->value);
    if (w->no_drop)
      continue;
    /* Incarnation numbers allow readers to detect dirty reads */
    i = LOCK_GET_INCARNATION(w->version) + 1;
    if (i > INCARNATION_MAX) {
      /* Simple approach: write new version (might trigger unnecessary aborts) */
      if (t == 0) {
        t = FETCH_INC_CLOCK + 1;
        if (t >= VERSION_MAX) {
# ifdef ROLLOVER_CLOCK
          /* We can still use VERSION_MAX for protecting read-only trasanctions from dirty reads */
          t = VERSION_MAX;
# else /* ! ROLLOVER_CLOCK */
          fprintf(stderr, "Exceeded maximum version number: 0x%lx\n", (unsigned long)t);
          exit(1);
# endif /* ! ROLLOVER_CLOCK */
        }
      }
      ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(t));
    } else {
      /* Use new incarnation number */
      ATOMIC_STORE_REL(w->lock, LOCK_UPD_INCARNATION(w->version, i));
    }
  }
#elif DESIGN == WRITE_BACK_ETL
  /* Drop locks */
  i = tx->w_set.nb_entries;
  if (i > 0) {
# ifdef READ_LOCKED_DATA
    /* Update instance number (becomes odd) */
    id = tx->id;
    assert(id % 2 == 0);
    ATOMIC_STORE_REL(&tx->id, id + 1);
# endif /* READ_LOCKED_DATA */
    w = tx->w_set.entries;
    for (; i > 0; i--, w++) {
      if (w->next == NULL) {
        /* Only drop lock for last covered address in write set */
        ATOMIC_STORE(w->lock, LOCK_SET_TIMESTAMP(w->version));
      }
      PRINT_DEBUG2("==> discard(t=%p[%lu-%lu],a=%p,d=%p-%lu,v=%lu)\n",
                   tx, (unsigned long)tx->start, (unsigned long)tx->end, w->addr, (void *)w->value, (unsigned long)w->value, (unsigned long)w->version);
    }
    /* Make sure that all lock releases become visible */
    ATOMIC_MB_WRITE;
# ifdef READ_LOCKED_DATA
    /* Update instance number (becomes even) */
    ATOMIC_STORE_REL(&tx->id, id + 2);
# endif /* READ_LOCKED_DATA */
  }
#else /* DESIGN == WRITE_BACK_CTL */
  if (tx->w_set.nb_acquired > 0) {
    w = tx->w_set.entries + tx->w_set.nb_entries;
    do {
      w--;
      if (!w->no_drop) {
        if (--tx->w_set.nb_acquired == 0) {
          /* Make sure that all lock releases become visible to other threads */
          ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(w->version));
        } else {
          ATOMIC_STORE(w->lock, LOCK_SET_TIMESTAMP(w->version));
        }
        PRINT_DEBUG2("==> discard(t=%p[%lu-%lu],a=%p,d=%p-%lu,v=%lu)\n",
                     tx, (unsigned long)tx->start, (unsigned long)tx->end, w->addr, (void *)w->value, (unsigned long)w->value, (unsigned long)w->version);
      }
    } while (tx->w_set.nb_acquired > 0);
  }
#endif /* DESIGN == WRITE_BACK_CTL */

#if CM == CM_PRIORITY || defined(INTERNAL_STATS)
  tx->retries++;
#endif /* CM == CM_PRIORITY || defined(INTERNAL_STATS) */
#ifdef INTERNAL_STATS
  tx->aborts++;
  if (tx->max_retries < tx->retries)
    tx->max_retries = tx->retries;
#endif /* INTERNAL_STATS */

  /* Callbacks */
  if (nb_abort_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_abort_cb; cb++)
      abort_cb[cb].f(TXARGS abort_cb[cb].arg);
  }

  /* Set status (no need for CAS or atomic op) */
  tx->status = TX_ABORTED;

  /* Reset nesting level */
  tx->nesting = 0;

#if CM == CM_BACKOFF
  /* Simple RNG (good enough for backoff) */
  tx->seed ^= (tx->seed << 17);
  tx->seed ^= (tx->seed >> 13);
  tx->seed ^= (tx->seed << 5);
  wait = tx->seed % tx->backoff;
  for (j = 0; j < wait; j++) {
    /* Do nothing */
  }
  if (tx->backoff < MAX_BACKOFF)
    tx->backoff <<= 1;
#endif /* CM == CM_BACKOFF */

#if CM == CM_DELAY || CM == CM_PRIORITY
  /* Wait until contented lock is free */
  if (tx->c_lock != NULL) {
    /* Busy waiting (yielding is expensive) */
    while (LOCK_GET_OWNED(ATOMIC_LOAD(tx->c_lock))) {
# ifdef WAIT_YIELD
      sched_yield();
# endif /* WAIT_YIELD */
    }
    tx->c_lock = NULL;
  }
#endif /* CM == CM_DELAY || CM == CM_PRIORITY */
  /* Jump back to transaction start */
  if (tx->jmp != NULL)
    siglongjmp(*tx->jmp, 1);
}

/*
 * Store a word-sized value (return write set entry or NULL).
 */
static inline w_entry_t *stm_write(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  volatile stm_word_t *lock;
  stm_word_t l, version;
  w_entry_t *w;
#if DESIGN == WRITE_BACK_ETL
  w_entry_t *prev = NULL;
#elif DESIGN == WRITE_THROUGH
  int duplicate = 0;
#endif /* DESIGN == WRITE_THROUGH */

  PRINT_DEBUG2("==> stm_write(t=%p[%lu-%lu],a=%p,d=%p-%lu,m=0x%lx)\n",
               tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, (void *)value, (unsigned long)value, (unsigned long)mask);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

  if (tx->ro) {
    /* Disable read-only and abort */
    assert(tx->attr != NULL);
    tx->attr->ro = 0;
#ifdef INTERNAL_STATS
    tx->aborts_ro++;
#endif /* INTERNAL_STATS */
    stm_rollback(tx);
    return NULL;
  }

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Try to acquire lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (LOCK_GET_OWNED(l)) {
    /* Locked */
    /* Do we own the lock? */
#if DESIGN == WRITE_THROUGH
    if (tx == (stm_tx_t *)LOCK_GET_ADDR(l)) {
      /* Yes */
# ifdef NO_DUPLICATES_IN_RW_SETS
      int i;
      /* Check if address is in write set (a lock may cover multiple addresses) */
      w = tx->w_set.entries;
      for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
        if (w->addr == addr) {
          if (mask == 0)
            return w;
          if (w->mask == 0) {
            /* Remember old value */
            w->value = ATOMIC_LOAD(addr);
            w->mask = mask;
          }
          /* Yes: only write to memory */
          PRINT_DEBUG2("==> stm_write(t=%p[%lu-%lu],a=%p,l=%p,*l=%lu,d=%p-%lu,m=0x%lx)\n",
                       tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, lock, (unsigned long)l, (void *)value, (unsigned long)value, (unsigned long)mask);
          if (mask != ~(stm_word_t)0)
            value = (ATOMIC_LOAD(addr) & ~mask) | (value & mask);
          ATOMIC_STORE(addr, value);
          return w;
        }
      }
# endif /* NO_DUPLICATES_IN_RW_SETS */
      /* Mark entry so that we do not drop the lock upon undo */
      duplicate = 1;
      /* Must add to write set (may add entry multiple times) */
      goto do_write;
    }
#elif DESIGN == WRITE_BACK_ETL
    w = (w_entry_t *)LOCK_GET_ADDR(l);
    /* Simply check if address falls inside our write set (avoids non-faulting load) */
    if (tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries) {
      /* Yes */
      prev = w;
      /* Did we previously write the same address? */
      while (1) {
        if (addr == prev->addr) {
          if (mask == 0)
            return prev;
          /* No need to add to write set */
          PRINT_DEBUG2("==> stm_write(t=%p[%lu-%lu],a=%p,l=%p,*l=%lu,d=%p-%lu,m=0x%lx)\n",
                       tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, lock, (unsigned long)l, (void *)value, (unsigned long)value, (unsigned long)mask);
          if (mask != ~(stm_word_t)0) {
            if (prev->mask == 0)
              prev->value = ATOMIC_LOAD(addr);
            value = (prev->value & ~mask) | (value & mask);
          }
          prev->value = value;
          prev->mask |= mask;
          return prev;
        }
        if (prev->next == NULL) {
          /* Remember last entry in linked list (for adding new entry) */
          break;
        }
        prev = prev->next;
      }
      /* Get version from previous write set entry (all entries in linked list have same version) */
      version = prev->version;
      /* Must add to write set */
      if (tx->w_set.nb_entries == tx->w_set.size) {
        /* Extend write set (invalidate pointers to write set entries => abort and reallocate) */
        tx->w_set.size *= 2;
        tx->w_set.reallocate = 1;
# ifdef INTERNAL_STATS
        tx->aborts_reallocate++;
# endif /* INTERNAL_STATS */
        stm_rollback(tx);
        return NULL;
      }
      w = &tx->w_set.entries[tx->w_set.nb_entries];
# ifdef READ_LOCKED_DATA
      w->version = version;
# endif /* READ_LOCKED_DATA */
      goto do_write;
    }
#elif DESIGN == WRITE_BACK_CTL
    /* Spin while locked (should not last long) */
    goto restart;
#endif /* DESIGN == WRITE_BACK_CTL */
    /* Conflict: CM kicks in */
#if CM == CM_PRIORITY
    if (tx->retries >= cm_threshold) {
      if (LOCK_GET_PRIORITY(l) < tx->priority ||
          (LOCK_GET_PRIORITY(l) == tx->priority &&
# if DESIGN == WRITE_BACK_ETL
           l < (stm_word_t)tx->w_set.entries
# else /* DESIGN != WRITE_BACK_ETL */
           l < (stm_word_t)tx
# endif /* DESIGN != WRITE_BACK_ETL */
           && !LOCK_GET_WAIT(l))) {
        /* We have higher priority */
        if (ATOMIC_CAS_FULL(lock, l, LOCK_SET_PRIORITY_WAIT(l, tx->priority)) == 0)
          goto restart;
        l = LOCK_SET_PRIORITY_WAIT(l, tx->priority);
      }
      /* Wait until lock is free or another transaction waits for one of our locks */
      while (1) {
        int nb;
        stm_word_t lw;

        w = tx->w_set.entries;
        for (nb = tx->w_set.nb_entries; nb > 0; nb--, w++) {
          lw = ATOMIC_LOAD(w->lock);
          if (LOCK_GET_WAIT(lw)) {
            /* Another transaction waits for one of our locks */
            goto give_up;
          }
        }
        /* Did the lock we are waiting for get updated? */
        lw = ATOMIC_LOAD(lock);
        if (l != lw) {
          l = lw;
          goto restart_no_load;
        }
      }
    give_up:
      if (tx->priority < PRIORITY_MAX) {
        tx->priority++;
      } else {
        PRINT_DEBUG("Reached maximum priority\n");
      }
      tx->c_lock = lock;
    }
#elif CM == CM_DELAY
    tx->c_lock = lock;
#endif /* CM == CM_DELAY */
    /* Abort */
#ifdef INTERNAL_STATS
    tx->aborts_locked_write++;
#endif /* INTERNAL_STATS */
#ifdef CONFLICT_TRACKING
    if (conflict_cb != NULL && l != LOCK_UNIT) {
      /* Call conflict callback */
# if DESIGN == WRITE_THROUGH
      stm_tx_t *other = (stm_tx_t *)LOCK_GET_ADDR(l);
# else /* DESIGN != WRITE_THROUGH */
      stm_tx_t *other = ((w_entry_t *)LOCK_GET_ADDR(l))->tx;
# endif /* DESIGN != WRITE_THROUGH */
      conflict_cb(tx, other);
    }
#endif /* CONFLICT_TRACKING */
    stm_rollback(tx);
    return NULL;
  } else {
    /* Not locked */
#if DESIGN == WRITE_BACK_CTL
    w = stm_has_written(tx, addr);
    if (w != NULL) {
      w->value = (w->value & ~mask) | (value & mask);
      w->mask |= mask;
      return w;
    }
#endif /* DESIGN == WRITE_BACK_CTL */
    /* Handle write after reads (before CAS) */
    version = LOCK_GET_TIMESTAMP(l);
#if DESIGN == WRITE_THROUGH && defined(ROLLOVER_CLOCK)
    if (version == VERSION_MAX) {
      /* Cannot acquire lock on address with version VERSION_MAX: abort */
# ifdef INTERNAL_STATS
      tx->aborts_rollover++;
# endif /* INTERNAL_STATS */
      stm_rollback(tx);
      return NULL;
    }
#endif /* DESIGN == WRITE_THROUGH && defined(ROLLOVER_CLOCK) */
    if (version > tx->end) {
      /* We might have read an older version previously */
      if (!tx->can_extend || stm_has_read(tx, lock) != NULL) {
        /* Read version must be older (otherwise, tx->end >= version) */
        /* Not much we can do: abort */
#if CM == CM_PRIORITY
        /* Abort caused by invisible reads */
        tx->visible_reads++;
#endif /* CM == CM_PRIORITY */
#ifdef INTERNAL_STATS
        tx->aborts_validate_write++;
#endif /* INTERNAL_STATS */
        stm_rollback(tx);
        return NULL;
      }
    }
    /* Acquire lock (ETL) */
#if DESIGN == WRITE_THROUGH
# if CM == CM_PRIORITY
    if (ATOMIC_CAS_FULL(lock, l, LOCK_SET_ADDR((stm_word_t)tx, tx->priority)) == 0)
      goto restart;
# else /* CM != CM_PRIORITY */
    if (ATOMIC_CAS_FULL(lock, l, LOCK_SET_ADDR((stm_word_t)tx)) == 0)
      goto restart;
# endif /* CM != CM_PRIORITY */
#elif DESIGN == WRITE_BACK_ETL
    if (tx->w_set.nb_entries == tx->w_set.size) {
      /* Extend write set (invalidate pointers to write set entries => abort and reallocate) */
      tx->w_set.size *= 2;
      tx->w_set.reallocate = 1;
# ifdef INTERNAL_STATS
      tx->aborts_reallocate++;
# endif /* INTERNAL_STATS */
      stm_rollback(tx);
      return NULL;
    }
    w = &tx->w_set.entries[tx->w_set.nb_entries];
# ifdef READ_LOCKED_DATA
    w->version = version;
# endif /* READ_LOCKED_DATA */
# if CM == CM_PRIORITY
    if (ATOMIC_CAS_FULL(lock, l, LOCK_SET_ADDR((stm_word_t)w, tx->priority)) == 0)
      goto restart;
# else /* CM != CM_PRIORITY */
    if (ATOMIC_CAS_FULL(lock, l, LOCK_SET_ADDR((stm_word_t)w)) == 0)
      goto restart;
# endif /* CM != CM_PRIORITY */
#endif /* DESIGN == WRITE_BACK_ETL */
  }
  /* We own the lock here (ETL) */
do_write:
  PRINT_DEBUG2("==> stm_write(t=%p[%lu-%lu],a=%p,l=%p,*l=%lu,d=%p-%lu,m=0x%lx)\n",
               tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, lock, (unsigned long)l, (void *)value, (unsigned long)value, (unsigned long)mask);

  /* Add address to write set */
#if DESIGN != WRITE_BACK_ETL
  if (tx->w_set.nb_entries == tx->w_set.size)
    stm_allocate_ws_entries(tx, 1);
  w = &tx->w_set.entries[tx->w_set.nb_entries++];
#endif /* DESIGN != WRITE_BACK_ETL */
  w->addr = addr;
  w->mask = mask;
  w->lock = lock;
  if (mask == 0) {
    /* Do not write anything */
#ifndef NDEBUG
    w->value = 0;
#endif /* ! NDEBUG */
  } else
#if DESIGN == WRITE_THROUGH
  {
    /* Remember old value */
    w->value = ATOMIC_LOAD(addr);
  }
  /* We store the old value of the lock (timestamp and incarnation) */
  w->version = l;
  w->no_drop = duplicate;
  if (mask == 0)
    return w;
  if (mask != ~(stm_word_t)0)
    value = (w->value & ~mask) | (value & mask);
  ATOMIC_STORE(addr, value);
#elif DESIGN == WRITE_BACK_ETL
  {
    /* Remember new value */
    if (mask != ~(stm_word_t)0)
      value = (ATOMIC_LOAD(addr) & ~mask) | (value & mask);
    w->value = value;
  }
# ifndef READ_LOCKED_DATA
  w->version = version;
# endif /* ! READ_LOCKED_DATA */
  w->next = NULL;
  if (prev != NULL) {
    /* Link new entry in list */
    prev->next = w;
  }
  tx->w_set.nb_entries++;
#else /* DESIGN == WRITE_BACK_CTL */
  {
    /* Remember new value */
    w->value = value;
  }
# ifndef NDEBUG
  w->version = version;
# endif
  w->no_drop = 1;
# ifdef USE_BLOOM_FILTER
  tx->w_set.bloom |= FILTER_BITS(addr) ;
# endif /* USE_BLOOM_FILTER */
#endif /* DESIGN == WRITE_BACK_CTL */

  return w;
}

/*
 * Store a word-sized value in a unit transaction.
 */
static inline int stm_unit_write(volatile stm_word_t *addr, stm_word_t value, stm_word_t mask, stm_word_t *timestamp)
{
  volatile stm_word_t *lock;
  stm_word_t l;

  PRINT_DEBUG2("==> stm_unit_write(a=%p,d=%p-%lu,m=0x%lx)\n",
               addr, (void *)value, (unsigned long)value, (unsigned long)mask);

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Try to acquire lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
  if (LOCK_GET_OWNED(l)) {
    /* Locked: wait until lock is free */
#ifdef WAIT_YIELD
    sched_yield();
#endif /* WAIT_YIELD */
    goto restart;
  }
  /* Not locked */
  if (timestamp != NULL && LOCK_GET_TIMESTAMP(l) > *timestamp) {
    /* Return current timestamp */
    *timestamp = LOCK_GET_TIMESTAMP(l);
    return 0;
  }
  /* TODO: need to store thread ID to be able to kill it (for wait freedom) */
  if (ATOMIC_CAS_FULL(lock, l, LOCK_UNIT) == 0)
    goto restart;
  ATOMIC_STORE(addr, value);
  /* Update timestamp with newer (more recent) value */
  l = FETCH_INC_CLOCK + 1;
  if (timestamp != NULL)
    *timestamp = l;
  /* Make sure that lock release becomes visible */
  ATOMIC_STORE_REL(lock, LOCK_SET_TIMESTAMP(l));
  return 1;
}

/*
 * Catch signal (to emulate non-faulting load).
 */
static void signal_catcher(int sig)
{
  stm_tx_t *tx = stm_get_tx();

  /* A fault might only occur upon a load concurrent with a free (read-after-free) */
  PRINT_DEBUG("Caught signal: %d\n", sig);

  if (tx == NULL || tx->jmp == NULL) {
    /* There is not much we can do: execution will restart at faulty load */
    fprintf(stderr, "Error: invalid memory accessed and no longjmp destination\n");
    exit(1);
  }

#ifdef INTERNAL_STATS
  tx->aborts_invalid_memory++;
#endif /* INTERNAL_STATS */
  /* Will cause a longjmp */
  stm_rollback(tx);
}

/* ################################################################### *
 * STM FUNCTIONS
 * ################################################################### */

/*
 * Called once (from main) to initialize STM infrastructure.
 */
void stm_init()
{
#if CM == CM_PRIORITY
  char *s;
#endif /* CM == CM_PRIORITY */
  struct sigaction act;

  PRINT_DEBUG("==> stm_init()\n");

  PRINT_DEBUG("\tsizeof(word)=%d\n", (int)sizeof(stm_word_t));

  PRINT_DEBUG("\tVERSION_MAX=0x%lx\n", (unsigned long)VERSION_MAX);

  COMPILE_TIME_ASSERT(sizeof(stm_word_t) == sizeof(void *));
  COMPILE_TIME_ASSERT(sizeof(stm_word_t) == sizeof(atomic_t));
#if DESIGN == WRITE_BACK_ETL && CM == CM_PRIORITY
  COMPILE_TIME_ASSERT((sizeof(w_entry_t) & ALIGNMENT_MASK) == 0); /* Multiple of ALIGNMENT */
#endif /* DESIGN == WRITE_BACK_ETL && CM == CM_PRIORITY */

#ifdef EPOCH_GC
  gc_init(stm_get_clock);
#endif /* EPOCH_GC */

  memset((void *)locks, 0, LOCK_ARRAY_SIZE * sizeof(stm_word_t));

#if CM == CM_PRIORITY
  s = getenv(VR_THRESHOLD);
  if (s != NULL)
    vr_threshold = (int)strtol(s, NULL, 10);
  else
    vr_threshold = VR_THRESHOLD_DEFAULT;
  PRINT_DEBUG("\tVR_THRESHOLD=%d\n", vr_threshold);
  s = getenv(CM_THRESHOLD);
  if (s != NULL)
    cm_threshold = (int)strtol(s, NULL, 10);
  else
    cm_threshold = CM_THRESHOLD_DEFAULT;
  PRINT_DEBUG("\tCM_THRESHOLD=%d\n", cm_threshold);
#endif /* CM == CM_PRIORITY */

  CLOCK = 0;
#ifdef ROLLOVER_CLOCK
  if (pthread_mutex_init(&tx_count_mutex, NULL) != 0) {
    fprintf(stderr, "Error creating mutex\n");
    exit(1);
  }
  if (pthread_cond_init(&tx_reset, NULL) != 0) {
    fprintf(stderr, "Error creating condition variable\n");
    exit(1);
  }
  tx_count = 0;
  tx_overflow = 0;
#endif /* ROLLOVER_CLOCK */

#ifndef TLS
  if (pthread_key_create(&thread_tx, NULL) != 0) {
    fprintf(stderr, "Error creating thread local\n");
    exit(1);
  }
#endif /* ! TLS */

  /* Catch signals for non-faulting load */
  act.sa_handler = signal_catcher;
  act.sa_flags = 0;
  sigemptyset(&act.sa_mask);
  if (sigaction(SIGBUS, &act, NULL) < 0 || sigaction(SIGSEGV, &act, NULL) < 0) {
    perror("sigaction");
    exit(1);
  }
}

/*
 * Called once (from main) to clean up STM infrastructure.
 */
void stm_exit()
{
  PRINT_DEBUG("==> stm_exit()\n");

#ifndef TLS
  pthread_key_delete(thread_tx);
#endif /* ! TLS */
#ifdef ROLLOVER_CLOCK
  pthread_cond_destroy(&tx_reset);
  pthread_mutex_destroy(&tx_count_mutex);
#endif /* ROLLOVER_CLOCK */

#ifdef EPOCH_GC
  gc_exit();
#endif /* EPOCH_GC */
}

/*
 * Called by the CURRENT thread to initialize thread-local STM data.
 */
TXTYPE stm_init_thread()
{
  stm_tx_t *tx;

  PRINT_DEBUG("==> stm_init_thread()\n");

#ifdef EPOCH_GC
  gc_init_thread();
#endif /* EPOCH_GC */

  /* Allocate descriptor */
#if CM == CM_PRIORITY && DESIGN != WRITE_BACK_ETL
  if (posix_memalign((void **)&tx, ALIGNMENT, sizeof(stm_tx_t)) != 0) {
    fprintf(stderr, "Error: cannot allocate aligned memory\n");
    exit(1);
  }
#else /* CM != CM_PRIORITY || DESIGN == WRITE_BACK_ETL */
  if ((tx = (stm_tx_t *)malloc(sizeof(stm_tx_t))) == NULL) {
    perror("malloc");
    exit(1);
  }
#endif /* CM != CM_PRIORITY || DESIGN == WRITE_BACK_ETL */
  /* Set status (no need for CAS or atomic op) */
  tx->status = TX_IDLE;
  /* Read set */
  tx->r_set.nb_entries = 0;
  tx->r_set.size = RW_SET_SIZE;
  stm_allocate_rs_entries(tx, 0);
  /* Write set */
  tx->w_set.nb_entries = 0;
  tx->w_set.size = RW_SET_SIZE;
#if DESIGN == WRITE_BACK_ETL
  tx->w_set.reallocate = 0;
#elif DESIGN == WRITE_BACK_CTL
  tx->w_set.nb_acquired = 0;
# ifdef USE_BLOOM_FILTER
  tx->w_set.bloom = 0;
# endif /* USE_BLOOM_FILTER */
#endif /* DESIGN == WRITE_BACK_CTL */
  stm_allocate_ws_entries(tx, 0);
  /* Nesting level */
  tx->nesting = 0;
  /* Transaction-specific data */
  memset(tx->data, 0, MAX_SPECIFIC * sizeof(void *));
#ifdef READ_LOCKED_DATA
  /* Instance number */
  tx->id = 0;
#endif /* READ_LOCKED_DATA */
#ifdef CONFLICT_TRACKING
  /* Thread identifier */
  tx->thread_id = pthread_self();
#endif /* CONFLICT_TRACKING */
#if CM == CM_DELAY || CM == CM_PRIORITY
  /* Contented lock */
  tx->c_lock = NULL;
#endif /* CM == CM_DELAY || CM == CM_PRIORITY */
#if CM == CM_BACKOFF
  /* Backoff */
  tx->backoff = MIN_BACKOFF;
  tx->seed = 123456789UL;
#endif /* CM == CM_BACKOFF */
#if CM == CM_PRIORITY
  /* Priority */
  tx->priority = 0;
  tx->visible_reads = 0;
#endif /* CM == CM_PRIORITY */
#if CM == CM_PRIORITY || defined(INTERNAL_STATS)
  tx->retries = 0;
#endif /* CM == CM_PRIORITY || defined(INTERNAL_STATS) */
#ifdef INTERNAL_STATS
  /* Statistics */
  tx->aborts = 0;
  tx->aborts_ro = 0;
  tx->aborts_locked_read = 0;
  tx->aborts_locked_write = 0;
  tx->aborts_validate_read = 0;
  tx->aborts_validate_write = 0;
  tx->aborts_validate_commit = 0;
  tx->aborts_invalid_memory = 0;
# if DESIGN == WRITE_BACK_ETL
  tx->aborts_reallocate = 0;
# endif /* DESIGN == WRITE_BACK_ETL */
# ifdef ROLLOVER_CLOCK
  tx->aborts_rollover = 0;
# endif /* ROLLOVER_CLOCK */
# ifdef READ_LOCKED_DATA
  tx->locked_reads_ok = 0;
  tx->locked_reads_failed = 0;
# endif /* READ_LOCKED_DATA */
  tx->max_retries = 0;
#endif /* INTERNAL_STATS */
  /* Store as thread-local data */
#ifdef TLS
  thread_tx = tx;
#else /* ! TLS */
  pthread_setspecific(thread_tx, tx);
#endif /* ! TLS */
#ifdef ROLLOVER_CLOCK
  stm_rollover_enter(tx);
#endif /* ROLLOVER_CLOCK */

  /* Callbacks */
  if (nb_init_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_init_cb; cb++)
      init_cb[cb].f(TXARGS init_cb[cb].arg);
  }

  PRINT_DEBUG("==> %p\n", tx);

  TX_RETURN;
}

/*
 * Called by the CURRENT thread to cleanup thread-local STM data.
 */
void stm_exit_thread(TXPARAM)
{
#ifdef EPOCH_GC
  stm_word_t t;
#endif /* EPOCH_GC */
  TX_GET;

  PRINT_DEBUG("==> stm_exit_thread(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Callbacks */
  if (nb_exit_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_exit_cb; cb++)
      exit_cb[cb].f(TXARGS exit_cb[cb].arg);
  }

#ifdef ROLLOVER_CLOCK
  stm_rollover_exit(tx);
#endif /* ROLLOVER_CLOCK */

#ifdef EPOCH_GC
  t = GET_CLOCK;
  gc_free(tx->r_set.entries, t);
  gc_free(tx->w_set.entries, t);
  gc_free(tx, t);
  gc_exit_thread();
#else /* ! EPOCH_GC */
  free(tx->r_set.entries);
  free(tx->w_set.entries);
  free(tx);
#endif /* ! EPOCH_GC */
}

/*
 * Called by the CURRENT thread to start a transaction.
 */
void stm_start(TXPARAMS sigjmp_buf *env, stm_tx_attr_t *attr)
{
  TX_GET;

  PRINT_DEBUG("==> stm_start(%p)\n", tx);

  /* Increment nesting level */
  if (tx->nesting++ > 0)
    return;

  /* Use setjmp/longjmp? */
  tx->jmp = env;
  /* Attributes */
  tx->attr = attr;
#if CM == CM_PRIORITY
  if (attr != NULL && attr->ro && tx->visible_reads >= vr_threshold && vr_threshold >= 0) {
    /* Disable read-only */
    attr->ro = 0;
  }
#endif /* CM == CM_PRIORITY */
  tx->ro = (attr == NULL ? 0 : attr->ro);
  /* Set status (no need for CAS or atomic op) */
  tx->status = TX_ACTIVE;
 start:
  /* Start timestamp */
  tx->start = tx->end = GET_CLOCK; /* OPT: Could be delayed until first read/write */
  /* Allow extensions */
  tx->can_extend = 1;
#ifdef ROLLOVER_CLOCK
  if (tx->start >= VERSION_MAX) {
    /* Overflow: we must reset clock */
    stm_overflow(tx);
    goto start;
  }
#endif /* ROLLOVER_CLOCK */
  /* Read/write set */
#if DESIGN == WRITE_BACK_ETL
  if (tx->w_set.reallocate) {
    /* Don't need to copy the content from the previous write set */
# ifdef EPOCH_GC
    gc_free(tx->w_set.entries, tx->start);
# else /* ! EPOCH_GC */
    free(tx->w_set.entries);
# endif /* ! EPOCH_GC */
    stm_allocate_ws_entries(tx, 0);
    tx->w_set.reallocate = 0;
  }
#elif DESIGN == WRITE_BACK_CTL
  tx->w_set.nb_acquired = 0;
# ifdef USE_BLOOM_FILTER
  tx->w_set.bloom = 0;
# endif /* USE_BLOOM_FILTER */
#endif /* DESIGN == WRITE_BACK_CTL */
  tx->w_set.nb_entries = 0;
  tx->r_set.nb_entries = 0;

#ifdef EPOCH_GC
  gc_set_epoch(tx->start);
#endif /* EPOCH_GC */

  /* Callbacks */
  if (nb_start_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_start_cb; cb++)
      start_cb[cb].f(TXARGS start_cb[cb].arg);
  }
}

/*
 * Called by the CURRENT thread to commit a transaction.
 */
int stm_commit(TXPARAM)
{
  w_entry_t *w;
  stm_word_t t;
  int i;
#if DESIGN == WRITE_BACK_CTL
  stm_word_t l, value;
#endif /* DESIGN == WRITE_BACK_CTL */
#ifdef READ_LOCKED_DATA
  stm_word_t id;
#endif /* READ_LOCKED_DATA */
  TX_GET;

  PRINT_DEBUG("==> stm_commit(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

  /* Decrement nesting level */
  if (--tx->nesting > 0)
    return 1;

  if (tx->w_set.nb_entries > 0) {
    /* Update transaction */

#if DESIGN == WRITE_BACK_CTL
    /* Acquire locks (in reverse order) */
    w = tx->w_set.entries + tx->w_set.nb_entries;
    do {
      w--;
      /* Try to acquire lock */
 restart:
      l = ATOMIC_LOAD(w->lock);
      if (LOCK_GET_OWNED(l)) {
        /* Do we already own the lock? */
        if (tx->w_set.entries <= (w_entry_t *)LOCK_GET_ADDR(l) && (w_entry_t *)LOCK_GET_ADDR(l) < tx->w_set.entries + tx->w_set.nb_entries) {
          /* Yes: ignore */
          continue;
        }
        /* Conflict: CM kicks in */
# if CM == CM_DELAY
        tx->c_lock = w->lock;
# endif /* CM == CM_DELAY */
        /* Abort self */
# ifdef INTERNAL_STATS
        tx->aborts_locked_write++;
# endif /* INTERNAL_STATS */
        stm_rollback(tx);
        return 0;
      }
      if (ATOMIC_CAS_FULL(w->lock, l, LOCK_SET_ADDR((stm_word_t)w)) == 0)
        goto restart;
      /* We own the lock here */
      w->no_drop = 0;
      /* Store version for validation of read set */
      w->version = LOCK_GET_TIMESTAMP(l);
      tx->w_set.nb_acquired++;
    } while (w > tx->w_set.entries);
#endif /* DESIGN == WRITE_BACK_CTL */

    /* Get commit timestamp */
    t = FETCH_INC_CLOCK + 1;
    if (t >= VERSION_MAX) {
#ifdef ROLLOVER_CLOCK
      /* Abort: will reset the clock on next transaction start or delete */
# ifdef INTERNAL_STATS
      tx->aborts_rollover++;
# endif /* INTERNAL_STATS */
      stm_rollback(tx);
      return 0;
#else /* ! ROLLOVER_CLOCK */
      fprintf(stderr, "Exceeded maximum version number: 0x%lx\n", (unsigned long)t);
      exit(1);
#endif /* ! ROLLOVER_CLOCK */
    }

    /* Try to validate (only if a concurrent transaction has committed since tx->start) */
    if (tx->start != t - 1 && !stm_validate(tx)) {
      /* Cannot commit */
#if CM == CM_PRIORITY
      /* Abort caused by invisible reads */
      tx->visible_reads++;
#endif /* CM == CM_PRIORITY */
#ifdef INTERNAL_STATS
      tx->aborts_validate_commit++;
#endif /* INTERNAL_STATS */
      stm_rollback(tx);
      return 0;
    }

#if DESIGN == WRITE_THROUGH
    /* Make sure that the updates become visible before releasing locks */
    ATOMIC_MB_WRITE;
    /* Drop locks and set new timestamp (traverse in reverse order) */
    w = tx->w_set.entries + tx->w_set.nb_entries - 1;
    for (i = tx->w_set.nb_entries; i > 0; i--, w--) {
      if (w->no_drop)
        continue;
      /* No need for CAS (can only be modified by owner transaction) */
      ATOMIC_STORE(w->lock, LOCK_SET_TIMESTAMP(t));
    }
    /* Make sure that all lock releases become visible */
    ATOMIC_MB_WRITE;
#elif DESIGN == WRITE_BACK_ETL
# ifdef READ_LOCKED_DATA
    /* Update instance number (becomes odd) */
    id = tx->id;
    assert(id % 2 == 0);
    ATOMIC_STORE_REL(&tx->id, id + 1);
# endif /* READ_LOCKED_DATA */
    /* Install new versions, drop locks and set new timestamp */
    w = tx->w_set.entries;
    for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
      if (w->mask != 0)
        ATOMIC_STORE(w->addr, w->value);
      /* Only drop lock for last covered address in write set */
      if (w->next == NULL)
        ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(t));

      PRINT_DEBUG2("==> write(t=%p[%lu-%lu],a=%p,d=%p-%d,v=%d)\n",
                   tx, (unsigned long)tx->start, (unsigned long)tx->end, w->addr, (void *)w->value, (int)w->value, (int)w->version);
    }
# ifdef READ_LOCKED_DATA
    /* Update instance number (becomes even) */
    ATOMIC_STORE_REL(&tx->id, id + 2);
# endif /* READ_LOCKED_DATA */
#else /* DESIGN == WRITE_BACK_CTL */
    /* Install new versions, drop locks and set new timestamp */
    w = tx->w_set.entries;
    for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
      if (w->mask == ~(stm_word_t)0) {
        ATOMIC_STORE(w->addr, w->value);
      } else if (w->mask != 0) {
        value = (ATOMIC_LOAD(w->addr) & ~w->mask) | (w->value & w->mask);
        ATOMIC_STORE(w->addr, w->value);
      }
      /* Only drop lock for last covered address in write set (cannot be "no drop") */
      if (!w->no_drop)
        ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(t));

      PRINT_DEBUG2("==> write(t=%p[%lu-%lu],a=%p,d=%p-%d,v=%d)\n",
                   tx, (unsigned long)tx->start, (unsigned long)tx->end, w->addr, (void *)w->value, (int)w->value, (int)w->version);
    }
#endif /* DESIGN == WRITE_BACK_CTL */
  }

#if CM == CM_PRIORITY || defined(INTERNAL_STATS)
  tx->retries = 0;
#endif /* CM == CM_PRIORITY || defined(INTERNAL_STATS) */

#if CM == CM_BACKOFF
  /* Reset backoff */
  tx->backoff = MIN_BACKOFF;
#endif /* CM == CM_BACKOFF */

#if CM == CM_PRIORITY
  /* Reset priority */
  tx->priority = 0;
  tx->visible_reads = 0;
#endif /* CM == CM_PRIORITY */

  /* Callbacks */
  if (nb_commit_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_commit_cb; cb++)
      commit_cb[cb].f(TXARGS commit_cb[cb].arg);
  }

  /* Set status (no need for CAS or atomic op) */
  tx->status = TX_COMMITTED;

  return 1;
}

/*
 * Called by the CURRENT thread to abort a transaction.
 */
void stm_abort(TXPARAM) 
{
  TX_GET;
  stm_rollback(tx);
}

/*
 * Called by the CURRENT thread to load a word-sized value.
 */
stm_word_t stm_load(TXPARAMS volatile stm_word_t *addr)
{
  volatile stm_word_t *lock;
  stm_word_t l, l2, value, version;
  r_entry_t *r;
#if CM == CM_PRIORITY || DESIGN == WRITE_BACK_ETL
  w_entry_t *w;
#endif /* CM == CM_PRIORITY || DESIGN == WRITE_BACK_ETL */
#if DESIGN == WRITE_BACK_CTL
  w_entry_t *written = NULL;
#endif /* DESIGN == WRITE_BACK_CTL */
#ifdef READ_LOCKED_DATA
  stm_word_t id;
  stm_tx_t *owner;
#endif /* READ_LOCKED_DATA */
  TX_GET;

  PRINT_DEBUG2("==> stm_load(t=%p[%lu-%lu],a=%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, addr);

  /* Check status */
  assert(tx->status == TX_ACTIVE);

#if CM == CM_PRIORITY
  if (tx->visible_reads >= vr_threshold && vr_threshold >= 0) {
    /* Visible reads: acquire lock first */
    w = stm_write(tx, addr, 0, 0);
    /* Make sure we did not abort */
    if(tx->status != TX_ACTIVE) {
      return 0;
    }
    assert(w != NULL);
    /* We now own the lock */
# if DESIGN == WRITE_THROUGH
    return ATOMIC_LOAD(addr);
# else /* DESIGN != WRITE_THROUGH */
    return w->mask == 0 ? ATOMIC_LOAD(addr) : w->value;
# endif /* DESIGN != WRITE_THROUGH */
  }
#endif /* CM == CM_PRIORITY */

#if DESIGN == WRITE_BACK_CTL
  /* Did we previously write the same address? */
  written = stm_has_written(tx, addr);
  if (written != NULL) {
    /* Yes: get value from write set if possible */
    if (written->mask == ~(stm_word_t)0) {
      value = written->value;
      /* No need to add to read set */
      PRINT_DEBUG2("==> stm_load(t=%p[%lu-%lu],a=%p,d=%p-%lu)\n",
                   tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, (void *)value, (unsigned long)value);
      return value;
    }
  }
#endif /* DESIGN == WRITE_BACK_CTL */

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Note: we could check for duplicate reads and get value from read set */

  /* Read lock, value, lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (LOCK_GET_OWNED(l)) {
    /* Locked */
    /* Do we own the lock? */
#if DESIGN == WRITE_THROUGH
    if (tx == (stm_tx_t *)LOCK_GET_ADDR(l)) {
      /* Yes: we have a version locked by us that was valid at write time */
      value = ATOMIC_LOAD(addr);
      /* No need to add to read set (will remain valid) */
      PRINT_DEBUG2("==> stm_load(t=%p[%lu-%lu],a=%p,l=%p,*l=%lu,d=%p-%lu)\n",
                   tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, lock, (unsigned long)l, (void *)value, (unsigned long)value);
      return value;
    }
#elif DESIGN == WRITE_BACK_ETL
    w = (w_entry_t *)LOCK_GET_ADDR(l);
    /* Simply check if address falls inside our write set (avoids non-faulting load) */
    if (tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries) {
      /* Yes: did we previously write the same address? */
      while (1) {
        if (addr == w->addr) {
          /* Yes: get value from write set (or from memory if mask was empty) */
          value = (w->mask == 0 ? ATOMIC_LOAD(addr) : w->value);
          break;
        }
        if (w->next == NULL) {
          /* No: get value from memory */
          value = ATOMIC_LOAD(addr);
          break;
        }
        w = w->next;
      }
      /* No need to add to read set (will remain valid) */
      PRINT_DEBUG2("==> stm_load(t=%p[%lu-%lu],a=%p,l=%p,*l=%lu,d=%p-%lu)\n",
                   tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, lock, (unsigned long)l, (void *)value, (unsigned long)value);
      return value;
    }
# ifdef READ_LOCKED_DATA
    if (l == LOCK_UNIT) {
      /* Data modified by a unit store: should not last long => retry */
      goto restart;
    }
    /* Try to read old version: we can safely access write set thanks to epoch-based GC */
    owner = w->tx;
    id = ATOMIC_LOAD_ACQ(&owner->id);
    /* Check that data is not being written */
    if ((id & 0x01) != 0) {
      /* Even identifier while address is locked: should not last long => retry */
      goto restart;
    }
    /* Check that the lock did not change */
    l2 = ATOMIC_LOAD_ACQ(lock);
    if (l != l2) {
      l = l2;
      goto restart_no_load;
    }
    /* Read old version */
    version =  ATOMIC_LOAD(&w->version);
    /* Read data */
    value = ATOMIC_LOAD(addr);
    /* Check that data has not been written */
    if (id != ATOMIC_LOAD_ACQ(&owner->id)) {
      /* Data concurrently modified: a new version might be available => retry */
      goto restart;
    }
    if (version >= tx->start && (version <= tx->end || (!tx->ro && tx->can_extend && stm_extend(tx)))) {
      /* Success */
#  ifdef INTERNAL_STATS
      tx->locked_reads_ok++;
#  endif /* INTERNAL_STATS */
      goto add_to_read_set;
    }
    /* Invalid version: not much we can do => fail */
#  ifdef INTERNAL_STATS
    tx->locked_reads_failed++;
#  endif /* INTERNAL_STATS */
# endif /* READ_LOCKED_DATA */
#elif DESIGN == WRITE_BACK_CTL
    /* Spin while locked (should not last long) */
    goto restart;
#endif /* DESIGN == WRITE_BACK_CTL */
    /* Conflict: CM kicks in */
    /* TODO: we could check for duplicate reads and get value from read set (should be rare) */
#if CM == CM_PRIORITY
    if (tx->retries >= cm_threshold) {
      if (LOCK_GET_PRIORITY(l) < tx->priority ||
          (LOCK_GET_PRIORITY(l) == tx->priority &&
# if DESIGN == WRITE_BACK_ETL
           l < (stm_word_t)tx->w_set.entries
# else /* DESIGN != WRITE_BACK_ETL */
           l < (stm_word_t)tx
# endif /* DESIGN != WRITE_BACK_ETL */
           && !LOCK_GET_WAIT(l))) {
        /* We have higher priority */
        if (ATOMIC_CAS_FULL(lock, l, LOCK_SET_PRIORITY_WAIT(l, tx->priority)) == 0)
          goto restart;
        l = LOCK_SET_PRIORITY_WAIT(l, tx->priority);
      }
      /* Wait until lock is free or another transaction waits for one of our locks */
      while (1) {
        int nb;
        stm_word_t lw;

        w = tx->w_set.entries;
        for (nb = tx->w_set.nb_entries; nb > 0; nb--, w++) {
          lw = ATOMIC_LOAD(w->lock);
          if (LOCK_GET_WAIT(lw)) {
            /* Another transaction waits for one of our locks */
            goto give_up;
          }
        }
        /* Did the lock we are waiting for get updated? */
        lw = ATOMIC_LOAD(lock);
        if (l != lw) {
          l = lw;
          goto restart_no_load;
        }
      }
    give_up:
      if (tx->priority < PRIORITY_MAX) {
        tx->priority++;
      } else {
        PRINT_DEBUG("Reached maximum priority\n");
      }
      tx->c_lock = lock;
    }
#elif CM == CM_DELAY
    tx->c_lock = lock;
#endif /* CM == CM_DELAY */
    /* Abort */
#ifdef INTERNAL_STATS
    tx->aborts_locked_read++;
#endif /* INTERNAL_STATS */
#ifdef CONFLICT_TRACKING
    if (conflict_cb != NULL && l != LOCK_UNIT) {
      /* Call conflict callback */
# if DESIGN == WRITE_THROUGH
      stm_tx_t *other = (stm_tx_t *)LOCK_GET_ADDR(l);
# else /* DESIGN != WRITE_THROUGH */
      stm_tx_t *other = ((w_entry_t *)LOCK_GET_ADDR(l))->tx;
# endif /* DESIGN != WRITE_THROUGH */
      conflict_cb(tx, other);
    }
#endif /* CONFLICT_TRACKING */
    stm_rollback(tx);
    return 0;
  } else {
    /* Not locked */
    value = ATOMIC_LOAD_ACQ(addr);
    l2 = ATOMIC_LOAD_ACQ(lock);
    if (l != l2) {
      l = l2;
      goto restart_no_load;
    }
    /* Check timestamp */
    version = LOCK_GET_TIMESTAMP(l);
    /* Valid version? */
    if (version > tx->end) {
      /* No: try to extend first (except for read-only transactions: no read set) */
      if (tx->ro || !tx->can_extend || !stm_extend(tx)) {
        /* Not much we can do: abort */
#if CM == CM_PRIORITY
        /* Abort caused by invisible reads */
        tx->visible_reads++;
#endif /* CM == CM_PRIORITY */
#ifdef INTERNAL_STATS
        tx->aborts_validate_read++;
#endif /* INTERNAL_STATS */
        stm_rollback(tx);
        return 0;
      }
      /* Verify that version has not been overwritten (read value has not
       * yet been added to read set and may have not been checked during
       * extend) */
      l = ATOMIC_LOAD_ACQ(lock);
      if (l != l2) {
        l = l2;
        goto restart_no_load;
      }
      /* Worked: we now have a good version (version <= tx->end) */
    }
  }
  /* We have a good version: add to read set (update transactions) and return value */

#if DESIGN == WRITE_BACK_CTL
  /* Did we previously write the same address? */
  if (written != NULL) {
    value = (value & ~written->mask) | (written->value & written->mask);
    /* Must still add to read set */
  }
#endif /* DESIGN == WRITE_BACK_CTL */
#ifdef READ_LOCKED_DATA
add_to_read_set:
#endif /* READ_LOCKED_DATA */
  if (!tx->ro) {
    /* Add address and version to read set */
    if (tx->r_set.nb_entries == tx->r_set.size)
      stm_allocate_rs_entries(tx, 1);
    r = &tx->r_set.entries[tx->r_set.nb_entries++];
    r->version = version;
    r->lock = lock;
  }

  PRINT_DEBUG2("==> stm_load(t=%p[%lu-%lu],a=%p,l=%p,*l=%lu,d=%p-%lu,v=%lu)\n",
               tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, lock, (unsigned long)l, (void *)value, (unsigned long)value, (unsigned long)version);

  return value;
}

/*
 * Called by the CURRENT thread to store a word-sized value.
 */
void stm_store(TXPARAMS volatile stm_word_t *addr, stm_word_t value)
{
  TX_GET;
  stm_write(tx, addr, value, ~(stm_word_t)0);
}

/*
 * Called by the CURRENT thread to store part of a word-sized value.
 */
void stm_store2(TXPARAMS volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  TX_GET;
  stm_write(tx, addr, value, mask);
}

/*
 * Called by the CURRENT thread to inquire about the status of a transaction.
 */
int stm_active(TXPARAM)
{
  TX_GET;

  return (tx->status == TX_ACTIVE);
}

/*
 * Called by the CURRENT thread to inquire about the status of a transaction.
 */
int stm_aborted(TXPARAM)
{
  TX_GET;

  return (tx->status == TX_ABORTED);
}

/*
 * Called by the CURRENT thread to obtain an environment for setjmp/longjmp.
 */
sigjmp_buf *stm_get_env(TXPARAM)
{
  TX_GET;

  /* Only return environment for top-level transaction */
  return tx->nesting == 0 ? &tx->env : NULL;
}

/*
 * Get transaction attributes.
 */
stm_tx_attr_t *stm_get_attributes(TXPARAM)
{
  TX_GET;

  return tx->attr;
}

/*
 * Return statistics about a thread/transaction.
 */
int stm_get_stats(TXPARAMS const char *name, void *val)
{
  TX_GET;

  if (strcmp("read_set_size", name) == 0) {
    *(unsigned int *)val = tx->r_set.size;
    return 1;
  }
  if (strcmp("write_set_size", name) == 0) {
    *(unsigned int *)val = tx->w_set.size;
    return 1;
  }
  if (strcmp("read_set_nb_entries", name) == 0) {
    *(unsigned int *)val = tx->r_set.nb_entries;
    return 1;
  }
  if (strcmp("write_set_nb_entries", name) == 0) {
    *(unsigned int *)val = tx->w_set.nb_entries;
    return 1;
  }
  if (strcmp("read_only", name) == 0) {
    *(unsigned int *)val = tx->ro;
    return 1;
  }
#ifdef INTERNAL_STATS
  if (strcmp("nb_aborts", name) == 0) {
    *(unsigned long *)val = tx->aborts;
    return 1;
  }
  if (strcmp("nb_aborts_ro", name) == 0) {
    *(unsigned long *)val = tx->aborts_ro;
    return 1;
  }
  if (strcmp("nb_aborts_locked_read", name) == 0) {
    *(unsigned long *)val = tx->aborts_locked_read;
    return 1;
  }
  if (strcmp("nb_aborts_locked_write", name) == 0) {
    *(unsigned long *)val = tx->aborts_locked_write;
    return 1;
  }
  if (strcmp("nb_aborts_validate_read", name) == 0) {
    *(unsigned long *)val = tx->aborts_validate_read;
    return 1;
  }
  if (strcmp("nb_aborts_validate_write", name) == 0) {
    *(unsigned long *)val = tx->aborts_validate_write;
    return 1;
  }
  if (strcmp("nb_aborts_validate_commit", name) == 0) {
    *(unsigned long *)val = tx->aborts_validate_commit;
    return 1;
  }
  if (strcmp("nb_aborts_invalid_memory", name) == 0) {
    *(unsigned long *)val = tx->aborts_invalid_memory;
    return 1;
  }
# if DESIGN == WRITE_BACK_ETL
  if (strcmp("nb_aborts_reallocate", name) == 0) {
    *(unsigned long *)val = tx->aborts_reallocate;
    return 1;
  } 
# endif /* DESIGN == WRITE_BACK_ETL */
# ifdef ROLLOVER_CLOCK
  if (strcmp("nb_aborts_rollover", name) == 0) {
    *(unsigned long *)val = tx->aborts_rollover;
    return 1;
  }
# endif /* ROLLOVER_CLOCK */
# ifdef READ_LOCKED_DATA
  if (strcmp("locked_reads_ok", name) == 0) {
    *(unsigned long *)val = tx->locked_reads_ok;
    return 1;
  } 
  if (strcmp("locked_reads_failed", name) == 0) {
    *(unsigned long *)val = tx->locked_reads_failed;
    return 1;
  } 
# endif /* READ_LOCKED_DATA */
  if (strcmp("max_retries", name) == 0) {
    *(unsigned long *)val = tx->max_retries;
    return 1;
  }
#endif /* INTERNAL_STATS */
  return 0;
}

/*
 * Return STM parameters.
 */
int stm_get_parameter(const char *name, void *val)
{
  if (strcmp("contention_manager", name) == 0) {
    *(const char **)val = cm_names[CM];
    return 1;
  }
  if (strcmp("design", name) == 0) {
    *(const char **)val = design_names[DESIGN];
    return 1;
  }
  if (strcmp("initial_rw_set_size", name) == 0) {
    *(int *)val = RW_SET_SIZE;
    return 1;
  }
#if CM == CM_BACKOFF
  if (strcmp("min_backoff", name) == 0) {
    *(unsigned long *)val = MIN_BACKOFF;
    return 1;
  }
  if (strcmp("max_backoff", name) == 0) {
    *(unsigned long *)val = MAX_BACKOFF;
    return 1;
  }
#endif /* CM == CM_BACKOFF */
#if CM == CM_PRIORITY
  if (strcmp("vr_threshold", name) == 0) {
    *(int *)val = vr_threshold;
    return 1;
  }
  if (strcmp("cm_threshold", name) == 0) {
    *(int *)val = cm_threshold;
    return 1;
  }
#endif /* CM == CM_PRIORITY */
#ifdef COMPILE_FLAGS
  if (strcmp("compile_flags", name) == 0) {
    *(const char **)val = XSTR(COMPILE_FLAGS);
    return 1;
  }
#endif /* COMPILE_FLAGS */
  return 0;
}

/*
 * Set STM parameters.
 */
int stm_set_parameter(const char *name, void *val)
{
#if CM == CM_PRIORITY
  if (strcmp("vr_threshold", name) == 0) {
    vr_threshold = *(int *)val;
    return 1;
  }
  if (strcmp("cm_threshold", name) == 0) {
    cm_threshold = *(int *)val;
    return 1;
  }
#endif /* CM_PRIORITY */
  return 0;
}

/*
 * Create transaction-specific data (return -1 on error).
 */
int stm_create_specific()
{
  if (nb_specific >= MAX_SPECIFIC) {
    fprintf(stderr, "Error: maximum number of specific slots reached\n");
    return -1;
  }
  return nb_specific++;
}

/*
 * Store transaction-specific data.
 */
void stm_set_specific(TXPARAMS int key, void *data)
{
  TX_GET;

  assert (key >= 0 && key < nb_specific);
  tx->data[key] = data;
}

/*
 * Fetch transaction-specific data.
 */
void *stm_get_specific(TXPARAMS int key)
{
  TX_GET;

  assert (key >= 0 && key < nb_specific);
  return tx->data[key];
}

/*
 * Register callbacks for an external module (must be called before creating transactions).
 */
int stm_register(void (*on_thread_init)(TXPARAMS void *arg),
                 void (*on_thread_exit)(TXPARAMS void *arg),
                 void (*on_start)(TXPARAMS void *arg),
                 void (*on_commit)(TXPARAMS void *arg),
                 void (*on_abort)(TXPARAMS void *arg),
                 void *arg)
{
  if ((on_thread_init != NULL && nb_init_cb >= MAX_CB) ||
      (on_thread_exit != NULL && nb_exit_cb >= MAX_CB) ||
      (on_start != NULL && nb_start_cb >= MAX_CB) ||
      (on_commit != NULL && nb_commit_cb >= MAX_CB) ||
      (on_abort != NULL && nb_abort_cb >= MAX_CB)) {
    fprintf(stderr, "Error: maximum number of modules reached\n");
    return 0;
  }
  /* New callback */
  if (on_thread_init != NULL) {
    init_cb[nb_init_cb].f = on_thread_init;
    init_cb[nb_init_cb++].arg = arg;
  }
  /* Delete callback */
  if (on_thread_exit != NULL) {
    exit_cb[nb_exit_cb].f = on_thread_exit;
    exit_cb[nb_exit_cb++].arg = arg;
  }
  /* Start callback */
  if (on_start != NULL) {
    start_cb[nb_start_cb].f = on_start;
    start_cb[nb_start_cb++].arg = arg;
  }
  /* Commit callback */
  if (on_commit != NULL) {
    commit_cb[nb_commit_cb].f = on_commit;
    commit_cb[nb_commit_cb++].arg = arg;
  }
  /* Abort callback */
  if (on_abort != NULL) {
    abort_cb[nb_abort_cb].f = on_abort;
    abort_cb[nb_abort_cb++].arg = arg;
  }

  return 1;
}

/*
 * Called by the CURRENT thread to load a word-sized value in a unit transaction.
 */
stm_word_t stm_unit_load(volatile stm_word_t *addr, stm_word_t *timestamp)
{
  volatile stm_word_t *lock;
  stm_word_t l, l2, value;

  PRINT_DEBUG2("==> stm_unit_load(a=%p)\n", addr);

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Read lock, value, lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (LOCK_GET_OWNED(l)) {
    /* Locked: wait until lock is free */
#ifdef WAIT_YIELD
    sched_yield();
#endif /* WAIT_YIELD */
    goto restart;
  }
  /* Not locked */
  value = ATOMIC_LOAD_ACQ(addr);
  l2 = ATOMIC_LOAD_ACQ(lock);
  if (l != l2) {
    l = l2;
    goto restart_no_load;
  }

  if (timestamp != NULL)
    *timestamp = LOCK_GET_TIMESTAMP(l);

  PRINT_DEBUG2("==> stm_unit_load(a=%p,l=%p,*l=%lu,d=%p-%lu)\n",
               addr, lock, (unsigned long)l, (void *)value, (unsigned long)value);

  return value;
}

/*
 * Called by the CURRENT thread to store a word-sized value in a unit transaction.
 */
int stm_unit_store(volatile stm_word_t *addr, stm_word_t value, stm_word_t *timestamp)
{
  return stm_unit_write(addr, value, ~(stm_word_t)0, timestamp);
}

/*
 * Called by the CURRENT thread to store part of a word-sized value in a unit transaction.
 */
int stm_unit_store2(volatile stm_word_t *addr, stm_word_t value, stm_word_t mask, stm_word_t *timestamp)
{
  return stm_unit_write(addr, value, mask, timestamp);
}

/*
 * Enable or disable extensions and set upper bound on snapshot.
 */
void stm_set_extension(TXPARAMS int enable, stm_word_t *timestamp)
{
  TX_GET;

  tx->can_extend = enable;
  if (timestamp != NULL && *timestamp < tx->end)
    tx->end = *timestamp;
}

/*
 * Get curent value of global clock.
 */
stm_word_t stm_get_clock()
{
  return GET_CLOCK;
}

/*
 * Get current transaction descriptor.
 */
stm_tx_t *stm_current_tx()
{
  return stm_get_tx();
}

/* ################################################################### *
 * UNDOCUMENTED STM FUNCTIONS (USE WITH CARE!)
 * ################################################################### */

#ifdef CONFLICT_TRACKING
/*
 * Get thread identifier of other transaction.
 */
int stm_get_thread_id(stm_tx_t *tx, pthread_t *id)
{
  *id = tx->thread_id;
  return 1;
}

/*
 * Set global conflict callback.
 */
int stm_set_conflict_cb(void (*on_conflict)(stm_tx_t *tx1, stm_tx_t *tx2))
{
  conflict_cb = on_conflict;
  return 1;
}
#endif /* CONFLICT_TRACKING */
