/**
 * This code is largely copied from Nginx's secure_link module, located in:
 * dependencies/downloads/nginx-1.0.12/src/http/modules/ngx_http_secure_link_module.c
 *
 * Copyright (C) 2002-2012 Igor Sysoev
 * Copyright (C) 2011,2012 Nginx, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Modifications that differ from ngx_http_secure_link_module.c are copyrighted
 * and licensed as follows:
 *
 * Copyright 2012 HellaSec, LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * ==== doorman module for nginx ====
 *
 * Access control for GET requests using the client puzzle algorithm as
 * described in "Client Puzzles: A Cryptographic Countermeasure Against
 * Connection Depletion Attacks." http://www.rsa.com/rsalabs/node.asp?id=2050
 *
 * The general idea is to rate-limit GET requests by forcing clients to pay
 * an "admission fee" in order to have a request admitted. Clients pay
 * admisson by burning their own CPU cycles (in JavaScript). This mechanism
 * rate limits clients according to their CPU resources. This form of rate
 * limiting is desirable when other forms of rate limiting (e.g.
 * HttpLimitReqModule) are undesirable -- such as when the attacker has many IP
 * addresses or legitimate users are being proxied through a single IP address.
 *
 * The mechanism works as follows.
 *   - Let x = hash(s, u, a, e) aka actual_hash, where:
 *          - s is a secret string (only known to the server)
 *          - u is the uri for the request, e.g. "/index.php"
 *          - a is the args for the reuqest, e.g. "" or "title=Main_Page"
 *          - e is the expiration timestamp
 *   - Let y = hash(x) aka meta_hash
 *   - Let the puzzle be the 6-tuple (u, a, e, y, truncate(x), b), where truncate(x) is the
 *     "truncated "version of x (aka trunc_hash), i.e. x but with b bits removed (aka
 *     missing_bits)
 *   - Give the puzzle to the client
 *       - Note, the client does not know the complete value x, but knows an approximate
 *         value of x
 *   - The client solves the puzzle by guessing various values for the value x,
 *     (i.e. a brute force search). We'll call a particular guess x'.
 *   - The client knows they have found the solution when hash(x') = y
 *   - The client re-sends the request r', along with the solution x.
 *          - r' = (u, a, e, x)
 *   - The server can quickly check whether a solution is valid by calculating
 *     hash(s,u,a,e); if it matches the given x then the server knows that the
 *     client has spent the CPU time to brute-force the hash
 *   - The protocol also uses nonces to prevent replay attacks
 *
 * Can be configured to:
 *  (1) have static puzzle diffuclty, or
 *  (2) choose dynamic puzzle difficulty depending on the upstream load (as
 *      determined by the nginx_upstream_overload module)
 *
 * ==== TODO ====
 * Since this is a very experimental POC lots of todos...
 * - Make it easier to configure
 *     - e.g. right now a lot of the logic for this module appears in nginx
 *       config files just because its convenient -- but thats not really where
 *        it belongs
 * - Profile under load an optimitize as needed
 * - Create different error handlers (i.e. goto not_found_foo)
 * - Determine if md5 provides adequate cryptographic properties
 * - Presentonly only works if there is 1 nginx worker; need to setup shared
 *   memory to suppport multiple nginx workers
 * - config like "doorman $arg_admitkey,$arg_admitkey_expire" seems bad
 *      why not just defined two variables, e.g. $doorman_admit_key and
 *      $doorman_admitkey_expire and avoid this whole comma syntax?
 * - Make code secure (e.g. buffer overflows, printf errors, off by ones, ...)
 * - Identify configuration errors during configuration step
 * - So many areas to improve code re-use...
 * - base64 encoding
 * - Instead of specifying burst_len and sleep_time, the puzzle has a param
 *   "utilization" within range (0, 1.0] -- then the client adapts its behavior
 *   to meet targetted CPU utilization (or something like that)
 * - Generate secret key when loading configuration, by pulling random bits
 *   from /dev/random (make sure to abort if reading from /dev/random blocks)
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>

// is there a better way to include this .h?
#include "../nginx_upstream_overload/ngx_http_upstream_overload.h"

#include "bayes.h"

// Defined in upstream_overload module
// This extern is a hacky way for doorman and load balancer to share memory, but
// it works for now.
//extern struct ngx_http_upstream_overload_peer_state_s *upstream_overload_peer_state;

// Reload the signature file every RELOAD_SIG_PERIOD seconds
// TODO improvements:
//  - Add a unique ID at the top of every signature file. That way Doorman can peek at
//    the file to determine if it needs reloading
//  - Have RELOAD_SIG_PERIOD specified via a config parameter
//  - Only reload signatures when an attack us occuring (or has occured recently)
#define RELOAD_SIG_PERIOD 1.0

// TODO: config param. Also note this must be on ramdisk for good performance
#define DOORMAN_SIGFILE  "/home/nginx_user/ramdisk/signature.txt"

// TODO: make this non global and shared-mem safe. Each worker
// can probably just load its own signature
bayes_feature * doorman_bayes_model = NULL;

// TODO: adapt this probability based on observations
#define APRIORI_POSITIVE 0.5

// suspicious requests receive puzzles that take 2^SIG_SERVICE_PENALTY times as long to solve
// TODO: take confidence score into account when choosing puzzle complexity
#define SIG_SERVICE_PENALTY 3.0

// md5 has 16-byte hashes
#define DOORMAN_HASH_LEN 16
// md5 requires 32 hex nibbles
#define DOORMAN_HASH_STR_SIZE 32
// The sizes of some pre-allocated strings
#define DOORMAN_INTEGER_STR_SIZE 32
#define DOORMAN_EXPIRE_STR_SIZE 32

#define DOR_REQ_ORIGINAL   1   // a request that does not have a key
#define DOR_REQ_EXPIRED    2   // a request that has expired
#define DOR_REQ_FAILURE    3   // a request with an incorrect key or some other non-expiration error
#define DOR_REQ_CHECK_HASH 4   // a request with that needs to have its key validated
typedef ngx_uint_t doorman_request_type_t;

#define INVALID_TIME ((time_t) -1)

// TODO: this global variable is a hack. find a safe, efficient place to put it (and method for accessing it)
// in shared memory.
// Holds the last time the puzzle complexity was updated
time_t last_puzzle_change = INVALID_TIME;
time_t last_overload_check = INVALID_TIME;

// Last time the signature was reloaded
time_t last_sig_reload = INVALID_TIME;

double num_missing_bits = 0.0;

/**
 * Behavior of adapative puzzle
 * TODO: allow these variables to be defined in the config
 ***************************************************************/

