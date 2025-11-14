// dpdk_scale_demo_fixed.c
// DPDK ScaleMate Demo (single-file, fixed RX handling)
// - Proper DPDK port init + mbuf pool
// - RX worker that polls and frees mbufs (ensures RX counters update)
// - ncurses dashboard with per-port PPS/BPS and Scale decision
// - Demo low thresholds for visibility
// - Graceful shutdown (Ctrl-C)

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <ncurses.h>
#include <time.h>
#include <signal.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_launch.h>

//
// CONFIG (tweak these for your NIC)
//
#define LINK_CAPACITY_GBPS 100.0   // adjust to NIC (10/25/40/100)
#define INTERVAL_MS 1000

// Demo (low) thresholds
#define DEMO_CPU_THRESH 0.20       // 20%
#define DEMO_RING_FILL  0.10       // 10%
#define DEMO_DROP_RATIO 0.001      // 0.1%
#define DEMO_RX_UTIL    0.30       // 30%

// mbuf pool params
#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256
#define BURST_SIZE 32

// Custom ncurses color IDs
#define CLR_RED     1
#define CLR_GREEN   2
#define CLR_YELLOW  3
#define CLR_CYAN    4

// CLI
static bool opt_verbose = false;
static bool opt_color = true;

// graceful shutdown
static volatile sig_atomic_t stop_requested = 0;
static void handle_sigint(int _) { (void)_; stop_requested = 1; }

// helper: monotonic time
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// CPU util from /proc/stat (fraction 0..1)
static double get_cpu_util_percent(void)
{
    static unsigned long long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat","r");
    if (!f) return 0.0;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0.0; }
    fclose(f);

    unsigned long long user=0, nice=0, systemv=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
    int n = sscanf(line, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &systemv, &idle, &iowait, &irq, &softirq, &steal);
    if (n < 4) return 0.0;

    unsigned long long idle_all = idle + iowait;
    unsigned long long non_idle = user + nice + systemv + irq + softirq + steal;
    unsigned long long total = idle_all + non_idle;

    unsigned long long diff_total = (total > prev_total) ? (total - prev_total) : 0;
    unsigned long long diff_idle  = (idle_all > prev_idle) ? (idle_all - prev_idle) : 0;

    prev_total = total;
    prev_idle  = idle_all;

    if (diff_total == 0) return 0.0;
    double util = 1.0 - ((double)diff_idle / (double)diff_total);
    if (util < 0) util = 0; if (util > 1) util = 1;
    return util;
}

// decision types
typedef enum { DECISION_STABLE=0, DECISION_SCALE_UP, DECISION_SCALE_OUT } decision_t;

static decision_t decide_demo(double rx_util, double cpu_util, double ring_fill, double drop_ratio, char *reason, size_t rlen)
{
    if (cpu_util > DEMO_CPU_THRESH || ring_fill > DEMO_RING_FILL || drop_ratio > DEMO_DROP_RATIO) {
        snprintf(reason, rlen, "Scale-Up: cpu=%.0f%% ring=%.1f%% drops=%.4f",
                 cpu_util*100.0, ring_fill*100.0, drop_ratio);
        return DECISION_SCALE_UP;
    }
    if (rx_util > DEMO_RX_UTIL && cpu_util > DEMO_CPU_THRESH) {
        snprintf(reason, rlen, "Scale-Out: rx=%.1f%% cpu=%.0f%%", rx_util*100.0, cpu_util*100.0);
        return DECISION_SCALE_OUT;
    }
    snprintf(reason, rlen, "Stable: rx=%.1f%% cpu=%.0f%% ring=%.1f%% drops=%.4f",
             rx_util*100.0, cpu_util*100.0, ring_fill*100.0, drop_ratio);
    return DECISION_STABLE;
}

