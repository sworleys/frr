/* PIM Mlag Code.
 * Copyright (C) 2018 Cumulus Networks, Inc.
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

#include "pimd.h"
#include "pim_mlag.h"
#include "pim_zebra.h"
#include "pim_oil.h"
#include "pim_upstream.h"
#include "pim_vxlan.h"

extern struct zclient *zclient;

#define PIM_MLAG_METADATA_LEN 4

/*********************ACtual Data processing *****************************/
/* TBD: There can be duplicate updates to FIB***/
#define PIM_MLAG_ADD_OIF_TO_OIL(ch, ch_oil)                                    \
	do {                                                                   \
		if (PIM_DEBUG_MLAG)                                            \
			zlog_debug(                                            \
				"%s: add Dual-active Interface to %s "         \
				"to oil:%s",                                   \
				__func__, ch->interface->name, ch->sg_str);    \
		pim_channel_add_oif(ch_oil, ch->interface,                     \
				    PIM_OIF_FLAG_PROTO_IGMP);                  \
		ch->mlag_am_i_df = true;                                       \
	} while (0)

#define PIM_MLAG_DEL_OIF_TO_OIL(ch, ch_oil)                                    \
	do {                                                                   \
		if (PIM_DEBUG_MLAG)                                            \
			zlog_debug(                                            \
				"%s: del Dual-active Interface to %s "         \
				"to oil:%s",                                   \
				__func__, ch->interface->name, ch->sg_str);    \
		pim_channel_del_oif(ch_oil, ch->interface,                     \
				    PIM_OIF_FLAG_PROTO_IGMP);                  \
		ch->mlag_am_i_df = false;                                      \
	} while (0)


#define PIM_MLAG_UPDATE_OIL_BASED_ON_DR(pim_ifp, ch, ch_oil)                   \
	do {                                                                   \
		if (PIM_I_am_DR(pim_ifp))                                      \
			PIM_MLAG_ADD_OIF_TO_OIL(ch, ch_oil);                   \
		else                                                           \
			PIM_MLAG_DEL_OIF_TO_OIL(ch, ch_oil);                   \
	} while (0)

#define PIM_MLAG_UPDATE_OIL_BASED_ON_MLAG_ROLE(ch, ch_oil)                     \
	do {                                                                   \
		if (router->mlag_role == MLAG_ROLE_PRIMARY)                    \
			PIM_MLAG_ADD_OIF_TO_OIL(ch, ch_oil);                   \
		else                                                           \
			PIM_MLAG_DEL_OIF_TO_OIL(ch, ch_oil);                   \
	} while (0)


static void pim_mlag_calculate_df_for_ifchannel(struct pim_ifchannel *ch)
{
	struct pim_interface *pim_ifp = NULL;
	struct pim_upstream *upstream = ch->upstream;
	struct channel_oil *ch_oil = NULL;

	pim_ifp = (ch->interface) ? ch->interface->info : NULL;
	ch_oil = (upstream) ? upstream->channel_oil : NULL;

	if (!pim_ifp || !upstream || !ch_oil)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Calculating DF for Dual active if-channel%s",
			   __func__, ch->sg_str);

	/* Standalone mode: Traffic will not be forwarded */
	if (router->mlag_role == MLAG_ROLE_NONE) {
		PIM_MLAG_UPDATE_OIL_BASED_ON_MLAG_ROLE(ch, ch_oil);
		return;
	}

	/* Local Interface is not configured with Dual active */
	if (!PIM_I_am_DualActive(pim_ifp)
	    || ch->mlag_peer_is_dual_active == false) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: MLAG config miss local:%d, peer:%d",
				   __func__, PIM_I_am_DualActive(pim_ifp),
				   ch->mlag_peer_is_dual_active);
		PIM_MLAG_UPDATE_OIL_BASED_ON_MLAG_ROLE(ch, ch_oil);
		return;
	}

	if (ch->mlag_local_cost_to_rp != ch->mlag_peer_cost_to_rp) {
		if (PIM_DEBUG_MLAG)
			zlog_debug(
				"%s: Cost_to_rp  is not same local:%u, peer:%u",
				__func__, ch->mlag_local_cost_to_rp,
				ch->mlag_peer_cost_to_rp);
		if (ch->mlag_local_cost_to_rp < ch->mlag_peer_cost_to_rp)
			/* My cost to RP is better then peer */
			PIM_MLAG_ADD_OIF_TO_OIL(ch, ch_oil);
		else
			PIM_MLAG_DEL_OIF_TO_OIL(ch, ch_oil);
	} else {
		/* Cost is same, Tie break is MLAG Role */
		PIM_MLAG_UPDATE_OIL_BASED_ON_MLAG_ROLE(ch, ch_oil);
	}
}


/******************POsting Local data to peer****************************/

