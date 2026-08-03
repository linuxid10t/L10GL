#ifndef L10GL_VIRGE_TEXHEAP_H
#define L10GL_VIRGE_TEXHEAP_H

#include <stdint.h>

struct virge_texheap_block;

struct virge_texheap {
    uint32_t start;
    uint32_t end;
    struct virge_texheap_block *free;
};

int virge_texheap_init(struct virge_texheap *heap, uint32_t start,
                       uint32_t end);
void virge_texheap_destroy(struct virge_texheap *heap);
int virge_texheap_alloc(struct virge_texheap *heap, uint32_t size,
                        uint32_t *address);
int virge_texheap_free(struct virge_texheap *heap, uint32_t address,
                       uint32_t size);
uint32_t virge_texheap_free_bytes(const struct virge_texheap *heap);

#endif
