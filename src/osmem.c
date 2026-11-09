// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>

#include "osmem.h"
#include "block_meta.h"

/*
 * Minimal implementation of malloc/calloc/realloc/free on top of the Linux
 * brk() and mmap() system calls.
 *
 * Design overview
 * ---------------
 *  - Every allocation is prefixed by a `struct block_meta` header.  The header
 *    is 32 bytes on a 64-bit machine and, together with every payload, is kept
 *    aligned to 8 bytes.
 *  - "Small" requests are served from the program break (brk/sbrk); "large"
 *    ones are served through anonymous mmap().  The threshold is the payload
 *    plus header crossing MMAP_THRESHOLD for malloc/realloc and the page size
 *    for calloc.
 *  - On the very first heap use we preallocate a full MMAP_THRESHOLD (128 KiB)
 *    chunk and carve later allocations out of it, which keeps the number of
 *    brk() calls low.
 *  - Heap blocks are kept in an address-ordered doubly linked list.  Freed
 *    heap blocks are only marked STATUS_FREE (the break is never lowered) and
 *    reused later with a best-fit search.  Adjacent free blocks are coalesced
 *    lazily, right before a search and while a realloc grows in place.
 *  - mmap()-ed blocks are self contained: they are not part of the list and
 *    are returned to the OS with munmap() on free.
 */

#define ALIGNMENT		8
#define ALIGN(size)		(((size) + (ALIGNMENT - 1)) & ~((size_t)(ALIGNMENT - 1)))
#define META_SIZE		(ALIGN(sizeof(struct block_meta)))
#define MMAP_THRESHOLD		(128 * 1024)

/* Head of the address-ordered list of heap (brk) blocks. */
static struct block_meta *heap_head;

/* Return the payload pointer that corresponds to a block header. */
static inline void *block_to_payload(struct block_meta *block)
{
	return (void *)((char *)block + META_SIZE);
}

