; ModuleID = 'full.ll'
source_filename = "full.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.dataplane_context = type { i32 }
%struct.network_buf_handle = type opaque
%struct.tcp_opts = type { %struct.tcp_timestamp_opt* }
%struct.tcp_timestamp_opt = type <{ i8, i8, i32, i32 }>
%struct.rte_mbuf = type { [0 x i8*], i8*, %union.anon, [0 x i64], i16, %union.anon.0, i16, i16, i64, [0 x i8*], %union.anon.1, i32, i16, i16, %union.anon.4, i16, i16, i64, [0 x i8*], %union.anon.10, %struct.rte_mempool*, %struct.rte_mbuf*, %union.anon.11, i16, i16, i32, %struct.rte_mbuf_ext_shared_info*, [2 x i64] }
%union.anon = type { i64 }
%union.anon.0 = type { %struct.rte_atomic16_t }
%struct.rte_atomic16_t = type { i16 }
%union.anon.1 = type { i32 }
%union.anon.4 = type { %union.anon.5 }
%union.anon.5 = type { %struct.anon.6 }
%struct.anon.6 = type { %union.anon.7, i32 }
%union.anon.7 = type { i32 }
%union.anon.10 = type { i8* }
%struct.rte_mempool = type opaque
%union.anon.11 = type { i64 }
%struct.rte_mbuf_ext_shared_info = type opaque
%struct.pkt_tcp = type <{ %struct.eth_hdr, %struct.ip_hdr, %struct.tcp_hdr }>
%struct.eth_hdr = type { %struct.eth_addr, %struct.eth_addr, i16 }
%struct.eth_addr = type { [6 x i8] }
%struct.ip_hdr = type { i8, i8, i16, i16, i16, i8, i8, i16, i32, i32 }
%struct.tcp_hdr = type { i16, i16, i32, i32, i16, i16, i16, i16 }

; Function Attrs: nofree nounwind uwtable

; Function Attrs: nounwind readnone speculatable willreturn
declare i16 @llvm.bswap.i16(i16) #1

