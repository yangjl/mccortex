#include "global.h"
#include "all_tests.h"
#include "db_graph.h"
#include "db_node.h"
#include "build_graph.h"

#define read_set(r,s) do { \
  size_t _l = strlen(s); memcpy((r).seq.b,s,_l+1); (r).seq.end = _l; \
} while(0)

static Covg kmer_get_covg(const char *kmer, const dBGraph *db_graph)
{
  BinaryKmer bkmer = binary_kmer_from_str(kmer, db_graph->kmer_size);
  dBNode node = db_graph_find(db_graph, bkmer);
  return db_node_get_covg(db_graph, node.key, 0);
}

void test_build_graph()
{
  test_status("[build_graph] testing remove PCR duplicates...");

  // Construct 1 colour graph with kmer-size=11
  dBGraph graph;
  size_t kmer_size = 19, ncols = 1;

  db_graph_alloc(&graph, kmer_size, ncols, ncols, 1024);
  // Graph data
  graph.bktlocks = calloc2(roundup_bits2bytes(graph.ht.num_of_buckets), 1);
  graph.col_edges = calloc2(graph.ht.capacity * ncols, sizeof(Edges));
  graph.col_covgs = calloc2(graph.ht.capacity * ncols, sizeof(Covg));

  // 1 bit for forward, 1 bit for reverse per kmer
  graph.readstrt = calloc2(roundup_bits2bytes(graph.ht.capacity)*2,
                           sizeof(uint8_t));

  char empty[10] = "";
  char *seq1 = malloc2(1024), *seq2 = malloc2(1024);
  read_t r1 = {.name = {.b = empty, .end = 0, .size = 0},
               .seq  = {.b = seq1, .end = 0, .size = 0},
               .qual = {.b = empty, .end = 0, .size = 0}};

  read_t r2 = {.name = {.b = empty, .end = 0, .size = 0},
               .seq  = {.b = seq2, .end = 0, .size = 0},
               .qual = {.b = empty, .end = 0, .size = 0}};

  LoadingStats stats = LOAD_STATS_INIT_MACRO;

  // Test loading empty reads are ok
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_FF,
                         &stats, 0, &graph);

  // Test FF reads dups are removed
  read_set(r1, "CTACGATGTATGCTTAGCTGTTCCG");
  read_set(r2, "TAGAACGTTCCCTACACGTCCTATG");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_FF,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 1);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 1);

  // Check we filter out a duplicate FF
  read_set(r1, "CTACGATGTATGCTTAGCTAATGAT");
  read_set(r2, "TAGAACGTTCCCTACACGTTGTTTG");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_FF,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 1);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 1);

  // Check we filter out a duplicate FR
  // revcmp TAGAACGTTCCCTACACGT -> AGCTAAGCATACATCGTAG
  read_set(r1, "CTACGATGTATGCTTAGCTCCGAAG");
  read_set(r2, "AGACTAAGCTAAGCATACATCGTAG");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_FR,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 1);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 1);

  // Check we filter out a duplicate RF
  // revcmp CTACGATGTATGCTTAGCT -> ACGTGTAGGGAACGTTCTA
  read_set(r1, "AGGAGTTGTCTTCTAAGGAAACGTGTAGGGAACGTTCTA");
  read_set(r2, "TAGAACGTTCCCTACACGTTTTCCACGAGTTAATCTAAG");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_RF,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 1);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 1);

  // Check we filter out a duplicate RF
  // revcmp CTACGATGTATGCTTAGCT -> ACGTGTAGGGAACGTTCTA
  // revcmp TAGAACGTTCCCTACACGT -> AGCTAAGCATACATCGTAG
  read_set(r1, "AACCCTAAAAACGTGTAGGGAACGTTCTA");
  read_set(r2, "AATGCGTGTTAGCTAAGCATACATCGTAG");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_RR,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 1);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 1);

  // Check add a duplicate when filtering is turned off
  read_set(r1, "CTACGATGTATGCTTAGCTAATGAT");
  read_set(r2, "TAGAACGTTCCCTACACGTTGTTTG");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, false, READPAIR_FF,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 2);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 2);

  // Check SE duplicate removal with FF reads
  read_set(r1, "CTACGATGTATGCTTAGCTAGTGTGATATCCTCC");
  build_graph_from_reads(&r1, NULL, 0, 9, 9, 9, true, false, READPAIR_FF,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 2);

  // Check SE duplicate removal with RR reads
  read_set(r1, "GCGTTACCTACTGACAGCTAAGCATACATCGTAG");
  build_graph_from_reads(&r1, NULL, 0, 9, 9, 9, true, false, READPAIR_RR,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 2);

  // Check we don't filter out reads when kmers in opposite direction
  // revcmp CTACGATGTATGCTTAGCT -> ACGTGTAGGGAACGTTCTA
  // revcmp TAGAACGTTCCCTACACGT -> AGCTAAGCATACATCGTAG
  read_set(r1, "ACGTGTAGGGAACGTTCTA""CTTCTACCGGAGGAT");
  read_set(r2, "AGCTAAGCATACATCGTAG""TACAATGCACCCTCC");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_FF,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 3);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 3);

  // shouldn't work a second time
  // revcmp CTACGATGTATGCTTAGCT -> ACGTGTAGGGAACGTTCTA
  // revcmp TAGAACGTTCCCTACACGT -> AGCTAAGCATACATCGTAG
  read_set(r1, "ACGTGTAGGGAACGTTCTA""CTTCTACCGGAGGAT");
  read_set(r2, "AGCTAAGCATACATCGTAG""TACAATGCACCCTCC");
  build_graph_from_reads(&r1, &r2, 0, 9, 9, 9, false, true, READPAIR_FF,
                         &stats, 0, &graph);
  TASSERT(kmer_get_covg("CTACGATGTATGCTTAGCT", &graph) == 3);
  TASSERT(kmer_get_covg("TAGAACGTTCCCTACACGT", &graph) == 3);

  free(seq1);
  free(seq2);
  free(graph.readstrt);
  free(graph.bktlocks);
  free(graph.col_edges);
  free(graph.col_covgs);
  db_graph_dealloc(&graph);
}