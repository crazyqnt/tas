-include Makefile.local

#
# Makefile.local is expected to provide:
# $INTRINHELPER => folder containing intrinhelper.h of the vectorizer
# $DPDK_INST => location of DPDK installation headers
# $VECTORIZER => location of the vectorizer binary
#

CXXFLAGS=-I$(INTRINHELPER) -I$(DPDK_INST) -Itas/fast -Itas/include -Iinclude
CXXFLAGS+= -ferror-limit=30

test1:
	$(VECTORIZER) tas/fast/fast_flows.c -- $(CXXFLAGS)
