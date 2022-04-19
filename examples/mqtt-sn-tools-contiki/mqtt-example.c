/*
  Basic MQTT-SN client library
  Copyright (C) 2013 Nicholas Humfrey

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

  Modifications:
  Copyright (C) 2013 Adam Renner
 */


#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "sys/etimer.h"
#include "sys/clock.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ip/uip-nameserver.h"
#include "mqtt-sn.h"
#include "rpl.h"
#include "simple-udp.h"

#include "sys/log.h"
#define LOG_MODULE "MQTT-SN"
#define LOG_LEVEL LOG_LEVEL_INFO

#define DEBUG LOG_INFO
#define LOG_INFO printf

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "dev/leds.h"

#define UDP_PORT 1883
#define USE_MDNS (1) //set to 1 and the code will try to resolve the address below, otherwise, you must set the connection address manually
#define BROKER_ADDRESS labscpi.eletrica.eng.br

#define REQUEST_RETRIES 4
#define DEFAULT_SEND_INTERVAL		(4 * CLOCK_SECOND)
#define REPLY_TIMEOUT (3 * CLOCK_SECOND)

static struct mqtt_sn_connection mqtt_sn_c;
static resolv_status_t set_connection_address(uip_ipaddr_t *ipaddr);

//those are the topic strings to be used below
static char upstream_topic[] = "0000000000000000/upstream\0";
static char control_topic[] = "0000000000000000/control\0";

//This is a list of All Topics that the code knows
static char* topics[] = {upstream_topic, control_topic};

//This is a list of All Topics that the code will subscribe to. If a topic is null, the code will only register (to get an id)
static char* subscribe_topics[] = {NULL, control_topic};

//this is an enum with illustrative names that makes it easy to use the topics
typedef enum {
	UPSTREAM,
	CONTROL,
	NUM_TOPICS
} mqtt_topics;

int16_t mqtt_topic_ids[NUM_TOPICS];
int16_t mqtt_msg_ids[NUM_TOPICS];

static char mqtt_client_id[17];

static publish_packet_t incoming_packet;

static uint16_t mqtt_keep_alive=10;
static int8_t qos = 1;
static uint8_t retain = FALSE;
static char device_id[17];
static clock_time_t send_interval;

static mqtt_sn_subscribe_request subreq;
static mqtt_sn_register_request regreq;

static uint32_t gRegAckEvent=-1;
static uint32_t gSubAckEvent=-1;

//uint8_t debug = FALSE;

static enum mqttsn_connection_status connection_state = MQTTSN_DISCONNECTED;

/*A few events for managing device state*/
static process_event_t mqttsn_connack_event;


PROCESS(publish_process, "Periodically publish in the upstream topic");
PROCESS(example_mqttsn_process, "Main process. Start others when its time");
PROCESS(registration_subscription_process, "Configure Connection and Topic Registration");

AUTOSTART_PROCESSES(&example_mqttsn_process);

/*---------------------------------------------------------------------------*/
static void
puback_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
	LOG_INFO("Message delivered successfully\n");
}
/*---------------------------------------------------------------------------*/
static void
connack_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
	uint8_t connack_return_code;
	connack_return_code = *(data + 3);
	LOG_INFO("Connack received\n");
	if (connack_return_code == ACCEPTED) {
		process_post(&example_mqttsn_process, mqttsn_connack_event, NULL);
	} else {
		LOG_INFO("Connack error: %s\n", mqtt_sn_return_code_string(connack_return_code));
	}
}
/*---------------------------------------------------------------------------*/
static void
regack_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
	regack_packet_t incoming_regack;
	uint32_t i=0;
	memcpy(&incoming_regack, data, datalen);
	LOG_INFO("Regack received: ");
	for(i=0;i<NUM_TOPICS;i++)
	{
		if(incoming_regack.message_id == mqtt_msg_ids[i])
		{
			if (incoming_regack.return_code == ACCEPTED)
			{
				mqtt_topic_ids[i] = uip_htons(incoming_regack.topic_id);
				LOG_INFO(" topic %s is registered as %d\n",topics[i], mqtt_topic_ids[i]);
				process_post(&registration_subscription_process, gRegAckEvent, 0);
				break;
			}
			else
			{
				LOG_INFO("Regack error: %s\n", mqtt_sn_return_code_string(incoming_regack.return_code));
			}
		}

	}
}
/*---------------------------------------------------------------------------*/
static void
suback_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
	suback_packet_t incoming_suback;
	uint32_t i=0;
	memcpy(&incoming_suback, data, datalen);
	LOG_INFO("Suback received: ");
	for(i=0;i<NUM_TOPICS;i++)
	{
		if(incoming_suback.message_id == mqtt_msg_ids[i])
		{
			if (incoming_suback.return_code == ACCEPTED)
			{
				mqtt_topic_ids[i] = uip_htons(incoming_suback.topic_id);
				LOG_INFO(" topic %s is subscribed as %d\n",topics[i], mqtt_topic_ids[i]);
				process_post(&registration_subscription_process, gSubAckEvent, 0);
				break;
			}
			else
			{
				LOG_INFO("Suback error: %s\n", mqtt_sn_return_code_string(incoming_suback.return_code));
			}
		}
	}
}