; Function Attrs: nofree nounwind uwtable
define dso_local void @fast_flows_packet_parse_v8_M0_vvvv(<8 x i1>   %0, <8 x %struct.dataplane_context*>   %ctx, <8 x %struct.network_buf_handle*>  %nbhs, <8 x i8**>  %fss, <8 x %struct.tcp_opts*> %tos)   {
entry.rv:
  %neg.implicit_wfv_mask_SIMD = xor <8 x i1> %0, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  br label %.rv

.rv:                                              ; preds = %entry.rv
  br label %.rv1

.rv1:                                             ; preds = %.rv
  %1 = call <8 x i8*> @llvm.masked.gather.v8p0i8.v8p0p0i8(<8 x i8**> %fss, i32 8, <8 x i1> %0, <8 x i8*> undef)
  %cmp_SIMD = icmp eq <8 x i8*> %1, zeroinitializer
  %neg.cmp_SIMD = xor <8 x i1> %cmp_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_.1_SIMD = and <8 x i1> %0, %neg.cmp_SIMD
  %edge_.0_SIMD = and <8 x i1> %0, %cmp_SIMD
  br label %if.end.rv

if.end.rv:                                        ; preds = %.rv1
  %buf_addr.i = bitcast <8 x %struct.network_buf_handle*> %nbhs to <8 x i8**>
  %2 = call <8 x i8*> @llvm.masked.gather.v8p0i8.v8p0p0i8(<8 x i8**> %buf_addr.i, i32 64, <8 x i1> %edge_.1_SIMD, <8 x i8*> undef)
  %3 = bitcast <8 x %struct.network_buf_handle*> %nbhs to <8 x %struct.rte_mbuf*>
  %data_off.i = getelementptr inbounds %struct.rte_mbuf, <8 x %struct.rte_mbuf*> %3, i64 0, i32 4
  %4 = call <8 x i16> @llvm.masked.gather.v8i16.v8p0i16(<8 x i16*> %data_off.i, i32 16, <8 x i1> %edge_.1_SIMD, <8 x i16> undef)
  %idx.ext.i_SIMD = zext <8 x i16> %4 to <8 x i64>
  %data_len.i = getelementptr inbounds %struct.rte_mbuf, <8 x %struct.rte_mbuf*> %3, i64 0, i32 12
  %5 = call <8 x i16> @llvm.masked.gather.v8i16.v8p0i16(<8 x i16*> %data_len.i, i32 8, <8 x i1> %edge_.1_SIMD, <8 x i16> undef)
  %add.ptr.i = getelementptr i8, <8 x i8*> %2, <8 x i64> %idx.ext.i_SIMD
  %ip = getelementptr inbounds i8, <8 x i8*> %add.ptr.i, i64 14
  %6 = call <8 x i8> @llvm.masked.gather.v8i8.v8p0i8(<8 x i8*> %ip, i32 1, <8 x i1> %edge_.1_SIMD, <8 x i8> undef)
  %conv_SIMD = zext <8 x i16> %5 to <8 x i64>
  %cmp2_SIMD = icmp ult <8 x i16> %5, <i16 54, i16 54, i16 54, i16 54, i16 54, i16 54, i16 54, i16 54>
  %type = getelementptr inbounds i8, <8 x i8*> %add.ptr.i, i64 12
  %7 = bitcast <8 x i8*> %type to <8 x i16*>
  %8 = call <8 x i16> @llvm.masked.gather.v8i16.v8p0i16(<8 x i16*> %7, i32 1, <8 x i1> %edge_.1_SIMD, <8 x i16> undef)
  %cmp6_SIMD = icmp ne <8 x i16> %8, <i16 8, i16 8, i16 8, i16 8, i16 8, i16 8, i16 8, i16 8>
  %or59_SIMD = or <8 x i1> %cmp2_SIMD, %cmp6_SIMD
  %9 = getelementptr inbounds i8, <8 x i8*> %add.ptr.i, i64 23
  %10 = call <8 x i8> @llvm.masked.gather.v8i8.v8p0i8(<8 x i8*> %9, i32 1, <8 x i1> %edge_.1_SIMD, <8 x i8> undef)
  %cmp10_SIMD = icmp ne <8 x i8> %10, <i8 6, i8 6, i8 6, i8 6, i8 6, i8 6, i8 6, i8 6>
  %or1260_SIMD = or <8 x i1> %or59_SIMD, %cmp10_SIMD
  %11 = icmp ne <8 x i8> %6, <i8 69, i8 69, i8 69, i8 69, i8 69, i8 69, i8 69, i8 69>
  %12 = or <8 x i1> %11, %or1260_SIMD
  %_hdrlen_rsvd_flags = getelementptr inbounds i8, <8 x i8*> %add.ptr.i, i64 46
  %13 = bitcast <8 x i8*> %_hdrlen_rsvd_flags to <8 x i16*>
  %14 = call <8 x i16> @llvm.masked.gather.v8i16.v8p0i16(<8 x i16*> %13, i32 1, <8 x i1> %edge_.1_SIMD, <8 x i16> undef)
  %extract43 = extractelement <8 x i16> %14, i32 7
  %extract40 = extractelement <8 x i16> %14, i32 6
  %extract37 = extractelement <8 x i16> %14, i32 5
  %extract34 = extractelement <8 x i16> %14, i32 4
  %extract31 = extractelement <8 x i16> %14, i32 3
  %extract28 = extractelement <8 x i16> %14, i32 2
  %extract25 = extractelement <8 x i16> %14, i32 1
  %extract23 = extractelement <8 x i16> %14, i32 0
  %15 = trunc <8 x i16> %14 to <8 x i8>
  %cmp24_SIMD = icmp ult <8 x i8> %15, <i8 80, i8 80, i8 80, i8 80, i8 80, i8 80, i8 80, i8 80>
  %or2663_SIMD = or <8 x i1> %12, %cmp24_SIMD
  %len29 = getelementptr inbounds i8, <8 x i8*> %add.ptr.i, i64 16
  %16 = bitcast <8 x i8*> %len29 to <8 x i16*>
  %17 = call <8 x i16> @llvm.masked.gather.v8i16.v8p0i16(<8 x i16*> %16, i32 1, <8 x i1> %edge_.1_SIMD, <8 x i16> undef)
  %extract20 = extractelement <8 x i16> %17, i32 7
  %extract17 = extractelement <8 x i16> %17, i32 6
  %extract14 = extractelement <8 x i16> %17, i32 5
  %extract11 = extractelement <8 x i16> %17, i32 4
  %extract8 = extractelement <8 x i16> %17, i32 3
  %extract5 = extractelement <8 x i16> %17, i32 2
  %extract2 = extractelement <8 x i16> %17, i32 1
  %extract = extractelement <8 x i16> %17, i32 0
  %rev.i = tail call i16 @llvm.bswap.i16(i16 %extract) #6
  %scalarized = insertelement <8 x i16> undef, i16 %rev.i, i64 0
  %rev.i3 = tail call i16 @llvm.bswap.i16(i16 %extract2) #6
  %scalarized4 = insertelement <8 x i16> %scalarized, i16 %rev.i3, i64 1
  %rev.i6 = tail call i16 @llvm.bswap.i16(i16 %extract5) #6
  %scalarized7 = insertelement <8 x i16> %scalarized4, i16 %rev.i6, i64 2
  %rev.i9 = tail call i16 @llvm.bswap.i16(i16 %extract8) #6
  %scalarized10 = insertelement <8 x i16> %scalarized7, i16 %rev.i9, i64 3
  %rev.i12 = tail call i16 @llvm.bswap.i16(i16 %extract11) #6
  %scalarized13 = insertelement <8 x i16> %scalarized10, i16 %rev.i12, i64 4
  %rev.i15 = tail call i16 @llvm.bswap.i16(i16 %extract14) #6
  %scalarized16 = insertelement <8 x i16> %scalarized13, i16 %rev.i15, i64 5
  %rev.i18 = tail call i16 @llvm.bswap.i16(i16 %extract17) #6
  %scalarized19 = insertelement <8 x i16> %scalarized16, i16 %rev.i18, i64 6
  %rev.i21 = tail call i16 @llvm.bswap.i16(i16 %extract20) #6
  %scalarized22 = insertelement <8 x i16> %scalarized19, i16 %rev.i21, i64 7
  %conv31_SIMD = zext <8 x i16> %scalarized22 to <8 x i64>
  %add_SIMD = add nuw nsw <8 x i64> %conv31_SIMD, <i64 14, i64 14, i64 14, i64 14, i64 14, i64 14, i64 14, i64 14>
  %cmp32_SIMD = icmp ugt <8 x i64> %add_SIMD, %conv_SIMD
  %or3464_SIMD = or <8 x i1> %or2663_SIMD, %cmp32_SIMD
  %rev.i.i = tail call i16 @llvm.bswap.i16(i16 %extract23) #6
  %scalarized24 = insertelement <8 x i16> undef, i16 %rev.i.i, i64 0
  %rev.i.i26 = tail call i16 @llvm.bswap.i16(i16 %extract25) #6
  %scalarized27 = insertelement <8 x i16> %scalarized24, i16 %rev.i.i26, i64 1
  %rev.i.i29 = tail call i16 @llvm.bswap.i16(i16 %extract28) #6
  %scalarized30 = insertelement <8 x i16> %scalarized27, i16 %rev.i.i29, i64 2
  %rev.i.i32 = tail call i16 @llvm.bswap.i16(i16 %extract31) #6
  %scalarized33 = insertelement <8 x i16> %scalarized30, i16 %rev.i.i32, i64 3
  %rev.i.i35 = tail call i16 @llvm.bswap.i16(i16 %extract34) #6
  %scalarized36 = insertelement <8 x i16> %scalarized33, i16 %rev.i.i35, i64 4
  %rev.i.i38 = tail call i16 @llvm.bswap.i16(i16 %extract37) #6
  %scalarized39 = insertelement <8 x i16> %scalarized36, i16 %rev.i.i38, i64 5
  %rev.i.i41 = tail call i16 @llvm.bswap.i16(i16 %extract40) #6
  %scalarized42 = insertelement <8 x i16> %scalarized39, i16 %rev.i.i41, i64 6
  %rev.i.i44 = tail call i16 @llvm.bswap.i16(i16 %extract43) #6
  %scalarized45 = insertelement <8 x i16> %scalarized42, i16 %rev.i.i44, i64 7
  %18 = lshr <8 x i16> %scalarized45, <i16 12, i16 12, i16 12, i16 12, i16 12, i16 12, i16 12, i16 12>
  %mul.i_SIMD = shl nuw nsw <8 x i16> %18, <i16 2, i16 2, i16 2, i16 2, i16 2, i16 2, i16 2, i16 2>
  %sub.i_SIMD = add nsw <8 x i16> %mul.i_SIMD, <i16 -20, i16 -20, i16 -20, i16 -20, i16 -20, i16 -20, i16 -20, i16 -20>
  %ts.i = getelementptr inbounds %struct.tcp_opts, <8 x %struct.tcp_opts*> %tos, i64 0, i32 0
  call void @llvm.masked.scatter.v8p0s_struct.tcp_timestamp_opts.v8p0p0s_struct.tcp_timestamp_opts(<8 x %struct.tcp_timestamp_opt*> zeroinitializer, <8 x %struct.tcp_timestamp_opt**> %ts.i, i32 8, <8 x i1> %edge_.1_SIMD)
  %cmp.i_SIMD = icmp ult <8 x i16> %scalarized45, <i16 20480, i16 20480, i16 20480, i16 20480, i16 20480, i16 20480, i16 20480, i16 20480>
  %neg.cmp.i_SIMD = xor <8 x i1> %cmp.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_if.end.1_SIMD = and <8 x i1> %edge_.1_SIMD, %neg.cmp.i_SIMD
  %edge_if.end.0_SIMD = and <8 x i1> %edge_.1_SIMD, %cmp.i_SIMD
  %19 = getelementptr inbounds i8, <8 x i8*> %add.ptr.i, i64 54
  %20 = bitcast <8 x i8*> %add.ptr.i to <8 x %struct.pkt_tcp*>
  br label %lor.lhs.false.i.rv

lor.lhs.false.i.rv:                               ; preds = %if.end.rv
  %conv6.i_SIMD = zext <8 x i16> %sub.i_SIMD to <8 x i64>
  %sub8.i_SIMD = add nsw <8 x i64> %conv_SIMD, <i64 -54, i64 -54, i64 -54, i64 -54, i64 -54, i64 -54, i64 -54, i64 -54>
  %cmp9.i_SIMD = icmp ult <8 x i64> %sub8.i_SIMD, %conv6.i_SIMD
  %neg.cmp9.i_SIMD = xor <8 x i1> %cmp9.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_lor.lhs.false.i.1_SIMD = and <8 x i1> %edge_if.end.1_SIMD, %neg.cmp9.i_SIMD
  %edge_lor.lhs.false.i.0_SIMD = and <8 x i1> %edge_if.end.1_SIMD, %cmp9.i_SIMD
  br label %if.end.i.rv

if.end.i.rv:                                      ; preds = %lor.lhs.false.i.rv
  %21 = bitcast <8 x i8*> %19 to <8 x i32*>
  %22 = call <8 x i32> @llvm.masked.gather.v8i32.v8p0i32(<8 x i32*> %21, i32 4, <8 x i1> %edge_lor.lhs.false.i.1_SIMD, <8 x i32> undef)
  %cmp12.i_SIMD = icmp eq <8 x i32> %22, <i32 168296705, i32 168296705, i32 168296705, i32 168296705, i32 168296705, i32 168296705, i32 168296705, i32 168296705>
  %cmp15.i_SIMD = icmp eq <8 x i16> %sub.i_SIMD, <i16 12, i16 12, i16 12, i16 12, i16 12, i16 12, i16 12, i16 12>
  %or.cond.i_SIMD = and <8 x i1> %cmp15.i_SIMD, %cmp12.i_SIMD
  %23 = trunc <8 x i32> %22 to <8 x i8>
  %neg.or.cond.i_SIMD = xor <8 x i1> %or.cond.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_if.end.i.1_SIMD = and <8 x i1> %edge_lor.lhs.false.i.1_SIMD, %neg.or.cond.i_SIMD
  %edge_if.end.i.0_SIMD = and <8 x i1> %edge_lor.lhs.false.i.1_SIMD, %or.cond.i_SIMD
  br label %while.cond.preheader.i.rv

while.cond.preheader.i.rv:                        ; preds = %if.end.i.rv
  %conv21.i_SIMD = zext <8 x i16> %sub.i_SIMD to <8 x i32>
  %cmp2295.i_SIMD = icmp eq <8 x i16> %sub.i_SIMD, zeroinitializer
  %neg.cmp2295.i_SIMD = xor <8 x i1> %cmp2295.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_while.cond.preheader.i.1_SIMD = and <8 x i1> %edge_if.end.i.1_SIMD, %neg.cmp2295.i_SIMD
  %edge_while.cond.preheader.i.0_SIMD = and <8 x i1> %edge_if.end.i.1_SIMD, %cmp2295.i_SIMD
  br label %while.body.lr.ph.i.rv

while.body.lr.ph.i.rv:                            ; preds = %while.cond.preheader.i.rv
  %24 = bitcast <8 x %struct.tcp_opts*> %tos to <8 x i8**>
  br label %while.body.i.rv

if.then17.i.rv:                                   ; preds = %if.then42.loopexit.rv
  %25 = bitcast <8 x %struct.tcp_opts*> %tos to <8 x i8**>
  %26 = getelementptr inbounds i8, <8 x i8*> %add.ptr.i, i64 56
  call void @llvm.masked.scatter.v8p0i8.v8p0p0i8(<8 x i8*> %26, <8 x i8**> %25, i32 8, <8 x i1> %edge_if.end.i.0_SIMD)
  %27 = bitcast <8 x i8*> %26 to <8 x %struct.tcp_timestamp_opt*>
  br label %tcp_parse_options.exit.s.rv

while.body.i.divexit.rv:                          ; preds = %while.body.i.rv
  %.lcssa8_SIMD = phi <8 x %struct.tcp_timestamp_opt*> [ %.track6_SIMD, %while.body.i.rv ]
  %loop.exit.dedicated.xlcssa_SIMD = phi <8 x i1> [ %loop.exit.dedicated.xtrack_SIMD, %while.body.i.rv ]
  %.lcssa_SIMD = phi <8 x %struct.tcp_timestamp_opt*> [ %.track_SIMD, %while.body.i.rv ]
  %loop.exit.dedicated1.xlcssa_SIMD = phi <8 x i1> [ %loop.exit.dedicated1.xtrack_SIMD, %while.body.i.rv ]
  %loop.exit.dedicated4.xlcssa_SIMD = phi <8 x i1> [ %loop.exit.dedicated4.xtrack_SIMD, %while.body.i.rv ]
  %loop.exit.dedicated3.xlcssa_SIMD = phi <8 x i1> [ %loop.exit.dedicated3.xtrack_SIMD, %while.body.i.rv ]
  %neg.loop.exit.dedicated3.xlcssa_SIMD = xor <8 x i1> %loop.exit.dedicated3.xlcssa_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_while.body.i.divexit.1_SIMD = and <8 x i1> %edge_while.body.i.1_SIMD, %neg.loop.exit.dedicated3.xlcssa_SIMD
  %edge_while.body.i.divexit.0_SIMD = and <8 x i1> %edge_while.body.i.1_SIMD, %loop.exit.dedicated3.xlcssa_SIMD
  br label %loop.exit.dedicated3.xlcssa.else.rv

while.body.i.rv:                                  ; preds = %while.body.i.pure.rv, %while.body.lr.ph.i.rv
  %_SIMD = phi <8 x %struct.tcp_timestamp_opt*> [ zeroinitializer, %while.body.lr.ph.i.rv ], [ %.R19.b_SIMD, %while.body.i.pure.rv ]
  %_SIMD46 = phi <8 x i8> [ %23, %while.body.lr.ph.i.rv ], [ %40, %while.body.i.pure.rv ]
  %conv2097.i_SIMD = phi <8 x i32> [ zeroinitializer, %while.body.lr.ph.i.rv ], [ %conv20.i_SIMD, %while.body.i.pure.rv ]
  %off.096.i_SIMD = phi <8 x i16> [ zeroinitializer, %while.body.lr.ph.i.rv ], [ %add64.i_SIMD, %while.body.i.pure.rv ]
  %while.body.i.live_SIMD = phi <8 x i1> [ %edge_while.cond.preheader.i.1_SIMD, %while.body.lr.ph.i.rv ], [ %.b4388_SIMD, %while.body.i.pure.rv ]
  %loop.exit.dedicated3.xtrack_SIMD = phi <8 x i1> [ zeroinitializer, %while.body.lr.ph.i.rv ], [ %.b4689_SIMD, %while.body.i.pure.rv ]
  %loop.exit.dedicated4.xtrack_SIMD = phi <8 x i1> [ zeroinitializer, %while.body.lr.ph.i.rv ], [ %.b4990_SIMD, %while.body.i.pure.rv ]
  %loop.exit.dedicated1.xtrack_SIMD = phi <8 x i1> [ zeroinitializer, %while.body.lr.ph.i.rv ], [ %.b5291_SIMD, %while.body.i.pure.rv ]
  %.track_SIMD = phi <8 x %struct.tcp_timestamp_opt*> [ undef, %while.body.lr.ph.i.rv ], [ %.R19.b.R.b_SIMD, %while.body.i.pure.rv ]
  %loop.exit.dedicated.xtrack_SIMD = phi <8 x i1> [ zeroinitializer, %while.body.lr.ph.i.rv ], [ %.b5792_SIMD, %while.body.i.pure.rv ]
  %.track6_SIMD = phi <8 x %struct.tcp_timestamp_opt*> [ undef, %while.body.lr.ph.i.rv ], [ %.R60.b_SIMD, %while.body.i.pure.rv ]
  %28 = and <8 x i1> %while.body.i.live_SIMD, %edge_while.cond.preheader.i.1_SIMD
  %redOr = call i1 @llvm.experimental.vector.reduce.or.v8i1(<8 x i1> %28)
  %neg.while.body.i.exec = xor i1 %redOr, true
  %.splatinsert = insertelement <8 x i1> undef, i1 %neg.while.body.i.exec, i32 0
  %.splat = shufflevector <8 x i1> %.splatinsert, <8 x i1> undef, <8 x i32> zeroinitializer
  %edge_while.body.i.1_SIMD = and <8 x i1> %edge_while.cond.preheader.i.1_SIMD, %.splat
  %.splatinsert47 = insertelement <8 x i1> undef, i1 %redOr, i32 0
  %.splat48 = shufflevector <8 x i1> %.splatinsert47, <8 x i1> undef, <8 x i32> zeroinitializer
  %edge_while.body.i.0_SIMD = and <8 x i1> %edge_while.cond.preheader.i.1_SIMD, %.splat48
  br i1 %redOr, label %while.body.i.test.rv, label %while.body.i.divexit.rv

while.body.i.test.rv:                             ; preds = %while.body.i.rv
  %edge_while.body.i.test.0_SIMD = and <8 x i1> %edge_while.body.i.0_SIMD, %while.body.i.live_SIMD
  %neg.while.body.i.live_SIMD = xor <8 x i1> %while.body.i.live_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_while.body.i.test.1_SIMD = and <8 x i1> %edge_while.body.i.0_SIMD, %neg.while.body.i.live_SIMD
  br label %while.body.i.offset.rv

while.body.i.offset.rv:                           ; preds = %while.body.i.test.rv
  %idxprom.i_SIMD = zext <8 x i16> %off.096.i_SIMD to <8 x i64>
  %arrayidx24.i = getelementptr inbounds %struct.pkt_tcp, <8 x %struct.pkt_tcp*> %20, i64 1, i32 0, i32 0, i32 0, <8 x i64> %idxprom.i_SIMD
  br label %divswitch5.rv

divswitch.rv:                                     ; preds = %divswitch5.rv
  %29 = icmp eq <8 x i8> %_SIMD46, zeroinitializer
  %neg.9_SIMD = xor <8 x i1> %29, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_divswitch.1_SIMD = and <8 x i1> %edge_divswitch5.1_SIMD, %neg.9_SIMD
  %edge_divswitch.0_SIMD = and <8 x i1> %edge_divswitch5.1_SIMD, %29
  br label %if.else38.i.rv

divswitch5.rv:                                    ; preds = %while.body.i.offset.rv
  %30 = icmp eq <8 x i8> %_SIMD46, <i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1>
  %neg._SIMD = xor <8 x i1> %30, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_divswitch5.1_SIMD = and <8 x i1> %edge_while.body.i.test.0_SIMD, %neg._SIMD
  %edge_divswitch5.0_SIMD = and <8 x i1> %edge_while.body.i.test.0_SIMD, %30
  br label %divswitch.rv

if.else38.i.rv:                                   ; preds = %divswitch.rv
  %sub27.i_SIMD = sub nsw <8 x i32> %conv21.i_SIMD, %conv2097.i_SIMD
  %conv39.i_SIMD = and <8 x i32> %sub27.i_SIMD, <i32 254, i32 254, i32 254, i32 254, i32 254, i32 254, i32 254, i32 254>
  %cmp40.i_SIMD = icmp eq <8 x i32> %conv39.i_SIMD, zeroinitializer
  %neg.cmp40.i_SIMD = xor <8 x i1> %cmp40.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_if.else38.i.1_SIMD = and <8 x i1> %edge_divswitch.1_SIMD, %neg.cmp40.i_SIMD
  %edge_if.else38.i.0_SIMD = and <8 x i1> %edge_divswitch.1_SIMD, %cmp40.i_SIMD
  br label %if.end43.i.rv

if.end43.i.rv:                                    ; preds = %if.else38.i.rv
  %add.i_SIMD = add nuw nsw <8 x i32> %conv2097.i_SIMD, <i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1>
  %idxprom45.i_SIMD = zext <8 x i32> %add.i_SIMD to <8 x i64>
  %arrayidx46.i = getelementptr inbounds %struct.pkt_tcp, <8 x %struct.pkt_tcp*> %20, i64 1, i32 0, i32 0, i32 0, <8 x i64> %idxprom45.i_SIMD
  %31 = call <8 x i8> @llvm.masked.gather.v8i8.v8p0i8(<8 x i8*> %arrayidx46.i, i32 1, <8 x i1> %edge_if.else38.i.1_SIMD, <8 x i8> undef)
  %cmp48.i_SIMD = icmp eq <8 x i8> %_SIMD46, <i8 8, i8 8, i8 8, i8 8, i8 8, i8 8, i8 8, i8 8>
  %edge_if.end43.i.0_SIMD = and <8 x i1> %edge_if.else38.i.1_SIMD, %cmp48.i_SIMD
  %neg.cmp48.i_SIMD = xor <8 x i1> %cmp48.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_if.end43.i.1_SIMD = and <8 x i1> %edge_if.else38.i.1_SIMD, %neg.cmp48.i_SIMD
  br label %if.then50.i.rv

if.then50.i.rv:                                   ; preds = %if.end43.i.rv
  %cmp52.i_SIMD = icmp eq <8 x i8> %31, <i8 10, i8 10, i8 10, i8 10, i8 10, i8 10, i8 10, i8 10>
  %edge_if.then50.i.0_SIMD = and <8 x i1> %edge_if.end43.i.0_SIMD, %cmp52.i_SIMD
  %neg.cmp52.i_SIMD = xor <8 x i1> %cmp52.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_if.then50.i.1_SIMD = and <8 x i1> %edge_if.end43.i.0_SIMD, %neg.cmp52.i_SIMD
  br label %if.end55.i.rv

if.end55.i.rv:                                    ; preds = %if.then50.i.rv
  call void @llvm.masked.scatter.v8p0i8.v8p0p0i8(<8 x i8*> %arrayidx24.i, <8 x i8**> %24, i32 8, <8 x i1> %edge_if.then50.i.0_SIMD)
  %32 = bitcast <8 x i8*> %arrayidx24.i to <8 x %struct.tcp_timestamp_opt*>
  br label %if.end61.i.s.rv

if.end61.i.s.rv:                                  ; preds = %if.end55.i.rv
  %.R19.b_SIMD = select <8 x i1> %edge_if.then50.i.0_SIMD, <8 x %struct.tcp_timestamp_opt*> %32, <8 x %struct.tcp_timestamp_opt*> %_SIMD
  %.b_SIMD = select <8 x i1> %edge_if.then50.i.0_SIMD, <8 x i8> <i8 10, i8 10, i8 10, i8 10, i8 10, i8 10, i8 10, i8 10>, <8 x i8> %_SIMD46
  %.R24.b_SIMD = select <8 x i1> %edge_if.end43.i.1_SIMD, <8 x i8> %31, <8 x i8> %.b_SIMD
  br label %if.end61.i.rv

if.end61.i.rv:                                    ; preds = %if.end61.i.s.rv
  %33 = or <8 x i1> %edge_divswitch5.0_SIMD, %edge_if.then50.i.0_SIMD
  %34 = or <8 x i1> %33, %edge_if.end43.i.1_SIMD
  %conv62.i_SIMD = zext <8 x i8> %.R24.b_SIMD to <8 x i16>
  %add64.i_SIMD = add <8 x i16> %off.096.i_SIMD, %conv62.i_SIMD
  %cmp22.i_SIMD = icmp ugt <8 x i16> %sub.i_SIMD, %add64.i_SIMD
  %neg.cmp22.i_SIMD = xor <8 x i1> %cmp22.i_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_if.end61.i.1_SIMD = and <8 x i1> %34, %neg.cmp22.i_SIMD
  %edge_if.end61.i.0_SIMD = and <8 x i1> %34, %cmp22.i_SIMD
  br label %if.end61.while.body_crit_edge.i.rv

while.body.i.pure.s.rv:                           ; preds = %if.end61.while.body_crit_edge.i.rv
  %edge_divswitch.0.not_SIMD = xor <8 x i1> %edge_divswitch.0_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b3785_SIMD = and <8 x i1> %edge_divswitch.0.not_SIMD, %while.body.i.live_SIMD
  %edge_if.end61.i.1.not_SIMD = xor <8 x i1> %edge_if.end61.i.1_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b3986_SIMD = and <8 x i1> %edge_if.end61.i.1.not_SIMD, %.b3785_SIMD
  %edge_if.then50.i.1.not_SIMD = xor <8 x i1> %edge_if.then50.i.1_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b4187_SIMD = and <8 x i1> %edge_if.then50.i.1.not_SIMD, %.b3986_SIMD
  %edge_if.else38.i.0.not_SIMD = xor <8 x i1> %edge_if.else38.i.0_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b4388_SIMD = and <8 x i1> %edge_if.else38.i.0.not_SIMD, %.b4187_SIMD
  %.b4689_SIMD = or <8 x i1> %edge_if.else38.i.0_SIMD, %loop.exit.dedicated3.xtrack_SIMD
  %.b4990_SIMD = or <8 x i1> %edge_if.then50.i.1_SIMD, %loop.exit.dedicated4.xtrack_SIMD
  %.b5291_SIMD = or <8 x i1> %edge_if.end61.i.1_SIMD, %loop.exit.dedicated1.xtrack_SIMD
  %.R19.b.R.b_SIMD = select <8 x i1> %edge_if.end61.i.1_SIMD, <8 x %struct.tcp_timestamp_opt*> %.R19.b_SIMD, <8 x %struct.tcp_timestamp_opt*> %.track_SIMD
  %.b5792_SIMD = or <8 x i1> %edge_divswitch.0_SIMD, %loop.exit.dedicated.xtrack_SIMD
  %.R60.b_SIMD = select <8 x i1> %edge_divswitch.0_SIMD, <8 x %struct.tcp_timestamp_opt*> %_SIMD, <8 x %struct.tcp_timestamp_opt*> %.track6_SIMD
  br label %while.body.i.pure.rv

while.body.i.pure.rv:                             ; preds = %while.body.i.pure.s.rv
  %35 = or <8 x i1> %edge_divswitch.0_SIMD, %edge_if.end61.i.1_SIMD
  %36 = or <8 x i1> %35, %edge_if.then50.i.1_SIMD
  %37 = or <8 x i1> %36, %edge_if.else38.i.0_SIMD
  %38 = or <8 x i1> %37, %edge_while.body.i.test.1_SIMD
  %39 = or <8 x i1> %38, %edge_if.end61.i.0_SIMD
  br label %while.body.i.rv

if.end61.while.body_crit_edge.i.rv:               ; preds = %if.end61.i.rv
  %conv20.i_SIMD = zext <8 x i16> %add64.i_SIMD to <8 x i32>
  %idxprom.phi.trans.insert.i_SIMD = zext <8 x i16> %add64.i_SIMD to <8 x i64>
  %arrayidx24.phi.trans.insert.i = getelementptr inbounds %struct.pkt_tcp, <8 x %struct.pkt_tcp*> %20, i64 1, i32 0, i32 0, i32 0, <8 x i64> %idxprom.phi.trans.insert.i_SIMD
  %40 = call <8 x i8> @llvm.masked.gather.v8i8.v8p0i8(<8 x i8*> %arrayidx24.phi.trans.insert.i, i32 1, <8 x i1> %edge_if.end61.i.0_SIMD, <8 x i8> undef)
  br label %while.body.i.pure.s.rv

loop.exit.dedicated.rv:                           ; preds = %loop.exit.dedicated1.xlcssa.else.rv
  br label %loop.exit.dedicated1.rv

loop.exit.dedicated1.xlcssa.else.rv:              ; preds = %loop.exit.dedicated4.xlcssa.else.rv
  br label %loop.exit.dedicated.rv

loop.exit.dedicated1.rv:                          ; preds = %loop.exit.dedicated.rv
  br label %tcp_parse_options.exit.loopexit.s.rv

tcp_parse_options.exit.loopexit.s.rv:             ; preds = %loop.exit.dedicated1.rv
  %.lcssa.R.b_SIMD = select <8 x i1> %edge_loop.exit.dedicated4.xlcssa.else.0_SIMD, <8 x %struct.tcp_timestamp_opt*> %.lcssa_SIMD, <8 x %struct.tcp_timestamp_opt*> %.lcssa8_SIMD
  br label %tcp_parse_options.exit.loopexit.rv

tcp_parse_options.exit.loopexit.rv:               ; preds = %tcp_parse_options.exit.loopexit.s.rv
  %41 = or <8 x i1> %edge_loop.exit.dedicated4.xlcssa.else.0_SIMD, %edge_loop.exit.dedicated4.xlcssa.else.1_SIMD
  br label %loop.exit.dedicated4.rv

tcp_parse_options.exit.s.rv:                      ; preds = %if.then17.i.rv
  %edge_if.end.i.0.not_SIMD = xor <8 x i1> %edge_if.end.i.0_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b6693_SIMD = and <8 x i1> %edge_if.end.i.0.not_SIMD, %41
  %.R68.b_SIMD = select <8 x i1> %edge_if.end.i.0_SIMD, <8 x %struct.tcp_timestamp_opt*> %27, <8 x %struct.tcp_timestamp_opt*> %.lcssa.R.b_SIMD
  br label %tcp_parse_options.exit.rv

tcp_parse_options.exit.rv:                        ; preds = %tcp_parse_options.exit.s.rv
  %42 = or <8 x i1> %.b6693_SIMD, %edge_if.end.i.0_SIMD
  %cmp39_SIMD = icmp eq <8 x %struct.tcp_timestamp_opt*> %.R68.b_SIMD, zeroinitializer
  %or4166_SIMD = or <8 x i1> %or3464_SIMD, %cmp39_SIMD
  %edge_tcp_parse_options.exit.0_SIMD = and <8 x i1> %42, %or4166_SIMD
  %neg.or4166_SIMD = xor <8 x i1> %or4166_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_tcp_parse_options.exit.1_SIMD = and <8 x i1> %42, %neg.or4166_SIMD
  br label %if.then42.s.rv

loop.exit.dedicated3.xlcssa.else.rv:              ; preds = %while.body.i.divexit.rv
  %neg.loop.exit.dedicated4.xlcssa_SIMD = xor <8 x i1> %loop.exit.dedicated4.xlcssa_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_loop.exit.dedicated3.xlcssa.else.1_SIMD = and <8 x i1> %edge_while.body.i.divexit.1_SIMD, %neg.loop.exit.dedicated4.xlcssa_SIMD
  %edge_loop.exit.dedicated3.xlcssa.else.0_SIMD = and <8 x i1> %edge_while.body.i.divexit.1_SIMD, %loop.exit.dedicated4.xlcssa_SIMD
  br label %loop.exit.dedicated4.xlcssa.else.rv

loop.exit.dedicated3.rv:                          ; preds = %loop.exit.dedicated4.rv
  br label %if.then42.loopexit.s.rv

loop.exit.dedicated4.xlcssa.else.rv:              ; preds = %loop.exit.dedicated3.xlcssa.else.rv
  %neg.loop.exit.dedicated1.xlcssa_SIMD = xor <8 x i1> %loop.exit.dedicated1.xlcssa_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %edge_loop.exit.dedicated4.xlcssa.else.1_SIMD = and <8 x i1> %edge_loop.exit.dedicated3.xlcssa.else.1_SIMD, %neg.loop.exit.dedicated1.xlcssa_SIMD
  %edge_loop.exit.dedicated4.xlcssa.else.0_SIMD = and <8 x i1> %edge_loop.exit.dedicated3.xlcssa.else.1_SIMD, %loop.exit.dedicated1.xlcssa_SIMD
  br label %loop.exit.dedicated1.xlcssa.else.rv

loop.exit.dedicated4.rv:                          ; preds = %tcp_parse_options.exit.loopexit.rv
  br label %loop.exit.dedicated3.rv

if.then42.loopexit.s.rv:                          ; preds = %loop.exit.dedicated3.rv
  %edge_while.body.i.divexit.0.not_SIMD = xor <8 x i1> %edge_while.body.i.divexit.0_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b6394_SIMD = and <8 x i1> %edge_while.body.i.divexit.0.not_SIMD, %edge_loop.exit.dedicated3.xlcssa.else.0_SIMD
  br label %if.then42.loopexit.rv

if.then42.loopexit.rv:                            ; preds = %if.then42.loopexit.s.rv
  %43 = or <8 x i1> %.b6394_SIMD, %edge_while.body.i.divexit.0_SIMD
  br label %if.then17.i.rv

if.then42.s.rv:                                   ; preds = %tcp_parse_options.exit.rv
  br label %if.then42.rv

if.then42.rv:                                     ; preds = %if.then42.s.rv
  %44 = or <8 x i1> %43, %edge_tcp_parse_options.exit.0_SIMD
  %45 = or <8 x i1> %44, %edge_while.cond.preheader.i.0_SIMD
  %46 = or <8 x i1> %45, %edge_lor.lhs.false.i.0_SIMD
  %47 = or <8 x i1> %46, %edge_if.end.0_SIMD
  call void @llvm.masked.scatter.v8p0i8.v8p0p0i8(<8 x i8*> zeroinitializer, <8 x i8**> %fss, i32 8, <8 x i1> %47)
  br label %cleanup.s.rv

cleanup.s.rv:                                     ; preds = %if.then42.rv
  %edge_tcp_parse_options.exit.1.not_SIMD = xor <8 x i1> %edge_tcp_parse_options.exit.1_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b8395_SIMD = and <8 x i1> %edge_tcp_parse_options.exit.1.not_SIMD, %47
  %edge_.0.not_SIMD = xor <8 x i1> %edge_.0_SIMD, <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>
  %.b8496_SIMD = and <8 x i1> %edge_.0.not_SIMD, %.b8395_SIMD
  br label %cleanup.rv

cleanup.rv:                                       ; preds = %cleanup.s.rv
  %48 = or <8 x i1> %.b8496_SIMD, %edge_tcp_parse_options.exit.1_SIMD
  %49 = or <8 x i1> %48, %edge_.0_SIMD
  ret void
}