void pim_mlag_add_entry_to_peer(struct pim_ifchannel *ch)
{
	struct stream *s = NULL;
	struct pim_interface *pim_ifp = ch->interface->info;
	struct vrf *vrf = vrf_lookup_by_id(ch->interface->vrf_id);

	if (router->connected_to_mlag == false) {
		/* Not connected to peer, update FIB based on DR role*/
		pim_mlag_calculate_df_for_ifchannel(ch);
		return;
	}

	s = stream_new(MLAG_MROUTE_ADD_MSGSIZE + PIM_MLAG_METADATA_LEN);
	if (!s)
		return;

	stream_putl(s, MLAG_MROUTE_ADD);
	stream_put(s, vrf->name, VRF_NAMSIZ);
	stream_putl(s, htonl(ch->sg.src.s_addr));
	stream_putl(s, htonl(ch->sg.grp.s_addr));
	stream_putl(s, ch->mlag_local_cost_to_rp);
	stream_putl(s, MLAG_OWNER_INTERFACE);
	stream_putc(s, PIM_I_am_DR(pim_ifp));
	stream_putc(s, PIM_I_am_DualActive(pim_ifp));
	stream_putl(s, ch->interface->vrf_id);
	stream_put(s, ch->interface->name, INTERFACE_NAMSIZ);

	stream_fifo_push_safe(router->mlag_fifo, s);
	pim_mlag_signal_zpthread();

	pim_mlag_calculate_df_for_ifchannel(ch);
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Enqueued MLAG Route add for %s", __func__,
			   ch->sg_str);
}

/*
 * The iNtention of posting Delete is to clean teh DB at MLAGD
 */
void pim_mlag_del_entry_to_peer(struct pim_ifchannel *ch)
{
	struct stream *s = NULL;
	struct vrf *vrf = vrf_lookup_by_id(ch->interface->vrf_id);

	s = stream_new(MLAG_MROUTE_DEL_MSGSIZE + PIM_MLAG_METADATA_LEN);
	if (!s)
		return;

	stream_putl(s, MLAG_MROUTE_DEL);
	stream_put(s, vrf->name, VRF_NAMSIZ);
	stream_putl(s, htonl(ch->sg.src.s_addr));
	stream_putl(s, htonl(ch->sg.grp.s_addr));
	stream_putl(s, MLAG_OWNER_INTERFACE);
	stream_putl(s, ch->interface->vrf_id);
	stream_put(s, ch->interface->name, INTERFACE_NAMSIZ);

	stream_fifo_push_safe(router->mlag_fifo, s);
	pim_mlag_signal_zpthread();

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Enqueued MLAG Route del for %s", __func__,
			   ch->sg_str);
}

/******************End of posting local data to peer ********************/

/******************************* pim upstream sync **************************/
/* Update DF role for the upstream entry and return true on role change */
static bool pim_mlag_up_df_role_update(struct pim_upstream *up,
		bool is_df, const char *reason)
{
	struct channel_oil *c_oil = up->channel_oil;
	bool old_is_df = !PIM_UPSTREAM_FLAG_TEST_MLAG_NON_DF(up->flags);

	if (is_df == old_is_df)
		return false;

	if (PIM_DEBUG_MLAG)
		zlog_debug("local MLAG mroute %s role changed to %s based on %s",
				up->sg_str, is_df ? "df" : "non-df", reason);

	if (is_df)
		PIM_UPSTREAM_FLAG_UNSET_MLAG_NON_DF(up->flags);
	else
		PIM_UPSTREAM_FLAG_SET_MLAG_NON_DF(up->flags);

	/* If the DF role has changed re-install OIL. Active-Active devices
	 * and vxlan termination device (ipmr-lo) are suppressed on the non-DF.
	 * This may leave the mroute with the empty OIL in which case the
	 * the forwarding entry's sole purpose is to just blackhole the flow
	 * headed to the switch.
	 */
	if (c_oil && c_oil->installed)
		pim_mroute_add(c_oil, __PRETTY_FUNCTION__);
	return true;
}

/* Run per-upstream entry DF election and return true on role change */
static bool pim_mlag_up_df_role_elect(struct pim_upstream *up)
{
	bool is_df;
	uint32_t remote_cost;
	uint32_t local_cost;
	bool rv;

	if (!pim_up_mlag_is_local(up))
		return false;

	/* We are yet to rx a status update from the local MLAG daemon so
	 * we will assume DF status.
	 */
	if (!(router->mlag_flags & PIM_MLAGF_STATUS_RXED))
		return pim_mlag_up_df_role_update(up,
				true /*is_df*/, "mlagd-down");

	/* If not connected to peer assume DF role on the MLAG primary
	 * switch (and non-DF on the secondary switch.
	 */
	if (!(router->mlag_flags & PIM_MLAGF_REMOTE_CONN_UP)) {
		is_df = (router->mlag_role == MLAG_ROLE_PRIMARY) ? true : false;
		return pim_mlag_up_df_role_update(up,
				is_df, "peer-down");
	}

	/* If we are connected to peer switch but don't have a mroute
	 * from it we have to assume non-DF role to avoid duplicates.
	 * Note: When the peer connection comes up we wait for initial
	 * replay to complete before moving "strays" i.e. local-mlag-mroutes
	 * without a remote reference to non-df role.
	 */
	if (!PIM_UPSTREAM_FLAG_TEST_MLAG_PEER(up->flags))
		return pim_mlag_up_df_role_update(up,
				false /*is_df*/, "no-peer-mroute");

	/* switch with the lowest RPF cost wins. if both switches have the same
	 * cost MLAG role is used as a tie breaker (MLAG primary wins).
	 */
	remote_cost = up->mlag.peer_mrib_metric;
	local_cost = pim_up_mlag_local_cost(up);
	if (local_cost == remote_cost) {
		is_df = (router->mlag_role == MLAG_ROLE_PRIMARY) ? true : false;
		rv = pim_mlag_up_df_role_update(up, is_df, "equal-cost");
	} else {
		is_df = (local_cost < remote_cost) ? true : false;
		rv = pim_mlag_up_df_role_update(up, is_df, "cost");
	}

	return rv;
}

