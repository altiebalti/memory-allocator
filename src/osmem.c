// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>

#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT		8
#define ALIGN(size)		(((size) + (ALIGNMENT - 1)) & ~((size_t)(ALIGNMENT - 1)))
#define META_SIZE		(ALIGN(sizeof(struct block_meta)))
#define MMAP_THRESHOLD		(128 * 1024)

/* Address-ordered list of heap (brk) blocks; mmap blocks stay out of it. */
static struct block_meta *heap_head;

static inline void *block_to_payload(struct block_meta *block)
{
	return (void *)((char *)block + META_SIZE);
}

static inline struct block_meta *payload_to_block(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

static struct block_meta *heap_last_block(void)
{
	struct block_meta *block = heap_head;

	if (!block)
		return NULL;
	while (block->next)
		block = block->next;
	return block;
}

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

/* Smallest free block that fits. */
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

/* Split only if the leftover holds a header plus at least ALIGNMENT bytes. */
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

static void *grow_break(size_t increment)
{
	void *prev = sbrk(increment);

	DIE(prev == (void *)-1, "sbrk");
	return prev;
}

/* First heap use: grab a full 128 KiB chunk as one free block. */
static void preallocate_heap(void)
{
	struct block_meta *block = (struct block_meta *)sbrk(0);

	DIE(block == (void *)-1, "sbrk");
	grow_break(MMAP_THRESHOLD);

	block->size = MMAP_THRESHOLD - META_SIZE;
	block->status = STATUS_FREE;
	block->prev = NULL;
	block->next = NULL;
	heap_head = block;
}

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

	last = heap_last_block();
	if (last && last->status == STATUS_FREE) {
		/* Extend the last block in place instead of making a new one. */
		grow_break(payload - last->size);
		last->size = payload;
		last->status = STATUS_ALLOC;
		return last;
	}

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

/* Shared by malloc and calloc; only the mmap threshold differs. */
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

	if (nmemb > (size_t)-1 / size)
		return NULL;

	total = nmemb * size;
	ptr = allocate(total, (size_t)getpagesize());
	if (ptr)
		memset(ptr, 0, ALIGN(total));
	return ptr;
}

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

	/* Heap block: mark free, reuse later (coalescing is lazy). */
	block->status = STATUS_FREE;
}

void os_free(void *ptr)
{
	free_block(ptr);
}

/* Fall-back for realloc: allocate, copy, free the old block. */
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

	if (block->status == STATUS_FREE)
		return NULL;

	payload = ALIGN(size);

	/* Mapped blocks and heap blocks crossing the threshold must relocate. */
	if (block->status == STATUS_MAPPED || META_SIZE + payload >= MMAP_THRESHOLD)
		return realloc_relocate(block, ptr, size);

	if (payload <= block->size) {
		split_block(block, payload);
		return ptr;
	}

	if (!block->next) {
		grow_break(payload - block->size);
		block->size = payload;
		return ptr;
	}

	/* Grow in place by absorbing following free neighbours. */
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

	if (!block->next) {
		grow_break(payload - block->size);
		block->size = payload;
		return ptr;
	}

	return realloc_relocate(block, ptr, size);
}
