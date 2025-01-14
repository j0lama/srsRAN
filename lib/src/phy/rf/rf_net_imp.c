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

#include "rf_net_imp.h"
#include "rf_helper.h"
#include "rf_plugin.h"
#include "rf_net_imp_trx.h"
#include <math.h>
#include <srsran/phy/common/phy_common.h>
#include <srsran/phy/common/timestamp.h>
#include <srsran/phy/utils/vector.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

typedef struct {
  // Common attributes
  char*            devname;
  srsran_rf_info_t info;
  uint32_t         nof_channels;

  // RF State
  uint32_t srate; // radio rate configured by upper layers
  uint32_t base_srate;
  uint32_t decim_factor; // decimation factor between base_srate used on transport on radio's rate
  double   rx_gain;
  double   tx_gain;
  uint32_t tx_freq_mhz[SRSRAN_MAX_CHANNELS];
  uint32_t rx_freq_mhz[SRSRAN_MAX_CHANNELS];
  bool     tx_off;
  char     id[RF_PARAM_LEN];
  char     proto[RF_PARAM_LEN];

  // Server
  rf_net_tx_t transmitter[SRSRAN_MAX_CHANNELS];
  rf_net_rx_t receiver[SRSRAN_MAX_CHANNELS];

  // Various sample buffers
  cf_t* buffer_decimation[SRSRAN_MAX_CHANNELS];
  cf_t* buffer_tx;

  // Rx timestamp
  uint64_t next_rx_ts;

  pthread_mutex_t tx_config_mutex;
  pthread_mutex_t rx_config_mutex;
  pthread_mutex_t decim_mutex;
  pthread_mutex_t rx_gain_mutex;
} rf_net_handler_t;

static void update_rates(rf_net_handler_t* handler, double srate);

/*
 * Static Atributes
 */
const char net_devname[4] = "net";

/*
 * Static methods
 */

void rf_net_info(char* id, const char* format, ...)
{
#if VERBOSE
  struct timeval t;
  gettimeofday(&t, NULL);
  va_list args;
  va_start(args, format);
  printf("[%s@%02ld.%06ld] ", id ? id : "net", t.tv_sec % 10, t.tv_usec);
  vprintf(format, args);
  va_end(args);
#else  /* VERBOSE */
  // Do nothing
#endif /* VERBOSE */
}

void rf_net_error(char* id, const char* format, ...)
{
  struct timeval t;
  gettimeofday(&t, NULL);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

static inline int update_ts(void* h, uint64_t* ts, int nsamples, const char* dir)
{
  int ret = SRSRAN_ERROR;

  if (h && nsamples > 0) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;

    (*ts) += nsamples;

    srsran_timestamp_t _ts = {};
    srsran_timestamp_init_uint64(&_ts, *ts, handler->base_srate);
    rf_net_info(
        handler->id, "    -> next %s time after %d samples: %d + %.3f\n", dir, nsamples, _ts.full_secs, _ts.frac_secs);

    ret = SRSRAN_SUCCESS;
  }

  return ret;
}

int rf_net_handle_error(char* id, const char* text)
{
  int ret = SRSRAN_SUCCESS;

  switch (errno) {
    // handled errors
    case EAGAIN:
      rf_net_info(id, "Warning %s: %s\n", text, strerror(errno));
      break;

    // critical non-handled errors
    default:
      ret = SRSRAN_ERROR;
      rf_net_error(id, "Error %s: %s\n", text, strerror(errno));
  }

  return ret;
}

/*
 * Public methods
 */

void rf_net_suppress_stdout(void* h)
{
  // do nothing
}

void rf_net_register_error_handler(void* h, srsran_rf_error_handler_t new_handler, void* arg)
{
  // do nothing
}

const char* rf_net_devname(void* h)
{
  return net_devname;
}

int rf_net_start_rx_stream(void* h, bool now)
{
  return SRSRAN_SUCCESS;
}

int rf_net_stop_rx_stream(void* h)
{
  return 0;
}

void rf_net_flush_buffer(void* h)
{
  printf("%s\n", __FUNCTION__);
}