// Min amount of time between changing puzzle complexity
#define OVERLOAD_MODE_CHECK_PERIOD 1
#define PUZZLE_UPDATE_PERIOD    15
#define EXTREME_DIRE_MIN_SUCCESS_RATE   ((double) 0.05)
#define DIRE_MIN_SUCCESS_RATE   ((double) 0.85)
#define MIN_SUCCESS_RATE        ((double) 0.95)
#define MAX_SUCCESS_RATE        ((double) 0.99)
#define THROUGHPUT_THRESHOLD    ((double) 0.10)     // measured in requests per sec
#define MAX_MISSING_BITS        128

typedef struct {
    double evicted_count;
    double throughput_count;
    double rejected_count;
    double window_size;
    double success_rate;
    double throughput_rate;
    double rejected_rate;
} ngx_http_doorman_aggregate_stats_t;

/**
 * Values defined in the configuration. See ngx_http_doorman_commands
 */
typedef struct {
    ngx_http_complex_value_t  *variable;
    ngx_http_complex_value_t  *md5;
    ngx_http_complex_value_t  *signature_key;
    ngx_str_t                  arg_key_name;
    ngx_str_t                  arg_expire_name;
    ngx_uint_t                 expire_delta;
    ngx_uint_t                 init_missing_bits;
    ngx_uint_t                 min_missing_bits;
    ngx_uint_t                 max_missing_bits;
    ngx_uint_t                 burst_len;
    ngx_uint_t                 sleep_time;
} ngx_http_doorman_conf_t;


/**
 * Defines the structure for the ctx variable, which holds
 * per-request specific data. Most of these values simply
 * hold the actual string data that is associated with nginx
 * variables. For example, the $doorman_expire nginx variable
 * simply points to the ctx->expire string
 * Variables needed by the puzzle.html SSI page
 */
typedef struct {

    // the uri of the original request
    // associated with $doorman_orig_uri
    ngx_str_t                  orig_uri;

    // the args of the original request
    // associated with $doorman_orig_args
    ngx_str_t                  orig_args;

    // timestamp for when the puzzle expires.
    // when creating the puzzle this will be set to the current_time plus
    //      expire_delta
    // when validating a puzzle solution, this will be set the expire time
    //      grabbed from the arguments
    // associated with $doqorman_expire
    u_char expire_data[DOORMAN_EXPIRE_STR_SIZE];
    ngx_str_t                  expire;

    // the meta hash (see top-level documentation)
    // associated with $doorman_meta_hash
    u_char meta_hash_data[DOORMAN_HASH_STR_SIZE];
    ngx_str_t                  meta_hash;

    // the truncated hash (see top-level documentation)
    // associated with $doorman_trunc_hash
    u_char trunc_hash_data[DOORMAN_HASH_STR_SIZE];
    ngx_str_t                  trunc_hash;

    // the number of bits missing from trunc_hash
    // associated with $doorman_missing_bits
    u_char missing_bits_data[DOORMAN_INTEGER_STR_SIZE];
    ngx_str_t                  missing_bits;

    // the client puzzle-solver performs $doorman_burst_len hashes
    // then sleeps for $doorman_sleep_time milliseconds, then does another burst
    u_char burst_len_data[DOORMAN_INTEGER_STR_SIZE];
    ngx_str_t                  burst_len;

    u_char sleep_time_data[DOORMAN_INTEGER_STR_SIZE];
    ngx_str_t                  sleep_time;

} ngx_http_doorman_ctx_t;


static ngx_int_t ngx_http_doorman_expire_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

static void *ngx_http_doorman_create_conf(ngx_conf_t *cf);
static char *ngx_http_doorman_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_doorman_add_variables(ngx_conf_t *cf);