float stof(const char* s)
{
  float rez = 0, fact = 1;
  if (*s == '-'){
    s++;
    fact = -1;
  }
  for (int point_seen = 0; *s; s++){
    if (*s == '.'){
      point_seen = 1;
      continue;
    };
    int d = *s - '0';
    if (d >= 0 && d <= 9){
      if (point_seen) fact /= 10.0f;
      rez = rez * 10.0f + (float)d;
    }
  }
  return rez * fact;
}


static void
publish_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
	static uint8_t str[32];
	static uint32_t topic_id = 0;
	static int32_t topic_index=-1;
	static uint32_t i;



	//publish_packet_t* pkt = (publish_packet_t*)data;
	memcpy(&incoming_packet, data, datalen);
	incoming_packet.data[datalen-7] = 0x00;
	topic_id = uip_htons(incoming_packet.topic_id);
	LOG_INFO("Published message received (topic %d): %s\n", topic_id, incoming_packet.data);

	//find topic index
	for(i=0;i<NUM_TOPICS;i++)
	{
		if(topic_id == mqtt_topic_ids[i])
		{
			topic_index = i;
			break;
		}
	}

	switch (topic_index)
	{
	case UPSTREAM:
	{
		break;
	}
	case CONTROL:
	{
	    LOG_INFO("Control message (%s): %s\n", topics[CONTROL], incoming_packet.data);
		break;
	}
	default:
		break;
	}



}
/*---------------------------------------------------------------------------*/
static void
pingreq_receiver(struct mqtt_sn_connection *mqc, const uip_ipaddr_t *source_addr, const uint8_t *data, uint16_t datalen)
{
	LOG_INFO("PingReq received\n");
}
/*---------------------------------------------------------------------------*/
/*Add callbacks here if we make them*/
static const struct mqtt_sn_callbacks mqtt_sn_call = {
		publish_receiver,
		pingreq_receiver,
		NULL,
		connack_receiver,
		regack_receiver,
		puback_receiver,
		suback_receiver,
		NULL,
		NULL
};