bool rf_net_has_rssi(void* h)
{
  return false;
}

float rf_net_get_rssi(void* h)
{
  return 0.0;
}

int rf_net_open(char* args, void** h)
{
  return rf_net_open_multi(args, h, 1);
}

int rf_net_open_multi(char* args, void** h, uint32_t nof_channels)
{
  int ret = SRSRAN_ERROR;
  if (h && nof_channels < SRSRAN_MAX_CHANNELS) {
    *h = NULL;

    rf_net_handler_t* handler = (rf_net_handler_t*)malloc(sizeof(rf_net_handler_t));
    if (!handler) {
      perror("malloc");
      return SRSRAN_ERROR;
    }
    bzero(handler, sizeof(rf_net_handler_t));
    *h                  = handler;
    handler->base_srate = NET_BASERATE_DEFAULT_HZ; // Sample rate for 100 PRB cell
    pthread_mutex_lock(&handler->rx_gain_mutex);
    handler->rx_gain = 0.0;
    pthread_mutex_unlock(&handler->rx_gain_mutex);
    handler->info.max_rx_gain = NET_MAX_GAIN_DB;
    handler->info.min_rx_gain = NET_MIN_GAIN_DB;
    handler->info.max_tx_gain = NET_MAX_GAIN_DB;
    handler->info.min_tx_gain = NET_MIN_GAIN_DB;
    handler->nof_channels     = nof_channels;
    strcpy(handler->id, "net\0");

    rf_net_opts_t rx_opts = {};
    rf_net_opts_t tx_opts = {};
    tx_opts.id            = handler->id;
    rx_opts.id            = handler->id;

    if (pthread_mutex_init(&handler->tx_config_mutex, NULL)) {
      perror("Mutex init");
    }
    if (pthread_mutex_init(&handler->rx_config_mutex, NULL)) {
      perror("Mutex init");
    }
    if (pthread_mutex_init(&handler->decim_mutex, NULL)) {
      perror("Mutex init");
    }
    if (pthread_mutex_init(&handler->rx_gain_mutex, NULL)) {
      perror("Mutex init");
    }

    // parse args
    if (args && strlen(args)) {
      // base_srate
      parse_uint32(args, "base_srate", -1, &handler->base_srate);

      // id
      parse_string(args, "id", -1, handler->id);

      // protocol
      parse_string(args, "protocol", -1, handler->proto);
      if(!strcmp(handler->proto, "tcp")) {
        rx_opts.proto = NET_TCP;
        tx_opts.proto = NET_TCP;
      }
      else if(!strcmp(handler->proto, "udp")) {
        rx_opts.proto = NET_UDP;
        tx_opts.proto = NET_UDP;
      }
      else {
        fprintf(stderr, "[net] Invalid protocol: only 'tcp' and 'udp' are supported.\n");
        goto clean_exit;
      }

      // rx_format
      rx_opts.sample_format = NET_TYPE_FC32;

      // tx_format
      tx_opts.sample_format = NET_TYPE_FC32;
    
    } else {
      fprintf(stderr,
              "[net] Error: No device 'args' option has been set. Please make sure to set this option to be able to "
              "use the NET no-RF module\n");
      goto clean_exit;
    }

    update_rates(handler, 1.92e6);

    for (int i = 0; i < handler->nof_channels; i++) {
      // local_addr
      char local_addr[RF_PARAM_LEN] = {};
      parse_string(args, "local_addr", i, local_addr);

      // rx_freq
      double rx_freq = 0.0f;
      parse_double(args, "rx_freq", i, &rx_freq);
      rx_opts.frequency_mhz = (uint32_t)(rx_freq / 1e6);

      // rx_offset
      parse_int32(args, "rx_offset", i, &rx_opts.sample_offset);

      // peer_addr
      char peer_addr[RF_PARAM_LEN] = {};
      parse_string(args, "peer_addr", i, peer_addr);

      // tx_freq
      double tx_freq = 0.0f;
      parse_double(args, "tx_freq", i, &tx_freq);
      tx_opts.frequency_mhz = (uint32_t)(tx_freq / 1e6);

      // tx_offset
      parse_int32(args, "tx_offset", i, &tx_opts.sample_offset);

      // fail_on_disconnect
      char tmp[RF_PARAM_LEN] = {};
      parse_string(args, "fail_on_disconnect", i, tmp);
      if (strncmp(tmp, "true", RF_PARAM_LEN) == 0 || strncmp(tmp, "yes", RF_PARAM_LEN) == 0) {
        rx_opts.fail_on_disconnect = true;
      }

      // trx_timeout_ms
      rx_opts.trx_timeout_ms = NET_TIMEOUT_MS; /* 2 Seconds (2000 ms) */
      parse_uint32(args, "trx_timeout_ms", i, &rx_opts.trx_timeout_ms);

      // log_trx_timeout
      char tmp2[RF_PARAM_LEN] = {};
      parse_string(args, "log_trx_timeout", i, tmp);
      if (strncmp(tmp2, "true", RF_PARAM_LEN) == 0 || strncmp(tmp2, "yes", RF_PARAM_LEN) == 0) {
        rx_opts.log_trx_timeout = true;
      }

      /* Open ports */
      if(!strcmp(handler->id, "enb")) { /* eNB */
        printf("Initializing eNB...\n");
        // initialize eNB receiver
        if (strlen(local_addr) != 0) {
          if (rf_net_rx_open(&handler->receiver[i], rx_opts, local_addr) != SRSRAN_SUCCESS) {
            fprintf(stderr, "[net] Error: opening receiver\n");
            goto clean_exit;
          }
        } else {
          fprintf(stdout, "[net] %s Local address not specified. Disabling receiver.\n", handler->id);
        }
        printf("eNB receiver ready.\n");

        // initialize eNB transmitter
        if (strlen(peer_addr) != 0) {
          if (rf_net_tx_open(&handler->transmitter[i], tx_opts, peer_addr) != SRSRAN_SUCCESS) {
            fprintf(stderr, "[net] Error: opening transmitter\n");
            goto clean_exit;
          }
        } else {
          fprintf(stdout, "[net] %s Tx port not specified. Disabling transmitter.\n", handler->id);
          handler->tx_off = true;
        }
        printf("eNB transmitter ready.\n");
      }
      else { /* UE */
        printf("Initializing UE...\n");

        // initialize UE transmitter
        if (strlen(peer_addr) != 0) {
          if (rf_net_tx_open(&handler->transmitter[i], tx_opts, peer_addr) != SRSRAN_SUCCESS) {
            fprintf(stderr, "[net] Error: opening transmitter\n");
            goto clean_exit;
          }
        } else {
          fprintf(stdout, "[net] %s Tx port not specified. Disabling transmitter.\n", handler->id);
          handler->tx_off = true;
        }
        printf("UE transmitter ready.\n");

        // initialize UE receiver
        if (strlen(local_addr) != 0) {
          if (rf_net_rx_open(&handler->receiver[i], rx_opts, local_addr) != SRSRAN_SUCCESS) {
            fprintf(stderr, "[net] Error: opening receiver\n");
            goto clean_exit;
          }
        } else {
          fprintf(stdout, "[net] %s Rx port not specified. Disabling receiver.\n", handler->id);
        }
        printf("UE receiver ready.\n");
      }

      if (!handler->transmitter[i].running && !handler->receiver[i].running) {
        fprintf(stderr, "[net] Error: Neither Tx port nor Rx port specified.\n");
        goto clean_exit;
      }
    }

    // Create decimation and overflow buffer
    for (uint32_t i = 0; i < handler->nof_channels; i++) {
      handler->buffer_decimation[i] = srsran_vec_malloc(NET_MAX_BUFFER_SIZE);
      if (!handler->buffer_decimation[i]) {
        fprintf(stderr, "Error: allocating decimation buffer\n");
        goto clean_exit;
      }
    }

    handler->buffer_tx = srsran_vec_malloc(NET_MAX_BUFFER_SIZE);
    if (!handler->buffer_tx) {
      fprintf(stderr, "Error: allocating tx buffer\n");
      goto clean_exit;
    }

    ret = SRSRAN_SUCCESS;

  clean_exit:
    if (ret) {
      rf_net_close(handler);
    }
  }
  return ret;
}

