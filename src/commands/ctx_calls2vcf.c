#include "global.h"
#include "commands.h"
#include "util.h"
#include "file_util.h"
#include "db_graph.h"
#include "seq_reader.h"
#include "call_file_reader.h"
#include "json_hdr.h"
#include "chrom_pos_list.h" // Parse chromosome position lists

#include "sam.h"
#include "seq-align/src/needleman_wunsch.h"

#define DEFAULT_MIN_MAPQ 30 /* min MAPQ considered */
#define DEFAULT_MAX_ALEN 500 /* max allele length */
#define DEFAULT_MAX_PDIFF 500 /* max allele length */

const char calls2vcf_usage[] =
"usage: "CMD" calls2vcf [options] <in.txt.gz> <ref.fa> [ref2.fa ...]\n"
"\n"
"  Convert a bubble or breakpoint call file to VCF. If input is a bubble file\n"
"  the --mapped <flanks.sam> argument is required.\n"
"\n"
"  -h, --help             This help message\n"
"  -q, --quiet            Silence status output normally printed to STDERR\n"
"  -f, --force            Overwrite output files\n"
"  -o, --out <out.txt>    Save output graph file [default: STDOUT]\n"
"\n"
"  -F, --flanks <in.bam>  Mapped flanks in SAM or BAM file\n"
"  -Q, --min-mapq <Q>     Flank must map with MAPQ >= <Q> [default: "QUOTE_VALUE(DEFAULT_MIN_MAPQ)"]\n"
"  -A, --max-allele <M>   Max allele length considered [default: "QUOTE_VALUE(DEFAULT_MAX_ALEN)"]\n"
"  -D, --max-diff <D>     Max difference in path lengths [default: "QUOTE_VALUE(DEFAULT_MAX_PDIFF)"]\n"
"\n"
"  Alignment scoring:\n"
"  -m, --match <m>       [default:  1]\n"
"  -M, --mismatch <m>    [default: -2]\n"
"  -g, --gap-open <m>    [default: -4]\n"
"  -G, --gap-extend <m>  [default: -1]\n"
"\n";

static struct option longopts[] =
{
// General options
  {"help",         no_argument,       NULL, 'h'},
  {"out",          required_argument, NULL, 'o'},
  {"force",        no_argument,       NULL, 'f'},
// command specific
  {"flanks",       required_argument, NULL, 'F'},
  {"min-mapq",     required_argument, NULL, 'Q'},
  {"max-allele",   required_argument, NULL, 'A'},
  {"max-diff",     required_argument, NULL, 'D'},
// alignment
  {"match",        required_argument, NULL, 'm'},
  {"mismatch",     required_argument, NULL, 'M'},
  {"gap-open",     required_argument, NULL, 'g'},
  {"gap-extend",   required_argument, NULL, 'G'},
  {NULL, 0, NULL, 0}
};

//
// Parameters
//
static const char *input_path = NULL;
static const char *out_path = NULL, default_out_path[] = "-";
// Filtering parameters
static size_t min_mapq = SIZE_MAX;
static size_t max_allele_len = SIZE_MAX;
static size_t max_path_diff = SIZE_MAX;
// Alignment parameters
static int nwmatch = 1, nwmismatch = -2, nwgapopen = -4, nwgapextend = -1;
// ref paths
static char **ref_paths;
static size_t num_ref_paths = 0;
// flank file
const char *sam_path = NULL;

//
// Things we figure out by looking at the input
//
bool input_bubble_format = false;
size_t kmer_size, num_samples;

//
// Reference genome
//
// Hash map of chromosome name -> sequence
static khash_t(ChromHash) *genome;
static ReadBuffer chroms;

// Flank mapping
static samFile *samfh;
static bam_hdr_t *bam_header;
static bam1_t *bam;

// nw alignment
static nw_aligner_t *nw_aligner;
static alignment_t *aln;
static scoring_t nw_scoring_flank, nw_scoring_allele;

//
// Statistics
//
// VCF printing stats
static size_t num_entries_read = 0, num_entries_well_mapped = 0, num_vars_printed = 0;

// Bubble statistics
static size_t num_flank5p_unmapped = 0, num_flank5p_lowqual = 0;
static size_t num_flank3p_multihits = 0, num_flank3p_approx_match = 0;
static size_t num_flank3p_not_found = 0;

// Breakpoint statistics
static size_t num_flanks_not_uniquely_mapped = 0;
static size_t num_flanks_diff_chroms = 0;
static size_t num_flanks_diff_strands = 0;

// Both
static size_t num_flanks_overlap_too_large = 0;
static size_t num_flanks_too_far_apart = 0;

// Processing
static size_t num_nw_allele = 0, num_nw_flank = 0;

static void print_stat(size_t nom, size_t denom, const char *descr)
{
  char nom_str[50], denom_str[50];
  ulong_to_str(nom,   nom_str);
  ulong_to_str(denom, denom_str);
  status("   %s / %s (%6.2f%%) %s", nom_str, denom_str, (100.0*nom)/denom, descr);
}

