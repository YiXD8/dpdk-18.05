/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2013 Tilera Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Tilera Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>

#include <sys/queue.h>
#include <sys/stat.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>
#include <rte_timer.h>
#include <rte_malloc.h>

#include "testpmd.h"

#define min(X,Y) ((X) < (Y) ? (X) : (Y))
#define max(X,Y) ((X) > (Y) ? (X) : (Y))

#define SERVERNUM 4
static struct   ether_addr eth_addr_array[SERVERNUM];
static uint32_t ip_addr_array[SERVERNUM];

static int  verbose           = 1;
static int  total_flow_num    = 4; // total flows for all servers
static int  current_server_id = 0;

/* Configuration files to be placed in app/test-pmd/config/ */
static const char ethaddr_filename[] = "app/test-pmd/config/eth_addr_info.txt";
static const char ipaddr_filename[]  = "app/test-pmd/config/ip_addr_info.txt";
static const char flow_filename[]    = "app/test-pmd/config/flow_info.txt";

#define DEFAULT_PKT_SIZE 1500
#define L2_LEN sizeof(struct ether_hdr)
#define L3_LEN sizeof(struct ipv4_hdr)
#define L4_LEN sizeof(struct tcp_hdr)
#define HDR_ONLY_SIZE (L2_LEN + L3_LEN + L4_LEN) 

/* Define ip header info */
#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

/* Homa header info */
#define PT_HOMA_GRANT_REQUEST 0x10
#define PT_HOMA_GRANT 0x11
#define PT_HOMA_DATA 0x12

/* Redefine TCP header fields for Homa */
#define PKT_TYPE_8BITS tcp_flags
#define FLOW_ID_16BITS rx_win
// Homa grant request header ONLY
#define FLOW_SIZE_LOW_16BITS tcp_urp
#define FLOW_SIZE_HIGH_16BITS cksum
// Homa grant header ONLY
#define PRIORITY_GRANTED_8BITS data_off 
#define SEQ_GRANTED_LOW_16BITS tcp_urp
#define SEQ_GRANTED_HIGH_16BITS cksum
// Homa data header ONLY
#define DATA_LEN_16BITS tcp_urp

/* Homa states */
#define HOMA_SEND_GRANT_REQUEST_SENT 0x00
#define HOMA_SEND_GRANT_RECEIVING 0x01
#define HOMA_SEND_CLOSED 0x02
#define HOMA_RECEIVE_GRANT_SENDING 0x03
#define HOMA_RECEIVE_CLOSED 0x04

/* Homa transport configuration (parameters and variables) */
#define RTT_BYTES 20000 // Calculated based on BDP
#define MAX_GRANT_TRANSMIT_ONE_TIME 32
#define MAX_REQUEST_RETRANSMIT_ONE_TIME 16
#define RETRANSMIT_TIMEOUT 0.01
#define BURST_THRESHOLD 32

#define UNSCHEDULED_PRIORITY 6 // Unscheduled priority levels
#define SCHEDULED_PRIORITY 2 // Scheduled priority levels
static const int prio_cut_off_bytes[] = 
    {2000, 4000, 6000, 8000, 10000}; // Map message size to divided n+1 unscheduled priorities
static const int prio_map[] = {0, 1, 2, 3, 4, 5, 6, 7}; // 0-n from low to high priority

double start_cycle, elapsed_cycle;
double flowgen_start_time;
double hz;
struct fwd_stream *global_fs;

struct flow_info {
    uint32_t dst_ip;
    uint32_t src_ip;
    uint16_t dst_port;
    uint16_t src_port;

    uint8_t  flow_state;
    uint32_t flow_size; /* flow total size */
    uint32_t remain_size; /* flow remain size */
    double   start_time;
    double   finish_time;
    int      fct_printed;
    int      flow_finished;

    uint32_t data_seqnum;
    uint32_t data_recv_next; 
    uint32_t granted_seqnum;
    uint32_t granted_priority;
    double   last_grant_request_sent_time;
};
struct flow_info *sender_flows;
struct flow_info *receiver_flows;

struct rte_mbuf *sender_pkts_burst[MAX_PKT_BURST];
struct rte_mbuf *receiver_pkts_burst[MAX_PKT_BURST];
int sender_current_burst_size;
int receiver_current_burst_size;

