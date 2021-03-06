/* -*- mode: c; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: t -*-
 * vim: ts=4 sw=4 noet ai cindent syntax=c
 *
 * Conky, a system monitor, based on torsmo
 *
 * Any original torsmo code is licensed under the BSD license
 *
 * All code written since the fork of torsmo is licensed under the GPL
 *
 * Please see COPYING for details
 *
 * Copyright (c) 2005-2010 Brenden Matthews, Philip Kovacs, et. al.
 *	(see AUTHORS)
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "text_object.h"
#include "logging.h"
#include <stdlib.h>
#include <stdio.h>

/* text_object_list
 *
 * this list is special. it looks like this:
 *  NULL <-- obj1 <--> obj2 <--> ... <--> objN --> NULL
 *            ^-------root_object----------^
 *             directions are reversed here
 *
 * why this is cool:
 * - root_object points both to the start and end of the list
 * - while traversing, the end of the list is always a NULL pointer
 *   (this works in BOTH directions)
 */

/* append an object to the given root object's list */
int append_object(struct text_object *root, struct text_object *obj)
{
	struct text_object *end;

	end = root->prev;
	obj->prev = end;
	obj->next = NULL;

	if (end) {
		if (end->next)
			CRIT_ERR(NULL, NULL, "houston, we have a lift-off");
		end->next = obj;
	} else {
		root->next = obj;
	}
	root->prev = obj;
	return 0;
}

/* ifblock handlers for the object list
 *
 * - each if points to it's else or endif
 * - each else points to it's endif
 *
 */

/* possible ifblock types
 * only used internally, so no need to make this public
 */
enum ifblock_type {
	IFBLOCK_IF = 1,
	IFBLOCK_ELSE,
	IFBLOCK_ENDIF,
};

/* linked list of ifblock objects, building a stack
 * only used internally, so no need to make this public
 */
struct ifblock_stack_obj {
	enum ifblock_type type;
	struct text_object *obj;
	struct ifblock_stack_obj *next;
};

/* push an ifblock object onto the stack
 * in fact, this does a lot more:
 * - IFBLOCK_IF is just pushed onto the stack
 * - IFBLOCK_ELSE updates the "next" pointer of the upmost
 *   object in the stack and is then pushed onto the stack
 * - IFBLOCK_ENDIF updates the "next" pointer of the upmost
 *   object in the stack and then triggers stack popping of
 *   any optional IFBLOCK_ELSE along with it's IFBLOCK_IF
 */
static int push_ifblock(struct ifblock_stack_obj **ifblock_stack_top,
                        struct text_object *obj, enum ifblock_type type)
{
	struct ifblock_stack_obj *stackobj;

	switch (type) {
		case IFBLOCK_ENDIF:
			if (!(*ifblock_stack_top))
				CRIT_ERR(NULL, NULL, "got an endif without matching if");
			(*ifblock_stack_top)->obj->ifblock_next = obj;
			/* if there's some else in between, remove and free it */
			if ((*ifblock_stack_top)->type == IFBLOCK_ELSE) {
				stackobj = *ifblock_stack_top;
				*ifblock_stack_top = stackobj->next;
				free(stackobj);
			}
			/* finally remove and free the if object */
			stackobj = *ifblock_stack_top;
			*ifblock_stack_top = stackobj->next;
			free(stackobj);
			break;
		case IFBLOCK_ELSE:
			if (!(*ifblock_stack_top))
				CRIT_ERR(NULL, NULL, "got an else without matching if");
			(*ifblock_stack_top)->obj->ifblock_next = obj;
			/* fall through */
		case IFBLOCK_IF:
			stackobj = malloc(sizeof(struct ifblock_stack_obj));
			stackobj->type = type;
			stackobj->obj = obj;
			stackobj->next = *ifblock_stack_top;
			*ifblock_stack_top = stackobj;
			break;
		default:
			CRIT_ERR(NULL, NULL, "push_ifblock() missuse detected!");
	}
	return 0;
}

/* public functions for client use */

int obj_be_ifblock_if(void **opaque, struct text_object *obj)
{
	return push_ifblock((struct ifblock_stack_obj **)opaque, obj, IFBLOCK_IF);
}
int obj_be_ifblock_else(void **opaque, struct text_object *obj)
{
	return push_ifblock((struct ifblock_stack_obj **)opaque, obj, IFBLOCK_ELSE);
}
int obj_be_ifblock_endif(void **opaque, struct text_object *obj)
{
	return push_ifblock((struct ifblock_stack_obj **)opaque, obj, IFBLOCK_ENDIF);
}

/* check if ifblock stack is empty
 * if so, return true (!= 0)
 */
int ifblock_stack_empty(void **opaque)
{
	return *opaque == NULL;
}