static void parse_cmdline_args(int argc, char **argv)
{
  // Arg parsing
  char cmd[100];
  char shortopts[300];
  cmd_long_opts_to_short(longopts, shortopts, sizeof(shortopts));
  int c;

  // silence error messages from getopt_long
  // opterr = 0;

  while((c = getopt_long_only(argc, argv, shortopts, longopts, NULL)) != -1) {
    cmd_get_longopt_str(longopts, c, cmd, sizeof(cmd));
    switch(c) {
      case 0: /* flag set */ break;
      case 'h': cmd_print_usage(NULL); break;
      case 'o': cmd_check(!out_path, cmd); out_path = optarg; break;
      case 'f': cmd_check(!futil_get_force(), cmd); futil_set_force(true); break;
      case 'F': cmd_check(!sam_path,cmd); sam_path = optarg; break;
      case 'Q': cmd_check(min_mapq == SIZE_MAX,cmd); min_mapq = cmd_uint32(cmd, optarg); break;
      case 'A': cmd_check(max_allele_len == SIZE_MAX,cmd); max_allele_len = cmd_uint32(cmd, optarg); break;
      case 'D': cmd_check(max_path_diff == SIZE_MAX, cmd); max_path_diff = cmd_uint32(cmd, optarg); break;
      case 'm': nwmatch = cmd_int32(cmd, optarg); break;
      case 'M': nwmismatch = cmd_int32(cmd, optarg); break;
      case 'g': nwgapopen = cmd_int32(cmd, optarg); break;
      case 'G': nwgapextend = cmd_int32(cmd, optarg); break;
      case ':': /* BADARG */
      case '?': /* BADCH getopt_long has already printed error */
        // cmd_print_usage(NULL);
        die("`"CMD" calls2vcf -h` for help. Bad option: %s", argv[optind-1]);
      default: ctx_assert2(0, "shouldn't reach here: %c", c);
    }
  }

  // Defaults for unset values
  if(out_path == NULL) out_path = default_out_path;
  if(min_mapq == SIZE_MAX) min_mapq = DEFAULT_MIN_MAPQ;
  if(max_allele_len == SIZE_MAX) max_allele_len = DEFAULT_MAX_ALEN;
  if(max_path_diff == SIZE_MAX) max_path_diff = DEFAULT_MAX_PDIFF;

  if(optind+2 > argc)
    cmd_print_usage("Require <in.txt.gz> and at least one reference");

  input_path = argv[optind];
  optind++;
  ref_paths = argv + optind;
  num_ref_paths = argc - optind;
}

// Setup pairwise aligner
static void nw_aligner_setup()
{
  nw_aligner = needleman_wunsch_new();
  aln = alignment_create(1024);
  scoring_init(&nw_scoring_flank, nwmatch, nwmismatch, nwgapopen, nwgapextend,
               true, true, 0, 0, 0, 0);
  scoring_init(&nw_scoring_allele, nwmatch, nwmismatch, nwgapopen, nwgapextend,
               false, false, 0, 0, 0, 0);
}

// Clean up pairwise aligner
static void nw_aligner_destroy()
{
  alignment_free(aln);
  needleman_wunsch_free(nw_aligner);
}

static const read_t* fetch_chrom(const char *chrom_name)
{
  // Fetch chromosome
  khiter_t k = kh_get(ChromHash, genome, chrom_name);
  if(k == kh_end(genome)) die("Cannot find chrom [%s]", chrom_name);
  const read_t *chr = kh_value(genome, k);
  return chr;
}

static size_t call_file_max_allele_len(const CallFileEntry *centry)
{
  size_t i, max = 0, nlines = call_file_num_lines(centry);
  for(i = 5; i < nlines; i+=2)
    max = MAX2(max, call_file_line_len(centry, i));
  return max;
}

static size_t call_file_min_allele_len(const CallFileEntry *centry)
{
  size_t i, min = SIZE_MAX, nlines = call_file_num_lines(centry);
  for(i = 5; i < nlines; i+=2)
    min = MIN2(min, call_file_line_len(centry, i));
  return min;
}

static const char* str_fasta_name_end(const char *title)
{
  while(!isspace(*title)) title++;
  return title;
}

static void bubble_get_end_kmer(const char *flank5p, size_t flank5p_len,
                                const char *flank3p, size_t flank3p_len,
                                size_t ksize, char *endkmer)
{
  // 3p flank may not be long enough to give kmer bases
  size_t flank3pcpy = MIN2(ksize, flank3p_len);
  size_t flank5pcpy = ksize - flank3pcpy; // Make up remaining sequence
  ctx_assert(flank5p_len <= flank5pcpy);

  memcpy(endkmer,            flank5p, flank5pcpy);
  memcpy(endkmer+flank5pcpy, flank3p, flank3pcpy);
  endkmer[ksize] = '\0';
}

