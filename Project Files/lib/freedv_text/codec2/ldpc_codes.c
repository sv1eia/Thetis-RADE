/*
  FILE...: ldpc_codes.c   (slimmed for Thetis vendor)
  AUTHOR.: David Rowe
  CREATED: July 2020

  Array of LDPC codes used for various Codec2 waveforms.

  THETIS-RADE NOTE (Christos Nikolaou / SV1EIA, 2026,
                      sv1eia@gmail.com):
    This file is a slim cut-down of upstream codec2-1.2.0/src/ldpc_codes.c.
*/

#include "ldpc_codes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "HRA_56_56.h"

struct LDPC ldpc_codes[] = {
    /* short rate 1/2 code -- the only one rade_text.c uses (HRA_56_56) */
    {"HRA_56_56", HRA_56_56_MAX_ITER, 0, 1, 1, HRA_56_56_CODELENGTH,
     HRA_56_56_NUMBERPARITYBITS, HRA_56_56_NUMBERROWSHCOLS,
     HRA_56_56_MAX_ROW_WEIGHT, HRA_56_56_MAX_COL_WEIGHT,
     (uint16_t *)HRA_56_56_H_rows, (uint16_t *)HRA_56_56_H_cols},
};

int ldpc_codes_num(void) { return sizeof(ldpc_codes) / sizeof(struct LDPC); }

void ldpc_codes_list() {
  fprintf(stderr, "\n");
  for (int c = 0; c < ldpc_codes_num(); c++) {
    int n = ldpc_codes[c].NumberRowsHcols + ldpc_codes[c].NumberParityBits;
    int k = ldpc_codes[c].NumberRowsHcols;
    float rate = (float)k / n;
    fprintf(stderr, "%-20s rate %3.2f (%d,%d) \n", ldpc_codes[c].name,
            (double)rate, n, k);
  }
  fprintf(stderr, "\n");
}

int ldpc_codes_find(char name[]) {
  int code_index = -1;
  for (int c = 0; c < ldpc_codes_num(); c++)
    if (strcmp(ldpc_codes[c].name, name) == 0) code_index = c;
  assert(code_index != -1); /* code not found */
  return code_index;
}
