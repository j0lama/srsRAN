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

#include "rf_udp_imp_trx.h"
#include <inttypes.h>
#include <srsran/config.h>
#include <srsran/phy/utils/vector.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int rf_udp_tx_open(rf_udp_tx_t* q, rf_udp_opts_t opts, char* sock_args)
{
  struct sockaddr_in addr;
  int ret = SRSRAN_ERROR;

  if (q) {
    // Zero object
    bzero(q, sizeof(rf_udp_tx_t));

    // Copy id
    strncpy(q->id, opts.id, UDP_ID_STRLEN - 1);
    q->id[UDP_ID_STRLEN - 1] = '\0';

    // Create socket
    q->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (q->sock < 0) {
      fprintf(stderr, "[udp] Error: creating transmitter socket\n");
      goto clean_exit;
    }
    q->socket_type   = opts.socket_type;
    q->sample_format = opts.sample_format;
    q->frequency_mhz = opts.frequency_mhz;
    q->sample_offset = opts.sample_offset;

    rf_udp_info(q->id, "Connecting transmitter: %s\n", sock_args);

    /* Connect UDP socket */
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    if(inet_pton(AF_INET, sock_args, &(addr.sin_addr)) != 1) {
       fprintf(stderr, "[udp] Error: invalid IP address (%s)\n", sock_args);
        goto clean_exit;
    }
    bzero(&(dpu_addr.sin_zero),8);

    if(connect(q->sock, (struct sockaddr *) &addr, sizeof(struct sockaddr)) < 0) {
      fprintf(stderr, "Error: connecting transmitter socket (%s): %s\n", sock_args, strerror(errno));
      goto clean_exit;
    }

    if (opts.trx_timeout_ms) {
      int timeout = opts.trx_timeout_ms;
      if (setsockopt(q->sock, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
        fprintf(stderr, "Error: setting receive timeout on tx socket\n");
        goto clean_exit;
      }

      if (setsockopt(q->sock, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        fprintf(stderr, "Error: setting send timeout on tx socket\n");
        goto clean_exit;
      }

      struct linger lin;
      lin.l_onoff = 1;
      lin.l_linger = 0;
      if (setsockopt(q->sock, SO_LINGER, &lin, sizeof(lin)) == -1) {
        fprintf(stderr, "Error: setting linger timeout on tx socket\n");
        goto clean_exit;
      }
    }

    if (pthread_mutex_init(&q->mutex, NULL)) {
      fprintf(stderr, "Error: creating mutex\n");
      goto clean_exit;
    }

    q->temp_buffer_convert = srsran_vec_malloc(UDP_MAX_BUFFER_SIZE);
    if (!q->temp_buffer_convert) {
      fprintf(stderr, "Error: allocating rx buffer\n");
      goto clean_exit;
    }

    q->zeros = srsran_vec_malloc(UDP_MAX_BUFFER_SIZE);
    if (!q->zeros) {
      fprintf(stderr, "Error: allocating zeros\n");
      goto clean_exit;
    }
    bzero(q->zeros, UDP_MAX_BUFFER_SIZE);

    q->running = true;

    ret = SRSRAN_SUCCESS;
  }

clean_exit:
  return ret;
}

static int _rf_udp_tx_baseband(rf_udp_tx_t* q, cf_t* buffer, uint32_t nsamples)
{
  int n = SRSRAN_ERROR;

  while (n < 0 && q->running) {
    // convert samples if necessary
    void*    buf       = (buffer) ? buffer : q->zeros;
    uint32_t sample_sz = sizeof(cf_t);

    // Send base-band if request was received
    n = send(q->sock, buf, (size_t)sample_sz*nsamples, 0);
    if (n < 0) {
      if (rf_udp_handle_error(q->id, "tx baseband send")) {
        n = SRSRAN_ERROR;
        goto clean_exit;
      }
    } else if (n != NSAMPLES2NBYTES(nsamples)) {
      rf_udp_error(q->id,
                   "[udp] Error: transmitter expected %d bytes and sent %d. %s.\n",
                   NSAMPLES2NBYTES(nsamples),
                   n,
                   strerror(errno));
      n = SRSRAN_ERROR;
      goto clean_exit;
    }

    // If failed to receive request or send base-band, keep trying
  }

  // Increment sample counter
  q->nsamples += nsamples;
  n = nsamples;

clean_exit:
  return n;
}

int rf_udp_tx_align(rf_udp_tx_t* q, uint64_t ts)
{
  pthread_mutex_lock(&q->mutex);

  int64_t nsamples = (int64_t)ts - (int64_t)q->nsamples;

  if (nsamples > 0) {
    rf_udp_info(q->id, " - Detected Tx gap of %d samples.\n", nsamples);
    _rf_udp_tx_baseband(q, q->zeros, (uint32_t)nsamples);
  }

  pthread_mutex_unlock(&q->mutex);

  return (int)nsamples;
}

int rf_udp_tx_baseband(rf_udp_tx_t* q, cf_t* buffer, uint32_t nsamples)
{
  int n;

  pthread_mutex_lock(&q->mutex);

  if (q->sample_offset > 0) {
    _rf_udp_tx_baseband(q, q->zeros, (uint32_t)q->sample_offset);
    q->sample_offset = 0;
  } else if (q->sample_offset < 0) {
    n = SRSRAN_MIN(-q->sample_offset, nsamples);
    buffer += n;
    nsamples -= n;
    q->sample_offset += n;
    if (nsamples == 0) {
      return n;
    }
  }

  n = _rf_udp_tx_baseband(q, buffer, nsamples);

  pthread_mutex_unlock(&q->mutex);

  return n;
}

int rf_udp_tx_get_nsamples(rf_udp_tx_t* q)
{
  pthread_mutex_lock(&q->mutex);
  int ret = q->nsamples;
  pthread_mutex_unlock(&q->mutex);
  return ret;
}

int rf_udp_tx_zeros(rf_udp_tx_t* q, uint32_t nsamples)
{
  pthread_mutex_lock(&q->mutex);

  rf_udp_info(q->id, " - Tx %d Zeros.\n", nsamples);
  _rf_udp_tx_baseband(q, q->zeros, (uint32_t)nsamples);

  pthread_mutex_unlock(&q->mutex);

  return (int)nsamples;
}

bool rf_udp_tx_match_freq(rf_udp_tx_t* q, uint32_t freq_hz)
{
  bool ret = false;
  if (q) {
    ret = (q->frequency_mhz == 0 || q->frequency_mhz == freq_hz);
  }
  return ret;
}

void rf_udp_tx_close(rf_udp_tx_t* q)
{
  pthread_mutex_lock(&q->mutex);
  q->running = false;
  pthread_mutex_unlock(&q->mutex);

  pthread_mutex_destroy(&q->mutex);

  if (q->zeros) {
    free(q->zeros);
  }

  if (q->temp_buffer_convert) {
    free(q->temp_buffer_convert);
  }

  if (q->sock) {
    close(q->sock);
    q->sock = 0;
  }
}

bool rf_udp_tx_is_running(rf_udp_tx_t* q)
{
  if (!q) {
    return false;
  }

  bool ret = false;
  pthread_mutex_lock(&q->mutex);
  ret = q->running;
  pthread_mutex_unlock(&q->mutex);

  return ret;
}