/* sender task states */
int sender_total_flow_num              = 0; // sender flows for this server
int sender_grant_request_sent_flow_num = 0;
int sender_active_flow_num             = 0;
int sender_finished_flow_num           = 0;
int sender_next_unstart_flow_id        = -1;
int sender_current_burst_size          = 0;

#define MAX_CONCURRENT_FLOW 100
int sender_request_sent_flow_array[MAX_CONCURRENT_FLOW];
int sender_active_flow_array[MAX_CONCURRENT_FLOW];

/* receiver task states */
int receiver_total_flow_num     = 0; // receiver flows for this server
int receiver_active_flow_num    = 0;
int receiver_finished_flow_num  = 0;
int receiver_current_burst_size = 0;

int receiver_active_flow_array[MAX_CONCURRENT_FLOW];

/* declaration of functions */
static void
main_flowgen(struct fwd_stream *fs);

static int
start_new_flow(void);

static void
sender_send_pkt(void);

static void
receiver_send_pkt(void);

static void
recv_pkt(struct fwd_stream *fs);

static void
recv_data(struct tcp_hdr *transport_recv_hdr);

static void
recv_grant(struct tcp_hdr *transport_recv_hdr);

static void
process_ack(struct tcp_hdr* transport_recv_hdr);

static void
construct_grant_request(uint32_t flow_id);

static void
construct_grant(uint32_t flow_id, uint32_t seq_granted, uint8_t priority_granted);

static void
construct_data(uint32_t flow_id, uint32_t ack_seq);

static void
init(void);

static void
read_config(void);

static inline uint16_t
ip_sum(const unaligned_uint16_t *hdr, int hdr_len);

static inline void
print_ether_addr(const char *what, struct ether_addr *eth_addr);

static inline void
add_sender_grant_request_sent_flow(int flow_id);

static inline void
add_receiver_active_flow(int flow_id);

static inline int
find_next_sender_grant_request_sent_flow(int start_index);

static inline void
remove_sender_grant_request_sent_flow(int flow_id);

static inline void
remove_receiver_active_flow(int flow_id);

static inline void
sort_receiver_active_flow_by_remaining_size(void);

static inline int
find_next_unstart_flow_id(void);

static inline void 
remove_newline(char *str);

static inline uint32_t
map_to_unscheduled_priority(int flow_size);

static inline int
get_src_server_id(uint32_t flow_id, struct flow_info *flows);

static inline int
get_dst_server_id(uint32_t flow_id, struct flow_info *flows);

static void
print_fct(void);


static inline uint16_t
ip_sum(const unaligned_uint16_t *hdr, int hdr_len)
{
    uint32_t sum = 0;

    while (hdr_len > 1)
    {
        sum += *hdr++;
        if (sum & 0x80000000)
            sum = (sum & 0xFFFF) + (sum >> 16);
        hdr_len -= 2;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

static void 
remove_newline(char *str)
{
    for (uint32_t i = 0; i < strlen(str); i++) {
        if (str[i] == '\r' || str[i] == '\n')
            str[i] = '\0';
    }
}

static inline void
print_ether_addr(const char *what, struct ether_addr *eth_addr)
{
    char buf[ETHER_ADDR_FMT_SIZE];
    ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
    printf("%s%s", what, buf);
}

/* Map flow size to unscheduled priority */
static inline uint32_t
map_to_unscheduled_priority(int flow_size)
{
    for (int i=0; i<UNSCHEDULED_PRIORITY-1; i++) {
        if (flow_size<prio_cut_off_bytes[i])
            return i;
    }
    return (UNSCHEDULED_PRIORITY-1);
}


/* Map flow src ip to server id */
static inline int
get_src_server_id(uint32_t flow_id, struct flow_info *flows) 
{
    for (int server_index =0; server_index < SERVERNUM; server_index++) {
        if (flows[flow_id].src_ip == ip_addr_array[server_index]) {
            return server_index; 
        }
    }
    return -1;
}

/* Map flow dst ip to server id */
static inline int
get_dst_server_id(uint32_t flow_id, struct flow_info *flows) 
{
    for (int server_index =0; server_index < SERVERNUM; server_index++) {
        if (flows[flow_id].dst_ip == ip_addr_array[server_index]) {
            return server_index; 
        }
    }
    return -1;
}

static inline void
add_sender_grant_request_sent_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] < 0) {
            sender_request_sent_flow_array[i] = flow_id;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: run out of memory for add_sender_grant_request_sent_flow\n");
    }
    sender_grant_request_sent_flow_num++;
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    sender_flows[flow_id].flow_state = HOMA_SEND_GRANT_REQUEST_SENT; 
    sender_flows[flow_id].last_grant_request_sent_time = rte_rdtsc() / (double)hz;
}

