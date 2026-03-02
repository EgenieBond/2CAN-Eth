/*
 * raw_tcp_server.c
 *
 * TCP RAW server (single client)
 */

#include "lwip/tcp.h"
#include "lwip/inet.h"     // ip4_addr1/2/3/4 (на всякий случай)
#include "debug_uart.h"
#include <string.h>

#define TCP_SERVER_PORT 2001
#define RX_BUFFER_SIZE  256

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *client_pcb = NULL;

static char rx_buffer[RX_BUFFER_SIZE];
static int  rx_buffer_pos = 0;

/* ===== CALLBACKS ===== */

static err_t tcp_server_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(tpcb);

  DebugUART_Print("[TCP] CONNECTED cb err=%d\r\n", (int)err);
  return ERR_OK;
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(tpcb);

  DebugUART_Print("[TCP] SENT cb: acked=%u bytes\r\n", (unsigned)len);
  return ERR_OK;
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err)
{
  LWIP_UNUSED_ARG(arg);

  if (p == NULL)
  {
    DebugUART_Print("[TCP] Client disconnected (p==NULL)\r\n");
    tcp_close(tpcb);
    client_pcb = NULL;
    rx_buffer_pos = 0;
    return ERR_OK;
  }

  if (err != ERR_OK)
  {
    DebugUART_Print("[TCP] RECV err=%d -> drop\r\n", (int)err);
    pbuf_free(p);
    return err;
  }

  /* сообщаем стеку, что данные получены */
  tcp_recved(tpcb, p->tot_len);

  /* обрабатываем цепочку pbuf */
  for (struct pbuf *q = p; q != NULL; q = q->next)
  {
    const uint8_t *data = (const uint8_t *)q->payload;
    const uint16_t len  = q->len;

    for (uint16_t i = 0; i < len; i++)
    {
      if (rx_buffer_pos < (RX_BUFFER_SIZE - 1))
      {
        rx_buffer[rx_buffer_pos++] = (char)data[i];

        /* команда закончилась по CR */
        if (data[i] == '\r')
        {
          rx_buffer[rx_buffer_pos] = '\0';
          DebugUART_Print("[TCP] CMD: %s", rx_buffer);

          /* ЭХО (пока) */
          err_t wr = tcp_write(tpcb, rx_buffer, (u16_t)rx_buffer_pos, TCP_WRITE_FLAG_COPY);
          if (wr != ERR_OK)
          {
            DebugUART_Print("[TCP] tcp_write err=%d\r\n", (int)wr);
          }
          else
          {
            err_t out = tcp_output(tpcb);
            DebugUART_Print("[TCP] tcp_output err=%d\r\n", (int)out);
          }

          rx_buffer_pos = 0;
        }
      }
      else
      {
        DebugUART_Print("[TCP] RX overflow -> reset line buffer\r\n");
        rx_buffer_pos = 0;
        /* выходим из обоих циклов */
        q = NULL;
        break;
      }
    }
  }

  pbuf_free(p);
  return ERR_OK;
}

static void tcp_server_error(void *arg, err_t err)
{
  LWIP_UNUSED_ARG(arg);

  DebugUART_Print("[TCP] ERROR cb err=%d\r\n", (int)err);
  client_pcb = NULL;
  rx_buffer_pos = 0;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  LWIP_UNUSED_ARG(arg);

  if (err != ERR_OK || newpcb == NULL)
  {
    DebugUART_Print("[TCP] ACCEPT err=%d newpcb=%p\r\n", (int)err, (void*)newpcb);
    return ERR_VAL;
  }

  if (client_pcb)
  {
    DebugUART_Print("[TCP] Reject 2nd client\r\n");
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  client_pcb = newpcb;
  rx_buffer_pos = 0;

  /* SYN дошёл, стэк принял соединение */
  DebugUART_Print("[TCP] SYN->ACCEPT from %d.%d.%d.%d:%u\r\n",
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
  /* connected callback обычно для active-open, но оставим — иногда удобно */
  tcp_server_connected(NULL, newpcb, ERR_OK);

  /* приветствие */
  const char *welcome = "CAN-Ethernet Gateway ready. Use SLCAN commands.\r\n";
  err_t wr = tcp_write(newpcb, welcome, (u16_t)strlen(welcome), TCP_WRITE_FLAG_COPY);
  if (wr == ERR_OK)
  {
    tcp_output(newpcb);
  }
  else
  {
    DebugUART_Print("[TCP] welcome tcp_write err=%d\r\n", (int)wr);
  }

  return ERR_OK;
}

/* ===== INIT ===== */

void RawTcpServer_Init(void)
{
  DebugUART_Print("[TCP] RawTcpServer_Init enter\r\n");

  if (server_pcb)
  {
    DebugUART_Print("[TCP] closing previous server pcb\r\n");
    tcp_close(server_pcb);
    server_pcb = NULL;
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
  server_pcb = tcp_listen_with_backlog_and_err(server_pcb, 2, &err2);
  if (!server_pcb)
  {
    DebugUART_Print("[TCP] tcp_listen failed err=%d\r\n", (int)err2);
    return;
  }

  tcp_accept(server_pcb, tcp_server_accept);

  DebugUART_Print("[TCP] Listening on port %d\r\n", TCP_SERVER_PORT);
}
