SHELL:=/bin/bash -euo pipefail

#
# s0: CCCGTAGGTAAGGGCGTTAGTGCAAGGCCACATTGGGACACGAGTTGATA
#     gCCGTAGGTAAGGGCGTTAGTGC
#
# s1: GAACACCCTTTGGTTTAAGCCGGGTTGGAGTTGGCCAAAGAAGTTCAACG
#
# mx: gCCGTAGGTAAGGGCGTTAGaCGGGTTGGAGTTGGCCAAAGAAGTTCAAgg
#     .1111111111111111111.2222222222222222222222222222..
#
#     repeat of 5bp
#     gCCGTAGGTAAGGGCGTTAGTGCAGTGCAAGGCCACATTGGGACACGAGTTGAgg
#     111111111111111111111111xxxxx111111111111111111111111..
#     deletion of 1bp
#     cAACACCCTTTGGTTTAAGCCG-GTTGGAGTTGGCCAAAGAAGTTCAAgg
#     2222222222222222222222 2222222222222222222222222..

K=11
CTXDIR=../..
CTX=$(CTXDIR)/bin/ctx31
CTX2DOT=$(CTXDIR)/scripts/cortex_to_graphviz.pl
BRKCHCK=$(CTXDIR)/scripts/cortex_brkpnts_check_ref.pl

SEQS=seq0.fa seq1.fa mix.fa
GRAPHS=$(SEQS:.fa=.k$(K).ctx)
FLAT_GRAPHS=$(shell printf "0:%s\n" $(GRAPHS))
TGTS=breakpoints.txt breakpoints.vcf $(GRAPHS)
# join.k$(K).ctx

all: $(TGTS) check

seq0.fa:
	(echo '>seq0a'; \
	 echo CCCGTAGGTAAGGGCGTTAGTGCAAGGCCACATTGGGACACGAGTTGATA; \
	 echo '>seq0b'; \
	 echo gCCGTAGGTAAGGGCGTTAGTGC) > seq0.fa

seq1.fa:
	(echo '>seq1a'; \
	 echo GAACACCCTTTGGTTTAAGCCGGGTTGGAGTTGGCCAAAGAAGTTCAACG) > seq1.fa

mix.fa:
	(echo '>mix1'; \
	 echo gCCGTAGGTAAGGGCGTTAGaCGGGTTGGAGTTGGCCAAAGAAGTTCAAgg; \
	 echo '>mix2'; \
	 echo gCCGTAGGTAAGGGCGTTAGTGCAGTGCAAGGCCACATTGGGACACGAGTTGAgg; \
	 echo '>mix3'; \
	 echo cAACACCCTTTGGTTTAAGCCGGTTGGAGTTGGCCAAAGAAGTTCAAgg;) > mix.fa

%.k$(K).ctx: %.fa
	$(CTX) build -m 10M -k $(K) --sample $< --seq $< $@

breakpoints.txt: $(GRAPHS) $(SEQS)
	$(CTX) breakpoints -t 1 -m 10M --minref 5 --maxref 20 --seq seq0.fa --seq seq1.fa --out breakpoints.txt.gz mix.k$(K).ctx
	gzip -d breakpoints.txt.gz

breakpoints.vcf: breakpoints.txt $(SEQS)
	$(CTX) calls2vcf -o $@ breakpoints.txt seq0.fa seq1.fa

check: breakpoints.txt seq0.fa seq1.fa
	cat breakpoints.txt
	$(BRKCHCK) breakpoints.txt seq0.fa seq1.fa

join.k$(K).ctx: $(GRAPHS)
	$(CTX) join -o $@ $(FLAT_GRAPHS)

seq.k$(K).pdf: join.k$(K).ctx
	$(CTX2DOT) --simplify $< | dot -Tpdf > $@

plots: seq.k$(K).pdf

clean:
	rm -rf $(TGTS) $(SEQS) breakpoints.txt.gz seq.k$(K).pdf join.k$(K).ctx

.PHONY: all clean plots check