int rf_net_close(void* h)
{
  rf_net_handler_t* handler = (rf_net_handler_t*)h;

  rf_net_info(handler->id, "Closing ...\n");

  for (int i = 0; i < handler->nof_channels; i++) {
    rf_net_tx_close(&handler->transmitter[i]);
    rf_net_rx_close(&handler->receiver[i]);
  }

  for (uint32_t i = 0; i < handler->nof_channels; i++) {
    if (handler->buffer_decimation[i]) {
      free(handler->buffer_decimation[i]);
    }
  }

  if (handler->buffer_tx) {
    free(handler->buffer_tx);
  }

  pthread_mutex_destroy(&handler->tx_config_mutex);
  pthread_mutex_destroy(&handler->rx_config_mutex);
  pthread_mutex_destroy(&handler->decim_mutex);
  pthread_mutex_destroy(&handler->rx_gain_mutex);

  // Free all
  free(handler);

  return SRSRAN_SUCCESS;
}

void update_rates(rf_net_handler_t* handler, double srate)
{
  pthread_mutex_lock(&handler->decim_mutex);
  if (handler) {
    // Decimation must be full integer
    if (((uint64_t)handler->base_srate % (uint64_t)srate) == 0) {
      handler->srate        = (uint32_t)srate;
      handler->decim_factor = handler->base_srate / handler->srate;
    } else {
      fprintf(stderr,
              "Error: couldn't update sample rate. %.2f is not divisible by %.2f\n",
              srate / 1e6,
              handler->base_srate / 1e6);
    }
    printf("Current sample rate is %.2f MHz with a base rate of %.2f MHz (x%d decimation)\n",
           handler->srate / 1e6,
           handler->base_srate / 1e6,
           handler->decim_factor);
  }
  pthread_mutex_unlock(&handler->decim_mutex);
}

