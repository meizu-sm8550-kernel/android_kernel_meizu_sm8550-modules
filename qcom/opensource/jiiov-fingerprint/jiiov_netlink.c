// SPDX-License-Identifier: GPL-2.0-only
/* Note ANC HAL transport: fixed protocol/type 30, userspace port 100. */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "jiiov.h"

static DEFINE_MUTEX(jiiov_nl_lock);
static struct sock *jiiov_nl_socket;

int jiiov_netlink_send(u8 event)
{
	struct sk_buff *skb;
	struct nlmsghdr *header;
	int ret;

	if (event > 7)
		return -EINVAL;

	mutex_lock(&jiiov_nl_lock);
	if (!jiiov_nl_socket) {
		ret = -ENOTCONN;
		goto unlock;
	}
	skb = nlmsg_new(1, GFP_KERNEL);
	if (!skb) {
		ret = -ENOMEM;
		goto unlock;
	}
	header = nlmsg_put(skb, 0, 0, 30, 1, 0);
	if (!header) {
		kfree_skb(skb);
		ret = -EMSGSIZE;
		goto unlock;
	}
	*(u8 *)nlmsg_data(header) = event;
	/* netlink_unicast consumes skb, including on error. */
	ret = netlink_unicast(jiiov_nl_socket, skb, 100, MSG_DONTWAIT);
unlock:
	mutex_unlock(&jiiov_nl_lock);
	return ret;
}

static void jiiov_netlink_receive(struct sk_buff *skb)
{
	struct nlmsghdr *header;
	u8 event;

	if (!pskb_may_pull(skb, NLMSG_HDRLEN))
		return;
	header = nlmsg_hdr(skb);
	if (!nlmsg_ok(header, skb->len) || nlmsg_len(header) < 1)
		return;
	if (!pskb_may_pull(skb, NLMSG_HDRLEN + 1))
		return;

	/* HAL sends 144 bytes, but only the first payload byte is initialized. */
	header = nlmsg_hdr(skb);
	event = *(u8 *)nlmsg_data(header);
	/* 7 wakes HAL's blocking recvmsg before its teardown pthread_join. */
	if (event == 0 || event == 7)
		jiiov_netlink_send(event);
}

int jiiov_netlink_init(void)
{
	struct netlink_kernel_cfg config = {
		.input = jiiov_netlink_receive,
	};
	struct sock *socket;

	socket = netlink_kernel_create(&init_net, 30, &config);
	if (!socket)
		return -EADDRINUSE;
	mutex_lock(&jiiov_nl_lock);
	jiiov_nl_socket = socket;
	mutex_unlock(&jiiov_nl_lock);
	return 0;
}

void jiiov_netlink_exit(void)
{
	struct sock *socket;

	/* Platform unregister has already stopped every IRQ/work/sysfs sender. */
	mutex_lock(&jiiov_nl_lock);
	socket = jiiov_nl_socket;
	jiiov_nl_socket = NULL;
	mutex_unlock(&jiiov_nl_lock);
	if (socket)
		netlink_kernel_release(socket);
}
