/*
 * Copyright (c) 2018 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if 1
#define SYS_LOG_DOMAIN "tc-app"
#define NET_SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#define NET_LOG_ENABLED 1
#endif

#include <zephyr.h>
#include <errno.h>

#include <net/net_core.h>
#include <net/net_l2.h>
#include <net/net_if.h>
#include <net/ethernet.h>
#include <net/net_context.h>
#include <net/net_app.h>

#define MY_PORT 0
#define PEER_PORT 4242

#define WAIT_TIME  K_SECONDS(2)
#define CONNECT_TIME  K_SECONDS(10)

#if defined(CONFIG_NET_IPV6)
static struct net_app_ctx udp6[NET_TC_COUNT];
#endif
#if defined(CONFIG_NET_IPV4)
static struct net_app_ctx udp4[NET_TC_COUNT];
#endif

static struct k_sem quit_lock;

/* Generated by http://www.lipsum.com/
 * 3 paragraphs, 176 words, 1230 bytes of Lorem Ipsum
 */
const char lorem_ipsum[] =
	"Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
	"Vestibulum id cursus felis, sit amet suscipit velit. Integer "
	"facilisis malesuada porta. Nunc at accumsan mauris. Etiam vehicula, "
	"arcu consequat feugiat venenatis, tellus velit gravida ligula, quis "
	"posuere sem leo eget urna. Curabitur condimentum leo nec orci "
	"mattis, nec faucibus dui rutrum. Ut mollis orci in iaculis "
	"consequat. Nulla volutpat nibh eu velit sagittis, a iaculis dui "
	"aliquam."
	"\n"
	"Quisque interdum consequat eros a eleifend. Fusce dapibus nisl "
	"sit amet velit posuere imperdiet. Quisque accumsan tempor massa "
	"sit amet tincidunt. Integer sollicitudin vehicula tristique. Nulla "
	"sagittis massa turpis, ac ultricies neque posuere eu. Nulla et "
	"imperdiet ex. Etiam venenatis sed lacus tincidunt hendrerit. In "
	"libero nisl, congue id tellus vitae, tincidunt tristique mauris. "
	"Nullam sed porta massa. Sed condimentum sem eu convallis euismod. "
	"Suspendisse lobortis purus faucibus, gravida turpis id, mattis "
	"velit. Maecenas eleifend sapien eu tincidunt lobortis. Sed elementum "
	"sapien id enim laoreet consequat."
	"\n"
	"Aenean et neque aliquam, lobortis lectus in, consequat leo. Sed "
	"quis egestas nulla. Quisque ac risus quis elit mollis finibus. "
	"Phasellus efficitur imperdiet metus."
	"\n";

static int ipsum_len = sizeof(lorem_ipsum) - 1;

struct stats {
	u32_t sent;
	u32_t received;
	u32_t dropped;
	u32_t wrong_order;
	u32_t invalid;

	u32_t sent_time_sum;
	u32_t sent_time_count;
	s64_t sent_time; /* in milliseconds */
};

struct configs;

struct data {
	/* Work controlling udp data sending */
	struct k_delayed_work recv;
	struct net_app_ctx *udp;
	struct configs *conf;
	sa_family_t family;

	const char *proto;
	u32_t expecting_udp;
	u8_t priority;

	struct stats stats;
};

struct configs {
	struct data ipv4[NET_TC_COUNT];
	struct data ipv6[NET_TC_COUNT];
};

static struct configs conf = {
	.ipv4 = {
		[0 ... (NET_TC_COUNT - 1)] = {
			.proto = "IPv4",
			.family = AF_INET,
		}
	},
	.ipv6 = {
		[0 ... (NET_TC_COUNT - 1)] = {
			.proto = "IPv6",
			.family = AF_INET6,
		}
	}
};

#define TYPE_SEQ_NUM 42

struct header {
	u8_t type;
	u8_t len;
	union {
		u8_t value[0];
		struct {
			u32_t seq;
			s64_t sent;
		};
	};
} __packed;

#if CONFIG_NET_VLAN_COUNT > 1
#define CREATE_MULTIPLE_TAGS
#endif

struct ud {
	struct net_if *first;
	struct net_if *second;
};

#if defined(CREATE_MULTIPLE_TAGS)
static void iface_cb(struct net_if *iface, void *user_data)
{
	struct ud *ud = user_data;

	if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET)) {
		return;
	}

	if (iface == ud->first) {
		return;
	}

	ud->second = iface;
}
#endif

