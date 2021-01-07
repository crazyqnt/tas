#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <rte_config.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

#include <tas.h>
#include <tas_memif.h>
#include "../../tas/include/config.h"
#include "../../tas/fast/internal.h"
#include "../../tas/fast/fastemu.h"

#include <intrinhelper.h>

#define TEST_IP   0x0a010203
#define TEST_PORT 12345

#define TEST_LIP   0x0a010201
#define TEST_LPORT 23456

#define TEST_DATA_SIZE 128

// Initializations taken from fastpath unit test

#if RTE_VER_YEAR < 19
  typedef struct ether_addr macaddr_t;
#else
  typedef struct rte_ether_addr macaddr_t;
#endif
macaddr_t eth_addr;

void *tas_shm = (void *) 0;

struct flextcp_pl_mem state_base;
struct flextcp_pl_mem *fp_state = &state_base;

struct dataplane_context **ctxs = NULL;
struct configuration config;

__m256i qman_set_vec(__m512i t, __m256i id, __m256i rate, __m256i avail,
    __m256i max_chunk, __m256i flags, __mmask8 k) {
    printf("qman_set_vec\n");
    return _mm256_set1_epi32(0);
}

int qman_set(struct qman_thread *t, uint32_t id, uint32_t rate, uint32_t avail,
    uint16_t max_chunk, uint8_t flags)
{
  printf("qman_set(%u)\n", id);
  return 0;
}

void notify_fastpath_core(unsigned core)
{
  printf("notify_fastpath_core(%u)\n", core);
}

// Fix for for Visual Studio Code to shut up already
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

/* initialize basic flow state */
static void flow_init(uint32_t fid, uint32_t rxlen, uint32_t txlen, uint64_t opaque)
{
  struct flextcp_pl_flowst *fs = &state_base.flowst[fid];
  void *rxbuf = mmap(NULL, rxlen, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  void *txbuf = mmap(NULL, rxlen, PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

  fs->opaque = opaque;
  fs->rx_base_sp = (uintptr_t) rxbuf;
  fs->tx_base = (uintptr_t) txbuf;
  fs->rx_len = rxlen;
  fs->tx_len = txlen;
  fs->local_ip = t_beui32(TEST_LIP);
  fs->remote_ip = t_beui32(TEST_IP);
  fs->local_port = t_beui16(TEST_LPORT);
  fs->remote_port = t_beui16(TEST_PORT);
  fs->rx_avail = rxlen;
  fs->rx_remote_avail = rxlen;
  fs->tx_rate = 10000;
  fs->rtt_est = 18;

  fs->rx_next_seq = 0x0001A000;
  fs->tx_next_seq = 0x000FA000;
}

/* alloc dummy mbuf */
static struct rte_mbuf *mbuf_alloc(void)
{
  struct rte_mbuf *tmb = calloc(1, 2048);
  tmb->data_off = 256;
  tmb->buf_addr = (uint8_t *) (tmb + 1) + tmb->data_off;
  tmb->buf_len = 2048 - sizeof(*tmb);
  return tmb;
}

static struct rte_mbuf *create_payload_package(uint32_t* cur_seq) {
    struct rte_mbuf *tmb = mbuf_alloc();

    int tcp_extra_header_len = 3;
    struct pkt_tcp *p = network_buf_bufoff((struct network_buf_handle*)tmb);
    p->eth.type = __bswap_16(ETH_TYPE_IP);
    p->ip.ttl = 100; // ? less, more?
    p->ip.proto = IP_PROTO_TCP;
    p->ip._v_hl = (4 << 4) | 5;
    p->ip.dest = t_beui32(TEST_LIP);
    p->ip.src = t_beui32(TEST_IP);
    p->ip.len = __bswap_16(sizeof(struct ip_hdr) + sizeof(struct tcp_hdr) + tcp_extra_header_len * 4 + TEST_DATA_SIZE);
    p->tcp.dest = t_beui16(TEST_LPORT);
    p->tcp.src = t_beui16(TEST_PORT);
    p->tcp._hdrlen_rsvd_flags = __bswap_16(
        ((5 + tcp_extra_header_len) << 12) | 0 // other flags here! length: 5 for TCP standard header, 3 for timestamp + end of list indicator
    );
    p->tcp.seqno = __bswap_32(*cur_seq);
    *cur_seq += TEST_DATA_SIZE;

    uint8_t *opt = (uint8_t *) (p + 1); // TCP options
    opt[0] = TCP_OPT_TIMESTAMP; // option type
    opt[1] = sizeof(struct tcp_timestamp_opt); // option length
    // leave it at all zeroes
    int next_opt_start = sizeof(struct tcp_timestamp_opt);
    opt[next_opt_start] = TCP_OPT_END_OF_OPTIONS; // end of list

    tmb->data_len = sizeof(*p) + tcp_extra_header_len * 4 + TEST_DATA_SIZE;

    return tmb;
}

static void init_config() {
    config.shm_len = 0xFFFFFFFFFFFFFFFF; // to prevent asserts, hopefully
}

int run_measurement() {
    struct flextcp_pl_flowst *fs = &state_base.flowst[0];
    struct dataplane_context ctx;
    memset(&ctx, 0, sizeof(ctx));

    flow_init(0, 1024 * 8, 1024 * 8, 123456);

    uint32_t seq = fs->rx_next_seq;
    struct rte_mbuf *tmb = create_payload_package(&seq);

    struct tcp_opts topts;

    fast_flows_packet_parse(&ctx, (struct network_buf_handle*)tmb, (void**)&fs, &topts);

    printf("%lu\n", (uintptr_t)fs);

    fast_flows_packet(&ctx, (struct network_buf_handle*)tmb, (void**)&fs, &topts, 0);

    return 0;
}

int main(int argc, char *argv[]) {
    memset(&state_base, 0, sizeof(state_base));

    return run_measurement();
}