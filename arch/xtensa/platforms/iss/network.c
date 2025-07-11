// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 * arch/xtensa/platforms/iss/network.c
 *
 * Platform specific initialization.
 *
 * Authors: Chris Zankel <chris@zankel.net>
 * Based on work form the UML team.
 *
 * Copyright 2005 Tensilica Inc.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/list.h>
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/if_ether.h>
#include <linux/inetdevice.h>
#include <linux/init.h>
#include <linux/if_tun.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/memblock.h>
#include <linux/ethtool.h>
#include <linux/rtnetlink.h>
#include <linux/platform_device.h>

#include <platform/simcall.h>

#define DRIVER_NAME "iss-netdev"
#define ETH_MAX_PACKET 1500
#define ETH_HEADER_OTHER 14
#define ISS_NET_TIMER_VALUE (HZ / 10)

/* ------------------------------------------------------------------------- */

/* We currently only support the TUNTAP transport protocol. */

#define TRANSPORT_TUNTAP_NAME "tuntap"
#define TRANSPORT_TUNTAP_MTU ETH_MAX_PACKET

struct tuntap_info {
	char dev_name[IFNAMSIZ];
	int fd;
};

/* ------------------------------------------------------------------------- */


struct iss_net_private;

struct iss_net_ops {
	int (*open)(struct iss_net_private *lp);
	void (*close)(struct iss_net_private *lp);
	int (*read)(struct iss_net_private *lp, struct sk_buff **skb);
	int (*write)(struct iss_net_private *lp, struct sk_buff **skb);
	unsigned short (*protocol)(struct sk_buff *skb);
	int (*poll)(struct iss_net_private *lp);
};

/* This structure contains out private information for the driver. */

struct iss_net_private {
	spinlock_t lock;
	struct net_device *dev;
	struct platform_device pdev;
	struct timer_list tl;
	struct rtnl_link_stats64 stats;

	struct timer_list timer;
	unsigned int timer_val;

	int index;
	int mtu;

	struct {
		union {
			struct tuntap_info tuntap;
		} info;

		const struct iss_net_ops *net_ops;
	} tp;

};

/* ================================ HELPERS ================================ */


static char *split_if_spec(char *str, ...)
{
	char **arg, *end;
	va_list ap;

	va_start(ap, str);
	while ((arg = va_arg(ap, char**)) != NULL) {
		if (*str == '\0') {
			va_end(ap);
			return NULL;
		}
		end = strchr(str, ',');
		if (end != str)
			*arg = str;
		if (end == NULL) {
			va_end(ap);
			return NULL;
		}
		*end++ = '\0';
		str = end;
	}
	va_end(ap);
	return str;
}

/* Set Ethernet address of the specified device. */

static void setup_etheraddr(struct net_device *dev, char *str)
{
	u8 addr[ETH_ALEN];

	if (str == NULL)
		goto random;

	if (!mac_pton(str, addr)) {
		pr_err("%s: failed to parse '%s' as an ethernet address\n",
		       dev->name, str);
		goto random;
	}
	if (is_multicast_ether_addr(addr)) {
		pr_err("%s: attempt to assign a multicast ethernet address\n",
		       dev->name);
		goto random;
	}
	if (!is_valid_ether_addr(addr)) {
		pr_err("%s: attempt to assign an invalid ethernet address\n",
		       dev->name);
		goto random;
	}
	if (!is_local_ether_addr(addr))
		pr_warn("%s: assigning a globally valid ethernet address\n",
			dev->name);
	eth_hw_addr_set(dev, addr);
	return;

random:
	pr_info("%s: choosing a random ethernet address\n",
		dev->name);
	eth_hw_addr_random(dev);
}

/* ======================= TUNTAP TRANSPORT INTERFACE ====================== */