static ngx_command_t  ngx_http_doorman_commands[] = {

    /**
     * doorman will hold "$arg_a,$arg_b"
     * The part before the comma specifies the
     * variable containing the admission key and the part after the comma
     * specifies the expiration timestamp. Doing it this way allows
     * the administrator to specificy the arguments that will hold
     * the admission key and expire timestamp, without hard coding
     * it in this module.
     *
     * Value is assigned to conf->variable
     *
     * Example: doorman $arg_admitkey,$arg_admitkey_expire;
     */
    { ngx_string("doorman"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, variable),
      NULL },

    /**
     * md5 will hold something like "sEcr3t$uri$doorman_filtered_args$doorman_expire"
     * this module will generate actual_hash by (1) instantiating the md5
     * complex value into a variable, then (2) taking the md5 hash of the resulting
     * variable.
     */
    { ngx_string("doorman_signature_key"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, signature_key),
      NULL },

    /**
     * md5 will hold something like "sEcr3t$uri$doorman_filtered_args$doorman_expire"
     * this module will generate actual_hash by (1) instantiating the md5
     * complex value into a variable, then (2) taking the md5 hash of the resulting
     * variable.
     */
    { ngx_string("doorman_md5"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, md5),
      NULL },

    /**
     * the name of the argument used to retrieve the puzzle key from the url args
     * Should not include a dollar sign, but should include "arg_" in the prefix.
     *
     * Example: doorman_arg_key_name arg_admit_key
     * then in the url "/index.php?foo=1&admit_key=70&bar=2" the middle parameter
     * will hold the puzzle solution and the actual key in this example is 70
     */
    { ngx_string("doorman_arg_key_name"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, arg_key_name),
      NULL },

    /**
     * the name of the argument used to retrieve the expiration timestamp from the url
     * args. Should not include a dollar sign, but should include "arg_" in the prefix.
     */
    { ngx_string("doorman_arg_expire_name"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, arg_expire_name),
      NULL },

    /**
     * How many seconds does the request have before it expires?
     */
    { ngx_string("doorman_expire_delta"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, expire_delta),
      NULL },

    /**
     * the initial value for missing_bits
     */
    { ngx_string("doorman_init_missing_bits"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, init_missing_bits),
      NULL },

    /**
     * min_missing_bits
     */
    { ngx_string("doorman_min_missing_bits"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, min_missing_bits),
      NULL },

    /**
     * max_missing_bits
     */
    { ngx_string("doorman_max_missing_bits"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, max_missing_bits),
      NULL },

    /**
     * For the puzzle-solver client, brute-force hashes in bursts of $doorman_burst_len
     */
    { ngx_string("doorman_burst_len"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, burst_len),
      NULL },

    /**
     * For the puzzle-solver client, between bursts of brute-force attempts sleep $doorman_sleep_time
     * milliseconds
     */
    { ngx_string("doorman_sleep_time"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_doorman_conf_t, sleep_time),
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_doorman_module_ctx = {
    ngx_http_doorman_add_variables,        /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_doorman_create_conf,          /* create location configuration */
    ngx_http_doorman_merge_conf            /* merge location configuration */
};


ngx_module_t  ngx_http_doorman_module = {
    NGX_MODULE_V1,
    &ngx_http_doorman_module_ctx,          /* module context */
    ngx_http_doorman_commands,             /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

/**
 *
 */
#define RL1 "%%rl1%% "
#define RL2 "%%rl2%% "
#define RL3 "%%rl3%% "
#define RL4 "%%rl4%% "
#define RL5 "%%rl5%% "
#define RL6 "%%rl6%% "
#define RL7 "%%rl7%% "
#define RL8 "%%rl8%% "
#define RL9 "%%rl9%% "
#define RL10 "%%rl10%% "
#define RL11 "%%rl11%% "
#define RL12 "%%rl12%% "
#define RL13 "%%rl13%% "
#define RL14 "%%rl14%% "
#define RL15 "%%rl15%% "
#define RL16 "%%rl16%% "
#define RL17 "%%rl17%% "
#define RL18 "%%rl18%% "
#define RL19 "%%rl19%% "
#define RL20 "%%rl20%% "
#define RL21 "%%rl21%% "
#define RL22 "%%rl22%% "
#define RL23 "%%rl23%% "
#define RL24 "%%rl24%% "
#define RL25 "%%rl25%% "

#define TWO_1 2
#define TWO_2 4
#define TWO_3 8
#define TWO_4 16
#define TWO_5 32
#define TWO_6 64
#define TWO_7 128
#define TWO_8 256
#define TWO_9 512
#define TWO_10 1024
#define TWO_11 2048
#define TWO_12 4096
#define TWO_13 8192
#define TWO_14 16384
#define TWO_15 32768
#define TWO_16 65536
#define TWO_17 131072
#define TWO_18 262144
#define TWO_19 524288
#define TWO_20 1048576
#define TWO_21 2097152
#define TWO_22 4194304
#define TWO_23 8388608
#define TWO_24 16777216
#define TWO_25 33554432

static ngx_str_t ngx_http_request_length1 = ngx_string(RL1 );
static ngx_str_t ngx_http_request_length2 = ngx_string(RL1 RL2);
static ngx_str_t ngx_http_request_length3 = ngx_string(RL1 RL2 RL3);
static ngx_str_t ngx_http_request_length4 = ngx_string(RL1 RL2 RL3 RL4);
static ngx_str_t ngx_http_request_length5 = ngx_string(RL1 RL2 RL3 RL4 RL5);
static ngx_str_t ngx_http_request_length6 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6);
static ngx_str_t ngx_http_request_length7 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7);
static ngx_str_t ngx_http_request_length8 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8);
static ngx_str_t ngx_http_request_length9 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9);
static ngx_str_t ngx_http_request_length10 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10);
static ngx_str_t ngx_http_request_length11 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11);
static ngx_str_t ngx_http_request_length12 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12);
static ngx_str_t ngx_http_request_length13 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13);
static ngx_str_t ngx_http_request_length14 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14);
static ngx_str_t ngx_http_request_length15 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15);
static ngx_str_t ngx_http_request_length16 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16);
static ngx_str_t ngx_http_request_length17 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17);
static ngx_str_t ngx_http_request_length18 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18);
static ngx_str_t ngx_http_request_length19 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18 RL19);
static ngx_str_t ngx_http_request_length20 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18 RL19 RL20);
static ngx_str_t ngx_http_request_length21 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18 RL19 RL20 RL21);
static ngx_str_t ngx_http_request_length22 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18 RL19 RL20 RL21 RL22);
static ngx_str_t ngx_http_request_length23 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18 RL19 RL20 RL21 RL22 RL23);
static ngx_str_t ngx_http_request_length24 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18 RL19 RL20 RL21 RL22 RL23 RL24);
static ngx_str_t ngx_http_request_length25 = ngx_string(RL1 RL2 RL3 RL4 RL5 RL6 RL7 RL8 RL9 RL10 RL11 RL12 RL13 RL14 RL15 RL16 RL17 RL18 RL19 RL20 RL21 RL22 RL23 RL24 RL25);

/**
 * names of nginx variables (associated with request) that this module defines
 */
static ngx_str_t  ngx_http_doorman_result_name       = ngx_string("doorman_result");
static ngx_str_t  ngx_http_doorman_orig_uri_name     = ngx_string("doorman_orig_uri");
static ngx_str_t  ngx_http_doorman_orig_args_name    = ngx_string("doorman_orig_args");
static ngx_str_t  ngx_http_doorman_expire_name       = ngx_string("doorman_expire");
static ngx_str_t  ngx_http_doorman_meta_hash_name    = ngx_string("doorman_meta_hash");
static ngx_str_t  ngx_http_doorman_trunc_hash_name   = ngx_string("doorman_trunc_hash");
static ngx_str_t  ngx_http_doorman_missing_bits_name = ngx_string("doorman_missing_bits");
static ngx_str_t  ngx_http_doorman_burst_len_name    = ngx_string("doorman_burst_len");
static ngx_str_t  ngx_http_doorman_sleep_time_name   = ngx_string("doorman_sleep_time");
static ngx_str_t  ngx_http_doorman_request_length = ngx_string("doorman_request_length");

// Computes hash of src_str and stores it in result_buf
// PRECONDITION: buf points to a u_char array of size DOORMAN_HASH_LEN
static void
ngx_http_doorman_hash(u_char *result_buf, ngx_str_t *src_str)
{
    ngx_md5_t md5;
    ngx_md5_init(&md5);
    ngx_md5_update(&md5, src_str->data, src_str->len);
    ngx_md5_final(result_buf, &md5);
}

// Sets the most insignificant bits in buf to 0 (missing_bits)
// PRECONDITIONS: buf points to a u_char array of size DOORMAN_HASH_LEN
//      0 <= missing_bits <= 8 * DOORMAN_HASH_LEN
// TODO: instead of int use nginx uint
static void
ngx_http_doorman_truncate(u_char *buf, int missing_bits)
{
    int num_whole_bytes = missing_bits / 8;
    int remaining_bits = missing_bits % 8;
    int byte, byte_i;

    for (byte = 0; byte < num_whole_bytes; byte++) {
        byte_i = DOORMAN_HASH_LEN - 1 - byte;
        buf[byte_i] = 0;
    }

    byte_i = DOORMAN_HASH_LEN - 1 - byte;
    buf[byte_i] = (buf[byte_i] >> remaining_bits) << remaining_bits;
}

// translates hash buf (with DOORMAN_HASH_LEN bytes) into a
// url-safe str (with str->data having  DOORMAN_HASH_STR_SIZE bytes)
//
// For now doing my own hex conversion because it's easier and safer
// to re-implement it than to reverse engineer the nginx API
// for snprintf or base64 encoding from the source
static void
ngx_http_doorman_hashval_to_str(ngx_str_t *result_str, u_char *src_buf)
{
    static char hex[] = "0123456789abcdef";
    result_str->len = DOORMAN_HASH_STR_SIZE;
    ngx_uint_t  buf_i;
    ngx_uint_t  str_i;
    u_char      byte;
    u_char      nibble;

    for (buf_i = 0, str_i = 0 ; buf_i < DOORMAN_HASH_LEN; buf_i++, str_i += 2) {
        byte = src_buf[buf_i];
        nibble = (byte & 0xf0) >> 4;
        result_str->data[str_i] = hex[nibble];
        nibble = byte & 0xf;
        result_str->data[str_i + 1] = hex[nibble];
    }
}


