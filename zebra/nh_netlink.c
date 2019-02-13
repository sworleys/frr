/*
 * Zebra - nexthop object kernel parsing
 * Copyright (C) 2019 Cumulus Networks Inc.
 *               Stephen Worley
 *
 * FRR is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * FRR is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#ifdef HAVE_NETLINK

#include <linux/rtnetlink.h>
#include <linux/nexthop.h>

#include "log.h"

#include "zebra/zebra_ns.h"
#include "zebra/rt_netlink.h"
#include "zebra/nh_netlink.h"


/**
 * netlink_nexthop_change_read() - Read in change about nexthops from the kernel
 *
 * @h:		Netlink message header
 * @startup:	Are we reading under startup conditions?
 * Return:	Result status
 */
static int netlink_nexthop_change_read(struct nlmsghdr *h, ns_id_t ns_id,
				       int startup)
{
	int len;
	struct nhmsg *nhm;
	struct rtattr *tb[NHA_MAX + 1];


	nhm = NLMSG_DATA(h);

	if (startup && h->nlmsg_type != RTM_NEWNEXTHOP)
		return 0;

	len = h->nlmsg_len - NLMSG_LENGTH(sizeof(struct nhmsg));
	if (len < 0) {
		zlog_err("%s: Message received from netlink is of a broken size %d %zu",
			 __PRETTY_FUNCTION__, h->nlmsg_len,
			 (size_t)NLMSG_LENGTH(sizeof(struct nhmsg)));
		return -1;
	}
	memset(tb, 0, sizeof(tb));
	netlink_parse_rtattr(tb, NHA_MAX, RTM_NHA(nhm), len);

	if (tb[NHA_ID])
		zlog_debug("ID: %d", *((int *)RTA_DATA(tb[NHA_ID])));
//	if (tb[NHA_GROUP])
//		zlog_debug("Group: %d", RTA_DATA(tb[NHA_ID]);

	return 0;
}

/**
 * netlink_request_nexthop() - Request nextop information from the kernel
 * @zns:	Zebra namespace
 * @family:	AF_* netlink family
 * @type:	RTM_* route type
 * Return:	Result status
 */
static int netlink_request_nexthop(struct zebra_ns *zns, int family, int type)
{
	struct {
		struct nlmsghdr n;
		struct nhmsg nhm;
	} req;

	/* Form the request, specifying filter (rtattr) if needed. */
	memset(&req, 0, sizeof(req));
	req.n.nlmsg_type = type;
	req.n.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH | NLM_F_REQUEST;
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct nhmsg));
	req.nhm.nh_family = family;

	return netlink_request(&zns->netlink_cmd, &req.n);
}

/**
 * netlink_nexthop_read() - Nexthop read function using netlink interface
 * 
 * @zns:	Zebra name space
 * Return:	Result status
 * Only called at bootstrap time.
 */
int netlink_nexthop_read(struct zebra_ns *zns)
{
	int ret;
	struct zebra_dplane_info dp_info;

	zebra_dplane_info_from_zns(&dp_info, zns, true /*is_cmd*/);

	/* Get nexthop objects */
	ret = netlink_request_nexthop(zns, AF_UNSPEC, RTM_GETNEXTHOP);
	if (ret < 0)
		return ret;
	ret = netlink_parse_info(netlink_nexthop_change_read, &zns->netlink_cmd,
				 &dp_info, 0, 1);
	return 0;
}


#endif /* HAVE_NETLINK */
