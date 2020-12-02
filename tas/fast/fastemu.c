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

#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

#include <intrinhelper.h>

#include <tas_memif.h>

#include "internal.h"
#include "fastemu.h"

#define DATAPLANE_TSCS

#ifdef DATAPLANE_STATS
# ifdef DATAPLANE_TSCS
#   define STATS_TS(n) uint64_t n = rte_get_tsc_cycles()
#   define STATS_TSADD(c, f, n) __sync_fetch_and_add(&c->stat_##f, n)
# else
#   define STATS_TS(n) do { } while (0)
#   define STATS_TSADD(c, f, n) do { } while (0)
# endif
#   define STATS_ADD(c, f, n) __sync_fetch_and_add(&c->stat_##f, n)
#else
#   define STATS_TS(n) do { } while (0)
#   define STATS_TSADD(c, f, n) do { } while (0)
#   define STATS_ADD(c, f, n) do { } while (0)
#endif


static void dataplane_block(struct dataplane_context *ctx, uint32_t ts);
static unsigned poll_rx(struct dataplane_context *ctx, uint32_t ts,
    uint64_t tsc) __attribute__((noinline));
static unsigned poll_queues(struct dataplane_context *ctx, uint32_t ts)  __attribute__((noinline));
static unsigned poll_kernel(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_qman(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_qman_fwd(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static void poll_scale(struct dataplane_context *ctx);

static inline uint8_t bufcache_prealloc(struct dataplane_context *ctx, uint16_t num,
    struct network_buf_handle ***handles);
static inline void bufcache_alloc(struct dataplane_context *ctx, uint16_t num);
static inline void bufcache_free(struct dataplane_context *ctx,
    struct network_buf_handle *handle);

static inline void tx_flush(struct dataplane_context *ctx);
static inline void tx_send(struct dataplane_context *ctx,
    struct network_buf_handle *nbh, uint16_t off, uint16_t len);

static void arx_cache_flush(struct dataplane_context *ctx, uint64_t tsc) __attribute__((noinline));

int dataplane_init(void)
{
  if (FLEXNIC_INTERNAL_MEM_SIZE < sizeof(struct flextcp_pl_mem)) {
    fprintf(stderr, "dataplane_init: internal flexnic memory size not "
        "sufficient (got %x, need %zx)\n", FLEXNIC_INTERNAL_MEM_SIZE,
        sizeof(struct flextcp_pl_mem));
    return -1;
  }

  if (fp_cores_max > FLEXNIC_PL_APPST_CTX_MCS) {
    fprintf(stderr, "dataplane_init: more cores than FLEXNIC_PL_APPST_CTX_MCS "
        "(%u)\n", FLEXNIC_PL_APPST_CTX_MCS);
    return -1;
  }
  if (FLEXNIC_PL_FLOWST_NUM > FLEXNIC_NUM_QMQUEUES) {
    fprintf(stderr, "dataplane_init: more flow states than queue manager queues"
        "(%u > %u)\n", FLEXNIC_PL_FLOWST_NUM, FLEXNIC_NUM_QMQUEUES);
    return -1;
  }

  return 0;
}

int dataplane_context_init(struct dataplane_context *ctx)
{
  char name[32];

  /* initialize forwarding queue */
  sprintf(name, "qman_fwd_ring_%u", ctx->id);
  if ((ctx->qman_fwd_ring = rte_ring_create(name, 32 * 1024, rte_socket_id(),
          RING_F_SC_DEQ)) == NULL)
  {
    fprintf(stderr, "initializing rte_ring_create");
    return -1;
  }

  /* initialize queue manager */
  if (qman_thread_init(ctx) != 0) {
    fprintf(stderr, "initializing qman thread failed\n");
    return -1;
  }

  /* initialize network queue */
  if (network_thread_init(ctx) != 0) {
    fprintf(stderr, "initializing rx thread failed\n");
    return -1;
  }

  ctx->poll_next_ctx = ctx->id;

  ctx->evfd = eventfd(0, EFD_NONBLOCK);
  assert(ctx->evfd != -1);
  ctx->ev.epdata.event = EPOLLIN;
  int r = rte_epoll_ctl(RTE_EPOLL_PER_THREAD, EPOLL_CTL_ADD, ctx->evfd, &ctx->ev);
  assert(r == 0);
  fp_state->kctx[ctx->id].evfd = ctx->evfd;

#ifdef DEBUG_STATS
  memset(ctx->packets_queued, 0, sizeof(uint64_t) * 16);
  memset(ctx->packets_processed, 0, sizeof(uint64_t) * 8);
#endif
#ifdef FFPACKET_STATS
  memset(ctx->ffp_pos, 0, sizeof(uint64_t) * 16);
#endif

  return 0;
}

void dataplane_context_destroy(struct dataplane_context *ctx)
{
}

void dataplane_loop(struct dataplane_context *ctx)
{
  struct notify_blockstate nbs;
  uint32_t ts;
  uint64_t cyc, prev_cyc;
  int was_idle = 1;

  notify_canblock_reset(&nbs);
  while (!exited) {
    unsigned n = 0;

    /* count cycles of previous iteration if it was busy */
    prev_cyc = cyc;
    cyc = rte_get_tsc_cycles();
#ifdef DEBUG_STATS
    if (prev_cyc / DEBUG_PRINT_INTERVAL != cyc / DEBUG_PRINT_INTERVAL) {
      fprintf(stderr, "debug stats: queue: ");
      for (unsigned i = 0; i < 16; i++) {
        if (i == 0) {
          fprintf(stderr, "%lu", ctx->packets_queued[i]);
        } else {
          fprintf(stderr, "/ %lu", ctx->packets_queued[i]);
        }
      }
      fprintf(stderr, "\ndebug stats: processed: ");
      for (unsigned i = 0; i < 8; i++) {
        if (i == 0) {
          fprintf(stderr, "%lu", ctx->packets_processed[i]);
        } else {
          fprintf(stderr, "/ %lu", ctx->packets_processed[i]);
        }
      }
      fprintf(stderr, "\n");
    }
#endif
#ifdef FFPACKET_STATS
    if (prev_cyc / DEBUG_PRINT_INTERVAL != cyc / DEBUG_PRINT_INTERVAL) {
      fprintf(stderr, "ffpacket stats: ");
      for (unsigned i = 0; i < 16; i++) {
        if (i == 0) {
          fprintf(stderr, "%lu", ctx->ffp_pos[i]);
        } else {
          fprintf(stderr, "/ %lu", ctx->ffp_pos[i]);
        }
      }
      fprintf(stderr, "\n");
    }
#endif
    if (!was_idle)
      ctx->loadmon_cyc_busy += cyc - prev_cyc;


    ts = qman_timestamp(cyc);

    STATS_TS(start);
    n += poll_rx(ctx, ts, cyc);
    STATS_TS(rx);
    tx_flush(ctx);

    n += poll_qman_fwd(ctx, ts);

    STATS_TSADD(ctx, cyc_rx, rx - start);
    n += poll_qman(ctx, ts);
    STATS_TS(qm);
    STATS_TSADD(ctx, cyc_qm, qm - rx);
    n += poll_queues(ctx, ts);
    STATS_TS(qs);
    STATS_TSADD(ctx, cyc_qs, qs - qm);
    n += poll_kernel(ctx, ts);

    /* flush transmit buffer */
    tx_flush(ctx);

    if (ctx->id == 0)
      poll_scale(ctx);

    was_idle = (n == 0);
    if (config.fp_interrupts && notify_canblock(&nbs, !was_idle, cyc)) {
      dataplane_block(ctx, ts);
      notify_canblock_reset(&nbs);
    }
  }
}

static void dataplane_block(struct dataplane_context *ctx, uint32_t ts)
{
  uint32_t max_timeout;
  uint64_t val;
  int ret, i;
  struct rte_epoll_event event[2];

  if (network_rx_interrupt_ctl(&ctx->net, 1) != 0) {
    return;
  }

  max_timeout = qman_next_ts(&ctx->qman, ts);

  ret = rte_epoll_wait(RTE_EPOLL_PER_THREAD, event, 2,
      max_timeout == (uint32_t) -1 ? -1 : max_timeout / 1000);
  if (ret < 0) {
    perror("dataplane_block: rte_epoll_wait failed");
    abort();
  }

  for(i = 0; i < ret; i++) {
    if(event[i].fd == ctx->evfd) {
      ret = read(ctx->evfd, &val, sizeof(uint64_t));
      if ((ret > 0 && ret != sizeof(uint64_t)) ||
          (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
      {
        perror("dataplane_block: read failed");
        abort();
      }
    }
  }
  network_rx_interrupt_ctl(&ctx->net, 0);
}

#ifdef DATAPLANE_STATS
static inline uint64_t read_stat(uint64_t *p)
{
  return __sync_lock_test_and_set(p, 0);
}

void dataplane_dump_stats(void)
{
  struct dataplane_context *ctx;
  unsigned i;

  for (i = 0; i < fp_cores_max; i++) {
    ctx = ctxs[i];
    fprintf(stderr, "dp stats %u: "
        "qm=(%"PRIu64",%"PRIu64",%"PRIu64")  "
        "rx=(%"PRIu64",%"PRIu64",%"PRIu64")  "
        "qs=(%"PRIu64",%"PRIu64",%"PRIu64")  "
        "cyc=(%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64")\n", i,
        read_stat(&ctx->stat_qm_poll), read_stat(&ctx->stat_qm_empty),
        read_stat(&ctx->stat_qm_total),
        read_stat(&ctx->stat_rx_poll), read_stat(&ctx->stat_rx_empty),
        read_stat(&ctx->stat_rx_total),
        read_stat(&ctx->stat_qs_poll), read_stat(&ctx->stat_qs_empty),
        read_stat(&ctx->stat_qs_total),
        read_stat(&ctx->stat_cyc_db), read_stat(&ctx->stat_cyc_qm),
        read_stat(&ctx->stat_cyc_rx), read_stat(&ctx->stat_cyc_qs));
  }
}
#endif

// The _mm512_popcnt_epi64 instruction is relatively new so we will not rely on it
static __m512i popcnt_512_64(__m512i vec) {
  unsigned long long data[8];
  unsigned long long ret[8];
  _mm512_storeu_epi64(data, vec);
  for (unsigned i = 0; i < 8; i++) {
    ret[i] = _mm_popcnt_u64(data[i]);
  }
  return _mm512_loadu_epi64(ret);
}

#pragma vectorize
void dummy() {
}

__m512i network_buf_bufoff_vec(__m512i, __mmask8);

static unsigned poll_rx(struct dataplane_context *ctx, uint32_t ts,
    uint64_t tsc)
{
  int ret;
  unsigned i, n;
  uint32_t freebuf[BATCH_SIZE] = { 0 };
  void *fss[BATCH_SIZE];
  struct tcp_opts tcpopts[BATCH_SIZE];
  struct network_buf_handle *bhs[BATCH_SIZE];

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < n)
    n = TXBUF_SIZE - ctx->tx_num;

  STATS_ADD(ctx, rx_poll, 1);

  /* receive packets */
  ret = network_poll(&ctx->net, n, bhs);
  if (ret <= 0) {
    STATS_ADD(ctx, rx_empty, 1);
    return 0;
  }
#ifdef DEBUG_STATS
  __sync_fetch_and_add(&ctx->packets_queued[ret - 1], 1);
#endif
  STATS_ADD(ctx, rx_total, n);
  n = ret;

  int n0 = n, n1 = 0;
  if (n > VEC_WIDTH) {
    n1 = n - VEC_WIDTH;
    n0 = VEC_WIDTH;
  }

  //fprintf(stderr, "Ok, here we go: %d\n", n);

  //for (unsigned j = 0; j < BATCH_SIZE; j++) {

    __m512i ctx_vec = _mm512_set1_epi64((uintptr_t) ctx);
    __m512i bhs_vec_0 = _mm512_loadu_epi64(bhs);
    __m512i bhs_vec_1 = _mm512_loadu_epi64(bhs + VEC_WIDTH);
    __m512i fss_vec_0 = _mm512_set_epi64((uintptr_t) (fss + 7), (uintptr_t) (fss + 6),
      (uintptr_t) (fss + 5), (uintptr_t) (fss + 4), (uintptr_t) (fss + 3),
      (uintptr_t) (fss + 2), (uintptr_t) (fss + 1), (uintptr_t) (fss + 0)  
    );
    __m512i fss_vec_1 = _mm512_set_epi64((uintptr_t) (fss + 15), (uintptr_t) (fss + 14),
      (uintptr_t) (fss + 13), (uintptr_t) (fss + 12), (uintptr_t) (fss + 11),
      (uintptr_t) (fss + 10), (uintptr_t) (fss + 9), (uintptr_t) (fss + 8)  
    );
    __m512i tcpopts_vec_0 = _mm512_set_epi64((uintptr_t) (tcpopts + 7), (uintptr_t) (tcpopts + 6),
      (uintptr_t) (tcpopts + 5), (uintptr_t) (tcpopts + 4), (uintptr_t) (tcpopts + 3),
      (uintptr_t) (tcpopts + 2), (uintptr_t) (tcpopts + 1), (uintptr_t) (tcpopts + 0)  
    );
    __m512i tcpopts_vec_1 = _mm512_set_epi64((uintptr_t) (tcpopts + 15), (uintptr_t) (tcpopts + 14),
      (uintptr_t) (tcpopts + 13), (uintptr_t) (tcpopts + 12), (uintptr_t) (tcpopts + 11),
      (uintptr_t) (tcpopts + 10), (uintptr_t) (tcpopts + 9), (uintptr_t) (tcpopts + 8)  
    );
    __mmask8 mask_0 = _cvtu32_mask8((1 << n0) - 1);
    __mmask8 mask_1 = _cvtu32_mask8((1 << n1) - 1);

  /* prefetch packet contents (1st cache line) */
  
    for (i = 0; i < n; i++) {
      rte_prefetch0(network_buf_bufoff(bhs[i]));
    }
    //rte_prefetch0_vec(network_buf_bufoff_vec(bhs_vec_0, mask_0), mask_0);
    //rte_prefetch0_vec(network_buf_bufoff_vec(bhs_vec_1, mask_1), mask_1);

  /* look up flow states */
  //fast_flows_packet_fss(ctx, bhs, fss, n);
    fast_flows_packet_fss_vec(ctx_vec, bhs_vec_0, fss_vec_0, mask_0);
    fast_flows_packet_fss_vec(ctx_vec, bhs_vec_1, fss_vec_1, mask_1);
    //printf("FSS: ");
    //d_print_512u(_mm512_mask_i64gather_epi64(_mm512_set1_epi64(1), mask, fss_vec, NULL, 1), mask);

  /* prefetch packet contents (2nd cache line, TS opt overlaps) */
  
    for (i = 0; i < n; i++) {
      rte_prefetch0(network_buf_bufoff(bhs[i]) + 64);
    }
    
    //rte_prefetch0_vec(_mm512_add_epi64(network_buf_bufoff_vec(bhs_vec_0, mask_0), _mm512_set1_epi64(64)), mask_0);
    //rte_prefetch0_vec(_mm512_add_epi64(network_buf_bufoff_vec(bhs_vec_1, mask_1), _mm512_set1_epi64(64)), mask_1);

  /* parse packets */
  //fast_flows_packet_parse(ctx, bhs, fss, tcpopts, n);
    fast_flows_packet_parse_vec(ctx_vec, bhs_vec_0, fss_vec_0, tcpopts_vec_0, mask_0);
    fast_flows_packet_parse_vec(ctx_vec, bhs_vec_1, fss_vec_1, tcpopts_vec_1, mask_1);
    //printf("FSS post parse: ");
    //d_print_512u(_mm512_mask_i64gather_epi64(_mm512_set1_epi64(1), mask, fss_vec, NULL, 1), mask);

  /*
    for (i = 0; i < n; i++) {
      rte_prefetch0(network_buf_bufoff(bhs[i]));
    }
    for (i = 0; i < n; i++) {
      fast_flows_packet_fss(ctx, bhs[i], &fss[i]);
    }
    for (i = 0; i < n; i++) {
      rte_prefetch0(network_buf_bufoff(bhs[i]) + 64);
    }
    for (i = 0; i < n; i++) {
      fast_flows_packet_parse(ctx, bhs[i], &fss[i], &tcpopts[i]);
    }
  
    for (i = 0; i < n; i++) {
      if (fss[i] != NULL) {
        ret = fast_flows_packet(ctx, bhs[i], fss[i], &tcpopts[i], ts);
      } else {
        ret = -1;
      }
      if (ret > 0) {
        freebuf[i] = 1;
      } else if (ret < 0) {
        fast_kernel_packet(ctx, bhs[i]);
      }
    }
    */

    __m512i fss_loaded_0 = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), mask_0, fss_vec_0, NULL, 1);
    __m512i fss_loaded_1 = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), mask_1, fss_vec_1, NULL, 1);
    __m512i conflicts_0 = _mm512_conflict_epi64(fss_loaded_0);
    __m512i conflicts_1 = _mm512_conflict_epi64(fss_loaded_1);
    __mmask8 masks[BATCH_SIZE];
    __mmask8 zeroes_0 = _mm512_cmpeq_epi64_mask(fss_loaded_0, _mm512_setzero_si512());
    __mmask8 zeroes_1 = _mm512_cmpeq_epi64_mask(fss_loaded_1, _mm512_setzero_si512());
    __mmask8 not_zeroes_0 = _knot_mask8(zeroes_0);
    __mmask8 not_zeroes_1 = _knot_mask8(zeroes_1);
    __m512i popcnt_0 = popcnt_512_64(conflicts_0);
    __m512i popcnt_1 = popcnt_512_64(conflicts_1);
    masks[0] = _kand_mask8(_kor_mask8(_mm512_cmpeq_epi64_mask(popcnt_0, _mm512_setzero_si512()), zeroes_0), mask_0);
    masks[8] = _kand_mask8(_kor_mask8(_mm512_cmpeq_epi64_mask(popcnt_1, _mm512_setzero_si512()), zeroes_1), mask_1);
    __mmask8 mask_and_notzero_0 = _kand_mask8(mask_0, not_zeroes_0);
    __mmask8 mask_and_notzero_1 = _kand_mask8(mask_1, not_zeroes_1);
    for (unsigned i = 1; i < 8; i++) {
      masks[i] = _kand_mask8(_mm512_cmpeq_epi64_mask(popcnt_0, _mm512_set1_epi64(i)), mask_and_notzero_0);
    }
    for (unsigned i = 9; i < 16; i++) {
      masks[i] = _kand_mask8(_mm512_cmpeq_epi64_mask(popcnt_1, _mm512_set1_epi64(i - 8)), mask_and_notzero_1);
    }

    for (i = 0; i < n; i++) {
      uint32_t maski = _cvtmask8_u32(masks[i]);
      if (maski == 0) {
        continue;
      } else if (_mm_popcnt_u32(maski) < 4) { // ok then
        int amnt = _mm_popcnt_u32(maski);
        uint32_t all_indices[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        uint32_t indices[8] = {0};
        __m256i vind = _mm256_loadu_epi32(all_indices);
        _mm256_mask_compressstoreu_epi32(indices, masks[i], vind);
        for (unsigned j = 0; j < amnt; j++) {
          uint32_t index = indices[j];
          if (i >= 8) {
            index += 8;
          }
          if (fss[index] != NULL) {
            ret = fast_flows_packet(ctx, bhs[index], fss[index], &tcpopts[index], ts);
          } else {
            ret = -1;
          }

          if (ret > 0) {
            freebuf[index] = 1;
          } else if (ret < 0) {
            fast_kernel_packet(ctx, bhs[index]);
          }
        }
        continue;
      }
#ifdef DEBUG_STATS
      __sync_fetch_and_add(&ctx->packets_processed[_mm_popcnt_u64(_cvtmask8_u32(masks[i])) - 1], 1);
#endif

      __m256i ret_vec;
      __mmask8 cmp;
      if (i >= 8) {
        cmp = _mm512_cmpneq_epi64_mask(_mm512_mask_i64gather_epi64(_mm512_undefined_epi32(), masks[i], fss_vec_1, NULL, 1), _mm512_set1_epi64(0));
      } else {
        cmp = _mm512_cmpneq_epi64_mask(_mm512_mask_i64gather_epi64(_mm512_undefined_epi32(), masks[i], fss_vec_0, NULL, 1), _mm512_set1_epi64(0));
      }
      __mmask8 if_mask = _kand_mask8(cmp, masks[i]);
      __m256i ts_vec = _mm256_set1_epi32(ts);
      /* run fast-path for flows with flow state */
      /*
      if (fss[i] != NULL) {
        ret = fast_flows_packet(ctx, bhs[i], fss[i], &tcpopts[i], ts);
      } else {
        ret = -1;
      }*/
      if (i >= 8) {
        ret_vec = _mm256_mask_mov_epi32(_mm256_set1_epi32(-1), if_mask,
          fast_flows_packet_vec(ctx_vec, bhs_vec_1,
            _mm512_mask_i64gather_epi64(_mm512_undefined_epi32(), if_mask, fss_vec_1, NULL, 1),
            tcpopts_vec_1, ts_vec, if_mask
          )
        );
      } else {
        ret_vec = _mm256_mask_mov_epi32(_mm256_set1_epi32(-1), if_mask,
          fast_flows_packet_vec(ctx_vec, bhs_vec_0,
            _mm512_mask_i64gather_epi64(_mm512_undefined_epi32(), if_mask, fss_vec_0, NULL, 1),
            tcpopts_vec_0, ts_vec, if_mask
          )
        );
      }

      cmp = _mm256_cmpgt_epi32_mask(ret_vec, _mm256_set1_epi32(0));
      if_mask = _kand_mask8(cmp, masks[i]);
      if (i >= 8) {
        //_mm512_mask_i64scatter_epi8_custom(NULL, if_mask, freebuf_vec_1, _mm256_set1_epi32(1));
        _mm256_mask_storeu_epi32(&freebuf[8], if_mask, _mm256_set1_epi32(1));
      } else {
        //_mm512_mask_i64scatter_epi8_custom(NULL, if_mask, freebuf_vec_0, _mm256_set1_epi32(1));
        _mm256_mask_storeu_epi32(&freebuf[0], if_mask, _mm256_set1_epi32(1));
      }
      cmp = _kandn_mask8(cmp, _mm256_cmplt_epi32_mask(ret_vec, _mm256_set1_epi32(0)));
      if_mask = _kand_mask8(cmp, masks[i]);
      if (i >= 8) {
        fast_kernel_packet_vec(ctx_vec, bhs_vec_1, if_mask);
      } else {
        fast_kernel_packet_vec(ctx_vec, bhs_vec_0, if_mask);
      }

      /*
    if (ret > 0) {
      freebuf[i] = 1;
    } else if (ret < 0) {
      fast_kernel_packet(ctx, bhs[i]);
    }
      */
    }

  //}

  arx_cache_flush(ctx, tsc);

  /* free received buffers */
  for (i = 0; i < n; i++) {
    if (freebuf[i] == 0)
      bufcache_free(ctx, bhs[i]);
  }

  return n;
}

