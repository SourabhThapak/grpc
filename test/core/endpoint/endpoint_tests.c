/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "test/core/endpoint/endpoint_tests.h"

#include <sys/types.h>

#include <grpc/support/alloc.h>
#include <grpc/support/slice.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>

/*
   General test notes:

   All tests which write data into an endpoint write i%256 into byte i, which
   is verified by readers.

   In general there are a few interesting things to vary which may lead to
   exercising different codepaths in an implementation:
   1. Total amount of data written to the endpoint
   2. Size of slice allocations
   3. Amount of data we read from or write to the endpoint at once

   The tests here tend to parameterize these where applicable.

*/

ssize_t count_and_unref_slices(gpr_slice *slices, size_t nslices,
                               int *current_data) {
  ssize_t num_bytes = 0;
  int i;
  int j;
  unsigned char *buf;
  for (i = 0; i < nslices; ++i) {
    buf = GPR_SLICE_START_PTR(slices[i]);
    for (j = 0; j < GPR_SLICE_LENGTH(slices[i]); ++j) {
      GPR_ASSERT(buf[j] == *current_data);
      *current_data = (*current_data + 1) % 256;
    }
    num_bytes += GPR_SLICE_LENGTH(slices[i]);
    gpr_slice_unref(slices[i]);
  }
  return num_bytes;
}

static grpc_endpoint_test_fixture begin_test(grpc_endpoint_test_config config,
                                             const char *test_name,
                                             ssize_t slice_size) {
  gpr_log(GPR_INFO, "%s/%s", test_name, config.name);
  return config.create_fixture(slice_size);
}

static void end_test(grpc_endpoint_test_config config) { config.clean_up(); }

static gpr_slice *allocate_blocks(ssize_t num_bytes, ssize_t slice_size,
                                  size_t *num_blocks, int *current_data) {
  ssize_t nslices = num_bytes / slice_size + (num_bytes % slice_size ? 1 : 0);
  gpr_slice *slices = malloc(sizeof(gpr_slice) * nslices);
  ssize_t num_bytes_left = num_bytes;
  int i;
  int j;
  unsigned char *buf;
  *num_blocks = nslices;

  for (i = 0; i < nslices; ++i) {
    slices[i] = gpr_slice_malloc(slice_size > num_bytes_left ? num_bytes_left
                                                             : slice_size);
    num_bytes_left -= GPR_SLICE_LENGTH(slices[i]);
    buf = GPR_SLICE_START_PTR(slices[i]);
    for (j = 0; j < GPR_SLICE_LENGTH(slices[i]); ++j) {
      buf[j] = *current_data;
      *current_data = (*current_data + 1) % 256;
    }
  }
  GPR_ASSERT(num_bytes_left == 0);
  return slices;
}

struct read_and_write_test_state {
  grpc_endpoint *read_ep;
  grpc_endpoint *write_ep;
  gpr_mu mu;
  gpr_cv cv;
  ssize_t target_bytes;
  ssize_t bytes_read;
  ssize_t current_write_size;
  ssize_t bytes_written;
  int current_read_data;
  int current_write_data;
  int read_done;
  int write_done;
};

static void read_and_write_test_read_handler(void *data, gpr_slice *slices,
                                             size_t nslices,
                                             grpc_endpoint_cb_status error) {
  struct read_and_write_test_state *state = data;
  GPR_ASSERT(error != GRPC_ENDPOINT_CB_ERROR);
  if (error == GRPC_ENDPOINT_CB_SHUTDOWN) {
    gpr_log(GPR_INFO, "Read handler shutdown");
    gpr_mu_lock(&state->mu);
    state->read_done = 1;
    gpr_cv_signal(&state->cv);
    gpr_mu_unlock(&state->mu);
    return;
  }

  state->bytes_read +=
      count_and_unref_slices(slices, nslices, &state->current_read_data);
  if (state->bytes_read == state->target_bytes) {
    gpr_log(GPR_INFO, "Read handler done");
    gpr_mu_lock(&state->mu);
    state->read_done = 1;
    gpr_cv_signal(&state->cv);
    gpr_mu_unlock(&state->mu);
  } else {
    grpc_endpoint_notify_on_read(
        state->read_ep, read_and_write_test_read_handler, data, gpr_inf_future);
  }
}

