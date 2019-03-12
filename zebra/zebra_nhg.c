/* Zebra NHG Code.
 * Copyright (C) 2019 Cumulus Networks, Inc.
 *                    Donald Sharp
 *
 * This file is part of FRR.
 *
 * FRR is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * FRR is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FRR; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include "zebra.h"

#include "nexthop.h"
#include "nexthop_group.h"
#include "jhash.h"
#include "routemap.h"

#include "connected.h"
#include "debug.h"
#include "zebra_router.h"
#include "zebra_nhg.h"
#include "zebra_rnh.h"
#include "zebra_routemap.h"
#include "zserv.h"
#include "zebra_errors.h"

/**
 * zebra_nhg_lookup_id() - Lookup the nexthop group id in the id table
 *
 * @id:		ID to look for
 *
 * Return:	Nexthop hash entry if found/NULL if not found
 */
struct nhg_hash_entry *zebra_nhg_lookup_id(uint32_t id)
{
	struct nhg_hash_entry lookup = {0};

	lookup.id = id;
	return hash_lookup(zrouter.nhgs_id, &lookup);
}

/**
 * zebra_nhg_insert_id() - Insert a nhe into the id hashed table
 *
 * @nhe:	The entry directly from the other table
 *
 * Return:	Result status
 */
int zebra_nhg_insert_id(struct nhg_hash_entry *nhe)
{
	if (hash_lookup(zrouter.nhgs_id, nhe)) {
		flog_err(
			EC_ZEBRA_NHG_TABLE_INSERT_FAILED,
			"Failed inserting NHG id=%u into the ID hash table, entry already exists",
			nhe->id);
		return -1;
	}

	hash_get(zrouter.nhgs_id, nhe, hash_alloc_intern);

	return 0;
}

static void *zebra_nhg_alloc(void *arg)
{
	/* lock for getiing and setting the id */
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	/* id counter to keep in sync with kernel */
	static uint32_t id_counter = 0;
	struct nhg_hash_entry *nhe;
	struct nhg_hash_entry *copy = arg;

	nhe = XCALLOC(MTYPE_TMP, sizeof(struct nhg_hash_entry));

	pthread_mutex_lock(&lock); /* Lock, set the id counter from kernel */
	if (copy->id) {
		/* This is from the kernel if it has an id */
		if (copy->id > id_counter) {
			/* Increase our counter so we don't try to create
			 * an ID that already exists
			 */
			id_counter = copy->id;
		}
		nhe->id = copy->id;
		/* Mark as valid since from the kernel */
		SET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
		SET_FLAG(nhe->flags, NEXTHOP_GROUP_VALID);
	} else {
		nhe->id = ++id_counter;
	}
	pthread_mutex_unlock(&lock);

	nhe->vrf_id = copy->vrf_id;
	nhe->refcnt = 0;
	nhe->is_kernel_nh = false;
	nhe->dplane_ref = zebra_router_get_next_sequence();
	nhe->ifp = NULL;
	nhe->nhg.nexthop = NULL;

	nexthop_group_copy(&nhe->nhg, &copy->nhg);

	/* Add to id table as well */
	zebra_nhg_insert_id(nhe);


	return nhe;
}

static uint32_t zebra_nhg_hash_key_nexthop_group(struct nexthop_group *nhg)
{
	struct nexthop *nh;
	uint32_t i;
	uint32_t key = 0;

	/*
	 * We are not interested in hashing over any recursively
	 * resolved nexthops
	 */
	for (nh = nhg->nexthop; nh; nh = nh->next) {
		key = jhash_1word(nh->type, key);
		key = jhash_2words(nh->vrf_id, nh->nh_label_type, key);
		/* gate and blackhole are together in a union */
		key = jhash(&nh->gate, sizeof(nh->gate), key);
		key = jhash(&nh->src, sizeof(nh->src), key);
		key = jhash(&nh->rmap_src, sizeof(nh->rmap_src), key);
		if (nh->nh_label) {
			for (i = 0; i < nh->nh_label->num_labels; i++)
				key = jhash_1word(nh->nh_label->label[i], key);
		}
		switch (nh->type) {
		case NEXTHOP_TYPE_IPV4_IFINDEX:
		case NEXTHOP_TYPE_IPV6_IFINDEX:
		case NEXTHOP_TYPE_IFINDEX:
			key = jhash_1word(nh->ifindex, key);
			break;
		case NEXTHOP_TYPE_BLACKHOLE:
		case NEXTHOP_TYPE_IPV4:
		case NEXTHOP_TYPE_IPV6:
			break;
		}
	}
	return key;
}