double rf_net_set_rx_srate(void* h, double srate)
{
  double ret = 0.0;
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    update_rates(handler, srate);
    ret = handler->srate;
  }
  return ret;
}

double rf_net_set_tx_srate(void* h, double srate)
{
  double ret = 0.0;
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    update_rates(handler, srate);
    ret = srate;
  }
  return ret;
}

int rf_net_set_rx_gain(void* h, double gain)
{
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    pthread_mutex_lock(&handler->rx_gain_mutex);
    handler->rx_gain = gain;
    pthread_mutex_unlock(&handler->rx_gain_mutex);
  }
  return SRSRAN_SUCCESS;
}

int rf_net_set_rx_gain_ch(void* h, uint32_t ch, double gain)
{
  return rf_net_set_rx_gain(h, gain);
}

int rf_net_set_tx_gain(void* h, double gain)
{
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    pthread_mutex_lock(&handler->tx_config_mutex);
    handler->tx_gain = gain;
    pthread_mutex_unlock(&handler->tx_config_mutex);
  }
  return SRSRAN_SUCCESS;
}

int rf_net_set_tx_gain_ch(void* h, uint32_t ch, double gain)
{
  return rf_net_set_tx_gain(h, gain);
}

double rf_net_get_rx_gain(void* h)
{
  double ret = 0.0;
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    pthread_mutex_lock(&handler->rx_gain_mutex);
    ret = handler->rx_gain;
    pthread_mutex_unlock(&handler->rx_gain_mutex);
  }
  return ret;
}

double rf_net_get_tx_gain(void* h)
{
  float ret = NAN;
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    pthread_mutex_lock(&handler->tx_config_mutex);
    ret = handler->tx_gain;
    pthread_mutex_unlock(&handler->tx_config_mutex);
  }
  return ret;
}

srsran_rf_info_t* rf_net_get_info(void* h)
{
  srsran_rf_info_t* info = NULL;
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    info                      = &handler->info;
  }
  return info;
}

