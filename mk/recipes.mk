# Generic recipes

DEPFLAGS ?= -MT $@ -MMD -MP -MF $(@:.o=.Td)
OUTPUT_OPTION.c ?= -o $@
POSTCOMPILE_DEPS = mv -f $(@:.o=.Td) $(@:.o=.d)

# Compile C to object file while generating dependency
COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) -c
%.o: %.c
%.o: %.c %.d
	$(VECTORIZER) $< -m -o $(<:.c=.vc) -- $(CFLAGS) $(CPPFLAGS)
	$(COMPILE.c) $(OUTPUT_OPTION.c) -x c $(<:.c=.vc)
	@$(POSTCOMPILE_DEPS)

# Compile C to position independent object file while generating dependency
COMPILE_SHARED.c = $(CC) $(DEPFLAGS) $(CFLAGS_SHARED) $(CPPFLAGS) -c
%.shared.o: %.c
%.shared.o: %.c %.shared.d
	$(COMPILE_SHARED.c) $(OUTPUT_OPTION.c) $<
	@$(POSTCOMPILE_DEPS)

# Link binary from objects
LINK = $(CC) $(LDFLAGS)
%: %.o
	$(LINK) $^ $(LDLIBS) -o $@

# Link shared library from objects
LINK.so = $(CC) $(LDFLAGS) -shared
%.so:
	$(LINK.so) $^ $(LDLIBS) $(OUTPUT_OPTION.c)

%.d: ;
.PRECIOUS: %.d