uint32_t zebra_nhg_hash_key(void *arg)
{
	struct nhg_hash_entry *nhe = arg;

	int key = 0x5a351234;

	key = jhash_1word(nhe->vrf_id, key);

	key = jhash_1word(zebra_nhg_hash_key_nexthop_group(&nhe->nhg), key);


	return key;
}

uint32_t zebra_nhg_id_key(void *arg)
{
	struct nhg_hash_entry *nhe = arg;

	return nhe->id;
}

bool zebra_nhg_hash_equal(const void *arg1, const void *arg2)
{
	const struct nhg_hash_entry *nhe1 = arg1;
	const struct nhg_hash_entry *nhe2 = arg2;
	struct nexthop *nh1, *nh2;
	uint32_t nh_count = 0;

	if (nhe1->vrf_id != nhe2->vrf_id)
		return false;

	/*
	 * Again we are not interested in looking at any recursively
	 * resolved nexthops.  Top level only
	 */
	for (nh1 = nhe1->nhg.nexthop; nh1; nh1 = nh1->next) {
		uint32_t inner_nh_count = 0;
		for (nh2 = nhe2->nhg.nexthop; nh2; nh2 = nh2->next) {
			if (inner_nh_count == nh_count) {
				break;
			}
			inner_nh_count++;
		}

		if (!nexthop_same(nh1, nh2))
			return false;

		nh_count++;
	}

	return true;
}

bool zebra_nhg_hash_id_equal(const void *arg1, const void *arg2)
{
	const struct nhg_hash_entry *nhe1 = arg1;
	const struct nhg_hash_entry *nhe2 = arg2;

	return nhe1->id == nhe2->id;
}

struct nhg_hash_entry *zebra_nhg_find_id(uint32_t id, struct nexthop_group *nhg)
{
	// TODO: How this will work is yet to be determined
	return NULL;
}

/**
 * zebra_nhg_find() - Find the zebra nhg in our table, or create it
 *
 * @nhg:	Nexthop group we lookup with
 * @vrf_id:	VRF id
 * @id:		ID we lookup with, 0 means its from us and we need to give it
 * 		an ID, otherwise its from the kernel as we use the ID it gave
 * 		us.
 *
 * Return:	Hash entry found or created
 */
struct nhg_hash_entry *zebra_nhg_find(struct nexthop_group *nhg,
				      vrf_id_t vrf_id, uint32_t id)
{
	struct nhg_hash_entry lookup = {0};
	struct nhg_hash_entry *nhe = NULL;

	lookup.id = id;
	lookup.vrf_id = vrf_id;
	lookup.nhg = *nhg;


	nhe = hash_lookup(zrouter.nhgs, &lookup);

	if (!nhe) {
		nhe = hash_get(zrouter.nhgs, &lookup, zebra_nhg_alloc);
	} else {
		if (id) {
			/* Duplicate but with different ID from the kernel */

			/* The kernel allows duplicate nexthops as long as they
			 * have different IDs. We are ignoring those to prevent
			 * syncing problems with the kernel changes.
			 */
			flog_warn(
				EC_ZEBRA_DUPLICATE_NHG_MESSAGE,
				"Nexthop Group from kernel with ID (%d) is a duplicate, ignoring",
				id);
			return NULL;
		}
	}

	return nhe;
}

/**
 * zebra_nhg_free() - Free the nexthop group hash entry
 *
 * arg:	Nexthop group entry to free
 */
void zebra_nhg_free(void *arg)
{
	struct nhg_hash_entry *nhe = NULL;

	nhe = (struct nhg_hash_entry *)arg;

	nexthops_free(nhe->nhg.nexthop);
	XFREE(MTYPE_TMP, nhe);
}

/**
 * zebra_nhg_release() - Release a nhe from the tables
 *
 * @nhe:	Nexthop group hash entry
 */
void zebra_nhg_release(struct nhg_hash_entry *nhe)
{
	if (nhe->refcnt)
		flog_err(
			EC_ZEBRA_NHG_SYNC,
			"Releasing a nexthop group with ID (%u) that we are still using for a route",
			nhe->id);

	hash_release(zrouter.nhgs, nhe);
	hash_release(zrouter.nhgs_id, nhe);
	zebra_nhg_free(nhe);
}

