#include "global.h"
#include "chrom_pos_list.h"
#include "util.h"

// Sort by length, chrom, strand (fw,rv), start
int chrom_pos_cmp_len(const void *aa, const void *bb)
{
  const ChromPosOffset *a = (const ChromPosOffset*)aa;
  const ChromPosOffset *b = (const ChromPosOffset*)bb;
  int c;
  size_t len_a = chrom_pos_len(a), len_b = chrom_pos_len(b);

  if(len_a != len_b) return len_a > len_b ? -1 : 1;
  if((c = strcmp(a->chrom,b->chrom)) != 0) return c;
  if(a->fw_strand != b->fw_strand) return a->fw_strand ? -1 : 1;
  return a->start < b->start ? -1 : 1;
}

void chrom_pos_list_sort(ChromPosBuffer *buf)
{
  qsort(buf->data, buf->len, sizeof(buf->data[0]), chrom_pos_cmp_len);
}

// Get largest match
// return 1 if there is a unique longest and copies it to pos, otherwise 0
int chrom_pos_list_get_largest(const ChromPosBuffer *buf, ChromPosOffset *pos)
{
  if(buf->len == 0) return 0;
  size_t i, idx = 0, len, max;
  bool unique = true;
  len = max = chrom_pos_len(&buf->data[0]);

  for(i = 1; i < buf->len; i++) {
    len = chrom_pos_len(&buf->data[i]);
    if(len == max) unique = false;
    else if(len > max) {
      unique = true;
      idx = i;
      max = len;
    }
  }

  memcpy(pos, &buf->data[idx], sizeof(ChromPosOffset));
  return unique;
}

void chrom_pos_print(const ChromPosOffset *pos)
{
  printf("%s:%zu-%zu:%c:%zu\n", pos->chrom, pos->start, pos->end,
         pos->fw_strand ? '+' : '-', pos->offset);
}

void chrom_pos_validate(const ChromPosOffset *pos)
{
  // chrom_pos_print(pos);
  ctx_assert2(pos->start < pos->end, "end before start");
  ctx_assert(pos->chrom != NULL);
  ctx_assert(pos->fw_strand == 0 || pos->fw_strand == 1);
}

// chr:start-end:strand:offset
int _parse(char *str, ChromPosOffset *obj)
{
  char *chrom = NULL;
  size_t i, start = 0, end = 0, offset = 0;
  bool fw_strand = false;
  char *dash, *ptrs[4], *saveptr = NULL;

  for(i = 0; i < 4; i++, str = NULL) {
    if((ptrs[i] = strtok_r(str, ":", &saveptr)) == NULL) return -1;
  }

  // Check there are no more separators
  if(strtok_r(NULL, ":", &saveptr) != NULL) return -1;

  chrom = ptrs[0];

  // Split and parse start-end
  if((dash = strchr(ptrs[1],'-')) == NULL) return -1;
  *dash++ = '\0';
  if(!parse_entire_size(ptrs[1], &start) ||
     !parse_entire_size(dash,    &end)) return -1;  

  if(strcmp(ptrs[2],"+") != 0 && strcmp(ptrs[2],"-") != 0) return -1;
  fw_strand = (strcmp(ptrs[2],"+") == 0);

  if(!parse_entire_size(ptrs[3], &offset)) return -1;

  // if strand is +, start must be <= end
  // if strand is -, start must be >= end
  if((start < end && !fw_strand) || (start > end && fw_strand)) return -1;

  obj->chrom     = chrom;
  obj->start     = MIN2(start,end);
  obj->end       = MAX2(start,end);
  obj->fw_strand = fw_strand;
  obj->offset    = offset;

  chrom_pos_validate(obj);

  return 0;
}

// chr:start-end:strand:offset[,...]
// Return 0 on success, -1 on error
int chrom_pos_list_parse(char *str, ChromPosBuffer *buf)
{
  chrompos_buf_reset(buf);

  size_t len = strlen(str);
  if(len == 0) return 0;

  char *token, *saveptr = NULL;
  for(; (token = strtok_r(str, ",", &saveptr)) != NULL; str = NULL)
  {
    ChromPosOffset obj;
    if(_parse(token, &obj) != 0) return -1;
    chrompos_buf_add(buf, obj);
  }

  return 0;
}

