/*
 * Zebra dataplane layer.
 * Copyright (c) 2018 Volta Networks, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/libfrr.h"
#include "lib/debug.h"
#include "lib/frratomic.h"
#include "lib/frr_pthread.h"
#include "lib/memory.h"
#include "lib/queue.h"
#include "lib/zebra.h"
#include "zebra/zebra_router.h"
#include "zebra/zebra_memory.h"
#include "zebra/zebra_router.h"
#include "zebra/zebra_dplane.h"
#include "zebra/rt.h"
#include "zebra/debug.h"

/* Memory type for context blocks */
DEFINE_MTYPE(ZEBRA, DP_CTX, "Zebra DPlane Ctx")
DEFINE_MTYPE(ZEBRA, DP_PROV, "Zebra DPlane Provider")

#ifndef AOK
#  define AOK 0
#endif

/* Enable test dataplane provider */
/*#define DPLANE_TEST_PROVIDER 1 */

/* Default value for max queued incoming updates */
const uint32_t DPLANE_DEFAULT_MAX_QUEUED = 200;

/* Default value for new work per cycle */
const uint32_t DPLANE_DEFAULT_NEW_WORK = 100;

/* Validation check macro for context blocks */
/* #define DPLANE_DEBUG 1 */

#ifdef DPLANE_DEBUG

#  define DPLANE_CTX_VALID(p)	\
		assert((p) != NULL)

#else

#  define DPLANE_CTX_VALID(p)

#endif	/* DPLANE_DEBUG */

/*
 * Route information captured for route updates.
 */
struct dplane_route_info {

	/* Dest and (optional) source prefixes */
	struct prefix zd_dest;
	struct prefix zd_src;

	afi_t zd_afi;
	safi_t zd_safi;

	int zd_type;
	int zd_old_type;

	route_tag_t zd_tag;
	route_tag_t zd_old_tag;
	uint32_t zd_metric;
	uint32_t zd_old_metric;

	uint16_t zd_instance;
	uint16_t zd_old_instance;

	uint8_t zd_distance;
	uint8_t zd_old_distance;

	uint32_t zd_mtu;
	uint32_t zd_nexthop_mtu;

	/* Nexthop hash entry */
	struct nhg_hash_entry zd_nhe;

	/* Nexthops */
	struct nexthop_group zd_ng;

	/* "Previous" nexthops, used only in route updates without netlink */
	struct nexthop_group zd_old_ng;

	/* TODO -- use fixed array of nexthops, to avoid mallocs? */

};

/*
 * Pseudowire info for the dataplane
 */
struct dplane_pw_info {
	char ifname[IF_NAMESIZE];
	ifindex_t ifindex;
	int type;
	int af;
	int status;
	uint32_t flags;
	union g_addr dest;
	mpls_label_t local_label;
	mpls_label_t remote_label;

	/* Nexthops */
	struct nexthop_group nhg;

	union pw_protocol_fields fields;
};

/*
 * The context block used to exchange info about route updates across
 * the boundary between the zebra main context (and pthread) and the
 * dataplane layer (and pthread).
 */
struct zebra_dplane_ctx {

	/* Operation code */
	enum dplane_op_e zd_op;

	/* Status on return */
	enum zebra_dplane_result zd_status;

	/* Dplane provider id */
	uint32_t zd_provider;

	/* Flags - used by providers, e.g. */
	int zd_flags;

	bool zd_is_update;

	uint32_t zd_seq;
	uint32_t zd_old_seq;

	/* TODO -- internal/sub-operation status? */
	enum zebra_dplane_result zd_remote_status;
	enum zebra_dplane_result zd_kernel_status;

	vrf_id_t zd_vrf_id;
	uint32_t zd_table_id;

	/* Support info for either route or LSP update */
	union {
		struct dplane_route_info rinfo;
		zebra_lsp_t lsp;
		struct dplane_pw_info pw;
	} u;

	/* Namespace info, used especially for netlink kernel communication */
	struct zebra_dplane_info zd_ns_info;

	/* Embedded list linkage */
	TAILQ_ENTRY(zebra_dplane_ctx) zd_q_entries;
};

/* Flag that can be set by a pre-kernel provider as a signal that an update
 * should bypass the kernel.
 */
#define DPLANE_CTX_FLAG_NO_KERNEL 0x01


/*
 * Registration block for one dataplane provider.
 */
struct zebra_dplane_provider {
	/* Name */
	char dp_name[DPLANE_PROVIDER_NAMELEN + 1];

	/* Priority, for ordering among providers */
	uint8_t dp_priority;

	/* Id value */
	uint32_t dp_id;

	/* Mutex */
	pthread_mutex_t dp_mutex;

	/* Plugin-provided extra data */
	void *dp_data;

	/* Flags */
	int dp_flags;

	int (*dp_fp)(struct zebra_dplane_provider *prov);

	int (*dp_fini)(struct zebra_dplane_provider *prov, bool early_p);

	_Atomic uint32_t dp_in_counter;
	_Atomic uint32_t dp_in_queued;
	_Atomic uint32_t dp_in_max;
	_Atomic uint32_t dp_out_counter;
	_Atomic uint32_t dp_out_queued;
	_Atomic uint32_t dp_out_max;
	_Atomic uint32_t dp_error_counter;

	/* Queue of contexts inbound to the provider */
	struct dplane_ctx_q dp_ctx_in_q;

	/* Queue of completed contexts outbound from the provider back
	 * towards the dataplane module.
	 */
	struct dplane_ctx_q dp_ctx_out_q;

	/* Embedded list linkage for provider objects */
	TAILQ_ENTRY(zebra_dplane_provider) dp_prov_link;
};

/*
 * Globals
 */
static struct zebra_dplane_globals {
	/* Mutex to control access to dataplane components */
	pthread_mutex_t dg_mutex;

	/* Results callback registered by zebra 'core' */
	int (*dg_results_cb)(struct dplane_ctx_q *ctxlist);

	/* Sentinel for beginning of shutdown */
	volatile bool dg_is_shutdown;

	/* Sentinel for end of shutdown */
	volatile bool dg_run;

	/* Route-update context queue inbound to the dataplane */
	TAILQ_HEAD(zdg_ctx_q, zebra_dplane_ctx) dg_route_ctx_q;

	/* Ordered list of providers */
	TAILQ_HEAD(zdg_prov_q, zebra_dplane_provider) dg_providers_q;

	/* Counter used to assign internal ids to providers */
	uint32_t dg_provider_id;

	/* Limit number of pending, unprocessed updates */
	_Atomic uint32_t dg_max_queued_updates;

	/* Limit number of new updates dequeued at once, to pace an
	 * incoming burst.
	 */
	uint32_t dg_updates_per_cycle;

	_Atomic uint32_t dg_routes_in;
	_Atomic uint32_t dg_routes_queued;
	_Atomic uint32_t dg_routes_queued_max;
	_Atomic uint32_t dg_route_errors;
	_Atomic uint32_t dg_other_errors;

	_Atomic uint32_t dg_nexthops_in;
	_Atomic uint32_t dg_nexthop_errors;

	_Atomic uint32_t dg_lsps_in;
	_Atomic uint32_t dg_lsp_errors;

	_Atomic uint32_t dg_pws_in;
	_Atomic uint32_t dg_pw_errors;

	_Atomic uint32_t dg_update_yields;

	/* Dataplane pthread */
	struct frr_pthread *dg_pthread;

	/* Event-delivery context 'master' for the dplane */
	struct thread_master *dg_master;

	/* Event/'thread' pointer for queued updates */
	struct thread *dg_t_update;

	/* Event pointer for pending shutdown check loop */
	struct thread *dg_t_shutdown_check;

} zdplane_info;

/*
 * Lock and unlock for interactions with the zebra 'core' pthread
 */
#define DPLANE_LOCK() pthread_mutex_lock(&zdplane_info.dg_mutex)
#define DPLANE_UNLOCK() pthread_mutex_unlock(&zdplane_info.dg_mutex)


/*
 * Lock and unlock for individual providers
 */
#define DPLANE_PROV_LOCK(p)   pthread_mutex_lock(&((p)->dp_mutex))
#define DPLANE_PROV_UNLOCK(p) pthread_mutex_unlock(&((p)->dp_mutex))

/* Prototypes */
static int dplane_thread_loop(struct thread *event);
static void dplane_info_from_zns(struct zebra_dplane_info *ns_info,
				 struct zebra_ns *zns);
static enum zebra_dplane_result lsp_update_internal(zebra_lsp_t *lsp,
						    enum dplane_op_e op);
static enum zebra_dplane_result pw_update_internal(struct zebra_pw *pw,
						   enum dplane_op_e op);

/*
 * Public APIs
 */

/* Obtain thread_master for dataplane thread */
struct thread_master *dplane_get_thread_master(void)
{
	return zdplane_info.dg_master;
}

/*
 * Allocate a dataplane update context
 */
static struct zebra_dplane_ctx *dplane_ctx_alloc(void)
{
	struct zebra_dplane_ctx *p;

