#include "global.h"

#include "cmd.h"
#include "util.h"
#include "file_util.h"
#include "db_graph.h"
#include "add_read_paths.h"
#include "binary_format.h"
#include "path_format.h"
#include "graph_walker.h"
#include "bubble_caller.h"

static const char usage[] =
"usage: "CMD" thread [OPTIONS] <in.ctx>\n"
"  Thread reads through the graph.  Saves to file <in.ctp>\n"
"  Options:\n"
"    --se_list <col> <in.list>\n"
"    --pe_list <col> <pe.list1> <pe.list2>\n";

#define NUM_PASSES 1

int ctx_thread(CmdArgs *args)
{
  int argc = args->argc;
  char **argv = args->argv;
  if(argc < 1) print_usage(usage, NULL);

  uint32_t num_of_threads = args->num_threads;
  size_t mem_to_use = args->mem_to_use;
  if(!args->mem_to_use_set) print_usage(usage, "-m <M> required");

  char *input_ctx_path = argv[argc-1];

  // Check arguments
  if(!test_file_readable(input_ctx_path))
    print_usage(usage, "Cannot read input file: %s", input_ctx_path);

  // Set up output path
  char *out_path = malloc(strlen(input_ctx_path)+4);
  paths_format_filename(input_ctx_path, out_path);

  if(!test_file_writable(out_path))
    print_usage(usage, "Cannot write output file: %s", out_path);

  uint32_t col;
  int argi, argend = argc - 3;
  for(argi = 1; argi < argend; argi++) {
    if(strcmp(argv[argi], "--se_list") == 0)
    {
      if(argi+2 >= argend)
        print_usage(usage, "--se_list <col> <input.falist> missing args");

      if(!parse_entire_uint(argv[argi+1], &col))
        print_usage(usage, "--se_list <col> <input.falist> invalid colour");

      check_colour_or_ctx_list(argv[argi+2], false, false, true, 0, NULL);
      argi += 2;
    }
    else if(strcmp(argv[argi], "--pe_list") == 0)
    {
      if(argi+3 >= argend)
        print_usage(usage, "--pe_list <col> <in1.list> <in2.list> missing args");

      if(!parse_entire_uint(argv[argi+1], &col))
        print_usage(usage, "--pe_list <col> <in1.list> <in2.list> invalid colour");

      uint32_t files1, files2;
      files1 = check_colour_or_ctx_list(argv[argi+2], false, false, true, 0, NULL);
      files2 = check_colour_or_ctx_list(argv[argi+3], false, false, true, 0, NULL);
      if(files1 != files2)
        die("list mismatch [%s; %s]", argv[argi+2], argv[argi+3]);
      argi += 3;
    }
    else print_usage(usage, "Unknown argument: %s", argv[argi]);
  }

  // Probe binary to get kmer_size
  boolean is_binary = false;
  uint32_t kmer_size, num_of_cols;
  uint64_t num_kmers;

  if(!binary_probe(input_ctx_path, &is_binary, &kmer_size, &num_of_cols, &num_kmers))
    print_usage(usage, "Cannot read binary file: %s", input_ctx_path);
  else if(!is_binary)
    print_usage(usage, "Input binary file isn't valid: %s", input_ctx_path);

  // Decide on memory
  size_t hash_kmers, req_num_kmers = num_kmers*(1.0/IDEAL_OCCUPANCY);
  size_t hash_mem = hash_table_mem(req_num_kmers, &hash_kmers);

  size_t graph_mem = hash_mem +
                     hash_kmers * sizeof(Edges) + // edges
                     hash_kmers * sizeof(uint64_t) + // kmer_paths
                     round_bits_to_bytes(hash_kmers) * num_of_cols; // in col

  // visited fw/rv
  size_t thread_mem = round_bits_to_bytes(hash_kmers) * 2 * num_of_threads;

  if(graph_mem+thread_mem > mem_to_use) {
    print_usage(usage, "Not enough memory; hash table: %zu; threads: %zu",
                graph_mem, thread_mem);
  }

  // Allocate memory
  dBGraph db_graph;
  db_graph_alloc(&db_graph, kmer_size, num_of_cols, hash_kmers);

  size_t path_mem = mem_to_use - graph_mem - thread_mem;

  char graph_mem_str[100], per_thread_mem_str[100], path_mem_str[100];
  bytes_to_str(graph_mem, 1, graph_mem_str);
  bytes_to_str(thread_mem / num_of_threads, 1, per_thread_mem_str);
  bytes_to_str(path_mem, 1, path_mem_str);

  message("[memory]  graph: %s;  threads: %i x %s;  paths: %s\n",
          graph_mem_str, num_of_threads, per_thread_mem_str, path_mem_str);

  // Edges
  db_graph.edges = calloc(hash_kmers, sizeof(uint8_t));
  if(db_graph.edges == NULL) die("Out of memory");

  // In colour - used is traversal
  size_t words64_per_col = round_bits_to_words64(hash_kmers);
  db_graph.node_in_cols = calloc(words64_per_col*num_of_cols, sizeof(uint64_t));
  if(db_graph.node_in_cols == NULL) die("Out of memory");

  // Paths
  db_graph.kmer_paths = malloc(hash_kmers * sizeof(uint64_t));
  if(db_graph.kmer_paths == NULL) die("Out of memory");
  memset((void*)db_graph.kmer_paths, 0xff, hash_kmers * sizeof(uint64_t));

  uint8_t *path_store = malloc(path_mem);
  if(path_store == NULL) die("Out of memory");
  binary_paths_init(&db_graph.pdata, path_store, path_mem, num_of_cols);

  // Load graph
  SeqLoadingStats *stats = seq_loading_stats_create(0);
  SeqLoadingPrefs prefs = {.into_colour = 0, .merge_colours = false,
                           .load_seq = false,
                           .quality_cutoff = 0, .ascii_fq_offset = 0,
                           .homopolymer_cutoff = 0,
                           .remove_dups_se = false, .remove_dups_pe = false,
                           .load_binaries = true,
                           .must_exist_in_colour = -1,
                           .empty_colours = false,
                           .update_ginfo = true,
                           .db_graph = &db_graph};

  binary_load(input_ctx_path, &db_graph, &prefs, stats);
  hash_table_print_stats(&db_graph.ht);

  prefs.load_seq = true;
  prefs.load_binaries = false;

  // Parse input sequence
  size_t rep;
  for(rep = 0; rep < NUM_PASSES; rep++)
  {
    for(argi = 1; argi < argend; argi++) {
      if(strcmp(argv[argi], "--se_list") == 0) {
        parse_entire_uint(argv[argi+1], &col);
        add_read_paths_to_graph(argv[argi+2], NULL, NULL, col, NULL, 0,
                                prefs, num_of_threads);
        argi += 2;
      }
      else if(strcmp(argv[argi], "--pe_list") == 0) {
        parse_entire_uint(argv[argi+1], &col);
        add_read_paths_to_graph(NULL, argv[argi+2], argv[argi+3], col,
                                NULL, 0, prefs, num_of_threads);
        argi += 3;
      }
      else die("Unknown arg: %s", argv[argi]);
    }
  }

  paths_format_write(&db_graph, &db_graph.pdata, out_path);

  free(db_graph.edges);
  free(db_graph.node_in_cols);
  free((void *)db_graph.kmer_paths);
  free(path_store);

  seq_loading_stats_free(stats);
  db_graph_dealloc(&db_graph);

  message("  Paths written to: %s\n", out_path);
  message("Done.\n");
  free(out_path);

  return EXIT_SUCCESS;
}