static unsigned poll_queues(struct dataplane_context *ctx, uint32_t ts)
{
  struct network_buf_handle **handles;
  void *aqes[BATCH_SIZE];
  unsigned n, i, total = 0;
  uint16_t max, k = 0, num_bufs = 0, j;
  int ret;

  STATS_ADD(ctx, qs_poll, 1);

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < max)
    max = TXBUF_SIZE - ctx->tx_num;

  /* allocate buffers contents */
  max = bufcache_prealloc(ctx, max, &handles);

  for (n = 0; n < FLEXNIC_PL_APPCTX_NUM; n++) {
    fast_appctx_poll_pf(ctx, (ctx->poll_next_ctx + n) % FLEXNIC_PL_APPCTX_NUM);
  }

  for (n = 0; n < FLEXNIC_PL_APPCTX_NUM && k < max; n++) {
    for (i = 0; i < BATCH_SIZE && k < max; i++) {
      ret = fast_appctx_poll_fetch(ctx, ctx->poll_next_ctx, &aqes[k]);
      if (ret == 0)
        k++;
      else
        break;

      total++;
    }

    ctx->poll_next_ctx = (ctx->poll_next_ctx + 1) %
      FLEXNIC_PL_APPCTX_NUM;
  }

  for (j = 0; j < k; j++) {
    ret = fast_appctx_poll_bump(ctx, aqes[j], handles[num_bufs], ts);
    if (ret == 0)
      num_bufs++;
  }

  /* apply buffer reservations */
  bufcache_alloc(ctx, num_bufs);

  for (n = 0; n < FLEXNIC_PL_APPCTX_NUM; n++)
    fast_actx_rxq_probe(ctx, n);

  STATS_ADD(ctx, qs_total, total);
  if (total == 0)
    STATS_ADD(ctx, qs_empty, total);

  return total;
}

