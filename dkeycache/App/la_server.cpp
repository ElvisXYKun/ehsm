/*
 * Copyright (C) 2011-2020 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
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

#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/un.h>

#include "la_server.h"
#include "ulog_utils.h"
#include "enclave_u.h"
#include "datatypes.h"

#define BACKLOG 5
#define CONCURRENT_MAX 32
#define SERVER_PORT 8888
#define BUFFER_SIZE 1024
#define RECONNECT_TIMES 3
#define SLEEP_INTV 3

extern bool g_is_ready;
extern FdPool g_client_resrved_fds[CONCURRENT_MAX];

/* Function Description:
 * This is server initialization routine, it creates TCP sockets and listen on a port.
 * In Linux, it would listen on domain socket named '/var/run/ehsm/dkeyprovision.sock'
 * In Windows, it would listen on port 8888, which is for demonstration purpose
 * */

static void *HeartbeatToClientHandler(void *args)
{
    int bytes_written = 0;
    _response_header_t *heart_beat = nullptr;
    heart_beat = (_response_header_t *)malloc(sizeof(_response_header_t));
    if (heart_beat == nullptr)
    {
        log_d("getDomainkey malloc failed\n");
        goto out;
    }
    heart_beat->type = MSG_HEARTBEAT;
    while (true)
    {
        ocall_sleep(SLEEP_INTV);
        for (int i = 0; i < CONCURRENT_MAX; i++)
        {
            if (g_client_resrved_fds[i].fd != 0)
            {
                if (g_client_resrved_fds[i].errorCnt < RECONNECT_TIMES)
                {
                    bytes_written = send(g_client_resrved_fds[i].fd, reinterpret_cast<char *>(heart_beat), sizeof(_response_header_t), MSG_NOSIGNAL);
                    if (bytes_written<= 0)
                    {
                        g_client_resrved_fds[i].errorCnt++;
                    }
                    else
                    {
                        g_client_resrved_fds[i].errorCnt = 0;
                    }
                }
                else
                {
                    close(g_client_resrved_fds[i].fd);
                    g_client_resrved_fds[i].fd = 0;
                    g_client_resrved_fds[i].errorCnt = 0;
                }
            }
        }
    }
out:
    SAFE_FREE(heart_beat);
    pthread_exit((void *)-1);
}

