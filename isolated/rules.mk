include mk/subdir_pre.mk

isolated/vecmod.o: isolated/vecmod.ll
	llc -march=x86-64 -mcpu=skylake-avx512 -filetype=obj --relocation-model=pic isolated/vecmod.ll

isolated/fastpath: CPPFLAGS+= -Itas/include $(DPDK_CPPFLAGS)
isolated/fastpath: CFLAGS+= $(DPDK_CFLAGS)
isolated/fastpath: LDFLAGS+= $(DPDK_LDFLAGS)
isolated/fastpath: LDLIBS+= -lrte_eal
isolated/fastpath: isolated/fastpath.o tas/fast/fast_flows.o isolated/vecmod.o

DEPS += isolated/fastpath.d
CLEAN += isolated/fastpath.o

include mk/subdir_post.mk
