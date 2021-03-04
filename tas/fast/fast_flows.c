/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <intrinhelper.h>

#include <assert.h>
#include <rte_config.h>
#include <rte_ip.h>
#include <rte_hash_crc.h>

#include <tas_memif.h>
#include <utils_sync.h>

#include "internal.h"
#include "fastemu.h"
#include "tcp_common.h"

#define TCP_MSS 1448
#define TCP_MAX_RTT 100000

//#define SKIP_ACK 1

struct flow_key {
  ip_addr_t local_ip;
  ip_addr_t remote_ip;
  beui16_t local_port;
  beui16_t remote_port;
} __attribute__((packed));

#if 1
#define fs_lock(fs) util_spin_lock(&fs->lock)
#define fs_unlock(fs) util_spin_unlock(&fs->lock)
#else
#define fs_lock(fs) do {} while (0)
#define fs_unlock(fs) do {} while (0)
#endif

#ifdef ASTVEC_CURRENTLY_VECTORIZING
static void flow_tx_segment_vec(
    __m512i vctx, __m512i vnbh, __m512i vfs, __m256i vseq, __m256i vack, __m256i vrxwnd, __m256i vpayload,
    __m256i vpayload_pos, __m256i vts_echo, __m256i vts_my, __m256i vfin, __mmask8 k
) {}
#endif

#pragma vectorize
static void flow_tx_read(struct flextcp_pl_flowst *fs, uint32_t pos,
    uint16_t len, void *dst);
#pragma vectorize transposed(fs)
static void flow_rx_write(struct flextcp_pl_flowst *fs, uint32_t pos,
    uint16_t len, const void *src);
#ifdef FLEXNIC_PL_OOO_RECV
#pragma vectorize transposed(fs)
static void flow_rx_seq_write(struct flextcp_pl_flowst *fs, uint32_t seq,
    uint16_t len, const void *src);
#endif
static void flow_tx_segment(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, struct flextcp_pl_flowst *fs,
    uint32_t seq, uint32_t ack, uint32_t rxwnd, uint16_t payload,
    uint32_t payload_pos, uint32_t ts_echo, uint32_t ts_my, uint8_t fin);
static void flow_tx_segment_vec(
    __m512i vctx, __m512i vnbh, __m512i vfs, __m256i vseq, __m256i vack, __m256i vrxwnd, __m256i vpayload,
    __m256i vpayload_pos, __m256i vts_echo, __m256i vts_my, __m256i vfin, __mmask8 k
);
#pragma vectorize
static void flow_tx_ack(struct dataplane_context *ctx, uint32_t seq,
    uint32_t ack, uint32_t rxwnd, uint32_t echo_ts, uint32_t my_ts,
    struct network_buf_handle *nbh, struct tcp_timestamp_opt *ts_opt);
#pragma vectorize transposed(fs)
static void flow_reset_retransmit(struct flextcp_pl_flowst *fs);

#pragma vectorize
static inline void tcp_checksums(struct network_buf_handle *nbh,
    struct pkt_tcp *p, beui32_t ip_s, beui32_t ip_d, uint16_t l3_paylen);


void fast_flows_qman_pf(struct dataplane_context *ctx, uint32_t *queues,
    uint16_t n)
{
  uint16_t i;

  for (i = 0; i < n; i++) {
    rte_prefetch0(&fp_state->flowst[queues[i]]);
  }
}

void fast_flows_qman_pfbufs(struct dataplane_context *ctx, uint32_t *queues,
    uint16_t n)
{
  struct flextcp_pl_flowst *fs;
  uint16_t i;
  void *p;

  for (i = 0; i < n; i++) {
    fs = &fp_state->flowst[queues[i]];
    p = dma_pointer(fs->tx_base + fs->tx_next_pos, 1);
    rte_prefetch0(p);
    rte_prefetch0(p + 64);
  }
}

#pragma vectorize to_scalar
static int rte_ring_enqueue_wrapper(struct rte_ring *r, void *obj) {
  return rte_ring_enqueue(r, obj);
}

#pragma vectorize to_scalar
void notify_fastpath_core_wrapper(unsigned core) {
  notify_fastpath_core(core);
}

static void flow_tx_segment_modified(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, struct flextcp_pl_flowst *fs,
    uint32_t seq, uint32_t ack, uint32_t rxwnd, uint16_t payload,
    uint32_t payload_pos, uint32_t ts_echo, uint32_t ts_my, uint8_t fin) {
  flow_tx_segment(ctx, nbh, fs, seq, ack, rxwnd, payload, payload_pos, ts_echo, ts_my, fin);
}

static void flow_tx_segment_modified_vec(
  __m512i vctx, __m512i vnbh, __m512i vfs, __m256i vseq, __m256i vack, __m256i vrxwnd, __m256i vpayload,
  __m256i vpayload_pos, __m256i vts_echo, __m256i vts_my, __m256i vfin, __mmask8 k
) {
  // Just need to re-order the network buf handles
  // from [0, 1, 2, ...] to where 0 is at first set position in k, 1 is at second set position in k etc.
  vnbh = _mm512_maskz_expand_epi64(k, vnbh); // <- this should to just that!

  flow_tx_segment_vec(vctx, vnbh,
    vfs, vseq, vack, vrxwnd, vpayload, vpayload_pos, vts_echo, vts_my, vfin, k
  );
}

#pragma vectorize alive_check
int fast_flows_qman(struct dataplane_context *ctx, uint32_t queue,
    struct network_buf_handle *nbh, uint32_t ts)
{
  uint32_t flow_id = queue;
  struct flextcp_pl_flowst *fs = &fp_state->flowst[flow_id];
  uint32_t avail, len, tx_pos, tx_seq, ack, rx_wnd;
  uint16_t new_core;
  uint8_t fin;
  int ret = 0;

  fs_lock(fs);

  /* if connection has been moved, add to forwarding queue and stop */
  new_core = fp_state->flow_group_steering[fs->flow_group];
  if (new_core != ctx->id) {
    ALIVE_CHECK();
    /*fprintf(stderr, "fast_flows_qman: arrived on wrong core, forwarding "
        "%u -> %u (fs=%p, fg=%u)\n", ctx->id, new_core, fs, fs->flow_group);*/

    /* enqueue flo state on forwarding queue */
    if (rte_ring_enqueue_wrapper(ctxs[new_core]->qman_fwd_ring, fs) != 0) {
      ALIVE_CHECK();
      fprintf(stderr, "fast_flows_qman: rte_ring_enqueue failed\n");
      abort();
    }

    /* clear queue manager queue */
    if (qman_set(&ctx->qman, flow_id, 0, 0, 0,
          QMAN_SET_RATE | QMAN_SET_MAXCHUNK | QMAN_SET_AVAIL) != 0)
    {
      ALIVE_CHECK();
      fprintf(stderr, "flast_flows_qman: qman_set clear failed, UNEXPECTED\n");
      abort();
    }

    notify_fastpath_core_wrapper(new_core);

    ret = -1;
    goto unlock;
  }

  /* calculate how much is available to be sent */
  avail = tcp_txavail(fs, NULL);

#if PL_DEBUG_ATX
  fprintf(stderr, "ATX try_sendseg local=%08x:%05u remote=%08x:%05u "
      "tx_avail=%x tx_next_pos=%x avail=%u\n",
      f_beui32(fs->local_ip), f_beui16(fs->local_port),
      f_beui32(fs->remote_ip), f_beui16(fs->remote_port),
      fs->tx_avail, fs->tx_next_pos, avail);
#endif
#ifdef FLEXNIC_TRACING
  struct flextcp_pl_trev_afloqman te_afloqman = {
      .flow_id = flow_id,
      .tx_base = fs->tx_base,
      .tx_avail = fs->tx_avail,
      .tx_next_pos = fs->tx_next_pos,
      .tx_len = fs->tx_len,
      .rx_remote_avail = fs->rx_remote_avail,
      .tx_sent = fs->tx_sent,
    };
  trace_event(FLEXNIC_PL_TREV_AFLOQMAN, sizeof(te_afloqman), &te_afloqman);
#endif

  /* if there is no data available, stop */
  if (avail == 0) {
    ret = -1;
    goto unlock;
  }
  len = MIN(avail, TCP_MSS);

  /* state snapshot for creating segment */
  tx_seq = fs->tx_next_seq;
  tx_pos = fs->tx_next_pos;
  rx_wnd = fs->rx_avail;
  ack = fs->rx_next_seq;

  /* update tx flow state */
  fs->tx_next_seq += len;
  fs->tx_next_pos += len;
  if (fs->tx_next_pos >= fs->tx_len) {
    fs->tx_next_pos -= fs->tx_len;
  }
  fs->tx_sent += len;
  fs->tx_avail -= len;

  fin = (fs->rx_base_sp & FLEXNIC_PL_FLOWST_TXFIN) == FLEXNIC_PL_FLOWST_TXFIN &&
    !fs->tx_avail;

  /* make sure we don't send out dummy byte for FIN */
  if (fin) {
    //assert(len > 0);
    len--;
  }

  /* send out segment */
  flow_tx_segment_modified(ctx, nbh, fs, tx_seq, ack, rx_wnd, len, tx_pos,
      fs->tx_next_ts, ts, fin);
unlock:
  fs_unlock(fs);
  return ret;
}

#pragma vectorize alive_check
int fast_flows_qman_fwd(struct dataplane_context *ctx,
    struct flextcp_pl_flowst *fs)
{
  unsigned avail;
  uint16_t flow_id = fs - fp_state->flowst;

  /*fprintf(stderr, "fast_flows_qman_fwd: fs=%p\n", fs);*/

  fs_lock(fs);

  avail = tcp_txavail(fs, NULL);

  /* re-arm queue manager */
  if (qman_set(&ctx->qman, flow_id, fs->tx_rate, avail, TCP_MSS,
        QMAN_SET_RATE | QMAN_SET_MAXCHUNK | QMAN_SET_AVAIL) != 0)
  {
    ALIVE_CHECK();
    fprintf(stderr, "fast_flows_qman_fwd: qman_set failed, UNEXPECTED\n");
    abort();
  }

  fs_unlock(fs);
  return 0;
}

void fast_flows_packet_parse(struct dataplane_context *ctx,
    struct network_buf_handle **nbhs, void **fss, struct tcp_opts *tos,
    uint16_t n)
{
  struct pkt_tcp *p;
  uint16_t i, len;

  for (i = 0; i < n; i++) {
    if (fss[i] == NULL)
      continue;

    p = network_buf_bufoff(nbhs[i]);
    len = network_buf_len(nbhs[i]);

    int cond =
        (len < sizeof(*p)) |
        (f_beui16(p->eth.type) != ETH_TYPE_IP) |
        (p->ip.proto != IP_PROTO_TCP) |
        (IPH_V(&p->ip) != 4) |
        (IPH_HL(&p->ip) != 5) |
        (TCPH_HDRLEN(&p->tcp) < 5) |
        (len < f_beui16(p->ip.len) + sizeof(p->eth)) |
        (tcp_parse_options(p, len, &tos[i]) != 0) |
        (tos[i].ts == NULL);

    if (cond)
      fss[i] = NULL;
  }
}