double rf_net_set_rx_freq(void* h, uint32_t ch, double freq)
{
  double ret = NAN;
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    pthread_mutex_lock(&handler->rx_config_mutex);
    if (ch < handler->nof_channels && isnormal(freq) && freq > 0.0) {
      handler->rx_freq_mhz[ch] = (uint32_t)(freq / 1e6);
      ret                      = freq;
    }
    pthread_mutex_unlock(&handler->rx_config_mutex);
  }
  return ret;
}

double rf_net_set_tx_freq(void* h, uint32_t ch, double freq)
{
  double ret = NAN;
  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;
    pthread_mutex_lock(&handler->tx_config_mutex);
    if (ch < handler->nof_channels && isnormal(freq) && freq > 0.0) {
      handler->tx_freq_mhz[ch] = (uint32_t)(freq / 1e6);
      ret                      = freq;
    }
    pthread_mutex_unlock(&handler->tx_config_mutex);
  }
  return ret;
}

void rf_net_get_time(void* h, time_t* secs, double* frac_secs)
{
  if (h) {
    if (secs) {
      *secs = 0;
    }

    if (frac_secs) {
      *frac_secs = 0;
    }
  }
}

int rf_net_recv_with_time(void* h, void* data, uint32_t nsamples, bool blocking, time_t* secs, double* frac_secs)
{
  return rf_net_recv_with_time_multi(h, &data, nsamples, blocking, secs, frac_secs);
}

