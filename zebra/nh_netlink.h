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

#ifndef _ZEBRA_NH_NETLINK_H
#define _ZEBRA_NH_NETLINK_H

#ifdef HAVE_NETLINK

#include "zebra/zebra_dplane.h"
#include "zebra/kernel_netlink.h"

extern int netlink_nexthop_read(struct zebra_ns *zns);


#endif /* HAVE_NETLINK */
#endif /* _ZEBRA_NH_NETLINK_H */
