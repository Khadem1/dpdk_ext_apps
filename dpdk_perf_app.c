#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_errno.h>
#include <rte_malloc.h>

#define MAX_QUEUES      8
#define RX_RING_SIZE    1024
#define TX_RING_SIZE    1024
#define NUM_MBUFS       8192
#define MBUF_CACHE_SZ   256
#define BURST_SIZE      32
#define STATS_INTERVAL_SEC 2

static volatile bool force_quit = false;
static struct rte_mempool *mbuf_pool = NULL;

/* stats per (port,queue) */
struct pq_stats {
    uint64_t rx;
    uint64_t tx;
    uint64_t dropped;
} __rte_cache_aligned;
static struct pq_stats *stats = NULL;

/* signal handler */
static void
sig_handler(int signum)
{
    (void)signum;
    force_quit = true;
}

/* print device capabilities (safe usage) */
static void
print_dev_caps(uint16_t port)
{
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port, &dev_info);
    (void)ret; /* ignore - used to avoid warning */
    printf("Port %u driver=%s max_rxq=%u max_txq=%u\n",
           port,
           dev_info.driver_name ? dev_info.driver_name : "unknown",
           dev_info.max_rx_queues, dev_info.max_tx_queues);
    if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP)
        printf("  PMD supports RX timestamp offload\n");
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)
        printf("  PMD supports TX TCP checksum offload\n");
}

/* Initialize port with nb_rxq / nb_txq queues and request common offloads */
static int
port_init(uint16_t port, uint16_t nb_rxq, uint16_t nb_txq)
{
    if (!rte_eth_dev_is_valid_port(port)) {
        fprintf(stderr, "Invalid port %u\n", port);
        return -1;
    }

    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));

    //port_conf.rxmode.max_rx_pkt_len = RTE_ETHER_MAX_LEN;
    port_conf.rxmode.max_lro_pkt_size = RTE_ETHER_MAX_LEN;
    port_conf.rxmode.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM | RTE_ETH_RX_OFFLOAD_TIMESTAMP;
    port_conf.txmode.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                                RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                                RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                                RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0) {
        fprintf(stderr, "rte_eth_dev_info_get failed: %s\n", rte_strerror(-ret));
    }

    ret = rte_eth_dev_configure(port, nb_rxq, nb_txq, &port_conf);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_dev_configure failed: %d\n", ret);
        return ret;
    }

    struct rte_eth_rxconf rxq_conf = dev_info.default_rxconf;
    rxq_conf.rx_thresh.pthresh = 8;
    rxq_conf.rx_thresh.hthresh = 8;
    rxq_conf.rx_thresh.wthresh = 4;

    for (uint16_t q = 0; q < nb_rxq; q++) {
        ret = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE, rte_eth_dev_socket_id(port), &rxq_conf, mbuf_pool);
        if (ret < 0) {
            fprintf(stderr, "rx_queue_setup port %u q %u failed: %d\n", port, q, ret);
            return ret;
        }
    }

    struct rte_eth_txconf txq_conf = dev_info.default_txconf;
    txq_conf.tx_free_thresh = 0;
    txq_conf.tx_thresh.pthresh = 32;

    for (uint16_t q = 0; q < nb_txq; q++) {
        ret = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE, rte_eth_dev_socket_id(port), &txq_conf);
        if (ret < 0) {
            fprintf(stderr, "tx_queue_setup port %u q %u failed: %d\n", port, q, ret);
            return ret;
        }
    }

    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_dev_start failed for port %u: %d\n", port, ret);
        return ret;
    }

    rte_eth_promiscuous_enable(port);
    print_dev_caps(port);
    return 0;
}