static bool sam_fetch_coords(const CallFileEntry *centry,
                             const char *flank5p, size_t flank5p_len,
                             const char *flank3p, size_t flank3p_len,
                             const read_t **chrom,
                             size_t *start, size_t *end,
                             bool *fw_strand)
{
  if(sam_read1(samfh, bam_header, bam) < 0) die("We've run out of SAM entries!");

  if(bam->core.flag & BAM_FUNMAP) { num_flank5p_unmapped++; return false; }
  if(bam->core.qual < min_mapq)   { num_flank5p_lowqual++;  return false; }

  const char *chrom_name = bam_header->target_name[bam->core.tid];
  *chrom = fetch_chrom(chrom_name);
  *fw_strand = !bam_is_rev(bam);

  int cigar2rlen = bam_cigar2rlen(bam->core.n_cigar, bam_get_cigar(bam));

  // Get bam query name
  char *bname = bam_get_qname(bam);

  // Check entry/flank names match
  const char *hdrline = call_file_get_line(centry, 0);
  if(hdrline[0] != '>') die("Unexpected line: %s", hdrline);
  const char *hdrline_end = str_fasta_name_end(hdrline);
  if(strncmp(hdrline+1,bname, hdrline_end - hdrline) != 0)
    die("SAM/BAM and call entries mismatch '%s' vs '%s'", bname, hdrline);

  // Find 3p flank position using search for first kmer
  char endkmer[200];
  ctx_assert(kmer_size < sizeof(endkmer));
  ctx_assert(flank3p_len >= kmer_size || call_file_min_allele_len(centry) == 0);
  bubble_get_end_kmer(flank5p, flank5p_len, flank3p, flank3p_len, kmer_size, endkmer);
  if(bam_is_rev(bam)) dna_revcomp_str(endkmer, endkmer, kmer_size);

  // Determine search space
  // Choose a region of the ref to search for the end flank
  // end is index after last char
  long search_start, search_end;
  size_t longest_allele = call_file_max_allele_len(centry);

  if(bam_is_rev(bam)) {
    search_start = (long)bam->core.pos - (longest_allele + kmer_size*2 + 10);
    search_end = (long)bam->core.pos + kmer_size*2;
  } else {
    search_start = (long)bam->core.pos + cigar2rlen - kmer_size*2;
    search_end = (long)bam->core.pos + cigar2rlen + longest_allele + kmer_size*2 + 10;
  }

  search_start = MAX2(search_start, 0);
  search_end   = MIN2(search_end,   (long)(*chrom)->seq.end);

  char *search_region = (*chrom)->seq.b+search_start;
  size_t search_len = (size_t)(search_end - search_start);

  // Now do search with kmer
  // Attempt to find perfect match for kmer within search region
  // temporarily null terminate ref
  char save_ref_base = search_region[search_len];
  search_region[search_len] = '\0';

  // Search, if there is more than one match -> abandon
  char *kmer_match = strstr(search_region, endkmer);
  bool multiple_hits = (kmer_match && strstr(kmer_match+1, endkmer) != NULL);

  // Restore last base
  search_region[search_len] = save_ref_base;

  if(multiple_hits) { num_flank3p_multihits++; return false; }

  if(kmer_match != NULL) {
    if(bam_is_rev(bam)) {
      *start = kmer_match - (*chrom)->seq.b;
      *end   = bam->core.pos;
    } else {
      *start = bam->core.pos + cigar2rlen - 1;
      *end   = kmer_match + kmer_size - 1 - (*chrom)->seq.b;
    }
    return true;
  } else {
    // Look for approximate match
    needleman_wunsch_align2(search_region, endkmer, search_len, kmer_size,
                            &nw_scoring_flank, nw_aligner, aln);
    num_nw_flank++;
    const char *ref = aln->result_a, *alt = aln->result_b;
    // --aa--cc-cge
    // aa--ccd-dcge

    // Find positions of first and last match
    size_t i, l, r, matches = 0;
    size_t ref_offset_left = 0, ref_offset_rght = 0;

    for(l = 0; l < aln->length && (ref[l] == '-' || alt[l] == '-'); l++) {
      ref_offset_left += (ref[l] != '-');
    }
    for(r = aln->length-1; r != SIZE_MAX && (ref[l] == '-' || alt[l] == '-'); r--) {
      ref_offset_rght += (ref[l] != '-');
    }

    for(i = l; i <= r; i++) matches += (ref[i] == alt[i]);

    if(matches < kmer_size / 2)
    {
      // flank doesn't map well
      num_flank3p_not_found++;
      return false;
    }

    num_flank3p_approx_match++;

    if(bam_is_rev(bam)) {
      *start = search_region + ref_offset_left - (*chrom)->seq.b;
      *end   = bam->core.pos;
    } else {
      *start = bam->core.pos + cigar2rlen - 1;
      *end   = search_region + search_len - 1 - ref_offset_rght - (*chrom)->seq.b;
    }
    return true;
  }
}

// Trim up to k-1 bases from the end of bubble paths
// and copy to 3p flank
static void bubble_trim_alleles(CallFileEntry *centry, StrBuf *flank3pbuf)
{
  size_t i, trimlen, min_allele_len = SIZE_MAX;
  size_t nlines = call_file_num_lines(centry);

  for(i = 5; i < nlines; i += 2)
    min_allele_len = MIN2(call_file_line_len(centry,i), min_allele_len);

  trimlen = MIN2(min_allele_len, kmer_size-1);

  // Copy flank 3p
  const char *trimstr = call_file_get_line(centry, 5) +
                        call_file_line_len(centry, 5) - trimlen;
  strbuf_reset(flank3pbuf);
  strbuf_append_strn(flank3pbuf, trimstr, trimlen);
  strbuf_append_strn(flank3pbuf, call_file_get_line(centry, 3),
                                 call_file_line_len(centry, 3));

  // Trim alleles
  for(i = 5; i < nlines; i += 2) {
    call_file_line_len(centry,i) -= trimlen;
    call_file_get_line(centry,i)[call_file_line_len(centry,i)] = '\0';
  }
}