	/* TODO -- just alloc'ing memory, but would like to maintain
	 * a pool
	 */
	p = XCALLOC(MTYPE_DP_CTX, sizeof(struct zebra_dplane_ctx));

	return p;
}

/*
 * Free a dataplane results context.
 */
static void dplane_ctx_free(struct zebra_dplane_ctx **pctx)
{
	if (pctx == NULL)
		return;

	DPLANE_CTX_VALID(*pctx);

	/* TODO -- just freeing memory, but would like to maintain
	 * a pool
	 */

	/* Some internal allocations may need to be freed, depending on
	 * the type of info captured in the ctx.
	 */
	switch ((*pctx)->zd_op) {
	case DPLANE_OP_ROUTE_INSTALL:
	case DPLANE_OP_ROUTE_UPDATE:
	case DPLANE_OP_ROUTE_DELETE: {
		/* Free allocated nexthops */
		if ((*pctx)->u.rinfo.zd_ng.nexthop) {
			/* This deals with recursive nexthops too */
			nexthops_free((*pctx)->u.rinfo.zd_ng.nexthop);

			(*pctx)->u.rinfo.zd_ng.nexthop = NULL;
		}

		if ((*pctx)->u.rinfo.zd_old_ng.nexthop) {
			/* This deals with recursive nexthops too */
			nexthops_free((*pctx)->u.rinfo.zd_old_ng.nexthop);

			(*pctx)->u.rinfo.zd_old_ng.nexthop = NULL;
		}

		break;
	}

	case DPLANE_OP_NH_INSTALL:
	case DPLANE_OP_NH_UPDATE:
	case DPLANE_OP_NH_DELETE: {
		zebra_nhg_free_members(&(*pctx)->u.rinfo.zd_nhe);
		break;
	}

	case DPLANE_OP_LSP_INSTALL:
	case DPLANE_OP_LSP_UPDATE:
	case DPLANE_OP_LSP_DELETE:
	{
		zebra_nhlfe_t *nhlfe, *next;

		/* Free allocated NHLFEs */
		for (nhlfe = (*pctx)->u.lsp.nhlfe_list; nhlfe; nhlfe = next) {
			next = nhlfe->next;

			zebra_mpls_nhlfe_del(nhlfe);
		}

		/* Clear pointers in lsp struct, in case we're cacheing
		 * free context structs.
		 */
		(*pctx)->u.lsp.nhlfe_list = NULL;
		(*pctx)->u.lsp.best_nhlfe = NULL;

		break;
	}

	case DPLANE_OP_PW_INSTALL:
	case DPLANE_OP_PW_UNINSTALL:
		/* Free allocated nexthops */
		if ((*pctx)->u.pw.nhg.nexthop) {
			/* This deals with recursive nexthops too */
			nexthops_free((*pctx)->u.pw.nhg.nexthop);

			(*pctx)->u.pw.nhg.nexthop = NULL;
		}
		break;

	case DPLANE_OP_NONE:
		break;
	}

	XFREE(MTYPE_DP_CTX, *pctx);
	*pctx = NULL;
}

/*
 * Return a context block to the dplane module after processing
 */
void dplane_ctx_fini(struct zebra_dplane_ctx **pctx)
{
	/* TODO -- maintain pool; for now, just free */
	dplane_ctx_free(pctx);
}

/* Enqueue a context block */
void dplane_ctx_enqueue_tail(struct dplane_ctx_q *q,
			     const struct zebra_dplane_ctx *ctx)
{
	TAILQ_INSERT_TAIL(q, (struct zebra_dplane_ctx *)ctx, zd_q_entries);
}

/* Append a list of context blocks to another list */
void dplane_ctx_list_append(struct dplane_ctx_q *to_list,
			    struct dplane_ctx_q *from_list)
{
	if (TAILQ_FIRST(from_list)) {
		TAILQ_CONCAT(to_list, from_list, zd_q_entries);

		/* And clear 'from' list */
		TAILQ_INIT(from_list);
	}
}

/* Dequeue a context block from the head of a list */
struct zebra_dplane_ctx *dplane_ctx_dequeue(struct dplane_ctx_q *q)
{
	struct zebra_dplane_ctx *ctx = TAILQ_FIRST(q);

	if (ctx)
		TAILQ_REMOVE(q, ctx, zd_q_entries);

	return ctx;
}

/*
 * Accessors for information from the context object
 */
enum zebra_dplane_result dplane_ctx_get_status(
	const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_status;
}

void dplane_ctx_set_status(struct zebra_dplane_ctx *ctx,
			   enum zebra_dplane_result status)
{
	DPLANE_CTX_VALID(ctx);

	ctx->zd_status = status;
}

/* Retrieve last/current provider id */
uint32_t dplane_ctx_get_provider(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);
	return ctx->zd_provider;
}

/* Providers run before the kernel can control whether a kernel
 * update should be done.
 */
void dplane_ctx_set_skip_kernel(struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	SET_FLAG(ctx->zd_flags, DPLANE_CTX_FLAG_NO_KERNEL);
}

bool dplane_ctx_is_skip_kernel(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return CHECK_FLAG(ctx->zd_flags, DPLANE_CTX_FLAG_NO_KERNEL);
}

enum dplane_op_e dplane_ctx_get_op(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_op;
}

const char *dplane_op2str(enum dplane_op_e op)
{
	const char *ret = "UNKNOWN";

	switch (op) {
	case DPLANE_OP_NONE:
		ret = "NONE";
		break;

	/* Route update */
	case DPLANE_OP_ROUTE_INSTALL:
		ret = "ROUTE_INSTALL";
		break;
	case DPLANE_OP_ROUTE_UPDATE:
		ret = "ROUTE_UPDATE";
		break;
	case DPLANE_OP_ROUTE_DELETE:
		ret = "ROUTE_DELETE";
		break;

	/* Nexthop update */
	case DPLANE_OP_NH_INSTALL:
		ret = "NH_INSTALL";
		break;
	case DPLANE_OP_NH_UPDATE:
		ret = "NH_UPDATE";
		break;
	case DPLANE_OP_NH_DELETE:
		ret = "NH_DELETE";
		break;

	case DPLANE_OP_LSP_INSTALL:
		ret = "LSP_INSTALL";
		break;
	case DPLANE_OP_LSP_UPDATE:
		ret = "LSP_UPDATE";
		break;
	case DPLANE_OP_LSP_DELETE:
		ret = "LSP_DELETE";
		break;

	case DPLANE_OP_PW_INSTALL:
		ret = "PW_INSTALL";
		break;
	case DPLANE_OP_PW_UNINSTALL:
		ret = "PW_UNINSTALL";
		break;

	}

	return ret;
}

const char *dplane_res2str(enum zebra_dplane_result res)
{
	const char *ret = "<Unknown>";

	switch (res) {
	case ZEBRA_DPLANE_REQUEST_FAILURE:
		ret = "FAILURE";
		break;
	case ZEBRA_DPLANE_REQUEST_QUEUED:
		ret = "QUEUED";
		break;
	case ZEBRA_DPLANE_REQUEST_SUCCESS:
		ret = "SUCCESS";
		break;
	}

	return ret;
}

const struct prefix *dplane_ctx_get_dest(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->u.rinfo.zd_dest);
}

/* Source prefix is a little special - return NULL for "no src prefix" */
const struct prefix *dplane_ctx_get_src(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	if (ctx->u.rinfo.zd_src.prefixlen == 0 &&
	    IN6_IS_ADDR_UNSPECIFIED(&(ctx->u.rinfo.zd_src.u.prefix6))) {
		return NULL;
	} else {
		return &(ctx->u.rinfo.zd_src);
	}
}

bool dplane_ctx_is_update(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_is_update;
}

uint32_t dplane_ctx_get_seq(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_seq;
}

uint32_t dplane_ctx_get_old_seq(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_old_seq;
}

vrf_id_t dplane_ctx_get_vrf(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_vrf_id;
}

int dplane_ctx_get_type(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_type;
}

int dplane_ctx_get_old_type(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_old_type;
}

afi_t dplane_ctx_get_afi(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_afi;
}

safi_t dplane_ctx_get_safi(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_safi;
}

uint32_t dplane_ctx_get_table(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->zd_table_id;
}

route_tag_t dplane_ctx_get_tag(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_tag;
}

route_tag_t dplane_ctx_get_old_tag(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_old_tag;
}

uint16_t dplane_ctx_get_instance(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_instance;
}

uint16_t dplane_ctx_get_old_instance(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_old_instance;
}

uint32_t dplane_ctx_get_metric(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_metric;
}

uint32_t dplane_ctx_get_old_metric(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_old_metric;
}

uint32_t dplane_ctx_get_mtu(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_mtu;
}

uint32_t dplane_ctx_get_nh_mtu(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_nexthop_mtu;
}

uint8_t dplane_ctx_get_distance(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_distance;
}

uint8_t dplane_ctx_get_old_distance(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.rinfo.zd_old_distance;
}

const struct nexthop_group *dplane_ctx_get_ng(
	const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->u.rinfo.zd_ng);
}

const struct nexthop_group *dplane_ctx_get_old_ng(
	const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->u.rinfo.zd_old_ng);
}