int LaServer::init()
{
    log_i("Initializing ProtocolHandler [\"socket: %s\"]", UNIX_DOMAIN);
    struct sockaddr_un srv_addr;

    m_server_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    m_server_resrved_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_server_sock_fd == -1)
    {
        log_d("socket initiazation error\n");
        return -1;
    }

    srv_addr.sun_family = AF_UNIX;
    strncpy(srv_addr.sun_path, UNIX_DOMAIN, sizeof(srv_addr.sun_path) - 1);
    unlink(UNIX_DOMAIN);

    int bind_result = bind(m_server_sock_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (bind_result == -1)
    {
        log_d("bind error\n");
        close(m_server_sock_fd);
        return -1;
    }

    bind_result = bind(m_server_resrved_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (bind_result == -1)
    {
        log_d("bind error\n");
        close(m_server_resrved_fd);
        return -1;
    }

    if (listen(m_server_sock_fd, BACKLOG) == -1)
    {
        log_d("listen error\n");
        close(m_server_sock_fd);
        return -1;
    }

    if (listen(m_server_resrved_fd, BACKLOG) == -1)
    {
        log_d("listen error\n");
        close(m_server_resrved_fd);
        return -1;
    }

    m_shutdown = 0;

    log_i("Starting ProtocolHandler [\"socket: %s\"]", UNIX_DOMAIN);

    return 0;
}

/* Function Description:
 * This function is server's major routine, it uses select() to accept new connection and receive messages from clients.
 * When it receives clients' request messages, it would put the message to task queue and wake up worker thread to process the requests.
 * */
void LaServer::doWork()
{
    int client_fds[CONCURRENT_MAX] = {0};
    fd_set server_fd_set;
    int max_fd = -1;
    struct timeval tv;
    char input_msg[BUFFER_SIZE];
    char recv_msg[BUFFER_SIZE];
    pthread_t HeartbeatToClient_thread;
    if (pthread_create(&HeartbeatToClient_thread, NULL, HeartbeatToClientHandler, NULL) < 0)
    {
        log_d("could not create thread\n");
        // error handler
    }
    while (!m_shutdown)
    {
        // set 20s timeout for select()
        tv.tv_sec = 20;
        tv.tv_usec = 0;
        FD_ZERO(&server_fd_set);

        FD_SET(STDIN_FILENO, &server_fd_set);
        if (max_fd < STDIN_FILENO)
            max_fd = STDIN_FILENO;

        // listening on server socket
        FD_SET(m_server_sock_fd, &server_fd_set);
        if (max_fd < m_server_sock_fd)
            max_fd = m_server_sock_fd;

        FD_SET(m_server_resrved_fd, &server_fd_set);
        if (max_fd < m_server_resrved_fd)
            max_fd = m_server_resrved_fd;

        // listening on all client connections
        for (int i = 0; i < CONCURRENT_MAX; i++)
        {
            if (client_fds[i] != 0)
            {
                FD_SET(client_fds[i], &server_fd_set);
                if (max_fd < client_fds[i])
                    max_fd = client_fds[i];
            }
        }

        int ret = select(max_fd + 1, &server_fd_set, NULL, NULL, &tv);
        if (ret < 0)
        {
            log_i("Warning: server would shutdown\n");
            continue;
        }
        else if (ret == 0)
        {
            // timeout
            continue;
        }

        if (FD_ISSET(m_server_sock_fd, &server_fd_set))
        {
            // if there is new connection request
            struct sockaddr_un clt_addr;
            socklen_t len = sizeof(clt_addr);

            // accept this connection request
            int client_sock_fd = accept(m_server_sock_fd, (struct sockaddr *)&clt_addr, &len);
            if (g_is_ready == false)
            {
                close(client_sock_fd);
                client_sock_fd = 0;
            }

            if (client_sock_fd > 0)
            {
                // add new connection to connection pool if it's not full
                int index = -1;
                for (int i = 0; i < CONCURRENT_MAX; i++)
                {
                    if (client_fds[i] == 0)
                    {
                        index = i;
                        client_fds[i] = client_sock_fd;
                        break;
                    }
                }

                if (index < 0)
                {
                    log_i("server reach maximum connection!\n");
                    bzero(input_msg, BUFFER_SIZE);
                    strcpy(input_msg, "server reach maximum connection\n");
                    send(client_sock_fd, input_msg, BUFFER_SIZE, 0);
                }
            }
            else if (client_sock_fd < 0)
            {
                log_d("server: accept() return failure, %s, would exit.\n", strerror(errno));
                close(m_server_sock_fd);
                break;
            }
        }

        if (FD_ISSET(m_server_resrved_fd, &server_fd_set))
        {
            // if there is new connection request
            struct sockaddr_un clt_addr;
            socklen_t len = sizeof(clt_addr);

            // accept this connection request
            int client_sock_fd = accept(m_server_resrved_fd, (struct sockaddr *)&clt_addr, &len);
            if (g_is_ready == false)
            {
                close(client_sock_fd);
                client_sock_fd = 0;
            }

            if (client_sock_fd > 0)
            {
                // add new connection to connection pool if it's not full
                int index = -1;
                for (int i = 0; i < CONCURRENT_MAX; i++)
                {
                    if (g_client_resrved_fds[i].fd == 0)
                    {
                        index = i;
                        g_client_resrved_fds[i].fd = client_sock_fd;
                        break;
                    }
                }

                if (index < 0)
                {
                    log_i("server reach maximum connection!\n");
                    bzero(input_msg, BUFFER_SIZE);
                    strcpy(input_msg, "server reach maximum connection\n");
                    send(client_sock_fd, input_msg, BUFFER_SIZE, 0);
                }
            }
            else if (client_sock_fd < 0)
            {
                log_d("server: accept() return failure, %s, would exit.\n", strerror(errno));
                close(m_server_resrved_fd);
                break;
            }
        }

        for (int i = 0; i < CONCURRENT_MAX; i++)
        {
            if ((client_fds[i] != 0) && (FD_ISSET(client_fds[i], &server_fd_set)))
            {
                // there is request messages from client connectsions
                FIFO_MSG *msg;

                bzero(recv_msg, BUFFER_SIZE);
                long byte_num = recv(client_fds[i], recv_msg, BUFFER_SIZE, 0);
                if (byte_num > 0)
                {
                    if (byte_num > BUFFER_SIZE)
                        byte_num = BUFFER_SIZE;

                    recv_msg[byte_num] = '\0';

                    msg = (FIFO_MSG *)malloc(byte_num);
                    if (!msg)
                    {
                        log_e("memory allocation failure\n");
                        continue;
                    }
                    memset(msg, 0, byte_num);

                    memcpy(msg, recv_msg, byte_num);

                    msg->header.sockfd = client_fds[i];

                    // put request message to event queue
                    m_cptask->puttask(msg);
                }
                else if (byte_num < 0)
                {
                    log_d("failed to receive message.\n");
                }
                else
                {
                    // client connect is closed
                    FD_CLR(client_fds[i], &server_fd_set);
                    close(client_fds[i]);
                    client_fds[i] = 0;
                }
            }
        }
    }
}

/* Function Description:
 * This function is to shutdown server. It's called when process exits.
 * */
void LaServer::shutDown()
{
    log_i("Server would shutdown...\n");
    m_shutdown = 1;
    m_cptask->shutdown();

    close(m_server_sock_fd);
}
