// test_nodpdk.c
// Minimal testpmd-like packet pipeline without DPDK.
// - 8 RX queues -> 8 worker threads -> 8 TX threads
// - Simulated FPGA NIC sets hardware timestamps (T_hw_rx/T_hw_tx)
// - Uses SPSC lock-free rings for queue handoff
// - Stores sw timestamps in packet->sw_ts_ns (for latency calc)
//
// Build:
//   gcc -O3 -march=native test_nodpdk.c -o test_nodpdk -pthread
//
// Run:
//   ./test_nodpdk
//
// Ctrl+C to stop and print final stats.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>

#define N_QUEUES 8
#define RING_SIZE 1024  // must be power of two
#define RING_MASK (RING_SIZE - 1)
#define BATCH_SIZE 32
#define PKT_PAYLOAD 64

// Offload flags (simulated)
#define OFFLOAD_TS   (1 << 0)
#define OFFLOAD_CSUM (1 << 1)
#define OFFLOAD_VLAN (1 << 2)

static volatile bool running = true;

static inline uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t); // using realtime for clarity; can use CLOCK_MONOTONIC
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

// Simple packet representation
typedef struct {
    uint8_t payload[PKT_PAYLOAD];
    uint16_t len;
    uint32_t offloads;      // bits for offloads requested/supported
    uint64_t hw_rx_ts_ns;   // hardware ingress timestamp (set by "NIC/FPGA")
    uint64_t sw_rx_ts_ns;   // software timestamp taken on dequeue by app
    uint64_t hw_tx_ts_ns;   // hardware egress timestamp (set by "NIC/FPGA" on send)
} pkt_t;

// Single-producer single-consumer ring (lock-free)
typedef struct {
    pkt_t buffer[RING_SIZE];
    atomic_size_t head; // next write index (producer)
    atomic_size_t tail; // next read index (consumer)
} spsc_ring_t;

static spsc_ring_t rx_rings[N_QUEUES];
static spsc_ring_t tx_rings[N_QUEUES];

// Stats per queue
typedef struct {
    uint64_t rx_pkts;
    uint64_t proc_pkts;
    uint64_t tx_pkts;
    uint64_t dropped;
    uint64_t hw_latency_sum_ns; // sum of (hw_tx - hw_rx)
    uint64_t sw_latency_sum_ns; // sum of (hw_tx - sw_rx)
} stats_t;

static stats_t stats[N_QUEUES];

// Ring operations (producer pushes, consumer pops). Return true on success.
static inline bool ring_push(spsc_ring_t *r, const pkt_t *p) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t next = (head + 1) & RING_MASK;
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (next == tail) return false; // full
    r->buffer[head & RING_MASK] = *p;
    atomic_store_explicit(&r->head, next, memory_order_release);
    return true;
}

static inline bool ring_pop(spsc_ring_t *r, pkt_t *p) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head) return false; // empty
    *p = r->buffer[tail & RING_MASK];
    atomic_store_explicit(&r->tail, (tail + 1) & RING_MASK, memory_order_release);
    return true;
}

// Simulated NIC/FPGA: generates packets into rx_rings for each queue (round-robin)
// It sets hw_rx timestamp and some offload flags.
void *nic_thread(void *arg) {
    (void)arg;
    uint64_t pkt_idx = 0;
    while (running) {
        for (int q = 0; q < N_QUEUES && running; q++) {
            // produce a small burst for queue q
            for (int b = 0; b < 4; b++) {
                pkt_t p;
                p.len = PKT_PAYLOAD;
                // fill payload with some pattern
                for (int i = 0; i < PKT_PAYLOAD; i++) p.payload[i] = (uint8_t)(pkt_idx + i);
                // simulate offloads available on NIC - pretend all offloads supported
                p.offloads = OFFLOAD_CSUM | OFFLOAD_TS | OFFLOAD_VLAN;
                p.hw_rx_ts_ns = now_ns(); // hw timestamp at ingress
                p.sw_rx_ts_ns = 0;
                p.hw_tx_ts_ns = 0;

                // try to push; if full, count drop and continue
                if (!ring_push(&rx_rings[q], &p)) {
                    stats[q].dropped++;
                } else {
                    stats[q].rx_pkts++;
                }
                pkt_idx++;
            }
            // simulate NIC rate - small sleep to avoid runaway CPU
            // In real system this is hardware-driven; here we use a tight pause to simulate high-rate
            // usleep(0) yields - but keep very small sleep occasionally
            if ((pkt_idx & 0x3FFF) == 0) usleep(100);
        }
    }
    return NULL;
}

// Worker per RX queue: pulls from rx_rings[q], does minimal processing, pushes to tx_rings[q]
void *worker_thread(void *arg) {
    int q = (int)(uintptr_t)arg;
    pkt_t batch[BATCH_SIZE];
    while (running) {
        int got = 0;
        // batch pop
        for (int i = 0; i < BATCH_SIZE; i++) {
            pkt_t p;
            if (!ring_pop(&rx_rings[q], &p)) break;
            batch[got++] = p;
        }
        if (got == 0) {
            // polite pause
            sched_yield();
            continue;
        }

        // prefetch-like: touch first bytes
        for (int i = 0; i < got && i < 4; i++) __builtin_prefetch(batch[i].payload);

        uint64_t sw_ts = now_ns();
        for (int i = 0; i < got; i++) {
            pkt_t *p = &batch[i];
            p->sw_rx_ts_ns = sw_ts; // mark software dequeue timestamp

            // Minimal in-path processing:
            // - swap first 6 bytes and next 6 bytes to mimic MAC swap (cheap)
            if (p->len >= 12) {
                for (int k = 0; k < 6; k++) {
                    uint8_t tmp = p->payload[k];
                    p->payload[k] = p->payload[6 + k];
                    p->payload[6 + k] = tmp;
                }
            }

            // Simulate checksum offload handling: we would check ol_flags; here we just pretend it's done
            // (no action needed in app hot path)

            stats[q].proc_pkts++;

            // push to tx ring, if full drop (simulate system drop)
            if (!ring_push(&tx_rings[q], p)) {
                stats[q].dropped++;
            }
        }
    }
    return NULL;
}

