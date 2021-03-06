/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2014 The libLTE Developers. See the
 * COPYRIGHT file at the top-level directory of this distribution.
 *
 * \section LICENSE
 *
 * This file is part of the libLTE library.
 *
 * libLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * A copy of the GNU Lesser General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>

#include "liblte/phy/phch/regs.h"
#include "liblte/phy/phch/pcfich.h"
#include "liblte/phy/common/phy_common.h"
#include "liblte/phy/utils/bit.h"
#include "liblte/phy/utils/vector.h"
#include "liblte/phy/utils/debug.h"

// Table 5.3.4-1
static char cfi_table[4][PCFICH_CFI_LEN] = { { 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1 }, { 1, 0, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 1, 1, 0 }, { 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } // reserved
};

bool pcfich_exists(int nframe, int nslot) {
  return true;
}

/** Initializes the pcfich channel receiver. 
 * On error, returns -1 and frees the structrure 
 */
int pcfich_init(pcfich_t *q, regs_t *regs, lte_cell_t cell) {
  int ret = LIBLTE_ERROR_INVALID_INPUTS;
  
  if (q                         != NULL &&
      regs                      != NULL &&
      lte_cell_isvalid(&cell)) 
  {   
    ret = LIBLTE_ERROR;
    
    bzero(q, sizeof(pcfich_t));
    q->cell = cell;
    q->regs = regs;

    if (modem_table_lte(&q->mod, LTE_QPSK, false)) {
      goto clean;
    }

    demod_hard_init(&q->demod);
    demod_hard_table_set(&q->demod, LTE_QPSK);

    for (int nsf = 0; nsf < NSUBFRAMES_X_FRAME; nsf++) {
      if (sequence_pcfich(&q->seq_pcfich[nsf], 2 * nsf, q->cell.id)) {
        goto clean;
      }
    }

    q->nof_symbols = PCFICH_RE;

    ret = LIBLTE_SUCCESS;
  }
  
  clean: 
  if (ret == LIBLTE_ERROR) {
    pcfich_free(q);
  }
  return ret;
}

void pcfich_free(pcfich_t *q) {
  for (int ns = 0; ns < NSUBFRAMES_X_FRAME; ns++) {
    sequence_free(&q->seq_pcfich[ns]);
  }
  modem_table_free(&q->mod);
}

/** Finds the CFI with minimum distance with the vector of received 32 bits.
 * Saves the CFI value in the cfi pointer and returns the distance.
 */
int pcfich_cfi_decode(char bits[PCFICH_CFI_LEN], uint32_t *cfi) {
  int i, j;
  int distance, index = -1;
  int min = 32;

  for (i = 0; i < 3; i++) {
    distance = 0;
    for (j = 0; j < PCFICH_CFI_LEN; j++) {
      distance += (bits[j] ^ cfi_table[i][j]);
    }
    DEBUG("CFI=%d, distance:%d\n", i, distance);
    if (distance < min) {
      min = distance;
      index = i;
    }
  }
  if (cfi) {
    *cfi = index + 1;
  }
  return min;
}

/** Encodes the CFI producing a vector of 32 bits.
 *  36.211 10.3 section 5.3.4
 */
int pcfich_cfi_encode(int cfi, char bits[PCFICH_CFI_LEN]) {
  if (cfi < 1 || cfi > 3) {
    return LIBLTE_ERROR_INVALID_INPUTS;
  } else{
    memcpy(bits, cfi_table[cfi - 1], PCFICH_CFI_LEN * sizeof(char));
    return LIBLTE_SUCCESS;    
  }
}

/* Decodes the PCFICH channel and saves the CFI in the cfi pointer.
 *
 * Returns 1 if successfully decoded the CFI, 0 if not and -1 on error
 */