static int tuntap_open(struct iss_net_private *lp)
{
	struct ifreq ifr;
	char *dev_name = lp->tp.info.tuntap.dev_name;
	int err = -EINVAL;
	int fd;

	fd = simc_open("/dev/net/tun", 02, 0); /* O_RDWR */
	if (fd < 0) {
		pr_err("%s: failed to open /dev/net/tun, returned %d (errno = %d)\n",
		       lp->dev->name, fd, errno);
		return fd;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strscpy(ifr.ifr_name, dev_name, sizeof(ifr.ifr_name));

	err = simc_ioctl(fd, TUNSETIFF, &ifr);
	if (err < 0) {
		pr_err("%s: failed to set interface %s, returned %d (errno = %d)\n",
		       lp->dev->name, dev_name, err, errno);
		simc_close(fd);
		return err;
	}

	lp->tp.info.tuntap.fd = fd;
	return err;
}

static void tuntap_close(struct iss_net_private *lp)
{
	simc_close(lp->tp.info.tuntap.fd);
	lp->tp.info.tuntap.fd = -1;
}

static int tuntap_read(struct iss_net_private *lp, struct sk_buff **skb)
{
	return simc_read(lp->tp.info.tuntap.fd,
			(*skb)->data, (*skb)->dev->mtu + ETH_HEADER_OTHER);
}

static int tuntap_write(struct iss_net_private *lp, struct sk_buff **skb)
{
	return simc_write(lp->tp.info.tuntap.fd, (*skb)->data, (*skb)->len);
}

static unsigned short tuntap_protocol(struct sk_buff *skb)
{
	return eth_type_trans(skb, skb->dev);
}

static int tuntap_poll(struct iss_net_private *lp)
{
	return simc_poll(lp->tp.info.tuntap.fd);
}

static const struct iss_net_ops tuntap_ops = {
	.open		= tuntap_open,
	.close		= tuntap_close,
	.read		= tuntap_read,
	.write		= tuntap_write,
	.protocol	= tuntap_protocol,
	.poll		= tuntap_poll,
};

/*
 * ethX=tuntap,[mac address],device name
 */

static int tuntap_probe(struct iss_net_private *lp, int index, char *init)
{
	struct net_device *dev = lp->dev;
	char *dev_name = NULL, *mac_str = NULL, *rem = NULL;

	/* Transport should be 'tuntap': ethX=tuntap,mac,dev_name */

	if (strncmp(init, TRANSPORT_TUNTAP_NAME,
		    sizeof(TRANSPORT_TUNTAP_NAME) - 1))
		return 0;

	init += sizeof(TRANSPORT_TUNTAP_NAME) - 1;
	if (*init == ',') {
		rem = split_if_spec(init + 1, &mac_str, &dev_name, NULL);
		if (rem != NULL) {
			pr_err("%s: extra garbage on specification : '%s'\n",
			       dev->name, rem);
			return 0;
		}
	} else if (*init != '\0') {
		pr_err("%s: invalid argument: %s. Skipping device!\n",
		       dev->name, init);
		return 0;
	}

	if (!dev_name) {
		pr_err("%s: missing tuntap device name\n", dev->name);
		return 0;
	}

	strscpy(lp->tp.info.tuntap.dev_name, dev_name,
		sizeof(lp->tp.info.tuntap.dev_name));

	setup_etheraddr(dev, mac_str);

	lp->mtu = TRANSPORT_TUNTAP_MTU;

	lp->tp.info.tuntap.fd = -1;
	lp->tp.net_ops = &tuntap_ops;

	return 1;
}

/* ================================ ISS NET ================================ */

static int iss_net_rx(struct net_device *dev)
{
	struct iss_net_private *lp = netdev_priv(dev);
	int pkt_len;
	struct sk_buff *skb;

	/* Check if there is any new data. */

	if (lp->tp.net_ops->poll(lp) == 0)
		return 0;

	/* Try to allocate memory, if it fails, try again next round. */

	skb = dev_alloc_skb(dev->mtu + 2 + ETH_HEADER_OTHER);
	if (skb == NULL) {
		spin_lock_bh(&lp->lock);
		lp->stats.rx_dropped++;
		spin_unlock_bh(&lp->lock);
		return 0;
	}

	skb_reserve(skb, 2);

	/* Setup skb */

	skb->dev = dev;
	skb_reset_mac_header(skb);
	pkt_len = lp->tp.net_ops->read(lp, &skb);
	skb_put(skb, pkt_len);

	if (pkt_len > 0) {
		skb_trim(skb, pkt_len);
		skb->protocol = lp->tp.net_ops->protocol(skb);

		spin_lock_bh(&lp->lock);
		lp->stats.rx_bytes += skb->len;
		lp->stats.rx_packets++;
		spin_unlock_bh(&lp->lock);
		netif_rx(skb);
		return pkt_len;
	}
	kfree_skb(skb);
	return pkt_len;
}

static int iss_net_poll(struct iss_net_private *lp)
{
	int err, ret = 0;

	if (!netif_running(lp->dev))
		return 0;

	while ((err = iss_net_rx(lp->dev)) > 0)
		ret++;

	if (err < 0) {
		pr_err("Device '%s' read returned %d, shutting it down\n",
		       lp->dev->name, err);
		dev_close(lp->dev);
	} else {
		/* FIXME reactivate_fd(lp->fd, ISS_ETH_IRQ); */
	}

	return ret;
}


static void iss_net_timer(struct timer_list *t)
{
	struct iss_net_private *lp = timer_container_of(lp, t, timer);

	iss_net_poll(lp);
	mod_timer(&lp->timer, jiffies + lp->timer_val);
}


static int iss_net_open(struct net_device *dev)
{
	struct iss_net_private *lp = netdev_priv(dev);
	int err;

	err = lp->tp.net_ops->open(lp);
	if (err < 0)
		return err;

	netif_start_queue(dev);

	/* clear buffer - it can happen that the host side of the interface
	 * is full when we get here. In this case, new data is never queued,
	 * SIGIOs never arrive, and the net never works.
	 */
	while ((err = iss_net_rx(dev)) > 0)
		;

	timer_setup(&lp->timer, iss_net_timer, 0);
	lp->timer_val = ISS_NET_TIMER_VALUE;
	mod_timer(&lp->timer, jiffies + lp->timer_val);

	return err;
}

static int iss_net_close(struct net_device *dev)
{
	struct iss_net_private *lp = netdev_priv(dev);

	netif_stop_queue(dev);
	timer_delete_sync(&lp->timer);
	lp->tp.net_ops->close(lp);

	return 0;
}

static int iss_net_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct iss_net_private *lp = netdev_priv(dev);
	int len;

	netif_stop_queue(dev);

	len = lp->tp.net_ops->write(lp, &skb);

	if (len == skb->len) {
		spin_lock_bh(&lp->lock);
		lp->stats.tx_packets++;
		lp->stats.tx_bytes += skb->len;
		spin_unlock_bh(&lp->lock);
		netif_trans_update(dev);
		netif_start_queue(dev);

		/* this is normally done in the interrupt when tx finishes */
		netif_wake_queue(dev);

	} else if (len == 0) {
		netif_start_queue(dev);
		spin_lock_bh(&lp->lock);
		lp->stats.tx_dropped++;
		spin_unlock_bh(&lp->lock);

	} else {
		netif_start_queue(dev);
		pr_err("%s: %s failed(%d)\n", dev->name, __func__, len);
	}


	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}