/**
 * zebra_nhg_decrement_ref() - Decrement the reference count, release if unused
 *
 * @nhe:	Nexthop group hash entry
 *
 * If the counter hits 0 and is not a nexthop group that was created by the
 * kernel, we don't need to have it in our table anymore.
 */
void zebra_nhg_decrement_ref(struct nhg_hash_entry *nhe)
{
	nhe->refcnt--;

	if (!nhe->is_kernel_nh && nhe->refcnt <= 0) {
		zebra_nhg_uninstall_kernel(nhe);
	}

	// re->ng = NULL;
}

static void nexthop_set_resolved(afi_t afi, const struct nexthop *newhop,
				 struct nexthop *nexthop)
{
	struct nexthop *resolved_hop;

	resolved_hop = nexthop_new();
	SET_FLAG(resolved_hop->flags, NEXTHOP_FLAG_ACTIVE);

	resolved_hop->vrf_id = nexthop->vrf_id;
	switch (newhop->type) {
	case NEXTHOP_TYPE_IPV4:
	case NEXTHOP_TYPE_IPV4_IFINDEX:
		/* If the resolving route specifies a gateway, use it */
		resolved_hop->type = newhop->type;
		resolved_hop->gate.ipv4 = newhop->gate.ipv4;

		if (newhop->ifindex) {
			resolved_hop->type = NEXTHOP_TYPE_IPV4_IFINDEX;
			resolved_hop->ifindex = newhop->ifindex;
		}
		break;
	case NEXTHOP_TYPE_IPV6:
	case NEXTHOP_TYPE_IPV6_IFINDEX:
		resolved_hop->type = newhop->type;
		resolved_hop->gate.ipv6 = newhop->gate.ipv6;

		if (newhop->ifindex) {
			resolved_hop->type = NEXTHOP_TYPE_IPV6_IFINDEX;
			resolved_hop->ifindex = newhop->ifindex;
		}
		break;
	case NEXTHOP_TYPE_IFINDEX:
		/* If the resolving route is an interface route,
		 * it means the gateway we are looking up is connected
		 * to that interface. (The actual network is _not_ onlink).
		 * Therefore, the resolved route should have the original
		 * gateway as nexthop as it is directly connected.
		 *
		 * On Linux, we have to set the onlink netlink flag because
		 * otherwise, the kernel won't accept the route.
		 */
		resolved_hop->flags |= NEXTHOP_FLAG_ONLINK;
		if (afi == AFI_IP) {
			resolved_hop->type = NEXTHOP_TYPE_IPV4_IFINDEX;
			resolved_hop->gate.ipv4 = nexthop->gate.ipv4;
		} else if (afi == AFI_IP6) {
			resolved_hop->type = NEXTHOP_TYPE_IPV6_IFINDEX;
			resolved_hop->gate.ipv6 = nexthop->gate.ipv6;
		}
		resolved_hop->ifindex = newhop->ifindex;
		break;
	case NEXTHOP_TYPE_BLACKHOLE:
		resolved_hop->type = NEXTHOP_TYPE_BLACKHOLE;
		resolved_hop->bh_type = nexthop->bh_type;
		break;
	}

	if (newhop->flags & NEXTHOP_FLAG_ONLINK)
		resolved_hop->flags |= NEXTHOP_FLAG_ONLINK;

	/* Copy labels of the resolved route */
	if (newhop->nh_label)
		nexthop_add_labels(resolved_hop, newhop->nh_label_type,
				   newhop->nh_label->num_labels,
				   &newhop->nh_label->label[0]);

	resolved_hop->rparent = nexthop;
	nexthop_add(&nexthop->resolved, resolved_hop);
}

/* If force flag is not set, do not modify falgs at all for uninstall
   the route from FIB. */
static int nexthop_active(afi_t afi, struct route_entry *re,
			  struct nexthop *nexthop, bool set,
			  struct route_node *top)
{
	struct prefix p;
	struct route_table *table;
	struct route_node *rn;
	struct route_entry *match = NULL;
	int resolved;
	struct nexthop *newhop;
	struct interface *ifp;
	rib_dest_t *dest;

	if ((nexthop->type == NEXTHOP_TYPE_IPV4)
	    || nexthop->type == NEXTHOP_TYPE_IPV6)
		nexthop->ifindex = 0;

