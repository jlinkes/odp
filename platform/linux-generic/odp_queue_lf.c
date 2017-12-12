/* Copyright (c) 2018, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <odp/api/queue.h>
#include <odp/api/atomic.h>
#include <odp/api/shared_memory.h>
#include <odp_queue_lf.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include <odp_debug_internal.h>

#define RING_LF_SIZE   32
#define QUEUE_LF_NUM   128
#define ENQ_RETRIES    (RING_LF_SIZE / 4)
#define DEQ_RETRIES    (RING_LF_SIZE / 8)

#ifdef __SIZEOF_INT128__

typedef unsigned __int128 u128_t;

static inline u128_t atomic_load_u128(u128_t *atomic)
{
	return __atomic_load_n(atomic, __ATOMIC_RELAXED);
}

static inline void atomic_zero_u128(u128_t *atomic)
{
	__atomic_store_n(atomic, 0, __ATOMIC_RELAXED);
}

static inline int atomic_cas_rel_u128(u128_t *atomic, u128_t old_val,
				      u128_t new_val)
{
	return __atomic_compare_exchange_n(atomic, &old_val, new_val,
					   0 /* strong */,
					   __ATOMIC_RELEASE,
					   __ATOMIC_RELAXED);
}

static inline int atomic_cas_acq_u128(u128_t *atomic, u128_t old_val,
				      u128_t new_val)
{
	return __atomic_compare_exchange_n(atomic, &old_val, new_val,
					   0 /* strong */,
					   __ATOMIC_ACQUIRE,
					   __ATOMIC_RELAXED);
}

#else

/* These definitions enable build in non 128 bit compatible systems.
 * Implementation is active only when 128 bit lockfree atomics are available.
 * So, these are never actually used. */
typedef struct {
	uint64_t u64[2];
} u128_t ODP_ALIGNED(16);

static inline u128_t atomic_load_u128(u128_t *atomic)
{
	return *atomic;
}

static inline void atomic_zero_u128(u128_t *atomic)
{
	atomic->u64[0] = 0;
	atomic->u64[1] = 0;
}

static inline int atomic_cas_rel_u128(u128_t *atomic, u128_t old_val,
				      u128_t new_val)
{
	(void)old_val;
	*atomic = new_val;
	return 1;
}

static inline int atomic_cas_acq_u128(u128_t *atomic, u128_t old_val,
				      u128_t new_val)
{
	return atomic_cas_rel_u128(atomic, old_val, new_val);
}

#endif

/* Node in lock-free ring */
typedef union {
	u128_t u128;

	struct {
		/* 0: empty, 1: data */
		uint64_t mark:  1;

		/* A cache aligned pointer fits into 63 bits, since the least
		 * significant bits are zero. */
		uint64_t ptr:  63;

		/* Data with lowest counter value is the head. */
		uint64_t count;
	} s;

} ring_lf_node_t;

/* Lock-free ring */
typedef struct {
	ring_lf_node_t   node[RING_LF_SIZE];
	int              used;
	odp_atomic_u64_t enq_counter;

} queue_lf_t ODP_ALIGNED_CACHE;

/* Lock-free queue globals */
typedef struct {
	queue_lf_t queue_lf[QUEUE_LF_NUM];
	odp_shm_t  shm;

} queue_lf_global_t ODP_ALIGNED_CACHE;

static queue_lf_global_t *queue_lf_glb;

static inline int next_idx(int idx)
{
	int next = idx + 1;

	if (next == RING_LF_SIZE)
		next = 0;

	return next;
}

static int queue_lf_enq(queue_t q_int, odp_buffer_hdr_t *buf_hdr)
{
	queue_entry_t *queue;
	queue_lf_t *queue_lf;
	int i, j, i_node;
	int found;
	ring_lf_node_t node_val;
	ring_lf_node_t new_val;
	ring_lf_node_t *node;
	uint64_t counter;

	queue    = qentry_from_int(q_int);
	queue_lf = queue->s.queue_lf;

	i_node = 0;

	counter = odp_atomic_fetch_inc_u64(&queue_lf->enq_counter);

	for (j = 0; j < ENQ_RETRIES; j++) {
		found = 0;

		/* Find empty node */
		for (i = 0; i < RING_LF_SIZE; i++) {
			i_node = next_idx(i_node);
			node = &queue_lf->node[i_node];
			node_val.u128 = atomic_load_u128(&node->u128);

			if (node_val.s.mark == 0) {
				found = 1;
				break;
			}
		}

		/* Queue is full */
		if (found == 0)
			return -1;

		/* Try to insert data */
		new_val.s.mark  = 1;
		new_val.s.count = counter;
		new_val.s.ptr   = ((uintptr_t)buf_hdr) >> 1;

		if (atomic_cas_rel_u128(&node->u128, node_val.u128,
					new_val.u128))
			return 0;
	}

	return -1;
}

