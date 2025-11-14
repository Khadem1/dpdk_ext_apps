// dpdk_scale_demo.c
// DPDK ScaleMate Demo (single-file)
// - DPDK stats via rte_eth_stats_get
// - CPU util from /proc/stat
// - ncurses dashboard
// - Demo LOW thresholds to show Scale-Up and Scale-Out

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <ncurses.h>
#include <time.h>

#include <rte_eal.h>
#include <rte_ethdev.h>

// ---------------- Config / Demo thresholds ----------------
#define LINK_CAPACITY_GBPS 100.0   // adjust to your NIC
#define INTERVAL_MS 1000
// Demo (low) thresholds:
#define DEMO_CPU_THRESH 0.20       // 20%
#define DEMO_RING_FILL  0.10       // 10%
#define DEMO_DROP_RATIO 0.001      // 0.1%
#define DEMO_RX_UTIL    0.30       // 30%

// Custom ncurses color IDs (avoid collisions)
#define CLR_RED     1
#define CLR_GREEN   2
#define CLR_YELLOW  3
#define CLR_CYAN    4

// CLI options
static bool opt_verbose = false;
static bool opt_color = true;

// ---------------- Utility: current time in seconds (double) --------------
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------- CPU util (from /proc/stat) ----------------
static double get_cpu_util_percent()
{
    static unsigned long long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0.0; }
    fclose(f);

    // parse: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    unsigned long long user=0, nice=0, systemv=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
    sscanf(line, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &systemv, &idle, &iowait, &irq, &softirq, &steal);

    unsigned long long idle_all = idle + iowait;
    unsigned long long non_idle = user + nice + systemv + irq + softirq + steal;
    unsigned long long total = idle_all + non_idle;

    unsigned long long diff_total = total - prev_total;
    unsigned long long diff_idle = idle_all - prev_idle;

    prev_total = total;
    prev_idle = idle_all;

    if (diff_total == 0) return 0.0;
    double util = 1.0 - ((double)diff_idle / (double)diff_total);
    if (util < 0) util = 0;
    if (util > 1) util = 1;
    return util; // fraction 0..1
}