	if (set) {
		UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_RECURSIVE);
		nexthops_free(nexthop->resolved);
		nexthop->resolved = NULL;
		re->nexthop_mtu = 0;
	}

	/*
	 * If the kernel has sent us a route, then
	 * by golly gee whiz it's a good route.
	 */
	if (re->type == ZEBRA_ROUTE_KERNEL ||
	    re->type == ZEBRA_ROUTE_SYSTEM)
		return 1;

	/* Skip nexthops that have been filtered out due to route-map */
	/* The nexthops are specific to this route and so the same */
	/* nexthop for a different route may not have this flag set */
	if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_FILTERED)) {
		if (IS_ZEBRA_DEBUG_RIB_DETAILED)
			zlog_debug("\t%s: Nexthop Filtered",
				   __PRETTY_FUNCTION__);
		return 0;
	}

	/*
	 * Check to see if we should trust the passed in information
	 * for UNNUMBERED interfaces as that we won't find the GW
	 * address in the routing table.
	 * This check should suffice to handle IPv4 or IPv6 routes
	 * sourced from EVPN routes which are installed with the
	 * next hop as the remote VTEP IP.
	 */
	if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ONLINK)) {
		ifp = if_lookup_by_index(nexthop->ifindex, nexthop->vrf_id);
		if (!ifp) {
			if (IS_ZEBRA_DEBUG_RIB_DETAILED)
				zlog_debug(
					"\t%s: Onlink and interface: %u[%u] does not exist",
					__PRETTY_FUNCTION__, nexthop->ifindex,
					nexthop->vrf_id);
			return 0;
		}
		if (connected_is_unnumbered(ifp)) {
			if (if_is_operative(ifp))
				return 1;
			else {
				if (IS_ZEBRA_DEBUG_RIB_DETAILED)
					zlog_debug(
						"\t%s: Onlink and interface %s is not operative",
						__PRETTY_FUNCTION__, ifp->name);
				return 0;
			}
		}
		if (!if_is_operative(ifp)) {
			if (IS_ZEBRA_DEBUG_RIB_DETAILED)
				zlog_debug(
					"\t%s: Interface %s is not unnumbered",
					__PRETTY_FUNCTION__,
					ifp ? ifp->name : "Unknown");
			return 0;
		}
	}

	/* Make lookup prefix. */
	memset(&p, 0, sizeof(struct prefix));
	switch (afi) {
	case AFI_IP:
		p.family = AF_INET;
		p.prefixlen = IPV4_MAX_PREFIXLEN;
		p.u.prefix4 = nexthop->gate.ipv4;
		break;
	case AFI_IP6:
		p.family = AF_INET6;
		p.prefixlen = IPV6_MAX_PREFIXLEN;
		p.u.prefix6 = nexthop->gate.ipv6;
		break;
	default:
		assert(afi != AFI_IP && afi != AFI_IP6);
		break;
	}
	/* Lookup table.  */
	table = zebra_vrf_table(afi, SAFI_UNICAST, nexthop->vrf_id);
	if (!table) {
		if (IS_ZEBRA_DEBUG_RIB_DETAILED)
			zlog_debug("\t%s: Table not found",
				   __PRETTY_FUNCTION__);
		return 0;
	}

	rn = route_node_match(table, (struct prefix *)&p);
	while (rn) {
		route_unlock_node(rn);

		/* Lookup should halt if we've matched against ourselves ('top',
		 * if specified) - i.e., we cannot have a nexthop NH1 is
		 * resolved by a route NH1. The exception is if the route is a
		 * host route.
		 */
		if (top && rn == top)
			if (((afi == AFI_IP) && (rn->p.prefixlen != 32))
			    || ((afi == AFI_IP6) && (rn->p.prefixlen != 128))) {
				if (IS_ZEBRA_DEBUG_RIB_DETAILED)
					zlog_debug(
						"\t%s: Matched against ourself and prefix length is not max bit length",
						__PRETTY_FUNCTION__);
				return 0;
			}

		/* Pick up selected route. */
		/* However, do not resolve over default route unless explicitly
		 * allowed. */
		if (is_default_prefix(&rn->p)
		    && !rnh_resolve_via_default(p.family)) {
			if (IS_ZEBRA_DEBUG_RIB_DETAILED)
				zlog_debug(
					"\t:%s: Resolved against default route",
					__PRETTY_FUNCTION__);
			return 0;
		}

		dest = rib_dest_from_rnode(rn);
		if (dest && dest->selected_fib
		    && !CHECK_FLAG(dest->selected_fib->status,
				   ROUTE_ENTRY_REMOVED)
		    && dest->selected_fib->type != ZEBRA_ROUTE_TABLE)
			match = dest->selected_fib;

		/* If there is no selected route or matched route is EGP, go up
		   tree. */
		if (!match) {
			do {
				rn = rn->parent;
			} while (rn && rn->info == NULL);
			if (rn)
				route_lock_node(rn);

			continue;
		}

		if (match->type == ZEBRA_ROUTE_CONNECT) {
			/* Directly point connected route. */
			newhop = match->ng->nexthop;
			if (newhop) {
				if (nexthop->type == NEXTHOP_TYPE_IPV4
				    || nexthop->type == NEXTHOP_TYPE_IPV6)
					nexthop->ifindex = newhop->ifindex;
			}
			return 1;
		} else if (CHECK_FLAG(re->flags, ZEBRA_FLAG_ALLOW_RECURSION)) {
			resolved = 0;
			for (ALL_NEXTHOPS_PTR(match->ng, newhop)) {
				if (!CHECK_FLAG(match->status,
						ROUTE_ENTRY_INSTALLED))
					continue;
				if (CHECK_FLAG(newhop->flags,
					       NEXTHOP_FLAG_RECURSIVE))
					continue;

				if (set) {
					SET_FLAG(nexthop->flags,
						 NEXTHOP_FLAG_RECURSIVE);
					SET_FLAG(re->status,
						 ROUTE_ENTRY_NEXTHOPS_CHANGED);
					nexthop_set_resolved(afi, newhop,
							     nexthop);
				}
				resolved = 1;
			}
			if (resolved && set)
				re->nexthop_mtu = match->mtu;
			if (!resolved && IS_ZEBRA_DEBUG_RIB_DETAILED)
				zlog_debug("\t%s: Recursion failed to find",
					   __PRETTY_FUNCTION__);
			return resolved;
		} else if (re->type == ZEBRA_ROUTE_STATIC) {
			resolved = 0;
			for (ALL_NEXTHOPS_PTR(match->ng, newhop)) {
				if (!CHECK_FLAG(match->status,
						ROUTE_ENTRY_INSTALLED))
					continue;
				if (CHECK_FLAG(newhop->flags,
					       NEXTHOP_FLAG_RECURSIVE))
					continue;

				if (set) {
					SET_FLAG(nexthop->flags,
						 NEXTHOP_FLAG_RECURSIVE);
					nexthop_set_resolved(afi, newhop,
							     nexthop);
				}
				resolved = 1;
			}
			if (resolved && set)
				re->nexthop_mtu = match->mtu;

			if (!resolved && IS_ZEBRA_DEBUG_RIB_DETAILED)
				zlog_debug(
					"\t%s: Static route unable to resolve",
					__PRETTY_FUNCTION__);
			return resolved;
		} else {
			if (IS_ZEBRA_DEBUG_RIB_DETAILED) {
				zlog_debug("\t%s: Route Type %s has not turned on recursion",
					   __PRETTY_FUNCTION__,
					   zebra_route_string(re->type));
				if (re->type == ZEBRA_ROUTE_BGP &&
				    !CHECK_FLAG(re->flags, ZEBRA_FLAG_IBGP))
					zlog_debug("\tEBGP: see \"disable-ebgp-connected-route-check\" or \"disable-connected-check\"");
			}
			return 0;
		}
	}
	if (IS_ZEBRA_DEBUG_RIB_DETAILED)
		zlog_debug("\t%s: Nexthop did not lookup in table",
			   __PRETTY_FUNCTION__);
	return 0;
}

