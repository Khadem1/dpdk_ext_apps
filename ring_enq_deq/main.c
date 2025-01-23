#include <rte_ring.h>
#include <rte_mempool.h>
#include <stdio.h>
struct my_data {
    int value;
};
int main(int argc, char **argv) {
    struct rte_ring *ring;
    struct rte_mempool *mempool;
    struct my_data *data, *dequeued_data;
    int ret;
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        printf("eal init fail!!!\n");
    unsigned lcore_id;
    lcore_id = rte_lcore_id();
    printf("lcore %u\n", lcore_id);

    ring = rte_ring_create("my_ring", 128, SOCKET_ID_ANY, RING_F_SC_DEQ);
    if (!ring) {
        printf("Failed to create ring\n");
        return -1;
    }
    mempool = rte_mempool_create("MP", 1024,
                                32, 32, 0,
                                NULL, NULL, NULL, NULL,
                                0, 0);
    if (mempool == NULL) {
        printf("Failed to create the mempool\n");
        return -1;
    }

    for (int i = 0; i < 10; i++) {
        if (rte_mempool_get(mempool, (void **)&data) < 0) {
            printf("Failed to allocate object\n");
            return -1;
        }
        data->value = i;
        if (rte_ring_sp_enqueue(ring, data) < 0) {
            printf("Failed to enqueue object into the ring\n");
            rte_mempool_put(mempool, data);
        } else {
            printf("Enqueued object at address: %p\n", data);
            printf("Data enqueued: %d\n", data->value);

        }
    }

    // Dequeue and access objects from the ring
    for (int i = 0; i < 10; i++) {
        // Dequeue an object pointer from the ring
        if (rte_ring_sc_dequeue_burst(ring, (void **)&dequeued_data,1, NULL) < 0) {
            printf("Failed to dequeue object from the ring\n");
        } else {
            printf("Dequeued object at address: %p\n", dequeued_data);
            printf("dequeue data %d \n",dequeued_data->value);
            // Access and modify the object as needed
            // For example, cast to a known structure type:
            // struct my_struct *my_obj = (struct my_struct *)obj;
            // Access fields in `my_obj`, e.g., `my_obj->some_field = some_value;`

            // Return the object to the mempool after processing
            rte_mempool_put(mempool, dequeued_data);
        }
    }


    return 0;
}
