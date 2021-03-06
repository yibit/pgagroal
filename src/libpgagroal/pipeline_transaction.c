/*
 * Copyright (C) 2020 Red Hat
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgagroal */
#include <pgagroal.h>
#include <management.h>
#include <message.h>
#include <network.h>
#include <pipeline.h>
#include <pool.h>
#include <prometheus.h>
#include <server.h>
#include <shmem.h>
#include <worker.h>
#include <utils.h>

#define ZF_LOG_TAG "pipeline_transaction"
#include <zf_log.h>

/* system */
#include <errno.h>
#include <ev.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

static int  transaction_initialize(void*, void**, size_t*);
static void transaction_start(struct ev_loop *loop, struct worker_io*);
static void transaction_client(struct ev_loop *loop, struct ev_io *watcher, int revents);
static void transaction_server(struct ev_loop *loop, struct ev_io *watcher, int revents);
static void transaction_stop(struct ev_loop *loop, struct worker_io*);
static void transaction_destroy(void*, size_t);
static void transaction_periodic(void*, void*);

static void start_mgt(struct ev_loop *loop);
static void shutdown_mgt(struct ev_loop *loop, void* shmem);
static void accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents);

static int slot;
static char username[MAX_USERNAME_LENGTH];
static char database[MAX_DATABASE_LENGTH];
static bool in_tx;
static int next_message;
static int unix_socket = -1;
static struct ev_io io_mgt;
static struct worker_io server_io;

struct pipeline transaction_pipeline()
{
   struct pipeline pipeline;

   pipeline.initialize = &transaction_initialize;
   pipeline.start = &transaction_start;
   pipeline.client = &transaction_client;
   pipeline.server = &transaction_server;
   pipeline.stop = &transaction_stop;
   pipeline.destroy = &transaction_destroy;
   pipeline.periodic = &transaction_periodic;

   return pipeline;
}

static int
transaction_initialize(void* shmem, void** pipeline_shmem, size_t* pipeline_shmem_size)
{
   return 0;
}

static void
transaction_start(struct ev_loop *loop, struct worker_io* w)
{
   char p[MISC_LENGTH];
   bool is_new;
   struct configuration* config = NULL;

   config = (struct configuration*)w->shmem;

   slot = -1;
   memcpy(&username[0], config->connections[w->slot].username, MAX_USERNAME_LENGTH);
   memcpy(&database[0], config->connections[w->slot].database, MAX_DATABASE_LENGTH);
   in_tx = false;
   next_message = 0;

   memset(&p, 0, sizeof(p));
   snprintf(&p[0], sizeof(p), ".s.%d", getpid());

   if (pgagroal_bind_unix_socket(config->unix_socket_dir, &p[0], w->shmem, &unix_socket))
   {
      ZF_LOGF("pgagroal: Could not bind to %s/%s\n", config->unix_socket_dir, &p[0]);
      goto error;
   }

   start_mgt(loop);

   is_new = config->connections[w->slot].new;
   pgagroal_return_connection(w->shmem, w->slot, true);

   w->server_fd = -1;
   w->slot = -1;

   if (is_new)
   {
      /* Sleep for 5ms */
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 5000000L;
      nanosleep(&ts, NULL);
   }

   return;

error:

   exit_code = WORKER_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;
}

static void
transaction_stop(struct ev_loop *loop, struct worker_io* w)
{
   if (slot != -1)
   {
      struct configuration* config = NULL;

      config = (struct configuration*)w->shmem;

      /* We are either in 'X' or the client terminated (consider cancel query) */
      if (in_tx)
      {
         /* ROLLBACK */
         pgagroal_write_rollback(NULL, config->connections[slot].fd);
      }

      ev_io_stop(loop, (struct ev_io*)&server_io);
      pgagroal_return_connection(w->shmem, slot, true);
      slot = -1;
   }

   shutdown_mgt(loop, w->shmem);
}

static void
transaction_destroy(void* pipeline_shmem, size_t pipeline_shmem_size)
{
}

static void
transaction_periodic(void* shmem, void* pipeline_shmem)
{
}

