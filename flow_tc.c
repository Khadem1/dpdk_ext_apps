#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_mempool.h>

#define NB_MBUF 8192
#define NUM_DESC 1024

volatile bool keep_running = true;

static struct rte_mempool *mbuf_pool;

static void handle_signal(int sig) {
    keep_running = false;
}

static const struct rte_eth_conf port_conf_default = {
    .rxmode = { .mq_mode = RTE_ETH_MQ_RX_NONE },
    .txmode = { .mq_mode = RTE_ETH_MQ_TX_NONE },
};

int main(int argc, char **argv) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL init failed\n");

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No ports found\n");

    uint16_t port_id = 0;

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NB_MBUF, 0, 0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE,
                                        rte_socket_id());
    if (!mbuf_pool)
        rte_exit(EXIT_FAILURE, "Failed to create mempool\n");

    ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf_default);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot configure device\n");

    ret = rte_eth_rx_queue_setup(port_id, 0, NUM_DESC, rte_eth_dev_socket_id(port_id), NULL, mbuf_pool);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "RX queue setup failed\n");

    ret = rte_eth_tx_queue_setup(port_id, 0, NUM_DESC, rte_eth_dev_socket_id(port_id), NULL);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "TX queue setup failed\n");

    ret = rte_eth_dev_start(port_id);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to start port\n");

    rte_eth_promiscuous_enable(port_id);

    printf("Initialized port %u\n", port_id);

    // Create 1000 rules
    struct rte_flow *flows[100000];
    struct rte_flow_error error;

    for (uint16_t i = 0; i < 100000; i++) {
        struct rte_flow_attr attr = {
            .ingress = 1,
        };

        struct rte_flow_item_eth eth_spec = {0};
        struct rte_flow_item_eth eth_mask = {0};

        struct rte_flow_item_ipv4 ip_spec = {0};
        struct rte_flow_item_ipv4 ip_mask = {0};
        ip_spec.hdr.next_proto_id = IPPROTO_UDP;
        ip_mask.hdr.next_proto_id = 0xFF;

        struct rte_flow_item_udp udp_spec = {0};
        struct rte_flow_item_udp udp_mask = {0};
        udp_spec.hdr.dst_port = rte_cpu_to_be_16(10000 + i);
        udp_mask.hdr.dst_port = 0xFFFF;

        const struct rte_flow_item pattern[] = {
            { .type = RTE_FLOW_ITEM_TYPE_ETH,
              .spec = &eth_spec,
              .mask = &eth_mask },
            { .type = RTE_FLOW_ITEM_TYPE_IPV4,
              .spec = &ip_spec,
              .mask = &ip_mask },
            { .type = RTE_FLOW_ITEM_TYPE_UDP,
              .spec = &udp_spec,
              .mask = &udp_mask },
            { .type = RTE_FLOW_ITEM_TYPE_END },
        };

        struct rte_flow_action_queue queue = { .index = 0 };
        const struct rte_flow_action actions[] = {
            { .type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &queue },
            { .type = RTE_FLOW_ACTION_TYPE_END },
        };

        flows[i] = rte_flow_create(port_id, &attr, pattern, actions, &error);
        if (!flows[i]) {
            printf("Rule %u creation failed: %s\n", i, error.message ? error.message : "(no message)");
            break;
        }

        if ((i+1)%100 == 0)
            printf("Created %u rules...\n", i+1);
    }

    printf("Created flow rules. Press Ctrl+C to exit.\n");

    while (keep_running)
        sleep(1);

    printf("Cleaning up flow rules...\n");

    for (uint16_t i = 0; i < 100000; i++) {
        if (flows[i])
            rte_flow_destroy(port_id, flows[i], &error);
    }

    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);

    printf("Done.\n");
    return 0;
}