const struct zebra_dplane_info *dplane_ctx_get_ns(
	const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->zd_ns_info);
}

/* Accessors for nexthop information */
const struct nhg_hash_entry *
dplane_ctx_get_nhe(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);
	return &(ctx->u.rinfo.zd_nhe);
}

/* Accessors for LSP information */

mpls_label_t dplane_ctx_get_in_label(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.lsp.ile.in_label;
}

uint8_t dplane_ctx_get_addr_family(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.lsp.addr_family;
}

uint32_t dplane_ctx_get_lsp_flags(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.lsp.flags;
}

const zebra_nhlfe_t *dplane_ctx_get_nhlfe(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.lsp.nhlfe_list;
}

const zebra_nhlfe_t *
dplane_ctx_get_best_nhlfe(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.lsp.best_nhlfe;
}

uint32_t dplane_ctx_get_lsp_num_ecmp(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.lsp.num_ecmp;
}

const char *dplane_ctx_get_pw_ifname(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.pw.ifname;
}

mpls_label_t dplane_ctx_get_pw_local_label(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.pw.local_label;
}

mpls_label_t dplane_ctx_get_pw_remote_label(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.pw.remote_label;
}

int dplane_ctx_get_pw_type(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.pw.type;
}

int dplane_ctx_get_pw_af(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.pw.af;
}

uint32_t dplane_ctx_get_pw_flags(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.pw.flags;
}

int dplane_ctx_get_pw_status(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return ctx->u.pw.status;
}

const union g_addr *dplane_ctx_get_pw_dest(
	const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->u.pw.dest);
}

const union pw_protocol_fields *dplane_ctx_get_pw_proto(
	const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->u.pw.fields);
}

const struct nexthop_group *
dplane_ctx_get_pw_nhg(const struct zebra_dplane_ctx *ctx)
{
	DPLANE_CTX_VALID(ctx);

	return &(ctx->u.pw.nhg);
}

/*
 * End of dplane context accessors
 */


/*
 * Retrieve the limit on the number of pending, unprocessed updates.
 */
uint32_t dplane_get_in_queue_limit(void)
{
	return atomic_load_explicit(&zdplane_info.dg_max_queued_updates,
				    memory_order_relaxed);
}

/*
 * Configure limit on the number of pending, queued updates.
 */
void dplane_set_in_queue_limit(uint32_t limit, bool set)
{
	/* Reset to default on 'unset' */
	if (!set)
		limit = DPLANE_DEFAULT_MAX_QUEUED;

	atomic_store_explicit(&zdplane_info.dg_max_queued_updates, limit,
			      memory_order_relaxed);
}

/*
 * Retrieve the current queue depth of incoming, unprocessed updates
 */
uint32_t dplane_get_in_queue_len(void)
{
	return atomic_load_explicit(&zdplane_info.dg_routes_queued,
				    memory_order_seq_cst);
}

/*
 * Common dataplane context init with zebra namespace info.
 */
static int dplane_ctx_ns_init(struct zebra_dplane_ctx *ctx,
			      struct zebra_ns *zns,
			      bool is_update)
{
	dplane_info_from_zns(&(ctx->zd_ns_info), zns);

#if defined(HAVE_NETLINK)
	/* Increment message counter after copying to context struct - may need
	 * two messages in some 'update' cases.
	 */
	if (is_update)
		zns->netlink_dplane.seq += 2;
	else
		zns->netlink_dplane.seq++;
#endif	/* HAVE_NETLINK */

	return AOK;
}

/*
 * Initialize a context block for a route update from zebra data structs.
 */
static int dplane_ctx_route_init(struct zebra_dplane_ctx *ctx,
				 enum dplane_op_e op,
				 struct route_node *rn,
				 struct route_entry *re)
{
	int ret = EINVAL;
	const struct route_table *table = NULL;
	const rib_table_info_t *info;
	const struct prefix *p, *src_p;
	struct zebra_ns *zns;
	struct zebra_vrf *zvrf;
	struct nexthop *nexthop;

	if (!ctx || !rn || !re)
		goto done;

	ctx->zd_op = op;
	ctx->zd_status = ZEBRA_DPLANE_REQUEST_SUCCESS;

	ctx->u.rinfo.zd_type = re->type;
	ctx->u.rinfo.zd_old_type = re->type;

	/* Prefixes: dest, and optional source */
	srcdest_rnode_prefixes(rn, &p, &src_p);

	prefix_copy(&(ctx->u.rinfo.zd_dest), p);

	if (src_p)
		prefix_copy(&(ctx->u.rinfo.zd_src), src_p);
	else
		memset(&(ctx->u.rinfo.zd_src), 0, sizeof(ctx->u.rinfo.zd_src));

	ctx->zd_table_id = re->table;

	ctx->u.rinfo.zd_metric = re->metric;
	ctx->u.rinfo.zd_old_metric = re->metric;
	ctx->zd_vrf_id = re->vrf_id;
	ctx->u.rinfo.zd_mtu = re->mtu;
	ctx->u.rinfo.zd_nexthop_mtu = re->nexthop_mtu;
	ctx->u.rinfo.zd_instance = re->instance;
	ctx->u.rinfo.zd_tag = re->tag;
	ctx->u.rinfo.zd_old_tag = re->tag;
	ctx->u.rinfo.zd_distance = re->distance;

	table = srcdest_rnode_table(rn);
	info = table->info;

	ctx->u.rinfo.zd_afi = info->afi;
	ctx->u.rinfo.zd_safi = info->safi;

	/* Extract ns info - can't use pointers to 'core' structs */
	zvrf = vrf_info_lookup(re->vrf_id);
	zns = zvrf->zns;

	dplane_ctx_ns_init(ctx, zns, (op == DPLANE_OP_ROUTE_UPDATE));

	/* Copy nexthops; recursive info is included too */
	copy_nexthops(&(ctx->u.rinfo.zd_ng.nexthop), re->ng->nexthop, NULL);

	/* TODO -- maybe use array of nexthops to avoid allocs? */

	/* Ensure that the dplane's nexthops flags are clear. */
	for (ALL_NEXTHOPS(ctx->u.rinfo.zd_ng, nexthop))
		UNSET_FLAG(nexthop->flags, NEXTHOP_FLAG_FIB);

	/* Trying out the sequence number idea, so we can try to detect
	 * when a result is stale.
	 */
	re->dplane_sequence = zebra_router_get_next_sequence();
	ctx->zd_seq = re->dplane_sequence;

	ret = AOK;

done:
	return ret;
}

/**
 * dplane_ctx_nexthop_init() - Initialize a context block for a nexthop update
 *
 * @ctx:	Dataplane context to init
 * @op:		Operation being performed
 * @nhe:	Nexthop group hash entry
 *
 * Return:	Result status
 */
static int dplane_ctx_nexthop_init(struct zebra_dplane_ctx *ctx,
				   enum dplane_op_e op,
				   struct nhg_hash_entry *nhe)
{
	struct zebra_ns *zns = NULL;

	int ret = EINVAL;

	if (!ctx || !nhe)
		goto done;

	ctx->zd_op = op;
	ctx->zd_status = ZEBRA_DPLANE_REQUEST_SUCCESS;

	/* Copy over nhe info */
	ctx->u.rinfo.zd_nhe.id = nhe->id;
	ctx->u.rinfo.zd_nhe.vrf_id = nhe->vrf_id;
	ctx->u.rinfo.zd_nhe.afi = nhe->afi;
	ctx->u.rinfo.zd_nhe.refcnt = nhe->refcnt;
	ctx->u.rinfo.zd_nhe.is_kernel_nh = nhe->is_kernel_nh;
	ctx->u.rinfo.zd_nhe.dplane_ref = nhe->dplane_ref;
	ctx->u.rinfo.zd_nhe.ifp = nhe->ifp;

	ctx->u.rinfo.zd_nhe.nhg = nexthop_group_new();
	nexthop_group_copy(ctx->u.rinfo.zd_nhe.nhg, nhe->nhg);

	if (nhe->nhg_depends)
		ctx->u.rinfo.zd_nhe.nhg_depends = list_dup(nhe->nhg_depends);


	/* Extract ns info - can't use pointers to 'core' structs */
	zns = ((struct zebra_vrf *)vrf_info_lookup(nhe->vrf_id))->zns;

	// TODO: Might not need to mark this as an update, since
	// it probably won't require two messages
	dplane_ctx_ns_init(ctx, zns, (op == DPLANE_OP_NH_UPDATE));

	ret = AOK;

done:
	return ret;
}

/*
 * Capture information for an LSP update in a dplane context.
 */