/**
 * Iterates over the arguments in args
 * p points to beginning of previous arg (or NULL if this is the first time calling
 *  this func)
 * returns pointer to beginning of next arg
 * *len == the number of chars skipped over == the length of the previous arg
 * if you don't care about the value of *len, then set len = NULL
 * or if there are no more args, then returns pointer just past end of args->data
 * TODO: this function needs serious scrutiny and testing
 */
static u_char *
ngx_http_doorman_next_arg(ngx_str_t *args, u_char *p, ngx_uint_t *len) {
    u_char *end = &args->data[args->len];
    ngx_uint_t counter = 0;

    if (p == NULL) {
        p = &args->data[0];
        if (len != NULL) {
            *len = 0;
        }
        return p;
    }

    while (p != end && *p != '&') {
        p++;
        counter++;
    }

    if (p != end) {
        // skip over the '&'
        p++;
        counter++;
    }

    if (len != NULL) {
        *len = counter;
    }

    return p;
}

/**
 * Finds the first occurence of the argument (named by arg_name) within args
 * Returns pointer to beginning of arg, or NULL if it can't be found
 * Assumptions (need to be validated):
 *  arguments are exactly delimited by '&' . I.e. if there are N arguments,
 *  then there are exactly N - 1 '&' characters appearing in args
 * TODO: this function needs serious scrutiny and testing
 */
static u_char *
ngx_http_doorman_find_arg(ngx_str_t *arg_name, ngx_str_t *args)
{
    u_char *end = &args->data[args->len];
    u_char *p = ngx_http_doorman_next_arg(args, NULL, NULL);
    u_char *q;
    ngx_uint_t i;

    while (p != end) {
        for (i = 0, q = p; i < arg_name->len; i++, q++) {
             // if end of argument is reached
             if (q == end || *q == '&') {
                break;
             }
             // if mismatch
             if (arg_name->data[i] != *q) {
                break;
             }
        }
        // if there was a match so far, then make sure there is an '=' sign at the end
        if (i == arg_name->len && q != end && *q == '=') {
            return p;
        }
        p = ngx_http_doorman_next_arg(args, p, NULL);
    }

    return NULL;
}

/**
 * removes the first occurence arg_key and arg_expire arguments from args
 * Consider assuming: args->data is null terminated
 * TODO: this function needs serious scrutiny and testing
 * TODO: even if these string manipulations are correct, I'm not convinced
 *  it's a good way to manipulate variable values
 */
static void
ngx_http_doorman_strip_arg(ngx_str_t *arg_name, ngx_str_t *args)
{
    ngx_uint_t arg_len;
    u_char *p, *q;
    u_char *end = &args->data[args->len]; // points to first char outside of args->data
    u_char *begin = &args->data[0];

    p = ngx_http_doorman_find_arg(arg_name, args);
    if (p != NULL) {
        // find the beginning of the next arg
        q = ngx_http_doorman_next_arg(args, p, &arg_len);
        // everything in the range [p, q) needs to be removed
        for (; q != end; p++, q++) {
            *p = *q;
        }
        args->len -= arg_len;
        // if the arg was preceded by an & AND the arg was the last arg
        // then need to swallow the previous &
        if (q == end && p != begin) {
            if (*(p - 1) == '&') {
                args->len -= 1;
            } else {
                // error?
            }
        }
    }
}


// returns value of string, or NULL if the arg doesn't exist
static ngx_variable_value_t *
ngx_doorman_arg_val(ngx_http_request_t *r, ngx_str_t *arg_name)
{
    ngx_uint_t                  hash;
    ngx_str_t                   proper_name;
    ngx_variable_value_t       *value;

    proper_name.len = arg_name->len + 4;
    proper_name.data = ngx_palloc(r->pool, sizeof(u_char) * proper_name.len);
    if (proper_name.data == NULL) {
        return NULL;
    }
    ngx_memcpy(proper_name.data, (u_char *) "arg_", sizeof(u_char) * 4);
    ngx_memcpy(&proper_name.data[4], arg_name->data, sizeof(u_char) * arg_name->len);

    hash = ngx_hash_key(proper_name.data, proper_name.len);
    value = ngx_http_get_variable(r, &proper_name, hash);

    if (value->not_found) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
           "doorman: %V not found", &proper_name);
        return NULL;
    } else {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
           "doorman: %V found", &proper_name);
        return value;
    }
}

/**
 * Returns one of:
 *  DOR_REQ_ORIGINAL
 *  DOR_REQ_EXPIRED
 *  DOR_REQ_FAILURE
 *  DOR_REQ_CHECK_HASH
 */
static doorman_request_type_t
ngx_doorman_req_type(ngx_http_request_t *r, ngx_http_doorman_ctx_t *ctx,
    ngx_http_doorman_conf_t *conf, ngx_variable_value_t ** key_value_ptr,
    ngx_variable_value_t ** expire_value_ptr)
{
    ngx_variable_value_t *key_value = ngx_doorman_arg_val(r, &conf->arg_key_name);
    ngx_variable_value_t *expire_value = ngx_doorman_arg_val(r, &conf->arg_expire_name);
    time_t expiration;

    *key_value_ptr = key_value;
    *expire_value_ptr = expire_value;

    if (key_value == NULL || expire_value == NULL) {
        if (key_value == NULL && expire_value == NULL) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman: an original request url (i.e. included neither arg_key nor arg_expire)");
            return DOR_REQ_ORIGINAL;
        } else {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "doorman: request args included arg_key or arg_expire but not both");
            return DOR_REQ_FAILURE;
        }
    }

    // request contains key and expire timestamp

    // validate timestamp
    expiration = ngx_atotm(expire_value->data, expire_value->len);
    if (expiration < ngx_time()) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "doorman: request expired");
        return DOR_REQ_EXPIRED;
    }

    // validate length of key
    if (key_value->len != DOORMAN_HASH_STR_SIZE) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "doorman: key_value->len != DOORMAN_HASH_STR_SIZE");
        return DOR_REQ_FAILURE;
    }


    // expiration timestamp is good, length of hash is good, now just need to validate hash
    return DOR_REQ_CHECK_HASH;

    /*if (ngx_http_doorman_str_to_hashval(given_hash, key_value->data) != NGX_OK) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "doorman: key_value contains invalid characters");
        return DOR_REQ_FAILURE;
    }*/

}

