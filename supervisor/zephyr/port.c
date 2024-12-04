void port_wake_main_task(void) {
}

void port_wake_main_task_from_isr(void) {
}

void port_yield(void) {
}

void port_boot_info(void) {
}

void port_heap_init(void) {
    uint32_t *heap_bottom = port_heap_get_bottom();
    uint32_t *heap_top = port_heap_get_top();
    size_t size = (heap_top - heap_bottom) * sizeof(uint32_t);
    heap = tlsf_create_with_pool(heap_bottom, size, size);
}

void *port_malloc(size_t size, bool dma_capable) {
    void *block = tlsf_malloc(heap, size);
    return block;
}

void port_free(void *ptr) {
    tlsf_free(heap, ptr);
}

void *port_realloc(void *ptr, size_t size) {
    return tlsf_realloc(heap, ptr, size);
}