static int dplane_ctx_lsp_init(struct zebra_dplane_ctx *ctx,
			       enum dplane_op_e op,
			       zebra_lsp_t *lsp)
{
	int ret = AOK;
	zebra_nhlfe_t *nhlfe, *new_nhlfe;

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("init dplane ctx %s: in-label %u ecmp# %d",
			   dplane_op2str(op), lsp->ile.in_label,
			   lsp->num_ecmp);

	ctx->zd_op = op;
	ctx->zd_status = ZEBRA_DPLANE_REQUEST_SUCCESS;

	/* Capture namespace info */
	dplane_ctx_ns_init(ctx, zebra_ns_lookup(NS_DEFAULT),
			   (op == DPLANE_OP_LSP_UPDATE));

	memset(&ctx->u.lsp, 0, sizeof(ctx->u.lsp));

	ctx->u.lsp.ile = lsp->ile;
	ctx->u.lsp.addr_family = lsp->addr_family;
	ctx->u.lsp.num_ecmp = lsp->num_ecmp;
	ctx->u.lsp.flags = lsp->flags;

	/* Copy source LSP's nhlfes, and capture 'best' nhlfe */
	for (nhlfe = lsp->nhlfe_list; nhlfe; nhlfe = nhlfe->next) {
		/* Not sure if this is meaningful... */
		if (nhlfe->nexthop == NULL)
			continue;

		new_nhlfe =
			zebra_mpls_lsp_add_nhlfe(
				&(ctx->u.lsp),
				nhlfe->type,
				nhlfe->nexthop->type,
				&(nhlfe->nexthop->gate),
				nhlfe->nexthop->ifindex,
				nhlfe->nexthop->nh_label->label[0]);

		if (new_nhlfe == NULL || new_nhlfe->nexthop == NULL) {
			ret = ENOMEM;
			break;
		}

		/* Need to copy flags too */
		new_nhlfe->flags = nhlfe->flags;
		new_nhlfe->nexthop->flags = nhlfe->nexthop->flags;

		if (nhlfe == lsp->best_nhlfe)
			ctx->u.lsp.best_nhlfe = new_nhlfe;
	}

	/* On error the ctx will be cleaned-up, so we don't need to
	 * deal with any allocated nhlfe or nexthop structs here.
	 */

	return ret;
}

/*
 * Capture information for an LSP update in a dplane context.
 */
static int dplane_ctx_pw_init(struct zebra_dplane_ctx *ctx,
			      enum dplane_op_e op,
			      struct zebra_pw *pw)
{
	struct prefix p;
	afi_t afi;
	struct route_table *table;
	struct route_node *rn;
	struct route_entry *re;

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("init dplane ctx %s: pw '%s', loc %u, rem %u",
			   dplane_op2str(op), pw->ifname, pw->local_label,
			   pw->remote_label);

	ctx->zd_op = op;
	ctx->zd_status = ZEBRA_DPLANE_REQUEST_SUCCESS;

	/* Capture namespace info: no netlink support as of 12/18,
	 * but just in case...
	 */
	dplane_ctx_ns_init(ctx, zebra_ns_lookup(NS_DEFAULT), false);

	memset(&ctx->u.pw, 0, sizeof(ctx->u.pw));

	/* This name appears to be c-string, so we use string copy. */
	strlcpy(ctx->u.pw.ifname, pw->ifname, sizeof(ctx->u.pw.ifname));

	ctx->zd_vrf_id = pw->vrf_id;
	ctx->u.pw.ifindex = pw->ifindex;
	ctx->u.pw.type = pw->type;
	ctx->u.pw.af = pw->af;
	ctx->u.pw.local_label = pw->local_label;
	ctx->u.pw.remote_label = pw->remote_label;
	ctx->u.pw.flags = pw->flags;

	ctx->u.pw.dest = pw->nexthop;

	ctx->u.pw.fields = pw->data;

	/* Capture nexthop info for the pw destination. We need to look
	 * up and use zebra datastructs, but we're running in the zebra
	 * pthread here so that should be ok.
	 */
	memcpy(&p.u, &pw->nexthop, sizeof(pw->nexthop));
	p.family = pw->af;
	p.prefixlen = ((pw->af == AF_INET) ?
		       IPV4_MAX_PREFIXLEN : IPV6_MAX_PREFIXLEN);

	afi = (pw->af == AF_INET) ? AFI_IP : AFI_IP6;
	table = zebra_vrf_table(afi, SAFI_UNICAST, pw->vrf_id);
	if (table) {
		rn = route_node_match(table, &p);
		if (rn) {
			RNODE_FOREACH_RE(rn, re) {
				if (CHECK_FLAG(re->flags, ZEBRA_FLAG_SELECTED))
					break;
			}

			if (re)
				copy_nexthops(&(ctx->u.pw.nhg.nexthop),
					      re->ng->nexthop, NULL);

			route_unlock_node(rn);
		}
	}

	return AOK;
}

/*
 * Enqueue a new route update,
 * and ensure an event is active for the dataplane pthread.
 */
static int dplane_route_enqueue(struct zebra_dplane_ctx *ctx)
{
	int ret = EINVAL;
	uint32_t high, curr;

	/* Enqueue for processing by the dataplane pthread */
	DPLANE_LOCK();
	{
		TAILQ_INSERT_TAIL(&zdplane_info.dg_route_ctx_q, ctx,
				  zd_q_entries);
	}
	DPLANE_UNLOCK();

	curr = atomic_add_fetch_explicit(
#ifdef __clang__
		/* TODO -- issue with the clang atomic/intrinsics currently;
		 * casting away the 'Atomic'-ness of the variable works.
		 */
		(uint32_t *)&(zdplane_info.dg_routes_queued),
#else
		&(zdplane_info.dg_routes_queued),
#endif
		1, memory_order_seq_cst);

	/* Maybe update high-water counter also */
	high = atomic_load_explicit(&zdplane_info.dg_routes_queued_max,
				    memory_order_seq_cst);
	while (high < curr) {
		if (atomic_compare_exchange_weak_explicit(
			    &zdplane_info.dg_routes_queued_max,
			    &high, curr,
			    memory_order_seq_cst,
			    memory_order_seq_cst))
			break;
	}

	/* Ensure that an event for the dataplane thread is active */
	ret = dplane_provider_work_ready();

	return ret;
}

/*
 * Utility that prepares a route update and enqueues it for processing
 */
static enum zebra_dplane_result
dplane_route_update_internal(struct route_node *rn,
			     struct route_entry *re,
			     struct route_entry *old_re,
			     enum dplane_op_e op)
{
	enum zebra_dplane_result result = ZEBRA_DPLANE_REQUEST_FAILURE;
	int ret = EINVAL;
	struct zebra_dplane_ctx *ctx = NULL;

	/* Obtain context block */
	ctx = dplane_ctx_alloc();
	if (ctx == NULL) {
		ret = ENOMEM;
		goto done;
	}

	/* Init context with info from zebra data structs */
	ret = dplane_ctx_route_init(ctx, op, rn, re);
	if (ret == AOK) {
		/* Capture some extra info for update case
		 * where there's a different 'old' route.
		 */
		if ((op == DPLANE_OP_ROUTE_UPDATE) &&
		    old_re && (old_re != re)) {
			ctx->zd_is_update = true;

			old_re->dplane_sequence =
				zebra_router_get_next_sequence();
			ctx->zd_old_seq = old_re->dplane_sequence;

			ctx->u.rinfo.zd_old_tag = old_re->tag;
			ctx->u.rinfo.zd_old_type = old_re->type;
			ctx->u.rinfo.zd_old_instance = old_re->instance;
			ctx->u.rinfo.zd_old_distance = old_re->distance;
			ctx->u.rinfo.zd_old_metric = old_re->metric;

#ifndef HAVE_NETLINK
			/* For bsd, capture previous re's nexthops too, sigh.
			 * We'll need these to do per-nexthop deletes.
			 */
			copy_nexthops(&(ctx->u.rinfo.zd_old_ng.nexthop),
				      old_re->ng.nexthop, NULL);
#endif	/* !HAVE_NETLINK */
		}

		/* Enqueue context for processing */
		ret = dplane_route_enqueue(ctx);
	}

done:
	/* Update counter */
	atomic_fetch_add_explicit(&zdplane_info.dg_routes_in, 1,
				  memory_order_relaxed);

	if (ret == AOK)
		result = ZEBRA_DPLANE_REQUEST_QUEUED;
	else {
		atomic_fetch_add_explicit(&zdplane_info.dg_route_errors, 1,
					  memory_order_relaxed);
		if (ctx)
			dplane_ctx_free(&ctx);
	}

	return result;
}

/**
 * dplane_nexthop_update_internal() - Helper for enqueuing nexthop changes
 *
 * @nhe:	Nexthop group hash entry where the change occured
 * @op:		The operation to be enqued
 *
 * Return:	Result of the change
 */
static enum zebra_dplane_result
dplane_nexthop_update_internal(struct nhg_hash_entry *nhe, enum dplane_op_e op)
{
	enum zebra_dplane_result result = ZEBRA_DPLANE_REQUEST_FAILURE;
	int ret = EINVAL;
	struct zebra_dplane_ctx *ctx = NULL;

	/* Obtain context block */
	ctx = dplane_ctx_alloc();
	if (!ctx) {
		ret = ENOMEM;
		goto done;
	}

	ret = dplane_ctx_nexthop_init(ctx, op, nhe);
	if (ret == AOK) {
		ret = dplane_route_enqueue(ctx);
	}
done:
	/* Update counter */
	atomic_fetch_add_explicit(&zdplane_info.dg_nexthops_in, 1,
				  memory_order_relaxed);

	if (ret == AOK)
		result = ZEBRA_DPLANE_REQUEST_QUEUED;
	else {
		atomic_fetch_add_explicit(&zdplane_info.dg_nexthop_errors, 1,
					  memory_order_relaxed);
		if (ctx)
			dplane_ctx_free(&ctx);
	}

	return result;
}