static void
transaction_client(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
   int status = MESSAGE_STATUS_ERROR;
   struct worker_io* wi = NULL;
   struct message* msg = NULL;
   struct configuration* config = NULL;

   wi = (struct worker_io*)watcher;
   config = (struct configuration*)wi->shmem;

   /* We can't use the information from wi except from client_fd/client_ssl */
   if (slot == -1)
   {
      if (pgagroal_get_connection(wi->shmem, &username[0], &database[0], true, true, &slot))
      {
         pgagroal_write_pool_full(wi->client_ssl, wi->client_fd);
         goto get_error;
      }

      wi->server_fd = config->connections[slot].fd;
      wi->slot = slot;

      ev_io_init((struct ev_io*)&server_io, transaction_server, config->connections[slot].fd, EV_READ);
      server_io.client_fd = wi->client_fd;
      server_io.server_fd = config->connections[slot].fd;
      server_io.slot = slot;
      server_io.client_ssl = wi->client_ssl;
      server_io.shmem = wi->shmem;
      server_io.pipeline_shmem = wi->pipeline_shmem;

      ev_io_start(loop, (struct ev_io*)&server_io);
   }

   if (wi->client_ssl == NULL)
   {
      status = pgagroal_read_socket_message(wi->client_fd, &msg);
   }
   else
   {
      status = pgagroal_read_ssl_message(wi->client_ssl, &msg);
   }
   if (likely(status == MESSAGE_STATUS_OK))
   {
      if (likely(msg->kind != 'X'))
      {
         status = pgagroal_write_socket_message(wi->server_fd, msg);
         if (unlikely(status == MESSAGE_STATUS_ERROR))
         {
            if (config->failover)
            {
               pgagroal_server_failover(config, slot);
               pgagroal_write_client_failover(wi->client_ssl, wi->client_fd);
               pgagroal_prometheus_failed_servers(config);

               goto failover;
            }
            else
            {
               goto server_error;
            }
         }
      }
      else if (msg->kind == 'X')
      {
         exit_code = WORKER_SUCCESS;
         running = 0;
      }
   }
   else
   {
      goto client_error;
   }

   ev_break(loop, EVBREAK_ONE);
   return;

client_error:
   ZF_LOGW("[C] Client error: %s (slot %d socket %d status %d)", strerror(errno), slot, wi->client_fd, status);
   errno = 0;

   exit_code = WORKER_CLIENT_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;

server_error:
   ZF_LOGW("[C] Server error: %s (slot %d socket %d status %d)", strerror(errno), slot, wi->server_fd, status);
   errno = 0;

   exit_code = WORKER_SERVER_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;

failover:

   exit_code = WORKER_FAILOVER;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;

get_error:
   ZF_LOGW("Failure during obtaining connection");

   exit_code = WORKER_SERVER_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;
}