static unsigned poll_kernel(struct dataplane_context *ctx, uint32_t ts)
{
  struct network_buf_handle **handles;
  unsigned total = 0;
  uint16_t max, k = 0;
  int ret;

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < max)
    max = TXBUF_SIZE - ctx->tx_num;

  max = (max > 8 ? 8 : max);
  /* allocate buffers contents */
  max = bufcache_prealloc(ctx, max, &handles);

  for (k = 0; k < max;) {
    ret = fast_kernel_poll(ctx, handles[k], ts);
 
    if (ret == 0)
      k++;
    else if (ret < 0)
      break;

    total++;
  }

  /* apply buffer reservations */
  bufcache_alloc(ctx, k);

  return total;
}

static unsigned poll_qman(struct dataplane_context *ctx, uint32_t ts)
{
  unsigned q_ids[BATCH_SIZE];
  uint16_t q_bytes[BATCH_SIZE];
  struct network_buf_handle **handles;
  uint16_t off = 0, max;
  int ret, i;
  //int use;

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_num < max)
    max = TXBUF_SIZE - ctx->tx_num;

  STATS_ADD(ctx, qm_poll, 1);

  /* allocate buffers contents */
  max = bufcache_prealloc(ctx, max, &handles);

  /* poll queue manager */
  ret = qman_poll(&ctx->qman, max, q_ids, q_bytes);
  if (ret <= 0) {
    STATS_ADD(ctx, qm_empty, 1);
    return 0;
  }

  STATS_ADD(ctx, qm_total, ret);

  for (i = 0; i < ret; i++) {
    rte_prefetch0(handles[i]);
  }

  for (i = 0; i < ret; i++) {
    rte_prefetch0((uint8_t *) handles[i] + 64);
  }

  /* prefetch packet contents */
  for (i = 0; i < ret; i++) {
    rte_prefetch0(network_buf_buf(handles[i]));
  }

  fast_flows_qman_pf(ctx, q_ids, ret);

  fast_flows_qman_pfbufs(ctx, q_ids, ret);

  for (unsigned i = 0; i * 8 < ret; ++i) {
    unsigned start = i * 8;
    __mmask8 mask = _cvtu32_mask8((1 << MIN(8, ret - start)) - 1);
    __m256i vq_ids = _mm256_loadu_epi32(&q_ids[start]);

    __m512i vctx = _mm512_set1_epi64((uintptr_t) ctx);
    __m512i vhandles = _mm512_loadu_epi64(&handles[start]);
    __m256i vts = _mm256_set1_epi32(ts);

    __m256i vret = fast_flows_qman_vec(vctx, vq_ids, vhandles, vts, mask);
    __mmask8 vuse = _mm256_mask_cmpeq_epi32_mask(mask, vret, _mm256_setzero_si256());
    //printf("Ye %d ", _mm_popcnt_u32(_cvtmask8_u32(vuse)));
    off += _mm_popcnt_u32(_cvtmask8_u32(vuse));
  }

  /*
  for (i = 0; i < ret; i++) {
    use = fast_flows_qman(ctx, q_ids[i], handles[off], ts);

    if (use == 0)
      off++;
  }
  */

  /* apply buffer reservations */
  bufcache_alloc(ctx, off);

  return ret;
}