/*
 * Enqueue a route 'add' for the dataplane.
 */
enum zebra_dplane_result dplane_route_add(struct route_node *rn,
					  struct route_entry *re)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (rn == NULL || re == NULL)
		goto done;

	ret = dplane_route_update_internal(rn, re, NULL,
					   DPLANE_OP_ROUTE_INSTALL);

done:
	return ret;
}

/*
 * Enqueue a route update for the dataplane.
 */
enum zebra_dplane_result dplane_route_update(struct route_node *rn,
					     struct route_entry *re,
					     struct route_entry *old_re)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (rn == NULL || re == NULL)
		goto done;

	ret = dplane_route_update_internal(rn, re, old_re,
					   DPLANE_OP_ROUTE_UPDATE);
done:
	return ret;
}

/*
 * Enqueue a route removal for the dataplane.
 */
enum zebra_dplane_result dplane_route_delete(struct route_node *rn,
					     struct route_entry *re)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (rn == NULL || re == NULL)
		goto done;

	ret = dplane_route_update_internal(rn, re, NULL,
					   DPLANE_OP_ROUTE_DELETE);

done:
	return ret;
}

/*
 * Enqueue a nexthop add for the dataplane.
 */
enum zebra_dplane_result dplane_nexthop_add(struct nhg_hash_entry *nhe)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (nhe)
		ret = dplane_nexthop_update_internal(nhe, DPLANE_OP_NH_INSTALL);
	return ret;
}

/*
 * Enqueue a nexthop update for the dataplane.
 */
enum zebra_dplane_result dplane_nexthop_update(struct nhg_hash_entry *nhe)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (nhe)
		ret = dplane_nexthop_update_internal(nhe, DPLANE_OP_NH_UPDATE);
	return ret;
}

/*
 * Enqueue a nexthop removal for the dataplane.
 */
enum zebra_dplane_result dplane_nexthop_delete(struct nhg_hash_entry *nhe)
{
	enum zebra_dplane_result ret = ZEBRA_DPLANE_REQUEST_FAILURE;

	if (nhe)
		ret = dplane_nexthop_update_internal(nhe, DPLANE_OP_NH_DELETE);

	return ret;
}
/*
 * Enqueue LSP add for the dataplane.
 */
enum zebra_dplane_result dplane_lsp_add(zebra_lsp_t *lsp)
{
	enum zebra_dplane_result ret =
		lsp_update_internal(lsp, DPLANE_OP_LSP_INSTALL);

	return ret;
}

/*
 * Enqueue LSP update for the dataplane.
 */
enum zebra_dplane_result dplane_lsp_update(zebra_lsp_t *lsp)
{
	enum zebra_dplane_result ret =
		lsp_update_internal(lsp, DPLANE_OP_LSP_UPDATE);

	return ret;
}

/*
 * Enqueue LSP delete for the dataplane.
 */
enum zebra_dplane_result dplane_lsp_delete(zebra_lsp_t *lsp)
{
	enum zebra_dplane_result ret =
		lsp_update_internal(lsp, DPLANE_OP_LSP_DELETE);

	return ret;
}

/*
 * Enqueue pseudowire install for the dataplane.
 */
enum zebra_dplane_result dplane_pw_install(struct zebra_pw *pw)
{
	return pw_update_internal(pw, DPLANE_OP_PW_INSTALL);
}

/*
 * Enqueue pseudowire un-install for the dataplane.
 */
enum zebra_dplane_result dplane_pw_uninstall(struct zebra_pw *pw)
{
	return pw_update_internal(pw, DPLANE_OP_PW_UNINSTALL);
}

/*
 * Common internal LSP update utility
 */
static enum zebra_dplane_result lsp_update_internal(zebra_lsp_t *lsp,
						    enum dplane_op_e op)
{
	enum zebra_dplane_result result = ZEBRA_DPLANE_REQUEST_FAILURE;
	int ret = EINVAL;
	struct zebra_dplane_ctx *ctx = NULL;

	/* Obtain context block */
	ctx = dplane_ctx_alloc();
	if (ctx == NULL) {
		ret = ENOMEM;
		goto done;
	}

	ret = dplane_ctx_lsp_init(ctx, op, lsp);
	if (ret != AOK)
		goto done;

	ret = dplane_route_enqueue(ctx);

done:
	/* Update counter */
	atomic_fetch_add_explicit(&zdplane_info.dg_lsps_in, 1,
				  memory_order_relaxed);

	if (ret == AOK)
		result = ZEBRA_DPLANE_REQUEST_QUEUED;
	else {
		atomic_fetch_add_explicit(&zdplane_info.dg_lsp_errors, 1,
					  memory_order_relaxed);
		if (ctx)
			dplane_ctx_free(&ctx);
	}

	return result;
}

/*
 * Internal, common handler for pseudowire updates.
 */
static enum zebra_dplane_result pw_update_internal(struct zebra_pw *pw,
						   enum dplane_op_e op)
{
	enum zebra_dplane_result result = ZEBRA_DPLANE_REQUEST_FAILURE;
	int ret;
	struct zebra_dplane_ctx *ctx = NULL;

	ctx = dplane_ctx_alloc();
	if (ctx == NULL) {
		ret = ENOMEM;
		goto done;
	}

	ret = dplane_ctx_pw_init(ctx, op, pw);
	if (ret != AOK)
		goto done;

	ret = dplane_route_enqueue(ctx);

done:
	/* Update counter */
	atomic_fetch_add_explicit(&zdplane_info.dg_pws_in, 1,
				  memory_order_relaxed);

	if (ret == AOK)
		result = ZEBRA_DPLANE_REQUEST_QUEUED;
	else {
		atomic_fetch_add_explicit(&zdplane_info.dg_pw_errors, 1,
					  memory_order_relaxed);
		if (ctx)
			dplane_ctx_free(&ctx);
	}

	return result;
}

/*
 * Handler for 'show dplane'
 */
int dplane_show_helper(struct vty *vty, bool detailed)
{
	uint64_t queued, queue_max, limit, errs, incoming, yields,
		other_errs;

	/* Using atomics because counters are being changed in different
	 * pthread contexts.
	 */
	incoming = atomic_load_explicit(&zdplane_info.dg_routes_in,
					memory_order_relaxed);
	limit = atomic_load_explicit(&zdplane_info.dg_max_queued_updates,
				     memory_order_relaxed);
	queued = atomic_load_explicit(&zdplane_info.dg_routes_queued,
				      memory_order_relaxed);
	queue_max = atomic_load_explicit(&zdplane_info.dg_routes_queued_max,
					 memory_order_relaxed);
	errs = atomic_load_explicit(&zdplane_info.dg_route_errors,
				    memory_order_relaxed);
	yields = atomic_load_explicit(&zdplane_info.dg_update_yields,
				      memory_order_relaxed);
	other_errs = atomic_load_explicit(&zdplane_info.dg_other_errors,
					  memory_order_relaxed);

	vty_out(vty, "Zebra dataplane:\nRoute updates:            %"PRIu64"\n",
		incoming);
	vty_out(vty, "Route update errors:      %"PRIu64"\n", errs);
	vty_out(vty, "Other errors       :      %"PRIu64"\n", other_errs);
	vty_out(vty, "Route update queue limit: %"PRIu64"\n", limit);
	vty_out(vty, "Route update queue depth: %"PRIu64"\n", queued);
	vty_out(vty, "Route update queue max:   %"PRIu64"\n", queue_max);
	vty_out(vty, "Dplane update yields:      %"PRIu64"\n", yields);

	return CMD_SUCCESS;
}

/*
 * Handler for 'show dplane providers'
 */
int dplane_show_provs_helper(struct vty *vty, bool detailed)
{
	struct zebra_dplane_provider *prov;
	uint64_t in, in_max, out, out_max;

	vty_out(vty, "Zebra dataplane providers:\n");

	DPLANE_LOCK();
	prov = TAILQ_FIRST(&zdplane_info.dg_providers_q);
	DPLANE_UNLOCK();

	/* Show counters, useful info from each registered provider */
	while (prov) {

		in = atomic_load_explicit(&prov->dp_in_counter,
					  memory_order_relaxed);
		in_max = atomic_load_explicit(&prov->dp_in_max,
					      memory_order_relaxed);
		out = atomic_load_explicit(&prov->dp_out_counter,
					   memory_order_relaxed);
		out_max = atomic_load_explicit(&prov->dp_out_max,
					       memory_order_relaxed);

		vty_out(vty, "%s (%u): in: %"PRIu64", q_max: %"PRIu64", "
			"out: %"PRIu64", q_max: %"PRIu64"\n",
			prov->dp_name, prov->dp_id, in, in_max, out, out_max);

		DPLANE_LOCK();
		prov = TAILQ_NEXT(prov, dp_prov_link);
		DPLANE_UNLOCK();
	}

	return CMD_SUCCESS;
}

