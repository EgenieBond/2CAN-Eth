/*
 * raw_tcp_client.c
 *
 * TCP RAW client for STM32
 * Задача:
 * - STM32 сама подключается к удалённому TCP server
 * - всё, что приходит от remote server, пересылается в local PC client,
 *   который подключён к TCP server STM32
 * - всё, что приходит от local PC client, raw_tcp_server.c пересылает сюда
 *
 * ВАЖНО:
 * В этом варианте УБРАНЫ автотестовые посылки stm_to_pc_001 / stm_to_pc_002 / ...
 * То есть клиент STM32 теперь только держит соединение и работает как часть моста.
 */

#include "raw_tcp_client.h"

#include "cmsis_os.h"
#include "debug_uart.h"
#include "raw_tcp_server.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/ip4_addr.h"

#include <string.h>
#include <stdint.h>

extern struct netif gnetif;

/*
 * IP/порт удалённого сервера, к которому STM32 подключается как клиент
 */
#define REMOTE_TCP_IP0    192
#define REMOTE_TCP_IP1    168
#define REMOTE_TCP_IP2    0
#define REMOTE_TCP_IP3    117

#define REMOTE_TCP_PORT   2001

#define TCP_CLIENT_TASK_STACK      4096
#define TCP_CLIENT_TASK_DELAY_MS   1000

static osThreadId_t g_rawTcpClientTaskHandle = NULL;

static struct tcp_pcb *g_client_pcb = NULL;
static volatile uint8_t g_client_connected = 0;
static volatile uint8_t g_connect_in_progress = 0;

/* ---------------- internal helpers ---------------- */

static void RawTcpClient_ResetState(void)
{
    g_client_pcb = NULL;
    g_client_connected = 0;
    g_connect_in_progress = 0;
}

static void RawTcpClient_ClosePcb(struct tcp_pcb *pcb)
{
    if (pcb == NULL)
        return;

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);

    err_t err = tcp_close(pcb);
    if (err != ERR_OK)
    {
        tcp_abort(pcb);
    }
}

static err_t raw_tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);

    DebugUART_Print("[TCP-CLI] sent acked=%u bytes\r\n", (unsigned)len);
    return ERR_OK;
}

static void raw_tcp_client_err(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    DebugUART_Print("[TCP-CLI] error cb err=%d\r\n", (int)err);
    RawTcpClient_ResetState();
}

static err_t raw_tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (p == NULL)
    {
        DebugUART_Print("[TCP-CLI] remote server disconnected\r\n");

        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);

        err_t close_err = tcp_close(tpcb);
        if (close_err != ERR_OK)
        {
            tcp_abort(tpcb);
        }

        RawTcpClient_ResetState();
        return ERR_OK;
    }

    if (err != ERR_OK)
    {
        DebugUART_Print("[TCP-CLI] recv err=%d\r\n", (int)err);
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        const uint8_t *data = (const uint8_t *)q->payload;
        const uint16_t len  = q->len;

        if ((data == NULL) || (len == 0U))
            continue;

        /*
         * Remote -> STM32 client -> local PC, подключённый к server STM32
         *
         * Мы уже внутри raw callback, то есть внутри tcpip_thread,
         * поэтому RawTcpServer_Send() можно вызывать напрямую.
         */
        int rc = RawTcpServer_Send(data, len);

        if (rc == 0)
        {
            DebugUART_Print("[TCP-CLI] REMOTE -> PC %u bytes OK\r\n", (unsigned)len);
        }
        else
        {
            DebugUART_Print("[TCP-CLI] REMOTE -> PC %u bytes FAIL rc=%d\r\n",
                            (unsigned)len, rc);
        }
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t raw_tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if ((err != ERR_OK) || (tpcb == NULL))
    {
        DebugUART_Print("[TCP-CLI] connect failed err=%d\r\n", (int)err);

        if (tpcb != NULL)
        {
            RawTcpClient_ClosePcb(tpcb);
        }

        RawTcpClient_ResetState();
        return err;
    }

    g_client_pcb = tpcb;
    g_client_connected = 1;
    g_connect_in_progress = 0;

    tcp_arg(tpcb, NULL);
    tcp_recv(tpcb, raw_tcp_client_recv);
    tcp_sent(tpcb, raw_tcp_client_sent);
    tcp_err(tpcb, raw_tcp_client_err);

    DebugUART_Print("[TCP-CLI] connected to server %d.%d.%d.%d:%d\r\n",
                    REMOTE_TCP_IP0, REMOTE_TCP_IP1, REMOTE_TCP_IP2, REMOTE_TCP_IP3,
                    REMOTE_TCP_PORT);

    return ERR_OK;
}