static void read_and_write_test_write_handler(void *data,
                                              grpc_endpoint_cb_status error) {
  struct read_and_write_test_state *state = data;
  gpr_slice *slices = NULL;
  size_t nslices;
  grpc_endpoint_write_status write_status;

  GPR_ASSERT(error != GRPC_ENDPOINT_CB_ERROR);

  if (error == GRPC_ENDPOINT_CB_SHUTDOWN) {
    gpr_log(GPR_INFO, "Write handler shutdown");
    gpr_mu_lock(&state->mu);
    state->write_done = 1;
    gpr_cv_signal(&state->cv);
    gpr_mu_unlock(&state->mu);
    return;
  }

  for (;;) {
    /* Need to do inline writes until they don't succeed synchronously or we
       finish writing */
    state->bytes_written += state->current_write_size;
    if (state->target_bytes - state->bytes_written <
        state->current_write_size) {
      state->current_write_size = state->target_bytes - state->bytes_written;
    }
    if (state->current_write_size == 0) {
      break;
    }

    slices = allocate_blocks(state->current_write_size, 8192, &nslices,
                             &state->current_write_data);
    write_status = grpc_endpoint_write(state->write_ep, slices, nslices,
                                       read_and_write_test_write_handler, state,
                                       gpr_inf_future);
    GPR_ASSERT(write_status != GRPC_ENDPOINT_WRITE_ERROR);
    free(slices);
    if (write_status == GRPC_ENDPOINT_WRITE_PENDING) {
      return;
    }
  }
  GPR_ASSERT(state->bytes_written == state->target_bytes);

  gpr_log(GPR_INFO, "Write handler done");
  gpr_mu_lock(&state->mu);
  state->write_done = 1;
  gpr_cv_signal(&state->cv);
  gpr_mu_unlock(&state->mu);
}

/* Do both reading and writing using the grpc_endpoint API.

   This also includes a test of the shutdown behavior.
 */
static void read_and_write_test(grpc_endpoint_test_config config,
                                ssize_t num_bytes, ssize_t write_size,
                                ssize_t slice_size, int shutdown) {
  struct read_and_write_test_state state;
  gpr_timespec rel_deadline = {20, 0};
  gpr_timespec deadline = gpr_time_add(gpr_now(), rel_deadline);
  grpc_endpoint_test_fixture f = begin_test(config, __FUNCTION__, slice_size);

  if (shutdown) {
    gpr_log(GPR_INFO, "Start read and write shutdown test");
  } else {
    gpr_log(GPR_INFO, "Start read and write test with %d bytes, slice size %d",
            num_bytes, slice_size);
  }

  gpr_mu_init(&state.mu);
  gpr_cv_init(&state.cv);

  state.read_ep = f.client_ep;
  state.write_ep = f.server_ep;
  state.target_bytes = num_bytes;
  state.bytes_read = 0;
  state.current_write_size = write_size;
  state.bytes_written = 0;
  state.read_done = 0;
  state.write_done = 0;
  state.current_read_data = 0;
  state.current_write_data = 0;

  /* Get started by pretending an initial write completed */
  state.bytes_written -= state.current_write_size;
  read_and_write_test_write_handler(&state, GRPC_ENDPOINT_CB_OK);

  grpc_endpoint_notify_on_read(state.read_ep, read_and_write_test_read_handler,
                               &state, gpr_inf_future);

  if (shutdown) {
    grpc_endpoint_shutdown(state.read_ep);
    grpc_endpoint_shutdown(state.write_ep);
  }

  gpr_mu_lock(&state.mu);
  while (!state.read_done || !state.write_done) {
    GPR_ASSERT(gpr_cv_wait(&state.cv, &state.mu, deadline) == 0);
  }
  gpr_mu_unlock(&state.mu);

  grpc_endpoint_destroy(state.read_ep);
  grpc_endpoint_destroy(state.write_ep);
  gpr_mu_destroy(&state.mu);
  gpr_cv_destroy(&state.cv);
  end_test(config);
}

struct timeout_test_state {
  gpr_event io_done;
};

static void read_timeout_test_read_handler(void *data, gpr_slice *slices,
                                           size_t nslices,
                                           grpc_endpoint_cb_status error) {
  struct timeout_test_state *state = data;
  GPR_ASSERT(error == GRPC_ENDPOINT_CB_TIMED_OUT);
  gpr_event_set(&state->io_done, (void *)1);
}

static void read_timeout_test(grpc_endpoint_test_config config,
                              ssize_t slice_size) {
  gpr_timespec timeout = gpr_time_from_micros(10000);
  gpr_timespec read_deadline = gpr_time_add(gpr_now(), timeout);
  gpr_timespec test_deadline =
      gpr_time_add(gpr_now(), gpr_time_from_micros(2000000));
  struct timeout_test_state state;
  grpc_endpoint_test_fixture f = begin_test(config, __FUNCTION__, slice_size);

  gpr_event_init(&state.io_done);

  grpc_endpoint_notify_on_read(f.client_ep, read_timeout_test_read_handler,
                               &state, read_deadline);
  GPR_ASSERT(gpr_event_wait(&state.io_done, test_deadline));
  grpc_endpoint_destroy(f.client_ep);
  grpc_endpoint_destroy(f.server_ep);
  end_test(config);
}

static void write_timeout_test_write_handler(void *data,
                                             grpc_endpoint_cb_status error) {
  struct timeout_test_state *state = data;
  GPR_ASSERT(error == GRPC_ENDPOINT_CB_TIMED_OUT);
  gpr_event_set(&state->io_done, (void *)1);
}

