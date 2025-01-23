#include <stdio.h>
#include <stdint.h>
#include <rte_ring.h>
#include <rte_mempool.h>


int main(int argc, char **argv) {
    struct rte_ring *ring;
    struct rte_mempool *mempool;
    void *obj;
    int ret;
    ret = rte_eal_init(argc, argv);
    
    ring = rte_ring_create("my_ring", 128, SOCKET_ID_ANY, RING_F_SC_DEQ);
    if (!ring) {
        printf("Failed to create ring\n");
        return -1;
    }

    mempool = rte_mempool_create("mempool_1", 1024,
                                32, 32, 0,
                                NULL, NULL, NULL, NULL,
                                0, 0);
    if (mempool == NULL) {
        printf("Failed to create the mempool\n");
        return -1;
    }

    for (int i = 0; i < 10; i++) {
        if (rte_mempool_get(mempool, &obj) < 0) {
            printf("Failed to allocate object\n");
            return -1;
        }

        if (rte_ring_sp_enqueue(ring, obj) < 0) {
            printf("Failed to enqueue object into the ring\n");
            rte_mempool_put(mempool, obj);
        } else {
            printf("Enqueued object at address: %p\n", obj);
        }
    }

    for (int i = 0; i < 10; i++) {
        if (rte_ring_sc_dequeue(ring, &obj) < 0) {
            printf("Failed to dequeue object from the ring\n");
        } else {
            printf("Dequeued object at address: %p\n", obj);
            rte_mempool_put(mempool, obj);
        }
    }

    return 0;
}

