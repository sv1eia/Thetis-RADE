/*---------------------------------------------------------------------------*\

  radc_const.h

  RADE V1 system constants, as configured by the reference streaming
  transmitter/receiver (radae/radae_txe.py, radae/radae_rxe.py):

    RADAE(num_features, latent_dim=80, rate_Fs=True, pilots=True, pilot_eq=True,
          eq_mean6=False, cyclic_prefix=0.004, coarse_mag=True,
          time_offset=-16, bottleneck=3), auxdata=True.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_CONST_H
#define RADC_CONST_H

/* Sample rates */
#define RADC_FS              8000     /* modem sample rate (Hz) */
#define RADC_FS_SPEECH       16000    /* speech sample rate (Hz) */

/* OFDM parameters */
#define RADC_NC              30       /* number of carriers */
#define RADC_M               160      /* samples per OFDM symbol (Fs/Rs') */
#define RADC_NCP             32       /* cyclic prefix samples (0.004*Fs) */
#define RADC_NS              4        /* data symbols per modem frame */
#define RADC_NZMF            3        /* latent vectors per modem frame */

/* Neural-codec parameters */
#define RADC_LATENT_DIM      80       /* latent vector dimension */
#define RADC_FRAMES_PER_STEP 4        /* encoder/decoder stride */
#define RADC_NUM_FEATURES    20       /* used vocoder features */
#define RADC_NUM_FEATURES_AUX 21      /* with auxiliary (unique-word) symbol */
#define RADC_NB_TOTAL_FEATURES 36     /* padded feature vector size */

/* Derived frame sizes */
#define RADC_NSYMB_MF        (RADC_NS + 1)                       /* symbols/frame: 1 pilot + Ns data */
#define RADC_NMF             ((RADC_NS + 1) * (RADC_M + RADC_NCP)) /* 960 samples */
#define RADC_NEOO            ((RADC_NS + 2) * (RADC_M + RADC_NCP)) /* 1152 samples */
#define RADC_NSEOO           ((RADC_NS - 1) * RADC_NC)            /* EOO data symbols: 90 */
#define RADC_BPS             2                                    /* bits per QPSK symbol */

/* Pilots */
#define RADC_BARKER_LEN      13

/* Bottleneck modes (1=tanh latent, 2=per-symbol limit, 3=PA saturation). */
#define RADC_BOTTLENECK_DEFAULT 3

/* Band-pass filter */
#define RADC_BPF_NTAP        101

/* Acquisition */
#define RADC_ACQ_FRANGE      100.0f
#define RADC_ACQ_FSTEP       2.5f
#define RADC_ACQ_NFREQ       40       /* ceil(FRANGE/FSTEP) frequency bins */
#define RADC_ACQ_PERR1       0.00001f
#define RADC_ACQ_PERR2       0.0001f

/* Receiver sync */
#define RADC_STATE_SEARCH    0
#define RADC_STATE_CANDIDATE 1
#define RADC_STATE_SYNC      2
#define RADC_TUNSYNC         3.0f     /* seconds before losing sync */
#define RADC_UW_ERR_THRESH   7        /* unique-word errors/sec before unsync */
#define RADC_TIME_OFFSET     (-16)    /* fine timing offset into the CP */
#define RADC_LOCAL_PATH_DELAY_S 0.0025f /* assumed path delay for the LS EQ */

/* Receive buffer: two modem frames + one symbol + CP, for timing slips. */
#define RADC_RX_BUF_SIZE     (2 * RADC_NMF + RADC_M + RADC_NCP)

#endif /* RADC_CONST_H */