/* Handle upstream entry add from the peer MLAG switch -
 * - if a local entry doesn't exist one is created with reference
 *   _MLAG_PEER
 * - if a local entry exists and has a MLAG OIF DF election is run.
 *   the non-DF switch stop forwarding traffic to MLAG devices.
 */
static void pim_mlag_up_remote_add(struct mlag_mroute_add *msg)
{
	struct pim_upstream *up;
	struct pim_instance *pim;
	int flags = 0;
	struct prefix_sg sg;
	struct vrf *vrf;
	char sg_str[PIM_SG_LEN];

	memset(&sg, 0, sizeof(struct prefix_sg));
	sg.src.s_addr = htonl(msg->source_ip);
	sg.grp.s_addr = htonl(msg->group_ip);
	if (PIM_DEBUG_MLAG)
		pim_str_sg_set(&sg, sg_str);

	if (PIM_DEBUG_MLAG)
		zlog_debug("remote MLAG mroute add %s:%s cost %d",
			msg->vrf_name, sg_str, msg->cost_to_rp);

	/* XXX - this is not correct. we MUST cache updates to avoid losing
	 * an entry because of race conditions with the peer switch.
	 */
	vrf = vrf_lookup_by_name(msg->vrf_name);
	if  (!vrf) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("remote MLAG mroute add failed %s:%s; no vrf",
					msg->vrf_name, sg_str);
		return;
	}
	pim = vrf->info;

	up = pim_upstream_find(pim, &sg);
	if (up) {
		/* upstream already exists; create peer reference if it
		 * doesn't already exist.
		 */
		if (!PIM_UPSTREAM_FLAG_TEST_MLAG_PEER(up->flags))
			pim_upstream_ref(pim, up,
					PIM_UPSTREAM_FLAG_MASK_MLAG_PEER,
					__PRETTY_FUNCTION__);
	} else {
		PIM_UPSTREAM_FLAG_SET_MLAG_PEER(flags);
		up = pim_upstream_add(pim, &sg, NULL /*iif*/, flags,
				__PRETTY_FUNCTION__, NULL /*if_ch*/);

		if (!up) {
			if (PIM_DEBUG_MLAG)
				zlog_debug("remote MLAG mroute add failed %s:%s",
						vrf->name, sg_str);
			return;
		}
	}
	up->mlag.peer_mrib_metric = msg->cost_to_rp;
	pim_mlag_up_df_role_elect(up);
}

/* Handle upstream entry del from the peer MLAG switch -
 * - peer reference is removed. this can result in the upstream
 *   being deleted altogether.
 * - if a local entry continues to exisy and has a MLAG OIF DF election
 *   is re-run (at the end of which the local entry will be the DF).
 */
static void pim_mlag_up_remote_deref(struct pim_instance *pim,
		struct pim_upstream *up)
{
	if (!PIM_UPSTREAM_FLAG_TEST_MLAG_PEER(up->flags))
		return;

	PIM_UPSTREAM_FLAG_UNSET_MLAG_PEER(up->flags);
	up = pim_upstream_del(pim, up, __PRETTY_FUNCTION__);
	if (up)
		pim_mlag_up_df_role_elect(up);
}
static void pim_mlag_up_remote_del(struct mlag_mroute_del *msg)
{
	struct pim_upstream *up;
	struct pim_instance *pim;
	struct prefix_sg sg;
	struct vrf *vrf;
	char sg_str[PIM_SG_LEN];

	memset(&sg, 0, sizeof(struct prefix_sg));
	sg.src.s_addr = htonl(msg->source_ip);
	sg.grp.s_addr = htonl(msg->group_ip);
	if (PIM_DEBUG_MLAG)
		pim_str_sg_set(&sg, sg_str);

	if (PIM_DEBUG_MLAG)
		zlog_debug("remote MLAG mroute del %s:%s", msg->vrf_name,
				sg_str);

	vrf = vrf_lookup_by_name(msg->vrf_name);
	if  (!vrf) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("remote MLAG mroute del skipped %s:%s; no vrf",
					msg->vrf_name, sg_str);
		return;
	}
	pim = vrf->info;

	up = pim_upstream_find(pim, &sg);
	if  (!up) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("remote MLAG mroute del skipped %s:%s; no up",
					vrf->name, sg_str);
		return;
	}

	pim_mlag_up_remote_deref(pim, up);
}

/* When we lose connection to the local MLAG daemon we can drop all remote
 * references.
 */