static int queue_lf_enq_multi(queue_t q_int, odp_buffer_hdr_t **buf_hdr,
			      int num)
{
	(void)num;

	if (queue_lf_enq(q_int, buf_hdr[0]) == 0)
		return 1;

	return 0;
}

static odp_buffer_hdr_t *queue_lf_deq(queue_t q_int)
{
	queue_entry_t *queue;
	queue_lf_t *queue_lf;
	int i, j, i_node;
	int found;
	ring_lf_node_t node_val, old_val, new_val;
	ring_lf_node_t *node, *old;
	uint64_t lowest;

	queue    = qentry_from_int(q_int);
	queue_lf = queue->s.queue_lf;
	i_node   = 0;

	for (j = 0; j < DEQ_RETRIES; j++) {
		found  = 0;
		lowest = -1;

		/* Find the head node. The one with data and
		 * the lowest counter. */
		for (i = 0; i < RING_LF_SIZE; i++) {
			i_node = next_idx(i_node);
			node = &queue_lf->node[i_node];
			node_val.u128 = atomic_load_u128(&node->u128);

			if (node_val.s.mark == 1 && node_val.s.count < lowest) {
				old          = node;
				old_val.u128 = node_val.u128;
				lowest       = node_val.s.count;
				found        = 1;
			}
		}

		/* Queue is empty */
		if (found == 0)
			return NULL;

		/* Try to remove data */
		new_val.u128   = old_val.u128;
		new_val.s.mark = 0;

		if (atomic_cas_acq_u128(&old->u128, old_val.u128,
					new_val.u128))
			return (void *)(((uintptr_t)old_val.s.ptr) << 1);
	}

	return NULL;
}

static int queue_lf_deq_multi(queue_t q_int, odp_buffer_hdr_t **buf_hdr,
			      int num)
{
	odp_buffer_hdr_t *buf;

	(void)num;

	buf = queue_lf_deq(q_int);

	if (buf == NULL)
		return 0;

	buf_hdr[0] = buf;
	return 1;
}

uint32_t queue_lf_init_global(uint32_t *queue_lf_size,
			      queue_lf_func_t *lf_func)
{
	odp_shm_t shm;
	bool lockfree = 0;

	/* 16 byte lockfree CAS operation is needed. */
#ifdef __SIZEOF_INT128__
	lockfree = __atomic_is_lock_free(16, NULL);
#endif

	ODP_DBG("\nLock-free queue init\n");
	ODP_DBG("  u128 lock-free: %i\n\n", lockfree);

	if (!lockfree)
		return 0;

	shm = odp_shm_reserve("odp_queues_lf", sizeof(queue_lf_global_t),
			      ODP_CACHE_LINE_SIZE, 0);

	queue_lf_glb = odp_shm_addr(shm);
	memset(queue_lf_glb, 0, sizeof(queue_lf_global_t));

	queue_lf_glb->shm = shm;

	memset(lf_func, 0, sizeof(queue_lf_func_t));
	lf_func->enq       = queue_lf_enq;
	lf_func->enq_multi = queue_lf_enq_multi;
	lf_func->deq       = queue_lf_deq;
	lf_func->deq_multi = queue_lf_deq_multi;

	*queue_lf_size = RING_LF_SIZE;

	return QUEUE_LF_NUM;
}

void queue_lf_term_global(void)
{
	odp_shm_t shm;

	if (queue_lf_glb == NULL)
		return;

	shm = queue_lf_glb->shm;

	if (odp_shm_free(shm) < 0)
		ODP_ERR("shm free failed");
}

static void init_queue(queue_lf_t *queue_lf)
{
	int i;

	odp_atomic_init_u64(&queue_lf->enq_counter, 0);

	for (i = 0; i < RING_LF_SIZE; i++)
		atomic_zero_u128(&queue_lf->node[i].u128);
}

void *queue_lf_create(queue_entry_t *queue)
{
	int i;
	queue_lf_t *queue_lf = NULL;

	if (queue->s.type != ODP_QUEUE_TYPE_PLAIN)
		return NULL;

	for (i = 0; i < QUEUE_LF_NUM; i++) {
		if (queue_lf_glb->queue_lf[i].used == 0) {
			queue_lf = &queue_lf_glb->queue_lf[i];
			memset(queue_lf, 0, sizeof(queue_lf_t));
			init_queue(queue_lf);
			queue_lf->used = 1;
			break;
		}
	}

	return queue_lf;
}

void queue_lf_destroy(void *queue_lf_ptr)
{
	queue_lf_t *queue_lf = queue_lf_ptr;

	queue_lf->used = 0;
}