/* This function verifies reachability of one given nexthop, which can be
 * numbered or unnumbered, IPv4 or IPv6. The result is unconditionally stored
 * in nexthop->flags field. If the 4th parameter, 'set', is non-zero,
 * nexthop->ifindex will be updated appropriately as well.
 * An existing route map can turn (otherwise active) nexthop into inactive, but
 * not vice versa.
 *
 * The return value is the final value of 'ACTIVE' flag.
 */

static unsigned nexthop_active_check(struct route_node *rn,
				     struct route_entry *re,
				     struct nexthop *nexthop, bool set)
{
	struct interface *ifp;
	route_map_result_t ret = RMAP_MATCH;
	int family;
	char buf[SRCDEST2STR_BUFFER];
	const struct prefix *p, *src_p;
	struct zebra_vrf *zvrf;

	srcdest_rnode_prefixes(rn, &p, &src_p);

	if (rn->p.family == AF_INET)
		family = AFI_IP;
	else if (rn->p.family == AF_INET6)
		family = AFI_IP6;
	else
		family = 0;
	switch (nexthop->type) {
	case NEXTHOP_TYPE_IFINDEX:
		ifp = if_lookup_by_index(nexthop->ifindex, nexthop->vrf_id);
		if (ifp && if_is_operative(ifp))
			SET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		else
			UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		break;
	case NEXTHOP_TYPE_IPV4:
	case NEXTHOP_TYPE_IPV4_IFINDEX:
		family = AFI_IP;
		if (nexthop_active(AFI_IP, re, nexthop, set, rn))
			SET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		else
			UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		break;
	case NEXTHOP_TYPE_IPV6:
		family = AFI_IP6;
		if (nexthop_active(AFI_IP6, re, nexthop, set, rn))
			SET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		else
			UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		break;
	case NEXTHOP_TYPE_IPV6_IFINDEX:
		/* RFC 5549, v4 prefix with v6 NH */
		if (rn->p.family != AF_INET)
			family = AFI_IP6;
		if (IN6_IS_ADDR_LINKLOCAL(&nexthop->gate.ipv6)) {
			ifp = if_lookup_by_index(nexthop->ifindex,
						 nexthop->vrf_id);
			if (ifp && if_is_operative(ifp))
				SET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
			else
				UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		} else {
			if (nexthop_active(AFI_IP6, re, nexthop, set, rn))
				SET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
			else
				UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		}
		break;
	case NEXTHOP_TYPE_BLACKHOLE:
		SET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		break;
	default:
		break;
	}
	if (!CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE)) {
		if (IS_ZEBRA_DEBUG_RIB_DETAILED)
			zlog_debug("\t%s: Unable to find a active nexthop",
				   __PRETTY_FUNCTION__);
		return 0;
	}

	/* XXX: What exactly do those checks do? Do we support
	 * e.g. IPv4 routes with IPv6 nexthops or vice versa?
	 */
	if (RIB_SYSTEM_ROUTE(re) || (family == AFI_IP && p->family != AF_INET)
	    || (family == AFI_IP6 && p->family != AF_INET6))
		return CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);

	/* The original code didn't determine the family correctly
	 * e.g. for NEXTHOP_TYPE_IFINDEX. Retrieve the correct afi
	 * from the rib_table_info in those cases.
	 * Possibly it may be better to use only the rib_table_info
	 * in every case.
	 */
	if (!family) {
		rib_table_info_t *info;

		info = srcdest_rnode_table_info(rn);
		family = info->afi;
	}

	memset(&nexthop->rmap_src.ipv6, 0, sizeof(union g_addr));

	zvrf = zebra_vrf_lookup_by_id(nexthop->vrf_id);
	if (!zvrf) {
		if (IS_ZEBRA_DEBUG_RIB_DETAILED)
			zlog_debug("\t%s: zvrf is NULL", __PRETTY_FUNCTION__);
		return CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
	}

	/* It'll get set if required inside */
	ret = zebra_route_map_check(family, re->type, re->instance, p,
				    nexthop, zvrf, re->tag);
	if (ret == RMAP_DENYMATCH) {
		if (IS_ZEBRA_DEBUG_RIB) {
			srcdest_rnode2str(rn, buf, sizeof(buf));
			zlog_debug(
				"%u:%s: Filtering out with NH out %s due to route map",
				re->vrf_id, buf,
				ifindex2ifname(nexthop->ifindex,
					       nexthop->vrf_id));
		}
		UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
	}
	return CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
}