/*
 * Provider registration
 */
int dplane_provider_register(const char *name,
			     enum dplane_provider_prio prio,
			     int flags,
			     int (*fp)(struct zebra_dplane_provider *),
			     int (*fini_fp)(struct zebra_dplane_provider *,
					    bool early),
			     void *data,
			     struct zebra_dplane_provider **prov_p)
{
	int ret = 0;
	struct zebra_dplane_provider *p = NULL, *last;

	/* Validate */
	if (fp == NULL) {
		ret = EINVAL;
		goto done;
	}

	if (prio <= DPLANE_PRIO_NONE ||
	    prio > DPLANE_PRIO_LAST) {
		ret = EINVAL;
		goto done;
	}

	/* Allocate and init new provider struct */
	p = XCALLOC(MTYPE_DP_PROV, sizeof(struct zebra_dplane_provider));

	pthread_mutex_init(&(p->dp_mutex), NULL);
	TAILQ_INIT(&(p->dp_ctx_in_q));
	TAILQ_INIT(&(p->dp_ctx_out_q));

	p->dp_priority = prio;
	p->dp_fp = fp;
	p->dp_fini = fini_fp;
	p->dp_data = data;

	/* Lock - the dplane pthread may be running */
	DPLANE_LOCK();

	p->dp_id = ++zdplane_info.dg_provider_id;

	if (name)
		strlcpy(p->dp_name, name, DPLANE_PROVIDER_NAMELEN);
	else
		snprintf(p->dp_name, DPLANE_PROVIDER_NAMELEN,
			 "provider-%u", p->dp_id);

	/* Insert into list ordered by priority */
	TAILQ_FOREACH(last, &zdplane_info.dg_providers_q, dp_prov_link) {
		if (last->dp_priority > p->dp_priority)
			break;
	}

	if (last)
		TAILQ_INSERT_BEFORE(last, p, dp_prov_link);
	else
		TAILQ_INSERT_TAIL(&zdplane_info.dg_providers_q, p,
				  dp_prov_link);

	/* And unlock */
	DPLANE_UNLOCK();

	if (IS_ZEBRA_DEBUG_DPLANE)
		zlog_debug("dplane: registered new provider '%s' (%u), prio %d",
			   p->dp_name, p->dp_id, p->dp_priority);

done:
	if (prov_p)
		*prov_p = p;

	return ret;
}

/* Accessors for provider attributes */
const char *dplane_provider_get_name(const struct zebra_dplane_provider *prov)
{
	return prov->dp_name;
}

uint32_t dplane_provider_get_id(const struct zebra_dplane_provider *prov)
{
	return prov->dp_id;
}

void *dplane_provider_get_data(const struct zebra_dplane_provider *prov)
{
	return prov->dp_data;
}

int dplane_provider_get_work_limit(const struct zebra_dplane_provider *prov)
{
	return zdplane_info.dg_updates_per_cycle;
}

/* Lock/unlock a provider's mutex - iff the provider was registered with
 * the THREADED flag.
 */
void dplane_provider_lock(struct zebra_dplane_provider *prov)
{
	if (dplane_provider_is_threaded(prov))
		DPLANE_PROV_LOCK(prov);
}

void dplane_provider_unlock(struct zebra_dplane_provider *prov)
{
	if (dplane_provider_is_threaded(prov))
		DPLANE_PROV_UNLOCK(prov);
}

/*
 * Dequeue and maintain associated counter
 */
struct zebra_dplane_ctx *dplane_provider_dequeue_in_ctx(
	struct zebra_dplane_provider *prov)
{
	struct zebra_dplane_ctx *ctx = NULL;

	dplane_provider_lock(prov);

	ctx = TAILQ_FIRST(&(prov->dp_ctx_in_q));
	if (ctx) {
		TAILQ_REMOVE(&(prov->dp_ctx_in_q), ctx, zd_q_entries);

		atomic_fetch_sub_explicit(&prov->dp_in_queued, 1,
					  memory_order_relaxed);
	}

	dplane_provider_unlock(prov);

	return ctx;
}

/*
 * Dequeue work to a list, return count
 */
int dplane_provider_dequeue_in_list(struct zebra_dplane_provider *prov,
				    struct dplane_ctx_q *listp)
{
	int limit, ret;
	struct zebra_dplane_ctx *ctx;

	limit = zdplane_info.dg_updates_per_cycle;

	dplane_provider_lock(prov);

	for (ret = 0; ret < limit; ret++) {
		ctx = TAILQ_FIRST(&(prov->dp_ctx_in_q));
		if (ctx) {
			TAILQ_REMOVE(&(prov->dp_ctx_in_q), ctx, zd_q_entries);

			TAILQ_INSERT_TAIL(listp, ctx, zd_q_entries);
		} else {
			break;
		}
	}

	if (ret > 0)
		atomic_fetch_sub_explicit(&prov->dp_in_queued, ret,
					  memory_order_relaxed);

	dplane_provider_unlock(prov);

	return ret;
}

/*
 * Enqueue and maintain associated counter
 */
void dplane_provider_enqueue_out_ctx(struct zebra_dplane_provider *prov,
				     struct zebra_dplane_ctx *ctx)
{
	dplane_provider_lock(prov);

	TAILQ_INSERT_TAIL(&(prov->dp_ctx_out_q), ctx,
			  zd_q_entries);

	dplane_provider_unlock(prov);

	atomic_fetch_add_explicit(&(prov->dp_out_counter), 1,
				  memory_order_relaxed);
}

/*
 * Accessor for provider object
 */
bool dplane_provider_is_threaded(const struct zebra_dplane_provider *prov)
{
	return (prov->dp_flags & DPLANE_PROV_FLAG_THREADED);
}

/*
 * Internal helper that copies information from a zebra ns object; this is
 * called in the zebra main pthread context as part of dplane ctx init.
 */
static void dplane_info_from_zns(struct zebra_dplane_info *ns_info,
				 struct zebra_ns *zns)
{
	ns_info->ns_id = zns->ns_id;

#if defined(HAVE_NETLINK)
	ns_info->is_cmd = true;
	ns_info->nls = zns->netlink_dplane;
#endif /* NETLINK */
}

/*
 * Provider api to signal that work/events are available
 * for the dataplane pthread.
 */
int dplane_provider_work_ready(void)
{
	/* Note that during zebra startup, we may be offered work before
	 * the dataplane pthread (and thread-master) are ready. We want to
	 * enqueue the work, but the event-scheduling machinery may not be
	 * available.
	 */
	if (zdplane_info.dg_run) {
		thread_add_event(zdplane_info.dg_master,
				 dplane_thread_loop, NULL, 0,
				 &zdplane_info.dg_t_update);
	}

	return AOK;
}

/*
 * Kernel dataplane provider
 */

/*
 * Handler for kernel LSP updates
 */
static enum zebra_dplane_result
kernel_dplane_lsp_update(struct zebra_dplane_ctx *ctx)
{
	enum zebra_dplane_result res;

	/* Call into the synchronous kernel-facing code here */
	res = kernel_lsp_update(ctx);

	if (res != ZEBRA_DPLANE_REQUEST_SUCCESS)
		atomic_fetch_add_explicit(
			&zdplane_info.dg_lsp_errors, 1,
			memory_order_relaxed);

	return res;
}

/*
 * Handler for kernel pseudowire updates
 */
static enum zebra_dplane_result
kernel_dplane_pw_update(struct zebra_dplane_ctx *ctx)
{
	enum zebra_dplane_result res;

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("Dplane pw %s: op %s af %d loc: %u rem: %u",
			   dplane_ctx_get_pw_ifname(ctx),
			   dplane_op2str(ctx->zd_op),
			   dplane_ctx_get_pw_af(ctx),
			   dplane_ctx_get_pw_local_label(ctx),
			   dplane_ctx_get_pw_remote_label(ctx));

	res = kernel_pw_update(ctx);

	if (res != ZEBRA_DPLANE_REQUEST_SUCCESS)
		atomic_fetch_add_explicit(
			&zdplane_info.dg_pw_errors, 1,
			memory_order_relaxed);

	return res;
}

/*
 * Handler for kernel route updates
 */
static enum zebra_dplane_result
kernel_dplane_route_update(struct zebra_dplane_ctx *ctx)
{
	enum zebra_dplane_result res;

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL) {
		char dest_str[PREFIX_STRLEN];

		prefix2str(dplane_ctx_get_dest(ctx),
			   dest_str, sizeof(dest_str));

		zlog_debug("%u:%s Dplane route update ctx %p op %s",
			   dplane_ctx_get_vrf(ctx), dest_str,
			   ctx, dplane_op2str(dplane_ctx_get_op(ctx)));
	}

	/* Call into the synchronous kernel-facing code here */
	res = kernel_route_update(ctx);

	if (res != ZEBRA_DPLANE_REQUEST_SUCCESS)
		atomic_fetch_add_explicit(
			&zdplane_info.dg_route_errors, 1,
			memory_order_relaxed);

	return res;
}