/**
 * Fetch the largest match from a breakpoint call
 * @param is line to be parsed '>seqname ... chr=...'
 * @param buf is temporary buffer
 * @param flank is used to return result of largest match
 * @return 1 on success, 0 if not mapped. Calls die() on error
 */
static int brkpnt_fetch_first_match(const char *line, ChromPosBuffer *buf,
                                    ChromPosOffset *flank)
{
  char *list = strstr(line, " chr=");
  if(list == NULL) die("Cannot find flank position: %s", line);
  // Parse chr=seq0b:1-20:+:1,seq0a:2-20:+:2
  if(chrom_pos_list_parse(list+5, buf) < 0) die("Invalid positions: %s", line);
  return chrom_pos_list_get_largest(buf, flank);
}

static bool brkpnt_fetch_coords(const CallFileEntry *centry,
                                ChromPosBuffer *chrposbuf,
                                const read_t **chrom,
                                size_t *start, size_t *end,
                                bool *fw_strand)
{
  ChromPosOffset flank5p, flank3p;
  size_t n;

  if((n = call_file_num_lines(centry)) < 6) die("Fewer than 6 lines: %zu", n);

  char *line0 = call_file_get_line(centry,0);
  char *line2 = call_file_get_line(centry,2);

  bool success = (brkpnt_fetch_first_match(line0, chrposbuf, &flank5p) &&
                  brkpnt_fetch_first_match(line2, chrposbuf, &flank3p));

  // Check flank5p, flank3p go right up to the breakpoints
  if(success) {
    ctx_assert(flank5p.offset+chrom_pos_len(&flank5p) == call_file_line_len(centry,1));
    ctx_assert(flank3p.offset == 1);
  }

  // Didn't map uniquely, with mismatching chromosomes or strands
  if(!success) { num_flanks_not_uniquely_mapped++; return false; }
  if(strcmp(flank5p.chrom,flank3p.chrom) != 0) { num_flanks_diff_chroms++; return false; }
  if(flank5p.fw_strand != flank3p.fw_strand) { num_flanks_diff_strands++; return false; }

  // Copy results. ChromPosOffset coords are 1-based.
  *chrom = fetch_chrom(flank5p.chrom);
  *fw_strand = flank5p.fw_strand;
  if(flank5p.fw_strand) { *start = flank5p.end+1; *end = flank3p.start; }
  else                  { *start = flank3p.end+1; *end = flank5p.start; }

  // Convert to 0-based coords
  (*start)--;
  (*end)--;

  return true;
}

static void strbuf_append_dna(StrBuf *buf, const char *src, size_t len,
                              bool fw_strand)
{
  strbuf_ensure_capacity(buf, buf->end + len);
  if(fw_strand) {
    strbuf_append_strn(buf, src, len);
  } else {
    dna_revcomp_str(buf->b+buf->end, src, len);
    buf->end += len;
    buf->b[buf->end] = '\0';
  }
}

static size_t align_get_start(const char *ref, const char *alt, size_t len,
                              size_t offset)
{
  size_t i;
  for(i = offset; i < len; i++) {
    if(ref[i] != alt[i]) return offset;
  }
  return len;
}

static size_t align_get_end(const char *ref, const char *alt, size_t len,
                            size_t offset)
{
  size_t i;
  for(i = offset; i < len; i++) {
    if(ref[i] == alt[i]) return offset;
  }
  return len;
}

static size_t align_get_len(const char *allele, size_t len)
{
  size_t i, nbases = 0;
  for(i = 0; i < len; i++)
    if(allele[i] != '-')
      nbases++;
  return nbases;
}

// Print allele with previous base and no deletions
// 'A--CG-T' with prev_base 'C' => print 'CACGT'
static void print_vcf_allele(int prev_base, const char *allele, size_t len,
                             FILE *fout)
{
  size_t i;
  if(prev_base > 0) fputc((char)prev_base, fout);
  for(i = 0; i < len; i++)
    if(allele[i] != '-')
      fputc(allele[i], fout);
}

// @param vcf_pos is 1-based
// @param prev_base is -1 if SNP otherwise previous base
static void print_vcf_entry(const char *chrom_name, size_t vcf_pos, int prev_base,
                            const char *ref, const char *alt,
                            size_t aligned_len,
                            const char *info,
                            const char **genotypes,
                            FILE *fout)
{
  // CHROM POS ID REF ALT QUAL FILTER INFO
  fprintf(fout, "%s\t%zu\tvar%zu\t", chrom_name, vcf_pos, num_vars_printed);
  print_vcf_allele(prev_base, ref, aligned_len, fout);
  fputc('\t', fout);
  print_vcf_allele(prev_base, alt, aligned_len, fout);
  fputs("\t.\tPASS\t", fout);
  if(info) fputs(info, fout);
  else fputc('.', fout);
  fputs("\tGT", fout);

  // Print genotypes
  size_t i;
  if(genotypes) {
    for(i = 0; i < num_samples; i++) {
      fputc('\t', fout);
      fputs(genotypes[i], fout);
    }
  } else {
    for(i = 0; i < num_samples; i++) {
      fputs("\t.", fout);
    }
  }

  fputc('\n', fout);
  num_vars_printed++;
}

/**
 * @param ref_pos is 0-based here
 * @param info is extra text to print in the info field of each variant (may be NULL)
 * @param genotypes is strings to print in genotypes columns, of length num_samples.
*                   It may be NULL.
 */
