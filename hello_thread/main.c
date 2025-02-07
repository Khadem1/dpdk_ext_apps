#include <stdint.h>
#include <inttypes.h>
#include <signal.h>

#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_lcore.h>

volatile bool run = true;

int helloWaitTest(void *arg)
{
        uint16_t val = *(uint16_t *)arg;

        while(run)
        {
                printf(" val %u dpdk logical core %u logical core index %u\n", val, rte_lcore_id(), rte_lcore_index(rte_lcore_id()));
                rte_delay_ms(1000 + 1000 * val);
        }

        return 0;
}

static void
signal_handler(int signum)
{
        if (signum == SIGINT) {
                run = false;
        }
}


int main(int argc, char *argv[])
{
        signal(SIGINT, signal_handler);

        /* Initialize the Environment Abstraction Layer (EAL). */
        int ret = rte_eal_init(argc, argv);
        if (ret < 0)
                rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

        argc -= ret;
        argv += ret;

        uint16_t lcore_id = 0;
        RTE_LCORE_FOREACH_WORKER(lcore_id)
        {
                uint16_t temp = lcore_id;
                rte_eal_remote_launch(helloWaitTest, &temp, temp);
        }
        uint16_t temp = rte_lcore_id();
        helloWaitTest(&temp);

        rte_eal_mp_wait_lcore();
        /* clean up the EAL */
        rte_eal_cleanup();

        return 0;
}

