/*
 * Copyright (C) 2013-2014 Kay Sievers
 * Copyright (C) 2013-2014 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013-2014 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 * Copyright (C) 2013-2014 Linux Foundation
 * Copyright (C) 2014 Djalal Harouni <tixxdz@opendz.org>
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "bus.h"
#include "connection.h"
#include "domain.h"
#include "endpoint.h"
#include "handle.h"
#include "item.h"
#include "message.h"
#include "policy.h"

static void kdbus_ep_free(struct kdbus_node *node)
{
	struct kdbus_ep *ep = container_of(node, struct kdbus_ep, node);

	WARN_ON(!list_empty(&ep->conn_list));

	kdbus_policy_db_clear(&ep->policy_db);
	kdbus_bus_unref(ep->bus);
	kdbus_domain_user_unref(ep->user);
	kfree(ep);
}

static void kdbus_ep_release(struct kdbus_node *node, bool was_active)
{
	struct kdbus_ep *ep = container_of(node, struct kdbus_ep, node);

	/* disconnect all connections to this endpoint */
	for (;;) {
		struct kdbus_conn *conn;

		mutex_lock(&ep->lock);
		conn = list_first_entry_or_null(&ep->conn_list,
						struct kdbus_conn,
						ep_entry);
		if (!conn) {
			mutex_unlock(&ep->lock);
			break;
		}

		/* take reference, release lock, disconnect without lock */
		kdbus_conn_ref(conn);
		mutex_unlock(&ep->lock);

		kdbus_conn_disconnect(conn, false);
		kdbus_conn_unref(conn);
	}
}

/**
 * kdbus_ep_new() - create a new endpoint
 * @bus:		The bus this endpoint will be created for
 * @name:		The name of the endpoint
 * @access:		The access flags for this node (KDBUS_MAKE_ACCESS_*)
 * @uid:		The uid of the node
 * @gid:		The gid of the node
 * @is_custom:		Whether this is a custom endpoint
 *
 * This function will create a new enpoint with the given
 * name and properties for a given bus.
 *
 * Return: a new kdbus_ep on success, ERR_PTR on failure.
 */
struct kdbus_ep *kdbus_ep_new(struct kdbus_bus *bus, const char *name,
			      unsigned int access, kuid_t uid, kgid_t gid,
			      bool is_custom)
{
	struct kdbus_ep *e;
	int ret;

	/*
	 * Validate only custom endpoints names, default endpoints
	 * with a "bus" name are created when the bus is created
	 */
	if (is_custom) {
		ret = kdbus_verify_uid_prefix(name,
					      bus->domain->user_namespace,
					      uid);
		if (ret < 0)
			return ERR_PTR(ret);
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return ERR_PTR(-ENOMEM);

	kdbus_node_init(&e->node, KDBUS_NODE_ENDPOINT);

	e->node.free_cb = kdbus_ep_free;
	e->node.release_cb = kdbus_ep_release;
	e->node.uid = uid;
	e->node.gid = gid;
	e->node.mode = S_IRUSR | S_IWUSR;
	if (access & (KDBUS_MAKE_ACCESS_GROUP | KDBUS_MAKE_ACCESS_WORLD))
		e->node.mode |= S_IRGRP | S_IWGRP;
	if (access & KDBUS_MAKE_ACCESS_WORLD)
		e->node.mode |= S_IROTH | S_IWOTH;

	mutex_init(&e->lock);
	INIT_LIST_HEAD(&e->conn_list);
	kdbus_policy_db_init(&e->policy_db);
	e->has_policy = is_custom;
	e->bus = kdbus_bus_ref(bus);
	e->id = atomic64_inc_return(&bus->ep_seq_last);

	ret = kdbus_node_link(&e->node, &bus->node, name);
	if (ret < 0)
		goto exit_unref;

	/*
	 * Transactions on custom endpoints are never accounted on the global
	 * user limits. Instead, for each custom endpoint, we create a custom,
	 * unique user, which all transactions are accounted on. Regardless of
	 * the user using that endpoint, it is always accounted on the same
	 * user-object. This budget is not shared with ordniary users on
	 * non-custom endpoints.
	 */
	if (is_custom) {
		e->user = kdbus_domain_get_user(bus->domain, INVALID_UID);
		if (IS_ERR(e->user)) {
			ret = PTR_ERR(e->user);
			e->user = NULL;
			goto exit_unref;
		}
	}

	return e;

exit_unref:
	kdbus_node_deactivate(&e->node);
	kdbus_node_unref(&e->node);
	return ERR_PTR(ret);
}

/**
 * kdbus_ep_ref() - increase the reference counter of a kdbus_ep
 * @ep:			The endpoint to reference
 *
 * Every user of an endpoint, except for its creator, must add a reference to
 * the kdbus_ep instance using this function.
 *
 * Return: the ep itself
 */
struct kdbus_ep *kdbus_ep_ref(struct kdbus_ep *ep)
{
	if (ep)
		kdbus_node_ref(&ep->node);
	return ep;
}

/**
 * kdbus_ep_unref() - decrease the reference counter of a kdbus_ep
 * @ep:		The ep to unref
 *
 * Release a reference. If the reference count drops to 0, the ep will be
 * freed.
 *
 * Return: NULL
 */
struct kdbus_ep *kdbus_ep_unref(struct kdbus_ep *ep)
{
	if (ep)
		kdbus_node_unref(&ep->node);
	return NULL;
}

/**
 * kdbus_ep_activate() - Activatean endpoint
 * @ep:			Endpoint
 *
 * Return: 0 on success, negative error otherwise.
 */
int kdbus_ep_activate(struct kdbus_ep *ep)
{
	/*
	 * kdbus_ep_activate() must not be called multiple times, so if
	 * kdbus_node_activate() didn't activate the node, it must already be
	 * dead.
	 */
	if (!kdbus_node_activate(&ep->node))
		return -ESHUTDOWN;

	return 0;
}

/**
 * kdbus_ep_deactivate() - invalidate an endpoint
 * @ep:			Endpoint
 */
void kdbus_ep_deactivate(struct kdbus_ep *ep)
{
	kdbus_node_deactivate(&ep->node);
}

/**
 * kdbus_ep_policy_set() - set policy for an endpoint
 * @ep:			The endpoint
 * @items:		The kdbus items containing policy information
 * @items_size:		The total length of the items
 *
 * Only the endpoint owner should be able to call this function.
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_ep_policy_set(struct kdbus_ep *ep,
			const struct kdbus_item *items,
			size_t items_size)
{
	return kdbus_policy_set(&ep->policy_db, items, items_size, 0, true, ep);
}
