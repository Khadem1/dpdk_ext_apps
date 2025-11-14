// test_scaleup.c – DPDK Scale-Up / Scale-Out Telemetry Tool
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <ncurses.h>
#include <sys/sysinfo.h>

#include <rte_eal.h>
#include <rte_ethdev.h>

// --------------------- CONFIG ------------------------
#define LINK_CAPACITY_GBPS 100.0
#define T_WARN_DEFAULT 0.60
#define T_CRIT_DEFAULT 0.85
#define REFRESH_MS 1000

// Custom ncurses color IDs (avoid COLOR_CYAN conflict)
#define CLR_RED     1
#define CLR_GREEN   2
#define CLR_YELLOW  3
#define CLR_CYAN    4

// CLI options
bool opt_verbose = false;

// ---------------- SATURATION FUNCTIONS -----------------
double compute_SI(double util, double T_WARN, double T_CRIT) {
    if (util <= T_WARN)
        return (util / T_WARN) * 0.5;
    if (util < T_CRIT)
        return 0.5 + ((util - T_WARN) / (T_CRIT - T_WARN)) * 0.35;

    double tmp = 0.85 + ((util - T_CRIT) / (1.0 - T_CRIT)) * 0.15;
    return (tmp > 1.0) ? 1.0 : tmp;
}

double compute_CS(double bw_si, double cpu_si, double buf_si) {
    return (bw_si * 0.5 + cpu_si * 0.3 + buf_si * 0.2) * 100.0;
}

const char* decide_scale(double CS) {
    if (CS < 60) return "Scale-Up";
    if (CS < 85) return "Scale-Up (Warn)";
    return "Scale-Out";
}

// ---------------- CPU UTIL FUNCTION --------------------
double get_cpu_util() {
    static long prev_total = 0, prev_idle = 0;

    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) return 0.5;

    char buf[256];
    fgets(buf, sizeof(buf), fp);
    fclose(fp);

    long user, nicev, systemv, idle, iowait, irq, softirq, steal;
    sscanf(buf, "cpu  %ld %ld %ld %ld %ld %ld %ld %ld",
           &user,&nicev,&systemv,&idle,&iowait,&irq,&softirq,&steal);

    long idle_all = idle + iowait;
    long non_idle = user + nicev + systemv + irq + softirq + steal;
    long total = idle_all + non_idle;

    long diff_total = total - prev_total;
    long diff_idle  = idle_all - prev_idle;

    prev_total = total;
    prev_idle  = idle_all;

    if (diff_total == 0) return 0.5;

    double util = 1.0 - ((double)diff_idle / diff_total);
    if (util < 0) util = 0;
    if (util > 1) util = 1;
    return util;
}

// Dummy for now (replace with mempool stats later)
double get_buffer_util() {
    return 0.30; 
}

// ---------------- DASHBOARD ----------------------------
void init_dashboard() {
    initscr();
    cbreak();
    noecho();
    curs_set(FALSE);

    start_color();
    init_pair(CLR_RED, COLOR_RED, COLOR_BLACK);
    init_pair(CLR_GREEN, COLOR_GREEN, COLOR_BLACK);
    init_pair(CLR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
    init_pair(CLR_CYAN, COLOR_CYAN, COLOR_BLACK);
}

void print_header(uint16_t nb_ports, double T_WARN, double T_CRIT) {
    mvprintw(0,0,"DPDK Scale-Up/Scale-Out Monitor   Ports=%u  Refresh=%dms",
             nb_ports, REFRESH_MS);
    mvprintw(1,0,"Thresholds:  WARN=%.2f   CRIT=%.2f\n", T_WARN, T_CRIT);

    mvprintw(3,0,"+------+-------+-------+-------+-------+-----+------------------+");
    mvprintw(4,0,"|Port | BW%   | CPU%  | Buf%  |  SI   | CS  | Decision         |");
    mvprintw(5,0,"+------+-------+-------+-------+-------+-----+------------------+");
}

void print_port_row(
        int row, int port,
        double bw, double cpu, double buf,
        double SI, double CS,
        const char* decision)
{
    int color = CLR_GREEN;
    if (CS > 85) color = CLR_RED;
    else if (CS > 60) color = CLR_YELLOW;

    attron(COLOR_PAIR(color));
    mvprintw(row, 0,
        "| %3d | %5.1f | %5.1f | %5.1f | %.3f | %3.0f | %-16s |",
        port, bw*100, cpu*100, buf*100, SI, CS, decision);
    attroff(COLOR_PAIR(color));
}

// -------------------- MAIN ------------------------------
int main(int argc, char** argv)
{
    // CLI parse (minimal)
    for (int i=1; i<argc; i++)
        if (strcmp(argv[i], "--verbose") == 0)
            opt_verbose = true;

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "ERROR: EAL init failed\n");

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "ERROR: No DPDK ports available\n");

    double T_WARN = T_WARN_DEFAULT;
    double T_CRIT = T_CRIT_DEFAULT;

    init_dashboard();

    while (1) {
        print_header(nb_ports, T_WARN, T_CRIT);
        int row = 6;

        for (uint16_t port = 0; port < nb_ports; port++) {
            struct rte_eth_stats st;
            if (rte_eth_stats_get(port, &st) != 0)
                continue;

            double bw_util =
                ((double)(st.ipackets + st.opackets) / (LINK_CAPACITY_GBPS * 1e6));
            if (bw_util > 1.0) bw_util = 1.0;

            double cpu_util = get_cpu_util();
            double buf_util = get_buffer_util();

            double SI_bw  = compute_SI(bw_util, T_WARN, T_CRIT);
            double SI_cpu = compute_SI(cpu_util, T_WARN, T_CRIT);
            double SI_buf = compute_SI(buf_util, T_WARN, T_CRIT);

            double CS = compute_CS(SI_bw, SI_cpu, SI_buf);
            const char* decision = decide_scale(CS);

            print_port_row(row++, port, bw_util, cpu_util, buf_util,
                           SI_bw, CS, decision);
        }

        refresh();
        usleep(REFRESH_MS * 1000);
    }

    endwin();
    return 0;
}
