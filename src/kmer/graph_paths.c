#include "global.h"
#include "db_graph.h"
#include "db_node.h"
#include "graph_paths.h"
#include "packed_path.h"
#include "bit_macros.h"


// Similar to path_file_filter.c:path_file_load_check()
// Check kmer size matches and sample names match
void graphs_paths_compatible(const GraphFileReader *graphs, size_t num_graphs,
                             const PathFileReader *paths, size_t num_paths)
{
  size_t g, p, kmer_size;
  size_t ctx_max_cols = 0, ctp_max_cols = 0;
  size_t ctx_max_kmers = 0, ctp_max_kmers = 0;

  if(num_graphs) kmer_size = graphs[0].hdr.kmer_size;
  else if(num_paths) kmer_size = paths[0].hdr.kmer_size;
  else return;

  for(g = 0; g < num_graphs; g++) {
    if(graphs[g].hdr.kmer_size != kmer_size) {
      die("Kmer-size doesn't match between files [%zu vs %u]: %s",
          kmer_size, graphs[g].hdr.kmer_size, graphs[g].fltr.orig_path.buff);
    }
    ctx_max_cols = MAX2(ctx_max_cols, graph_file_usedcols(&graphs[g]));
    ctx_max_kmers = MAX2(ctx_max_kmers, graphs[g].hdr.num_of_kmers);
  }

  for(p = 0; p < num_paths; p++) {
    if(paths[p].hdr.kmer_size != kmer_size) {
      die("Kmer-size doesn't match between files [%zu vs %u]: %s",
          kmer_size, paths[p].hdr.kmer_size, paths[p].fltr.orig_path.buff);
    }
    ctp_max_cols = MAX2(ctp_max_cols, path_file_usedcols(&paths[p]));
    ctp_max_kmers = MAX2(ctp_max_kmers, paths[p].hdr.num_kmers_with_paths);
  }

  const FileFilter *fltr = (num_graphs ? &graphs[0].fltr : &paths[0].fltr);
  db_graph_check_kmer_size(kmer_size, fltr->orig_path.buff);

  if(ctp_max_kmers > ctx_max_kmers)
    die("More kmers in path files than in graph files!");

  if(ctp_max_cols > ctx_max_cols)
    die("More colours in path files than in graph files!");

  // Check sample names
  size_t pinto, pfrom, ginto, gfrom, i, j;
  StrBuf *pname, *gname;

  // Ugly loops I'm afraid
  // number of paths / graphs and ultimately number of colours should be low
  for(p = 0; p < num_paths; p++) {
    for(i = 0; i < paths[p].fltr.ncols; i++) {
      pinto = path_file_intocol(&paths[p], i);
      pfrom = path_file_fromcol(&paths[p], i);
      pname = &paths[p].hdr.sample_names[pfrom];
      for(g = 0; g < num_graphs; g++) {
        for(j = 0; j < graphs[g].fltr.ncols; j++) {
          ginto = graph_file_intocol(&graphs[g], j);
          gfrom = graph_file_fromcol(&graphs[g], j);
          gname = &graphs[g].hdr.ginfo[gfrom].sample_name;
          if(pinto == ginto && strcmp(pname->buff, gname->buff) != 0) {
            die("Sample names don't match\n%s:%zu%s\n%s:%zu%s\n",
                graphs[g].fltr.orig_path.buff, graphs[g].fltr.cols[j], gname->buff,
                paths[p].fltr.orig_path.buff, paths[p].fltr.cols[i], pname->buff);
          }
        }
      }
    }
  }
}


//
// Thread safe wrapper for path_store [mt for multithreaded]
//

