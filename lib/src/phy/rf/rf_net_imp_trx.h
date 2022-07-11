/**
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#ifndef SRSRAN_RF_NET_IMP_TRX_H
#define SRSRAN_RF_NET_IMP_TRX_H

#include <pthread.h>
#include <srsran/phy/utils/ringbuffer.h>
#include <stdbool.h>

/* Definitions */
#define VERBOSE (0)
#define NET_MONITOR (0)
#define NSAMPLES2NBYTES(X) (((uint32_t)(X)) * sizeof(cf_t))
#define NBYTES2NSAMPLES(X) ((X) / sizeof(cf_t))
#define NET_MAX_BUFFER_SIZE (NSAMPLES2NBYTES(3072000)) // 10 subframes at 20 MHz
#define NET_TIMEOUT_MS (2000)
#define NET_BASERATE_DEFAULT_HZ (23040000)
#define NET_ID_STRLEN 16
#define NET_MAX_GAIN_DB (30.0f)
#define NET_MIN_GAIN_DB (0.0f)

/* NET definitions */
#define NET_PORT 8008
#define NET_DATAFRAME_MAX_LENGTH 50000

typedef enum {NET_TCP, NET_UDP} rf_net_protocol_t;

typedef enum { NET_TYPE_FC32 = 0, NET_TYPE_SC16 } rf_net_format_t;

typedef struct {
  char            id[NET_ID_STRLEN];
  uint32_t        socket_type;
  rf_net_format_t sample_format;
  int             sock;
  uint64_t        nsamples;
  bool            running;
  pthread_mutex_t mutex;
  cf_t*           zeros;
  void*           temp_buffer_convert;
  uint32_t        frequency_mhz;
  int32_t         sample_offset;
  int             (*send_message)(int socket, void * buffer, size_t sz);
} rf_net_tx_t;

typedef struct {
  char                id[NET_ID_STRLEN];
  uint32_t            socket_type;
  rf_net_format_t     sample_format;
  int                 sock;
  int                 local_sock;
  uint64_t            nsamples;
  bool                running;
  pthread_t           thread;
  pthread_mutex_t     mutex;
  srsran_ringbuffer_t ringbuffer;
  cf_t*               temp_buffer;
  void*               temp_buffer_convert;
  uint32_t            frequency_mhz;
  bool                fail_on_disconnect;
  uint32_t            trx_timeout_ms;
  bool                log_trx_timeout;
  int32_t             sample_offset;
  int                 (*recv_message)(int socket, void * buffer);
} rf_net_rx_t;

typedef struct {
  const char*     id;
  uint32_t        socket_type;
  rf_net_format_t sample_format;
  uint32_t        frequency_mhz;
  bool            fail_on_disconnect;
  uint32_t        trx_timeout_ms;
  bool            log_trx_timeout;
  int32_t         sample_offset; ///< offset in samples
  rf_net_protocol_t proto;
} rf_net_opts_t;

/*
 * Common functions
 */
SRSRAN_API void rf_net_info(char* id, const char* format, ...);

SRSRAN_API void rf_net_error(char* id, const char* format, ...);

SRSRAN_API int rf_net_handle_error(char* id, const char* text);

/*
 * Transmitter functions
 */
SRSRAN_API int rf_net_tx_open(rf_net_tx_t* q, rf_net_opts_t opts, char* sock_args);

SRSRAN_API int rf_net_tx_align(rf_net_tx_t* q, uint64_t ts);

SRSRAN_API int rf_net_tx_baseband(rf_net_tx_t* q, cf_t* buffer, uint32_t nsamples);

SRSRAN_API int rf_net_tx_get_nsamples(rf_net_tx_t* q);

SRSRAN_API int rf_net_tx_zeros(rf_net_tx_t* q, uint32_t nsamples);

SRSRAN_API bool rf_net_tx_match_freq(rf_net_tx_t* q, uint32_t freq_hz);

SRSRAN_API void rf_net_tx_close(rf_net_tx_t* q);

SRSRAN_API bool rf_net_tx_is_running(rf_net_tx_t* q);

/*
 * Receiver functions
 */
SRSRAN_API int rf_net_rx_open(rf_net_rx_t* q, rf_net_opts_t opts, char* sock_args);

SRSRAN_API int rf_net_rx_baseband(rf_net_rx_t* q, cf_t* buffer, uint32_t nsamples);

SRSRAN_API bool rf_net_rx_match_freq(rf_net_rx_t* q, uint32_t freq_hz);

SRSRAN_API void rf_net_rx_close(rf_net_rx_t* q);

SRSRAN_API bool rf_net_rx_is_running(rf_net_rx_t* q);

#endif // SRSRAN_RF_NET_IMP_TRX_H