/*
 * Iterate over all nexthops of the given RIB entry and refresh their
 * ACTIVE flag.  If any nexthop is found to toggle the ACTIVE flag,
 * the whole re structure is flagged with ROUTE_ENTRY_CHANGED.
 * The 4th 'set' argument is transparently passed to nexthop_active_check().
 *
 * Return value is the new number of active nexthops.
 */

int nexthop_active_update(struct route_node *rn, struct route_entry *re,
			  bool set)
{
	struct nexthop *nexthop;
	union g_addr prev_src;
	unsigned int prev_active, new_active, old_num_nh;
	ifindex_t prev_index;
	uint8_t curr_active = 0;

	old_num_nh = nexthop_group_active_nexthop_num(re->ng);

	UNSET_FLAG(re->status, ROUTE_ENTRY_CHANGED);

	for (nexthop = re->ng->nexthop; nexthop; nexthop = nexthop->next) {
		/* No protocol daemon provides src and so we're skipping
		 * tracking it */
		prev_src = nexthop->rmap_src;
		prev_active = CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
		prev_index = nexthop->ifindex;
		/*
		 * We need to respect the multipath_num here
		 * as that what we should be able to install from
		 * a multipath perpsective should not be a data plane
		 * decision point.
		 */
		new_active = nexthop_active_check(rn, re, nexthop, set);
		if (new_active
		    && nexthop_group_active_nexthop_num(re->ng)
			       >= zrouter.multipath_num) {
			UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE);
			new_active = 0;
		}

		if (new_active)
			curr_active++;

		/* Don't allow src setting on IPv6 addr for now */
		if (prev_active != new_active || prev_index != nexthop->ifindex
		    || ((nexthop->type >= NEXTHOP_TYPE_IFINDEX
			 && nexthop->type < NEXTHOP_TYPE_IPV6)
			&& prev_src.ipv4.s_addr
				   != nexthop->rmap_src.ipv4.s_addr)
		    || ((nexthop->type >= NEXTHOP_TYPE_IPV6
			 && nexthop->type < NEXTHOP_TYPE_BLACKHOLE)
			&& !(IPV6_ADDR_SAME(&prev_src.ipv6,
					    &nexthop->rmap_src.ipv6)))) {
			SET_FLAG(re->status, ROUTE_ENTRY_CHANGED);
			SET_FLAG(re->status, ROUTE_ENTRY_NEXTHOPS_CHANGED);
		}
	}

	if (old_num_nh != curr_active)
		SET_FLAG(re->status, ROUTE_ENTRY_CHANGED);

	if (CHECK_FLAG(re->status, ROUTE_ENTRY_CHANGED)) {
		SET_FLAG(re->status, ROUTE_ENTRY_NEXTHOPS_CHANGED);
	}

	return curr_active;
}