/**
 * kernel_dplane_nexthop_update() - Handler for kernel nexthop updates
 *
 * @ctx:	Dataplane context
 *
 * Return:	Dataplane result flag
 */
static enum zebra_dplane_result
kernel_dplane_nexthop_update(struct zebra_dplane_ctx *ctx)
{
	enum zebra_dplane_result res;

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL) {
		zlog_debug("ID (%u) Dplane nexthop update ctx %p op %s",
			   dplane_ctx_get_nhe(ctx)->id, ctx,
			   dplane_op2str(dplane_ctx_get_op(ctx)));
	}

	res = kernel_nexthop_update(ctx);

	if (res != ZEBRA_DPLANE_REQUEST_SUCCESS)
		atomic_fetch_add_explicit(&zdplane_info.dg_nexthop_errors, 1,
					  memory_order_relaxed);

	return res;
}

/*
 * Kernel provider callback
 */
static int kernel_dplane_process_func(struct zebra_dplane_provider *prov)
{
	enum zebra_dplane_result res;
	struct zebra_dplane_ctx *ctx;
	int counter, limit;

	limit = dplane_provider_get_work_limit(prov);

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("dplane provider '%s': processing",
			   dplane_provider_get_name(prov));

	for (counter = 0; counter < limit; counter++) {

		ctx = dplane_provider_dequeue_in_ctx(prov);
		if (ctx == NULL)
			break;

		/* A previous provider plugin may have asked to skip the
		 * kernel update.
		 */
		if (dplane_ctx_is_skip_kernel(ctx)) {
			res = ZEBRA_DPLANE_REQUEST_SUCCESS;
			goto skip_one;
		}

		/* Dispatch to appropriate kernel-facing apis */
		switch (dplane_ctx_get_op(ctx)) {

		case DPLANE_OP_ROUTE_INSTALL:
		case DPLANE_OP_ROUTE_UPDATE:
		case DPLANE_OP_ROUTE_DELETE:
			res = kernel_dplane_route_update(ctx);
			break;

		case DPLANE_OP_NH_INSTALL:
		case DPLANE_OP_NH_UPDATE:
		case DPLANE_OP_NH_DELETE:
			res = kernel_dplane_nexthop_update(ctx);
			break;

		case DPLANE_OP_LSP_INSTALL:
		case DPLANE_OP_LSP_UPDATE:
		case DPLANE_OP_LSP_DELETE:
			res = kernel_dplane_lsp_update(ctx);
			break;

		case DPLANE_OP_PW_INSTALL:
		case DPLANE_OP_PW_UNINSTALL:
			res = kernel_dplane_pw_update(ctx);
			break;

		default:
			atomic_fetch_add_explicit(
				&zdplane_info.dg_other_errors, 1,
				memory_order_relaxed);

			res = ZEBRA_DPLANE_REQUEST_FAILURE;
			break;
		}

skip_one:
		dplane_ctx_set_status(ctx, res);

		dplane_provider_enqueue_out_ctx(prov, ctx);
	}

	/* Ensure that we'll run the work loop again if there's still
	 * more work to do.
	 */
	if (counter >= limit) {
		if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
			zlog_debug("dplane provider '%s' reached max updates %d",
				   dplane_provider_get_name(prov), counter);

		atomic_fetch_add_explicit(&zdplane_info.dg_update_yields,
					  1, memory_order_relaxed);

		dplane_provider_work_ready();
	}

	return 0;
}

#if DPLANE_TEST_PROVIDER

/*
 * Test dataplane provider plugin
 */

/*
 * Test provider process callback
 */
static int test_dplane_process_func(struct zebra_dplane_provider *prov)
{
	struct zebra_dplane_ctx *ctx;
	int counter, limit;

	/* Just moving from 'in' queue to 'out' queue */

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("dplane provider '%s': processing",
			   dplane_provider_get_name(prov));

	limit = dplane_provider_get_work_limit(prov);

	for (counter = 0; counter < limit; counter++) {

		ctx = dplane_provider_dequeue_in_ctx(prov);
		if (ctx == NULL)
			break;

		dplane_ctx_set_status(ctx, ZEBRA_DPLANE_REQUEST_SUCCESS);

		dplane_provider_enqueue_out_ctx(prov, ctx);
	}

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("dplane provider '%s': processed %d",
			   dplane_provider_get_name(prov), counter);

	/* Ensure that we'll run the work loop again if there's still
	 * more work to do.
	 */
	if (counter >= limit)
		dplane_provider_work_ready();

	return 0;
}

/*
 * Test provider shutdown/fini callback
 */
static int test_dplane_shutdown_func(struct zebra_dplane_provider *prov,
				     bool early)
{
	if (IS_ZEBRA_DEBUG_DPLANE)
		zlog_debug("dplane provider '%s': %sshutdown",
			   dplane_provider_get_name(prov),
			   early ? "early " : "");

	return 0;
}
#endif	/* DPLANE_TEST_PROVIDER */

/*
 * Register default kernel provider
 */
static void dplane_provider_init(void)
{
	int ret;

	ret = dplane_provider_register("Kernel",
				       DPLANE_PRIO_KERNEL,
				       DPLANE_PROV_FLAGS_DEFAULT,
				       kernel_dplane_process_func,
				       NULL,
				       NULL, NULL);

	if (ret != AOK)
		zlog_err("Unable to register kernel dplane provider: %d",
			 ret);

#if DPLANE_TEST_PROVIDER
	/* Optional test provider ... */
	ret = dplane_provider_register("Test",
				       DPLANE_PRIO_PRE_KERNEL,
				       DPLANE_PROV_FLAGS_DEFAULT,
				       test_dplane_process_func,
				       test_dplane_shutdown_func,
				       NULL /* data */, NULL);

	if (ret != AOK)
		zlog_err("Unable to register test dplane provider: %d",
			 ret);
#endif	/* DPLANE_TEST_PROVIDER */
}

/* Indicates zebra shutdown/exit is in progress. Some operations may be
 * simplified or skipped during shutdown processing.
 */
bool dplane_is_in_shutdown(void)
{
	return zdplane_info.dg_is_shutdown;
}

/*
 * Early or pre-shutdown, de-init notification api. This runs pretty
 * early during zebra shutdown, as a signal to stop new work and prepare
 * for updates generated by shutdown/cleanup activity, as zebra tries to
 * remove everything it's responsible for.
 * NB: This runs in the main zebra pthread context.
 */
void zebra_dplane_pre_finish(void)
{
	if (IS_ZEBRA_DEBUG_DPLANE)
		zlog_debug("Zebra dataplane pre-fini called");

	zdplane_info.dg_is_shutdown = true;

	/* TODO -- Notify provider(s) of pending shutdown */
}

/*
 * Utility to determine whether work remains enqueued within the dplane;
 * used during system shutdown processing.
 */
static bool dplane_work_pending(void)
{
	bool ret = false;
	struct zebra_dplane_ctx *ctx;
	struct zebra_dplane_provider *prov;

	/* TODO -- just checking incoming/pending work for now, must check
	 * providers
	 */
	DPLANE_LOCK();
	{
		ctx = TAILQ_FIRST(&zdplane_info.dg_route_ctx_q);
		prov = TAILQ_FIRST(&zdplane_info.dg_providers_q);
	}
	DPLANE_UNLOCK();

	if (ctx != NULL) {
		ret = true;
		goto done;
	}

	while (prov) {

		dplane_provider_lock(prov);

		ctx = TAILQ_FIRST(&(prov->dp_ctx_in_q));
		if (ctx == NULL)
			ctx = TAILQ_FIRST(&(prov->dp_ctx_out_q));

		dplane_provider_unlock(prov);

		if (ctx != NULL)
			break;

		DPLANE_LOCK();
		prov = TAILQ_NEXT(prov, dp_prov_link);
		DPLANE_UNLOCK();
	}

	if (ctx != NULL)
		ret = true;

done:
	return ret;
}

/*
 * Shutdown-time intermediate callback, used to determine when all pending
 * in-flight updates are done. If there's still work to do, reschedules itself.
 * If all work is done, schedules an event to the main zebra thread for
 * final zebra shutdown.
 * This runs in the dplane pthread context.
 */
static int dplane_check_shutdown_status(struct thread *event)
{
	if (IS_ZEBRA_DEBUG_DPLANE)
		zlog_debug("Zebra dataplane shutdown status check called");

	if (dplane_work_pending()) {
		/* Reschedule dplane check on a short timer */
		thread_add_timer_msec(zdplane_info.dg_master,
				      dplane_check_shutdown_status,
				      NULL, 100,
				      &zdplane_info.dg_t_shutdown_check);

		/* TODO - give up and stop waiting after a short time? */

	} else {
		/* We appear to be done - schedule a final callback event
		 * for the zebra main pthread.
		 */
		thread_add_event(zrouter.master, zebra_finalize, NULL, 0, NULL);
	}

	return 0;
}