static inline void
add_receiver_active_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (receiver_active_flow_array[i] < 0) {
            receiver_active_flow_array[i] = flow_id;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: run out of memory for add_receiver_active_flow\n");
    }
    receiver_active_flow_num++;
    receiver_total_flow_num++;
    receiver_flows[flow_id].flow_state = HOMA_RECEIVE_GRANT_SENDING;
}

static inline int
find_next_sender_grant_request_sent_flow(int start_index)
{
    for (int i=start_index+1; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] != -1) {
            return i;
        }
    }
    return -1;
}

static inline void
remove_receiver_active_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (receiver_active_flow_array[i] == flow_id) {
            receiver_active_flow_array[i] = -1;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: cannot find the node for remove_receiver_active_flow\n");
    }
    receiver_active_flow_num--;
    receiver_flows[flow_id].flow_state = HOMA_RECEIVE_CLOSED;
    receiver_flows[flow_id].flow_finished = 1;
    receiver_flows[flow_id].finish_time = rte_rdtsc() / (double)hz;
    receiver_finished_flow_num++;
}

static inline void
remove_sender_grant_request_sent_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] == flow_id) {
            sender_request_sent_flow_array[i] = -1;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: cannot find the node for remove_sender_grant_request_sent_flow\n");
    }
    sender_grant_request_sent_flow_num--;
    sender_flows[flow_id].flow_state = HOMA_SEND_GRANT_RECEIVING; 
}

static inline void
sort_receiver_active_flow_by_remaining_size(void)
{
    /* Selection sorting */
    int temp_id;
    for (int i=0; i<MAX_CONCURRENT_FLOW; i++) {
        for (int j=i+1; j<MAX_CONCURRENT_FLOW; j++) {
            if (receiver_active_flow_array[j] >= 0) {
                if (receiver_active_flow_array[i] >= 0) {
                    if (receiver_flows[receiver_active_flow_array[i]].remain_size >
                        receiver_flows[receiver_active_flow_array[j]].remain_size) {
                            temp_id = receiver_active_flow_array[i];
                            receiver_active_flow_array[i] = receiver_active_flow_array[j];
                            receiver_active_flow_array[j] = temp_id;
                        }
                }
                else {
                    receiver_active_flow_array[i] = receiver_active_flow_array[j];
                    receiver_active_flow_array[j] = -1;
                }
            }
        }
    }
}

static inline int
find_next_unstart_flow_id(void)
{
    int i;
    for (i=sender_next_unstart_flow_id+1; i<sender_total_flow_num; i++) {
        if (get_src_server_id(i, sender_flows) == current_server_id)
            return i;
    }
    return i;
}

static void
print_fct(void)
{
    for (int i=0; i<total_flow_num; i++) {
        if (receiver_flows[i].fct_printed == 0 && receiver_flows[i].flow_finished == 1) {
             printf("%d %lf\n", i, 
                receiver_flows[i].finish_time - receiver_flows[i].start_time);
             receiver_flows[i].fct_printed = 1;
         }
    }
}