static wq_item_status zebra_nhg_process(struct work_queue *item,
					void *data)
{
	return WQ_SUCCESS;
}

static void zebra_nhg_new(const char *name)
{
}

static void zebra_nhg_add_nexthop(const struct nexthop_group_cmd *nhgc,
				  const struct nexthop *nhop)
{
}

static void zebra_nhg_del_nexthop(const struct nexthop_group_cmd *nhgc,
				  const struct nexthop *nhop)
{
}

static void zebra_nhg_delete(const char *name)
{
}

/**
 * zebra_nhg_install_kernel() - Install Nexthop Group hash entry into kernel
 *
 * @nhe:	Nexthop Group hash entry to install
 */
void zebra_nhg_install_kernel(struct nhg_hash_entry *nhe)
{
	if (!CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED)) {
		nhe->is_kernel_nh = false;
		int ret = dplane_nexthop_add(nhe);
		switch (ret) {
		case ZEBRA_DPLANE_REQUEST_QUEUED:
			SET_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED);
			break;
		case ZEBRA_DPLANE_REQUEST_FAILURE:
			flog_err(
				EC_ZEBRA_DP_INSTALL_FAIL,
				"Failed to install Nexthop ID (%u) into the kernel",
				nhe->id);
			break;
		case ZEBRA_DPLANE_REQUEST_SUCCESS:
			SET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
			break;
		}
	}
}

/**
 * zebra_nhg_uninstall_kernel() - Uninstall Nexthop Group hash entry into kernel
 *
 * @nhe:	Nexthop Group hash entry to uninstall
 */
void zebra_nhg_uninstall_kernel(struct nhg_hash_entry *nhe)
{
	if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED)) {
		int ret = dplane_nexthop_delete(nhe);
		switch (ret) {
		case ZEBRA_DPLANE_REQUEST_QUEUED:
			SET_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED);
			break;
		case ZEBRA_DPLANE_REQUEST_FAILURE:
			flog_err(
				EC_ZEBRA_DP_DELETE_FAIL,
				"Failed to uninstall Nexthop ID (%u) from the kernel",
				nhe->id);
			break;
		case ZEBRA_DPLANE_REQUEST_SUCCESS:
			UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
			break;
		}
	}
}