static void pim_mlag_up_remote_del_all(void)
{
	struct listnode *upnode;
	struct listnode *nextnode;
	struct pim_upstream *up;
	struct vrf *vrf;
	struct pim_instance *pim;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name) {
		pim = vrf->info;
		for (ALL_LIST_ELEMENTS(pim->upstream_list, upnode,
			nextnode, up)) {
			pim_mlag_up_remote_deref(pim, up);
		}
	}
}

/* Send upstream entry to the local MLAG daemon (which will subsequently
 * send it to the peer MLAG switch).
 */
static void pim_mlag_up_local_add_send(struct pim_instance *pim,
		struct pim_upstream *up)
{
	struct stream *s = NULL;
	struct vrf *vrf = pim->vrf;

	if (!(router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP))
		return;

	s = stream_new(MLAG_MROUTE_ADD_MSGSIZE + PIM_MLAG_METADATA_LEN);
	if (!s)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("local MLAG mroute add %s:%s",
				vrf->name, up->sg_str);

	++router->mlag_stats.msg.mroute_add_tx;

	stream_putl(s, MLAG_MROUTE_ADD);
	stream_put(s, vrf->name, VRF_NAMSIZ);
	stream_putl(s, ntohl(up->sg.src.s_addr));
	stream_putl(s, ntohl(up->sg.grp.s_addr));

	stream_putl(s, pim_up_mlag_local_cost(up));
	/* XXX - who is addding*/
	stream_putl(s, MLAG_OWNER_VXLAN);
	/* XXX - am_i_DR field should be removed */
	stream_putc(s, false);
	stream_putc(s, !(PIM_UPSTREAM_FLAG_TEST_MLAG_NON_DF(up->flags)));
	stream_putl(s, vrf->vrf_id);
	/* XXX - this field is a No-op for VXLAN*/
	stream_put(s, NULL, INTERFACE_NAMSIZ);

	stream_fifo_push_safe(router->mlag_fifo, s);
	pim_mlag_signal_zpthread();
}

static void pim_mlag_up_local_del_send(struct pim_instance *pim,
		struct pim_upstream *up)
{
	struct stream *s = NULL;
	struct vrf *vrf = pim->vrf;

	if (!(router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP))
		return;

	s = stream_new(MLAG_MROUTE_DEL_MSGSIZE + PIM_MLAG_METADATA_LEN);
	if (!s)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("local MLAG mroute del %s:%s",
				vrf->name, up->sg_str);

	++router->mlag_stats.msg.mroute_del_tx;

	stream_putl(s, MLAG_MROUTE_DEL);
	stream_put(s, vrf->name, VRF_NAMSIZ);
	stream_putl(s, ntohl(up->sg.src.s_addr));
	stream_putl(s, ntohl(up->sg.grp.s_addr));
	/* XXX - who is adding */
	stream_putl(s, MLAG_OWNER_VXLAN);
	stream_putl(s, vrf->vrf_id);
	/* XXX - this field is a No-op for VXLAN */
	stream_put(s, NULL, INTERFACE_NAMSIZ);

	/* XXX - is this the the most optimal way to do things */
	stream_fifo_push_safe(router->mlag_fifo, s);
	pim_mlag_signal_zpthread();
}


/* Called when a local upstream entry is created or if it's cost changes */
void pim_mlag_up_local_add(struct pim_instance *pim,
		struct pim_upstream *up)
{
	pim_mlag_up_df_role_elect(up);
	/* XXX - need to add some dup checks here */
	pim_mlag_up_local_add_send(pim, up);
}

/* Called when local MLAG reference is removed from an upstream entry */
void pim_mlag_up_local_del(struct pim_instance *pim,
		struct pim_upstream *up)
{
	pim_mlag_up_df_role_elect(up);
	pim_mlag_up_local_del_send(pim, up);
}

/* When connection to local MLAG daemon is established all the local
 * MLAG upstream entries are replayed to it.
 */
static void pim_mlag_up_local_replay(void)
{
	struct listnode *upnode;
	struct pim_upstream *up;
	struct vrf *vrf;
	struct pim_instance *pim;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name) {
		pim = vrf->info;
		for (ALL_LIST_ELEMENTS_RO(pim->upstream_list, upnode, up)) {
			if (pim_up_mlag_is_local(up))
				pim_mlag_up_local_add_send(pim, up);
		}
	}
}

/* on local/remote mlag connection and role changes the DF status needs
 * to be re-evaluated
 */
static void pim_mlag_up_local_reeval(bool mlagd_send, const char *reason_code)
{
	struct listnode *upnode;
	struct pim_upstream *up;
	struct vrf *vrf;
	struct pim_instance *pim;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s re-run DF election because of %s",
				__func__, reason_code);
	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name) {
		pim = vrf->info;
		for (ALL_LIST_ELEMENTS_RO(pim->upstream_list, upnode, up)) {
			if (!pim_up_mlag_is_local(up))
				continue;
			/* if role changes re-send to peer */
			if (pim_mlag_up_df_role_elect(up) && mlagd_send)
				pim_mlag_up_local_add_send(pim, up);
		}
	}
}

/*****************PIM Actions for MLAG state chnages**********************/