static ngx_http_doorman_ctx_t *
ngx_http_doorman_create_ctx(ngx_http_request_t *r)
{
    ngx_http_doorman_ctx_t *ctx;

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_doorman_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }
    ngx_http_set_ctx(r, ctx, ngx_http_doorman_module);

    // initialize context variables to nothing-values
    ctx->orig_uri.len =
        ctx->orig_args.len =
        ctx->expire.len =
        ctx->meta_hash.len =
        ctx->trunc_hash.len =
        ctx->missing_bits.len =
        ctx->burst_len.len =
        ctx->sleep_time.len = 0;

    ctx->orig_uri.data = NULL;
    ctx->orig_args.data = NULL;
    ctx->expire.data = ctx->expire_data;
    ctx->missing_bits.data = ctx->missing_bits_data;
    ctx->meta_hash.data = ctx->meta_hash_data;
    ctx->trunc_hash.data = ctx->trunc_hash_data;
    ctx->burst_len.data = ctx->burst_len_data;
    ctx->sleep_time.data = ctx->sleep_time_data;

    return ctx;
}

static ngx_http_doorman_conf_t *
ngx_http_doorman_get_conf(ngx_http_request_t *r)
{
    ngx_http_doorman_conf_t * conf = ngx_http_get_module_loc_conf(r, ngx_http_doorman_module);

    if (conf->variable == NULL ||
        conf->md5 == NULL ||
        conf->signature_key == NULL ||
        conf->arg_key_name.len == 0 ||
        conf->arg_expire_name.len == 0 ||
        conf->expire_delta == NGX_CONF_UNSET_UINT ||
        conf->init_missing_bits == NGX_CONF_UNSET_UINT ||
        conf->min_missing_bits == NGX_CONF_UNSET_UINT ||
        conf->max_missing_bits == NGX_CONF_UNSET_UINT ||
        conf->burst_len == NGX_CONF_UNSET_UINT ||
        conf->sleep_time == NGX_CONF_UNSET_UINT) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                   "doorman: one of (variable, md5, signature_key, expire_delta, init_missing_bits, "
                   "min_missing_bits, max_missing_bits, burst_len, sleep_time) not defined");
        return NULL;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->variable == '%V'", &conf->variable->value);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->md5 == '%V'", &conf->md5->value);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->signature_key == '%V'", &conf->signature_key->value);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->arg_key_name == '%V'", &conf->arg_key_name);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->arg_expire_name == '%V'", &conf->arg_expire_name);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->expire_delta == %d", conf->expire_delta);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->init_missing_bits == %d", conf->init_missing_bits);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->min_missing_bits == %d", conf->min_missing_bits);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->max_missing_bits == %d", conf->max_missing_bits);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->burst_len == %d", conf->burst_len);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: conf->sleep_time == %d", conf->sleep_time);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: r->args == '%V'", &r->args);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: r->uri == '%V'", &r->uri);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: num_missing_bits == %1.3f", num_missing_bits);

    return conf;
}

static void
ngx_http_doorman_inc_missing_bits(ngx_http_request_t *r, ngx_http_doorman_conf_t *conf, double amount)
{
   num_missing_bits += amount;
    if (num_missing_bits < conf->max_missing_bits) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman puzzle: incremented missing bits to %1.3f", num_missing_bits);
    } else {
        num_missing_bits = (double) conf->max_missing_bits;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman puzzle: missing bits is maxed out at %1.3f", num_missing_bits);
    }
}

static void
ngx_http_doorman_dec_missing_bits(ngx_http_request_t *r, ngx_http_doorman_conf_t *conf, double amount)
{
    num_missing_bits -= amount;
    if (num_missing_bits > conf->min_missing_bits) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman puzzle: decremented missing bits to %1.3f", num_missing_bits);
    } else {
        num_missing_bits = (double) conf->min_missing_bits;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman puzzle: missing bits bottomed out at %1.3f", num_missing_bits);
    }
}

static void ngx_http_doorman_stats(ngx_http_doorman_aggregate_stats_t * stats, ngx_log_t * log) {

    ngx_spinlock(&upstream_overload_peer_state->lock, SPINLOCK_VALUE, SPINLOCK_NUM_SPINS);

    stats->evicted_count = (double) upstream_overload_peer_state->stats.evicted_count;
    stats->throughput_count = (double) upstream_overload_peer_state->stats.throughput_count;
    stats->rejected_count = (double) upstream_overload_peer_state->stats.rejected_count;
    stats->window_size = (double) upstream_overload_peer_state->stats.window_size;

    // proportion of requests that were admitted with needing an eviction
    if (upstream_overload_peer_state->stats.throughput_count == 0) {
        stats->success_rate = 0;
    } else {
        stats->success_rate = 1.0 - (stats->evicted_count / (stats->evicted_count + stats->throughput_count));
    }

    // average number of requests per second doorman admitted
    stats->throughput_rate = stats->throughput_count / stats->window_size;
    stats->rejected_rate = stats->rejected_count / stats->window_size;

    ngx_unlock(&upstream_overload_peer_state->lock);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
               "doorman puzzle: stats evicted_count == %1.3f", stats->evicted_count);
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
               "doorman puzzle: stats throughput_count == %1.3f, throughput_rate == %1.3f", stats->throughput_count, stats->throughput_rate);
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
               "doorman puzzle: stats rejected_count == %1.3f, rejected_rate == %1.3f", stats->rejected_count, stats->rejected_rate);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
               "doorman puzzle: stats success_rate == %1.3f", stats->success_rate);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
               "doorman puzzle: num_missing_bits == %1.3f", num_missing_bits);

}

static void
ngx_http_doorman_reload_signature(ngx_http_request_t *r, ngx_http_doorman_conf_t *conf)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
       "doorman signature: reloading");

    delete_model(&doorman_bayes_model);

    int fd = open(DOORMAN_SIGFILE, O_RDONLY);
    if (fd == -1) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "doorman signature: could not open file '%s'", DOORMAN_SIGFILE);
        return;
    }

    int num_features = load_model(&doorman_bayes_model, fd);
    if (num_features < 0) {
        ngx_log_error(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "doorman signature: error parsing file '%s': error=%d", DOORMAN_SIGFILE, num_features);
    } else {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "doorman signature: read %d feature(s) from '%s'", num_features, DOORMAN_SIGFILE);
    }
    last_sig_reload = ngx_time();
}