// TX thread per queue: consumes tx_rings[q], simulates DMA to NIC, NIC sets hw_tx timestamp and "sends"
void *tx_thread(void *arg) {
    int q = (int)(uintptr_t)arg;
    pkt_t p;
    while (running) {
        if (!ring_pop(&tx_rings[q], &p)) {
            sched_yield();
            continue;
        }
        // simulate DMA latency jitter (small)
        // nanosleep with 100-500 ns would be ideal, but nanosleep granularity is ms — so simulate by busy loop
        // very small busy loop to simulate processing latency
        uint64_t start = now_ns();
        while (now_ns() - start < 200) { /* spin ~200 ns */ __asm__ __volatile__(""); }

        // NIC assigns hw_tx timestamp on egress
        p.hw_tx_ts_ns = now_ns();

        // compute latencies
        if (p.hw_rx_ts_ns) {
            uint64_t dev_lat = (p.hw_tx_ts_ns > p.hw_rx_ts_ns) ? (p.hw_tx_ts_ns - p.hw_rx_ts_ns) : 0;
            stats[q].hw_latency_sum_ns += dev_lat;
        }
        if (p.sw_rx_ts_ns) {
            uint64_t e2e = (p.hw_tx_ts_ns > p.sw_rx_ts_ns) ? (p.hw_tx_ts_ns - p.sw_rx_ts_ns) : 0;
            stats[q].sw_latency_sum_ns += e2e;
        }

        stats[q].tx_pkts++;
        // "send" complete: in a real system packet goes out on wire. Here we drop/free it.
    }
    return NULL;
}

void print_stats_periodic(void) {
    uint64_t last_print = now_ns();
    while (running) {
        sleep(1);
        uint64_t now = now_ns();
        if (now - last_print >= 1000000000ULL) {
            printf("=== Stats (per-queue) ===\n");
            for (int q = 0; q < N_QUEUES; q++) {
                uint64_t rx = stats[q].rx_pkts;
                uint64_t proc = stats[q].proc_pkts;
                uint64_t tx = stats[q].tx_pkts;
                uint64_t drop = stats[q].dropped;
                uint64_t hw_lat_avg = (tx ? stats[q].hw_latency_sum_ns / tx : 0);
                uint64_t sw_lat_avg = (tx ? stats[q].sw_latency_sum_ns / tx : 0);
                printf("Q%02d: rx=%8" PRIu64 " proc=%8" PRIu64 " tx=%8" PRIu64 " drop=%6" PRIu64
                       " hw_lat_avg=%6" PRIu64 "ns sw_lat_avg=%6" PRIu64 "ns\n",
                       q, rx, proc, tx, drop, hw_lat_avg, sw_lat_avg);
            }
            printf("=========================\n");
            last_print = now;
        }
    }
}

// signal handler
void handle_sigint(int sig) {
    (void)sig;
    running = false;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Starting test_nodpdk simulation (8 queues). Ctrl+C to stop.\n");

    // init rings
    for (int i = 0; i < N_QUEUES; i++) {
        atomic_init(&rx_rings[i].head, 0);
        atomic_init(&rx_rings[i].tail, 0);
        atomic_init(&tx_rings[i].head, 0);
        atomic_init(&tx_rings[i].tail, 0);
        memset(&stats[i], 0, sizeof(stats[i]));
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // create NIC thread
    pthread_t nic;
    if (pthread_create(&nic, NULL, nic_thread, NULL) != 0) {
        perror("pthread_create(nic)");
        return 1;
    }

    // create worker threads (one per RX queue)
    pthread_t workers[N_QUEUES];
    for (int q = 0; q < N_QUEUES; q++) {
        if (pthread_create(&workers[q], NULL, worker_thread, (void *)(uintptr_t)q) != 0) {
            perror("pthread_create(worker)");
            return 1;
        }
    }

    // create tx threads (one per TX queue)
    pthread_t txs[N_QUEUES];
    for (int q = 0; q < N_QUEUES; q++) {
        if (pthread_create(&txs[q], NULL, tx_thread, (void *)(uintptr_t)q) != 0) {
            perror("pthread_create(tx)");
            return 1;
        }
    }

    // stats printer (main thread)
    print_stats_periodic();

    // join threads (cleanup)
    running = false;
    pthread_join(nic, NULL);
    for (int q = 0; q < N_QUEUES; q++) {
        pthread_join(workers[q], NULL);
        pthread_join(txs[q], NULL);
    }

    // final stats
    printf("Final stats:\n");
    for (int q = 0; q < N_QUEUES; q++) {
        uint64_t rx = stats[q].rx_pkts;
        uint64_t tx = stats[q].tx_pkts;
        uint64_t drop = stats[q].dropped;
        uint64_t hw_lat_avg = (tx ? stats[q].hw_latency_sum_ns / tx : 0);
        uint64_t sw_lat_avg = (tx ? stats[q].sw_latency_sum_ns / tx : 0);
        printf("Q%02d: rx=%" PRIu64 " tx=%" PRIu64 " drop=%" PRIu64 " hw_lat_avg=%" PRIu64 "ns sw_lat_avg=%" PRIu64 "ns\n",
               q, rx, tx, drop, hw_lat_avg, sw_lat_avg);
    }

    printf("Bye\n");
    return 0;
}