static void iss_net_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *stats)
{
	struct iss_net_private *lp = netdev_priv(dev);

	spin_lock_bh(&lp->lock);
	*stats = lp->stats;
	spin_unlock_bh(&lp->lock);
}

static void iss_net_set_multicast_list(struct net_device *dev)
{
}

static void iss_net_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
}

static int iss_net_change_mtu(struct net_device *dev, int new_mtu)
{
	return -EINVAL;
}

static void iss_net_user_timer_expire(struct timer_list *unused)
{
}


static struct platform_driver iss_net_driver = {
	.driver = {
		.name  = DRIVER_NAME,
	},
};

static int driver_registered;

static const struct net_device_ops iss_netdev_ops = {
	.ndo_open		= iss_net_open,
	.ndo_stop		= iss_net_close,
	.ndo_get_stats64	= iss_net_get_stats64,
	.ndo_start_xmit		= iss_net_start_xmit,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_change_mtu		= iss_net_change_mtu,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_tx_timeout		= iss_net_tx_timeout,
	.ndo_set_rx_mode	= iss_net_set_multicast_list,
};

static void iss_net_pdev_release(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct iss_net_private *lp =
		container_of(pdev, struct iss_net_private, pdev);

	free_netdev(lp->dev);
}

static void iss_net_configure(int index, char *init)
{
	struct net_device *dev;
	struct iss_net_private *lp;

	dev = alloc_etherdev(sizeof(*lp));
	if (dev == NULL) {
		pr_err("eth_configure: failed to allocate device\n");
		return;
	}

	/* Initialize private element. */

	lp = netdev_priv(dev);
	*lp = (struct iss_net_private) {
		.dev			= dev,
		.index			= index,
	};

	spin_lock_init(&lp->lock);
	/*
	 * If this name ends up conflicting with an existing registered
	 * netdevice, that is OK, register_netdev{,ice}() will notice this
	 * and fail.
	 */
	snprintf(dev->name, sizeof(dev->name), "eth%d", index);

	/*
	 * Try all transport protocols.
	 * Note: more protocols can be added by adding '&& !X_init(lp, eth)'.
	 */

	if (!tuntap_probe(lp, index, init)) {
		pr_err("%s: invalid arguments. Skipping device!\n",
		       dev->name);
		goto err_free_netdev;
	}

	pr_info("Netdevice %d (%pM)\n", index, dev->dev_addr);

	/* sysfs register */

	if (!driver_registered) {
		if (platform_driver_register(&iss_net_driver))
			goto err_free_netdev;
		driver_registered = 1;
	}

	lp->pdev.id = index;
	lp->pdev.name = DRIVER_NAME;
	lp->pdev.dev.release = iss_net_pdev_release;
	if (platform_device_register(&lp->pdev))
		goto err_free_netdev;
	SET_NETDEV_DEV(dev, &lp->pdev.dev);

	dev->netdev_ops = &iss_netdev_ops;
	dev->mtu = lp->mtu;
	dev->watchdog_timeo = (HZ >> 1);
	dev->irq = -1;

	rtnl_lock();
	if (register_netdevice(dev)) {
		rtnl_unlock();
		pr_err("%s: error registering net device!\n", dev->name);
		platform_device_unregister(&lp->pdev);
		/* dev is freed by the iss_net_pdev_release callback */
		return;
	}
	rtnl_unlock();

	timer_setup(&lp->tl, iss_net_user_timer_expire, 0);

	return;

err_free_netdev:
	free_netdev(dev);
}