; Function Attrs: convergent norecurse nounwind readnone
declare i1 @rv_entry_mask() #2

; Function Attrs: convergent norecurse nounwind readnone
declare i1 @rv_any(i1) #2

; Function Attrs: nounwind readonly willreturn
declare <8 x i8*> @llvm.masked.gather.v8p0i8.v8p0p0i8(<8 x i8**>, i32 immarg, <8 x i1>, <8 x i8*>) #3

; Function Attrs: nounwind readonly willreturn
declare <8 x i16> @llvm.masked.gather.v8i16.v8p0i16(<8 x i16*>, i32 immarg, <8 x i1>, <8 x i16>) #3

; Function Attrs: nounwind readonly willreturn
declare <8 x i8> @llvm.masked.gather.v8i8.v8p0i8(<8 x i8*>, i32 immarg, <8 x i1>, <8 x i8>) #3

; Function Attrs: nounwind willreturn
declare void @llvm.masked.scatter.v8p0s_struct.tcp_timestamp_opts.v8p0p0s_struct.tcp_timestamp_opts(<8 x %struct.tcp_timestamp_opt*>, <8 x %struct.tcp_timestamp_opt**>, i32 immarg, <8 x i1>) #4

; Function Attrs: nounwind readonly willreturn
declare <8 x i32> @llvm.masked.gather.v8i32.v8p0i32(<8 x i32*>, i32 immarg, <8 x i1>, <8 x i32>) #3