/* Read basic info of server id, mac and ip */
static void
read_config(void)
{
    FILE *fd = NULL;
    char line[256] = {0};
    int  server_id;
    uint32_t src_ip_segment1 = 0, src_ip_segment2 = 0, src_ip_segment3 = 0, src_ip_segment4 = 0;

    /* Read ethernet address info */
    server_id = 0;
    fd = fopen(ethaddr_filename, "r");
    if (!fd)
        printf("%s: no such file\n", ethaddr_filename);
    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%hhu %hhu %hhu %hhu %hhu %hhu", &eth_addr_array[server_id].addr_bytes[0], 
            &eth_addr_array[server_id].addr_bytes[1], &eth_addr_array[server_id].addr_bytes[2],
            &eth_addr_array[server_id].addr_bytes[3], &eth_addr_array[server_id].addr_bytes[4], 
            &eth_addr_array[server_id].addr_bytes[5]);
        if (verbose > 0) {
           printf("\nServer id = %d   ", server_id);
           print_ether_addr("eth=", &eth_addr_array[server_id]);
        }
        server_id++;
    }
    fclose(fd);

    /* Read ip address info */
    server_id = 0;
    fd = fopen(ipaddr_filename, "r");
    if (!fd)
        printf("%s: no such file\n", ipaddr_filename);
    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%u %u %u %u", &src_ip_segment1, &src_ip_segment2, &src_ip_segment3, &src_ip_segment4);
        ip_addr_array[server_id] = IPv4(src_ip_segment1, src_ip_segment2, src_ip_segment3, src_ip_segment4);
        if (verbose > 0) {
            printf("\nServer id = %d   ", server_id);
            printf("ip=%u.%u.%u.%u (%u)", src_ip_segment1, src_ip_segment2, 
                src_ip_segment3, src_ip_segment4, ip_addr_array[server_id]);
        }
        server_id++;
    }
    fclose(fd);

    printf("\nEnd of read_config\n\n");
}

/* Init sender flow info */
static void
init(void)
{
    char     line[256] = {0};
    uint32_t flow_id = 0;
    uint32_t src_ip_segment1 = 0, src_ip_segment2 = 0, src_ip_segment3 = 0, src_ip_segment4 = 0;
    uint32_t dst_ip_segment1 = 0, dst_ip_segment2 = 0, dst_ip_segment3 = 0, dst_ip_segment4 = 0;
    uint16_t udp_src_port = 0, udp_dst_port = 0;
    uint32_t flow_size = 0;
    double   start_time;
    
    FILE *fd = fopen(flow_filename, "r");
    if (!fd)
        printf("%s: no such file\n", flow_filename);

    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%u %u %u %u %u %u %u %u %u %hu %hu %u %lf", &flow_id, 
            &src_ip_segment1, &src_ip_segment2, &src_ip_segment3, &src_ip_segment4, 
            &dst_ip_segment1, &dst_ip_segment2, &dst_ip_segment3, &dst_ip_segment4, 
            &udp_src_port, &udp_dst_port, &flow_size, &start_time);
        sender_flows[flow_id].src_ip      = IPv4(src_ip_segment1, src_ip_segment2, src_ip_segment3, src_ip_segment4);
        sender_flows[flow_id].dst_ip      = IPv4(dst_ip_segment1, dst_ip_segment2, dst_ip_segment3, dst_ip_segment4);
        sender_flows[flow_id].src_port    = udp_src_port;
        sender_flows[flow_id].dst_port    = udp_dst_port;
        sender_flows[flow_id].flow_size   = flow_size;
        sender_flows[flow_id].remain_size = flow_size;
        sender_flows[flow_id].data_seqnum = 1;
        sender_flows[flow_id].start_time  = start_time;

        if (get_src_server_id(flow_id, sender_flows) == current_server_id)
            sender_total_flow_num++;
        
        if (verbose > 1) {
            printf("Flow info: flow_id=%u, src_ip=%u, dst_ip=%u, "
                "src_port=%hu, dst_port=%hu, flow_size=%u, start_time=%lf\n",  
                flow_id, sender_flows[flow_id].src_ip, sender_flows[flow_id].dst_ip, 
                sender_flows[flow_id].src_port, sender_flows[flow_id].dst_port, 
                sender_flows[flow_id].flow_size, sender_flows[flow_id].start_time); 
        }
    }
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    fclose(fd);

    /*sender_request_sent_flow_array = rte_zmalloc("testpmd: int", 
        MAX_CONCURRENT_FLOW*sizeof(int), RTE_CACHE_LINE_SIZE);
    sender_active_flow_array = rte_zmalloc("testpmd: int", 
        MAX_CONCURRENT_FLOW*sizeof(int), RTE_CACHE_LINE_SIZE);
    receiver_active_flow_array = rte_zmalloc("testpmd: int", 
        MAX_CONCURRENT_FLOW*sizeof(int), RTE_CACHE_LINE_SIZE);*/
    for (int i=0; i<MAX_CONCURRENT_FLOW; i++) {
        sender_request_sent_flow_array[i] = -1;
        sender_active_flow_array[i] = -1;
        receiver_active_flow_array[i] = -1;
    }

    if (verbose > 0) {
        printf("Flow info summary: total_flow_num = %d, sender_total_flow_num = %d\n",
            total_flow_num, sender_total_flow_num);
    }

    printf("End of init\n\n");
}