static void
ngx_http_doorman_update_puzzle(ngx_http_request_t *r, ngx_http_doorman_conf_t *conf)
{
    ngx_http_doorman_aggregate_stats_t stats;

    if (upstream_overload_peer_state == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "doorman: upstream_overload_peer_state not set");
        return;
    }

    // Calculate stats
    ngx_http_doorman_stats(&stats, r->connection->log);

    if (stats.evicted_count > 0 && stats.success_rate < DIRE_MIN_SUCCESS_RATE) {
        if (stats.success_rate < EXTREME_DIRE_MIN_SUCCESS_RATE) {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: success_rate==%1.3f < extreme_dire_min_success_rate==%1.3f", stats.success_rate, DIRE_MIN_SUCCESS_RATE);
            // evict everthing

        } else {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: success_rate==%1.3f < dire_min_success_rate==%1.3f", stats.success_rate, DIRE_MIN_SUCCESS_RATE);
        }
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: throughput==%1.3f >= threshold==%1.3f", stats.throughput_rate, THROUGHPUT_THRESHOLD);
            ngx_http_doorman_inc_missing_bits(r, conf, 128.0);
    } else if (stats.rejected_rate > 0.2) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: rejected_rate==%1.3f > 0.2 ", stats.rejected_rate);
        ngx_http_doorman_inc_missing_bits(r, conf, 2.0);
    } else if (stats.evicted_count > 0 && stats.success_rate < MIN_SUCCESS_RATE) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: success_rate==%1.3f < min_success_rate==%1.3f", stats.success_rate, MIN_SUCCESS_RATE);
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: throughput==%1.3f >= threshold==%1.3f", stats.throughput_rate, THROUGHPUT_THRESHOLD);
            ngx_http_doorman_inc_missing_bits(r, conf, 1.0);
    } else if (stats.success_rate > MAX_SUCCESS_RATE) {
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: success_rate==%1.3f > max_success_rate==%1.3f (throughout==%1.3f)", stats.success_rate, MAX_SUCCESS_RATE, stats.throughput_rate);
        ngx_http_doorman_dec_missing_bits(r, conf, 1.0);
    } else {
        ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: min_success_rate==%1.3f <= success_rate==%1.3f <= max_success_rate==%1.3f (throughput==%1.3f)",
               MIN_SUCCESS_RATE, stats.success_rate, MAX_SUCCESS_RATE, stats.throughput_rate);
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman puzzle: keeping num_missing_bits the same at %1.3f", num_missing_bits);
    }

    last_puzzle_change = ngx_time();
}