static unsigned poll_qman_fwd(struct dataplane_context *ctx, uint32_t ts)
{
  void *flow_states[4 * BATCH_SIZE];
  int ret, i;

  /* poll queue manager forwarding ring */
  ret = rte_ring_dequeue_burst(ctx->qman_fwd_ring, flow_states, 4 * BATCH_SIZE, NULL);
  for (i = 0; i < ret; i++) {
    fast_flows_qman_fwd(ctx, flow_states[i]);
  }

  return ret;
}

static inline uint8_t bufcache_prealloc(struct dataplane_context *ctx, uint16_t num,
    struct network_buf_handle ***handles)
{
  uint16_t grow, res, head, g, i;
  struct network_buf_handle *nbh;

  /* try refilling buffer cache */
  if (ctx->bufcache_num < num) {
    grow = BUFCACHE_SIZE - ctx->bufcache_num;
    head = (ctx->bufcache_head + ctx->bufcache_num) & (BUFCACHE_SIZE - 1);

    if (head + grow <= BUFCACHE_SIZE) {
      res = network_buf_alloc(&ctx->net, grow, ctx->bufcache_handles + head);
    } else {
      g = BUFCACHE_SIZE - head;
      res = network_buf_alloc(&ctx->net, g, ctx->bufcache_handles + head);
      if (res == g) {
        res += network_buf_alloc(&ctx->net, grow - g, ctx->bufcache_handles);
      }
    }

    for (i = 0; i < res; i++) {
      g = (head + i) & (BUFCACHE_SIZE - 1);
      nbh = ctx->bufcache_handles[g];
      ctx->bufcache_handles[g] = (struct network_buf_handle *)
        ((uintptr_t) nbh);
    }

    ctx->bufcache_num += res;
  }
  num = MIN(num, (ctx->bufcache_head + ctx->bufcache_num <= BUFCACHE_SIZE ?
        ctx->bufcache_num : BUFCACHE_SIZE - ctx->bufcache_head));