int rf_net_recv_with_time_multi(void* h, void** data, uint32_t nsamples, bool blocking, time_t* secs, double* frac_secs)
{
  int ret = SRSRAN_ERROR;

  if (h) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;

    // Map ports to data buffers according to the selected frequencies
    pthread_mutex_lock(&handler->rx_config_mutex);
    bool  mapped[SRSRAN_MAX_CHANNELS]  = {}; // Mapped mask, set to true when the physical channel is used
    cf_t* buffers[SRSRAN_MAX_CHANNELS] = {}; // Buffer pointers, NULL if unmatched

    // For each logical channel...
    for (uint32_t logical = 0; logical < handler->nof_channels; logical++) {
      bool unmatched = true;

      // For each physical channel...
      for (uint32_t physical = 0; physical < handler->nof_channels; physical++) {
        // Consider a match if the physical channel is NOT mapped and the frequency match
        if (!mapped[physical] && rf_net_rx_match_freq(&handler->receiver[physical], handler->rx_freq_mhz[logical])) {
          // Not mapped and matched frequency with receiver
          buffers[physical] = (cf_t*)data[logical];
          mapped[physical]  = true;
          unmatched         = false;
          break;
        }
      }

      // If no matching frequency found; set data to zeros
      if (unmatched) {
        srsran_vec_zero(data[logical], nsamples);
      }
    }
    pthread_mutex_unlock(&handler->rx_config_mutex);

    // Protect the access to decim_factor since is a shared variable
    pthread_mutex_lock(&handler->decim_mutex);
    uint32_t decim_factor = handler->decim_factor;
    decim_factor = 1;
    pthread_mutex_unlock(&handler->decim_mutex);

    uint32_t nbytes            = NSAMPLES2NBYTES(nsamples * decim_factor);
    uint32_t nsamples_baserate = nsamples * decim_factor;

    rf_net_info(handler->id, "Rx %d samples (%d B)\n", nsamples, nbytes);

    // set timestamp for this reception
    if (secs != NULL && frac_secs != NULL) {
      srsran_timestamp_t ts = {};
      srsran_timestamp_init_uint64(&ts, handler->next_rx_ts, handler->base_srate);
      *secs      = ts.full_secs;
      *frac_secs = ts.frac_secs;
    }

    // return if receiver is turned off
    if (!rf_net_rx_is_running(&handler->receiver[0])) {
      update_ts(handler, &handler->next_rx_ts, nsamples_baserate, "rx");
      return nsamples;
    }

    // Check available buffer size
    if (nbytes > NET_MAX_BUFFER_SIZE) {
      fprintf(stderr,
              "[net] Error: Trying to receive %d B but buffer is only %zu B at channel %d.\n",
              nbytes,
              NET_MAX_BUFFER_SIZE,
              0);
      goto clean_exit;
    }

    // receive samples
    srsran_timestamp_t ts_tx = {}, ts_rx = {};
    srsran_timestamp_init_uint64(&ts_tx, rf_net_tx_get_nsamples(&handler->transmitter[0]), handler->base_srate);
    srsran_timestamp_init_uint64(&ts_rx, handler->next_rx_ts, handler->base_srate);
    rf_net_info(handler->id, " - next rx time: %d + %.3f\n", ts_rx.full_secs, ts_rx.frac_secs);
    rf_net_info(handler->id, " - next tx time: %d + %.3f\n", ts_tx.full_secs, ts_tx.frac_secs);

    // Leave time for the Tx to transmit
    usleep((1000000UL * nsamples_baserate) / handler->base_srate);

    // check for tx gap if we're also transmitting on this radio
    for (int i = 0; i < handler->nof_channels; i++) {
      if (rf_net_tx_is_running(&handler->transmitter[i])) {
        rf_net_tx_align(&handler->transmitter[i], handler->next_rx_ts + nsamples_baserate);
      }
    }

    // copy from rx buffer as many samples as requested into provided buffer
    bool    completed                  = false;
    int32_t count[SRSRAN_MAX_CHANNELS] = {};
    while (!completed) {
      uint32_t completed_count = 0;

      // Iterate channels
      for (uint32_t i = 0; i < handler->nof_channels; i++) {
        // Select buffer based on decim_factor
        cf_t* ptr = (decim_factor != 1 || buffers[i] == NULL) ? handler->buffer_decimation[i] : buffers[i];

        // Completed condition
        if (count[i] < nsamples_baserate && rf_net_rx_is_running(&handler->receiver[i])) {
          // Keep receiving
          int32_t n = rf_net_rx_baseband(&handler->receiver[i], &ptr[count[i]], nsamples_baserate);
          if (n > SRSRAN_SUCCESS) {
            // No error
            count[i] += n;
          } else if (n == SRSRAN_ERROR_TIMEOUT) {
            if (handler->receiver[i].log_trx_timeout) {
              fprintf(stderr, "Error: timeout receiving samples after %dms\n", handler->receiver[i].trx_timeout_ms);
            }
            // Other end disconnected, either keep going, or fail
            if (handler->receiver[i].fail_on_disconnect) {
              goto clean_exit;
            }
          } else if (n < SRSRAN_SUCCESS) {
            // Other error, exit
            fprintf(stderr, "Error: receiving data.\n");
            goto clean_exit;
          }
        } else {
          // Completed, count it
          completed_count++;
        }
      }

      // Check if all channels are completed
      completed = (completed_count == handler->nof_channels);
    }
    rf_net_info(handler->id,
                " - read %d samples. %d samples available\n",
                NBYTES2NSAMPLES(nbytes),
                NBYTES2NSAMPLES(srsran_ringbuffer_status(&handler->receiver[0].ringbuffer)));

    // decimate if needed
    if (decim_factor != 1) {
      for (uint32_t c = 0; c < handler->nof_channels; c++) {
        // skip if buffer is not available
        if (buffers[c]) {
          cf_t* dst = buffers[c];
          cf_t* ptr = handler->buffer_decimation[c];

          for (uint32_t i = 0, n = 0; i < nsamples; i++) {
            // Averaging decimation
            cf_t avg = 0.0f;
            for (int j = 0; j < decim_factor; j++, n++) {
              avg += ptr[n];
            }
            dst[i] = avg / decim_factor; // divide by decim_factor later via scale
          }

          rf_net_info(handler->id,
                      "  - re-adjust bytes due to %dx decimation %d --> %d samples)\n",
                      decim_factor,
                      nsamples_baserate,
                      nsamples);
        }
      }
    }


    // Set gain
    pthread_mutex_lock(&handler->rx_gain_mutex);
    float scale = srsran_convert_dB_to_amplitude(handler->rx_gain);
    pthread_mutex_unlock(&handler->rx_gain_mutex);
    
    for (uint32_t c = 0; c < handler->nof_channels; c++) {
      if (buffers[c]) {
        srsran_vec_sc_prod_cfc(buffers[c], scale, buffers[c], nsamples);
      }
    }

    // update rx time
    update_ts(handler, &handler->next_rx_ts, nsamples_baserate, "rx");
  }

  ret = nsamples;