int pcfich_decode(pcfich_t *q, cf_t *slot_symbols, cf_t *ce[MAX_PORTS],
    uint32_t nsubframe, uint32_t *cfi, uint32_t *distance) {
  int dist;

  /* Set pointers for layermapping & precoding */
  int i;
  cf_t *x[MAX_LAYERS];
  cf_t *ce_precoding[MAX_PORTS];

  if (q                 != NULL                 && 
      slot_symbols      != NULL                 && 
      nsubframe         <  NSUBFRAMES_X_FRAME) 
  {

    /* number of layers equals number of ports */
    for (i = 0; i < MAX_PORTS; i++) {
      x[i] = q->pcfich_x[i];
    }
    for (i = 0; i < MAX_PORTS; i++) {
      ce_precoding[i] = q->ce[i];
    }

    /* extract symbols */
    if (q->nof_symbols
        != regs_pcfich_get(q->regs, slot_symbols, q->pcfich_symbols[0])) {
      fprintf(stderr, "There was an error getting the PCFICH symbols\n");
      return LIBLTE_ERROR;
    }

    /* extract channel estimates */
    for (i = 0; i < q->cell.nof_ports; i++) {
      if (q->nof_symbols != regs_pcfich_get(q->regs, ce[i], q->ce[i])) {
        fprintf(stderr, "There was an error getting the PCFICH symbols\n");
        return LIBLTE_ERROR;
      }
    }

    /* in control channels, only diversity is supported */
    if (q->cell.nof_ports == 1) {
      /* no need for layer demapping */
      predecoding_single_zf(q->pcfich_symbols[0], q->ce[0], q->pcfich_d,
          q->nof_symbols);
    } else {
      predecoding_diversity_zf(q->pcfich_symbols[0], ce_precoding, x,
          q->cell.nof_ports, q->nof_symbols);
      layerdemap_diversity(x, q->pcfich_d, q->cell.nof_ports,
          q->nof_symbols / q->cell.nof_ports);
    }

    /* demodulate symbols */
    demod_hard_demodulate(&q->demod, q->pcfich_d, q->data, q->nof_symbols);

    /* Scramble with the sequence for slot nslot */
    scrambling_b(&q->seq_pcfich[nsubframe], q->data);

    /* decode CFI */
    dist = pcfich_cfi_decode(q->data, cfi);
    if (distance) {
      *distance = dist;
    }
    if (dist < PCFICH_MAX_DISTANCE) {
      return 1;
    } else {
      return 0;
    }
  } else {
    return LIBLTE_ERROR_INVALID_INPUTS;
  }
  
}

/** Encodes CFI and maps symbols to the slot
 */
int pcfich_encode(pcfich_t *q, uint32_t cfi, cf_t *slot_symbols[MAX_PORTS],
    uint32_t subframe) {
  int i;

  if (q                 != NULL                 && 
      cfi               <  3                    &&
      slot_symbols      != NULL                 && 
      subframe         <  NSUBFRAMES_X_FRAME) 
  {

    /* Set pointers for layermapping & precoding */
    cf_t *x[MAX_LAYERS];
    cf_t *symbols_precoding[MAX_PORTS];

    /* number of layers equals number of ports */
    for (i = 0; i < q->cell.nof_ports; i++) {
      x[i] = q->pcfich_x[i];
    }
    for (i = 0; i < MAX_PORTS; i++) {
      symbols_precoding[i] = q->pcfich_symbols[i];
    }

    /* pack CFI */
    pcfich_cfi_encode(cfi, q->data);

    /* scramble for slot sequence nslot */
    scrambling_b(&q->seq_pcfich[subframe], q->data);

    mod_modulate(&q->mod, q->data, q->pcfich_d, PCFICH_CFI_LEN);

    /* layer mapping & precoding */
    if (q->cell.nof_ports > 1) {
      layermap_diversity(q->pcfich_d, x, q->cell.nof_ports, q->nof_symbols);
      precoding_diversity(x, symbols_precoding, q->cell.nof_ports,
          q->nof_symbols / q->cell.nof_ports);
    } else {
      memcpy(q->pcfich_symbols[0], q->pcfich_d, q->nof_symbols * sizeof(cf_t));
    }

    /* mapping to resource elements */
    for (i = 0; i < q->cell.nof_ports; i++) {
      if (regs_pcfich_put(q->regs, q->pcfich_symbols[i], slot_symbols[i]) < 0) {
        fprintf(stderr, "Error putting PCHICH resource elements\n");
        return LIBLTE_ERROR;
      }
    }
    return LIBLTE_SUCCESS;
  } else {
    return LIBLTE_ERROR_INVALID_INPUTS;
  }
}