/* notify the anycast VTEP component about state changes */
static inline void pim_mlag_vxlan_state_update(void)
{
	bool enable = !!(router->mlag_flags & PIM_MLAGF_STATUS_RXED);
	bool peer_state = !!(router->mlag_flags & PIM_MLAGF_REMOTE_CONN_UP);

	pim_vxlan_mlag_update(enable, peer_state, router->mlag_role,
			router->peerlink_rif_p, &router->local_vtep_ip);

}

void pim_mlag_update_dr_state_to_peer(struct interface *ifp)
{
	struct pim_interface *pim_ifp = ifp->info;
	struct pim_instance *pim;
	struct pim_upstream *up;
	struct pim_ifchannel *ch;
	struct listnode *node;


	if (!pim_ifp && PIM_I_am_DualActive(pim_ifp))
		return;

	pim = pim_ifp->pim;
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: DR on Interface-%s changed, updating to peer",
			   __func__, ifp->name);

	for (ALL_LIST_ELEMENTS_RO(pim->upstream_list, node, up)) {
		ch = pim_ifchannel_find(ifp, &up->sg);
		if (ch)
			pim_mlag_add_entry_to_peer(ch);
	}
}

void pim_mlag_update_cost_to_rp_to_peer(struct pim_upstream *up)
{
	struct listnode *chnode;
	struct listnode *chnextnode;
	struct pim_ifchannel *ch = NULL;
	struct pim_interface *pim_ifp = NULL;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: RP cost of upstream-%s changed, update",
			   __func__, up->sg_str);

	for (ALL_LIST_ELEMENTS(up->ifchannels, chnode, chnextnode, ch)) {
		pim_ifp = (ch->interface) ? ch->interface->info : NULL;
		if (!pim_ifp)
			continue;

		if (PIM_I_am_DualActive(pim_ifp)) {
			ch->mlag_local_cost_to_rp =
				up->rpf.source_nexthop.mrib_route_metric;
			pim_mlag_add_entry_to_peer(ch);
		}
	}
}

static void pim_mlag_handle_state_change_for_ifp(struct pim_instance *pim,
						 struct interface *ifp,
						 bool role_change,
						 bool state_change)
{
	struct pim_ifchannel *ch;
	struct pim_interface *pim_ifp = ifp->info;

	RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
		if (ch) {
			if (role_change == true)
				pim_mlag_calculate_df_for_ifchannel(ch);
			else if (state_change == true) {
				if (router->connected_to_mlag == true)
					pim_mlag_add_entry_to_peer(ch);
				else {
					/* Reset peer data */
					ch->mlag_peer_cost_to_rp =
						PIM_ASSERT_ROUTE_METRIC_MAX;
					pim_mlag_calculate_df_for_ifchannel(ch);
				}
			}
		}
	}
}

static int pim_mlag_role_change_handler(struct thread *thread)
{
	struct vrf *vrf;
	struct interface *ifp;
	struct pim_interface *pim_ifp;

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (!vrf->info)
			continue;

		FOR_ALL_INTERFACES (vrf, ifp) {
			if (!ifp->info)
				continue;
			pim_ifp = ifp->info;
			if (!ifp->info || !PIM_I_am_DualActive(pim_ifp))
				continue;
			pim_mlag_handle_state_change_for_ifp(vrf->info, ifp,
							     true, false);
		}
	}
	return (0);
}

static int pim_mlag_state_change_handler(struct thread *thread)
{
	struct vrf *vrf;
	struct interface *ifp;
	struct pim_interface *pim_ifp;

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (!vrf->info)
			continue;

		FOR_ALL_INTERFACES (vrf, ifp) {
			if (!ifp->info)
				continue;
			pim_ifp = ifp->info;
			if (!ifp->info || !PIM_I_am_DualActive(pim_ifp))
				continue;
			pim_mlag_handle_state_change_for_ifp(vrf->info, ifp,
							     false, true);
		}
	}
	return (0);
}

/**************End of PIM Actions for MLAG State changes******************/


/********************API to process PIM MLAG Data ************************/

