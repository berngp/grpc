/*
 *
 * Copyright 2016 gRPC authors.
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

#include "src/core/lib/security/credentials/ssl/ssl_credentials.h"

#include <string.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/surface/api_trace.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <src/core/lib/security/transport/security_connector.h>
#include <include/grpc/grpc_security.h>

//
// SSL Channel Credentials.
//

void grpc_tsi_ssl_pem_key_cert_pairs_destroy(tsi_ssl_pem_key_cert_pair *kp,
                                             size_t num_key_cert_pairs) {
  if (kp == NULL) return;
  for (size_t i = 0; i < num_key_cert_pairs; i++) {
    gpr_free((void *)kp[i].private_key);
    gpr_free((void *)kp[i].cert_chain);
  }
  gpr_free(kp);
}

static void ssl_destruct(grpc_exec_ctx *exec_ctx,
                         grpc_channel_credentials *creds) {
  grpc_ssl_credentials *c = (grpc_ssl_credentials *)creds;
  gpr_free(c->config.pem_root_certs);
  grpc_tsi_ssl_pem_key_cert_pairs_destroy(c->config.pem_key_cert_pair, 1);
  if (c->config.verify_options.verify_peer_destruct != NULL) {
    c->config.verify_options.verify_peer_destruct(c->config.verify_options.verify_peer_callback_userdata);
  }
}

static grpc_security_status ssl_create_security_connector(
    grpc_exec_ctx *exec_ctx, grpc_channel_credentials *creds,
    grpc_call_credentials *call_creds, const char *target,
    const grpc_channel_args *args, grpc_channel_security_connector **sc,
    grpc_channel_args **new_args) {
  grpc_ssl_credentials *c = (grpc_ssl_credentials *)creds;
  grpc_security_status status = GRPC_SECURITY_OK;
  const char *overridden_target_name = NULL;
  for (size_t i = 0; args && i < args->num_args; i++) {
    grpc_arg *arg = &args->args[i];
    if (strcmp(arg->key, GRPC_SSL_TARGET_NAME_OVERRIDE_ARG) == 0 &&
        arg->type == GRPC_ARG_STRING) {
      overridden_target_name = arg->value.string;
      break;
    }
  }
  status = grpc_ssl_channel_security_connector_create(
      exec_ctx, creds, call_creds, &c->config, target, overridden_target_name,
      sc);
  if (status != GRPC_SECURITY_OK) {
    return status;
  }
  grpc_arg new_arg = grpc_channel_arg_string_create(
      (char *)GRPC_ARG_HTTP2_SCHEME, (char *)"https");
  *new_args = grpc_channel_args_copy_and_add(args, &new_arg, 1);
  return status;
}

static grpc_channel_credentials_vtable ssl_vtable = {
    ssl_destruct, ssl_create_security_connector, NULL};

static void ssl_build_config(const char *pem_root_certs,
                             grpc_ssl_pem_key_cert_pair *pem_key_cert_pair,
                             verify_peer_options *verify_options,
                             grpc_ssl_config *config) {
  if (pem_root_certs != NULL) {
    config->pem_root_certs = gpr_strdup(pem_root_certs);
  }
  if (pem_key_cert_pair != NULL) {
    GPR_ASSERT(pem_key_cert_pair->private_key != NULL);
    GPR_ASSERT(pem_key_cert_pair->cert_chain != NULL);
    config->pem_key_cert_pair = (tsi_ssl_pem_key_cert_pair *)gpr_zalloc(
        sizeof(tsi_ssl_pem_key_cert_pair));
    config->pem_key_cert_pair->cert_chain =
        gpr_strdup(pem_key_cert_pair->cert_chain);
    config->pem_key_cert_pair->private_key =
        gpr_strdup(pem_key_cert_pair->private_key);
  }
  if (verify_options != NULL) {
    memcpy(&config->verify_options, verify_options, sizeof(verify_peer_options));
  }
}

grpc_channel_credentials *grpc_ssl_credentials_create(
    const char *pem_root_certs, grpc_ssl_pem_key_cert_pair *pem_key_cert_pair,
    verify_peer_options *verify_options, void *reserved) {
  grpc_ssl_credentials *c =
      (grpc_ssl_credentials *)gpr_zalloc(sizeof(grpc_ssl_credentials));
  GRPC_API_TRACE(
      "grpc_ssl_credentials_create(pem_root_certs=%s, "
      "pem_key_cert_pair=%p, "
      "verify_options=%p, "
      "reserved=%p)",
      4, (pem_root_certs, pem_key_cert_pair, verify_options, reserved));
  GPR_ASSERT(reserved == NULL);
  c->base.type = GRPC_CHANNEL_CREDENTIALS_TYPE_SSL;
  c->base.vtable = &ssl_vtable;
  gpr_ref_init(&c->base.refcount, 1);
  ssl_build_config(pem_root_certs, pem_key_cert_pair, verify_options, &c->config);
  return &c->base;
}

//
// SSL Server Credentials.
//

static void ssl_server_destruct(grpc_exec_ctx *exec_ctx,
                                grpc_server_credentials *creds) {
  grpc_ssl_server_credentials *c = (grpc_ssl_server_credentials *)creds;
  grpc_tsi_ssl_pem_key_cert_pairs_destroy(c->config.pem_key_cert_pairs,
                                          c->config.num_key_cert_pairs);
  gpr_free(c->config.pem_root_certs);
}

static grpc_security_status ssl_server_create_security_connector(
    grpc_exec_ctx *exec_ctx, grpc_server_credentials *creds,
    grpc_server_security_connector **sc) {
  grpc_ssl_server_credentials *c = (grpc_ssl_server_credentials *)creds;
  return grpc_ssl_server_security_connector_create(exec_ctx, creds, &c->config,
                                                   sc);
}

static grpc_server_credentials_vtable ssl_server_vtable = {
    ssl_server_destruct, ssl_server_create_security_connector};

tsi_ssl_pem_key_cert_pair *grpc_convert_grpc_to_tsi_cert_pairs(
    const grpc_ssl_pem_key_cert_pair *pem_key_cert_pairs,
    size_t num_key_cert_pairs) {
  tsi_ssl_pem_key_cert_pair *tsi_pairs = NULL;
  if (num_key_cert_pairs > 0) {
    GPR_ASSERT(pem_key_cert_pairs != NULL);
    tsi_pairs = (tsi_ssl_pem_key_cert_pair *)gpr_zalloc(
        num_key_cert_pairs * sizeof(tsi_ssl_pem_key_cert_pair));
  }
  for (size_t i = 0; i < num_key_cert_pairs; i++) {
    GPR_ASSERT(pem_key_cert_pairs[i].private_key != NULL);
    GPR_ASSERT(pem_key_cert_pairs[i].cert_chain != NULL);
    tsi_pairs[i].cert_chain = gpr_strdup(pem_key_cert_pairs[i].cert_chain);
    tsi_pairs[i].private_key = gpr_strdup(pem_key_cert_pairs[i].private_key);
  }
  return tsi_pairs;
}

static void ssl_build_server_config(
    const char *pem_root_certs, grpc_ssl_pem_key_cert_pair *pem_key_cert_pairs,
    size_t num_key_cert_pairs,
    grpc_ssl_client_certificate_request_type client_certificate_request,
    grpc_ssl_server_config *config) {
  config->client_certificate_request = client_certificate_request;
  if (pem_root_certs != NULL) {
    config->pem_root_certs = gpr_strdup(pem_root_certs);
  }
  config->pem_key_cert_pairs = grpc_convert_grpc_to_tsi_cert_pairs(
      pem_key_cert_pairs, num_key_cert_pairs);
  config->num_key_cert_pairs = num_key_cert_pairs;
}

grpc_server_credentials *grpc_ssl_server_credentials_create(
    const char *pem_root_certs, grpc_ssl_pem_key_cert_pair *pem_key_cert_pairs,
    size_t num_key_cert_pairs, int force_client_auth, void *reserved) {
  return grpc_ssl_server_credentials_create_ex(
      pem_root_certs, pem_key_cert_pairs, num_key_cert_pairs,
      force_client_auth
          ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
          : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE,
      reserved);
}

grpc_server_credentials *grpc_ssl_server_credentials_create_ex(
    const char *pem_root_certs, grpc_ssl_pem_key_cert_pair *pem_key_cert_pairs,
    size_t num_key_cert_pairs,
    grpc_ssl_client_certificate_request_type client_certificate_request,
    void *reserved) {
  grpc_ssl_server_credentials *c = (grpc_ssl_server_credentials *)gpr_zalloc(
      sizeof(grpc_ssl_server_credentials));
  GRPC_API_TRACE(
      "grpc_ssl_server_credentials_create_ex("
      "pem_root_certs=%s, pem_key_cert_pairs=%p, num_key_cert_pairs=%lu, "
      "client_certificate_request=%d, reserved=%p)",
      5, (pem_root_certs, pem_key_cert_pairs, (unsigned long)num_key_cert_pairs,
          client_certificate_request, reserved));
  GPR_ASSERT(reserved == NULL);
  c->base.type = GRPC_CHANNEL_CREDENTIALS_TYPE_SSL;
  gpr_ref_init(&c->base.refcount, 1);
  c->base.vtable = &ssl_server_vtable;
  ssl_build_server_config(pem_root_certs, pem_key_cert_pairs,
                          num_key_cert_pairs, client_certificate_request,
                          &c->config);
  return &c->base;
}