static int init_app(void)
{
	struct net_if *iface;
	int ret;

#if defined(CREATE_MULTIPLE_TAGS)
	struct net_if_addr *ifaddr;
	struct in_addr addr4;
	struct in6_addr addr6;
	struct ud ud;
#endif

	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	if (!iface) {
		NET_ERR("No ethernet interfaces found.");
		return -ENOENT;
	}

#if defined(CONFIG_NET_VLAN)
	ret = net_eth_vlan_enable(iface, CONFIG_SAMPLE_VLAN_TAG);
	if (ret < 0) {
		NET_ERR("Cannot enable VLAN for tag %d (%d)",
			CONFIG_SAMPLE_VLAN_TAG, ret);
	}
#endif

#if defined(CREATE_MULTIPLE_TAGS)
	ud.first = iface;
	ud.second = NULL;

	net_if_foreach(iface_cb, &ud);

	/* This sample has two VLANs. For the second one we need to manually
	 * create IP address for this test. But first the VLAN needs to be
	 * added to the interface so that IPv6 DAD can work properly.
	 */
	ret = net_eth_vlan_enable(ud.second, CONFIG_SAMPLE_VLAN_TAG_2);
	if (ret < 0) {
		NET_ERR("Cannot enable VLAN for tag %d (%d)",
			CONFIG_SAMPLE_VLAN_TAG_2, ret);
	}

#if defined(CONFIG_NET_IPV6)
	if (net_addr_pton(AF_INET6, CONFIG_SAMPLE_IPV6_ADDR_2, &addr6)) {
		NET_ERR("Invalid address: %s", CONFIG_SAMPLE_IPV6_ADDR_2);
		return -EINVAL;
	}

	ifaddr = net_if_ipv6_addr_add(ud.second, &addr6, NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		NET_ERR("Cannot add %s to interface %p",
			CONFIG_SAMPLE_IPV6_ADDR_2, ud.second);
		return -EINVAL;
	}
#else
	ARG_UNUSED(addr6);
#endif /* IPV6 */

#if defined(CONFIG_NET_IPV4)
	if (net_addr_pton(AF_INET, CONFIG_SAMPLE_IPV4_ADDR_2, &addr4)) {
		NET_ERR("Invalid address: %s", CONFIG_SAMPLE_IPV4_ADDR_2);
		return -EINVAL;
	}

	ifaddr = net_if_ipv4_addr_add(ud.second, &addr4, NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		NET_ERR("Cannot add %s to interface %p",
			CONFIG_SAMPLE_IPV4_ADDR_2, ud.second);
		return -EINVAL;
	}
#else
	ARG_UNUSED(addr4);
#endif /* IPV4 */

#endif

	return ret;
}

static u32_t calc_time(u32_t count, u32_t sum)
{
	if (!count) {
		return 0;
	}

	return (sum * 1000) / count;
}

#define PRINT_STATISTICS_INTERVAL (30 * MSEC_PER_SEC)

static void stats(struct data *data)
{
	static bool first = true;
	static s64_t next_print;
	s64_t curr = k_uptime_get();

	if (!next_print || (next_print < curr &&
	    (!((curr - next_print) > PRINT_STATISTICS_INTERVAL)))) {
		s64_t new_print;
		int i;

		if (first) {
			first = false;
			goto skip_print;
		}

		NET_INFO("Traffic class statistics:");
		NET_INFO("   Prio\tSent\tRecv\tDrop\tMiss\tTime (us)");

#if defined(CONFIG_NET_IPV6)
		for (i = 0; i < NET_TC_COUNT; i++) {
			u32_t round_trip_time =
				calc_time(
				     data->conf->ipv6[i].stats.sent_time_count,
				     data->conf->ipv6[i].stats.sent_time_sum);

			NET_INFO("v6 %d\t%u\t%u\t%u\t%u\t%u",
				 data->conf->ipv6[i].priority,
				 data->conf->ipv6[i].stats.sent,
				 data->conf->ipv6[i].stats.received,
				 data->conf->ipv6[i].stats.dropped,
				 data->conf->ipv6[i].stats.wrong_order,
				 round_trip_time);
		}
#endif

#if defined(CONFIG_NET_IPV4)
		for (i = 0; i < NET_TC_COUNT; i++) {
			u32_t round_trip_time =
				calc_time(
				     data->conf->ipv4[i].stats.sent_time_count,
				     data->conf->ipv4[i].stats.sent_time_sum);

			NET_INFO("v4 %d\t%u\t%u\t%u\t%u\t%u",
				 data->conf->ipv4[i].priority,
				 data->conf->ipv4[i].stats.sent,
				 data->conf->ipv4[i].stats.received,
				 data->conf->ipv4[i].stats.dropped,
				 data->conf->ipv4[i].stats.wrong_order,
				 round_trip_time);
		}
#endif
		NET_INFO("---");

skip_print:
		new_print = curr + PRINT_STATISTICS_INTERVAL;
		if (new_print > curr) {
			next_print = new_print;
		} else {
			/* Overflow */
			next_print = PRINT_STATISTICS_INTERVAL -
				(LLONG_MAX - curr);
		}
	}
}