static void pim_mlag_process_mlagd_state_change(struct mlag_status msg)
{
	bool role_chg = false;
	bool state_chg = false;
	bool notify_vxlan = false;
	struct interface *peerlink_rif_p;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: msg dump: my_role:%d, peer_state:%d", __func__,
			   msg.my_role, msg.peer_state);

	if (!(router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP)) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: msg ignored mlagd process state down",
					__func__);
		return;
	}
	++router->mlag_stats.msg.mlag_status_updates;

	/* evaluate the changes first */
	if (router->mlag_role != msg.my_role) {
		role_chg = true;
		notify_vxlan = true;
		router->mlag_role = msg.my_role;
	}

	strcpy(router->peerlink_rif, msg.peerlink_rif);
	/* XXX - handle the case where we may rx the interface name from the
	 * MLAG daemon before we get the interface from zebra.
	 */
	peerlink_rif_p = if_lookup_by_name(router->peerlink_rif, VRF_DEFAULT);
	if (router->peerlink_rif_p != peerlink_rif_p) {
		router->peerlink_rif_p = peerlink_rif_p;
		notify_vxlan = true;
	}

	if (msg.peer_state == MLAG_STATE_RUNNING) {
		if (!(router->mlag_flags & PIM_MLAGF_REMOTE_CONN_UP)) {
			state_chg = true;
			notify_vxlan = true;
			router->mlag_flags |= PIM_MLAGF_REMOTE_CONN_UP;
		}
		router->connected_to_mlag = true;
	} else {
		if (router->mlag_flags & PIM_MLAGF_REMOTE_CONN_UP) {
			++router->mlag_stats.peer_session_downs;
			state_chg = true;
			notify_vxlan = true;
			router->mlag_flags &= ~PIM_MLAGF_REMOTE_CONN_UP;
		}
		router->connected_to_mlag = false;
	}

	/* apply the changes */
	/* when connection to mlagd comes up we hold send mroutes till we have
	 * rxed the status and had a chance to re-valuate DF state
	 */
	if (!(router->mlag_flags & PIM_MLAGF_STATUS_RXED)) {
		router->mlag_flags |= PIM_MLAGF_STATUS_RXED;
		pim_mlag_vxlan_state_update();
		/* on session up re-eval DF status */
		pim_mlag_up_local_reeval(false /*mlagd_send*/, "mlagd_up");
		/* replay all the upstream entries to the local MLAG daemon */
		pim_mlag_up_local_replay();
		return;
	}

	if (notify_vxlan)
		pim_mlag_vxlan_state_update();

	if (state_chg) {
		if (!(router->mlag_flags & PIM_MLAGF_REMOTE_CONN_UP)) {
			/* when a connection goes down the primary takes over
			 * DF role for all entries
			 */
			pim_mlag_up_local_reeval(true /*mlagd_send*/,
					"peer_down");
		}
		/* XXX - when session comes up we need to wait for
		 * REMOTE_REPLAY_DONE before running re-election on local-mlag
		 * entries that are missing remote reference
		 */
		pim_mlag_up_local_reeval(true /*mlagd_send*/,
				"peer_up");
	} else if (role_chg) {
		/* MLAG role changed without a state change */
		pim_mlag_up_local_reeval(true /*mlagd_send*/, "role_chg");
		thread_add_event(router->master, pim_mlag_role_change_handler,
				 NULL, 0, NULL);
	}
}

static void pim_mlag_process_vxlan_update(struct mlag_vxlan *msg)
{
	char addr_buf1[INET_ADDRSTRLEN];
	char addr_buf2[INET_ADDRSTRLEN];
	uint32_t local_ip;

	if (!(router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP)) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: msg ignored mlagd process state down",
					__func__);
		return;
	}

	++router->mlag_stats.msg.vxlan_updates;
	router->anycast_vtep_ip.s_addr = htonl(msg->anycast_ip);
	local_ip = htonl(msg->local_ip);
	if (router->local_vtep_ip.s_addr != local_ip) {
		router->local_vtep_ip.s_addr = local_ip;
		pim_mlag_vxlan_state_update();
	}

	if (PIM_DEBUG_MLAG) {
		inet_ntop(AF_INET, &router->local_vtep_ip,
				addr_buf1, INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &router->anycast_vtep_ip,
				addr_buf2, INET_ADDRSTRLEN);

		zlog_debug("%s: msg dump: local-ip:%s, anycast-ip:%s",
				__func__, addr_buf1, addr_buf2);
	}
}

static void pim_mlag_process_mroute_add(struct mlag_mroute_add msg)
{
	struct vrf *vrf = NULL;
	struct interface *ifp = NULL;
	struct pim_ifchannel *ch = NULL;
	struct prefix_sg sg;

	if (PIM_DEBUG_MLAG) {
		zlog_debug(
			"%s: msg dump: vrf_name:%s, s.ip:0x%x, g.ip:0x%x cost:%u",
			__func__, msg.vrf_name, msg.source_ip, msg.group_ip,
			msg.cost_to_rp);
		zlog_debug(
			"owner_id:%d, DR:%d, Dual active:%d, vrf_id:0x%x intf_name:%s",
			msg.owner_id, msg.am_i_dr, msg.am_i_dual_active,
			msg.vrf_id, msg.intf_name);
	}

	if (!(router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP)) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: msg ignored mlagd process state down",
				   __func__);
		return;
	}

	++router->mlag_stats.msg.mroute_add_rx;

	if (msg.owner_id == MLAG_OWNER_VXLAN) {
		pim_mlag_up_remote_add(&msg);
		return;
	}

	vrf = vrf_lookup_by_name(msg.vrf_name);
	if (vrf)
		ifp = if_lookup_by_name(msg.intf_name, vrf->vrf_id);

	if (!vrf || !ifp || !ifp->info) {
		if (PIM_DEBUG_MLAG)
			zlog_debug(
				"%s: Invalid params...vrf:%p, ifp,%p, pim_ifp:%p",
				__func__, vrf, ifp, ifp->info);
		return;
	}

	memset(&sg, 0, sizeof(struct prefix_sg));
	sg.src.s_addr = ntohl(msg.source_ip);
	sg.grp.s_addr = ntohl(msg.group_ip);

	ch = pim_ifchannel_find(ifp, &sg);
	if (ch) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: Updating ifchannel-%s peer mlag params",
				   __func__, ch->sg_str);
		ch->mlag_peer_cost_to_rp = msg.cost_to_rp;
		ch->mlag_peer_is_dr = msg.am_i_dr;
		ch->mlag_peer_is_dual_active = msg.am_i_dual_active;
		pim_mlag_calculate_df_for_ifchannel(ch);
	} else {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: failed to find if-channel...",
				   __func__);
	}
}