clean_exit:

  return ret;
}

int rf_net_send_timed(void*  h,
                      void*  data,
                      int    nsamples,
                      time_t secs,
                      double frac_secs,
                      bool   has_time_spec,
                      bool   blocking,
                      bool   is_start_of_burst,
                      bool   is_end_of_burst)
{
  void* _data[4] = {data, NULL, NULL, NULL};

  return rf_net_send_timed_multi(
      h, _data, nsamples, secs, frac_secs, has_time_spec, blocking, is_start_of_burst, is_end_of_burst);
}

// TODO: Implement Tx upsampling
int rf_net_send_timed_multi(void*  h,
                            void*  data[4],
                            int    nsamples,
                            time_t secs,
                            double frac_secs,
                            bool   has_time_spec,
                            bool   blocking,
                            bool   is_start_of_burst,
                            bool   is_end_of_burst)
{
  int ret = SRSRAN_ERROR;

  if (h && data && nsamples > 0) {
    rf_net_handler_t* handler = (rf_net_handler_t*)h;

    // Map ports to data buffers according to the selected frequencies
    pthread_mutex_lock(&handler->tx_config_mutex);
    bool  mapped[SRSRAN_MAX_CHANNELS]  = {}; // Mapped mask, set to true when the physical channel is used
    cf_t* buffers[SRSRAN_MAX_CHANNELS] = {}; // Buffer pointers, NULL if unmatched or zero transmission

    // For each logical channel...
    for (uint32_t logical = 0; logical < handler->nof_channels; logical++) {
      // For each physical channel...
      for (uint32_t physical = 0; physical < handler->nof_channels; physical++) {
        // Consider a match if the physical channel is NOT mapped and the frequency match
        if (!mapped[physical] && rf_net_tx_match_freq(&handler->transmitter[physical], handler->tx_freq_mhz[logical])) {
          // Not mapped and matched frequency with receiver
          buffers[physical] = (cf_t*)data[logical];
          mapped[physical]  = true;
          break;
        }
      }
    }

    // Load transmission gain
    float tx_gain = srsran_convert_dB_to_amplitude(handler->tx_gain);

    pthread_mutex_unlock(&handler->tx_config_mutex);

    // If the Tx gain is NAN, INF or 0.0, use 1.0
    if (!isnormal(tx_gain)) {
      tx_gain = 1.0f;
    }

    // Protect the access to decim_factor since is a shared variable
    pthread_mutex_lock(&handler->decim_mutex);
    uint32_t decim_factor = handler->decim_factor;
    decim_factor = 1;
    pthread_mutex_unlock(&handler->decim_mutex);

    uint32_t nbytes            = NSAMPLES2NBYTES(nsamples);
    uint32_t nsamples_baseband = nsamples * decim_factor;
    uint32_t nbytes_baseband   = NSAMPLES2NBYTES(nsamples_baseband);
    if (nbytes_baseband > NET_MAX_BUFFER_SIZE) {
      fprintf(stderr, "Error: trying to transmit too many samples (%d > %zu).\n", nbytes, NET_MAX_BUFFER_SIZE);
      goto clean_exit;
    }

    rf_net_info(handler->id, "Tx %d samples (%d B)\n", nsamples, nbytes);

    // return if transmitter is switched off
    if (handler->tx_off) {
      return SRSRAN_SUCCESS;
    }

    // check if this is a tx in the future
    if (has_time_spec) {
      rf_net_info(handler->id, "    - tx time: %d + %.3f\n", secs, frac_secs);

      srsran_timestamp_t ts = {};
      srsran_timestamp_init(&ts, secs, frac_secs);
      uint64_t tx_ts              = srsran_timestamp_uint64(&ts, handler->base_srate);
      int      num_tx_gap_samples = 0;

      for (int i = 0; i < handler->nof_channels; i++) {
        if (rf_net_tx_is_running(&handler->transmitter[i])) {
          num_tx_gap_samples = rf_net_tx_align(&handler->transmitter[i], tx_ts);
        }
      }

      if (num_tx_gap_samples < 0) {
        fprintf(stderr,
                "[net] Error: tx time is %.3f ms in the past (%" PRIu64 " < %" PRIu64 ")\n",
                -1000.0 * num_tx_gap_samples / handler->base_srate,
                tx_ts,
                (uint64_t)rf_net_tx_get_nsamples(&handler->transmitter[0]));
        goto clean_exit;
      }
    }

    // Send base-band samples
    for (int i = 0; i < handler->nof_channels; i++) {
      if (buffers[i] != NULL) {
        // Select buffer pointer depending on interpolation
        cf_t* buf = (decim_factor != 1) ? handler->buffer_tx : buffers[i];

        // Interpolate if required
        if (decim_factor != 1) {
          rf_net_info(handler->id,
                      "  - re-adjust bytes due to %dx interpolation %d --> %d samples)\n",
                      decim_factor,
                      nsamples,
                      nsamples_baseband);

          int   n   = 0;
          cf_t* src = buffers[i];
          for (int k = 0; k < nsamples; k++) {
            // perform zero order hold
            for (int j = 0; j < decim_factor; j++, n++) {
              buf[n] = src[k];
            }
          }

          if (nsamples_baseband != n) {
            fprintf(stderr,
                    "Number of tx samples (%d) does not match with number of interpolated samples (%d)\n",
                    nsamples_baseband,
                    n);
            goto clean_exit;
          }
        }

        // Scale according to current gain
        srsran_vec_sc_prod_cfc(buf, tx_gain, buf, nsamples_baseband);

        // Finally, transmit baseband
        int n = rf_net_tx_baseband(&handler->transmitter[i], buf, nsamples_baseband);
        if (n == SRSRAN_ERROR) {
          goto clean_exit;
        }
      } else {
        int n = rf_net_tx_zeros(&handler->transmitter[i], nsamples_baseband);
        if (n == SRSRAN_ERROR) {
          goto clean_exit;
        }
      }
    }
  }

  ret = SRSRAN_SUCCESS;

clean_exit:

  return ret;
}