#pragma vectorize
void fast_flows_packet_parse_m(struct dataplane_context *ctx,
    struct network_buf_handle *nbhs, void **fss, struct tcp_opts *tos)
{
  struct pkt_tcp *p;
  uint16_t len;

  //for (i = 0; i < n; i++) {
    if (*fss == NULL)
      return;

    p = network_buf_bufoff(nbhs);
    len = network_buf_len(nbhs);
    uint8_t ip_v_hl = p->ip._v_hl;

    int cond =
        (len < sizeof(*p)) |
        (f_beui16(p->eth.type) != ETH_TYPE_IP) |
        (p->ip.proto != IP_PROTO_TCP) |
        //(IPH_V(&p->ip) != 4) |
        ((ip_v_hl >> 4) != 4) |
        ((ip_v_hl & 0x0F) != 5) |
        (TCPH_HDRLEN(&p->tcp) < 5) |
        (len < f_beui16(p->ip.len) + sizeof(p->eth)) |
        (tcp_parse_options(p, len, tos) != 0) |
        ((*tos).ts == NULL);

    if (cond)
      *fss = NULL;
  //}
}

__m512i inline extract_16_bit(__m512i src) {
  return _mm512_srli_epi64(_mm512_slli_epi64(src, 48), 48);
}

__m512i inline extract_16_bit_bswap(__m512i src) {
  __m512i high = _mm512_srli_epi64(_mm512_slli_epi64(src, 56), 48);
  __m512i low = _mm512_srli_epi64(_mm512_slli_epi64(_mm512_srli_epi64(src, 8), 56), 56);
  return _mm512_or_epi64(high, low);
}

__m512i inline extract_8_bit(__m512i src) {
  return _mm512_srli_epi64(_mm512_slli_epi64(src, 56), 56);
}

__m512i inline mask_extract_8_bit(__m512i def, __mmask8 k, __m512i src) {
  return _mm512_mask_srli_epi64(def, k, _mm512_slli_epi64(src, 56), 56);
}

__m512i inline extract_24_bit(__m512i src) {
  return _mm512_srli_epi64(_mm512_slli_epi64(src, 40), 40);
}

__m512i inline extract_32_bit(__m512i src) {
  return _mm512_srli_epi64(_mm512_slli_epi64(src, 32), 32);
}

// extract the two bytes starting at the 32th bit and swap them
__m512i inline extract_16_at_32_bit_bswap(__m512i src) {
  __m512i high = _mm512_slli_epi64(_mm512_srli_epi64(_mm512_slli_epi64(src, 24), 56), 8);
  __m512i low = _mm512_srli_epi64(_mm512_slli_epi64(src, 16), 56);
  return _mm512_or_epi64(high, low);
}

void fast_flows_packet_parse_handvec(__m512i ctx, __m512i nbhs, __m512i fss, __m512i tos, __mmask8 k) {
  k = _kandn_mask8(_mm512_cmpeq_epi64_mask(_mm512_mask_i64gather_epi64(_mm512_undefined(), k, fss, NULL, 1), _mm512_setzero()), k);

  __m512i p = _mm512_add_epi64(_mm512_mask_i64gather_epi64(_mm512_undefined(), k, nbhs, offsetof(struct rte_mbuf, buf_addr), 1),
    extract_16_bit(_mm512_mask_i64gather_epi64(_mm512_undefined(), k, nbhs, offsetof(struct rte_mbuf, data_off), 1))
  );
  __m512i len = extract_16_bit(_mm512_mask_i64gather_epi64(_mm512_undefined(), k, nbhs, offsetof(struct rte_mbuf, data_len), 1));
  //__m512i ip_v_hl = extract_8_bit(_mm512_mask_i64gather_epi64(_mm512_undefined(), k, p, offsetof(struct pkt_tcp, ip._v_hl), 1));
  __m512i tcp_headerlen = _mm512_srli_epi64(extract_16_bit_bswap(
    _mm512_mask_i64gather_epi64(_mm512_undefined(), k, p, offsetof(struct pkt_tcp, tcp._hdrlen_rsvd_flags), 1)
  ), 12);

  __mmask8 big_or = _cvtu32_mask8(0);
  big_or = _kor_mask8(big_or,
    _mm512_cmplt_epu64_mask(len, _mm512_set1_epi64(sizeof(struct pkt_tcp)))
  );
  __m512i eth_type_and_above = _mm512_mask_i64gather_epi64(_mm512_undefined(), k, p, offsetof(struct pkt_tcp, eth.type), 1);
  big_or = _kor_mask8(
    big_or,
    _mm512_cmpneq_epu64_mask(extract_24_bit(
      eth_type_and_above
    ), _mm512_set1_epi64(
      __bswap_16(ETH_TYPE_IP) | ((4 << 4) | 5) << 16
    ))
  ); // combined eth.type and ip._v_hl check
  big_or = _kor_mask8(
    big_or,
    _mm512_cmpneq_epu64_mask(extract_8_bit(
      _mm512_mask_i64gather_epi64(_mm512_undefined(), k, p, offsetof(struct pkt_tcp, ip.proto), 1)
    ), _mm512_set1_epi64(IP_PROTO_TCP))
  );
  /*
  big_or = _kor_mask8(
    big_or,
    _mm512_cmpneq_epu64_mask(ip_v_hl, _mm512_set1_epi64((4 << 4) | 5))
  );*/
  __mmask8 tcp_headerlen_toosmall = _mm512_cmplt_epu64_mask(tcp_headerlen, _mm512_set1_epi64(5));
  big_or = _kor_mask8(
    big_or,
    tcp_headerlen_toosmall
  );
  big_or = _kor_mask8(
    big_or,
    _mm512_cmplt_epu64_mask(
      len,
      _mm512_add_epi64(
        extract_16_at_32_bit_bswap(eth_type_and_above),
        _mm512_set1_epi64(sizeof(struct eth_hdr))
      )
    )
  );

  // TCP parse options
  __m512i opts = _mm512_add_epi64(p, _mm512_set1_epi64(sizeof(struct pkt_tcp)));
  __m512i opts_len = _mm512_sub_epi64(_mm512_slli_epi64(tcp_headerlen, 2), _mm512_set1_epi64(20));
  __m512i off = _mm512_setzero_si512();
  _mm512_mask_i64scatter_epi64(offsetof(struct tcp_opts, ts), k, tos, _mm512_setzero_si512(), 1);

  __mmask8 topt_initial_fail = _kor_mask8(
    tcp_headerlen_toosmall,
    _mm512_cmpgt_epi64_mask(
      opts_len,
      _mm512_sub_epi64(len, _mm512_set1_epi64(sizeof(struct pkt_tcp)))
    )
  );
  __mmask8 topt_mask = _kandn_mask8(
    topt_initial_fail, k
  );
  big_or = _kor_mask8(big_or, topt_initial_fail);

  __m512i opt_fast_check = extract_32_bit(
    _mm512_mask_i64gather_epi64(_mm512_undefined(), topt_mask, opts, NULL, 1)
  );

  __mmask8 fastpath = _mm512_cmpeq_epi64_mask(opt_fast_check, 
    _mm512_set1_epi64(((TCP_OPT_NO_OP) | (TCP_OPT_NO_OP << 8) | (TCP_OPT_TIMESTAMP << 16) | (sizeof(struct tcp_timestamp_opt) << 24)))
  );
  fastpath = _kand_mask8(fastpath, _mm512_cmpeq_epi64_mask(opts_len, _mm512_set1_epi64(2 + sizeof(struct tcp_timestamp_opt))));
  fastpath = _kand_mask8(fastpath, topt_mask);
  _mm512_mask_i64scatter_epi64(offsetof(struct tcp_opts, ts), fastpath, tos, _mm512_add_epi64(opts, _mm512_set1_epi64(2)), 1);
  __mmask8 ts_was_set = fastpath;

  topt_mask = _kandn_mask8(fastpath, topt_mask);

  if (_cvtmask8_u32(topt_mask) != 0) { // TCP parse options main body
    while (true) {
      topt_mask = _kand_mask8(
        topt_mask,
        _mm512_cmplt_epu64_mask(off, opts_len)
      );

      if (_cvtmask8_u32(topt_mask) == 0) {
        break;
      }

      __mmask8 iteration_mask = topt_mask;

      __m512i opt_kind = extract_8_bit(
        _mm512_mask_i64gather_epi64(_mm512_undefined(), iteration_mask, _mm512_add_epi64(opts, off), NULL, 1)
      );
      __m512i opt_avail = _mm512_sub_epi64(opts_len, off);
      __m512i opt_len = _mm512_undefined_epi32();

      __mmask8 end_of_opts = _mm512_cmpeq_epi64_mask(opt_kind, _mm512_set1_epi64(TCP_OPT_END_OF_OPTIONS));
      topt_mask = _kandn_mask8(end_of_opts, topt_mask);
      iteration_mask = _kandn_mask8(end_of_opts, iteration_mask); // break of opt_kind == TCP_OPT_END_OF_OPTIONS

      __mmask8 cur_if_mask = _kand_mask8(
        iteration_mask,
        _mm512_cmpeq_epi64_mask(opt_kind, _mm512_set1_epi64(TCP_OPT_NO_OP))
      );
      opt_len = _mm512_mask_set1_epi64(opt_len, cur_if_mask, 1);
      cur_if_mask = _kandn_mask8(
        cur_if_mask,
        iteration_mask
      ); // should be the else mask?

      if (_cvtmask8_u32(cur_if_mask) != 0) {
        __mmask8 too_short = _mm512_cmplt_epu64_mask(opt_avail, _mm512_set1_epi64(2));
        big_or = _kor_mask8(big_or, too_short);
        cur_if_mask = _kandn_mask8(too_short, cur_if_mask);
        iteration_mask = _kandn_mask8(too_short, iteration_mask);
        topt_mask = _kandn_mask8(too_short, topt_mask);

        opt_len = mask_extract_8_bit(
          opt_len,
          cur_if_mask,
          _mm512_mask_i64gather_epi64(_mm512_undefined(), iteration_mask, _mm512_add_epi64(opts, off), (void*) 1, 1)
        ); // opt_len = opt[off + 1];

        cur_if_mask = _kand_mask8(
          cur_if_mask,
          _mm512_cmpeq_epi64_mask(opt_kind, _mm512_set1_epi64(TCP_OPT_TIMESTAMP))
        );

        __mmask8 len_invalid = _mm512_cmpneq_epi64_mask(opt_len, _mm512_set1_epi64(sizeof(struct tcp_timestamp_opt)));
        len_invalid = _kand_mask8(len_invalid, cur_if_mask);
        if (_cvtmask8_u32(len_invalid) != 0) {
          big_or = _kor_mask8(big_or, len_invalid);
          cur_if_mask = _kandn_mask8(len_invalid, cur_if_mask);
          iteration_mask = _kandn_mask8(len_invalid, iteration_mask);
          topt_mask = _kandn_mask8(len_invalid, topt_mask);
        }

        _mm512_mask_i64scatter_epi64(offsetof(struct tcp_opts, ts), cur_if_mask, tos, _mm512_add_epi64(opts, off), 1);
        ts_was_set = _kor_mask8(ts_was_set, cur_if_mask);
      }

      off = _mm512_mask_add_epi64(off, iteration_mask, off, opt_len);
    }
  }
  // All for which ts was not set
  big_or = _kor_mask8(
    big_or,
    _knot_mask8(ts_was_set)
  );

  _mm512_mask_i64scatter_epi64(NULL, big_or, fss, _mm512_setzero_si512(), 1); // set to zero where big_or is true
}