static void pim_mlag_process_mroute_del(struct mlag_mroute_del msg)
{
	if (PIM_DEBUG_MLAG) {
		zlog_debug("%s: msg dump: vrf_name:%s, s.ip:0x%x, g.ip:0x%x ",
			   __func__, msg.vrf_name, msg.source_ip, msg.group_ip);
		zlog_debug("owner_id:%d, vrf_id:0x%x intf_name:%s", msg.owner_id,
			   msg.vrf_id, msg.intf_name);
	}

	if (!(router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP)) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: msg ignored mlagd process state down",
					__func__);
		return;
	}

	++router->mlag_stats.msg.mroute_del_rx;

	if (msg.owner_id == MLAG_OWNER_VXLAN) {
		pim_mlag_up_remote_del(&msg);
		return;
	}
}

static void pim_mlag_process_peer_status_update(struct mlag_pim_status msg)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: msg dump: switchd_state:%d, svi_state:%d",
			   __func__, msg.switchd_state, msg.svi_state);
	++router->mlag_stats.msg.pim_status_updates;
}

int pim_zebra_mlag_handle_msg(struct stream *s, int len)
{
	struct mlag_msg mlag_msg;
	char buf[80];
	int rc = 0;

	rc = zebra_mlag_lib_decode_mlag_hdr(s, &mlag_msg);
	if (rc)
		return (rc);

	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: Received msg type:%s length:%d, bulk_cnt:%d",
			__func__,
			zebra_mlag_lib_msgid_to_str(mlag_msg.msg_type, buf, 80),
			mlag_msg.data_len, mlag_msg.msg_cnt);

	switch (mlag_msg.msg_type) {
	case MLAG_STATUS_UPDATE: {
		struct mlag_status msg;

		rc = zebra_mlag_lib_decode_mlag_status(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_mlagd_state_change(msg);
	} break;
	case MLAG_VXLAN_UPDATE:
	{
		struct mlag_vxlan msg;

		rc = zebra_mlag_lib_decode_vxlan_update(s, &msg);
		if (rc)
			return rc;
		pim_mlag_process_vxlan_update(&msg);
	}
	break;
	case MLAG_MROUTE_ADD: {
		struct mlag_mroute_add msg;

		rc = zebra_mlag_lib_decode_mroute_add(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_mroute_add(msg);
	} break;
	case MLAG_MROUTE_DEL: {
		struct mlag_mroute_del msg;

		rc = zebra_mlag_lib_decode_mroute_del(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_mroute_del(msg);
	} break;
	case MLAG_MROUTE_ADD_BULK: {
		struct mlag_mroute_add msg;
		int i = 0;

		for (i = 0; i < mlag_msg.msg_cnt; i++) {

			rc = zebra_mlag_lib_decode_mroute_add(s, &msg);
			if (rc)
				return (rc);
			pim_mlag_process_mroute_add(msg);
		}
	} break;
	case MLAG_MROUTE_DEL_BULK: {
		struct mlag_mroute_del msg;
		int i = 0;

		for (i = 0; i < mlag_msg.msg_cnt; i++) {

			rc = zebra_mlag_lib_decode_mroute_del(s, &msg);
			if (rc)
				return (rc);
			pim_mlag_process_mroute_del(msg);
		}
	} break;
	case MLAG_PIM_STATUS_UPDATE: {
		struct mlag_pim_status msg;

		rc = zebra_mlag_lib_decode_pim_status(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_peer_status_update(msg);

	} break;
	default:
		break;
	}
	return 0;
}

/****************End of PIM Mesasge processing handler********************/

int pim_zebra_mlag_process_up(void)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Received Process-Up from Mlag", __func__);

	/*
	 * Incase of local MLAG restyrat, PIM needs to replay all the dat
	 * since MLAG is empty.
	 */
	router->connected_to_mlag = true;
	router->mlag_flags |= PIM_MLAGF_LOCAL_CONN_UP;
	thread_add_event(router->master, pim_mlag_state_change_handler, NULL, 0,
			 NULL);
	return 0;
}

static void pim_mlag_param_reset(void)
{
	/* reset the cached params and stats */
	router->mlag_flags &= ~(PIM_MLAGF_STATUS_RXED |
			PIM_MLAGF_LOCAL_CONN_UP |
			PIM_MLAGF_REMOTE_CONN_UP);
	router->local_vtep_ip.s_addr = INADDR_ANY;
	router->anycast_vtep_ip.s_addr = INADDR_ANY;
	router->mlag_role = MLAG_ROLE_NONE;
	memset(&router->mlag_stats.msg, 0, sizeof(router->mlag_stats.msg));
	router->peerlink_rif[0] = '\0';
}

int pim_zebra_mlag_process_down(void)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Received Process-Down from Mlag", __func__);

	/*
	 * Local CLAG is down, reset peer data
	 * and forward teh traffic if we are DR
	 */
	if (router->mlag_flags & PIM_MLAGF_REMOTE_CONN_UP)
		++router->mlag_stats.peer_session_downs;
	router->connected_to_mlag = false;
	pim_mlag_param_reset();
	/* on mlagd session down re-eval DF status */
	pim_mlag_up_local_reeval(false /*mlagd_send*/, "mlagd_down");
	/* flush all remote references */
	pim_mlag_up_remote_del_all();
	/* notify the vxlan component */
	pim_mlag_vxlan_state_update();
	thread_add_event(router->master, pim_mlag_state_change_handler, NULL, 0,
			 NULL);
	return 0;
}

static int pim_mlag_register_handler(struct thread *thread)
{
	uint32_t bit_mask = 0;

	if (!zclient)
		return -1;

	SET_FLAG(bit_mask, (1 << MLAG_STATUS_UPDATE));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_ADD));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_DEL));
	SET_FLAG(bit_mask, (1 << MLAG_DUMP));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_ADD_BULK));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_DEL_BULK));
	SET_FLAG(bit_mask, (1 << MLAG_PIM_STATUS_UPDATE));
	SET_FLAG(bit_mask, (1 << MLAG_PIM_CFG_DUMP));
	SET_FLAG(bit_mask, (1 << MLAG_VXLAN_UPDATE));

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Posting Client Register to MLAG mask:0x%x",
			   __func__, bit_mask);

	zclient_send_mlag_register(zclient, bit_mask);
	return 0;
}