static struct net_pkt *prepare_send_pkt(struct net_app_ctx *ctx,
					const char *name,
					int *expecting_len,
					struct data *data)
{
	struct net_pkt *send_pkt;
	struct header *hdr;
	u32_t seq;
	s32_t timeout = K_SECONDS(1);

	send_pkt = net_app_get_net_pkt(ctx, data->family, timeout);
	if (!send_pkt) {
		return NULL;
	}

	seq = htonl(data->stats.sent + 1);

	*expecting_len = net_pkt_append(send_pkt, *expecting_len,
					lorem_ipsum, timeout);

	hdr = (struct header *)send_pkt->frags->data;
	hdr->type = TYPE_SEQ_NUM;
	hdr->len = sizeof(seq);

	UNALIGNED_PUT(seq, &hdr->seq);
	UNALIGNED_PUT(k_uptime_get(), &hdr->sent);

	return send_pkt;
}

static bool send_udp_data(struct net_app_ctx *ctx, struct data *data)
{
	s32_t timeout = K_SECONDS(1);
	struct net_pkt *pkt;
	size_t len;
	int ret;

	data->expecting_udp = sys_rand32_get() % ipsum_len;

	pkt = prepare_send_pkt(ctx, data->proto, &data->expecting_udp, data);
	if (!pkt) {
		return false;
	}

	len = net_pkt_get_len(pkt);

	NET_ASSERT_INFO(data->expecting_udp == len,
			"Data to send %d bytes, real len %zu",
			data->expecting_udp, len);

	data->stats.sent_time = k_uptime_get();

	ret = net_app_send_pkt(ctx, pkt, NULL, 0, timeout,
			       UINT_TO_POINTER(len));
	if (ret < 0) {
		net_pkt_unref(pkt);
	}

	data->stats.sent++;

	k_delayed_work_submit(&data->recv, WAIT_TIME);

	stats(data);

	return true;
}

static void send_more_data(struct net_app_ctx *ctx, struct data *data)
{
	bool ret;

	do {
		ret = send_udp_data(ctx, data);
		if (!ret) {
			/* Avoid too much flooding */
			k_sleep(K_MSEC(10));
		}
	} while (!ret);

	/* We should not call k_yield() here as that will not let lower
	 * priority thread to run.
	 */
	k_sleep(K_MSEC(1));
}

static void udp_received(struct net_app_ctx *ctx,
			 struct net_pkt *pkt,
			 int status,
			 void *user_data)
{
	struct data *data = ctx->user_data;
	struct header *hdr = (struct header *)net_pkt_appdata(pkt);

	ARG_UNUSED(user_data);
	ARG_UNUSED(status);

	if (data->expecting_udp != net_pkt_appdatalen(pkt)) {
		NET_DBG("Sent %d bytes, received %u bytes",
			data->expecting_udp, net_pkt_appdatalen(pkt));
	}

	net_pkt_unref(pkt);

	k_delayed_work_cancel(&data->recv);

	if (hdr->type != TYPE_SEQ_NUM) {
		data->stats.invalid++;
	} else {
		if (ntohl(UNALIGNED_GET(&hdr->seq)) != data->stats.sent) {
			data->stats.wrong_order++;
		} else {
			data->stats.received++;

			data->stats.sent_time_sum += k_uptime_get() -
				data->stats.sent_time;
			data->stats.sent_time_count++;
		}
	}

	send_more_data(ctx, data);
}