static void align_biallelic(const char *ref, const char *alt, size_t aligned_len,
                            const read_t *chr, size_t ref_pos,
                            const char *info, const char **genotypes,
                            FILE *fout)
{
  size_t i, start, end = 0;
  size_t ref_allele_len, alt_allele_len;
  int prev_base, vcf_pos;
  bool is_snp;

  while((start = align_get_start(ref, alt, aligned_len, end)) < aligned_len)
  {
    // Update ref offset
    for(i = end; i < start; i++)
      if(ref[i] != '-') ref_pos++;

    end = align_get_end(ref, alt, aligned_len, start);

    ref_allele_len = align_get_len(ref+start, end-start);
    alt_allele_len = align_get_len(alt+start, end-start);
    is_snp = ref_allele_len == 1 && alt_allele_len == 1;

    vcf_pos = ref_pos+1; // Convert to 1-based

    if(!is_snp) {
      prev_base = ref_pos > 0 ? chr->seq.b[ref_pos-1] : 'N';
      vcf_pos--;
    } else {
      prev_base = -1;
    }

    print_vcf_entry(chr->name.b, vcf_pos, prev_base,
                    ref+start, alt+start, end-start,
                    info, genotypes, fout);

    ref_pos += ref_allele_len;
  }
}

/*
// Return -1 on error
static long get_entry_id(const char *hdrline, bool bubble_format)
{
  const char *start;
  char *end = NULL;
  const char *expstr = bubble_format ? ">bubble." : ">brkpnt.";
  if(strncmp(hdrline,expstr,strlen(expstr)) != 0) return -1;
  start = hdrline + strlen(expstr);
  unsigned long id = strtoul(start, &end, 10);
  if(end == NULL || *end != '.') return -1;
  return id;
}
*/

/**
 * Parse header line from FASTA to fetch call id
 * Expect ">bubble.<id>." or ">brkpnt.<id>."
 * @param idstr copy call id string to memory pointed to by idstr
 * @param size is size of memory pointed to by idstr
 * @return length of string or -1 on error format error, -2 if idstr not big enough
 */
static int get_callid_str(const char *hdrline, bool bubble_format,
                          char *idstr, size_t size)
{
  const char *start, *end, *expstr = bubble_format ? ">bubble." : ">brkpnt.";
  if(strncmp(hdrline,expstr,strlen(expstr)) != 0) return -1;
  start = hdrline + strlen(expstr);
  if((end = strchr(start, '.')) == NULL) return -1;
  size_t len = end - start;
  if(len+1 > size) return -2;
  memcpy(idstr, start, len);
  idstr[len] = '\0';
  return len;
}

static void align_entry_allele(const char *line, size_t linelen,
                               const char *flank5p, size_t flank5p_len,
                               const char *flank3p, size_t flank3p_len,
                               const read_t *chr,
                               size_t ref_start, size_t ref_end,
                               size_t ncpy, bool cpy_flnk_5p,
                               bool fw_strand,
                               const char *info, const char **genotypes,
                               StrBuf *tmpbuf, FILE *fout)
{
  (void)flank3p_len;

  const char *seq;
  size_t seqlen;

  if(ncpy == 0 && fw_strand) {
    seq = line;
    seqlen = linelen;
  } else {
    strbuf_reset(tmpbuf);

    // printf(" ref_start: %zu ref_end: %zu\n", ref_start, ref_end);
    // printf(" ncpy: %zu fw_strand: %i cpy_flnk_5p: %i\n", ncpy, fw_strand, cpy_flnk_5p);

    if(ncpy > 0) {
      if(fw_strand && cpy_flnk_5p)
        strbuf_append_dna(tmpbuf, flank5p+flank5p_len-ncpy, ncpy, fw_strand);
      else if(!fw_strand && !cpy_flnk_5p)
        strbuf_append_dna(tmpbuf, flank3p, ncpy, fw_strand);
    }

    // Copy allele
    strbuf_append_dna(tmpbuf, line, linelen, fw_strand);

    if(ncpy > 0) {
      if(!fw_strand && cpy_flnk_5p)
        strbuf_append_dna(tmpbuf, flank5p+flank5p_len-ncpy, ncpy, fw_strand);
      else if(fw_strand && !cpy_flnk_5p)
        strbuf_append_dna(tmpbuf, flank3p, ncpy, fw_strand);
    }

    seq = tmpbuf->b;
    seqlen = tmpbuf->end;
  }

  // printf("%.*s vs %.*s\n", (int)(ref_end-ref_start), chr->seq.b + ref_start,
  //                          (int)seqlen, seq);

  // Align chrom and seq
  needleman_wunsch_align2(chr->seq.b + ref_start, seq,
                          ref_end-ref_start, seqlen,
                          &nw_scoring_allele, nw_aligner, aln);
  num_nw_allele++;

  // Break into variants and print VCF
  align_biallelic(aln->result_a, aln->result_b, aln->length,
                  chr, ref_start,
                  info, genotypes, fout);
}

#define GENO_0     0
#define GENO_1     1
#define GENO_UNDEF 2
static const char genotype_strs[3][2] = {"0", "1", "."};

