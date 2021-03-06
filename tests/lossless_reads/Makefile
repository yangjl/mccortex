SHELL:=/bin/bash -euo pipefail

#
# Isaac Turner 2014-10-08
#
# In this test we generate a random 1001 base sequence, add 9bp unique kmers
# either side and build graph and path files. Then, using k=9, we reassemble
# contigs from the graph+path, remove duplicates, trim off 9bp added each side
# and compare to original.
#

CTXDIR=../..
CTX=$(CTXDIR)/bin/ctx31
DNACAT=$(CTXDIR)/libs/seq_file/bin/dnacat
PERFECTCOVG=$(CTXDIR)/libs/seq_file/scripts/perfect_covg.sh
MAXIMALSUBSTR=$(CTXDIR)/libs/maximal_substrs/maxsubstrs

# 4**7 = 16,384
K=7
GENOME=10000
TGTS=seq.fa seq.uniq.fa reads.uniq.fa seq.uniq.k$(K).ctx seq.uniq.k$(K).ctp.gz seq.regen.fa

all: $(TGTS) check

seq.fa:
	$(DNACAT) -n $(GENOME) -F -M <(echo seq) > $@

seq.uniq.fa: seq.fa
	$(CTX) uniqkmers -m 10M -k $(K) -F $< 0 > $@

reads.uniq.fa: seq.uniq.fa
	maxrepeat=$$($(DNACAT) -r $< | $(DNACAT) -P $< - | $(MAXIMALSUBSTR) - | \
		           awk '{l=length($1); if(l>x){x=l}} END{print x}'); \
	readlen=$$[$$maxrepeat + 2]; \
	echo "Max repeat: $$maxrepeat Read length: $$readlen"; \
	$(PERFECTCOVG) $$readlen $< > $@

seq.uniq.k$(K).ctx: reads.uniq.fa
	$(CTX) build -m 10M -k $(K) --sample SeqUniq --seq $< $@

seq.uniq.k$(K).ctp.gz: seq.uniq.k$(K).ctx reads.uniq.fa
	$(CTX) thread --seq reads.uniq.fa --out $@ seq.uniq.k$(K).ctx

seq.regen.fa: seq.uniq.k$(K).ctx seq.uniq.k$(K).ctp.gz
	$(CTX) contigs -G $(GENOME) -o - -p seq.uniq.k$(K).ctp.gz seq.uniq.k$(K).ctx | \
	$(CTX) rmsubstr -n $$[1000+2*$(GENOME)] -k $(K) - | \
	$(DNACAT) -P - | awk '{print substr($$0,$(K)+1,length($$0)-$(K)*2)}'> $@

check: seq.fa seq.regen.fa seq.uniq.k7.ctx
	@echo Check kmer count distribution...
	$(CTX) view -q -k seq.uniq.k7.ctx | awk '{print $$2}' | sort -n | uniq -c
	@echo; echo Checking if regenerated file matches original...
	diff -q <($(DNACAT) -r -k -P seq.fa) <($(DNACAT) -r -k -P seq.regen.fa)
	@echo

clean:
	rm -rf $(TGTS)

.PHONY: all clean