// ---------------- ncurses UI helpers ----------------
static void ui_init(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(FALSE);
    if (has_colors()) {
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

// ---------------- Scale decision (demo thresholds) ----------------
typedef enum { DECISION_STABLE=0, DECISION_SCALE_UP, DECISION_SCALE_OUT } decision_t;

static decision_t decide_demo(double rx_util, double cpu_util, double ring_fill, double drop_ratio, char *reason, size_t rlen)
{
    // SCALE-UP if CPU > 20% OR ring_fill > 10% OR drops > 0.1%
    if (cpu_util > DEMO_CPU_THRESH || ring_fill > DEMO_RING_FILL || drop_ratio > DEMO_DROP_RATIO) {
        snprintf(reason, rlen, "Scale-Up: cpu=%.0f%% ring=%.1f%% drops=%.4f",
                 cpu_util*100.0, ring_fill*100.0, drop_ratio);
        return DECISION_SCALE_UP;
    }
    // SCALE-OUT if rx_util>30% AND cpu>20%
    if (rx_util > DEMO_RX_UTIL && cpu_util > DEMO_CPU_THRESH) {
        snprintf(reason, rlen, "Scale-Out: rx=%.1f%% cpu=%.0f%%",
                 rx_util*100.0, cpu_util*100.0);
        return DECISION_SCALE_OUT;
    }
    snprintf(reason, rlen, "Stable: rx=%.1f%% cpu=%.0f%% ring=%.1f%% drops=%.4f",
             rx_util*100.0, cpu_util*100.0, ring_fill*100.0, drop_ratio);
    return DECISION_STABLE;
}

// ---------------- Main program ----------------
int main(int argc, char **argv)
{
    // simple CLI parse
    for (int i=1;i<argc;i++){
        if (strcmp(argv[i],"--verbose")==0) opt_verbose=true;
        if (strcmp(argv[i],"--no-color")==0) opt_color=false;
    }

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

    // allocate arrays for previous counters
    uint64_t *prev_ipackets = calloc(nb_ports, sizeof(uint64_t));
    uint64_t *prev_opackets = calloc(nb_ports, sizeof(uint64_t));
    uint64_t *prev_ibytes   = calloc(nb_ports, sizeof(uint64_t));
    uint64_t *prev_obytes   = calloc(nb_ports, sizeof(uint64_t));
    uint64_t *prev_imissed  = calloc(nb_ports, sizeof(uint64_t));
    uint64_t *prev_errors   = calloc(nb_ports, sizeof(uint64_t));
    double   *prev_time     = calloc(1, sizeof(double));

    double last_time = now_s();
    prev_time[0] = last_time;

    ui_init();

    // header row
    mvprintw(0,0,"DPDK ScaleMate Demo (low thresholds)   Ports=%u   Interval=%d ms", nb_ports, INTERVAL_MS);
    mvprintw(1,0,"Thresholds (demo): SCALE-UP cpu>20%% or ring>10%% or drops>0.1%% | SCALE-OUT rx>30%% and cpu>20%%");
    mvprintw(3,0,"+------+---------+---------+---------+--------+-------+-------------------------+");
    mvprintw(4,0,"| Port | Rx-pps  | Tx-pps  | Rx-bps  | Drop%  |  CPU  | Decision / Reason       |");
    mvprintw(5,0,"+------+---------+---------+---------+--------+-------+-------------------------+");

    while (1) {
        double t0 = now_s();
        // read stats for each port
        for (uint16_t port=0; port<nb_ports; port++) {
            struct rte_eth_stats st;
            if (rte_eth_stats_get(port, &st) != 0) {
                mvprintw(7+port,0,"| %3u  |  stat read error                                   |", port);
                continue;
            }

            // deltas
            uint64_t d_ipackets = st.ipackets - prev_ipackets[port];
            uint64_t d_opackets = st.opackets - prev_opackets[port];
            uint64_t d_ibytes   = st.ibytes   - prev_ibytes[port];
            uint64_t d_obytes   = st.obytes   - prev_obytes[port];
            uint64_t d_imissed  = st.imissed  - prev_imissed[port];
            uint64_t d_errors   = (st.ierrors + st.oerrors) - prev_errors[port];

            double t1 = now_s();
            double dt = t1 - last_time;
            if (dt <= 0) dt = INTERVAL_MS / 1000.0;

            double rx_pps = d_ipackets / dt;
            double tx_pps = d_opackets / dt;
            double rx_bps = (double)d_ibytes * 8.0 / dt;
            double tx_bps = (double)d_obytes * 8.0 / dt;

            // util relative to link capacity
            double link_bps = LINK_CAPACITY_GBPS * 1e9;
            double rx_util = (rx_bps / link_bps); if (rx_util < 0) rx_util = 0; if (rx_util > 1) rx_util = 1;

            // drop ratio (d_imissed relative to total pkts seen)
            double drop_ratio = 0.0;
            uint64_t total_seen = (d_ipackets + d_imissed);
            if (total_seen > 0) drop_ratio = (double)d_imissed / (double)total_seen;

            // approximate ring_fill: if imissed increases a lot, consider ring congested
            double ring_fill = (double)st.imissed / (double)(st.ipackets + st.imissed + 1);
            if (ring_fill < 0) ring_fill = 0; if (ring_fill > 1) ring_fill = 1;

            // CPU util
            double cpu_util = get_cpu_util_percent();

            // decision (demo thresholds)
            char reason[128];
            decision_t dec = decide_demo(rx_util, cpu_util, ring_fill, drop_ratio, reason, sizeof(reason));

            // format decision text
            const char *dec_text = "Stable";
            int color = CLR_GREEN;
            if (dec == DECISION_SCALE_UP) { dec_text = "SCALE-UP"; color = CLR_YELLOW; }
            else if (dec == DECISION_SCALE_OUT) { dec_text = "SCALE-OUT"; color = CLR_RED; }

            // print row
            if (has_colors() && opt_color) attron(COLOR_PAIR(color));
            mvprintw(7+port, 0,
                     "| %4u | %7.0f | %7.0f | %7.0fk | %6.3f | %5.1f%% | %-23s |",
                     port, rx_pps, tx_pps, rx_bps/1000.0, drop_ratio*100.0, cpu_util*100.0, reason);
            if (has_colors() && opt_color) attroff(COLOR_PAIR(color));

            // store previous
            prev_ipackets[port] = st.ipackets;
            prev_opackets[port] = st.opackets;
            prev_ibytes[port]   = st.ibytes;
            prev_obytes[port]   = st.obytes;
            prev_imissed[port]  = st.imissed;
            prev_errors[port]   = st.ierrors + st.oerrors;
        }

        // finalize row boundary
        int last_row = 7 + nb_ports;
        mvprintw(last_row, 0, "+------+---------+---------+---------+--------+-------+-------------------------+");

        refresh();

        // sleep remaining interval
        double t_elapsed = now_s() - t0;
        double to_wait = (INTERVAL_MS / 1000.0) - t_elapsed;
        if (to_wait > 0) usleep((useconds_t)(to_wait * 1e6));
        last_time = now_s();
    }

    // cleanup (never reached)
    ui_shutdown();
    free(prev_ipackets); free(prev_opackets); free(prev_ibytes);
    free(prev_obytes); free(prev_imissed); free(prev_errors); free(prev_time);
    return 0;
}