static void
construct_grant_request(uint32_t flow_id)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;
    
    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, sender_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[current_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = 0; // highest priority for grant requests
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(sender_flows[flow_id].src_ip);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(sender_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port              = rte_cpu_to_be_16(sender_flows[flow_id].src_port);
    transport_hdr->dst_port              = rte_cpu_to_be_16(sender_flows[flow_id].dst_port);
    transport_hdr->sent_seq              = rte_cpu_to_be_32(sender_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack              = 0;
    transport_hdr->PKT_TYPE_8BITS        = PT_HOMA_GRANT_REQUEST;
    transport_hdr->FLOW_ID_16BITS        = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    transport_hdr->FLOW_SIZE_LOW_16BITS  = rte_cpu_to_be_16((uint16_t)(sender_flows[flow_id].flow_size & 0xffff));
    transport_hdr->FLOW_SIZE_HIGH_16BITS = (uint16_t)((sender_flows[flow_id].flow_size >> 16) & 0xffff);
    
    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;
    sender_pkts_burst[sender_current_burst_size] = pkt;
    sender_current_burst_size++;
    if (sender_current_burst_size >= BURST_THRESHOLD) {
        sender_send_pkt();
        sender_current_burst_size = 0;
    }
}

static void
construct_grant(uint32_t flow_id, uint32_t seq_granted, uint8_t priority_granted)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, receiver_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[current_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = 0; // highest priority for grants
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].src_ip);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port                = rte_cpu_to_be_16(receiver_flows[flow_id].src_port);
    transport_hdr->dst_port                = rte_cpu_to_be_16(receiver_flows[flow_id].dst_port);
    transport_hdr->sent_seq                = rte_cpu_to_be_32(receiver_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack                = 0;
    transport_hdr->PKT_TYPE_8BITS          = PT_HOMA_GRANT;
    transport_hdr->FLOW_ID_16BITS          = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    transport_hdr->PRIORITY_GRANTED_8BITS  = priority_granted;
    transport_hdr->SEQ_GRANTED_LOW_16BITS  = rte_cpu_to_be_16((uint16_t)(seq_granted & 0xffff));
    transport_hdr->SEQ_GRANTED_HIGH_16BITS = (uint16_t)((seq_granted >> 16) & 0xffff);

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;
    receiver_pkts_burst[receiver_current_burst_size] = pkt;
    receiver_current_burst_size++;
    if (receiver_current_burst_size >= BURST_THRESHOLD) {
        receiver_send_pkt();
        receiver_current_burst_size = 0;
    }
}

static void
construct_data(uint32_t flow_id, uint32_t ack_seq)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    uint16_t data_len;
    int      dst_server_id;
    unsigned pkt_size = DEFAULT_PKT_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, sender_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[current_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = sender_flows[flow_id].granted_priority;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(sender_flows[flow_id].src_ip);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(sender_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port       = rte_cpu_to_be_16(sender_flows[flow_id].src_port);
    transport_hdr->dst_port       = rte_cpu_to_be_16(sender_flows[flow_id].dst_port);
    transport_hdr->sent_seq       = rte_cpu_to_be_32(sender_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack       = ack_seq;
    transport_hdr->PKT_TYPE_8BITS = PT_HOMA_DATA;
    transport_hdr->FLOW_ID_16BITS = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    data_len = (pkt_size - HDR_ONLY_SIZE);
    if (data_len > sender_flows[flow_id].remain_size) {
        data_len = sender_flows[flow_id].remain_size;
        pkt_size = HDR_ONLY_SIZE + data_len;
    }
    transport_hdr->DATA_LEN_16BITS = RTE_CPU_TO_BE_16(data_len);
    sender_flows[flow_id].data_seqnum += data_len; 
    sender_flows[flow_id].remain_size -= data_len;

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;
    sender_pkts_burst[sender_current_burst_size] = pkt;
    sender_current_burst_size++;
    if (sender_current_burst_size >= BURST_THRESHOLD) {
        sender_send_pkt();
        sender_current_burst_size = 0;
    }
}

static void
process_ack(struct tcp_hdr* transport_recv_hdr)
{
    int datalen = rte_be_to_cpu_16(transport_recv_hdr->DATA_LEN_16BITS);
    uint16_t flow_id = (uint16_t)rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);

    if (rte_be_to_cpu_32(transport_recv_hdr->sent_seq) != receiver_flows[flow_id].data_recv_next) {
        printf("[%d] : data loss detected. (expected = %u, received = %u)\n", flow_id, 
            receiver_flows[flow_id].data_recv_next, rte_be_to_cpu_32(transport_recv_hdr->sent_seq));
    }
    receiver_flows[flow_id].data_recv_next += datalen;
    receiver_flows[flow_id].remain_size -= datalen;
    if (receiver_flows[flow_id].remain_size <= 0) {
        remove_receiver_active_flow(flow_id);
    }
}

static int
recv_grant_request(struct tcp_hdr *transport_recv_hdr, struct ipv4_hdr *ipv4_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint32_t flow_size_lowpart = (uint32_t)rte_be_to_cpu_16(transport_recv_hdr->FLOW_SIZE_LOW_16BITS);
    uint32_t flow_size_highpart = ((uint32_t)transport_recv_hdr->FLOW_SIZE_HIGH_16BITS << 16) & 0xffff0000;
    uint32_t flow_size = flow_size_highpart + flow_size_lowpart;

    if (receiver_flows[flow_id].flow_state == HOMA_RECEIVE_GRANT_SENDING)
        return 0; 
    
    if (verbose > 0) {
        printf("receive grant request of flow %u\n", flow_id);
    }

    add_receiver_active_flow(flow_id);

    receiver_flows[flow_id].flow_size      = flow_size;
    receiver_flows[flow_id].remain_size    = flow_size;
    receiver_flows[flow_id].src_port       = RTE_BE_TO_CPU_16(transport_recv_hdr->dst_port);
    receiver_flows[flow_id].dst_port       = RTE_BE_TO_CPU_16(transport_recv_hdr->src_port);
    receiver_flows[flow_id].src_ip         = rte_be_to_cpu_32(ipv4_hdr->dst_addr);
    receiver_flows[flow_id].dst_ip         = rte_be_to_cpu_32(ipv4_hdr->src_addr);
    receiver_flows[flow_id].start_time     = rte_rdtsc() / (double)hz;
    receiver_flows[flow_id].fct_printed    = 0;
    receiver_flows[flow_id].flow_finished  = 0;
    receiver_flows[flow_id].data_recv_next = 1;
    receiver_flows[flow_id].data_seqnum    = 1;
    
    return 1;
}

static void
recv_grant(struct tcp_hdr *transport_recv_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint32_t seq_granted_lowpart = (uint32_t)rte_be_to_cpu_16(transport_recv_hdr->SEQ_GRANTED_LOW_16BITS);
    uint32_t seq_granted_highpart = ((uint32_t)transport_recv_hdr->SEQ_GRANTED_HIGH_16BITS << 16) & 0xffff0000;
    uint32_t seq_granted = seq_granted_highpart + seq_granted_lowpart;
    struct   rte_mbuf *queued_pkt;
    struct   tcp_hdr *transport_hdr;
    int      queued_pkt_type, queued_flow_id;

    switch (sender_flows[flow_id].flow_state) {
        case HOMA_SEND_GRANT_REQUEST_SENT:
            remove_sender_grant_request_sent_flow(flow_id);
        case HOMA_SEND_GRANT_RECEIVING:
            sender_flows[flow_id].granted_seqnum = seq_granted;
            sender_flows[flow_id].granted_priority = transport_recv_hdr->PRIORITY_GRANTED_8BITS;
            /* construct new data according to SRPT */
            queued_pkt = sender_pkts_burst[sender_current_burst_size-1];
            transport_hdr = rte_pktmbuf_mtod_offset(queued_pkt, struct tcp_hdr *, L2_LEN + L3_LEN);
            queued_pkt_type = transport_hdr->PKT_TYPE_8BITS;
            queued_flow_id = transport_hdr->FLOW_ID_16BITS;
            if (queued_pkt_type == PT_HOMA_DATA && 
                sender_flows[queued_flow_id].remain_size > sender_flows[flow_id].remain_size) {
                while (sender_flows[flow_id].remain_size > 0 &&
                    sender_flows[flow_id].granted_seqnum > sender_flows[flow_id].data_seqnum) {
                    construct_data(flow_id, transport_recv_hdr->sent_seq);
                }
            }
            if (sender_flows[flow_id].remain_size == 0) {
                sender_finished_flow_num++;
                sender_flows[flow_id].flow_state = HOMA_SEND_CLOSED;
            }
            break;
        default:
            break;
    }
}

static void
recv_data(struct tcp_hdr *transport_recv_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);

    // drop all data packets if received before the grant requests
    if (receiver_flows[flow_id].flow_state != HOMA_RECEIVE_GRANT_SENDING) {
        return;
    }

    process_ack(transport_recv_hdr);
}

/* Receive and process a burst of packets. */
static void
recv_pkt(struct fwd_stream *fs)
{
    struct   rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct   rte_mbuf *mb;
    uint16_t nb_rx;
    struct   ipv4_hdr *ipv4_hdr;
    struct   tcp_hdr *transport_recv_hdr;
    uint8_t  l4_proto;
    uint8_t  pkt_type;

    /* Receive a burst of packets. */
    nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, pkts_burst, nb_pkt_per_burst);

    if (unlikely(nb_rx == 0))
        return;

#ifdef RTE_TEST_PMD_RECORD_BURST_STATS
    fs->rx_burst_stats.pkt_burst_spread[nb_rx]++;
#endif
    fs->rx_packets += nb_rx;

    /* Process a burst of packets. */
    for (int i = 0; i < nb_rx; i++) {
        mb = pkts_burst[i]; 
        ipv4_hdr = rte_pktmbuf_mtod_offset(mb, struct ipv4_hdr *, L2_LEN);
        l4_proto = ipv4_hdr->next_proto_id;
        if (l4_proto == IPPROTO_TCP) {
            transport_recv_hdr = rte_pktmbuf_mtod_offset(mb, struct tcp_hdr *, L2_LEN + L3_LEN);
            pkt_type = transport_recv_hdr->PKT_TYPE_8BITS;
            switch (pkt_type) {
                case PT_HOMA_GRANT_REQUEST:
                    recv_grant_request(transport_recv_hdr, ipv4_hdr);
                    break;
                case PT_HOMA_GRANT:
                    recv_grant(transport_recv_hdr);
                    break;
                case PT_HOMA_DATA:
                    recv_data(transport_recv_hdr);
                    break;
                default:
                    break;
            }
        }
        rte_pktmbuf_free(mb);
    }
}