/**
 * Parse header line from breakpoint call file to generate genotype strings.
 * Calls die() if there are formatting errors in the hdrline
 * @param hdrline contains " cols=1,4" string
 * @param genotypes used to set genotypes to either "." or "1" if specified in cols=
 * @param nsamples the number of elements in `genotypes`
 */
static void brkpnt_parse_genotype_colours(const char *hdrline,
                                          const char **genotypes, size_t nsamples)
{
  size_t i;
  char *end;
  for(i = 0; i < nsamples; i++) genotypes[i] = genotype_strs[GENO_UNDEF];
  const char *str = strstr(hdrline," cols=");
  if(str == NULL) die("Cannot find colours: '%s'", hdrline);
  str += strlen(" cols=");
  while(1) {
    unsigned long tmp = strtoul(str, &end, 10);
    if(end == NULL || end == str || (*end != ' ' && *end != ',' && *end != '\0') ||
       tmp >= nsamples) {
      die("Bad line [nsamples: %zu]: %s", nsamples, hdrline);
    }
    genotypes[tmp] = genotype_strs[GENO_1];
    if(*end != ',') break;
    str = end+1;
  }
}

static void align_entry(CallFileEntry *centry, const char *callid,
                        const char *flank5p, size_t flank5p_len,
                        const char *flank3p, size_t flank3p_len,
                        const read_t *chr,
                        size_t ref_start, size_t ref_end,
                        bool fw_strand,
                        StrBuf *tmpbuf, const char **genotypes,
                        FILE *fout)
{
  size_t i, ncpy = 0;
  bool cpy_flnk_5p = false;

  // If variant starts after it ends, we need to copy some sequence to fix this
  if(ref_start > ref_end) {
    ncpy = ref_start - ref_end;
    if(ncpy > flank5p_len && ncpy > flank3p_len) {
      num_flanks_overlap_too_large++;
      // printf("Copy too much %zu > %zu %zu\n", ncpy, flank5p_len, flank3p_len);
      return; // can't align
    }
    cpy_flnk_5p = (ncpy > flank5p_len);
    if(fw_strand == cpy_flnk_5p) ref_start -= ncpy;
    else                         ref_end   += ncpy;
  }

  ctx_assert(ref_start <= ref_end);

  if(ref_end-ref_start > max_allele_len) {
    num_flanks_too_far_apart++;
    return;
  }

  if(ref_end > chr->seq.end) die("Out of range: %zu > %zu", ref_end, chr->seq.end);

  num_entries_well_mapped++;

  // If not fw strand, we need to flip each allele

  // Deal with alleles one at a time vs ref
  // First allele stored in line 5:
  //   >flank5p\n<seq>\n>flank3p\n<seq>\n>allele1\n<seq>
  char *line;
  size_t linelen;

  char info[200];
  strcpy(info, input_bubble_format ? "BUBBLE=" : "BRKPNT=");
  strcpy(info+7, callid);

  size_t nlines = call_file_num_lines(centry);

  for(i = 5; i < nlines; i+=2)
  {
    line = call_file_get_line(centry,i);
    linelen = call_file_line_len(centry,i);

    if(genotypes) {
      ctx_assert(!input_bubble_format);
      char *hdrline = call_file_get_line(centry,i-1);
      brkpnt_parse_genotype_colours(hdrline, genotypes, num_samples);
    }

    align_entry_allele(line, linelen,
                       flank5p, flank5p_len, flank3p, flank3p_len,
                       chr, ref_start, ref_end, ncpy, cpy_flnk_5p,
                       fw_strand,
                       info, genotypes,
                       tmpbuf, fout);
  }
}

static void parse_entries(gzFile gzin, FILE *fout)
{
  CallFileEntry centry;
  call_file_entry_alloc(&centry);

  ChromPosBuffer chrposbuf;
  chrompos_buf_alloc(&chrposbuf, 32);

  StrBuf tmpbuf, flank3pbuf;
  strbuf_alloc(&tmpbuf, 1024);
  strbuf_alloc(&flank3pbuf, 1024);

  const char *flank5p, *flank3p;
  size_t flank5p_len, flank3p_len;

  const read_t *chrom = NULL;
  size_t ref_start = 0, ref_end = 0;
  bool mapped = false, fw_strand = false;

  const char **genotypes = NULL;

  if(!input_bubble_format)
    genotypes = ctx_calloc(num_samples, sizeof(char*));

  for(; call_file_read(gzin, input_path, &centry); num_entries_read++)
  {
    size_t nlines = call_file_num_lines(&centry);
    ctx_assert2(!(nlines&1) && nlines >= 6, "Too few lines: %zu", nlines);

    flank5p = call_file_get_line(&centry,1);
    flank5p_len = call_file_line_len(&centry,1);

    // Read a corresponding SAM entry
    if(input_bubble_format)
    {
      // Trim down alleles, add to 3p flank
      bubble_trim_alleles(&centry, &flank3pbuf);
      flank3p = flank3pbuf.b;
      flank3p_len = flank3pbuf.end;

      mapped = sam_fetch_coords(&centry, flank5p, flank5p_len, flank3p, flank3p_len,
                                &chrom, &ref_start, &ref_end, &fw_strand);
    }
    else {
      flank3p = call_file_get_line(&centry, 3);
      flank3p_len = call_file_line_len(&centry, 3);

      mapped = brkpnt_fetch_coords(&centry, &chrposbuf,
                                   &chrom, &ref_start, &ref_end, &fw_strand);
    }

    if(mapped)
    {
      // Get call id
      const char *hdrline = call_file_get_line(&centry, 0);
      char callid[100];
      int r = get_callid_str(hdrline, input_bubble_format, callid, sizeof(callid));
      if(r == -1) die("Poorly formatted: %s", hdrline);
      if(r == -2) die("Call id string is too long: %s", hdrline);

      align_entry(&centry, callid, flank5p, flank5p_len, flank3p, flank3p_len,
                  chrom, ref_start, ref_end, fw_strand,
                  &tmpbuf, genotypes,
                  fout);
    }
  }

  ctx_free(genotypes);
  call_file_entry_dealloc(&centry);
  chrompos_buf_dealloc(&chrposbuf);
  strbuf_dealloc(&tmpbuf);
  strbuf_dealloc(&flank3pbuf);
}

