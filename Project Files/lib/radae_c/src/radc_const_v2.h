/*---------------------------------------------------------------------------*\

  radc_const_v2.h

  RADE V2 system constants, as configured by the reference streaming
  transmitter/receiver (radae/tx2.py, radae/rx2.py, radae/radae_v2.py):

    RADAE(num_features=21, latent_dim=56, EbNodB=100, Nzmf=1, rate_Fs=True,
          cyclic_prefix=0.004, w1_dec=128, w1_dec_stateful=128, peak=True),
    auxdata=True, pilots=False.

  V2 waveform differs sharply from V1: no pilots in the data frame, Nc=14
  carriers, M=128 IDFT, Ns=2 symbols/frame, and a detection-only EOO (six
  pend_cp symbols). DSP constants use the RADC_V2_ prefix; the neural-codec
  constants come from the generated rade_v2_constants.h (RADE_V2_ prefix).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_CONST_V2_H
#define RADC_CONST_V2_H

/* Sample rates */
#define RADC_V2_FS              8000     /* modem sample rate (Hz) */
#define RADC_V2_FS_SPEECH       16000    /* speech sample rate (Hz) */

/* OFDM parameters (Rs'=62.5 Hz, M=Fs/Rs'=128) */
#define RADC_V2_NC              14       /* number of carriers (data only, no pilots) */
#define RADC_V2_M               128      /* IDFT/DFT size (samples per OFDM symbol) */
#define RADC_V2_NCP             32       /* cyclic prefix samples (0.004*Fs) */
#define RADC_V2_NS              2        /* data symbols per modem frame */
#define RADC_V2_NZMF            1        /* latent vectors per modem frame */
#define RADC_V2_CARRIER1_INDEX  17       /* round((1500 - 62.5*Nc/2)/62.5) */

/* Neural-codec parameters (mirror rade_v2_constants.h, RADC_V2_ prefix) */
#define RADC_V2_LATENT_DIM      56       /* latent vector dimension (= 28 QPSK syms) */
#define RADC_V2_FRAMES_PER_STEP 4        /* encoder/decoder stride */
#define RADC_V2_NUM_FEATURES    20       /* used vocoder features */
#define RADC_V2_NUM_FEATURES_AUX 21      /* with auxiliary symbol */
#define RADC_V2_NB_TOTAL_FEATURES 36     /* padded feature vector size */

/* Derived frame sizes */
#define RADC_V2_SYM_LEN         (RADC_V2_M + RADC_V2_NCP)            /* 160 */
#define RADC_V2_NMF             (RADC_V2_NS * RADC_V2_SYM_LEN)       /* 320 samples/modem frame */
#define RADC_V2_NSMF            (RADC_V2_NZMF * RADC_V2_LATENT_DIM / 2) /* 28 QPSK syms/frame */
#define RADC_V2_N_EOO           6                                    /* pend_cp symbols in eoo_v2 */
#define RADC_V2_NEOO            (RADC_V2_N_EOO * RADC_V2_SYM_LEN)     /* 960 EOO samples */
#define RADC_V2_BPS             2                                    /* bits per QPSK symbol */
#define RADC_V2_BARKER_LEN      13

/* EOO V2 gain: backoff = 10^(-8/20), gain = backoff * M / sqrt(Nc) (peak <= 1). */
#define RADC_V2_EOO_BACKOFF_DB  (-8.0)

/* Timing (samples) */
#define RADC_V2_TIME_OFFSET         (-16)   /* time-domain sampling offset into CP */
#define RADC_V2_CORRECT_TIME_OFFSET (-8)    /* freq-domain applied delay/advance */

/* Receiver (RADEv2Receiver) constants */
#define RADC_V2_ACQ_ALPHA   0.95f           /* Ry_smooth IIR coefficient */
#define RADC_V2_TRACK_BETA  0.999f          /* delta_hat / freq_offset IIR coefficient */
#define RADC_V2_TSIG        0.38f           /* signal-detect threshold on |Ry_smooth| */
#define RADC_V2_TSIN        4.0f            /* sine-wave detect ratio threshold */
#define RADC_V2_TEOO        0.75f           /* smoothed pend-correlation EOO threshold */
#define RADC_V2_ALPHA_EOO   0.70f           /* EOO pend-correlation IIR coefficient */
#define RADC_V2_HANGOVER    75              /* symbols of signal loss before unsync */
#define RADC_V2_SNR_CORR_A  1.24392558f     /* SNR linear correction slope */
#define RADC_V2_SNR_CORR_B  3.33253932f     /* SNR linear correction offset (dB) */
#define RADC_V2_AGC_TARGET_DB (-3.0)        /* AGC RMS target = 1.0 * 10^(-3/20) */

#endif /* RADC_CONST_V2_H */