#pragma vectorize
void fast_flows_packet_pfbufs(struct dataplane_context *ctx,
    void *fss, uint16_t n)
{
  uint64_t rx_base;
  void *p;
  struct flextcp_pl_flowst *fs;
  if (fss == NULL)
    return;

  fs = fss;
  rx_base = fs->rx_base_sp & FLEXNIC_PL_FLOWST_RX_MASK;
  p = dma_pointer(rx_base + fs->rx_next_pos, 1);
  //rte_prefetch0(p);
}

void inline add_stat(uint64_t* pos, uint64_t by) {
  *pos += by;
}

void inline add_stat_vec(__m512i pos, __m512i by, __mmask8 m) {
  if (_cvtmask8_u32(m) != 0) {
    __m128i lower_pos = _mm512_extracti64x2_epi64(pos, 0);
    __m128i lower_by = _mm512_extracti64x2_epi64(by, 0);
    uint64_t i_pos = _mm_extract_epi64(lower_pos, 0);
    uint64_t i_by = _mm_extract_epi64(lower_by, 0);
    *((uint64_t*) i_pos) += i_by;
  }
}

/* Received packet */
int fast_flows_packet(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, void *fsp, struct tcp_opts *opts,
    uint32_t ts)
{
  struct pkt_tcp *p = network_buf_bufoff(nbh);
  struct flextcp_pl_flowst *fs = fsp;
  uint32_t payload_bytes, payload_off, seq, ack, old_avail, new_avail,
           orig_payload;
  uint8_t *payload;
  uint32_t rx_bump = 0, tx_bump = 0, rx_pos, rtt;
  int no_permanent_sp = 0;
  uint16_t tcp_extra_hlen, trim_start, trim_end;
  uint16_t flow_id = fs - fp_state->flowst;
  int trigger_ack = 0, fin_bump = 0;

  tcp_extra_hlen = (TCPH_HDRLEN(&p->tcp) - 5) * 4;
  payload_off = sizeof(*p) + tcp_extra_hlen;
  payload_bytes =
      f_beui16(p->ip.len) - (sizeof(p->ip) + sizeof(p->tcp) + tcp_extra_hlen);
  orig_payload = payload_bytes;

#if PL_DEBUG_ARX
  fprintf(stderr, "FLOW local=%08x:%05u remote=%08x:%05u  RX: seq=%u ack=%u "
      "flags=%x payload=%u\n",
      f_beui32(p->ip.dest), f_beui16(p->tcp.dest),
      f_beui32(p->ip.src), f_beui16(p->tcp.src), f_beui32(p->tcp.seqno),
      f_beui32(p->tcp.ackno), TCPH_FLAGS(&p->tcp), payload_bytes);
#endif

  fs_lock(fs);

#ifdef FLEXNIC_TRACING
  struct flextcp_pl_trev_rxfs te_rxfs = {
      .local_ip = f_beui32(p->ip.dest),
      .remote_ip = f_beui32(p->ip.src),
      .local_port = f_beui16(p->tcp.dest),
      .remote_port = f_beui16(p->tcp.src),

      .flow_id = flow_id,
      .flow_seq = f_beui32(p->tcp.seqno),
      .flow_ack = f_beui32(p->tcp.ackno),
      .flow_flags = TCPH_FLAGS(&p->tcp),
      .flow_len = payload_bytes,

      .fs_rx_nextpos = fs->rx_next_pos,
      .fs_rx_nextseq = fs->rx_next_seq,
      .fs_rx_avail = fs->rx_avail,
      .fs_tx_nextpos = fs->tx_next_pos,
      .fs_tx_nextseq = fs->tx_next_seq,
      .fs_tx_sent = fs->tx_sent,
      .fs_tx_avail = fs->tx_avail,
    };
  trace_event(FLEXNIC_PL_TREV_RXFS, sizeof(te_rxfs), &te_rxfs);
#endif

#if PL_DEBUG_ARX
  fprintf(stderr, "FLOW local=%08x:%05u remote=%08x:%05u  ST: op=%"PRIx64
      " rx_pos=%x rx_next_seq=%u rx_avail=%x  tx_pos=%x tx_next_seq=%u"
      " tx_sent=%u sp=%u\n",
      f_beui32(p->ip.dest), f_beui16(p->tcp.dest),
      f_beui32(p->ip.src), f_beui16(p->tcp.src), fs->opaque, fs->rx_next_pos,
      fs->rx_next_seq, fs->rx_avail, fs->tx_next_pos, fs->tx_next_seq,
      fs->tx_sent, fs->slowpath);
#endif

  /* state indicates slow path */
  if (UNLIKELY((fs->rx_base_sp & FLEXNIC_PL_FLOWST_SLOWPATH) != 0)) {
    fprintf(stderr, "dma_krx_pkt_fastpath: slowpath because of state\n");
    goto slowpath;
  }

  /* if we get weird flags -> kernel */
  if (UNLIKELY((TCPH_FLAGS(&p->tcp) & ~(TCP_ACK | TCP_PSH | TCP_ECE | TCP_CWR |
            TCP_FIN)) != 0))
  {
    if ((TCPH_FLAGS(&p->tcp) & TCP_SYN) != 0) {
      /* for SYN/SYN-ACK we'll let the kernel handle them out of band */
      no_permanent_sp = 1;
    } else {
      fprintf(stderr, "dma_krx_pkt_fastpath: slow path because of flags (%x)\n",
          TCPH_FLAGS(&p->tcp));
    }
    goto slowpath;
  }

  /* calculate how much data is available to be sent before processing this
   * packet, to detect whether more data can be sent afterwards */
  old_avail = tcp_txavail(fs, NULL);

  seq = f_beui32(p->tcp.seqno);
  ack = f_beui32(p->tcp.ackno);
  rx_pos = fs->rx_next_pos;

  /* trigger an ACK if there is payload (even if we discard it) */
#ifndef SKIP_ACK
  if (payload_bytes > 0)
    trigger_ack = 1;
#endif

  /* Stats for CC */
  if ((TCPH_FLAGS(&p->tcp) & TCP_ACK) == TCP_ACK) {
    fs->cnt_rx_acks++;
  }

  /* if there is a valid ack, process it */
  if (LIKELY((TCPH_FLAGS(&p->tcp) & TCP_ACK) == TCP_ACK &&
      tcp_valid_rxack(fs, ack, &tx_bump) == 0))
  {
    fs->cnt_rx_ack_bytes += tx_bump;
    if ((TCPH_FLAGS(&p->tcp) & TCP_ECE) == TCP_ECE) {
      fs->cnt_rx_ecn_bytes += tx_bump;
    }

    if (LIKELY(tx_bump <= fs->tx_sent)) {
      fs->tx_sent -= tx_bump;
    } else {
#ifdef ALLOW_FUTURE_ACKS
      fs->tx_next_seq += tx_bump - fs->tx_sent;
      fs->tx_next_pos += tx_bump - fs->tx_sent;
      if (fs->tx_next_pos >= fs->tx_len)
        fs->tx_next_pos -= fs->tx_len;
      fs->tx_avail -= tx_bump - fs->tx_sent;
      fs->tx_sent = 0;
#else
      /* this should not happen */
      fprintf(stderr, "dma_krx_pkt_fastpath: acked more bytes than sent\n");
      abort();
#endif
    }

    /* duplicate ack */
    if (UNLIKELY(tx_bump != 0)) {
      fs->rx_dupack_cnt = 0;
    } else if (UNLIKELY(orig_payload == 0 && ++fs->rx_dupack_cnt >= 3)) {
      /* reset to last acknowledged position */
      flow_reset_retransmit(fs);
      goto unlock;
    }
  }

#ifdef FLEXNIC_PL_OOO_RECV
  /* check if we should drop this segment */
  if (UNLIKELY(tcp_trim_rxbuf(fs, seq, payload_bytes, &trim_start, &trim_end) != 0)) {
    /* packet is completely outside of unused receive buffer */
    trigger_ack = 1;
    goto unlock;
  }

  /* trim payload to what we can actually use */
  payload_bytes -= trim_start + trim_end;
  payload_off += trim_start;
  payload = (uint8_t *) p + payload_off;
  seq += trim_start;

  /* handle out of order segment */
  if (UNLIKELY(seq != fs->rx_next_seq)) {
    trigger_ack = 1;

    /* if there is no payload abort immediately */
    if (payload_bytes == 0) {
      goto unlock;
    }

    /* otherwise check if we can add it to the out of order interval */
    if (fs->rx_ooo_len == 0) {
      fs->rx_ooo_start = seq;
      fs->rx_ooo_len = payload_bytes;
      flow_rx_seq_write(fs, seq, payload_bytes, payload);
      /*fprintf(stderr, "created OOO interval (%p start=%u len=%u)\n",
          fs, fs->rx_ooo_start, fs->rx_ooo_len);*/
    } else if (seq + payload_bytes == fs->rx_ooo_start) {
      /* TODO: those two overlap checks should be more sophisticated */
      fs->rx_ooo_start = seq;
      fs->rx_ooo_len += payload_bytes;
      flow_rx_seq_write(fs, seq, payload_bytes, payload);
      /*fprintf(stderr, "extended OOO interval (%p start=%u len=%u)\n",
          fs, fs->rx_ooo_start, fs->rx_ooo_len);*/
    } else if (fs->rx_ooo_start + fs->rx_ooo_len == seq) {
      /* TODO: those two overlap checks should be more sophisticated */
      fs->rx_ooo_len += payload_bytes;
      flow_rx_seq_write(fs, seq, payload_bytes, payload);
      /*fprintf(stderr, "extended OOO interval (%p start=%u len=%u)\n",
          fs, fs->rx_ooo_start, fs->rx_ooo_len);*/
    } else {
      /*fprintf(stderr, "Sad, no luck with OOO interval (%p ooo.start=%u "
          "ooo.len=%u seq=%u bytes=%u)\n", fs, fs->rx_ooo_start,
          fs->rx_ooo_len, seq, payload_bytes);*/
    }
    goto unlock;
  }

#else
  /* check if we should drop this segment */
  if (tcp_valid_rxseq(fs, seq, payload_bytes, &trim_start, &trim_end) != 0) {
    trigger_ack = 1;
#if 0
    fprintf(stderr, "dma_krx_pkt_fastpath: packet with bad seq "
        "(got %u, expect %u, avail %u, payload %u)\n", seq, fs->rx_next_seq,
        fs->rx_avail, payload_bytes);
#endif
    goto unlock;
  }

  /* trim payload to what we can actually use */
  payload_bytes -= trim_start + trim_end;
  payload_off += trim_start;
  payload = (uint8_t *) p + payload_off;
#endif

  /* update rtt estimate */
  fs->tx_next_ts = f_beui32(opts->ts->ts_val);
  if (LIKELY((TCPH_FLAGS(&p->tcp) & TCP_ACK) == TCP_ACK &&
      f_beui32(opts->ts->ts_ecr) != 0))
  {
    rtt = ts - f_beui32(opts->ts->ts_ecr);
    if (rtt < TCP_MAX_RTT) {
      if (LIKELY(fs->rtt_est != 0)) {
        fs->rtt_est = (fs->rtt_est * 7 + rtt) / 8;
      } else {
        fs->rtt_est = rtt;
      }
    }
  }

  fs->rx_remote_avail = f_beui16(p->tcp.wnd);

  /* make sure we don't receive anymore payload after FIN */
  if ((fs->rx_base_sp & FLEXNIC_PL_FLOWST_RXFIN) == FLEXNIC_PL_FLOWST_RXFIN &&
      payload_bytes > 0)
  {
    fprintf(stderr, "fast_flows_packet: data after FIN dropped\n");
    goto unlock;
  }

  /* if there is payload, dma it to the receive buffer */
  if (payload_bytes > 0) {
    flow_rx_write(fs, fs->rx_next_pos, payload_bytes, payload);

    rx_bump = payload_bytes;
    fs->rx_avail -= payload_bytes;
    fs->rx_next_pos += payload_bytes;
    if (fs->rx_next_pos >= fs->rx_len) {
      fs->rx_next_pos -= fs->rx_len;
    }
    assert(fs->rx_next_pos < fs->rx_len);
    fs->rx_next_seq += payload_bytes;
#ifndef SKIP_ACK
    trigger_ack = 1;
#endif

#ifdef FLEXNIC_PL_OOO_RECV
    /* if we have out of order segments, check whether buffer is continuous
     * or superfluous */
    if (UNLIKELY(fs->rx_ooo_len != 0)) {
      if (tcp_trim_rxbuf(fs, fs->rx_ooo_start, fs->rx_ooo_len, &trim_start,
            &trim_end) != 0) {
          /*fprintf(stderr, "dropping ooo (%p ooo.start=%u ooo.len=%u seq=%u "
              "len=%u next_seq=%u)\n", fs, fs->rx_ooo_start, fs->rx_ooo_len, seq,
              payload_bytes, fs->rx_next_seq);*/
        /* completely superfluous: drop out of order interval */
        fs->rx_ooo_len = 0;
      } else {
        /* adjust based on overlap */
        fs->rx_ooo_start += trim_start;
        fs->rx_ooo_len -= trim_start + trim_end;
        /*fprintf(stderr, "adjusting ooo (%p ooo.start=%u ooo.len=%u seq=%u "
            "len=%u next_seq=%u)\n", fs, fs->rx_ooo_start, fs->rx_ooo_len, seq,
            payload_bytes, fs->rx_next_seq);*/
        if (fs->rx_ooo_len > 0 && fs->rx_ooo_start == fs->rx_next_seq) {
          /* yay, we caught up, make continuous and drop OOO interval */
          /*fprintf(stderr, "caught up with ooo buffer (%p start=%u len=%u)\n",
              fs, fs->rx_ooo_start, fs->rx_ooo_len);*/

          rx_bump += fs->rx_ooo_len;
          fs->rx_avail -= fs->rx_ooo_len;
          fs->rx_next_pos += fs->rx_ooo_len;
          if (fs->rx_next_pos >= fs->rx_len) {
            fs->rx_next_pos -= fs->rx_len;
          }
          assert(fs->rx_next_pos < fs->rx_len);
          fs->rx_next_seq += fs->rx_ooo_len;

          fs->rx_ooo_len = 0;
        }
      }
    }
#endif
  }

  if ((TCPH_FLAGS(&p->tcp) & TCP_FIN) == TCP_FIN &&
      !(fs->rx_base_sp & FLEXNIC_PL_FLOWST_RXFIN))
  {
    if (fs->rx_next_seq == f_beui32(p->tcp.seqno) + orig_payload && !fs->rx_ooo_len) {
      fin_bump = 1;
      fs->rx_base_sp |= FLEXNIC_PL_FLOWST_RXFIN;
      /* FIN takes up sequence number space */
      fs->rx_next_seq++;
      trigger_ack = 1;
    } else {
      fprintf(stderr, "fast_flows_packet: ignored fin because out of order\n");
    }
  }

unlock:
  /* if we bumped at least one, then we need to add a notification to the
   * queue */
  if (LIKELY(rx_bump != 0 || tx_bump != 0 || fin_bump)) {
#if PL_DEBUG_ARX
    fprintf(stderr, "dma_krx_pkt_fastpath: updating application state\n");
#endif

    uint16_t type;
    type = FLEXTCP_PL_ARX_CONNUPDATE;

    if (fin_bump) {
      type |= FLEXTCP_PL_ARX_FLRXDONE << 8;
    }

#ifdef FLEXNIC_TRACING
    struct flextcp_pl_trev_arx te_arx = {
        .opaque = fs->opaque,
        .rx_bump = rx_bump,
        .tx_bump = tx_bump,
        .rx_pos = rx_pos,
        .flags = type,

        .flow_id = flow_id,
        .db_id = fs->db_id,

        .local_ip = f_beui32(p->ip.dest),
        .remote_ip = f_beui32(p->ip.src),
        .local_port = f_beui16(p->tcp.dest),
        .remote_port = f_beui16(p->tcp.src),
      };
    trace_event(FLEXNIC_PL_TREV_ARX, sizeof(te_arx), &te_arx);
#endif

    arx_cache_add(ctx, fs->db_id, fs->opaque, rx_bump, rx_pos, tx_bump, type);
  }

  /* Flow control: More receiver space? -> might need to start sending */
  new_avail = tcp_txavail(fs, NULL);
  if (new_avail > old_avail) {
    /* update qman queue */
    if (qman_set(&ctx->qman, flow_id, fs->tx_rate, new_avail -
          old_avail, TCP_MSS, QMAN_SET_RATE | QMAN_SET_MAXCHUNK
          | QMAN_ADD_AVAIL) != 0)
    {
      fprintf(stderr, "fast_flows_packet: qman_set 1 failed, UNEXPECTED\n");
      abort();
    }
  }

  /* if we need to send an ack, also send packet to TX pipeline to do so */
  if (trigger_ack) {
    flow_tx_ack(ctx, fs->tx_next_seq, fs->rx_next_seq, fs->rx_avail,
        fs->tx_next_ts, ts, nbh, opts->ts);
  }

  fs_unlock(fs);
  return trigger_ack;

slowpath:
  if (!no_permanent_sp) {
    fs->rx_base_sp |= FLEXNIC_PL_FLOWST_SLOWPATH;
  }

  fs_unlock(fs);
  /* TODO: should pass current flow state to kernel as well */
  return -1;
}