/* ------------------------------------------------------------------------- */

/* Filled in during early boot */

struct list_head eth_cmd_line = LIST_HEAD_INIT(eth_cmd_line);

struct iss_net_init {
	struct list_head list;
	char *init;		/* init string */
	int index;
};

/*
 * Parse the command line and look for 'ethX=...' fields, and register all
 * those fields. They will be later initialized in iss_net_init.
 */

static int __init iss_net_setup(char *str)
{
	struct iss_net_init *device = NULL;
	struct iss_net_init *new;
	struct list_head *ele;
	char *end;
	int rc;
	unsigned n;

	end = strchr(str, '=');
	if (!end) {
		pr_err("Expected '=' after device number\n");
		return 1;
	}
	*end = 0;
	rc = kstrtouint(str, 0, &n);
	*end = '=';
	if (rc < 0) {
		pr_err("Failed to parse '%s'\n", str);
		return 1;
	}
	str = end;

	list_for_each(ele, &eth_cmd_line) {
		device = list_entry(ele, struct iss_net_init, list);
		if (device->index == n)
			break;
	}

	if (device && device->index == n) {
		pr_err("Device %u already configured\n", n);
		return 1;
	}

	new = memblock_alloc(sizeof(*new), SMP_CACHE_BYTES);
	if (new == NULL) {
		pr_err("Alloc_bootmem failed\n");
		return 1;
	}

	INIT_LIST_HEAD(&new->list);
	new->index = n;
	new->init = str + 1;

	list_add_tail(&new->list, &eth_cmd_line);
	return 1;
}

__setup("eth", iss_net_setup);

/*
 * Initialize all ISS Ethernet devices previously registered in iss_net_setup.
 */

static int iss_net_init(void)
{
	struct list_head *ele, *next;

	/* Walk through all Ethernet devices specified in the command line. */

	list_for_each_safe(ele, next, &eth_cmd_line) {
		struct iss_net_init *eth;
		eth = list_entry(ele, struct iss_net_init, list);
		iss_net_configure(eth->index, eth->init);
	}

	return 1;
}
device_initcall(iss_net_init);
