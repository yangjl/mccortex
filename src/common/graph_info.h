#ifndef DB_INFO_H_
#define DB_INFO_H_

#include <inttypes.h>
#include "graph_typedef.h"

void graph_info_init(GraphInfo *ginfo);
void graph_info_alloc(GraphInfo *ginfo);
void graph_info_dealloc(GraphInfo *ginfo);

// Update mean read length in a colour, eg when you merge a new binary
// what it used to be, ie the previous_seq)
void graph_info_update_contigs(GraphInfo *ginfo,
                               uint64_t added_seq, uint64_t num_contigs);

void graph_info_cpy(GraphInfo *dst, const GraphInfo *src);
void graph_info_merge(GraphInfo *dst, const GraphInfo *src);

#endif /* GRAPH_INFO_H_ */