static void write_timeout_test(grpc_endpoint_test_config config,
                               ssize_t slice_size) {
  gpr_timespec timeout = gpr_time_from_micros(10000);
  gpr_timespec write_deadline = gpr_time_add(gpr_now(), timeout);
  gpr_timespec test_deadline =
      gpr_time_add(gpr_now(), gpr_time_from_micros(2000000));
  struct timeout_test_state state;
  int current_data = 1;
  gpr_slice *slices;
  size_t nblocks;
  size_t size;
  grpc_endpoint_test_fixture f = begin_test(config, __FUNCTION__, slice_size);

  gpr_event_init(&state.io_done);

  /* TODO(klempner): Factor this out with the equivalent code in tcp_test.c */
  for (size = 1;; size *= 2) {
    slices = allocate_blocks(size, 1, &nblocks, &current_data);
    switch (grpc_endpoint_write(f.client_ep, slices, nblocks,
                                write_timeout_test_write_handler, &state,
                                write_deadline)) {
      case GRPC_ENDPOINT_WRITE_DONE:
        break;
      case GRPC_ENDPOINT_WRITE_ERROR:
        gpr_log(GPR_ERROR, "error writing");
        abort();
      case GRPC_ENDPOINT_WRITE_PENDING:
        GPR_ASSERT(gpr_event_wait(&state.io_done, test_deadline));
        gpr_free(slices);
        goto exit;
    }
    gpr_free(slices);
  }
exit:
  grpc_endpoint_destroy(f.client_ep);
  grpc_endpoint_destroy(f.server_ep);
  end_test(config);
}

typedef struct {
  gpr_event ev;
  grpc_endpoint *ep;
} shutdown_during_write_test_state;

static void shutdown_during_write_test_read_handler(
    void *user_data, gpr_slice *slices, size_t nslices,
    grpc_endpoint_cb_status error) {
  size_t i;
  shutdown_during_write_test_state *st = user_data;

  for (i = 0; i < nslices; i++) {
    gpr_slice_unref(slices[i]);
  }

  if (error != GRPC_ENDPOINT_CB_OK) {
    grpc_endpoint_destroy(st->ep);
    gpr_event_set(&st->ev, (void *)(gpr_intptr)error);
  } else {
    grpc_endpoint_notify_on_read(st->ep,
                                 shutdown_during_write_test_read_handler,
                                 user_data, gpr_inf_future);
  }
}

static void shutdown_during_write_test_write_handler(
    void *user_data, grpc_endpoint_cb_status error) {
  shutdown_during_write_test_state *st = user_data;
  gpr_log(GPR_INFO, "shutdown_during_write_test_write_handler: error = %d",
          error);
  grpc_endpoint_destroy(st->ep);
  gpr_event_set(&st->ev, (void *)(gpr_intptr)error);
}

static void shutdown_during_write_test(grpc_endpoint_test_config config,
                                       ssize_t slice_size) {
  /* test that shutdown with a pending write creates no leaks */
  gpr_timespec deadline;
  size_t size;
  size_t nblocks;
  int current_data = 1;
  shutdown_during_write_test_state read_st;
  shutdown_during_write_test_state write_st;
  gpr_slice *slices;
  grpc_endpoint_test_fixture f = begin_test(config, __FUNCTION__, slice_size);

  gpr_log(GPR_INFO, "testing shutdown during a write");

  read_st.ep = f.client_ep;
  write_st.ep = f.server_ep;
  gpr_event_init(&read_st.ev);
  gpr_event_init(&write_st.ev);

#if 0
  read_st.ep = grpc_tcp_create(sv[1], &em);
  write_st.ep = grpc_tcp_create(sv[0], &em);
#endif

  grpc_endpoint_notify_on_read(read_st.ep,
                               shutdown_during_write_test_read_handler,
                               &read_st, gpr_inf_future);
  for (size = 1;; size *= 2) {
    slices = allocate_blocks(size, 1, &nblocks, &current_data);
    switch (grpc_endpoint_write(write_st.ep, slices, nblocks,
                                shutdown_during_write_test_write_handler,
                                &write_st, gpr_inf_future)) {
      case GRPC_ENDPOINT_WRITE_DONE:
        break;
      case GRPC_ENDPOINT_WRITE_ERROR:
        gpr_log(GPR_ERROR, "error writing");
        abort();
      case GRPC_ENDPOINT_WRITE_PENDING:
        grpc_endpoint_shutdown(write_st.ep);
        deadline =
            gpr_time_add(gpr_now(), gpr_time_from_micros(10 * GPR_US_PER_SEC));
        GPR_ASSERT(gpr_event_wait(&write_st.ev, deadline));
        GPR_ASSERT(gpr_event_wait(&read_st.ev, deadline));
        gpr_free(slices);
        end_test(config);
        return;
    }
    gpr_free(slices);
  }

  gpr_log(GPR_ERROR, "should never reach here");
  abort();
}

void grpc_endpoint_tests(grpc_endpoint_test_config config) {
  read_and_write_test(config, 10000000, 100000, 8192, 0);
  read_and_write_test(config, 1000000, 100000, 1, 0);
  read_and_write_test(config, 100000000, 100000, 1, 1);
  read_timeout_test(config, 1000);
  write_timeout_test(config, 1000);
  shutdown_during_write_test(config, 1000);
}