static void
send_grant(void)
{
    if (receiver_current_burst_size > 0) {
        receiver_send_pkt();
    }

    /* generate new grants for SCHEDULED_PRIORITY messages */
    sort_receiver_active_flow_by_remaining_size();
    for (int i=0; i<SCHEDULED_PRIORITY; i++) {
        if (receiver_active_flow_array[i] >= 0) {
            int flow_id = receiver_active_flow_array[i];
            construct_grant(flow_id, receiver_flows[flow_id].data_recv_next+RTT_BYTES, prio_map[i]);
        }
        else
            break;
    }
}

/* Sender send a burst of packets */
static void
sender_send_pkt(void)
{
    uint16_t nb_pkt = sender_current_burst_size;
    uint16_t nb_tx = rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, sender_pkts_burst, nb_pkt);

    if (unlikely(nb_tx < nb_pkt) && global_fs->retry_enabled) {
        uint32_t retry = 0;
        while (nb_tx < nb_pkt && retry++ < burst_tx_retry_num) {
            rte_delay_us(burst_tx_delay_time);
            nb_tx += rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, 
                                        &sender_pkts_burst[nb_tx], nb_pkt - nb_tx);
        }
    }

    global_fs->tx_packets += nb_tx;
    sender_current_burst_size = 0;
}

