/*
 * libpri: An implementation of Primary Rate ISDN
 *
 * Written by Mark Spencer <markster@digium.com>
 *
 * Copyright (C) 2001-2005, Digium, Inc.
 * All Rights Reserved.
 */

/*
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2 as published by the
 * Free Software Foundation. See the LICENSE file included with
 * this program for more details.
 *
 * In addition, when this program is distributed with Asterisk in
 * any form that would qualify as a 'combined work' or as a
 * 'derivative work' (but not mere aggregation), you can redistribute
 * and/or modify the combination under the terms of the license
 * provided with that copy of Asterisk, instead of the license
 * terms granted here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libpri.h"
#include "pri_internal.h"


/*! Initial number of scheduled timer slots. */
#define SCHED_EVENTS_INITIAL	128
/*!
 * Maximum number of scheduled timer slots.
 * Should be a power of 2 multiple of SCHED_EVENTS_INITIAL.
 */
#define SCHED_EVENTS_MAX		8192

/*! \brief The maximum number of timers that were active at once. */
static unsigned maxsched = 0;

/* Scheduler routines */

/*!
 * \internal
 * \brief Increase the number of scheduler timer slots available.
 *
 * \param ctrl D channel controller.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int pri_schedule_grow(struct pri *ctrl)
{
	unsigned num_slots;
	struct pri_sched *timers;

	/* Determine how many slots in the new timer table. */
	if (ctrl->sched.num_slots) {
		if (SCHED_EVENTS_MAX <= ctrl->sched.num_slots) {
			/* Cannot grow the timer table any more. */
			return -1;
		}
		num_slots = ctrl->sched.num_slots * 2;
		if (SCHED_EVENTS_MAX < num_slots) {
			num_slots = SCHED_EVENTS_MAX;
		}
	} else {
		num_slots = SCHED_EVENTS_INITIAL;
	}

	/* Get and initialize the new timer table. */
	timers = calloc(num_slots, sizeof(struct pri_sched));
	if (!timers) {
		/* Could not get a new timer table. */
		return -1;
	}
	if (ctrl->sched.timer) {
		/* Copy over the old timer table. */
		memcpy(timers, ctrl->sched.timer,
			ctrl->sched.num_slots * sizeof(struct pri_sched));
		free(ctrl->sched.timer);
	}

	/* Put the new timer table in place. */
	ctrl->sched.timer = timers;
	ctrl->sched.num_slots = num_slots;
	return 0;
}

/*!
 * \brief Start a timer to schedule an event.
 *
 * \param ctrl D channel controller.
 * \param ms Number of milliseconds to scheduled event.
 * \param function Callback function to call when timeout.
 * \param data Value to give callback function when timeout.
 *
 * \retval 0 if scheduler table is full and could not schedule the event.
 * \retval id Scheduled event id.
 */
int pri_schedule_event(struct pri *ctrl, int ms, void (*function)(void *data), void *data)
{
	unsigned max_used;
	unsigned x;
	struct timeval tv;

	/* Scheduling runs on master channels only */
	ctrl = PRI_MASTER(ctrl);

	max_used = ctrl->sched.max_used;
	for (x = 0; x < max_used; ++x) {
		if (!ctrl->sched.timer[x].callback) {
			break;
		}
	}
	if (x == ctrl->sched.num_slots && pri_schedule_grow(ctrl)) {
		pri_error(ctrl, "No more room in scheduler\n");
		return 0;
	}
	if (ctrl->sched.max_used <= x) {
		ctrl->sched.max_used = x + 1;
	}
	if (x >= maxsched) {
		maxsched = x + 1;
	}
	gettimeofday(&tv, NULL);
	tv.tv_sec += ms / 1000;
	tv.tv_usec += (ms % 1000) * 1000;
	if (tv.tv_usec > 1000000) {
		tv.tv_usec -= 1000000;
		tv.tv_sec += 1;
	}
	ctrl->sched.timer[x].when = tv;
	ctrl->sched.timer[x].callback = function;
	ctrl->sched.timer[x].data = data;
	return x + 1;
}

/*!
 * \brief Determine the time of the next scheduled event to expire.
 *
 * \param ctrl D channel controller.
 *
 * \return Time of the next scheduled event to expire or NULL if no timers active.
 */
struct timeval *pri_schedule_next(struct pri *ctrl)
{
	struct timeval *closest = NULL;
	unsigned x;

	/* Scheduling runs on master channels only */
	ctrl = PRI_MASTER(ctrl);

	/* Scan the scheduled timer slots backwards so we can update the max_used value. */
	for (x = ctrl->sched.max_used; x--;) {
		if (ctrl->sched.timer[x].callback) {
			if (!closest) {
				/* This is the highest sheduled timer slot in use. */
				closest = &ctrl->sched.timer[x].when;
				ctrl->sched.max_used = x + 1;
			} else if ((closest->tv_sec > ctrl->sched.timer[x].when.tv_sec)
				|| ((closest->tv_sec == ctrl->sched.timer[x].when.tv_sec)
					&& (closest->tv_usec > ctrl->sched.timer[x].when.tv_usec))) {
				closest = &ctrl->sched.timer[x].when;
			}
		}
	}
	if (!closest) {
		/* No scheduled timer slots are active. */
		ctrl->sched.max_used = 0;
	}
	return closest;
}

/*!
 * \internal
 * \brief Run all expired timers or return an event generated by an expired timer.
 *
 * \param ctrl D channel controller.
 * \param tv Current time.
 *
 * \return Event for upper layer to process or NULL if all expired timers run.
 */
static pri_event *__pri_schedule_run(struct pri *ctrl, struct timeval *tv)
{
	unsigned x;
	unsigned max_used;
	void (*callback)(void *);
	void *data;

	/* Scheduling runs on master channels only */
	ctrl = PRI_MASTER(ctrl);

	max_used = ctrl->sched.max_used;
	for (x = 0; x < max_used; ++x) {
		if (ctrl->sched.timer[x].callback
			&& ((ctrl->sched.timer[x].when.tv_sec < tv->tv_sec)
			|| ((ctrl->sched.timer[x].when.tv_sec == tv->tv_sec)
			&& (ctrl->sched.timer[x].when.tv_usec <= tv->tv_usec)))) {
			/* This timer has expired. */
			ctrl->schedev = 0;
			callback = ctrl->sched.timer[x].callback;
			data = ctrl->sched.timer[x].data;
			ctrl->sched.timer[x].callback = NULL;
			callback(data);
			if (ctrl->schedev) {
				return &ctrl->ev;
			}
		}
	}
	return NULL;
}

/*!
 * \brief Run all expired timers or return an event generated by an expired timer.
 *
 * \param ctrl D channel controller.
 *
 * \return Event for upper layer to process or NULL if all expired timers run.
 */
pri_event *pri_schedule_run(struct pri *ctrl)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return __pri_schedule_run(ctrl, &tv);
}

/*!
 * \brief Delete a scheduled event.
 *
 * \param ctrl D channel controller.
 * \param id Scheduled event id to delete.
 * 0 is a disabled/unscheduled event id that is ignored.
 * 1 - MAX_SCHED is a valid event id.
 *
 * \return Nothing
 */
void pri_schedule_del(struct pri *ctrl, int id)
{
	/* Scheduling runs on master channels only */
	ctrl = PRI_MASTER(ctrl);

	if (0 < id && id <= ctrl->sched.num_slots) {
		ctrl->sched.timer[id - 1].callback = NULL;
	} else if (id) {
		pri_error(ctrl, "Asked to delete sched id %d??? num_slots=%d\n", id,
			ctrl->sched.num_slots);
	}
}
