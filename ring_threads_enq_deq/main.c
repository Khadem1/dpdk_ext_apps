#include <rte_ring.h>
#include <rte_mempool.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>

struct data_struct {
    int msg_type;
    int msg_code;
    int msg_len;
    char data[0];
};

int nr_of_queue_node = 4096;
struct rte_ring *ring = NULL;

struct my_data {
    int value;
};

void *enqueue_ring (void *arg)
{
    struct data_struct *data = NULL;
    while (1){
        if (rte_ring_full(ring)){
            printf("full \n");
            continue;
        }
        data = malloc(512);
        assert(rte_ring_enqueue(ring, data) == 0);
    }
}

void *dequeue_ring(void *arg){
    struct data_struct *data = NULL;
    while (1){
        if (rte_ring_empty(ring)){
            printf("empty \n");
            continue;
        }
        assert(rte_ring_dequeue(ring, data) == 0);
        free(data);
    }
}

int main(int argc, char **argv) {
    //struct rte_ring *ring;
    struct rte_mempool *mempool;
    struct my_data *data, *dequeued_data;
    int ret;
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        printf("eal init fail!!!\n");
    unsigned lcore_id;
    lcore_id = rte_lcore_id();
    printf("lcore %u\n", lcore_id);
    pthread_t penqueue1;
    pthread_t pdequeue1;

    ring = rte_ring_create("my_ring", nr_of_queue_node, SOCKET_ID_ANY, RING_F_SC_DEQ);
    if (!ring) {
        printf("Failed to create ring\n");
        return -1;
    }
    mempool = rte_mempool_create("MP", 1024,
                                32, 32, 0,
                                NULL, NULL, NULL, NULL,
                                0, 0);
    
    pthread_create(&penqueue1, NULL, enqueue_ring, NULL);
    pthread_create(&pdequeue1, NULL, dequeue_ring, NULL);

    while (1){
        sleep(1);
    }

    return 0;
}