static int connect_udp(sa_family_t family, struct net_app_ctx *ctx,
		       const char *peer, void *user_data, u8_t priority)
{
	struct data *data = user_data;
	size_t optlen = sizeof(priority);
	int ret;

	data->udp = ctx;

	ret = net_app_init_udp_client(ctx, NULL, NULL, peer, PEER_PORT,
				      WAIT_TIME, user_data);
	if (ret < 0) {
		NET_ERR("Cannot init %s UDP client (%d)", data->proto, ret);
		goto fail;
	}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	net_app_set_net_pkt_pool(ctx, tx_udp_slab, data_udp_pool);
#endif

	ret = net_app_set_cb(ctx, NULL, udp_received, NULL, NULL);
	if (ret < 0) {
		NET_ERR("Cannot set callbacks (%d)", ret);
		goto fail;
	}

	ret = net_app_connect(ctx, CONNECT_TIME);
	if (ret < 0) {
		NET_ERR("Cannot connect UDP (%d)", ret);
		goto fail;
	}

#if defined(CONFIG_NET_IPV4)
	if (family == AF_INET) {
		net_context_set_option(ctx->ipv4.ctx, NET_OPT_PRIORITY,
				       &priority, sizeof(u8_t));

		net_context_get_option(ctx->ipv4.ctx, NET_OPT_PRIORITY,
				       &priority, &optlen);
	}
#endif

#if defined(CONFIG_NET_IPV6)
	if (family == AF_INET6) {
		net_context_set_option(ctx->ipv6.ctx, NET_OPT_PRIORITY,
				       &priority, sizeof(u8_t));

		net_context_get_option(ctx->ipv6.ctx, NET_OPT_PRIORITY,
				       &priority, &optlen);
	}
#endif

	data->priority = priority;
fail:
	return ret;
}

static void wait_reply(struct k_work *work)
{
	/* This means that we did not receive response in time. */
	struct data *data = CONTAINER_OF(work, struct data, recv);

	data->stats.dropped++;

	/* Send a new packet at this point */
	send_more_data(data->udp, data);
}

static void setup_clients(void)
{
	int ret, i;

#if defined(CONFIG_NET_IPV6)
	for (i = 0; i < NET_TC_COUNT; i++) {
		k_delayed_work_init(&conf.ipv6[i].recv, wait_reply);

		conf.ipv6[i].conf = &conf;

		if (i % 2) {
			NET_DBG("TC %d connecting to %s", i,
				CONFIG_NET_APP_PEER_IPV6_ADDR);
			ret = connect_udp(AF_INET6, &udp6[i],
					  CONFIG_NET_APP_PEER_IPV6_ADDR,
					  &conf.ipv6[i], i);
		} else {
			NET_DBG("TC %d connecting to %s", i,
				CONFIG_SAMPLE_PEER_IPV6_ADDR_2);
			ret = connect_udp(AF_INET6, &udp6[i],
					  CONFIG_SAMPLE_PEER_IPV6_ADDR_2,
					  &conf.ipv6[i], i);
		}

		if (ret < 0) {
			NET_ERR("Cannot init IPv6 UDP client %d (%d)",
				i + 1, ret);
		}
	}
#endif

#if defined(CONFIG_NET_IPV4)
	for (i = 0; i < NET_TC_COUNT; i++) {
		k_delayed_work_init(&conf.ipv4[i].recv, wait_reply);

		conf.ipv4[i].conf = &conf;

		if (i % 2) {
			NET_DBG("TC %d connecting to %s", i,
				CONFIG_NET_APP_PEER_IPV4_ADDR);
			ret = connect_udp(AF_INET, &udp4[i],
					  CONFIG_NET_APP_PEER_IPV4_ADDR,
					  &conf.ipv4[i], i);
		} else {
			NET_DBG("TC %d connecting to %s", i,
				CONFIG_SAMPLE_PEER_IPV4_ADDR_2);
			ret = connect_udp(AF_INET, &udp4[i],
					  CONFIG_SAMPLE_PEER_IPV4_ADDR_2,
					  &conf.ipv4[i], i);
		}

		if (ret < 0) {
			NET_ERR("Cannot init IPv4 UDP client %d (%d)",
				i + 1, ret);
		}
	}
#endif

	/* We can start to send data when UDP is "connected" */
	for (i = 0; i < NET_TC_COUNT; i++) {
#if defined(CONFIG_NET_IPV6)
		send_more_data(&udp6[i], &conf.ipv6[i]);
#endif
#if defined(CONFIG_NET_IPV4)
		send_more_data(&udp4[i], &conf.ipv4[i]);
#endif
	}
}

void main(void)
{
	k_sem_init(&quit_lock, 0, UINT_MAX);

	init_app();

	/* This extra sleep is needed so that the network stabilizes a bit
	 * before we start to send data. This is important as we have multiple
	 * network interfaces and all of them should be configured properly
	 * before we continue.
	 */
	k_sleep(K_SECONDS(5));

	setup_clients();

	k_sem_take(&quit_lock, K_FOREVER);
}