// UI helpers
static void ui_init(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(FALSE);
    if (opt_color && has_colors()) {
        start_color();
        init_pair(CLR_RED, COLOR_RED, COLOR_BLACK);
        init_pair(CLR_GREEN, COLOR_GREEN, COLOR_BLACK);
        init_pair(CLR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
        init_pair(CLR_CYAN, COLOR_CYAN, COLOR_BLACK);
    }
}
static void ui_shutdown(void) {
    endwin();
}

// global for RX thread control
static volatile int rx_thread_running = 0;
static uint16_t rx_worker_port = 0;
static struct rte_mempool *global_mbuf_pool = NULL;

// RX worker: poll and free pkts so that RX counters update reliably.
// Run on a slave lcore via rte_eal_remote_launch.
static int rx_worker_main(__rte_unused void *arg)
{
    uint16_t port = rx_worker_port;
    struct rte_mbuf *pkts[BURST_SIZE];
    rx_thread_running = 1;
    while (!stop_requested) {
        const uint16_t nb_rx = rte_eth_rx_burst(port, 0, pkts, BURST_SIZE);
        if (nb_rx) {
            for (uint16_t i = 0; i < nb_rx; ++i) {
                rte_pktmbuf_free(pkts[i]);
            }
        } else {
            // small pause to avoid burning CPU too hard if idle
            rte_pause();
        }
    }
    rx_thread_running = 0;
    return 0;
}

// Initialize one port with 1 RX and 1 TX queue (sufficient for demo).
static int port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    int ret;
    struct rte_eth_conf port_conf = {0};
    const uint16_t rx_rings = 1, tx_rings = 1;
    const uint16_t rx_ring_size = 1024, tx_ring_size = 1024;

    if (!rte_eth_dev_is_valid_port(port)) {
        fprintf(stderr, "Invalid port %u\n", port);
        return -1;
    }

    ret = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_dev_configure failed: %d\n", ret);
        return ret;
    }

    for (uint16_t q = 0; q < rx_rings; q++) {
        ret = rte_eth_rx_queue_setup(port, q, rx_ring_size, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (ret < 0) {
            fprintf(stderr, "rte_eth_rx_queue_setup failed: %d\n", ret);
            return ret;
        }
    }
    for (uint16_t q = 0; q < tx_rings; q++) {
        ret = rte_eth_tx_queue_setup(port, q, tx_ring_size, rte_eth_dev_socket_id(port), NULL);
        if (ret < 0) {
            fprintf(stderr, "rte_eth_tx_queue_setup failed: %d\n", ret);
            return ret;
        }
    }

    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_dev_start failed: %d\n", ret);
        return ret;
    }

    rte_eth_promiscuous_enable(port);

    // check link up status
    struct rte_eth_link link;
    rte_eth_link_get_nowait(port, &link);
    if (!link.link_status) {
        fprintf(stderr, "Warning: port %u link is DOWN\n", port);
    }

    return 0;
}