/* Return the block header that corresponds to a payload pointer. */
static inline struct block_meta *payload_to_block(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

/* Walk to the last block in the heap list. */
static struct block_meta *heap_last_block(void)
{
	struct block_meta *block = heap_head;

	if (!block)
		return NULL;
	while (block->next)
		block = block->next;
	return block;
}

/* Merge every pair of adjacent free blocks in the list. */
static void coalesce_heap(void)
{
	struct block_meta *block = heap_head;

	while (block && block->next) {
		if (block->status == STATUS_FREE && block->next->status == STATUS_FREE) {
			block->size += META_SIZE + block->next->size;
			block->next = block->next->next;
			if (block->next)
				block->next->prev = block;
		} else {
			block = block->next;
		}
	}
}

/* Best-fit: smallest free block that can hold `payload` bytes. */
static struct block_meta *find_best_fit(size_t payload)
{
	struct block_meta *best = NULL;
	struct block_meta *block = heap_head;

	while (block) {
		if (block->status == STATUS_FREE && block->size >= payload) {
			if (!best || block->size < best->size)
				best = block;
		}
		block = block->next;
	}
	return best;
}

/*
 * Split `block` so that it holds exactly `payload` bytes and the remaining
 * space becomes a new free block.  The split is only performed when the
 * leftover can hold a header plus at least ALIGNMENT bytes of usable payload.
 */
static void split_block(struct block_meta *block, size_t payload)
{
	struct block_meta *rest;

	if (block->size < payload + META_SIZE + ALIGNMENT)
		return;

	rest = (struct block_meta *)((char *)block + META_SIZE + payload);
	rest->size = block->size - payload - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = block;
	rest->next = block->next;
	if (rest->next)
		rest->next->prev = rest;

	block->next = rest;
	block->size = payload;
}

/* Grow the program break by `increment` bytes, returning the old break. */
static void *grow_break(size_t increment)
{
	void *prev = sbrk(increment);

	DIE(prev == (void *)-1, "sbrk");
	return prev;
}

/* Preallocate the first 128 KiB heap chunk as a single free block. */
static void preallocate_heap(void)
{
	struct block_meta *block;

	/*
	 * sbrk(0) reports the current break (this is the brk(0) seen in the
	 * reference traces); sbrk(MMAP_THRESHOLD) then extends it by 128 KiB.
	 */
	block = (struct block_meta *)sbrk(0);
	DIE(block == (void *)-1, "sbrk");
	grow_break(MMAP_THRESHOLD);

	block->size = MMAP_THRESHOLD - META_SIZE;
	block->status = STATUS_FREE;
	block->prev = NULL;
	block->next = NULL;
	heap_head = block;
}

/* Allocate `payload` (already aligned) bytes on the heap. */
static struct block_meta *alloc_on_heap(size_t payload)
{
	struct block_meta *block, *last;

	if (!heap_head)
		preallocate_heap();

	coalesce_heap();

	block = find_best_fit(payload);
	if (block) {
		split_block(block, payload);
		block->status = STATUS_ALLOC;
		return block;
	}

	/* No free block is large enough. */
	last = heap_last_block();
	if (last && last->status == STATUS_FREE) {
		/* Extend the last block if it is free. */
		grow_break(payload - last->size);
		last->size = payload;
		last->status = STATUS_ALLOC;
		return last;
	}

	/* Otherwise append a brand new block at the top of the heap. */
	block = (struct block_meta *)grow_break(META_SIZE + payload);
	block->size = payload;
	block->status = STATUS_ALLOC;
	block->prev = last;
	block->next = NULL;
	if (last)
		last->next = block;
	else
		heap_head = block;
	return block;
}

/* Allocate `payload` (already aligned) bytes through mmap(). */
static struct block_meta *alloc_mapped(size_t payload)
{
	size_t total = META_SIZE + payload;
	struct block_meta *block;

	block = (struct block_meta *)mmap(NULL, total, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	DIE(block == MAP_FAILED, "mmap");

	block->size = payload;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;
	return block;
}

/* Core allocation path shared by malloc and calloc (threshold differs). */
static void *allocate(size_t size, size_t mmap_threshold)
{
	size_t payload;

	if (size == 0)
		return NULL;

	payload = ALIGN(size);

	if (META_SIZE + payload >= mmap_threshold)
		return block_to_payload(alloc_mapped(payload));

	return block_to_payload(alloc_on_heap(payload));
}

void *os_malloc(size_t size)
{
	return allocate(size, MMAP_THRESHOLD);
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *ptr;

	if (nmemb == 0 || size == 0)
		return NULL;

	/* Guard against size_t overflow of nmemb * size. */
	if (nmemb > (size_t)-1 / size)
		return NULL;

	total = nmemb * size;
	ptr = allocate(total, (size_t)getpagesize());
	if (ptr)
		memset(ptr, 0, ALIGN(total));
	return ptr;
}

/* Internal free, used both by os_free() and by os_realloc(). */
static void free_block(void *ptr)
{
	struct block_meta *block;

	if (!ptr)
		return;

	block = payload_to_block(ptr);

	if (block->status == STATUS_MAPPED) {
		DIE(munmap(block, META_SIZE + block->size) != 0, "munmap");
		return;
	}

	/* Heap block: mark free and reuse later (coalescing is lazy). */
	block->status = STATUS_FREE;
}

void os_free(void *ptr)
{
	free_block(ptr);
}

/* Allocate a fresh block, copy over the old payload and release the old one. */
static void *realloc_relocate(struct block_meta *block, void *ptr, size_t size)
{
	void *new_ptr = allocate(size, MMAP_THRESHOLD);
	size_t to_copy;

	if (!new_ptr)
		return NULL;

	to_copy = block->size < ALIGN(size) ? block->size : ALIGN(size);
	memcpy(new_ptr, ptr, to_copy);
	free_block(ptr);
	return new_ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;
	size_t payload;

	if (!ptr)
		return allocate(size, MMAP_THRESHOLD);

	if (size == 0) {
		free_block(ptr);
		return NULL;
	}

	block = payload_to_block(ptr);

	/* Robustness guard: realloc on an already-free block is undefined. */
	if (block->status == STATUS_FREE)
		return NULL;

	payload = ALIGN(size);

	/*
	 * A mapped block cannot be resized in place (mremap is forbidden) and a
	 * heap block that grows past the threshold must migrate to mmap, so both
	 * cases relocate.
	 */
	if (block->status == STATUS_MAPPED || META_SIZE + payload >= MMAP_THRESHOLD)
		return realloc_relocate(block, ptr, size);

	/* Shrinking (or same size): keep the block, split off any leftover. */
	if (payload <= block->size) {
		split_block(block, payload);
		return ptr;
	}

	/* Growing the last block: just push the break up. */
	if (!block->next) {
		grow_break(payload - block->size);
		block->size = payload;
		return ptr;
	}

	/* Growing: try to absorb following free neighbours one at a time. */
	while (block->size < payload && block->next &&
	       block->next->status == STATUS_FREE) {
		block->size += META_SIZE + block->next->size;
		block->next = block->next->next;
		if (block->next)
			block->next->prev = block;
	}

	if (block->size >= payload) {
		split_block(block, payload);
		return ptr;
	}

	/* Absorbing made the block the last one: extend the break. */
	if (!block->next) {
		grow_break(payload - block->size);
		block->size = payload;
		return ptr;
	}

	/* Still too small and boxed in: relocate. */
	return realloc_relocate(block, ptr, size);
}
