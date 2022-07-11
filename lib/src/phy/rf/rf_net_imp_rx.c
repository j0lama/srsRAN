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

#include "rf_net_imp_trx.h"
#include <inttypes.h>
#include <srsran/phy/utils/vector.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

int recv_udp(int sock, void * buffer)
{
  int n = 0;
  int offset = 0;

  do {
    n = recv(sock, buffer+offset, NET_DATAFRAME_MAX_LENGTH, 0);
    if(n == -1)
      return -1;
    offset += n;
  } while(n == NET_DATAFRAME_MAX_LENGTH);

  return offset;
}

int recv_tcp(int sock, void * buffer)
{
  return recv(sock, buffer, NET_MAX_BUFFER_SIZE, 0);
}

static void* rf_net_async_rx_thread(void* h)
{
  rf_net_rx_t* q = (rf_net_rx_t*)h;

  while (q->sock && rf_net_rx_is_running(q)) {
    int     nbytes = 0;
    int     n      = SRSRAN_ERROR;

    rf_net_info(q->id, "-- ASYNC RX wait...\n");

    // Receive baseband
    n = 1;
    for (n = (n < 0) ? 0 : -1; n < 0 && rf_net_rx_is_running(q);) {
      n = q->recv_message(q->sock, q->temp_buffer);
      if (n == -1) {
        if (rf_net_handle_error(q->id, "asynchronous rx baseband receive")) {
          return NULL;
        }

      } else if (n > NET_MAX_BUFFER_SIZE) {
        fprintf(stderr,
                "[net] Error: receiver expected <= %zu bytes and received %d at channel %d.\n",
                NET_MAX_BUFFER_SIZE,
                n,
                0);
        return NULL;
      } else {
        nbytes = n;
      }
    }

    // Write received data in buffer
    if (nbytes > 0) {
      n = -1;

      // Try to write in ring buffer
      while (n < 0 && rf_net_rx_is_running(q)) {
        n = srsran_ringbuffer_write_timed(&q->ringbuffer, q->temp_buffer, nbytes, q->trx_timeout_ms);
        if (n == SRSRAN_ERROR_TIMEOUT && q->log_trx_timeout) {
          fprintf(stderr, "Error: timeout writing samples to ringbuffer after %dms\n", q->trx_timeout_ms);
        }
      }

      // Check write
      if (nbytes == n) {
        rf_net_info(q->id,
                    "   - received %d baseband samples (%d B). %d samples available.\n",
                    NBYTES2NSAMPLES(n),
                    n,
                    NBYTES2NSAMPLES(srsran_ringbuffer_status(&q->ringbuffer)));
      }
    }
  }

  return NULL;
}

int rf_net_rx_open(rf_net_rx_t* q, rf_net_opts_t opts, char* sock_args)
{
  int ret = SRSRAN_ERROR;
  struct sockaddr_in addr;
  struct sockaddr_in remote_addr;
  int addrlen = sizeof(struct sockaddr);

  if (q) {
    // Zero object
    bzero(q, sizeof(rf_net_rx_t));

    // Copy id
    strncpy(q->id, opts.id, NET_ID_STRLEN - 1);
    q->id[NET_ID_STRLEN - 1] = '\0';

    /* Register the recv method based on the protocol */
    if(opts.proto == NET_TCP)
      q->recv_message = recv_tcp;
    else
      q->recv_message = recv_udp;

    // Create socket
    if(opts.proto == NET_TCP) {
      q->local_sock = socket(AF_INET, SOCK_STREAM, 0);
      q->sock = 0;
      if (!q->local_sock) {
        fprintf(stderr, "[net] Error creating transmitter TCP socket\n");
        goto clean_exit;
      }
    }
    else {
      q->sock = socket(AF_INET, SOCK_DGRAM, 0);
      q->local_sock = 0;
      if (!q->sock) {
        fprintf(stderr, "[net] Error creating transmitter UDP socket\n");
        goto clean_exit;
      }
    }

    q->socket_type        = opts.socket_type;
    q->sample_format      = opts.sample_format;
    q->frequency_mhz      = opts.frequency_mhz;
    q->fail_on_disconnect = opts.fail_on_disconnect;
    q->sample_offset      = opts.sample_offset;
    q->trx_timeout_ms     = opts.trx_timeout_ms;
    q->log_trx_timeout    = opts.log_trx_timeout;

    rf_net_info(q->id, "Binding receiver: %s\n", sock_args);

    /* Bind NET socket */
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NET_PORT);
    if(inet_pton(AF_INET, sock_args, &(addr.sin_addr)) != 1) {
       fprintf(stderr, "[net] Error: invalid IP address (%s)\n", sock_args);
        goto clean_exit;
    }
    bzero(&(addr.sin_zero),8);

    if(opts.proto == NET_TCP) {
      if (bind(q->local_sock,(struct sockaddr *)&addr, sizeof(struct sockaddr)) == -1) {
        fprintf(stderr, "Error: binding receiver TCP socket: %s\n", strerror(errno));
        goto clean_exit;
      }
    }
    else {
      if (bind(q->sock,(struct sockaddr *)&addr, sizeof(struct sockaddr)) == -1) {
        fprintf(stderr, "Error: binding receiver UDP socket: %s\n", strerror(errno));
        goto clean_exit;
      }
    }

    if (!opts.trx_timeout_ms) {
      struct timeval tv;
      tv.tv_sec = ((int) opts.trx_timeout_ms) / 1000;
      tv.tv_usec = 1000* (((int) opts.trx_timeout_ms) % 1000);
      if (setsockopt(q->sock, SOL_SOCKET, SO_RCVTIMEO, (void *) &tv, (socklen_t) sizeof(tv)) == -1) {
        fprintf(stderr, "Error: setting receive timeout on rx socket (%s)\n", strerror(errno));
        goto clean_exit;
      }

      if (setsockopt(q->sock, SOL_SOCKET, SO_SNDTIMEO, (void *) &tv, (socklen_t) sizeof(tv)) == -1) {
        fprintf(stderr, "Error: setting send timeout on rx socket (%s)\n", strerror(errno));
        goto clean_exit;
      }

      struct linger lin;
      lin.l_onoff = 1;
      lin.l_linger = 0;
      if (setsockopt(q->sock, SOL_SOCKET, SO_LINGER, (void *) &lin, (socklen_t) sizeof(lin)) == -1) {
        fprintf(stderr, "Error: setting linger timeout on rx socket (%s)\n", strerror(errno));
        goto clean_exit;
      }
    }

    /* If the protocol is TCP, listen and accept */
    if(opts.proto == NET_TCP) {
      /* Listen */
      if(listen(q->local_sock, 5) == -1) {
        fprintf(stderr, "Error: listening reciever TCP socket (%s)\n", strerror(errno));
        goto clean_exit;
      }

      /* Accept UE */
      q->sock = accept(q->local_sock, (struct sockaddr *)&remote_addr, (socklen_t *) &addrlen);
      if(q->sock == -1) {
        fprintf(stderr, "Error: accepting peer connection (%s)\n", strerror(errno));
        goto clean_exit;
      }
    }

    if (srsran_ringbuffer_init(&q->ringbuffer, NET_MAX_BUFFER_SIZE)) {
      fprintf(stderr, "Error: initiating ringbuffer\n");
      goto clean_exit;
    }

    q->temp_buffer = srsran_vec_malloc(NET_MAX_BUFFER_SIZE);
    if (!q->temp_buffer) {
      fprintf(stderr, "Error: allocating rx buffer\n");
      goto clean_exit;
    }

    q->temp_buffer_convert = srsran_vec_malloc(NET_MAX_BUFFER_SIZE);
    if (!q->temp_buffer_convert) {
      fprintf(stderr, "Error: allocating rx buffer\n");
      goto clean_exit;
    }

    if (pthread_mutex_init(&q->mutex, NULL)) {
      fprintf(stderr, "Error: creating mutex\n");
      goto clean_exit;
    }

    q->running = true;
    if (pthread_create(&q->thread, NULL, rf_net_async_rx_thread, q)) {
      fprintf(stderr, "Error: creating thread\n");
      goto clean_exit;
    }

    ret = SRSRAN_SUCCESS;
  }