  *handles = ctx->bufcache_handles + ctx->bufcache_head;

  return num;
}

static inline void bufcache_alloc(struct dataplane_context *ctx, uint16_t num)
{
  assert(num <= ctx->bufcache_num);

  ctx->bufcache_head = (ctx->bufcache_head + num) & (BUFCACHE_SIZE - 1);
  ctx->bufcache_num -= num;
}

static inline void bufcache_free(struct dataplane_context *ctx,
    struct network_buf_handle *handle)
{
  uint32_t head, num;

  num = ctx->bufcache_num;
  if (num < BUFCACHE_SIZE) {
    /* free to cache */
    head = (ctx->bufcache_head + num) & (BUFCACHE_SIZE - 1);
    ctx->bufcache_handles[head] = handle;
    ctx->bufcache_num = num + 1;
    network_buf_reset(handle);
  } else {
    /* free to network buffer manager */
    network_free(1, &handle);
  }
}

static inline void tx_flush(struct dataplane_context *ctx)
{
  int ret;
  unsigned i;

  if (ctx->tx_num == 0) {
    return;
  }

  /* try to send out packets */
  ret = network_send(&ctx->net, ctx->tx_num, ctx->tx_handles);

  if (ret == ctx->tx_num) {
    /* everything sent */
    ctx->tx_num = 0;
  } else if (ret > 0) {
    /* move unsent packets to front */
    for (i = ret; i < ctx->tx_num; i++) {
      ctx->tx_handles[i - ret] = ctx->tx_handles[i];
    }
    ctx->tx_num -= ret;
  }
}