/*
 * Shutdown, de-init api. This runs pretty late during shutdown,
 * after zebra has tried to free/remove/uninstall all routes during shutdown.
 * At this point, dplane work may still remain to be done, so we can't just
 * blindly terminate. If there's still work to do, we'll periodically check
 * and when done, we'll enqueue a task to the zebra main thread for final
 * termination processing.
 *
 * NB: This runs in the main zebra thread context.
 */
void zebra_dplane_finish(void)
{
	if (IS_ZEBRA_DEBUG_DPLANE)
		zlog_debug("Zebra dataplane fini called");

	thread_add_event(zdplane_info.dg_master,
			 dplane_check_shutdown_status, NULL, 0,
			 &zdplane_info.dg_t_shutdown_check);
}

/*
 * Main dataplane pthread event loop. The thread takes new incoming work
 * and offers it to the first provider. It then iterates through the
 * providers, taking complete work from each one and offering it
 * to the next in order. At each step, a limited number of updates are
 * processed during a cycle in order to provide some fairness.
 *
 * This loop through the providers is only run once, so that the dataplane
 * pthread can look for other pending work - such as i/o work on behalf of
 * providers.
 */
static int dplane_thread_loop(struct thread *event)
{
	struct dplane_ctx_q work_list;
	struct dplane_ctx_q error_list;
	struct zebra_dplane_provider *prov;
	struct zebra_dplane_ctx *ctx, *tctx;
	int limit, counter, error_counter;
	uint64_t curr, high;

	/* Capture work limit per cycle */
	limit = zdplane_info.dg_updates_per_cycle;

	/* Init temporary lists used to move contexts among providers */
	TAILQ_INIT(&work_list);
	TAILQ_INIT(&error_list);
	error_counter = 0;

	/* Check for zebra shutdown */
	if (!zdplane_info.dg_run)
		goto done;

	/* Dequeue some incoming work from zebra (if any) onto the temporary
	 * working list.
	 */
	DPLANE_LOCK();

	/* Locate initial registered provider */
	prov = TAILQ_FIRST(&zdplane_info.dg_providers_q);

	/* Move new work from incoming list to temp list */
	for (counter = 0; counter < limit; counter++) {
		ctx = TAILQ_FIRST(&zdplane_info.dg_route_ctx_q);
		if (ctx) {
			TAILQ_REMOVE(&zdplane_info.dg_route_ctx_q, ctx,
				     zd_q_entries);

			ctx->zd_provider = prov->dp_id;

			TAILQ_INSERT_TAIL(&work_list, ctx, zd_q_entries);
		} else {
			break;
		}
	}

	DPLANE_UNLOCK();

	atomic_fetch_sub_explicit(&zdplane_info.dg_routes_queued, counter,
				  memory_order_relaxed);

	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("dplane: incoming new work counter: %d", counter);

	/* Iterate through the registered providers, offering new incoming
	 * work. If the provider has outgoing work in its queue, take that
	 * work for the next provider
	 */
	while (prov) {

		/* At each iteration, the temporary work list has 'counter'
		 * items.
		 */
		if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
			zlog_debug("dplane enqueues %d new work to provider '%s'",
				   counter, dplane_provider_get_name(prov));

		/* Capture current provider id in each context; check for
		 * error status.
		 */
		TAILQ_FOREACH_SAFE(ctx, &work_list, zd_q_entries, tctx) {
			if (dplane_ctx_get_status(ctx) ==
			    ZEBRA_DPLANE_REQUEST_SUCCESS) {
				ctx->zd_provider = prov->dp_id;
			} else {
				/*
				 * TODO -- improve error-handling: recirc
				 * errors backwards so that providers can
				 * 'undo' their work (if they want to)
				 */

				/* Move to error list; will be returned
				 * zebra main.
				 */
				TAILQ_REMOVE(&work_list, ctx, zd_q_entries);
				TAILQ_INSERT_TAIL(&error_list,
						  ctx, zd_q_entries);
				error_counter++;
			}
		}

		/* Enqueue new work to the provider */
		dplane_provider_lock(prov);

		if (TAILQ_FIRST(&work_list))
			TAILQ_CONCAT(&(prov->dp_ctx_in_q), &work_list,
				     zd_q_entries);

		atomic_fetch_add_explicit(&prov->dp_in_counter, counter,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&prov->dp_in_queued, counter,
					  memory_order_relaxed);
		curr = atomic_load_explicit(&prov->dp_in_queued,
					    memory_order_relaxed);
		high = atomic_load_explicit(&prov->dp_in_max,
					    memory_order_relaxed);
		if (curr > high)
			atomic_store_explicit(&prov->dp_in_max, curr,
					      memory_order_relaxed);

		dplane_provider_unlock(prov);

		/* Reset the temp list (though the 'concat' may have done this
		 * already), and the counter
		 */
		TAILQ_INIT(&work_list);
		counter = 0;

		/* Call into the provider code. Note that this is
		 * unconditional: we offer to do work even if we don't enqueue
		 * any _new_ work.
		 */
		(*prov->dp_fp)(prov);

		/* Check for zebra shutdown */
		if (!zdplane_info.dg_run)
			break;

		/* Dequeue completed work from the provider */
		dplane_provider_lock(prov);

		while (counter < limit) {
			ctx = TAILQ_FIRST(&(prov->dp_ctx_out_q));
			if (ctx) {
				TAILQ_REMOVE(&(prov->dp_ctx_out_q), ctx,
					     zd_q_entries);

				TAILQ_INSERT_TAIL(&work_list,
						  ctx, zd_q_entries);
				counter++;
			} else
				break;
		}

		dplane_provider_unlock(prov);

		if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
			zlog_debug("dplane dequeues %d completed work from provider %s",
				   counter, dplane_provider_get_name(prov));

		/* Locate next provider */
		DPLANE_LOCK();
		prov = TAILQ_NEXT(prov, dp_prov_link);
		DPLANE_UNLOCK();
	}

	/* After all providers have been serviced, enqueue any completed
	 * work and any errors back to zebra so it can process the results.
	 */
	if (IS_ZEBRA_DEBUG_DPLANE_DETAIL)
		zlog_debug("dplane has %d completed, %d errors, for zebra main",
			   counter, error_counter);

	/*
	 * Hand lists through the api to zebra main,
	 * to reduce the number of lock/unlock cycles
	 */

	/* Call through to zebra main */
	(zdplane_info.dg_results_cb)(&error_list);

	TAILQ_INIT(&error_list);


	/* Call through to zebra main */
	(zdplane_info.dg_results_cb)(&work_list);

	TAILQ_INIT(&work_list);

done:
	return 0;
}

/*
 * Final phase of shutdown, after all work enqueued to dplane has been
 * processed. This is called from the zebra main pthread context.
 */
void zebra_dplane_shutdown(void)
{
	if (IS_ZEBRA_DEBUG_DPLANE)
		zlog_debug("Zebra dataplane shutdown called");

	/* Stop dplane thread, if it's running */

	zdplane_info.dg_run = false;

	THREAD_OFF(zdplane_info.dg_t_update);

	frr_pthread_stop(zdplane_info.dg_pthread, NULL);

	/* Destroy pthread */
	frr_pthread_destroy(zdplane_info.dg_pthread);
	zdplane_info.dg_pthread = NULL;
	zdplane_info.dg_master = NULL;

	/* TODO -- Notify provider(s) of final shutdown */

	/* TODO -- Clean-up provider objects */

	/* TODO -- Clean queue(s), free memory */
}

/*
 * Initialize the dataplane module during startup, internal/private version
 */
static void zebra_dplane_init_internal(void)
{
	memset(&zdplane_info, 0, sizeof(zdplane_info));

	pthread_mutex_init(&zdplane_info.dg_mutex, NULL);

	TAILQ_INIT(&zdplane_info.dg_route_ctx_q);
	TAILQ_INIT(&zdplane_info.dg_providers_q);

	zdplane_info.dg_updates_per_cycle = DPLANE_DEFAULT_NEW_WORK;

	zdplane_info.dg_max_queued_updates = DPLANE_DEFAULT_MAX_QUEUED;

	/* Register default kernel 'provider' during init */
	dplane_provider_init();
}

/*
 * Start the dataplane pthread. This step needs to be run later than the
 * 'init' step, in case zebra has fork-ed.
 */
void zebra_dplane_start(void)
{
	/* Start dataplane pthread */

	struct frr_pthread_attr pattr = {
		.start = frr_pthread_attr_default.start,
		.stop = frr_pthread_attr_default.stop
	};

	zdplane_info.dg_pthread = frr_pthread_new(&pattr, "Zebra dplane thread",
						  "Zebra dplane");

	zdplane_info.dg_master = zdplane_info.dg_pthread->master;

	zdplane_info.dg_run = true;

	/* Enqueue an initial event for the dataplane pthread */
	thread_add_event(zdplane_info.dg_master, dplane_thread_loop, NULL, 0,
			 &zdplane_info.dg_t_update);

	frr_pthread_run(zdplane_info.dg_pthread, NULL);
}

/*
 * Initialize the dataplane module at startup; called by zebra rib_init()
 */
void zebra_dplane_init(int (*results_fp)(struct dplane_ctx_q *))
{
	zebra_dplane_init_internal();
	zdplane_info.dg_results_cb = results_fp;
}
