/*
 * Copyright (C) 2013-2014 Kay Sievers
 * Copyright (C) 2013-2014 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013-2014 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 * Copyright (C) 2013-2014 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef __KDBUS_IOCTL_H
#define __KDBUS_IOCTL_H

struct kdbus_bus;
struct kdbus_domain;
struct kdbus_ep;

struct kdbus_bus *kdbus_ioctl_bus_make(struct kdbus_domain *domain,
				       void __user *buf);
struct kdbus_ep *kdbus_ioctl_endpoint_make(struct kdbus_bus *bus,
					   void __user *buf);

#endif