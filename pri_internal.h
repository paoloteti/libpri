/*
 * libpri: An implementation of Primary Rate ISDN
 *
 * Written by Mark Spencer <markster@linux-support.net>
 *
 * Copyright (C) 2001, Linux Support Services, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */
 
#ifndef _PRI_INTERNAL_H
#define _PRI_INTERNAL_H

#include <sys/time.h>

struct pri_sched {
	struct timeval when;
	void (*callback)(void *data);
	void *data;
};

struct q921_frame;
enum q931_state;
enum q931_mode;

/* No more than 128 scheduled events */
#define MAX_SCHED 128

struct pri {
	int fd;				/* File descriptor for D-Channel */
	struct pri *subchannel;	/* Sub-channel if appropriate */
	struct pri *master;		/* Master channel if appropriate */
	struct pri_sched pri_sched[MAX_SCHED];	/* Scheduled events */
	int debug;			/* Debug stuff */
	int state;			/* State of D-channel */
	int switchtype;		/* Switch type */
	int nsf;		/* Network-Specific Facility (if any) */
	int localtype;		/* Local network type (unknown, network, cpe) */
	int remotetype;		/* Remote network type (unknown, network, cpe) */

	int sapi;
	int tei;
	int protodisc;
	
	/* Q.921 State */
	int q921_state;	
	int window;			/* Max window size */
	int windowlen;		/* Fullness of window */
	int v_s;			/* Next N(S) for transmission */
	int v_a;			/* Last acknowledged frame */
	int v_r;			/* Next frame expected to be received */
	int v_na;			/* What we've told our peer we've acknowledged */
	int solicitfbit;	/* Have we sent an I or S frame with the F-bit set? */
	int retrans;		/* Retransmissions */
	int sentrej;		/* Are we in reject state */
	
	int cref;			/* Next call reference value */
	
	int busy;			/* Peer is busy */

	/* Various timers */
	int sabme_timer;	/* SABME retransmit */
	int t203_timer;		/* Max idle time */
	int t200_timer;		/* T-200 retransmission timer */
	
	/* Used by scheduler */
	struct timeval tv;
	int schedev;
	pri_event ev;		/* Static event thingy */
	
	/* Q.921 Re-transmission queue */
	struct q921_frame *txqueue;
	
	/* Q.931 calls */
	q931_call **callpool;
	q931_call *localpool;

	/* do we do overlap dialing */
	int overlapdial;

#ifdef LIBPRI_COUNTERS
	/* q921/q931 packet counters */
	unsigned int q921_txcount;
	unsigned int q921_rxcount;
	unsigned int q931_txcount;
	unsigned int q931_rxcount;
#endif
};

struct pri_sr {
	int transmode;
	int channel;
	int exclusive;
	int nonisdn;
	char *caller;
	int callerplan;
	char *callername;
	int callerpres;
	char *called;
	int calledplan;
	int userl1;
	int numcomplete;
};

/* Internal switch types */
#define PRI_SWITCH_GR303_EOC_PATH	10
#define PRI_SWITCH_GR303_TMC_SWITCHING	11

extern int pri_schedule_event(struct pri *pri, int ms, void (*function)(void *data), void *data);

extern pri_event *pri_schedule_run(struct pri *pri);

extern void pri_schedule_del(struct pri *pri, int ev);

extern pri_event *pri_mkerror(struct pri *pri, char *errstr);

extern void pri_message(char *fmt, ...);

extern void pri_error(char *fmt, ...);

#endif