/* Received packet */
#pragma vectorize alive_check
int fast_flows_packet_m(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, void *fsp, struct tcp_opts *opts,
    uint32_t ts)
{
  struct pkt_tcp *p = network_buf_bufoff(nbh);
  struct flextcp_pl_flowst *old_fs = fsp;
  uint16_t flow_id = old_fs - fp_state->flowst;
  //struct flextcp_pl_flowst *fs = __transpose(old_fs);
  struct flextcp_pl_flowst *fs = old_fs;
#ifdef ASTVEC_CURRENTLY_VECTORIZING
  __transpose_ignore(fs, &fs->local_ip, &fs->local_port, &fs->remote_ip, &fs->remote_port, &fs->remote_mac, &fs->flow_group, &fs->bump_seq, &fs->lock);
  __transpose_readonly(fs, &fs->tx_len, &fs->db_id, &fs->opaque, &fs->tx_rate);
#endif

  uint32_t payload_bytes, payload_off, seq, ack, old_avail, new_avail,
           orig_payload;
  uint8_t *payload;
  uint32_t rx_bump = 0, tx_bump = 0, rx_pos, rtt;
  int no_permanent_sp = 0;
  uint16_t tcp_extra_hlen, trim_start, trim_end;
  int trigger_ack = 0, fin_bump = 0;

  tcp_extra_hlen = (TCPH_HDRLEN(&p->tcp) - 5) * 4;
  payload_off = sizeof(*p) + tcp_extra_hlen;
  payload_bytes =
      f_beui16(p->ip.len) - (sizeof(p->ip) + sizeof(p->tcp) + tcp_extra_hlen);
  orig_payload = payload_bytes;

#ifdef FFPACKET_STATS
  add_stat(&ctx->ffp_pos[0], 1);
#endif
  uint32_t _tcph_flags = TCPH_FLAGS(&p->tcp);

#if PL_DEBUG_ARX
  fprintf(stderr, "FLOW local=%08x:%05u remote=%08x:%05u  RX: seq=%u ack=%u "
      "flags=%x payload=%u\n",
      f_beui32(p->ip.dest), f_beui16(p->tcp.dest),
      f_beui32(p->ip.src), f_beui16(p->tcp.src), f_beui32(p->tcp.seqno),
      f_beui32(p->tcp.ackno), TCPH_FLAGS(&p->tcp), payload_bytes);
#endif

  //fs_lock(fs);

#ifdef FLEXNIC_TRACING
  struct flextcp_pl_trev_rxfs te_rxfs = {
      .local_ip = f_beui32(p->ip.dest),
      .remote_ip = f_beui32(p->ip.src),
      .local_port = f_beui16(p->tcp.dest),
      .remote_port = f_beui16(p->tcp.src),

      .flow_id = flow_id,
      .flow_seq = f_beui32(p->tcp.seqno),
      .flow_ack = f_beui32(p->tcp.ackno),
      .flow_flags = TCPH_FLAGS(&p->tcp),
      .flow_len = payload_bytes,

      .fs_rx_nextpos = fs->rx_next_pos,
      .fs_rx_nextseq = fs->rx_next_seq,
      .fs_rx_avail = fs->rx_avail,
      .fs_tx_nextpos = fs->tx_next_pos,
      .fs_tx_nextseq = fs->tx_next_seq,
      .fs_tx_sent = fs->tx_sent,
      .fs_tx_avail = fs->tx_avail,
    };
  trace_event(FLEXNIC_PL_TREV_RXFS, sizeof(te_rxfs), &te_rxfs);
#endif

#if PL_DEBUG_ARX
  fprintf(stderr, "FLOW local=%08x:%05u remote=%08x:%05u  ST: op=%"PRIx64
      " rx_pos=%x rx_next_seq=%u rx_avail=%x  tx_pos=%x tx_next_seq=%u"
      " tx_sent=%u sp=%u\n",
      f_beui32(p->ip.dest), f_beui16(p->tcp.dest),
      f_beui32(p->ip.src), f_beui16(p->tcp.src), fs->opaque, fs->rx_next_pos,
      fs->rx_next_seq, fs->rx_avail, fs->tx_next_pos, fs->tx_next_seq,
      fs->tx_sent, fs->slowpath);
#endif

  /* state indicates slow path */
  if (UNLIKELY((fs->rx_base_sp & FLEXNIC_PL_FLOWST_SLOWPATH) != 0)) {
    ALIVE_CHECK();
    fprintf(stderr, "dma_krx_pkt_fastpath: slowpath because of state\n");
    goto slowpath;
  }

  /* if we get weird flags -> kernel */
  if (UNLIKELY((_tcph_flags & ~(TCP_ACK | TCP_PSH | TCP_ECE | TCP_CWR |
            TCP_FIN)) != 0))
  {
    ALIVE_CHECK();
#ifdef FFPACKET_STATS
    add_stat(&ctx->ffp_pos[1], 1);
#endif
    if ((_tcph_flags & TCP_SYN) != 0) {
      /* for SYN/SYN-ACK we'll let the kernel handle them out of band */
      no_permanent_sp = 1;
    } else {
      ALIVE_CHECK();
      //fprintf(stderr, "dma_krx_pkt_fastpath: slow path because of flags (%x)\n",
      //    TCPH_FLAGS(&p->tcp));
      fprintf(stderr, "dma_krx_pkt_fastpath: slow path because of flags\n");
    }
    goto slowpath;
  }

  /* calculate how much data is available to be sent before processing this
   * packet, to detect whether more data can be sent afterwards */
  old_avail = tcp_txavail(fs, NULL);

  seq = f_beui32(p->tcp.seqno);
  ack = f_beui32(p->tcp.ackno);
  rx_pos = fs->rx_next_pos;

  /* trigger an ACK if there is payload (even if we discard it) */
#ifndef SKIP_ACK
  if (payload_bytes > 0)
    trigger_ack = 1;
#endif

  /* Stats for CC */
  if ((_tcph_flags & TCP_ACK) == TCP_ACK) {
    fs->cnt_rx_acks++;
  }

  /* if there is a valid ack, process it */
  if (LIKELY((_tcph_flags & TCP_ACK) == TCP_ACK &&
      tcp_valid_rxack(fs, ack, &tx_bump) == 0))
  {
#ifdef FFPACKET_STATS
    add_stat(&ctx->ffp_pos[2], 1);
#endif
    fs->cnt_rx_ack_bytes += tx_bump;
    if ((_tcph_flags & TCP_ECE) == TCP_ECE) {
      fs->cnt_rx_ecn_bytes += tx_bump;
    }

    if (LIKELY(tx_bump <= fs->tx_sent)) {
      fs->tx_sent = fs->tx_sent - tx_bump;
    } else {
      ALIVE_CHECK();
#ifdef FFPACKET_STATS
    add_stat(&ctx->ffp_pos[3], 1);
#endif
#ifdef ALLOW_FUTURE_ACKS
      fs->tx_next_seq = fs->tx_next_seq + tx_bump - fs->tx_sent;
      fs->tx_next_pos += tx_bump - fs->tx_sent;
      if (fs->tx_next_pos >= fs->tx_len)
        fs->tx_next_pos -= fs->tx_len;
      fs->tx_next_pos = fs->tx_next_pos;
      fs->tx_avail = fs->tx_avail - tx_bump - fs->tx_sent;
      fs->tx_sent = 0;
#else
      /* this should not happen */
      fprintf(stderr, "dma_krx_pkt_fastpath: acked more bytes than sent\n");
      abort();
#endif
    }

    /* duplicate ack */
    if (UNLIKELY(tx_bump != 0)) {
      ALIVE_CHECK();
      fs->rx_dupack_cnt = 0;
    } else if (UNLIKELY(orig_payload == 0 && ++fs->rx_dupack_cnt >= 3)) {
      ALIVE_CHECK();
#ifdef FFPACKET_STATS
      add_stat(&ctx->ffp_pos[4], 1);
#endif
      /* reset to last acknowledged position */
      flow_reset_retransmit(fs);
      goto unlock;
    }
  }

#ifdef FLEXNIC_PL_OOO_RECV
  /* check if we should drop this segment */
  if (UNLIKELY(tcp_trim_rxbuf(fs, seq, payload_bytes, &trim_start, &trim_end) != 0)) {
    /* packet is completely outside of unused receive buffer */
    trigger_ack = 1;
    goto unlock;
  }

  /* trim payload to what we can actually use */
  payload_bytes -= trim_start + trim_end;
  payload_off += trim_start;
  payload = (uint8_t *) p + payload_off;
  seq += trim_start;

  /* handle out of order segment */
  if (UNLIKELY(seq != fs->rx_next_seq)) {
    ALIVE_CHECK();
#ifdef FFPACKET_STATS
    add_stat(&ctx->ffp_pos[5], 1);
#endif
    trigger_ack = 1;

    /* if there is no payload abort immediately */
    if (payload_bytes == 0) {
      goto unlock;
    }

    /* otherwise check if we can add it to the out of order interval */
    if (fs->rx_ooo_len == 0) {
      ALIVE_CHECK();
#ifdef FFPACKET_STATS
      add_stat(&ctx->ffp_pos[6], 1);
#endif
      fs->rx_ooo_start = seq;
      fs->rx_ooo_len = payload_bytes;
      flow_rx_seq_write(fs, seq, payload_bytes, payload);
      /*fprintf(stderr, "created OOO interval (%p start=%u len=%u)\n",
          fs, fs->rx_ooo_start, fs->rx_ooo_len);*/
    } else if (seq + payload_bytes == fs->rx_ooo_start) {
      ALIVE_CHECK();
#ifdef FFPACKET_STATS
      add_stat(&ctx->ffp_pos[7], 1);
#endif
      /* TODO: those two overlap checks should be more sophisticated */
      fs->rx_ooo_start = seq;
      fs->rx_ooo_len = fs->rx_ooo_len + payload_bytes;
      flow_rx_seq_write(fs, seq, payload_bytes, payload);
      /*fprintf(stderr, "extended OOO interval (%p start=%u len=%u)\n",
          fs, fs->rx_ooo_start, fs->rx_ooo_len);*/
    } else if (fs->rx_ooo_start + fs->rx_ooo_len == seq) {
      ALIVE_CHECK();
#ifdef FFPACKET_STATS
      add_stat(&ctx->ffp_pos[8], 1);
#endif
      /* TODO: those two overlap checks should be more sophisticated */
      fs->rx_ooo_len = fs->cnt_tx_drops + payload_bytes;
      flow_rx_seq_write(fs, seq, payload_bytes, payload);
      /*fprintf(stderr, "extended OOO interval (%p start=%u len=%u)\n",
          fs, fs->rx_ooo_start, fs->rx_ooo_len);*/
    } else {
      /*fprintf(stderr, "Sad, no luck with OOO interval (%p ooo.start=%u "
          "ooo.len=%u seq=%u bytes=%u)\n", fs, fs->rx_ooo_start,
          fs->rx_ooo_len, seq, payload_bytes);*/
    }
    goto unlock;
  }

#else
  /* check if we should drop this segment */
  if (tcp_valid_rxseq(fs, seq, payload_bytes, &trim_start, &trim_end) != 0) {
    trigger_ack = 1;
#if 0
    fprintf(stderr, "dma_krx_pkt_fastpath: packet with bad seq "
        "(got %u, expect %u, avail %u, payload %u)\n", seq, fs->rx_next_seq,
        fs->rx_avail, payload_bytes);
#endif
    goto unlock;
  }

  /* trim payload to what we can actually use */
  payload_bytes -= trim_start + trim_end;
  payload_off += trim_start;
  payload = (uint8_t *) p + payload_off;
#endif

  /* update rtt estimate */
  fs->tx_next_ts = f_beui32(opts->ts->ts_val);
  uint32_t opts_ts_ts_ecr = f_beui32(opts->ts->ts_ecr);
  if (LIKELY((_tcph_flags & TCP_ACK) == TCP_ACK &&
      opts_ts_ts_ecr != 0))
  {
#ifdef FFPACKET_STATS
    add_stat(&ctx->ffp_pos[9], 1);
#endif
    rtt = ts - opts_ts_ts_ecr;
    if (rtt < TCP_MAX_RTT) {
      if (LIKELY(fs->rtt_est != 0)) {
        fs->rtt_est = (fs->rtt_est * 7 + rtt) / 8;
      } else {
        fs->rtt_est = rtt;
      }
    }
  }

  fs->rx_remote_avail = f_beui16(p->tcp.wnd);

  /* make sure we don't receive anymore payload after FIN */
  if ((fs->rx_base_sp & FLEXNIC_PL_FLOWST_RXFIN) == FLEXNIC_PL_FLOWST_RXFIN &&
      payload_bytes > 0)
  {
    ALIVE_CHECK();
    fprintf(stderr, "fast_flows_packet: data after FIN dropped\n");
    goto unlock;
  }

  /* if there is payload, dma it to the receive buffer */
  if (payload_bytes > 0) {
    ALIVE_CHECK();
#ifdef FFPACKET_STATS
    add_stat(&ctx->ffp_pos[10], 1);
#endif
    flow_rx_write(fs, fs->rx_next_pos, payload_bytes, payload);

    rx_bump = payload_bytes;
    fs->rx_avail -= payload_bytes;
    fs->rx_next_pos += payload_bytes;
    if (fs->rx_next_pos >= fs->rx_len) {
      fs->rx_next_pos -= fs->rx_len;
    }
    //assert(fs->rx_next_pos < fs->rx_len);
    fs->rx_next_seq += payload_bytes;
#ifndef SKIP_ACK
    trigger_ack = 1;
#endif

#ifdef FLEXNIC_PL_OOO_RECV
    /* if we have out of order segments, check whether buffer is continuous
     * or superfluous */
    if (UNLIKELY(fs->rx_ooo_len != 0)) {
      ALIVE_CHECK();
#ifdef FFPACKET_STATS
      add_stat(&ctx->ffp_pos[11], 1);
#endif
      if (tcp_trim_rxbuf(fs, fs->rx_ooo_start, fs->rx_ooo_len, &trim_start,
            &trim_end) != 0) {
          /*fprintf(stderr, "dropping ooo (%p ooo.start=%u ooo.len=%u seq=%u "
              "len=%u next_seq=%u)\n", fs, fs->rx_ooo_start, fs->rx_ooo_len, seq,
              payload_bytes, fs->rx_next_seq);*/
        /* completely superfluous: drop out of order interval */
        fs->rx_ooo_len = 0;
      } else {
        ALIVE_CHECK();
        /* adjust based on overlap */
        fs->rx_ooo_start = fs->rx_ooo_start = fs->rx_ooo_start + trim_start;
        fs->rx_ooo_len -= trim_start + trim_end;
        /*fprintf(stderr, "adjusting ooo (%p ooo.start=%u ooo.len=%u seq=%u "
            "len=%u next_seq=%u)\n", fs, fs->rx_ooo_start, fs->rx_ooo_len, seq,
            payload_bytes, fs->rx_next_seq);*/
        if (fs->rx_ooo_len > 0 && fs->rx_ooo_start == fs->rx_next_seq) {
          ALIVE_CHECK();
          /* yay, we caught up, make continuous and drop OOO interval */
          /*fprintf(stderr, "caught up with ooo buffer (%p start=%u len=%u)\n",
              fs, fs->rx_ooo_start, fs->rx_ooo_len);*/

          rx_bump += fs->rx_ooo_len;
          fs->rx_avail -= fs->rx_ooo_len;
          fs->rx_next_pos += fs->rx_ooo_len;
          if (fs->rx_next_pos >= fs->rx_len) {
            fs->rx_next_pos -= fs->rx_len;
          }
          //assert(fs->rx_next_pos < fs->rx_len);
          fs->rx_next_seq += fs->rx_ooo_len;

          fs->rx_ooo_len = 0;
        }
      }
    }
#endif
  }

  if ((_tcph_flags & TCP_FIN) == TCP_FIN &&
      !(fs->rx_base_sp & FLEXNIC_PL_FLOWST_RXFIN))
  {
    ALIVE_CHECK();
#ifdef FFPACKET_STATS
    add_stat(&ctx->ffp_pos[12], 1);
#endif
    if (fs->rx_next_seq== f_beui32(p->tcp.seqno) + orig_payload && !fs->rx_ooo_len) {
      ALIVE_CHECK();
      fin_bump = 1;
      fs->rx_base_sp = fs->rx_base_sp | FLEXNIC_PL_FLOWST_RXFIN; // writing access
      /* FIN takes up sequence number space */
      fs->rx_next_seq++;
      trigger_ack = 1;
    } else {
      ALIVE_CHECK();
      fprintf(stderr, "fast_flows_packet: ignored fin because out of order\n");
    }
  }

unlock:
  /* if we bumped at least one, then we need to add a notification to the
   * queue */
  if (LIKELY(rx_bump != 0 || tx_bump != 0 || fin_bump)) {
    ALIVE_CHECK();
#if PL_DEBUG_ARX
    fprintf(stderr, "dma_krx_pkt_fastpath: updating application state\n");
#endif

    uint16_t type;
    type = FLEXTCP_PL_ARX_CONNUPDATE;

    if (fin_bump) {
      type |= FLEXTCP_PL_ARX_FLRXDONE << 8;
    }

#ifdef FLEXNIC_TRACING
    struct flextcp_pl_trev_arx te_arx = {
        .opaque = fs->opaque,
        .rx_bump = rx_bump,
        .tx_bump = tx_bump,
        .rx_pos = rx_pos,
        .flags = type,

        .flow_id = flow_id,
        .db_id = fs->db_id,

        .local_ip = f_beui32(p->ip.dest),
        .remote_ip = f_beui32(p->ip.src),
        .local_port = f_beui16(p->tcp.dest),
        .remote_port = f_beui16(p->tcp.src),
      };
    trace_event(FLEXNIC_PL_TREV_ARX, sizeof(te_arx), &te_arx);
#endif

    arx_cache_add(ctx, fs->db_id, fs->opaque, rx_bump, rx_pos, tx_bump, type);
  }

  /* Flow control: More receiver space? -> might need to start sending */
  new_avail = tcp_txavail(fs, NULL);
  if (new_avail > old_avail) {
    /* update qman queue */
    if (qman_set(&ctx->qman, flow_id, fs->tx_rate, new_avail -
          old_avail, TCP_MSS, QMAN_SET_RATE | QMAN_SET_MAXCHUNK
          | QMAN_ADD_AVAIL) != 0)
    {
      ALIVE_CHECK();
      fprintf(stderr, "fast_flows_packet: qman_set 1 failed, UNEXPECTED\n");
      abort();
    }
  }

  /* if we need to send an ack, also send packet to TX pipeline to do so */
  if (trigger_ack) {
    flow_tx_ack(ctx, fs->tx_next_seq, fs->rx_next_seq, fs->rx_avail,
        fs->tx_next_ts, ts, nbh, opts->ts);
  }

  //fs_unlock(fs);
  return trigger_ack;

slowpath:
  if (!no_permanent_sp) {
    fs->rx_base_sp = fs->rx_base_sp | FLEXNIC_PL_FLOWST_SLOWPATH; // writing access
  }

  //fs_unlock(fs);
  /* TODO: should pass current flow state to kernel as well */
  return -1;
}