/* worker: handle one (port,queue) pair; arg is (uintptr_t)queue_id */
static int
lcore_forward(void *arg)
{
    const uint16_t port = 0; /* single port example */
    const uint16_t q = (uint16_t)(uintptr_t)arg;
    struct rte_mbuf *bufs[BURST_SIZE];

    uint64_t last_tsc = rte_get_tsc_cycles();
    const uint64_t tsc_hz = rte_get_timer_hz();
    const uint64_t stats_tsc_period = tsc_hz * STATS_INTERVAL_SEC;

    printf("lcore %u: forwarding on port %u queue %u\n", rte_lcore_id(), port, q);

    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(port, q, bufs, BURST_SIZE);
        if (unlikely(nb_rx == 0)) {
            rte_pause(); /* polite busy-wait */
            continue;
        }

        /* prefetch first few packets */
        uint16_t p = (nb_rx < 4) ? nb_rx : 4;
        for (uint16_t i = 0; i < p; i++)
            rte_prefetch0(rte_pktmbuf_mtod(bufs[i], void *));

        /* per-packet quick processing & timestamp */
        uint64_t now_cycles = rte_get_timer_cycles();
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = bufs[i];

            /* store software timestamp (cycles) in udata64 for later use */
            //m->udata64 = now_cycles;

            /* tiny in-place L2 swap (cheap, demo only) */
            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            struct rte_ether_addr tmp;
            rte_ether_addr_copy(&eth->src_addr, &tmp);
            rte_ether_addr_copy(&eth->dst_addr, &eth->src_addr);
            rte_ether_addr_copy(&tmp, &eth->dst_addr);

            stats[q].rx++;
        }

        /* transmit on same port/queue (simple forward); production code maps dst appropriately */
        uint16_t nb_tx = rte_eth_tx_burst(port, q, bufs, nb_rx);
        if (unlikely(nb_tx < nb_rx)) {
            for (uint16_t i = nb_tx; i < nb_rx; i++) {
                rte_pktmbuf_free(bufs[i]);
                stats[q].dropped++;
            }
        } else {
            stats[q].tx += nb_tx;
        }

        /* periodic stats print from one worker core (cooperative) */
        if ((rte_get_tsc_cycles() - last_tsc) > stats_tsc_period) {
            uint64_t rx_sum = 0, tx_sum = 0, drop_sum = 0;
            for (int i = 0; i < MAX_QUEUES; i++) {
                rx_sum += stats[i].rx;
                tx_sum += stats[i].tx;
                drop_sum += stats[i].dropped;
            }
            printf("[lcore %u] totals: rx=%"PRIu64" tx=%"PRIu64" drop=%"PRIu64"\n",
                   rte_lcore_id(), rx_sum, tx_sum, drop_sum);
            last_tsc = rte_get_tsc_cycles();
        }
    }

    return 0;
}

int
main(int argc, char **argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "EAL init failed\n");
        return -1;
    }

    argc -= ret;
    argv += ret;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        fprintf(stderr, "No ports available\n");
        return -1;
    }

    /* create mbuf pool */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SZ, 0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL) {
        fprintf(stderr, "Cannot create mbuf pool\n");
        return -1;
    }

    /* allocate stats array sized for MAX_QUEUES */
    stats = rte_zmalloc("stats", sizeof(struct pq_stats) * MAX_QUEUES, RTE_CACHE_LINE_SIZE);
    if (!stats) {
        fprintf(stderr, "Failed to allocate stats\n");
        return -1;
    }

    /* init first port with MAX_QUEUES rx/tx */
    uint16_t port_id = 0;
    if (port_init(port_id, MAX_QUEUES, MAX_QUEUES) != 0) {
        fprintf(stderr, "port_init failed\n");
        return -1;
    }

    /* Launch one worker per RX queue (assign to slave lcores) */
    unsigned q = 0;
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (q >= MAX_QUEUES) break;
        printf("Launching lcore %u for queue %u\n", lcore_id, q);
        rte_eal_remote_launch(lcore_forward, (void *)(uintptr_t)q, lcore_id);
        q++;
    }

    /* If fewer slave lcores than queues, use master core(s) as well */
    if (q < MAX_QUEUES) {
        /* use master as needed */
        unsigned master = rte_lcore_id();
        for (; q < MAX_QUEUES; q++) {
            printf("Launching master for queue %u (fallback)\n", q);
            lcore_forward((void *)(uintptr_t)q);
        }
    }

    /* wait for workers */
    rte_eal_mp_wait_lcore();

    /* cleanup */
    printf("Stopping port %u\n", port_id);
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);

    printf("Bye\n");
    return 0;
}
