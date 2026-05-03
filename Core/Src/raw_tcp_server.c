/*
 * raw_tcp_server.c
 *
 * TCP RAW server (single client)
 */

#include "raw_tcp_server.h"
#include "client_handler.h"
#include "lwip/tcp.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"
#include "debug_uart.h"
#include <string.h>
#include <stdint.h>

#define TCP_SERVER_PORT 2001
#define RAW_TCP_ASYNC_TX_MAX_LEN 128

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *client_pcb = NULL;

/* =========================
 * Async TX context
 * ========================= */
typedef struct
{
    uint16_t len;
    uint8_t  data[RAW_TCP_ASYNC_TX_MAX_LEN];
} raw_tcp_async_tx_t;

static raw_tcp_async_tx_t g_async_tx;

/* ===== CALLBACKS ===== */

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);
    return ERR_OK;
}

static void tcp_server_error(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    DebugUART_Print("[TCP] ERROR cb err=%d\r\n", (int)err);
    client_pcb = NULL;
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (p == NULL)
    {
        DebugUART_Print("[TCP] Client disconnected (p==NULL)\r\n");

        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);

        err_t close_err = tcp_close(tpcb);
        if (close_err != ERR_OK)
        {
            DebugUART_Print("[TCP] tcp_close err=%d -> abort\r\n", (int)close_err);
            tcp_abort(tpcb);
        }

        client_pcb = NULL;
        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] RECV err=%d -> drop\r\n", (int)err);
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        const uint8_t *data = (const uint8_t *)q->payload;
        const uint16_t len  = q->len;

        //DebugUART_Print("[TCP] RX chunk len=%u\r\n", (unsigned)len);

        /* Передаем байты в pipeline */
        ClientHandler_InputBytes(data, len);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK || newpcb == NULL)
    {
        DebugUART_Print("[TCP] ACCEPT err=%d newpcb=%p\r\n", (int)err, (void*)newpcb);
        return ERR_VAL;
    }

    if (client_pcb != NULL)
    {
        DebugUART_Print("[TCP] Reject 2nd client\r\n");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    client_pcb = newpcb;

    DebugUART_Print("[TCP] ACCEPT from %d.%d.%d.%d:%u\r\n",
                    ip4_addr1(&newpcb->remote_ip),
                    ip4_addr2(&newpcb->remote_ip),
                    ip4_addr3(&newpcb->remote_ip),
                    ip4_addr4(&newpcb->remote_ip),
                    (unsigned)newpcb->remote_port);

    tcp_nagle_disable(newpcb);

    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, tcp_server_recv);
    tcp_err(newpcb, tcp_server_error);
    tcp_sent(newpcb, tcp_server_sent);

    return ERR_OK;
}

/* ===== INTERNAL SEND IN TCPIP THREAD ===== */

static void raw_tcp_send_cb(void *arg)
{
    raw_tcp_async_tx_t *ctx = (raw_tcp_async_tx_t *)arg;
    if (ctx == NULL)
        return;

    (void)RawTcpServer_Send(ctx->data, ctx->len);
}

/* ===== PUBLIC API ===== */

int RawTcpServer_HasClient(void)
{
    return (client_pcb != NULL) ? 1 : 0;
}

/*
 * IMPORTANT:
 * This function must be called only from tcpip_thread or raw callbacks.
 */
int RawTcpServer_Send(const uint8_t *data, size_t len)
{
    err_t wr;
    err_t out;

    if ((data == NULL) || (len == 0U))
        return -1;

    if (client_pcb == NULL)
    {
        DebugUART_Print("[TCP] SEND skipped: no active client\r\n");
        return -2;
    }

    wr = tcp_write(client_pcb, data, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (wr != ERR_OK)
    {
        if (wr == ERR_MEM)
            DebugUART_Print("[TCP] tcp_write ERR_MEM\r\n");
        else
            DebugUART_Print("[TCP] tcp_write err=%d\r\n", (int)wr);
        return -3;
    }

    out = tcp_output(client_pcb);
    if (out != ERR_OK)
    {
        DebugUART_Print("[TCP] tcp_output err=%d\r\n", (int)out);
        return -4;
    }

    return 0;
}

int RawTcpServer_SendAsync(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))
        return -1;

    if (len > RAW_TCP_ASYNC_TX_MAX_LEN)
    {
        DebugUART_Print("[TCP] Async send too long: %u > %u\r\n",
                        (unsigned)len,
                        (unsigned)RAW_TCP_ASYNC_TX_MAX_LEN);
        return -2;
    }

    if (client_pcb == NULL)
    {
        DebugUART_Print("[TCP] Async send skipped: no active client\r\n");
        return -3;
    }

    memcpy(g_async_tx.data, data, len);
    g_async_tx.len = (uint16_t)len;

    err_t cb_err = tcpip_callback(raw_tcp_send_cb, &g_async_tx);
    if (cb_err != ERR_OK)
    {
        DebugUART_Print("[TCP] tcpip_callback(send) err=%d\r\n", (int)cb_err);
        return -4;
    }

    return 0;
}

/* ===== INIT ===== */
void RawTcpServer_Init(void)
{
    if (server_pcb != NULL)
    {
        DebugUART_Print("[TCP] previous server pcb exists\r\n");
        return;
    }

    server_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!server_pcb)
    {
        DebugUART_Print("[TCP] tcp_new_ip_type failed\r\n");
        return;
    }

    err_t err = tcp_bind(server_pcb, IP_ANY_TYPE, TCP_SERVER_PORT);
    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP] tcp_bind failed err=%d\r\n", (int)err);
        tcp_close(server_pcb);
        server_pcb = NULL;
        return;
    }

    err_t err2 = ERR_OK;
    server_pcb = tcp_listen_with_backlog_and_err(server_pcb, 1, &err2);
    if (!server_pcb)
    {
        DebugUART_Print("[TCP] tcp_listen failed err=%d\r\n", (int)err2);
        return;
    }

    tcp_accept(server_pcb, tcp_server_accept);
    DebugUART_Print("[TCP] Listening on port %d\r\n", TCP_SERVER_PORT);
}