/**
 * zebra_nhg_uninstall_created() - Uninstall nexthops we created in the kernel
 *
 * @nhe:	Nexthop group hash entry
 */
static void zebra_nhg_uninstall_created(struct hash_bucket *bucket, void *arg)
{
	struct nhg_hash_entry *nhe = NULL;

	nhe = (struct nhg_hash_entry *)bucket->data;

	if (nhe && !nhe->is_kernel_nh)
		zebra_nhg_uninstall_kernel(nhe);
}

/**
 * zebra_nhg_cleanup_tables() - Iterate over our tables to uninstall nh's
 * 				we created
 */
void zebra_nhg_cleanup_tables(void)
{
	hash_iterate(zrouter.nhgs, zebra_nhg_uninstall_created, NULL);
}

/**
 * zebra_nhg_dplane_result() - Process dplane result
 *
 * @ctx:	Dataplane context
 */
void zebra_nhg_dplane_result(struct zebra_dplane_ctx *ctx)
{
	enum dplane_op_e op;
	enum zebra_dplane_result status;
	uint32_t id = 0;
	struct nhg_hash_entry *nhe = NULL;

	op = dplane_ctx_get_op(ctx);
	status = dplane_ctx_get_status(ctx);

	id = dplane_ctx_get_nhe(ctx)->id;
	nhe = zebra_nhg_lookup_id(id);

	if (nhe) {
		if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
			zlog_debug(
				"Nexthop dplane ctx %p, op %s, nexthop ID (%u), result %s",
				ctx, dplane_op2str(op), nhe->id,
				dplane_res2str(status));

		switch (op) {
		case DPLANE_OP_NH_DELETE:
			if (status == ZEBRA_DPLANE_REQUEST_SUCCESS) {
				UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
				zebra_nhg_release(nhe);
			} else {
				flog_err(
					EC_ZEBRA_DP_DELETE_FAIL,
					"Failed to uninstall Nexthop ID (%u) from the kernel",
					nhe->id);
			}
			break;
		case DPLANE_OP_NH_INSTALL:
		case DPLANE_OP_NH_UPDATE:
			if (status == ZEBRA_DPLANE_REQUEST_SUCCESS) {
				SET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
			} else {
				flog_err(
					EC_ZEBRA_DP_INSTALL_FAIL,
					"Failed to install Nexthop ID (%u) into the kernel",
					nhe->id);
				UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
			}
			UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED);
			break;
		case DPLANE_OP_ROUTE_INSTALL:
		case DPLANE_OP_ROUTE_UPDATE:
		case DPLANE_OP_ROUTE_DELETE:
		case DPLANE_OP_LSP_INSTALL:
		case DPLANE_OP_LSP_UPDATE:
		case DPLANE_OP_LSP_DELETE:
		case DPLANE_OP_PW_INSTALL:
		case DPLANE_OP_PW_UNINSTALL:
		case DPLANE_OP_NONE:
			break;
		}
		dplane_ctx_fini(&ctx);

	} else {
		flog_err(
			EC_ZEBRA_NHG_SYNC,
			"%s operation preformed on Nexthop ID (%u) in the kernel, that we no longer have in our table",
			dplane_op2str(op), id);
	}
}

void zebra_nhg_init(void)
{
	nexthop_group_init(zebra_nhg_new,
			   zebra_nhg_add_nexthop,
			   zebra_nhg_del_nexthop,
			   zebra_nhg_delete);

	zrouter.nhgq = work_queue_new(zrouter.master,
				      "Nexthop Group Processing");

	zrouter.nhgq->spec.workfunc = &zebra_nhg_process;
	zrouter.nhgq->spec.errorfunc = NULL;
	zrouter.nhgq->spec.completion_func = NULL;
	zrouter.nhgq->spec.max_retries = 3;
	zrouter.nhgq->spec.hold = ZEBRA_NHG_PROCESS_HOLD_TIME;
	zrouter.nhgq->spec.retry = ZEBRA_NHG_PROCESS_RETRY_TIME;
}

void zebra_nhg_terminate(void)
{

	work_queue_free_and_null(&zrouter.nhgq);
	nexthop_group_init(NULL, NULL, NULL, NULL);
}