/* Update receive and transmit queue pointers from application */
#pragma vectorize alive_check
int fast_flows_bump(struct dataplane_context *ctx, uint32_t flow_id,
    uint16_t bump_seq, uint32_t rx_bump, uint32_t tx_bump, uint8_t flags,
    struct network_buf_handle *nbh, uint32_t ts)
{
  struct flextcp_pl_flowst *fs = &fp_state->flowst[flow_id];
  uint32_t rx_avail_prev, old_avail, new_avail, tx_avail;
  int ret = -1;

  fs_lock(fs);
#ifdef FLEXNIC_TRACING
  struct flextcp_pl_trev_atx te_atx = {
      .rx_bump = rx_bump,
      .tx_bump = tx_bump,
      .bump_seq_ent = bump_seq,
      .bump_seq_flow = fs->bump_seq,
      .flags = flags,

      .local_ip = f_beui32(fs->local_ip),
      .remote_ip = f_beui32(fs->remote_ip),
      .local_port = f_beui16(fs->local_port),
      .remote_port = f_beui16(fs->remote_port),

      .flow_id = flow_id,
      .db_id = fs->db_id,

      .tx_next_pos = fs->tx_next_pos,
      .tx_next_seq = fs->tx_next_seq,
      .tx_avail_prev = fs->tx_avail,
      .rx_next_pos = fs->rx_next_pos,
      .rx_avail = fs->rx_avail,
      .tx_len = fs->tx_len,
      .rx_len = fs->rx_len,
      .rx_remote_avail = fs->rx_remote_avail,
      .tx_sent = fs->tx_sent,
    };
  trace_event(FLEXNIC_PL_TREV_ATX, sizeof(te_atx), &te_atx);
#endif

