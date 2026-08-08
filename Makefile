VARIANTS := \
	mpaic-arith \
	mpaic-arith-mt \
	mpaic-range \
	mpaic-range-mt

.PHONY: all clean $(VARIANTS)

all: $(VARIANTS)

$(VARIANTS):
	$(MAKE) -C $@

clean:
	@set -e; for variant in $(VARIANTS); do $(MAKE) -C "$$variant" clean; done