int main(int argc, char **argv)
{
    // parse args
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0) opt_verbose = true;
        if (strcmp(argv[i], "--no-color") == 0) opt_color = false;
    }

    signal(SIGINT, handle_sigint);

    // init DPDK EAL
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "EAL init failed\n");
        return 1;
    }

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        fprintf(stderr, "No DPDK ports found\n");
        return 1;
    }

    // create mbuf pool
    char pool_name[32];
    snprintf(pool_name, sizeof(pool_name), "MBUF_POOL_%d", getpid());
    global_mbuf_pool = rte_pktmbuf_pool_create(pool_name, NUM_MBUFS * nb_ports,
                                               MBUF_CACHE_SIZE, 0,
                                               RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (global_mbuf_pool == NULL) {
        fprintf(stderr, "Failed to create mbuf pool\n");
        return 1;
    }

    // for demo, we'll init port 0 (you can extend to loop ports)
    uint16_t port = 0;
    if (port_init(port, global_mbuf_pool) != 0) {
        fprintf(stderr, "Port init failed\n");
        return 1;
    }
    rx_worker_port = port;

    // launch RX worker on a slave lcore
    unsigned lcore_id = rte_get_next_lcore(rte_lcore_id(), 1, 0);
    if (lcore_id == RTE_MAX_LCORE) {
        fprintf(stderr, "No slave lcore available for rx thread; continuing without active rx worker\n");
    } else {
        ret = rte_eal_remote_launch(rx_worker_main, NULL, lcore_id);
        if (ret != 0) {
            fprintf(stderr, "Failed to launch rx worker on lcore %u\n", lcore_id);
        } else {
            if (opt_verbose) printf("RX worker launched on lcore %u\n", lcore_id);
        }
    }

    // allocate previous counters arrays for all ports
    uint16_t max_ports = nb_ports;
    uint64_t *prev_ipackets = calloc(max_ports, sizeof(uint64_t));
    uint64_t *prev_opackets = calloc(max_ports, sizeof(uint64_t));
    uint64_t *prev_ibytes   = calloc(max_ports, sizeof(uint64_t));
    uint64_t *prev_obytes   = calloc(max_ports, sizeof(uint64_t));
    uint64_t *prev_imissed  = calloc(max_ports, sizeof(uint64_t));
    uint64_t *prev_errors   = calloc(max_ports, sizeof(uint64_t));
    if (!prev_ipackets || !prev_opackets || !prev_ibytes || !prev_obytes || !prev_imissed || !prev_errors) {
        fprintf(stderr, "Allocation failed\n");
        return 1;
    }

    double last_time = now_s();

    // init UI
    ui_init();

    // header
    mvprintw(0,0,"DPDK ScaleMate Demo (fixed RX)   Ports=%u   Interval=%d ms", nb_ports, INTERVAL_MS);
    mvprintw(1,0,"Demo thresholds: SCALE-UP cpu>20%% or ring>10%% or drops>0.1%% | SCALE-OUT rx>30%% & cpu>20%%");
    mvprintw(3,0,"+------+---------+---------+---------+--------+-------+-------------------------+");
    mvprintw(4,0,"| Port | Rx-pps  | Tx-pps  | Rx-bps  | Drop%  |  CPU  | Decision / Reason       |");
    mvprintw(5,0,"+------+---------+---------+---------+--------+-------+-------------------------+");

    while (!stop_requested) {
        double t0 = now_s();
        for (uint16_t p = 0; p < nb_ports; ++p) {
            struct rte_eth_stats st;
            if (rte_eth_stats_get(p, &st) != 0) {
                mvprintw(7 + p, 0, "| %3u  |  stat read error                                   |", (unsigned)p);
                continue;
            }

            // compute deltas
            uint64_t d_ipackets = (st.ipackets >= prev_ipackets[p]) ? (st.ipackets - prev_ipackets[p]) : st.ipackets;
            uint64_t d_opackets = (st.opackets >= prev_opackets[p]) ? (st.opackets - prev_opackets[p]) : st.opackets;
            uint64_t d_ibytes   = (st.ibytes   >= prev_ibytes[p] ) ? (st.ibytes   - prev_ibytes[p]) : st.ibytes;
            uint64_t d_obytes   = (st.obytes   >= prev_obytes[p] ) ? (st.obytes   - prev_obytes[p]) : st.obytes;
            uint64_t d_imissed  = (st.imissed  >= prev_imissed[p]) ? (st.imissed  - prev_imissed[p]) : st.imissed;
            uint64_t d_errors   = (((uint64_t)st.ierrors + st.oerrors) >= prev_errors[p]) ? ((uint64_t)st.ierrors + st.oerrors - prev_errors[p]) : (uint64_t)st.ierrors + st.oerrors;

            double t1 = now_s();
            double dt = t1 - last_time;
            if (dt <= 0) dt = INTERVAL_MS / 1000.0;

            double rx_pps = (double)d_ipackets / dt;
            double tx_pps = (double)d_opackets / dt;
            double rx_bps = (double)d_ibytes * 8.0 / dt;
            double tx_bps = (double)d_obytes * 8.0 / dt;

            double link_bps = LINK_CAPACITY_GBPS * 1e9;
            double rx_util = (rx_bps / link_bps);
            if (rx_util < 0) rx_util = 0; if (rx_util > 1) rx_util = 1;

            // drop ratio: missed / (rx + missed)
            double drop_ratio = 0.0;
            uint64_t total_seen = d_ipackets + d_imissed;
            if (total_seen > 0) drop_ratio = (double)d_imissed / (double)total_seen;

            // approximate ring_fill: cumulative imissed / (ipackets + imissed + 1)
            double ring_fill = 0.0;
            uint64_t denom = st.ipackets + st.imissed + 1;
            if (denom > 0) ring_fill = (double)st.imissed / (double)denom;
            if (ring_fill < 0) ring_fill = 0; if (ring_fill > 1) ring_fill = 1;

            double cpu_util = get_cpu_util_percent();

            char reason[128];
            decision_t dec = decide_demo(rx_util, cpu_util, ring_fill, drop_ratio, reason, sizeof(reason));

            const char *dec_text = "Stable";
            int color = CLR_GREEN;
            if (dec == DECISION_SCALE_UP)  { dec_text = "SCALE-UP";  color = CLR_YELLOW; }
            if (dec == DECISION_SCALE_OUT) { dec_text = "SCALE-OUT"; color = CLR_RED; }

            if (opt_color && has_colors()) attron(COLOR_PAIR(color));
            mvprintw(7 + p, 0,
                     "| %4u | %7.0f | %7.0f | %7.0fk | %6.3f | %5.1f%% | %-23s |",
                     (unsigned)p, rx_pps, tx_pps, rx_bps/1000.0, drop_ratio*100.0, cpu_util*100.0, reason);
            if (opt_color && has_colors()) attroff(COLOR_PAIR(color));

            // save previous counters
            prev_ipackets[p] = st.ipackets;
            prev_opackets[p] = st.opackets;
            prev_ibytes[p]   = st.ibytes;
            prev_obytes[p]   = st.obytes;
            prev_imissed[p]  = st.imissed;
            prev_errors[p]   = (uint64_t)st.ierrors + st.oerrors;
        }

        int last_row = 7 + nb_ports;
        mvprintw(last_row, 0, "+------+---------+---------+---------+--------+-------+-------------------------+");
        refresh();

        // wait remaining
        double elapsed = now_s() - t0;
        double to_wait = (INTERVAL_MS / 1000.0) - elapsed;
        if (to_wait > 0) usleep((useconds_t)(to_wait * 1e6));
        last_time = now_s();
    }

    // stop RX worker (signal and wait)
    stop_requested = 1;
    // wait a short moment for worker to stop
    for (int i = 0; i < 50 && rx_thread_running; ++i) usleep(10000);

    ui_shutdown();
    free(prev_ipackets); free(prev_opackets); free(prev_ibytes);
    free(prev_obytes); free(prev_imissed); free(prev_errors);

    printf("\nExiting cleanly\n");
    return 0;
}