static void flanks_sam_open()
{
  if(!futil_path_has_extension(sam_path, ".bam") &&
     !futil_path_has_extension(sam_path, ".sam"))
  {
    cmd_print_usage("Mapped flanks is not .sam or .bam file: %s", sam_path);
  }

  bool isbam = futil_path_has_extension(sam_path, ".bam");

  samfh = sam_open(sam_path, isbam ? "rb" : "rs");
  if(samfh == NULL) die("Cannot open SAM/BAM %s", sam_path);

  // Load BAM header
  bam_header = sam_hdr_read(samfh);
  bam = bam_init1();
}

static void flanks_sam_close()
{
  sam_close(samfh);
  free(bam_header);
  free(bam);
}

static cJSON* read_input_header(gzFile gzin)
{
  cJSON *json;
  StrBuf hdrstr;
  strbuf_alloc(&hdrstr, 1024);
  json_hdr_read(NULL, gzin, input_path, &hdrstr);
  json = cJSON_Parse(hdrstr.b);
  if(json == NULL) die("Invalid JSON header: %s", input_path);

  // Check we can handle the kmer size
  kmer_size = json_hdr_get_kmer_size(json, input_path);
  db_graph_check_kmer_size(kmer_size, input_path);

  strbuf_dealloc(&hdrstr);

  return json;
}

/**
 * @return number of samples found in json header
 */
static size_t print_vcf_header(cJSON *json, FILE *fout)
{
  ctx_assert(json != NULL);

  char datestr[9];
  time_t date = time(NULL);
  strftime(datestr, 9, "%Y%m%d", localtime(&date));

  fprintf(fout, "##fileFormat=VCFv4.1\n##fileDate=%s\n", datestr);

  // Print commands used to generate header
  cJSON *commands = json_hdr_get(json, "commands", cJSON_Array, input_path);
  cJSON *command = commands->child;

  // Print this command
  char keystr[8];
  char *prevstr = NULL;
  size_t i;

  if(command) {
    cJSON *key = json_hdr_get(command, "key", cJSON_String, input_path);
    prevstr = key->valuestring;
  }

  // Print command entry for this command
  fprintf(fout, "##CMD=<key=\"%s\",prev=\"%s\",cmd=\"%s\",cwd=\"%s\">\n",
          hex_rand_str(keystr, sizeof(keystr)),
          prevstr ? prevstr : "NULL",
          cmd_get_cmdline(), cmd_get_cwd());

  // Print previous commands
  for(; command != NULL; command = command->next)
  {
    cJSON *key  = json_hdr_get(command, "key",  cJSON_String, input_path);
    cJSON *cmd  = json_hdr_get(command, "cmd",  cJSON_Array,  input_path);
    cJSON *cwd  = json_hdr_get(command, "cwd",  cJSON_String, input_path);
    cJSON *prev = json_hdr_get(command, "prev", cJSON_Array,  input_path);
    prev = prev->child; // result could be NULL
    if(prev && prev->type != cJSON_String) die("Invalid 'prev' field");
    fprintf(fout, "##CMD=<key=\"%s\",prev=\"%s", key->valuestring,
                  prev ? prev->valuestring : "NULL");
    if(prev) {
      while((prev = prev->next) != NULL) fprintf(fout, ";%s", prev->valuestring);
    }
    fprintf(fout, "\",cmd=\"");
    for(i = 0, cmd = cmd->child; cmd; cmd = cmd->next, i++) {
      if(i > 0) fputc(' ', fout);
      fputs(cmd->valuestring, fout);
    }
    fprintf(fout, "\",cwd=\"%s\">\n", cwd->valuestring);
  }

  // Print contigs lengths
  for(i = 0; i < chroms.len; i++) {
    fprintf(fout, "##contig=<id=%s,length=%zu>\n",
            chroms.data[i].name.b, chroms.data[i].seq.end);
  }

  // Print VCF column header
  fputs("#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT", fout);

  // Print a column for each sample
  cJSON *graph_json   = json_hdr_get(json,       "graph",   cJSON_Object, input_path);
  cJSON *colours_json = json_hdr_get(graph_json, "colours", cJSON_Array,  input_path);
  cJSON *colour_json  = colours_json->child;
  if(colour_json == NULL) die("Missing colours");

  // Also count the number of samples
  size_t nsamples;

  for(nsamples = 0; colour_json; colour_json = colour_json->next, nsamples++) {
    cJSON *sample_json = json_hdr_get(colour_json, "sample", cJSON_String, input_path);
    fputc('\t', fout);
    fputs(sample_json->valuestring, fout);
  }

  fputc('\n', fout);

  return nsamples;
}

