/*
 * echo-zephyr — a Contiki-NG node talking to Zephyr's stock echo_server
 * (samples/net/sockets/echo_server with overlay-802154.conf) over IEEE
 * 802.15.4 and 6LoWPAN.
 *
 * Zephyr's sample has fixed addresses (it is 2001:db8::1, its client
 * 2001:db8::2) and no routing protocol, so this node takes the client's
 * address and resolves the server with plain IPv6 Neighbor Discovery.  Every
 * two seconds it sends a datagram to port 4242, growing from 16 bytes (one
 * 802.15.4 frame) to 640 (six 6LoWPAN fragments each way), checks that the
 * same bytes come back and prints the round-trip time.
 *
 * Datagrams to the server go out as link-layer broadcast: the neighbor
 * cache keeps one IPv6 address per MAC, Zephyr's link-local one takes it,
 * and the entry for 2001:db8::1 is left without one.  The example was
 * written when the emulated Zephyr radio sent no 802.15.4 ACKs (up to
 * 0.4.0, docs/zephyr.md); Zephyr nodes acknowledge frames now, so with
 * UIP_DS6_NBR_CONF_MULTI_IPV6_ADDRS the frames would go out unicast and be
 * acknowledged.  Broadcast is kept: it needs neither.
 */
#include "contiki.h"
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uiplib.h"
#include "sys/rtimer.h"

#include <stdio.h>
#include <string.h>

#define ECHO_PORT  4242
#define LOCAL_PORT 4243
#define INTERVAL   (2 * CLOCK_SECOND)
#define TIMEOUT_MS 1500

static const uint16_t sizes[] = { 16, 64, 128, 256, 384, 512, 640 };
#define NSIZES (sizeof(sizes) / sizeof(sizes[0]))

static struct simple_udp_connection conn;
static uip_ipaddr_t server;
static uint8_t msg[640];
static uint16_t sent_len;
static rtimer_clock_t sent_at;
static bool waiting;
static unsigned round_ok, round_no = 1, idx;

PROCESS(echo_zephyr_process, "echo-zephyr");
AUTOSTART_PROCESSES(&echo_zephyr_process);

/* A short sequence-number header, then a letter pattern.  The header stays
 * well under the smallest size, so every datagram, 16 B included, carries
 * pattern bytes for the echo check to compare. */
static void
fill(uint16_t len, unsigned seq)
{
  int n = snprintf((char *)msg, sizeof(msg), "#%u ", seq);
  for(uint16_t i = n; i < len; i++) {
    msg[i] = 'a' + (i % 26);
  }
}

static unsigned
rtt_tenths_ms(void)
{
  return (unsigned)(((uint32_t)(RTIMER_NOW() - sent_at) * 10000UL) /
                    RTIMER_SECOND);
}

static void
next_size(bool ok)
{
  round_ok += ok;
  if(++idx == NSIZES) {
    printf("round %u: %u/%u sizes echoed by Zephyr\n",
           round_no, round_ok, (unsigned)NSIZES);
    idx = 0;
    round_ok = 0;
    round_no++;
  }
}

static void
rx(struct simple_udp_connection *c, const uip_ipaddr_t *from,
   uint16_t from_port, const uip_ipaddr_t *to, uint16_t to_port,
   const uint8_t *data, uint16_t len)
{
  if(!waiting) {
    return;
  }
  unsigned rtt = rtt_tenths_ms();
  waiting = false;
  bool ok = len == sent_len && memcmp(data, msg, len) == 0;
  printf("echo %3u B: %s, rtt %u.%u ms\n", sent_len,
         ok ? "ok" : "MISMATCH", rtt / 10, rtt % 10);
  next_size(ok);
}

PROCESS_THREAD(echo_zephyr_process, ev, data)
{
  static struct etimer et;
  static unsigned seq;
  uip_ipaddr_t addr;

  PROCESS_BEGIN();

  /* Zephyr's addresses: the server 2001:db8::1, this node the client's
   * 2001:db8::2, the /64 on-link so the server is resolved with ND. */
  uip_ip6addr(&addr, 0x2001, 0xdb8, 0, 0, 0, 0, 0, 0);
#if UIP_CONF_ROUTER
  uip_ds6_prefix_add(&addr, 64, 0, UIP_ND6_RA_FLAG_ONLINK, 0, 0);
#else
  uip_ds6_prefix_add(&addr, 64, 0);
#endif
  uip_ip6addr(&addr, 0x2001, 0xdb8, 0, 0, 0, 0, 0, 2);
  uip_ds6_addr_add(&addr, 0, ADDR_MANUAL);
  uip_ip6addr(&server, 0x2001, 0xdb8, 0, 0, 0, 0, 0, 1);

  simple_udp_register(&conn, LOCAL_PORT, NULL, ECHO_PORT, rx);
  printf("echo-zephyr: 2001:db8::2 -> Zephyr echo_server [2001:db8::1]:%u\n",
         ECHO_PORT);

  /* Let both nodes finish duplicate address detection, then send one
   * datagram that only starts Neighbor Discovery (the packet that triggers
   * a resolution is dropped) before the measured rounds. */
  etimer_set(&et, 3 * CLOCK_SECOND);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
  printf("resolving 2001:db8::1 with Neighbor Discovery\n");
  simple_udp_sendto(&conn, "nd", 2, &server);
  etimer_set(&et, CLOCK_SECOND);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

  etimer_set(&et, INTERVAL);
  while(1) {
    if(waiting) {
      printf("echo %3u B: no reply within %u ms\n", sent_len, TIMEOUT_MS);
      waiting = false;
      next_size(false);
    }
    sent_len = sizes[idx];
    fill(sent_len, ++seq);
    sent_at = RTIMER_NOW();
    waiting = true;
    simple_udp_sendto(&conn, msg, sent_len, &server);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    etimer_reset(&et);
  }

  PROCESS_END();
}