/* Receiver send a burst of packets */
static void
receiver_send_pkt(void)
{
    uint16_t nb_pkt = receiver_current_burst_size;
    uint16_t nb_tx = rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, receiver_pkts_burst, nb_pkt);

    if (unlikely(nb_tx < nb_pkt) && global_fs->retry_enabled) {
        uint32_t retry = 0;
        while (nb_tx < nb_pkt && retry++ < burst_tx_retry_num) {
            rte_delay_us(burst_tx_delay_time);
            nb_tx += rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, 
                                        &receiver_pkts_burst[nb_tx], nb_pkt - nb_tx);
        }
    }

    global_fs->tx_packets += nb_tx;
    receiver_current_burst_size = 0;
}

static int
start_new_flow(void)
{
    double now;
    int flow_id, request_retrans_check, retransmit_flow_index, max_request_retrans_check;

    if (sender_current_burst_size > 0) {
        sender_send_pkt();
    }

    /* retransmit timeout grant request */
    max_request_retrans_check = min(MAX_REQUEST_RETRANSMIT_ONE_TIME, sender_grant_request_sent_flow_num);
    if (sender_grant_request_sent_flow_num > 0) {
        retransmit_flow_index = 0;
        request_retrans_check = 0;
        while (request_retrans_check < max_request_retrans_check) {
            now =  rte_rdtsc() / (double)hz; 
            retransmit_flow_index = find_next_sender_grant_request_sent_flow(retransmit_flow_index);
            flow_id = sender_request_sent_flow_array[retransmit_flow_index];
            if ((now - sender_flows[flow_id].last_grant_request_sent_time) > RETRANSMIT_TIMEOUT) {
                construct_grant_request(flow_id);
                sender_flows[flow_id].last_grant_request_sent_time = now;
            }
            request_retrans_check++;
        }
    }

    /* start new flow */
    if (sender_next_unstart_flow_id < sender_total_flow_num) {
        flow_id = sender_next_unstart_flow_id;
        now = rte_rdtsc() / (double)hz;
        while ((sender_flows[flow_id].start_time + flowgen_start_time) <= now) {
            if (verbose > 0) {
                printf("start_new_flow %d at %lf\n", flow_id, now);
            }

            construct_grant_request(flow_id);
            add_sender_grant_request_sent_flow(flow_id);

            /* send RTT_BYTES unscheduled data */
            int max_unscheduled_pkt = RTT_BYTES / DEFAULT_PKT_SIZE;
            sender_flows[flow_id].granted_priority = map_to_unscheduled_priority(sender_flows[flow_id].flow_size);
            for (int i=0; i<max_unscheduled_pkt; i++) {
                construct_data(flow_id, 0);
                if (sender_flows[flow_id].remain_size == 0)
                    break;
            }

            if (sender_next_unstart_flow_id < sender_total_flow_num) {
                flow_id = sender_next_unstart_flow_id;
            } else {
                break;
            }
        }
    }

    return 0;
}

