/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "test/core/security/oauth2_utils.h"

#include <string.h>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>

#include "src/core/lib/security/credentials/credentials.h"

typedef struct {
  gpr_mu *mu;
  grpc_polling_entity pops;
  bool is_done;
  char *token;

  grpc_credentials_mdelem_array md_array;
  grpc_closure closure;
} oauth2_request;

static void on_oauth2_response(grpc_exec_ctx *exec_ctx, void *arg,
                               grpc_error *error) {
  oauth2_request *request = (oauth2_request *)arg;
  char *token = NULL;
  grpc_slice token_slice;
  if (error != GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR, "Fetching token failed: %s", grpc_error_string(error));
  } else {
    GPR_ASSERT(request->md_array.size == 1);
    token_slice = GRPC_MDVALUE(request->md_array.md[0]);
    token = (char *)gpr_malloc(GRPC_SLICE_LENGTH(token_slice) + 1);
    memcpy(token, GRPC_SLICE_START_PTR(token_slice),
           GRPC_SLICE_LENGTH(token_slice));
    token[GRPC_SLICE_LENGTH(token_slice)] = '\0';
  }
  grpc_credentials_mdelem_array_destroy(exec_ctx, &request->md_array);
  gpr_mu_lock(request->mu);
  request->is_done = true;
  request->token = token;
  GRPC_LOG_IF_ERROR(
      "pollset_kick",
      grpc_pollset_kick(grpc_polling_entity_pollset(&request->pops), NULL));
  gpr_mu_unlock(request->mu);
}

static void do_nothing(grpc_exec_ctx *exec_ctx, void *unused,
                       grpc_error *error) {}

char *grpc_test_fetch_oauth2_token_with_credentials(
    grpc_call_credentials *creds) {
  oauth2_request request;
  memset(&request, 0, sizeof(request));
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_closure do_nothing_closure;
  grpc_auth_metadata_context null_ctx = {"", "", NULL, NULL};

  grpc_pollset *pollset = (grpc_pollset *)gpr_zalloc(grpc_pollset_size());
  grpc_pollset_init(pollset, &request.mu);
  request.pops = grpc_polling_entity_create_from_pollset(pollset);
  request.is_done = false;

  GRPC_CLOSURE_INIT(&do_nothing_closure, do_nothing, NULL,
                    grpc_schedule_on_exec_ctx);

  GRPC_CLOSURE_INIT(&request.closure, on_oauth2_response, &request,
                    grpc_schedule_on_exec_ctx);

  grpc_error *error = GRPC_ERROR_NONE;
  if (grpc_call_credentials_get_request_metadata(
          &exec_ctx, creds, &request.pops, null_ctx, &request.md_array,
          &request.closure, &error)) {
    // Synchronous result; invoke callback directly.
    on_oauth2_response(&exec_ctx, &request, error);
    GRPC_ERROR_UNREF(error);
  }
  grpc_exec_ctx_flush(&exec_ctx);

  gpr_mu_lock(request.mu);
  while (!request.is_done) {
    grpc_pollset_worker *worker = NULL;
    if (!GRPC_LOG_IF_ERROR(
            "pollset_work",
            grpc_pollset_work(&exec_ctx,
                              grpc_polling_entity_pollset(&request.pops),
                              &worker, gpr_now(GPR_CLOCK_MONOTONIC),
                              gpr_inf_future(GPR_CLOCK_MONOTONIC)))) {
      request.is_done = true;
    }
  }
  gpr_mu_unlock(request.mu);

  grpc_pollset_shutdown(&exec_ctx, grpc_polling_entity_pollset(&request.pops),
                        &do_nothing_closure);
  grpc_exec_ctx_finish(&exec_ctx);
  gpr_free(grpc_polling_entity_pollset(&request.pops));
  return request.token;
}