// Check contig entries match reference
// We check that these match the reference just loaded
static void brkpnt_check_refs_match(cJSON *json)
{
  cJSON *brkpnts = json_hdr_get(json,    "breakpoints", cJSON_Object, input_path);
  cJSON *contigs = json_hdr_get(brkpnts, "contigs",     cJSON_Array,  input_path);
  cJSON *contig;
  size_t num_chroms = 0;

  for(contig = contigs->child; contig; contig = contig->next, num_chroms++)
  {
    cJSON *id  = json_hdr_get(contig, "id",     cJSON_String, input_path);
    cJSON *len = json_hdr_get(contig, "length", cJSON_Number, input_path);

    const char *chrom_name = id->valuestring;
    long chrom_len = len->valueint;
    size_t reflen;

    khiter_t k = kh_get(ChromHash, genome, chrom_name);
    if(k == kh_end(genome))
      die("Cannot find ref chrom: %s", chrom_name);
    else {
      reflen = kh_value(genome, k)->seq.end;
      if(reflen != (size_t)chrom_len) {
        die("Chrom lengths do not match %s input:%li ref:%zu",
            chrom_name, chrom_len, reflen);
      }
    }
  }

  if(num_chroms != chroms.len) {
    die("Number of chromosomes differ: %zu in header vs %zu in ref",
        num_chroms, chroms.len);
  }
}

int ctx_calls2vcf(int argc, char **argv)
{
  parse_cmdline_args(argc, argv);

  // These functions call die() on error
  gzFile gzin = futil_gzopen(input_path, "r");

  nw_aligner_setup();

  // Read file header
  cJSON *json = read_input_header(gzin);

  // Get format (bubble or breakpoint file)
  cJSON *json_fmt = json_hdr_get(json, "file_format", cJSON_String, input_path);
  if(strcmp(json_fmt->valuestring,"CtxBreakpoints") == 0) input_bubble_format = false;
  else if(strcmp(json_fmt->valuestring,"CtxBubbles") == 0) input_bubble_format = true;
  else die("Unknown format: '%s'", json_fmt->valuestring);

  status("Reading %s in %s format", futil_inpath_str(input_path),
         input_bubble_format ? "bubble" : "breakpoint");

  if(input_bubble_format && sam_path == NULL)
    cmd_print_usage("Require -F <flanks.sam> with bubble file");

  // Open flank file if it exists
  if(sam_path) flanks_sam_open();

  // Open output file
  FILE *fout = futil_open_create(out_path, "w");

  // Load reference genome
  read_buf_alloc(&chroms, 1024);
  genome = kh_init(ChromHash);
  seq_reader_load_ref_genome(ref_paths, num_ref_paths, &chroms, genome);

  if(!input_bubble_format) brkpnt_check_refs_match(json);

  // Run
  num_samples = print_vcf_header(json, fout);
  parse_entries(gzin, fout);

  // Print stats
  char num_entries_read_str[50];
  char num_vars_printed_str[50];
  ulong_to_str(num_entries_read, num_entries_read_str);
  ulong_to_str(num_vars_printed, num_vars_printed_str);

  status("Read %s entries, printed %s vcf entries to: %s",
         num_entries_read_str, num_vars_printed_str, futil_outpath_str(out_path));

  if(input_bubble_format) {
    // Bubble caller specific
    print_stat(num_flank5p_unmapped,    num_entries_read, "flank 5p unmapped");
    print_stat(num_flank5p_lowqual,     num_entries_read, "flank 5p low mapq");
    print_stat(num_flank3p_not_found,   num_entries_read, "flank 3p not found");
    print_stat(num_flank3p_multihits,   num_entries_read, "flank 3p multiple hits");
    print_stat(num_flank3p_approx_match,num_entries_read, "flank 3p approx match used");
  } else {
    // Breakpoint caller specific
    print_stat(num_flanks_not_uniquely_mapped, num_entries_read, "flank pairs contain one flank not mapped uniquely");
    print_stat(num_flanks_diff_chroms,         num_entries_read, "flank pairs map to diff chroms");
    print_stat(num_flanks_diff_strands,        num_entries_read, "flank pairs map to diff strands");
  }
  print_stat(num_flanks_too_far_apart,       num_entries_read, "flank pairs too far apart");
  print_stat(num_flanks_overlap_too_large,   num_entries_read, "flank pairs overlap too much");
  print_stat(num_entries_well_mapped,        num_entries_read, "flank pairs map well");

  status("Aligned %zu allele pairs and %zu flanks", num_nw_allele, num_nw_flank);

  // Finished - clean up
  cJSON_Delete(json);
  gzclose(gzin);
  fclose(fout);
  read_buf_dealloc(&chroms);
  kh_destroy_ChromHash(genome);
  nw_aligner_destroy();

  if(sam_path) flanks_sam_close();

  // hide unused method warnings
  (void)kh_del_ChromHash;
  (void)kh_put_ChromHash;
  (void)kh_get_ChromHash;
  (void)kh_clear_ChromHash;
  (void)kh_destroy_ChromHash;
  (void)kh_init_ChromHash;

  return EXIT_SUCCESS;
}