/* flowgen packet_fwd main function */
static void
main_flowgen(struct fwd_stream *fs)
{
    global_fs = fs;
    hz = rte_get_timer_hz(); 

    read_config(); /* read basic info of server number, mac and ip */

    sender_flows = rte_zmalloc("testpmd: struct flow_info",
            total_flow_num*sizeof(struct flow_info), RTE_CACHE_LINE_SIZE);
    receiver_flows = rte_zmalloc("testpmd: struct flow_info",
            total_flow_num*sizeof(struct flow_info), RTE_CACHE_LINE_SIZE);
    
    rte_delay_ms(2000);
    init(); /* init sender flow info */
    rte_delay_ms(2000);

    start_cycle = rte_rdtsc();
    elapsed_cycle = 0;
    flowgen_start_time = start_cycle / (double)hz;
    int main_flowgen_loop = 1;
    do {
        if (verbose > 1) {
            printf("main_flowgen_loop=%d at elapsed_time=%lf\n", main_flowgen_loop++, elapsed_cycle/(double)hz);
            printf("Enter start_new_flow...\n");
        }

        start_new_flow();

        if (verbose > 1) {
            printf("Exit start_new_flow...\n");
            printf("Enter recv_pkt...\n");
        }

        recv_pkt(fs);

        if (verbose > 1) {
            printf("Exit recv_pkt...\n");
            printf("Enter send_grant...\n");
        }

        send_grant();

        if (verbose > 1) {
            printf("Exit send_grant...\n\n");
        }

        elapsed_cycle = rte_rdtsc() - start_cycle; 
        if (elapsed_cycle > 40*hz)
            break;
    } while ( (elapsed_cycle < 40*hz) | 
        (receiver_finished_flow_num < receiver_total_flow_num) | 
        (sender_finished_flow_num < sender_total_flow_num) );

    print_fct();
}

struct fwd_engine flow_gen_engine = {
    .fwd_mode_name  = "flowgen",
    .port_fwd_begin = NULL,
    .port_fwd_end   = NULL,
    .packet_fwd     = main_flowgen,
};