void pim_mlag_register(void)
{
	if (router->mlag_process_register)
		return;

	router->mlag_process_register = true;

	thread_add_event(router->master, pim_mlag_register_handler,
			NULL, 0, NULL);
}

static int pim_mlag_deregister_handler(struct thread *thread)
{
	if (!zclient)
		return -1;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Posting Client De-Register to MLAG from PIM",
			   __func__);
	router->connected_to_mlag = false;
	zclient_send_mlag_deregister(zclient);
	return 0;
}

void pim_mlag_deregister(void)
{
	/* if somebody still interested in the MLAG channel skip de-reg */
	if (router->pim_mlag_intf_cnt || pim_vxlan_do_mlag_reg())
		return;

	/* not registered; nothing do */
	if (!router->mlag_process_register)
		return;

	router->mlag_process_register = false;

	thread_add_event(router->master, pim_mlag_deregister_handler,
			NULL, 0, NULL);
}

void pim_if_configure_mlag_dualactive(struct pim_interface *pim_ifp)
{
	if (!pim_ifp || pim_ifp->activeactive == true)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Configuring active-active on Interface: %s",
			   __func__, "NULL");

	pim_ifp->activeactive = true;
	if (pim_ifp->pim)
		pim_ifp->pim->inst_mlag_intf_cnt++;

	router->pim_mlag_intf_cnt++;
	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: Total MLAG configured Interfaces on router: %d, Inst:%d",
			__func__, router->pim_mlag_intf_cnt,
			pim_ifp->pim->inst_mlag_intf_cnt);

	if (router->pim_mlag_intf_cnt == 1) {
		/*
		 * atleast one Interface is configured for MLAG, send register
		 * to Zebra for receiving MLAG Updates
		 */
		pim_mlag_register();
	}
}

void pim_if_unconfigure_mlag_dualactive(struct pim_interface *pim_ifp)
{
	if (!pim_ifp || pim_ifp->activeactive == false)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: UnConfiguring active-active on Interface: %s",
			   __func__, "NULL");

	pim_ifp->activeactive = false;
	if (pim_ifp->pim)
		pim_ifp->pim->inst_mlag_intf_cnt--;

	router->pim_mlag_intf_cnt--;
	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: Total MLAG configured Interfaces on router: %d, Inst:%d",
			__func__, router->pim_mlag_intf_cnt,
			pim_ifp->pim->inst_mlag_intf_cnt);

	if (router->pim_mlag_intf_cnt == 0) {
		/*
		 * all the Interfaces are MLAG un-configured, post MLAG
		 * De-register to Zebra
		 */
		pim_mlag_deregister();
	}
}


void pim_instance_mlag_init(struct pim_instance *pim)
{
	if (!pim)
		return;

	pim->inst_mlag_intf_cnt = 0;
}


void pim_instance_mlag_terminate(struct pim_instance *pim)
{
	struct interface *ifp;

	if (!pim)
		return;

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;

		if (!pim_ifp || pim_ifp->activeactive == false)
			continue;

		pim_if_unconfigure_mlag_dualactive(pim_ifp);
	}
	pim->inst_mlag_intf_cnt = 0;
}

void pim_mlag_init(void)
{
	pim_mlag_param_reset();
	router->pim_mlag_intf_cnt = 0;
	router->connected_to_mlag = false;
	router->mlag_fifo = stream_fifo_new();
	router->zpthread_mlag_write = NULL;
	router->mlag_stream = stream_new(MLAG_BUF_LIMIT);
}
