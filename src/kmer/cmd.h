#ifndef CMD_H_
#define CMD_H_

#include <getopt.h>

void get_long_opt(const struct option *longs, char shortopt, char *cmd);
void long_opts_to_short(const struct option *longs, char *opts);

uint8_t cmd_parse_arg_uint8(const char *cmd, const char *arg);
uint32_t cmd_parse_arg_uint32(const char *cmd, const char *arg);
uint32_t cmd_parse_arg_uint32_nonzero(const char *cmd, const char *arg);
size_t cmd_parse_arg_mem(const char *cmd, const char *arg);

#define CTXCMD "ctx"QUOTE_VALUE(MAX_KMER_SIZE)
#define CMD "ctx"QUOTE_VALUE(MAX_KMER_SIZE)

#define DEFAULT_NTHREADS 2
#define DEFAULT_MEM 1UL<<29 /*512MB*/
#define DEFAULT_NKMERS 1UL<<22 /*4Million*/

typedef struct
{
  char *cmdline;
  int cmdidx; // command specified
  bool print_help;
  // kmer, mem, ncols
  bool num_kmers_set, mem_to_use_set, num_threads_set, use_ncols_set;
  size_t num_kmers, mem_to_use, use_ncols;
  // Threads
  bool max_io_threads_set, max_work_threads_set;
  size_t max_io_threads, max_work_threads;
  // Input/output files
  bool output_file_set;
  char *output_file;
  // ctp files
  size_t num_ctp_files;
  char **ctp_files;
  // arguments not including command:
  int argc;
  char **argv;
} CmdArgs;

// Defaults
#define CMD_ARGS_INIT_MACRO { \
  .cmdline = NULL, .cmdidx = -1, .print_help = false, \
  .num_kmers_set = false, .num_kmers = DEFAULT_NKMERS, \
  .mem_to_use_set = false, .mem_to_use = DEFAULT_MEM, \
  .max_io_threads_set = false, .max_io_threads = 4, \
  .max_work_threads_set = false, .max_work_threads = DEFAULT_NTHREADS, \
  .use_ncols_set = false, .use_ncols = 1, \
  .output_file_set = false, .output_file = NULL, \
  .num_ctp_files = 0, .ctp_files = NULL, \
  .argc = 0, .argv = NULL}

void cmd_alloc(CmdArgs *args, int argc, char **argv);
void cmd_free(CmdArgs *args);

// Print memory being used
void cmd_print_mem(size_t mem_bytes, const char *name);

// accptopts is a string of valid args,
// e.g. "tk" accepts kmer-size and number of threads
// NULL means anything valid, "" means no args valid
void cmd_accept_options(const CmdArgs *args, const char *accptopts,
                        const char *usage);
void cmd_require_options(const CmdArgs *args, const char *requireopts,
                         const char *usage);

// If your command accepts -n <kmers> and -m <mem> this may be useful
// extra_bits_per_kmer is additional memory per node, above hash table for
// BinaryKmers
// Resulting graph_mem is always < args->mem_to_use
// min_num_kmers and max_num_kmers are kmers that need to be held in the graph
// (i.e. min_num_kmers/IDEAL_OCCUPANCY)
size_t cmd_get_kmers_in_hash2(size_t mem_to_use, bool mem_to_use_set,
                              size_t num_kmers, bool num_kmers_set,
                              size_t extra_bits,
                              size_t min_num_kmer_req, size_t max_num_kmers_req,
                              bool use_mem_limit, size_t *graph_mem_ptr);

size_t cmd_get_kmers_in_hash(const CmdArgs *args, size_t extra_bits_per_kmer,
                             size_t min_num_kmers, size_t max_num_kmers,
                             bool use_mem_limit, size_t *graph_mem_ptr);

// Check memory against args->mem_to_use and total RAM
void cmd_check_mem_limit(size_t mem_to_use, size_t mem_requested);

// Once we have set cmd_usage, we can call cmd_print_usage() from anywhere
extern const char *cmd_usage;

void cmd_print_usage(const char *errfmt,  ...)
  __attribute__((noreturn))
  __attribute__((format(printf, 1, 2)));

#endif