static void
transaction_server(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
   int status = MESSAGE_STATUS_ERROR;
   bool fatal = false;
   struct worker_io* wi = NULL;
   struct message* msg = NULL;
   struct configuration* config = NULL;

   wi = (struct worker_io*)watcher;
   config = (struct configuration*)wi->shmem;

   /* We can't use the information from wi except from client_fd/client_ssl */
   wi->server_fd = config->connections[slot].fd;
   wi->slot = slot;

   if (!pgagroal_socket_isvalid(wi->client_fd))
   {
      goto client_error;
   }

   status = pgagroal_read_socket_message(wi->server_fd, &msg);
   if (likely(status == MESSAGE_STATUS_OK))
   {
      int offset = 0;

      while (offset < msg->length)
      {
         if (next_message == 0)
         {
            char kind = pgagroal_read_byte(msg->data + offset);
            int length = pgagroal_read_int32(msg->data + offset + 1);

            /* The Z message tell us the transaction state */
            if (kind == 'Z')
            {
               char tx_state = pgagroal_read_byte(msg->data + offset + 5);
               if (tx_state == 'I')
               {
                  in_tx = false;
               }
               else
               {
                  in_tx = true;
               }
            }

            /* Calculate the offset to the next message */
            if (offset + length + 1 <= msg->length)
            {
               next_message = 0;
               offset += length + 1;
            }
            else
            {
               next_message = length + 1 - (msg->length - offset);
               offset = msg->length;
            }
         }
         else
         {
            offset = MIN(next_message, msg->length);
            next_message -= offset;
         }
      }

      if (wi->client_ssl == NULL)
      {
         status = pgagroal_write_socket_message(wi->client_fd, msg);
      }
      else
      {
         status = pgagroal_write_ssl_message(wi->client_ssl, msg);
      }
      if (unlikely(status != MESSAGE_STATUS_OK))
      {
         goto client_error;
      }

      if (likely(msg->kind != 'E'))
      {
         if (!in_tx && slot != -1)
         {
            ev_io_stop(loop, (struct ev_io*)&server_io);

            if (pgagroal_return_connection(wi->shmem, slot, true))
            {
               goto return_error;
            }

            slot = -1;
         }
      }
      else
      {
         fatal = false;

         ev_io_stop(loop, (struct ev_io*)&server_io);

         if (!strncmp(msg->data + 6, "FATAL", 5) || !strncmp(msg->data + 6, "PANIC", 5))
            fatal = true;

         if (fatal)
         {
            exit_code = WORKER_SERVER_FATAL;
            running = 0;
         }
      }
   }
   else
   {
      goto server_error;
   }

   ev_break(loop, EVBREAK_ONE);
   return;

client_error:
   ZF_LOGW("[S] Client error: %s (slot %d socket %d status %d)", strerror(errno), slot, wi->client_fd, status);
   errno = 0;

   exit_code = WORKER_CLIENT_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;

server_error:
   ZF_LOGW("[S] Server error: %s (slot %d socket %d status %d)", strerror(errno), slot, wi->server_fd, status);
   errno = 0;

   exit_code = WORKER_SERVER_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;

return_error:
   ZF_LOGW("Failure during connection return");

   exit_code = WORKER_SERVER_FAILURE;
   running = 0;
   ev_break(loop, EVBREAK_ALL);
   return;
}

static void
start_mgt(struct ev_loop *loop)
{
   memset(&io_mgt, 0, sizeof(struct ev_io));
   ev_io_init(&io_mgt, accept_cb, unix_socket, EV_READ);
   ev_io_start(loop, &io_mgt);
}

static void
shutdown_mgt(struct ev_loop *loop, void* shmem)
{
   char p[MISC_LENGTH];
   struct configuration* config = NULL;

   config = (struct configuration*)shmem;

   memset(&p, 0, sizeof(p));
   snprintf(&p[0], sizeof(p), ".s.%d", getpid());

   ev_io_stop(loop, &io_mgt);
   pgagroal_disconnect(unix_socket);
   errno = 0;
   pgagroal_remove_unix_socket(config->unix_socket_dir, &p[0]);
   errno = 0;
}

static void
accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
   struct sockaddr_in client_addr;
   socklen_t client_addr_length;
   int client_fd;
   signed char id;
   int32_t slot;
   int payload_i;
   char* payload_s = NULL;

   ZF_LOGV("accept_cb: sockfd ready (%d)", revents);

   if (EV_ERROR & revents)
   {
      ZF_LOGD("accept_cb: invalid event: %s", strerror(errno));
      errno = 0;
      return;
   }

   client_addr_length = sizeof(client_addr);
   client_fd = accept(watcher->fd, (struct sockaddr *)&client_addr, &client_addr_length);
   if (client_fd == -1)
   {
      ZF_LOGD("accept: %s (%d)", strerror(errno), watcher->fd);
      errno = 0;
      return;
   }

   /* Process internal management request -- f.ex. returning a file descriptor to the pool */
   pgagroal_management_read_header(client_fd, &id, &slot);
   pgagroal_management_read_payload(client_fd, id, &payload_i, &payload_s);

   switch (id)
   {
      case MANAGEMENT_CLIENT_FD:
         ZF_LOGD("pgagroal: Management client file descriptor: Slot %d FD %d", slot, payload_i);
         break;
      default:
         ZF_LOGD("pgagroal: Unsupported management id: %d", id);
         break;
   }

   pgagroal_disconnect(client_fd);
}