static void raw_tcp_client_connect_cb(void *arg)
{
    LWIP_UNUSED_ARG(arg);

    if (!netif_is_up(&gnetif) || !netif_is_link_up(&gnetif))
    {
        return;
    }

    if (g_client_connected || g_connect_in_progress || (g_client_pcb != NULL))
    {
        return;
    }

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL)
    {
        DebugUART_Print("[TCP-CLI] tcp_new_ip_type failed\r\n");
        return;
    }

    ip_addr_t remote_ip;
    IP_ADDR4(&remote_ip,
             REMOTE_TCP_IP0,
             REMOTE_TCP_IP1,
             REMOTE_TCP_IP2,
             REMOTE_TCP_IP3);

    g_client_pcb = pcb;
    g_connect_in_progress = 1;

    DebugUART_Print("[TCP-CLI] connecting to %d.%d.%d.%d:%d ...\r\n",
                    REMOTE_TCP_IP0, REMOTE_TCP_IP1, REMOTE_TCP_IP2, REMOTE_TCP_IP3,
                    REMOTE_TCP_PORT);

    err_t err = tcp_connect(pcb, &remote_ip, REMOTE_TCP_PORT, raw_tcp_client_connected);

    DebugUART_Print("[TCP-CLI] tcp_connect returned err=%d\r\n", (int)err);

    if (err != ERR_OK)
    {
        RawTcpClient_ClosePcb(pcb);
        RawTcpClient_ResetState();
    }
}

static void RawTcpClientTask(void *argument)
{
    (void)argument;

    DebugUART_Print("[TCP-CLI] task started\r\n");

    for (;;)
    {
        if (!g_client_connected && !g_connect_in_progress)
        {
            if (netif_is_up(&gnetif) && netif_is_link_up(&gnetif))
            {
                err_t cb_err = tcpip_callback(raw_tcp_client_connect_cb, NULL);
                if (cb_err != ERR_OK)
                {
                    DebugUART_Print("[TCP-CLI] tcpip_callback(connect) err=%d\r\n", (int)cb_err);
                }
            }
        }

        osDelay(TCP_CLIENT_TASK_DELAY_MS);
    }
}

/* ---------------- public API ---------------- */

void RawTcpClientTask_Start(void)
{
    const osThreadAttr_t attr = {
        .name = "RawTcpClient",
        .stack_size = TCP_CLIENT_TASK_STACK,
        .priority = (osPriority_t)osPriorityNormal
    };

    g_rawTcpClientTaskHandle = osThreadNew(RawTcpClientTask, NULL, &attr);

    if (g_rawTcpClientTaskHandle == NULL)
    {
        DebugUART_Print("[TCP-CLI] ERROR: task create failed\r\n");
    }
    else
    {
        DebugUART_Print("[TCP-CLI] task created\r\n");
    }
}

int RawTcpClient_IsConnected(void)
{
    return (g_client_connected != 0U) ? 1 : 0;
}

int RawTcpClient_Send(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))
        return -1;

    if ((g_client_pcb == NULL) || (g_client_connected == 0U))
        return -2;

    err_t wr = tcp_write(g_client_pcb, data, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (wr != ERR_OK)
    {
        DebugUART_Print("[TCP-CLI] tcp_write err=%d\r\n", (int)wr);
        return -3;
    }

    err_t out = tcp_output(g_client_pcb);
    if (out != ERR_OK)
    {
        DebugUART_Print("[TCP-CLI] tcp_output err=%d\r\n", (int)out);
        return -4;
    }

    DebugUART_Print("[TCP-CLI] TX %u bytes\r\n", (unsigned)len);
    return 0;
}
