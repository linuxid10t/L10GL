#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "virge_texheap.h"

struct virge_texheap_block {
    uint32_t address;
    uint32_t size;
    struct virge_texheap_block *next;
};

static uint32_t align8(uint32_t value) { return (value + 7u) & ~7u; }

int virge_texheap_init(struct virge_texheap *heap, uint32_t start,
                       uint32_t end)
{
    struct virge_texheap_block *block;

    if (!heap || start > UINT32_MAX - 7u)
        return -EINVAL;
    start = align8(start);
    end &= ~7u;
    if (start >= end)
        return -ENOSPC;
    block = malloc(sizeof(*block));
    if (!block)
        return -ENOMEM;
    block->address = start;
    block->size = end - start;
    block->next = NULL;
    heap->start = start;
    heap->end = end;
    heap->free = block;
    return 0;
}

void virge_texheap_destroy(struct virge_texheap *heap)
{
    struct virge_texheap_block *block, *next;

    if (!heap)
        return;
    for (block = heap->free; block; block = next) {
        next = block->next;
        free(block);
    }
    heap->free = NULL;
}

int virge_texheap_alloc(struct virge_texheap *heap, uint32_t size,
                        uint32_t *address)
{
    struct virge_texheap_block **link, *block;

    if (!heap || !address || !size || size > UINT32_MAX - 7u)
        return -EINVAL;
    size = align8(size);
    for (link = &heap->free; (block = *link) != NULL; link = &block->next) {
        if (block->size < size)
            continue;
        *address = block->address;
        block->address += size;
        block->size -= size;
        if (!block->size) {
            *link = block->next;
            free(block);
        }
        return 0;
    }
    return -ENOSPC;
}

int virge_texheap_free(struct virge_texheap *heap, uint32_t address,
                       uint32_t size)
{
    struct virge_texheap_block **link, *next, *block, *previous = NULL;

    if (!heap || !size || (address & 7u) || size > UINT32_MAX - 7u)
        return -EINVAL;
    size = align8(size);
    if (size > heap->end - heap->start)
        return -ERANGE;
    if (address < heap->start || address > heap->end - size)
        return -ERANGE;
    for (link = &heap->free; *link && (*link)->address < address;
         link = &(*link)->next)
        previous = *link;
    next = *link;
    if ((previous && previous->address + previous->size > address) ||
        (next && address + size > next->address))
        return -EINVAL;
    if (previous && previous->address + previous->size == address) {
        previous->size += size;
        block = previous;
    } else {
        block = malloc(sizeof(*block));
        if (!block)
            return -ENOMEM;
        block->address = address;
        block->size = size;
        block->next = next;
        *link = block;
    }
    if (next && block->address + block->size == next->address) {
        block->size += next->size;
        block->next = next->next;
        free(next);
    }
    return 0;
}

uint32_t virge_texheap_free_bytes(const struct virge_texheap *heap)
{
    const struct virge_texheap_block *block;
    uint32_t total = 0;

    if (!heap)
        return 0;
    for (block = heap->free; block; block = block->next)
        total += block->size;
    return total;
}