  /* TODO: is this still necessary? */
  /* catch out of order bumps */
  if ((bump_seq >= fs->bump_seq &&
        bump_seq - fs->bump_seq > (UINT16_MAX / 2)) ||
      (bump_seq < fs->bump_seq &&
       (fs->bump_seq < ((UINT16_MAX / 4) * 3) ||
       bump_seq > (UINT16_MAX / 4))))
  {
    goto unlock;
  }
  fs->bump_seq = bump_seq;

  if ((fs->rx_base_sp & FLEXNIC_PL_FLOWST_TXFIN) == FLEXNIC_PL_FLOWST_TXFIN &&
      tx_bump != 0)
  {
    ALIVE_CHECK();
    /* TX already closed, don't accept anything for transmission */
    fprintf(stderr, "fast_flows_bump: tx bump while TX is already closed\n");
    tx_bump = 0;
  } else if ((flags & FLEXTCP_PL_ATX_FLTXDONE) == FLEXTCP_PL_ATX_FLTXDONE &&
      !(fs->rx_base_sp & FLEXNIC_PL_FLOWST_TXFIN) &&
      !tx_bump)
  {
    ALIVE_CHECK();
    /* Closing TX requires at least one byte (dummy) */
    fprintf(stderr, "fast_flows_bump: tx eos without dummy byte\n");
    goto unlock;
  }

  tx_avail = fs->tx_avail + tx_bump;

  /* validate tx bump */
  if (tx_bump > fs->tx_len || tx_avail > fs->tx_len ||
      tx_avail + fs->tx_sent > fs->tx_len)
  {
    ALIVE_CHECK();
    fprintf(stderr, "fast_flows_bump: tx bump too large\n");
    goto unlock;
  }
  /* validate rx bump */
  if (rx_bump > fs->rx_len || rx_bump + fs->rx_avail > fs->tx_len) {
    ALIVE_CHECK();
    fprintf(stderr, "fast_flows_bump: rx bump too large\n");
    goto unlock;
  }
  /* calculate how many bytes can be sent before and after this bump */
  old_avail = tcp_txavail(fs, NULL);
  new_avail = tcp_txavail(fs, &tx_avail);

  /* mark connection as closed if requested */
  if ((flags & FLEXTCP_PL_ATX_FLTXDONE) == FLEXTCP_PL_ATX_FLTXDONE &&
      !(fs->rx_base_sp & FLEXNIC_PL_FLOWST_TXFIN))
  {
    fs->rx_base_sp |= FLEXNIC_PL_FLOWST_TXFIN;
  }

  /* update queue manager queue */
  if (old_avail < new_avail) {
    if (qman_set(&ctx->qman, flow_id, fs->tx_rate, new_avail -
          old_avail, TCP_MSS, QMAN_SET_RATE | QMAN_SET_MAXCHUNK
          | QMAN_ADD_AVAIL) != 0)
    {
      ALIVE_CHECK();
      fprintf(stderr, "flast_flows_bump: qman_set 1 failed, UNEXPECTED\n");
      abort();
    }
  }

  /* update flow state */
  fs->tx_avail = tx_avail;
  rx_avail_prev = fs->rx_avail;
  fs->rx_avail += rx_bump;

  /* receive buffer freed up from empty, need to send out a window update, if
   * we're not sending anyways. */
  if (new_avail == 0 && rx_avail_prev == 0 && fs->rx_avail != 0) {
    flow_tx_segment_modified(ctx, nbh, fs, fs->tx_next_seq, fs->rx_next_seq,
        fs->rx_avail, 0, 0, fs->tx_next_ts, ts, 0);
    ret = 0;
  }

unlock:
  fs_unlock(fs);
  return ret;
}