; Function Attrs: nounwind readnone willreturn
declare i1 @llvm.experimental.vector.reduce.or.v8i1(<8 x i1>) #5

; Function Attrs: nounwind willreturn
declare void @llvm.masked.scatter.v8p0i8.v8p0p0i8(<8 x i8*>, <8 x i8**>, i32 immarg, <8 x i1>) #4

attributes #0 = { nofree nounwind uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "frame-pointer"="none" "less-precise-fpmad"="false" "min-legal-vector-width"="0" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readnone speculatable willreturn }
attributes #2 = { convergent norecurse nounwind readnone }
attributes #3 = { nounwind readonly willreturn }
attributes #4 = { nounwind willreturn }
attributes #5 = { nounwind readnone willreturn }
attributes #6 = { nounwind }

!llvm.module.flags = !{!0}
!llvm.ident = !{!1}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{!"clang version 11.0.0 (https://github.com/llvm/llvm-project.git e3547ade68232d74bffd0e126cc0ea0b3970fbf7)"}
!2 = !{!3, !3, i64 0}
!3 = !{!"any pointer", !4, i64 0}
!4 = !{!"omnipotent char", !5, i64 0}
!5 = !{!"Simple C/C++ TBAA"}
!6 = !{!7, !3, i64 0}
!7 = !{!"rte_mbuf", !4, i64 0, !3, i64 0, !4, i64 8, !4, i64 16, !8, i64 16, !4, i64 18, !8, i64 20, !8, i64 22, !9, i64 24, !4, i64 32, !4, i64 32, !10, i64 36, !8, i64 40, !8, i64 42, !4, i64 44, !8, i64 52, !8, i64 54, !9, i64 56, !4, i64 64, !4, i64 64, !3, i64 72, !3, i64 80, !4, i64 88, !8, i64 96, !8, i64 98, !10, i64 100, !3, i64 104, !4, i64 112}
!8 = !{!"short", !4, i64 0}
!9 = !{!"long", !4, i64 0}
!10 = !{!"int", !4, i64 0}
!11 = !{!7, !8, i64 16}
!12 = !{!7, !8, i64 40}
!13 = !{!14, !4, i64 14}
!14 = !{!"pkt_tcp", !15, i64 0, !17, i64 14, !18, i64 34}
!15 = !{!"eth_hdr", !16, i64 0, !16, i64 6, !8, i64 12}
!16 = !{!"eth_addr", !4, i64 0}
!17 = !{!"ip_hdr", !4, i64 0, !4, i64 1, !8, i64 2, !8, i64 4, !8, i64 6, !4, i64 8, !4, i64 9, !8, i64 10, !10, i64 12, !10, i64 16}
!18 = !{!"tcp_hdr", !8, i64 0, !8, i64 2, !10, i64 4, !10, i64 8, !8, i64 12, !8, i64 14, !8, i64 16, !8, i64 18}
!19 = !{!14, !8, i64 12}
!20 = !{!14, !4, i64 23}
!21 = !{!14, !8, i64 46}
!22 = !{!14, !8, i64 16}
!23 = !{!24, !3, i64 0}
!24 = !{!"tcp_opts", !3, i64 0}
!25 = !{!10, !10, i64 0}
!26 = !{!4, !4, i64 0}