/*---------------------------------------------------------------------------*/
/*this process will create a subscription and monitor for incoming traffic*/
PROCESS_THREAD(registration_subscription_process, ev, data)
{
	static uint32_t subscription_tries=0;
	static uint32_t registration_tries=0;
	static uint32_t timeout=0;
	static uint32_t i;
	static mqtt_sn_subscribe_request *req = &subreq;
	static struct etimer periodic_timer;
	PROCESS_BEGIN();

	for(i=0;i<NUM_TOPICS;i++)
	{
		mqtt_topic_ids[i] = -1;
		mqtt_msg_ids[i] = -1;
	}


	//registration
	send_interval = DEFAULT_SEND_INTERVAL;

	LOG_INFO("requesting registrations and subscriptions...\n");
	for(i=0; i < NUM_TOPICS;i++)
	{
		timeout = 1;
		subscription_tries = 0;
		while(subscription_tries < REQUEST_RETRIES)
		{
			if(timeout)
			{
				timeout = 0;
				if(subscribe_topics[i]!=NULL)
				{
					LOG_INFO("subscribing... topic: %s\n", topics[i]);
					req = &subreq;
					mqtt_msg_ids[i] = mqtt_sn_subscribe_try(req,&mqtt_sn_c,topics[i],0,REPLY_TIMEOUT);
				}
				else
				{
					LOG_INFO("registering... topic: %s\n", topics[i]);
					req = &regreq;
					mqtt_msg_ids[i] = mqtt_sn_register_try(req,&mqtt_sn_c,topics[i],REPLY_TIMEOUT);
				}
			}
			etimer_set(&periodic_timer, 5*CLOCK_SECOND);
			PROCESS_WAIT_EVENT();

			if((ev == PROCESS_EVENT_TIMER) && (data == &periodic_timer))
			{
				timeout = 1;
				subscription_tries++;
				if (req->state == MQTTSN_REQUEST_FAILED) {
					LOG_INFO("Regak/Suback error: %s\n", mqtt_sn_return_code_string(req->return_code));
				}
			}
			else if(ev == gSubAckEvent)
			{
				if (mqtt_sn_request_success(req)) {
					subscription_tries = 4;
					LOG_INFO("subscription acked\n");
				}
				else
				{
				    LOG_INFO("subscription nacked, error %d\n",req->state);

				}
			}
			else if(ev == gRegAckEvent)
			{
				if (mqtt_sn_request_success(req)) {
					subscription_tries = 4;
					LOG_INFO("registration acked\n");
				}
				else
				{
				    LOG_INFO("subscription nacked, error %d\n",req->state);
				}
			}
		}
	}
	process_start(&publish_process, 0);
	PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*this main process will create connection and register topics*/
/*---------------------------------------------------------------------------*/


static struct ctimer connection_timer;
static process_event_t connection_timeout_event;

static void connection_timer_callback(void *mqc)
{
	process_post(&example_mqttsn_process, connection_timeout_event, NULL);
}

/*---------------------------------------------------------------------------*/


PROCESS_THREAD(example_mqttsn_process, ev, data)
{
	static struct etimer periodic_timer;
	static struct etimer et;
	static uip_ipaddr_t broker_addr,google_dns, utfpr_dns;
	static uint8_t connection_retries = 0;
	resolv_status_t status = RESOLV_STATUS_ERROR;
	char contiki_hostname[16];

	PROCESS_BEGIN();

	mqttsn_connack_event = process_alloc_event();
	gRegAckEvent = process_alloc_event();
	gSubAckEvent = process_alloc_event();

	mqtt_sn_set_debug(1);

	uip_ip6addr(&google_dns, 0x2001, 0x4860, 0x4860, 0x0, 0x0, 0x0, 0x0, 0x8888);
	uip_ip6addr(&utfpr_dns, 0x2801, 0x82, 0xc004, 0x1, 0x1ce, 0x1ce, 0xbabe, 0x1);

	etimer_set(&periodic_timer, 2*CLOCK_SECOND);
	while(uip_ds6_get_global(ADDR_PREFERRED) == NULL)
	{
		PROCESS_WAIT_EVENT();
		if(etimer_expired(&periodic_timer))
		{
			DEBUG("Waiting IPv6 Stateless Autoconfiguration...\n");
			etimer_set(&periodic_timer, 2*CLOCK_SECOND);
		}
	}

	rpl_dag_t *dag = rpl_get_any_dag();
#if USE_MDNS==1
	if(dag) {
		//uip_ipaddr_copy(sixlbr_addr, globaladdr);
	    //DEBUG("Using Google DNS -> 2001:4860:4860::8888 ...\n");
		//uip_nameserver_update(&google_dns, UIP_NAMESERVER_INFINITE_LIFETIME);

		DEBUG("Using UTFPR DNS -> 2801:82:c004:1:1ce:1ce:babe:1 ...\n");
		uip_nameserver_update(&utfpr_dns, UIP_NAMESERVER_INFINITE_LIFETIME);
	}

	status = RESOLV_STATUS_UNCACHED;
	while(status != RESOLV_STATUS_CACHED) {
	    status = set_connection_address(&broker_addr);

	    if(status == RESOLV_STATUS_RESOLVING) {
	        //PROCESS_WAIT_EVENT_UNTIL(ev == resolv_event_found);
	        PROCESS_WAIT_EVENT();
	    } else if(status != RESOLV_STATUS_CACHED) {
	        DEBUG("Can't get connection address.\n");
	        etimer_set(&periodic_timer, 2*CLOCK_SECOND);
	        PROCESS_WAIT_EVENT();
	    }
	}
#else
	DEBUG("Manually Connecting to fd00::1 ...\n");
	uip_ip6addr(&broker_addr, 0xfd00,0,0,0,0,0,0,1);
#endif

	mqtt_sn_create_socket(&mqtt_sn_c,UDP_PORT, &broker_addr, UDP_PORT);

	(&mqtt_sn_c)->mc = &mqtt_sn_call;

	sprintf(device_id,"%02X%02X%02X%02X%02X%02X%02X%02X",linkaddr_node_addr.u8[0],
			linkaddr_node_addr.u8[1],linkaddr_node_addr.u8[2],linkaddr_node_addr.u8[3],
			linkaddr_node_addr.u8[4],linkaddr_node_addr.u8[5],linkaddr_node_addr.u8[6],
			linkaddr_node_addr.u8[7]);

	sprintf(mqtt_client_id,"sens%02X%02X%02X%02X",linkaddr_node_addr.u8[4],linkaddr_node_addr.u8[5],linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);

	//add device_id to topics prior to subscription
	memcpy(upstream_topic,device_id,16);
	memcpy(control_topic,device_id,16);


	/*Request a connection and wait for connack*/
	LOG_INFO("requesting connection \n ");
	connection_timeout_event = process_alloc_event();
	connection_retries = 0;
	ctimer_set( &connection_timer, REPLY_TIMEOUT, connection_timer_callback, NULL);
	mqtt_sn_send_connect(&mqtt_sn_c,mqtt_client_id,mqtt_keep_alive);
	connection_state = MQTTSN_WAITING_CONNACK;
	while (connection_retries < 15)
	{
		PROCESS_WAIT_EVENT();
		if (ev == mqttsn_connack_event) {
			//if success
			LOG_INFO("connection acked\n");
			ctimer_stop(&connection_timer);
			connection_state = MQTTSN_CONNECTED;
			connection_retries = 15;//using break here may mess up switch statement of proces
		}
		if (ev == connection_timeout_event) {
			connection_state = MQTTSN_CONNECTION_FAILED;
			connection_retries++;
			LOG_INFO("connection timeout\n");
			ctimer_restart(&connection_timer);
			if (connection_retries < 15) {
				mqtt_sn_send_connect(&mqtt_sn_c,mqtt_client_id,mqtt_keep_alive);
				connection_state = MQTTSN_WAITING_CONNACK;
			}
		}
	}
	ctimer_stop(&connection_timer);
	if (connection_state == MQTTSN_CONNECTED){
		process_start(&registration_subscription_process, 0);
	}
	else
	{
		LOG_INFO("unable to connect\n");
	}
	PROCESS_END();
}
/*---------------------------------------------------------------------------*/

/*this process will publish data at regular intervals*/
PROCESS_THREAD(publish_process, ev, data)
{
  static uint8_t registration_tries;
  static struct etimer send_timer;
  static uint8_t buf_len;
  static uint32_t message_number=0;
  static char buf[20];
  static mqtt_sn_register_request *rreq = &regreq;

  PROCESS_BEGIN();
  send_interval = DEFAULT_SEND_INTERVAL;

  etimer_set(&send_timer, send_interval);
  while(1)
  {
      PROCESS_WAIT_EVENT();
      if(ev == PROCESS_EVENT_TIMER)
      {
          sprintf(buf, "Message %" PRIu32, message_number); //removendo o warning do GCC para o uint32_t
          message_number++;
          buf_len = strlen(buf);
          mqtt_sn_send_publish(&mqtt_sn_c, mqtt_topic_ids[UPSTREAM],MQTT_SN_TOPIC_TYPE_NORMAL,buf, buf_len,qos,retain);
          etimer_reset(&send_timer);
          LOG_INFO("publishing at topic: %s -> msg: %s\n", topics[UPSTREAM], buf);
      }
  }
  PROCESS_END();
}


/*---------------------------------------------------------------------------*/
static resolv_status_t
set_connection_address(uip_ipaddr_t *ipaddr)
{
#ifndef UDP_CONNECTION_ADDR
#if RESOLV_CONF_SUPPORTS_MDNS
#define UDP_CONNECTION_ADDR       BROKER_ADDRESS
#elif UIP_CONF_ROUTER
#define UDP_CONNECTION_ADDR       fd00:0:0:0:0212:7404:0004:0404
#else
#define UDP_CONNECTION_ADDR       fe80:0:0:0:6466:6666:6666:6666
#endif
#endif /* !UDP_CONNECTION_ADDR */

#define _QUOTEME(x) #x
#define QUOTEME(x) _QUOTEME(x)

  resolv_status_t status = RESOLV_STATUS_ERROR;

  if(uiplib_ipaddrconv(QUOTEME(UDP_CONNECTION_ADDR), ipaddr) == 0) {
    uip_ipaddr_t *resolved_addr = NULL;
    status = resolv_lookup(QUOTEME(UDP_CONNECTION_ADDR),&resolved_addr);
    if(status == RESOLV_STATUS_UNCACHED || status == RESOLV_STATUS_EXPIRED) {
      DEBUG("Attempting to look up %s\n",QUOTEME(UDP_CONNECTION_ADDR));
      resolv_query(QUOTEME(UDP_CONNECTION_ADDR));
      status = RESOLV_STATUS_RESOLVING;
    } else if(status == RESOLV_STATUS_CACHED && resolved_addr != NULL) {
      DEBUG("Lookup of \"%s\" succeded!\n",QUOTEME(UDP_CONNECTION_ADDR));
    } else if(status == RESOLV_STATUS_RESOLVING) {
      DEBUG("Still looking up \"%s\"...\n",QUOTEME(UDP_CONNECTION_ADDR));
    } else {
      DEBUG("Lookup of \"%s\" failed. status = %d\n",QUOTEME(UDP_CONNECTION_ADDR),status);
    }
    if(resolved_addr)
      uip_ipaddr_copy(ipaddr, resolved_addr);
  } else {
    status = RESOLV_STATUS_CACHED;
  }

  return status;
}


