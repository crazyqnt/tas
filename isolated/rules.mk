include mk/subdir_pre.mk

isolated/fastpath: CPPFLAGS+= -Itas/include $(DPDK_CPPFLAGS)
isolated/fastpath: CFLAGS+= $(DPDK_CFLAGS)
isolated/fastpath: LDFLAGS+= $(DPDK_LDFLAGS)
isolated/fastpath: LDLIBS+= -lrte_eal
isolated/fastpath: isolated/fastpath.o tas/fast/fast_flows.o

DEPS += isolated/fastpath.d
CLEAN += isolated/fastpath.o

include mk/subdir_post.mk