static void poll_scale(struct dataplane_context *ctx)
{
  unsigned st = fp_scale_to;

  if (st == 0)
    return;

  fprintf(stderr, "Scaling fast path from %u to %u\n", fp_cores_cur, st);
  if (st < fp_cores_cur) {
    if (network_scale_down(fp_cores_cur, st) != 0) {
      fprintf(stderr, "network_scale_down failed\n");
      abort();
    }
  } else if (st > fp_cores_cur) {
    if (network_scale_up(fp_cores_cur, st) != 0) {
      fprintf(stderr, "network_scale_up failed\n");
      abort();
    }
  } else {
    fprintf(stderr, "poll_scale: warning core number didn't change\n");
  }

  fp_cores_cur = st;
  fp_scale_to = 0;
}

static void arx_cache_flush(struct dataplane_context *ctx, uint64_t tsc)
{
  uint16_t i;
  struct flextcp_pl_appctx *actx;
  struct flextcp_pl_arx *parx[BATCH_SIZE];

  for (i = 0; i < ctx->arx_num; i++) {
    actx = &fp_state->appctx[ctx->id][ctx->arx_ctx[i]];
    if (fast_actx_rxq_alloc(ctx, actx, &parx[i]) != 0) {
      /* TODO: how do we handle this? */
      fprintf(stderr, "arx_cache_flush: no space in app rx queue\n");
      abort();
    }
  }

  for (i = 0; i < ctx->arx_num; i++) {
    rte_prefetch0(parx[i]);
  }

  for (i = 0; i < ctx->arx_num; i++) {
    *parx[i] = ctx->arx_cache[i];
  }

  for (i = 0; i < ctx->arx_num; i++) {
    actx = &fp_state->appctx[ctx->id][ctx->arx_ctx[i]];
    notify_appctx(actx, tsc);
  }

  ctx->arx_num = 0;
}