clean_exit:
  return ret;
}

int rf_net_rx_baseband(rf_net_rx_t* q, cf_t* buffer, uint32_t nsamples)
{
  void*    dst_buffer = buffer;
  uint32_t sample_sz  = sizeof(cf_t);
  if (q->sample_format != NET_TYPE_FC32) {
    dst_buffer = q->temp_buffer_convert;
    sample_sz  = 2 * sizeof(short);
  }

  // If the read needs to be delayed
  while (q->sample_offset > 0) {
    uint32_t n_offset = SRSRAN_MIN(q->sample_offset, NBYTES2NSAMPLES(NET_MAX_BUFFER_SIZE));
    srsran_vec_zero(q->temp_buffer, n_offset);
    int n = srsran_ringbuffer_write(&q->ringbuffer, q->temp_buffer, (int)(n_offset * sample_sz));
    if (n < SRSRAN_SUCCESS) {
      return n;
    }
    q->sample_offset -= n_offset;
  }

  // If the read needs to be advanced
  while (q->sample_offset < 0) {
    uint32_t n_offset = SRSRAN_MIN(-q->sample_offset, NBYTES2NSAMPLES(NET_MAX_BUFFER_SIZE));
    int      n =
        srsran_ringbuffer_read_timed(&q->ringbuffer, q->temp_buffer, (int)(n_offset * sample_sz), q->trx_timeout_ms);
    if (n < SRSRAN_SUCCESS) {
      return n;
    }
    q->sample_offset += n_offset;
  }

  int n = srsran_ringbuffer_read_timed(&q->ringbuffer, dst_buffer, sample_sz * nsamples, q->trx_timeout_ms);
  if (n < 0) {
    return n;
  }

  if (q->sample_format == NET_TYPE_SC16) {
    srsran_vec_convert_if(dst_buffer, INT16_MAX, (float*)buffer, 2 * nsamples);
  }

  return n;
}

bool rf_net_rx_match_freq(rf_net_rx_t* q, uint32_t freq_hz)
{
  bool ret = false;
  if (q) {
    ret = (q->frequency_mhz == 0 || q->frequency_mhz == freq_hz);
  }
  return ret;
}

void rf_net_rx_close(rf_net_rx_t* q)
{
  rf_net_info(q->id, "Closing ...\n");

  pthread_mutex_lock(&q->mutex);
  q->running = false;
  pthread_mutex_unlock(&q->mutex);

  if (q->thread) {
    pthread_join(q->thread, NULL);
    pthread_detach(q->thread);
  }

  pthread_mutex_destroy(&q->mutex);

  srsran_ringbuffer_free(&q->ringbuffer);

  if (q->temp_buffer) {
    free(q->temp_buffer);
  }

  if (q->temp_buffer_convert) {
    free(q->temp_buffer_convert);
  }

  if (q->sock) {
    close(q->sock);
    q->sock = 0;
  }
  if(q->local_sock) {
    close(q->local_sock);
    q->local_sock = 0;
  }
}

bool rf_net_rx_is_running(rf_net_rx_t* q)
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