// instantiates the $doorman_result nginx-variable
static ngx_int_t
ngx_http_doorman_result_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_str_t                     temp_str;
    time_t                        gen_expire;   // the expiration value generated for the puzzle
    ngx_http_doorman_ctx_t       *ctx;
    ngx_http_doorman_conf_t      *conf;
    u_char                        actual_hash_buf[DOORMAN_HASH_LEN];
    u_char                        meta_hash_buf[DOORMAN_HASH_LEN];
    doorman_request_type_t        request_type;
    ngx_variable_value_t         *key_value;
    ngx_variable_value_t         *expire_value;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "\ndoorman: new_request");


    /**
     * Setup ctx and conf
     *************************************************************************/

    if (upstream_overload_peer_state != NULL) {
        ngx_spinlock(&upstream_overload_peer_state->lock, SPINLOCK_VALUE, SPINLOCK_NUM_SPINS);
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman: evict_rate == %d/%d", upstream_overload_peer_state->stats.evicted_count, upstream_overload_peer_state->stats.window_size);
        ngx_unlock(&upstream_overload_peer_state->lock);
    }
    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);
    if (ctx != NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman: called again but already initialized");
        return NGX_OK;
    }

    ctx = ngx_http_doorman_create_ctx(r);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    conf = ngx_http_doorman_get_conf(r);
    if (conf == NULL) {
        return NGX_ERROR;
    }


    /**
     * Relad the signature if needed
     *************************************************************************/
    if (last_sig_reload == INVALID_TIME ||
        ngx_time() - last_sig_reload > RELOAD_SIG_PERIOD) {
        ngx_http_doorman_reload_signature(r, conf);
    }

    /**
     * Classify the request
     *************************************************************************/
    ngx_str_t           request_str;
    if (ngx_http_complex_value(r, conf->signature_key, &request_str) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                   "doorman ERROR while ngx_http_complex_value(r, conf->signature_key, &request_str)");
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
       "request_str: '%V'", &request_str);

    int classification = classify(doorman_bayes_model, 0.5, (char*) request_str.data,
        (char*) &request_str.data[request_str.len]);

    if (classification > 0) {
        dd_error1(NGX_LOG_ERR, r->connection->log, 0,
           "doorman signature request_str: '%V' --> suspected high-density", &request_str);
    } else {
        dd_error1(NGX_LOG_ERR, r->connection->log, 0,
           "doorman signature request_str: '%V' --> suspected low-density", &request_str);
    }


    /**
     * Update the puzzle complexity if needed
     *************************************************************************/
    if (num_missing_bits <= 0.0 && (last_overload_check == INVALID_TIME ||
        ngx_time() - last_overload_check > OVERLOAD_MODE_CHECK_PERIOD)) {
        // update the stats first
        if (upstream_overload_peer_state != NULL) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman: updating stats");
            ngx_spinlock(&upstream_overload_peer_state->lock, SPINLOCK_VALUE, SPINLOCK_NUM_SPINS);
            ngx_http_upstream_overload_update_stats(r->connection->log, upstream_overload_peer_state, 0, 0, 0);
            ngx_unlock(&upstream_overload_peer_state->lock);
        }

        ngx_http_doorman_aggregate_stats_t stats;

        if (upstream_overload_peer_state == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "doorman: upstream_overload_peer_state not set");
        }

        // Calculate stats
        ngx_http_doorman_stats(&stats, r->connection->log);

        if (stats.evicted_count > 0 && stats.success_rate < DIRE_MIN_SUCCESS_RATE) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                   "doorman: entering overload mode");
            ngx_http_doorman_update_puzzle(r, conf);
        }

    } else if (last_puzzle_change == INVALID_TIME ||
        ngx_time() - last_puzzle_change > PUZZLE_UPDATE_PERIOD) {

        // update the stats first
        if (upstream_overload_peer_state != NULL) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman: updating stats");
            ngx_spinlock(&upstream_overload_peer_state->lock, SPINLOCK_VALUE, SPINLOCK_NUM_SPINS);
            ngx_http_upstream_overload_update_stats(r->connection->log, upstream_overload_peer_state, 0, 0, 0);
            ngx_unlock(&upstream_overload_peer_state->lock);
        }

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: updating puzzle:");
        ngx_http_doorman_update_puzzle(r, conf);
    }

    /**
     * Initialize some nginx variables that are needed regardless of request type
     *************************************************************************/

    // copy uri from the original request into orig_uri
    ctx->orig_uri.len = r->uri.len;
    ctx->orig_uri.data = ngx_pstrdup(r->pool, &r->uri);
    if (ctx->orig_uri.data == NULL) {
        return NGX_ERROR;
    }

    // copy args from the original request into orig_args
    ctx->orig_args.len = r->args.len;
    ctx->orig_args.data = ngx_pstrdup(r->pool, &r->args);
    if (ctx->orig_args.data == NULL) {
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: orig_args == '%V'", &ctx->orig_args);

    // strip the parameters for key and expire from orig_args
    ngx_http_doorman_strip_arg(&conf->arg_key_name, &ctx->orig_args);
    ngx_http_doorman_strip_arg(&conf->arg_expire_name, &ctx->orig_args);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: orig_args == '%V' after strip", &ctx->orig_args);


    /**
     * Determine what type of a request this is and route accordingly
     *************************************************************************/

    // if there are no missing bits, then just accept the request
    if (num_missing_bits <= 0.0) {
        v->len = 1;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = (u_char *) "1";

        /**
         * Output stats, purely for temporary debugging
         *************************************************************************/
        ngx_http_doorman_aggregate_stats_t stats;

        if (upstream_overload_peer_state != NULL) {
            // Calculate stats
            ngx_http_doorman_stats(&stats, r->connection->log);
        }

        return NGX_OK;
    }

    request_type = ngx_doorman_req_type(r, ctx, conf, &key_value, &expire_value);

    if (request_type == DOR_REQ_FAILURE || request_type == DOR_REQ_EXPIRED) {
        v->len = 1;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = (u_char *) "0";
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                   "doorman request_type == DOR_REQ_FAILURE || request_type == DOR_REQ_EXPIRED");
        return NGX_OK;
    } else if (request_type == DOR_REQ_CHECK_HASH) {

        // initialize the $doorman_expire variable to the expiration specified in the argument
        ctx->expire.data = expire_value->data;
        ctx->expire.len = expire_value->len;

        // perform variable substition in $doorman_md5
        if (ngx_http_complex_value(r, conf->md5, &temp_str) != NGX_OK) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                       "doorman ERROR while ngx_http_complex_value(r, conf->md5, &temp_str)");
            return NGX_ERROR;
        }
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "doorman link md5: \"%V\"", &temp_str);

        ngx_http_doorman_hash(actual_hash_buf, &temp_str);
        ngx_http_doorman_hashval_to_str(&temp_str, actual_hash_buf);
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "doorman actual_hash str: \"%V\"", &temp_str);
        v->len = 1;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;

        // is the key valid?
        if (ngx_strncmp(key_value->data, temp_str.data, temp_str.len) == 0) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "doorman hash is valid");
            v->data = (u_char *) "1";
            /**
             * Output stats, purely for temporary debugging
             *************************************************************************/
            ngx_http_doorman_aggregate_stats_t stats;

            if (upstream_overload_peer_state != NULL) {
                // Calculate stats
                ngx_http_doorman_stats(&stats, r->connection->log);
            }
        } else {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                       "doorman hash is invalid");
            v->data = (u_char *) "0";
        }

        // This is kind of an ugly hack, but what this does is remove the puzzle key and expires
        // parameters from the request that is headed towards the upstream worker.
        // This way, the upstream workers will never see puzzle key or expires parameter. I
        // imagine there is a better way to accomplish this feat in nginx, but this seems to work
        // for now
        r->args.len = ctx->orig_args.len;
        r->args.data = ctx->orig_args.data;

        return NGX_OK;
    }

    /**
     * Create a puzzle response for the request
     *************************************************************************/

    // initialize the $doorman_expire variable
    gen_expire = ngx_time() + conf->expire_delta;
    ngx_snprintf(ctx->expire.data, sizeof(ctx->expire_data), "%d%Z", gen_expire);
    ctx->expire.len = ngx_strlen(ctx->expire_data);
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: now(%d) + delay(%d) == expire('%V')", ngx_time(), conf->expire_delta, &ctx->expire);

    // perform variable substition in $doorman_md5
    if (ngx_http_complex_value(r, conf->md5, &temp_str) != NGX_OK) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman ERROR while ngx_http_complex_value(r, conf->md5, &temp_str)");
        return NGX_ERROR;
    }
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman link md5: \"%V\"", &temp_str);

    // actual_hash_buf = hash($doorman_md5)
    ngx_http_doorman_hash(actual_hash_buf, &temp_str);
    // temp_str = hex-string version of actual_hash_buf
    ngx_http_doorman_hashval_to_str(&temp_str, actual_hash_buf);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman actual_hash str: \"%V\"", &temp_str);

    double num_missing_bits_request = num_missing_bits;
    if (classification > 0) {
        // if the request is suspected to be high-density then increase missing bits
        num_missing_bits_request += SIG_SERVICE_PENALTY;
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman signature missing_bits: suspcious request, increasing missing bits "
                   "from %f to %f", num_missing_bits, num_missing_bits_request);
    }

    // initialize the $doorman_missing_bits variable
    ngx_snprintf(ctx->missing_bits.data, sizeof(ctx->missing_bits_data), "%d%Z", (ngx_uint_t) num_missing_bits_request);
    ctx->missing_bits.len = ngx_strlen(ctx->missing_bits_data);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: missing_bits == '%V'", &ctx->missing_bits);

    // initialize the $doorman_burst_len variable
    ngx_snprintf(ctx->burst_len.data, sizeof(ctx->burst_len_data), "%d%Z", conf->burst_len);
    ctx->burst_len.len = ngx_strlen(ctx->burst_len_data);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: burst_len == '%V'", &ctx->burst_len);

    // initialize the $doorman_sleep_time variable
    ngx_snprintf(ctx->sleep_time.data, sizeof(ctx->sleep_time_data), "%d%Z", conf->sleep_time);
    ctx->sleep_time.len = ngx_strlen(ctx->sleep_time_data);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "doorman: sleep_time == '%V'", &ctx->sleep_time);


    // meta_hash_buf = hash(hex-string version of actual_hash_buf)
    ngx_http_doorman_hash(meta_hash_buf, &temp_str);
    ngx_http_doorman_hashval_to_str(&ctx->meta_hash, meta_hash_buf);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman meta_hash str: \"%V\"", &ctx->meta_hash);

    ngx_http_doorman_truncate(actual_hash_buf, (ngx_uint_t) num_missing_bits_request);
    ngx_http_doorman_hashval_to_str(&ctx->trunc_hash, actual_hash_buf);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "doorman trunc_hash str: \"%V\"", &ctx->trunc_hash);

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_expire_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->expire.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->expire.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_missing_bits_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->missing_bits.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->missing_bits.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_request_length_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    if (r->request_length >= TWO_25) {
        v->len = ngx_http_request_length25.len;
        v->data = ngx_http_request_length25.data;
    } else if (r->request_length >= TWO_24) {
        v->len = ngx_http_request_length24.len;
        v->data = ngx_http_request_length24.data;
    } else if (r->request_length >= TWO_23) {
        v->len = ngx_http_request_length23.len;
        v->data = ngx_http_request_length23.data;
    } else if (r->request_length >= TWO_22) {
        v->len = ngx_http_request_length22.len;
        v->data = ngx_http_request_length22.data;
    } else if (r->request_length >= TWO_21) {
        v->len = ngx_http_request_length21.len;
        v->data = ngx_http_request_length21.data;
    } else if (r->request_length >= TWO_20) {
        v->len = ngx_http_request_length20.len;
        v->data = ngx_http_request_length20.data;
    } else if (r->request_length >= TWO_19) {
        v->len = ngx_http_request_length19.len;
        v->data = ngx_http_request_length19.data;
    } else if (r->request_length >= TWO_18) {
        v->len = ngx_http_request_length18.len;
        v->data = ngx_http_request_length18.data;
    } else if (r->request_length >= TWO_17) {
        v->len = ngx_http_request_length17.len;
        v->data = ngx_http_request_length17.data;
    } else if (r->request_length >= TWO_16) {
        v->len = ngx_http_request_length16.len;
        v->data = ngx_http_request_length16.data;
    } else if (r->request_length >= TWO_15) {
        v->len = ngx_http_request_length15.len;
        v->data = ngx_http_request_length15.data;
    } else if (r->request_length >= TWO_14) {
        v->len = ngx_http_request_length14.len;
        v->data = ngx_http_request_length14.data;
    } else if (r->request_length >= TWO_13) {
        v->len = ngx_http_request_length13.len;
        v->data = ngx_http_request_length13.data;
    } else if (r->request_length >= TWO_12) {
        v->len = ngx_http_request_length12.len;
        v->data = ngx_http_request_length12.data;
    } else if (r->request_length >= TWO_11) {
        v->len = ngx_http_request_length11.len;
        v->data = ngx_http_request_length11.data;
    } else if (r->request_length >= TWO_10) {
        v->len = ngx_http_request_length10.len;
        v->data = ngx_http_request_length10.data;
    } else if (r->request_length >= TWO_9) {
        v->len = ngx_http_request_length9.len;
        v->data = ngx_http_request_length9.data;
    } else if (r->request_length >= TWO_8) {
        v->len = ngx_http_request_length8.len;
        v->data = ngx_http_request_length8.data;
    } else if (r->request_length >= TWO_7) {
        v->len = ngx_http_request_length7.len;
        v->data = ngx_http_request_length7.data;
    } else if (r->request_length >= TWO_6) {
        v->len = ngx_http_request_length6.len;
        v->data = ngx_http_request_length6.data;
    } else if (r->request_length >= TWO_5) {
        v->len = ngx_http_request_length5.len;
        v->data = ngx_http_request_length5.data;
    } else if (r->request_length >= TWO_4) {
        v->len = ngx_http_request_length4.len;
        v->data = ngx_http_request_length4.data;
    } else if (r->request_length >= TWO_3) {
        v->len = ngx_http_request_length3.len;
        v->data = ngx_http_request_length3.data;
    } else if (r->request_length >= TWO_2) {
        v->len = ngx_http_request_length2.len;
        v->data = ngx_http_request_length2.data;
    } else {
        v->len = ngx_http_request_length1.len;
        v->data = ngx_http_request_length1.data;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_burst_len_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->burst_len.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->burst_len.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_sleep_time_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->sleep_time.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->sleep_time.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_orig_uri_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->orig_uri.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->orig_uri.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_orig_args_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->orig_args.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->orig_args.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_trunc_hash_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->trunc_hash.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->trunc_hash.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_doorman_meta_hash_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_doorman_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_doorman_module);

    if (ctx) {
        v->len = ctx->meta_hash.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->meta_hash.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static void *
ngx_http_doorman_create_conf(ngx_conf_t *cf)
{
    ngx_http_doorman_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_doorman_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->variable = NULL;
     *     conf->md5 = NULL;
     *     conf->orig_uri = NULL;
     *     conf->orig_args = NULL;
     */

    conf->expire_delta = NGX_CONF_UNSET_UINT;
    conf->init_missing_bits = NGX_CONF_UNSET_UINT;
    conf->min_missing_bits = NGX_CONF_UNSET_UINT;
    conf->max_missing_bits = NGX_CONF_UNSET_UINT;
    conf->burst_len = NGX_CONF_UNSET_UINT;
    conf->sleep_time = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_http_doorman_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_doorman_conf_t *prev = parent;
    ngx_http_doorman_conf_t *conf = child;

    if (conf->variable == NULL) {
        conf->variable = prev->variable;
    }

    if (conf->md5 == NULL) {
        conf->md5 = prev->md5;
    }

    if (conf->signature_key == NULL) {
        conf->signature_key = prev->signature_key;
    }

    ngx_conf_merge_str_value(conf->arg_key_name, prev->arg_key_name, "");
    ngx_conf_merge_str_value(conf->arg_expire_name, prev->arg_expire_name, "");
    ngx_conf_merge_uint_value(conf->expire_delta, prev->expire_delta, NGX_CONF_UNSET_UINT);
    ngx_conf_merge_uint_value(conf->init_missing_bits, prev->init_missing_bits, NGX_CONF_UNSET_UINT);
    ngx_conf_merge_uint_value(conf->min_missing_bits, prev->min_missing_bits, 0);
    ngx_conf_merge_uint_value(conf->max_missing_bits, prev->max_missing_bits, MAX_MISSING_BITS);
    ngx_conf_merge_uint_value(conf->burst_len, prev->burst_len, NGX_CONF_UNSET_UINT);
    ngx_conf_merge_uint_value(conf->sleep_time, prev->sleep_time, NGX_CONF_UNSET_UINT);

    if (conf->init_missing_bits != NGX_CONF_UNSET_UINT &&
        conf->init_missing_bits > MAX_MISSING_BITS) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "doorman_init_missing_bits is too big");
        return NGX_CONF_ERROR;
    }
    if (conf->min_missing_bits != NGX_CONF_UNSET_UINT) {
        if (conf->min_missing_bits > MAX_MISSING_BITS) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "doorman_min_missing_bits is too big");
            return NGX_CONF_ERROR;
        }
        if (conf->init_missing_bits != NGX_CONF_UNSET_UINT && conf->init_missing_bits < conf->min_missing_bits) {
            conf->init_missing_bits = conf->min_missing_bits;
        }
    }
    if (conf->max_missing_bits != NGX_CONF_UNSET_UINT) {
        if ( conf->max_missing_bits > MAX_MISSING_BITS) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "doorman_max_missing_bits is too big");
            return NGX_CONF_ERROR;
        }
        if (conf->init_missing_bits != NGX_CONF_UNSET_UINT && conf->init_missing_bits > conf->max_missing_bits) {
            conf->init_missing_bits = conf->max_missing_bits;
        }
    }
    if (conf->min_missing_bits != NGX_CONF_UNSET_UINT && conf->max_missing_bits != NGX_CONF_UNSET_UINT &&
        conf->min_missing_bits > conf->max_missing_bits) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "doorman_min_missing_bits > doorman_max_missing_bits");
        return NGX_CONF_ERROR;
    }

    num_missing_bits = (double) conf->init_missing_bits;
    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_doorman_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_result_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_result_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_expire_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_expire_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_orig_args_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_orig_args_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_orig_uri_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_orig_uri_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_trunc_hash_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_trunc_hash_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_meta_hash_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_meta_hash_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_missing_bits_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_missing_bits_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_request_length, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_request_length_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_burst_len_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_burst_len_variable;

    var = ngx_http_add_variable(cf, &ngx_http_doorman_sleep_time_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_http_doorman_sleep_time_variable;

    return NGX_OK;
}