// Returns true if new to colour, false otherwise
// packed points to <PathLen><PackedSeq>
// Returns address of path in PathStore by setting newidx
boolean graph_paths_find_or_add_mt(hkey_t hkey, dBGraph *db_graph, Colour ctpcol,
                                   const uint8_t *packed, size_t plen,
                                   PathIndex *newidx)
{
  PathStore *pstore = &db_graph->pdata;
  volatile uint8_t *kmerlocks = (volatile uint8_t *)db_graph->path_kmer_locks;

// path_nbytes is the number of bytes in <PackedSeq>
  size_t path_nbytes = (plen+3)/4;

  // debug
  // print_path(hkey, packed, pstore);

  // 1) Get lock for kmer
  // calling bitlock_yield_acquire instead of bitlock_acquire causes
  bitlock_yield_acquire(kmerlocks, hkey);

  const PathIndex next = *db_node_paths_volptr(db_graph, hkey);

  // 2) Search for path
  PathIndex match = path_store_find(pstore, next, packed, path_nbytes);
  uint8_t *colset;

  if(match != PATH_NULL)
  {
    // => if already exist -> add colour -> release lock
    colset = packedpath_get_colset(pstore->store+match);
    boolean added = !bitset_get(colset, ctpcol);
    bitset_set(colset, ctpcol);
    bitlock_release(kmerlocks, hkey);
    *newidx = match;
    return added;
  }

  // 3) shift store->next to make space
  size_t mem = packedpath_mem2(pstore->colset_bytes, path_nbytes);
  uint8_t *new_path;

  // atomic { new_path = pstore->next; pstore->next += mem; }
  // __sync_fetch_and_add(x,y) x and y need to be of same type
  new_path = __sync_fetch_and_add((uint8_t * volatile *)(void*)&pstore->next,
                                  (uint8_t *)mem);

  if(new_path + mem > pstore->end) die("Out of path memory!");

  // 4) Copy new entry

  // Prev
  packedpath_set_prev(new_path, next);

  // bitset
  colset = packedpath_get_colset(new_path);
  memset(colset, 0, pstore->colset_bytes);
  bitset_set(colset, ctpcol);

  // Length + Path
  memcpy(colset+pstore->colset_bytes, packed, sizeof(PathLen) + path_nbytes);

  // path must be written before we move the kmer path pointer forward
  // although there is a write-lock (kmerlocks), threads currently traversing
  // the graph would fall over
  __sync_synchronize();

  // 5) update kmer pointer
  PathIndex pindex = (uint64_t)(new_path - pstore->store);
  *db_node_paths_volptr(db_graph, hkey) = pindex;

  // Update number of kmers with paths if this the first path for this kmer
  if(next == PATH_NULL) {
    __sync_add_and_fetch((volatile size_t*)&pstore->num_kmers_with_paths, 1);
  }

  // Update number of paths
  __sync_add_and_fetch((volatile size_t*)&pstore->num_of_paths, 1);

  // status("npaths: %zu nkmers: %zu", pstore->num_of_paths, pstore->num_kmers_with_paths);

  __sync_synchronize();

  // 6) release kmer lock
  bitlock_release(kmerlocks, hkey);

  *newidx = pindex;
  return true;
}

//
// Checking
//

// 1) check node after node has indegree >1 in sample ctxcol
// 2) follow path, check each junction matches up with a node with outdegree >1
// col is graph colour
// packed is just <PackedBases>
void graph_path_check_valid(dBNode node, size_t ctxcol, const uint8_t *packed,
                            size_t nbases, const dBGraph *db_graph)
{
  assert(db_graph->num_edge_cols == db_graph->num_of_cols ||
         db_graph->node_in_cols != NULL);

  BinaryKmer bkmer;
  Edges edges;
  dBNode nodes[4];
  Nucleotide nucs[4];
  size_t i, j, n, edgecol = db_graph->num_edge_cols > 1 ? ctxcol : 0;
  // length is kmers and juctions
  size_t klen, plen;

  // char nstr[nbases+1];
  // for(i = 0; i < nbases; i++) nstr[i] = dna_nuc_to_char(bases[i]);
  // nstr[nbases] = '\0';
  // status("Check: %zu:%i len %zu %s", (size_t)node.key, node.orient, nbases, nstr);

  for(klen = 0, plen = 0; plen < nbases; klen++)
  {
    bkmer = db_node_get_bkmer(db_graph, node.key);
    edges = db_node_get_edges(db_graph, edgecol, node.key);

    // Check this node is in this colour
    if(db_graph->node_in_cols != NULL) {
      assert(db_node_has_col(db_graph, node.key, ctxcol));
    } else if(db_graph->col_covgs != NULL) {
      assert(db_node_covg(db_graph, node.key, ctxcol) > 0);
    }

    #ifdef CTXVERBOSE
      char bkmerstr[MAX_KMER_SIZE+1];
      binary_kmer_to_str(bkmer, db_graph->kmer_size, bkmerstr);
      status("klen: %zu plen: %zu %zu:%i %s",
             klen, plen, (size_t)node.key, node.orient, bkmerstr);
    #endif

    if(klen == 1) {
      dBNode rnode = db_node_reverse(node);
      Edges backedges = db_node_edges_in_col(rnode, ctxcol, db_graph);
      int outdegree = edges_get_outdegree(backedges, rnode.orient);
      if(outdegree <= 1) {
        status("outdegree: %i col: %zu", (int)outdegree, ctxcol);
      }
      assert(outdegree > 1);
    }

    n = db_graph_next_nodes(db_graph, bkmer, node.orient,
                            edges, nodes, nucs);

    assert(n > 0);

    // Reduce to nodes in our colour if edges limited
    if(db_graph->num_edge_cols == 1 && db_graph->node_in_cols != NULL) {
      for(i = 0, j = 0; i < n; i++) {
        if(db_node_has_col(db_graph, nodes[i].key, ctxcol)) {
          nodes[j] = nodes[i];
          nucs[j] = nucs[i];
          j++;
        }
      }
      n = j; // update number of next nodes
      assert(n > 0);
    }

    // If fork check nucleotide
    if(n > 1) {
      Nucleotide expbase = packed_fetch(packed, plen);

      for(i = 0; i < n && nucs[i] != expbase; i++);
      if(i == n) {
        printf("plen: %zu expected: %c\n", plen, dna_nuc_to_char(expbase));
        printf("Got: ");
        for(i = 0; i < n; i++) printf(" %c", dna_nuc_to_char(nucs[i]));
        printf("\n");
      }
      assert(i < n && nucs[i] == expbase);
      node = nodes[i];
      plen++;
    }
    else {
      node = nodes[0];
    }
  }
}