/* start retransmitting */
#pragma vectorize alive_check
void fast_flows_retransmit(struct dataplane_context *ctx, uint32_t flow_id)
{
  struct flextcp_pl_flowst *fs = &fp_state->flowst[flow_id];
  uint32_t old_avail, new_avail = -1;

  fs_lock(fs);

#ifdef FLEXNIC_TRACING
    struct flextcp_pl_trev_rexmit te_rexmit = {
        .flow_id = flow_id,
        .tx_avail = fs->tx_avail,
        .tx_sent = fs->tx_sent,
        .tx_next_pos = fs->tx_next_pos,
        .tx_next_seq = fs->tx_next_seq,
        .rx_remote_avail = fs->rx_remote_avail,
      };
    trace_event(FLEXNIC_PL_TREV_REXMIT, sizeof(te_rexmit), &te_rexmit);
#endif


  /*    uint32_t old_head = fs->tx_head;
      uint32_t old_sent = fs->tx_sent;
      uint32_t old_pos = fs->tx_next_pos;*/

  old_avail = tcp_txavail(fs, NULL);

  if (fs->tx_sent == 0) {
    /*fprintf(stderr, "fast_flows_retransmit: tx sent == 0\n");

      fprintf(stderr, "fast_flows_retransmit: "
          "old_avail=%u new_avail=%u head=%u tx_next_seq=%u old_head=%u "
          "old_sent=%u old_pos=%u new_pos=%u\n", old_avail, new_avail,
          fs->tx_head, fs->tx_next_seq, old_head, old_sent, old_pos,
          fs->tx_next_pos);*/
    goto out;
  }


  flow_reset_retransmit(fs);
  new_avail = tcp_txavail(fs, NULL);

  /*    fprintf(stderr, "fast_flows_retransmit: "
          "old_avail=%u new_avail=%u head=%u tx_next_seq=%u old_head=%u "
          "old_sent=%u old_pos=%u new_pos=%u\n", old_avail, new_avail,
          fs->tx_head, fs->tx_next_seq, old_head, old_sent, old_pos,
          fs->tx_next_pos);*/

  /* update queue manager */
  if (new_avail > old_avail) {
    if (qman_set(&ctx->qman, flow_id, fs->tx_rate, new_avail - old_avail,
          TCP_MSS, QMAN_SET_RATE | QMAN_SET_MAXCHUNK | QMAN_ADD_AVAIL) != 0)
    {
      ALIVE_CHECK();
      fprintf(stderr, "flast_flows_bump: qman_set 1 failed, UNEXPECTED\n");
      abort();
    }
  }

out:
  fs_unlock(fs);
  return;
}

/* read `len` bytes from position `pos` in cirucular transmit buffer */
#pragma vectorize to_scalar
static void flow_tx_read(struct flextcp_pl_flowst *fs, uint32_t pos,
    uint16_t len, void *dst)
{
  uint32_t part;

  if (LIKELY(pos + len <= fs->tx_len)) {
    dma_read(fs->tx_base + pos, len, dst);
  } else {
    part = fs->tx_len - pos;
    dma_read(fs->tx_base + pos, part, dst);
    dma_read(fs->tx_base, len - part, (uint8_t *) dst + part);
  }
}

/* write `len` bytes to position `pos` in cirucular receive buffer */
#pragma vectorize transposed(fs)
static void flow_rx_write(struct flextcp_pl_flowst *fs, uint32_t pos,
    uint16_t len, const void *src)
{
  uint32_t part;
  uint64_t rx_base = fs->rx_base_sp & FLEXNIC_PL_FLOWST_RX_MASK;

  if (LIKELY(pos + len <= fs->rx_len)) {
    dma_write(rx_base + pos, len, src);
  } else {
    ALIVE_CHECK();
    part = fs->rx_len - pos;
    dma_write(rx_base + pos, part, src);
    dma_write(rx_base, len - part, (const uint8_t *) src + part);
  }
}

#ifdef FLEXNIC_PL_OOO_RECV
#pragma vectorize alive_check transposed(fs)
static void flow_rx_seq_write(struct flextcp_pl_flowst *fs, uint32_t seq,
    uint16_t len, const void *src)
{
  uint32_t diff = seq - fs->rx_next_seq;
  uint32_t pos = fs->rx_next_pos + diff;
  if (pos >= fs->rx_len)
    pos -= fs->rx_len;
  //assert(pos < fs->rx_len);
  flow_rx_write(fs, pos, len, src);
}
#endif

#pragma vectorize to_scalar
static void flow_tx_segment(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, struct flextcp_pl_flowst *fs,
    uint32_t seq, uint32_t ack, uint32_t rxwnd, uint16_t payload,
    uint32_t payload_pos, uint32_t ts_echo, uint32_t ts_my, uint8_t fin)
{
  uint16_t hdrs_len, optlen, fin_fl;
  struct pkt_tcp *p = network_buf_buf(nbh);
  struct tcp_timestamp_opt *opt_ts;

  /* calculate header length depending on options */
  optlen = (sizeof(*opt_ts) + 3) & ~3;
  hdrs_len = sizeof(*p) + optlen;

  /* fill headers */
  p->eth.dest = fs->remote_mac;
  memcpy(&p->eth.src, &eth_addr, ETH_ADDR_LEN);
  p->eth.type = t_beui16(ETH_TYPE_IP);

  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(hdrs_len - offsetof(struct pkt_tcp, ip) + payload);
  p->ip.id = t_beui16(3); /* TODO: not sure why we have 3 here */
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_TCP;
  p->ip.chksum = 0;
  p->ip.src = fs->local_ip;
  p->ip.dest = fs->remote_ip;

  /* mark as ECN capable if flow marked so */
  if ((fs->rx_base_sp & FLEXNIC_PL_FLOWST_ECN) == FLEXNIC_PL_FLOWST_ECN) {
    IPH_ECN_SET(&p->ip, IP_ECN_ECT0);
  }

  fin_fl = (fin ? TCP_FIN : 0);

  p->tcp.src = fs->local_port;
  p->tcp.dest = fs->remote_port;
  p->tcp.seqno = t_beui32(seq);
  p->tcp.ackno = t_beui32(ack);
  TCPH_HDRLEN_FLAGS_SET(&p->tcp, 5 + optlen / 4, TCP_PSH | TCP_ACK | fin_fl);
  p->tcp.wnd = t_beui16(MIN(0xFFFF, rxwnd));
  p->tcp.chksum = 0;
  p->tcp.urgp = t_beui16(0);

  /* fill in timestamp option */
  memset(p + 1, 0, optlen);
  opt_ts = (struct tcp_timestamp_opt *) (p + 1);
  opt_ts->kind = TCP_OPT_TIMESTAMP;
  opt_ts->length = sizeof(*opt_ts);
  opt_ts->ts_val = t_beui32(ts_my);
  opt_ts->ts_ecr = t_beui32(ts_echo);

  /* add payload if requested */
  if (payload > 0) {
    flow_tx_read(fs, payload_pos, payload, (uint8_t *) p + hdrs_len);
  }

  /* checksums */
  tcp_checksums(nbh, p, fs->local_ip, fs->remote_ip, hdrs_len - offsetof(struct
        pkt_tcp, tcp) + payload);

#ifdef FLEXNIC_TRACING
  struct flextcp_pl_trev_txseg te_txseg = {
      .local_ip = f_beui32(p->ip.src),
      .remote_ip = f_beui32(p->ip.dest),
      .local_port = f_beui16(p->tcp.src),
      .remote_port = f_beui16(p->tcp.dest),

      .flow_seq = seq,
      .flow_ack = ack,
      .flow_flags = TCPH_FLAGS(&p->tcp),
      .flow_len = payload,
    };
  trace_event(FLEXNIC_PL_TREV_TXSEG, sizeof(te_txseg), &te_txseg);
#endif

  tx_send(ctx, nbh, 0, hdrs_len + payload);
}

#pragma vectorize to_scalar
static void flow_tx_ack(struct dataplane_context *ctx, uint32_t seq,
    uint32_t ack, uint32_t rxwnd, uint32_t echots, uint32_t myts,
    struct network_buf_handle *nbh, struct tcp_timestamp_opt *ts_opt)
{
  struct pkt_tcp *p;
  struct eth_addr eth;
  ip_addr_t ip;
  beui16_t port;
  uint16_t hdrlen;
  uint16_t ecn_flags = 0;

  p = network_buf_bufoff(nbh);

#if PL_DEBUG_TCPACK
  fprintf(stderr, "FLOW local=%08x:%05u remote=%08x:%05u ACK: seq=%u ack=%u\n",
      f_beui32(p->ip.dest), f_beui16(p->tcp.dest),
      f_beui32(p->ip.src), f_beui16(p->tcp.src), seq, ack);
#endif

  /* swap addresses */
  eth = p->eth.src;
  p->eth.src = p->eth.dest;
  p->eth.dest = eth;
  ip = p->ip.src;
  p->ip.src = p->ip.dest;
  p->ip.dest = ip;
  port = p->tcp.src;
  p->tcp.src = p->tcp.dest;
  p->tcp.dest = port;

  hdrlen = sizeof(*p) + (TCPH_HDRLEN(&p->tcp) - 5) * 4;

  /* If ECN flagged, set TCP response flag */
  if (IPH_ECN(&p->ip) == IP_ECN_CE) {
    ecn_flags = TCP_ECE;
  }

  /* mark ACKs as ECN in-capable */
  IPH_ECN_SET(&p->ip, IP_ECN_NONE);

  /* change TCP header to ACK */
  p->tcp.seqno = t_beui32(seq);
  p->tcp.ackno = t_beui32(ack);
  TCPH_HDRLEN_FLAGS_SET(&p->tcp, TCPH_HDRLEN(&p->tcp), TCP_ACK | ecn_flags);
  p->tcp.wnd = t_beui16(MIN(0xFFFF, rxwnd));
  p->tcp.urgp = t_beui16(0);

  /* fill in timestamp option */
  ts_opt->ts_val = t_beui32(myts);
  ts_opt->ts_ecr = t_beui32(echots);

  p->ip.len = t_beui16(hdrlen - offsetof(struct pkt_tcp, ip));
  p->ip.ttl = 0xff;

  /* checksums */
  tcp_checksums(nbh, p, p->ip.src, p->ip.dest, hdrlen - offsetof(struct
        pkt_tcp, tcp));

#ifdef FLEXNIC_TRACING
  struct flextcp_pl_trev_txack te_txack = {
      .local_ip = f_beui32(p->ip.src),
      .remote_ip = f_beui32(p->ip.dest),
      .local_port = f_beui16(p->tcp.src),
      .remote_port = f_beui16(p->tcp.dest),

      .flow_seq = seq,
      .flow_ack = ack,
      .flow_flags = TCPH_FLAGS(&p->tcp),
    };
  trace_event(FLEXNIC_PL_TREV_TXACK, sizeof(te_txack), &te_txack);
#endif

  tx_send(ctx, nbh, network_buf_off(nbh), hdrlen);
}

#pragma vectorize alive_check transposed(fs)
static void flow_reset_retransmit(struct flextcp_pl_flowst *fs)
{
  uint32_t x;

  /* reset flow state as if we never transmitted those segments */
  fs->rx_dupack_cnt = 0;

  fs->tx_next_seq -= fs->tx_sent;
  if (fs->tx_next_pos >= fs->tx_sent) {
    fs->tx_next_pos -= fs->tx_sent;
  } else {
    x = fs->tx_sent - fs->tx_next_pos;
    fs->tx_next_pos = fs->tx_len - x;
  }
  fs->tx_avail += fs->tx_sent;
  fs->rx_remote_avail += fs->tx_sent;
  fs->tx_sent = 0;

  /* cut rate by half if first drop in control interval */
  if (fs->cnt_tx_drops == 0) {
    fs->tx_rate /= 2;
  }

  fs->cnt_tx_drops++;
}

#pragma vectorize to_scalar
static inline void tcp_checksums(struct network_buf_handle *nbh,
    struct pkt_tcp *p, beui32_t ip_s, beui32_t ip_d, uint16_t l3_paylen)
{
  p->ip.chksum = 0;
  if (config.fp_xsumoffload) {
    p->tcp.chksum = tx_xsum_enable(nbh, &p->ip, ip_s, ip_d, l3_paylen);
  } else {
    p->tcp.chksum = 0;
    p->ip.chksum = rte_ipv4_cksum((void *) &p->ip);
    p->tcp.chksum = rte_ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->tcp);
  }
}