rf_dev_t srsran_rf_dev_net = {"net",
                              rf_net_devname,
                              rf_net_start_rx_stream,
                              rf_net_stop_rx_stream,
                              rf_net_flush_buffer,
                              rf_net_has_rssi,
                              rf_net_get_rssi,
                              rf_net_suppress_stdout,
                              rf_net_register_error_handler,
                              rf_net_open,
                              .srsran_rf_open_multi = rf_net_open_multi,
                              rf_net_close,
                              rf_net_set_rx_srate,
                              rf_net_set_rx_gain,
                              rf_net_set_rx_gain_ch,
                              rf_net_set_tx_gain,
                              rf_net_set_tx_gain_ch,
                              rf_net_get_rx_gain,
                              rf_net_get_tx_gain,
                              rf_net_get_info,
                              rf_net_set_rx_freq,
                              rf_net_set_tx_srate,
                              rf_net_set_tx_freq,
                              rf_net_get_time,
                              NULL,
                              rf_net_recv_with_time,
                              rf_net_recv_with_time_multi,
                              rf_net_send_timed,
                              .srsran_rf_send_timed_multi = rf_net_send_timed_multi};

#ifdef ENABLE_RF_PLUGINS
int register_plugin(rf_dev_t** rf_api)
{
  if (rf_api == NULL) {
    return SRSRAN_ERROR;
  }
  *rf_api = &srsran_rf_dev_net;
  return SRSRAN_SUCCESS;
}
#endif /* ENABLE_RF_PLUGINS */