static void packed_path_check(hkey_t hkey, const uint8_t *packed,
                              const GraphPathPairing *gp,
                              const dBGraph *db_graph)
{
  const PathStore *pstore = &db_graph->pdata;
  PathLen len_bases;
  Orientation orient;
  const uint8_t *colset, *seq;
  size_t i;

  colset = packedpath_get_colset(packed);

  seq = packedpath_seq(packed, pstore->colset_bytes);
  packedpath_get_len_orient(packed, pstore->colset_bytes, &len_bases, &orient);

  dBNode node = {.key = hkey, .orient = orient};

  // Check length
  size_t nbytes = sizeof(PathIndex) + pstore->colset_bytes +
                  sizeof(PathLen) + packedpath_len_nbytes(len_bases);

  assert(packed + nbytes <= pstore->end);

  // Check at least one colour is set
  uint8_t colset_or = 0;
  for(i = 0; i < pstore->colset_bytes; i++) colset_or |= colset[i];
  assert(colset_or != 0);

  // print path
  // print_path(node.key, packed+sizeof(PathIndex)+pstore->colset_bytes, pstore);

  for(i = 0; i < gp->n; i++)
    if(bitset_get(colset, gp->ctpcols[i]))
      graph_path_check_valid(node, gp->ctxcols[i], seq, len_bases, db_graph);
}

static void kmer_check_paths(hkey_t hkey, const GraphPathPairing *gp,
                             const dBGraph *db_graph,
                             size_t *npaths_ptr, size_t *nkmers_ptr)
{
  const PathStore *pdata = &db_graph->pdata;
  PathIndex pindex = db_node_paths(db_graph, hkey);
  uint8_t *packed;
  size_t num_paths = 0;

  while(pindex != PATH_NULL)
  {
    packed = pdata->store+pindex;
    packed_path_check(hkey, packed, gp, db_graph);
    pindex = packedpath_get_prev(packed);
    num_paths++;
  }

  *npaths_ptr += num_paths;
  *nkmers_ptr += (num_paths > 0);
}

void graph_paths_check_all_paths(const GraphPathPairing *gp,
                                 const dBGraph *db_graph)
{
  size_t num_paths = 0, num_kmers = 0;

  HASH_ITERATE(&db_graph->ht, kmer_check_paths, gp, db_graph,
               &num_paths, &num_kmers);

  assert(num_paths == db_graph->pdata.num_of_paths);
  assert(num_kmers == db_graph->pdata.num_kmers_with_paths);
}

void graph_path_check_path(hkey_t node, PathIndex pindex,
                           const GraphPathPairing *gp,
                           const dBGraph *db_graph)
{
  uint8_t *packed = db_graph->pdata.store+pindex;
  packed_path_check(node, packed, gp, db_graph);
}

// For debugging
static void _pstore_update_counts(hkey_t hkey, const dBGraph *db_graph,
                                  size_t *nkmers, size_t *npaths,
                                  size_t *nvisited)
{
  const PathStore *pstore = &db_graph->pdata;
  PathIndex pindex = db_node_paths(db_graph, hkey);
  size_t n;

  (*nvisited)++;

  if(pindex == PATH_NULL) return;
  (*nkmers)++;

  for(n = 1; (pindex = packedpath_get_prev(pstore->store+pindex)) != PATH_NULL; n++);
  (*npaths) += n;
}

void graph_paths_check_counts(const dBGraph *db_graph)
{
  const PathStore *pstore = &db_graph->pdata;
  size_t nkmers = 0, npaths = 0, nvisited = 0;

  HASH_ITERATE(&db_graph->ht, _pstore_update_counts,
               db_graph, &nkmers, &npaths, &nvisited);

  assert(nvisited == db_graph->ht.unique_kmers);
  assert(nkmers == pstore->num_kmers_with_paths);
  assert(npaths == pstore->num_of_paths);
}