void fast_flows_kernelxsums(struct network_buf_handle *nbh,
    struct pkt_tcp *p)
{
  tcp_checksums(nbh, p, p->ip.src, p->ip.dest,
      f_beui16(p->ip.len) - sizeof(p->ip));
}


static inline uint32_t flow_hash(struct flow_key *k)
{
  return crc32c_sse42_u32(k->local_port | (((uint32_t) k->remote_port) << 16),
      crc32c_sse42_u64(k->local_ip | (((uint64_t) k->remote_ip) << 32), 0));
}

#pragma vectorize
static inline uint32_t flow_hash_new(beui32_t local_ip, beui32_t remote_ip, beui16_t local_port, beui16_t remote_port) {
  uint32_t hash = 13;
  hash = 31 * hash + local_ip; // TODO: opt with shift and subtraction? also remove first multiplication
  hash = 31 * hash + remote_ip;
  hash = 31 * hash + local_port;
  hash = 31 * hash + remote_port;
  return hash;
}

#pragma vectorize to_scalar
static inline uint32_t flow_hash_and_double_prefetch(struct flow_key *k) {
  uint32_t hash = flow_hash(k);
  rte_prefetch0(&fp_state->flowht[hash % FLEXNIC_PL_FLOWHT_ENTRIES]);
  rte_prefetch0(&fp_state->flowht[(hash + 3) % FLEXNIC_PL_FLOWHT_ENTRIES]);
  return hash;
}

#pragma vectorize alive_check
uint32_t fast_flows_packet_fss1(struct network_buf_handle *nbhs) {
  struct pkt_tcp *p;
  uint32_t hash;
  p = network_buf_bufoff(nbhs);
  
  //hash = flow_hash(&key);
  hash =  flow_hash_new(p->ip.dest, p->ip.src, p->tcp.dest, p->tcp.src);
  rte_prefetch0(&fp_state->flowht[hash % FLEXNIC_PL_FLOWHT_ENTRIES]);
  rte_prefetch0(&fp_state->flowht[(hash + 3) % FLEXNIC_PL_FLOWHT_ENTRIES]);
  return hash;
}

#pragma vectorize alive_check
void fast_flows_packet_fss2(uint32_t hash) {
  uint32_t k, j, eh, fid, ffid;
  struct flextcp_pl_flowhte *e;
  for (j = 0; j < FLEXNIC_PL_FLOWHT_NBSZ; j++) {
    k = (hash + j) % FLEXNIC_PL_FLOWHT_ENTRIES;
    e = &fp_state->flowht[k];

    ffid = e->flow_id;
    //MEM_BARRIER();
    eh = e->flow_hash;

    fid = ffid & ((1 << FLEXNIC_PL_FLOWHTE_POSSHIFT) - 1);
    if ((ffid & FLEXNIC_PL_FLOWHTE_VALID) == 0 || eh != hash) {
      continue;
    }
    rte_prefetch0(&fp_state->flowst[fid]);
  }
}

#pragma vectorize alive_check
void fast_flows_packet_fss3(struct network_buf_handle *nbhs, void **fss, uint32_t hash) {
  uint32_t k, j, eh, fid, ffid;
  struct pkt_tcp *p;
  struct flextcp_pl_flowhte *e;
  struct flextcp_pl_flowst *fs;

  p = network_buf_bufoff(nbhs);
  *fss = NULL;

  for (j = 0; j < FLEXNIC_PL_FLOWHT_NBSZ; j++) {
    k = (hash + j) % FLEXNIC_PL_FLOWHT_ENTRIES;
    e = &fp_state->flowht[k];

    ffid = e->flow_id; // L1 misses
    //MEM_BARRIER();
    eh = e->flow_hash; // L1 misses

    fid = ffid & ((1 << FLEXNIC_PL_FLOWHTE_POSSHIFT) - 1);
    if ((ffid & FLEXNIC_PL_FLOWHTE_VALID) == 0 || eh != hash) {
      continue;
    }

    //MEM_BARRIER();
    fs = &fp_state->flowst[fid];
    if ((fs->local_ip == p->ip.dest) & // L1 misses for first access
        (fs->remote_ip == p->ip.src) &
        (fs->local_port == p->tcp.dest) &
        (fs->remote_port == p->tcp.src))
    {
      //rte_prefetch0((uint8_t *) fs + 64);
      *fss = &fp_state->flowst[fid];
      break;
    }
  }
}

void fast_flows_packet_fss(struct dataplane_context *ctx,
    struct network_buf_handle **nbhs, void **fss, uint16_t n)
{
  uint32_t hashes[n];
  uint32_t h, k, j, eh, fid, ffid;
  uint16_t i;
  struct pkt_tcp *p;
  //struct flow_key key;
  struct flextcp_pl_flowhte *e;
  struct flextcp_pl_flowst *fs;

  /* calculate hashes and prefetch hash table buckets */
  for (i = 0; i < n; i++) {
    p = network_buf_bufoff(nbhs[i]);

    /*
    key.local_ip = p->ip.dest;
    key.remote_ip = p->ip.src;
    key.local_port = p->tcp.dest;
    key.remote_port = p->tcp.src;
    */
    h = flow_hash_new(p->ip.dest, p->ip.src, p->tcp.dest, p->tcp.src);

    rte_prefetch0(&fp_state->flowht[h % FLEXNIC_PL_FLOWHT_ENTRIES]);
    rte_prefetch0(&fp_state->flowht[(h + 3) % FLEXNIC_PL_FLOWHT_ENTRIES]);
    hashes[i] = h;
  }

  /* prefetch flow state for buckets with matching hashes
   * (usually 1 per packet, except in case of collisions) */
  for (i = 0; i < n; i++) {
    h = hashes[i];
    for (j = 0; j < FLEXNIC_PL_FLOWHT_NBSZ; j++) {
      k = (h + j) % FLEXNIC_PL_FLOWHT_ENTRIES;
      e = &fp_state->flowht[k];

      ffid = e->flow_id;
      MEM_BARRIER();
      eh = e->flow_hash;

      fid = ffid & ((1 << FLEXNIC_PL_FLOWHTE_POSSHIFT) - 1);
      if ((ffid & FLEXNIC_PL_FLOWHTE_VALID) == 0 || eh != h) {
        continue;
      }

      rte_prefetch0(&fp_state->flowst[fid]);
    }
  }

  /* finish hash table lookup by checking 5-tuple in flow state */
  for (i = 0; i < n; i++) {
    p = network_buf_bufoff(nbhs[i]);
    fss[i] = NULL;
    h = hashes[i];

    for (j = 0; j < FLEXNIC_PL_FLOWHT_NBSZ; j++) {
      k = (h + j) % FLEXNIC_PL_FLOWHT_ENTRIES;
      e = &fp_state->flowht[k];

      ffid = e->flow_id;
      MEM_BARRIER();
      eh = e->flow_hash;

      fid = ffid & ((1 << FLEXNIC_PL_FLOWHTE_POSSHIFT) - 1);
      if ((ffid & FLEXNIC_PL_FLOWHTE_VALID) == 0 || eh != h) {
        continue;
      }

      MEM_BARRIER();
      fs = &fp_state->flowst[fid];
      if ((fs->local_ip == p->ip.dest) &
          (fs->remote_ip == p->ip.src) &
          (fs->local_port == p->tcp.dest) &
          (fs->remote_port == p->tcp.src))
      {
        rte_prefetch0((uint8_t *) fs + 64);
        fss[i] = &fp_state->flowst[fid];
        break;
      }
    }
  }
}


#pragma vectorize alive_check
void fast_flows_packet_fss_m(struct dataplane_context *ctx,
    struct network_buf_handle *nbhs, void **fss)
{
  uint32_t hash;
  uint32_t k, j, eh, fid, ffid;
  struct pkt_tcp *p;
  struct flextcp_pl_flowhte *e;
  struct flextcp_pl_flowst *fs;

  /* calculate hashes and prefetch hash table buckets */
  //for (i = 0; i < n; i++) {
    p = network_buf_bufoff(nbhs);
  beui32_t p_ip_dest = p->ip.dest;
  beui32_t p_ip_src = p->ip.src;
  beui16_t p_tcp_dest = p->tcp.dest;
  beui16_t p_tcp_src = p->tcp.src;

    //hash = flow_hash(&key);
    hash = flow_hash_new(p_ip_dest, p_ip_src, p_tcp_dest, p_tcp_src);

    //rte_prefetch0(&fp_state->flowht[hash % FLEXNIC_PL_FLOWHT_ENTRIES]);
    //rte_prefetch0(&fp_state->flowht[(hash + 3) % FLEXNIC_PL_FLOWHT_ENTRIES]);
    //hash = flow_hash_and_double_prefetch(&key);
    //hash = h;
  //}

  /* prefetch flow state for buckets with matching hashes
   * (usually 1 per packet, except in case of collisions) */
  //for (i = 0; i < n; i++) {
    //h = hashes[i];
    /*
    for (j = 0; j < FLEXNIC_PL_FLOWHT_NBSZ; j++) {
      k = (hash + j) % FLEXNIC_PL_FLOWHT_ENTRIES;
      e = &fp_state->flowht[k];

      ffid = e->flow_id;
      //MEM_BARRIER();
      eh = e->flow_hash;

      fid = ffid & ((1 << FLEXNIC_PL_FLOWHTE_POSSHIFT) - 1);
      if ((ffid & FLEXNIC_PL_FLOWHTE_VALID) == 0 || eh != hash) {
        continue;
      }
      rte_prefetch0(&fp_state->flowst[fid]);
    }
    */
  //}

  /* finish hash table lookup by checking 5-tuple in flow state */
  //for (i = 0; i < n; i++) {
    //p = network_buf_bufoff(nbhs);

    *fss = NULL;

    for (j = 0; j < FLEXNIC_PL_FLOWHT_NBSZ; j++) {
      k = (hash + j) % FLEXNIC_PL_FLOWHT_ENTRIES;
      e = &fp_state->flowht[k];

      ffid = e->flow_id; // L1 misses
      //MEM_BARRIER();
      eh = e->flow_hash; // L1 misses

      fid = ffid & ((1 << FLEXNIC_PL_FLOWHTE_POSSHIFT) - 1);
      if ((ffid & FLEXNIC_PL_FLOWHTE_VALID) == 0 || eh != hash) {
        continue;
      }

      //MEM_BARRIER();
      fs = &fp_state->flowst[fid];
      if ((fs->local_ip == p_ip_dest) & // L1 misses for first access
          (fs->remote_ip == p_ip_src) &
          (fs->local_port == p_tcp_dest) &
          (fs->remote_port == p_tcp_src))
      {
        //rte_prefetch0((uint8_t *) fs + 64);
        *fss = &fp_state->flowst[fid];
        break;
      }
    }
  //}
}
