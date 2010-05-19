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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "compat.h"
#include "libpri.h"
#include "pri_internal.h"
#include "pri_q921.h" 
#include "pri_q931.h"

/*
 * Define RANDOM_DROPS To randomly drop packets in order to simulate loss for testing
 * retransmission functionality
 */

/*
#define RANDOM_DROPS
*/

#define Q921_INIT(pri, hf) do { \
	memset(&(hf),0,sizeof(hf)); \
	(hf).h.sapi = (pri)->sapi; \
	(hf).h.ea1 = 0; \
	(hf).h.ea2 = 1; \
	(hf).h.tei = (pri)->tei; \
} while(0)

static void reschedule_t200(struct pri *pri);
static void q921_dump_pri(struct pri *pri);
static void q921_establish_data_link(struct pri *pri);
static void q921_mdl_error(struct pri *pri, char error);
static void q921_mdl_remove(struct pri *pri);

static void q921_setstate(struct pri *pri, int newstate)
{
	if (pri->debug & PRI_DEBUG_Q921_STATE) {
		if ((pri->q921_state != newstate) && (newstate != 7) && (newstate != 8)) {
			pri_message(pri, "Changing from state %d to %d\n", pri->q921_state, newstate);
		}
	}
	pri->q921_state = newstate;
}

static void q921_discard_retransmissions(struct pri *pri)
{
	struct q921_frame *f, *p;
	f = pri->txqueue;
	while(f) {
		p = f;
		f = f->next;
		/* Free frame */
		free(p);
	}
	pri->txqueue = NULL;
}

static void q921_discard_iqueue(struct pri *pri)
{
	q921_discard_retransmissions(pri);
}

static int q921_transmit(struct pri *pri, q921_h *h, int len) 
{
	int res;

	pri = PRI_MASTER(pri);

#ifdef RANDOM_DROPS
   if (!(random() % 3)) {
         pri_message(pri, " === Dropping Packet ===\n");
         return 0;
   }
#endif   
#ifdef LIBPRI_COUNTERS
	pri->q921_txcount++;      
#endif
	/* Just send it raw */
	if (pri->debug & (PRI_DEBUG_Q921_DUMP | PRI_DEBUG_Q921_RAW))
		q921_dump(pri, h, len, pri->debug & PRI_DEBUG_Q921_RAW, 1);
	/* Write an extra two bytes for the FCS */
	res = pri->write_func ? pri->write_func(pri, h, len + 2) : 0;
	if (res != (len + 2)) {
		pri_error(pri, "Short write: %d/%d (%s)\n", res, len + 2, strerror(errno));
		return -1;
	}
	return 0;
}

static void q921_send_tei(struct pri *pri, int message, int ri, int ai, int iscommand)
{
	q921_u *f;

	if (!(f = calloc(1, sizeof(*f) + 5)))
		return;

	Q921_INIT(pri, *f);
	f->h.c_r = (pri->localtype == PRI_NETWORK) ? iscommand : !iscommand;
	f->ft = Q921_FRAMETYPE_U;
	f->data[0] = 0x0f;	/* Management entity */
	f->data[1] = (ri >> 8) & 0xff;
	f->data[2] = ri & 0xff;
	f->data[3] = message;
	f->data[4] = (ai << 1) | 1;
	if (pri->debug & PRI_DEBUG_Q921_STATE)
		pri_message(pri, "Sending TEI management message %d, TEI=%d\n", message, ai);
	q921_transmit(pri, (q921_h *)f, 8);
	free(f);
}

static void q921_tei_request(void *vpri)
{
	struct pri *pri = (struct pri *)vpri;
	pri->n202_counter++;
	if (pri->n202_counter > pri->timers[PRI_TIMER_N202]) {
		pri_error(pri, "Unable to receive TEI from network!\n");
		pri->n202_counter = 0;
		return;
	}
	pri->ri = random() % 65535;
	q921_send_tei(PRI_MASTER(pri), Q921_TEI_IDENTITY_REQUEST, pri->ri, Q921_TEI_GROUP, 1);
	pri_schedule_del(pri, pri->t202_timer);
	pri->t202_timer = pri_schedule_event(pri, pri->timers[PRI_TIMER_T202], q921_tei_request, pri);
}

static void q921_send_ua(struct pri *pri, int pfbit)
{
	q921_h h;
	Q921_INIT(pri, h);
	h.u.m3 = 3;		/* M3 = 3 */
	h.u.m2 = 0;		/* M2 = 0 */
	h.u.p_f = pfbit;	/* Final bit on */
	h.u.ft = Q921_FRAMETYPE_U;
	switch(pri->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 0;
		break;
	case PRI_CPE:
		h.h.c_r = 1;
		break;
	default:
		pri_error(pri, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return;
	}
	if (pri->debug & (PRI_DEBUG_Q921_STATE | PRI_DEBUG_Q921_DUMP))
		pri_message(pri, "Sending Unnumbered Acknowledgement\n");
	q921_transmit(pri, &h, 3);
}

static void q921_send_sabme(struct pri *pri)
{
	q921_h h;

	Q921_INIT(pri, h);
	h.u.m3 = 3;	/* M3 = 3 */
	h.u.m2 = 3;	/* M2 = 3 */
	h.u.p_f = 1;	/* Poll bit set */
	h.u.ft = Q921_FRAMETYPE_U;
	switch(pri->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 1;
		break;
	case PRI_CPE:
		h.h.c_r = 0;
		break;
	default:
		pri_error(pri, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return;
	}
	q921_transmit(pri, &h, 3);
}

#if 0
static void q921_send_sabme_now(void *vpri)
{
	q921_send_sabme(vpri, 1);
}
#endif

static int q921_ack_packet(struct pri *pri, int num)
{
	struct q921_frame *f, *prev = NULL;
	f = pri->txqueue;
	while (f) {
		if (f->h.n_s == num) {
			/* Cancel each packet as necessary */
			/* That's our packet */
			if (prev)
				prev->next = f->next;
			else
				pri->txqueue = f->next;
			if (pri->debug & PRI_DEBUG_Q921_DUMP)
				pri_message(pri, "-- ACKing packet %d, new txqueue is %d (-1 means empty)\n", f->h.n_s, pri->txqueue ? pri->txqueue->h.n_s : -1);
			/* Update v_a */
			free(f);
			return 1;
		}
		prev = f;
		f = f->next;
	}
	return 0;
}

static void t203_expire(void *);
static void t200_expire(void *);

static void reschedule_t200(struct pri *pri)
{
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "-- Restarting T200 timer\n");
	pri_schedule_del(pri, pri->t200_timer);
	pri->t200_timer = pri_schedule_event(pri, pri->timers[PRI_TIMER_T200], t200_expire, pri);
}

#define restart_t200(pri) reschedule_t200((pri))

#if 0
static void reschedule_t203(struct pri *pri)
{
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "-- Restarting T203 timer\n");
	pri_schedule_del(pri, pri->t203_timer);
	pri->t203_timer = pri_schedule_event(pri, pri->timers[PRI_TIMER_T203], t203_expire, pri);
}
#endif

#if 0
static int q921_unacked_iframes(struct pri *pri)
{
	struct q921_frame *f = pri->txqueue;
	int cnt = 0;

	while(f) {
		if (f->transmitted)
			cnt++;
		f = f->next;
	}

	return cnt;
}
#endif

static void start_t203(struct pri *pri)
{
	if (pri->t203_timer) {
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "T203 requested to start without stopping first\n");
		pri_schedule_del(pri, pri->t203_timer);
	}
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "-- Starting T203 timer\n");
	pri->t203_timer = pri_schedule_event(pri, pri->timers[PRI_TIMER_T203], t203_expire, pri);
}

static void stop_t203(struct pri *pri)
{
	if (pri->t203_timer) {
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "-- Stopping T203 timer\n");
		pri_schedule_del(pri, pri->t203_timer);
		pri->t203_timer = 0;
	} else {
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "-- T203 requested to stop when not started\n");
	}
}

static void start_t200(struct pri *pri)
{
	if (pri->t200_timer) {
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "T200 requested to start without stopping first\n");
		pri_schedule_del(pri, pri->t200_timer);
	}
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "-- Starting T200 timer\n");
	pri->t200_timer = pri_schedule_event(pri, pri->timers[PRI_TIMER_T200], t200_expire, pri);
}

static void stop_t200(struct pri *pri)
{
	if (pri->t200_timer) {
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "-- Stopping T200 timer\n");
		pri_schedule_del(pri, pri->t200_timer);
		pri->t200_timer = 0;
	} else {
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "-- T200 requested to stop when not started\n");
	}
}

/* This is the equivalent of the I-Frame queued up path in Figure B.7 in MULTI_FRAME_ESTABLISHED */
static int q921_send_queued_iframes(struct pri *pri)
{
	struct q921_frame *f;
	int frames_txd = 0;

	if (pri->peer_rx_busy || (pri->v_s == Q921_ADD(pri->v_a, pri->timers[PRI_TIMER_K]))) {
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "Couldn't transmit I frame at this time due to peer busy condition or window shut\n");
		return 0;
	}

	f = pri->txqueue;
	while (f && (pri->v_s != Q921_ADD(pri->v_a, pri->timers[PRI_TIMER_K]))) {
		if (!f->transmitted) {
			/* Send it now... */
			if (pri->debug & PRI_DEBUG_Q921_DUMP)
				pri_message(pri, "-- Finally transmitting %d, since window opened up (%d)\n", f->h.n_s, pri->timers[PRI_TIMER_K]);
			f->transmitted++;
			f->h.n_s = pri->v_s;
			f->h.n_r = pri->v_r;
			f->h.ft = 0;
			f->h.p_f = 0;
			q921_transmit(pri, (q921_h *)(&f->h), f->len);
			Q921_INC(pri->v_s);
			frames_txd++;
			pri->acknowledge_pending = 0;
		}
		f = f->next;
	}

	if (frames_txd) {
		if (!pri->t200_timer) {
			stop_t203(pri);
			start_t200(pri);
		}
	}

	return frames_txd;
}

static void q921_reject(struct pri *pri, int pf)
{
	q921_h h;
	Q921_INIT(pri, h);
	h.s.x0 = 0;	/* Always 0 */
	h.s.ss = 2;	/* Reject */
	h.s.ft = 1;	/* Frametype (01) */
	h.s.n_r = pri->v_r;	/* Where to start retransmission */
	h.s.p_f = pf;	
	switch(pri->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 0;
		break;
	case PRI_CPE:
		h.h.c_r = 1;
		break;
	default:
		pri_error(pri, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return;
	}
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "Sending Reject (%d)\n", pri->v_r);
	q921_transmit(pri, &h, 4);
}

static void q921_rr(struct pri *pri, int pbit, int cmd) {
	q921_h h;
	Q921_INIT(pri, h);
	h.s.x0 = 0;	/* Always 0 */
	h.s.ss = 0; /* Receive Ready */
	h.s.ft = 1;	/* Frametype (01) */
	h.s.n_r = pri->v_r;	/* N/R */
	h.s.p_f = pbit;		/* Poll/Final set appropriately */
	switch(pri->localtype) {
	case PRI_NETWORK:
		if (cmd)
			h.h.c_r = 1;
		else
			h.h.c_r = 0;
		break;
	case PRI_CPE:
		if (cmd)
			h.h.c_r = 0;
		else
			h.h.c_r = 1;
		break;
	default:
		pri_error(pri, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return;
	}
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "Sending Receiver Ready (%d)\n", pri->v_r);
	q921_transmit(pri, &h, 4);
}

static void transmit_enquiry(struct pri *pri)
{
	if (!pri->own_rx_busy) {
		q921_rr(pri, 1, 1);
		pri->acknowledge_pending = 0;
		start_t200(pri);
	} else {
		/* XXX: Implement me... */
	}
}

static void t200_expire(void *vpri)
{
	struct pri *pri = vpri;

	if (pri->debug & PRI_DEBUG_Q921_DUMP) {
		pri_message(pri, "%s\n", __FUNCTION__);
		q921_dump_pri(pri);
	}

	pri->t200_timer = 0;

	switch (pri->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		pri->RC = 0;
		transmit_enquiry(pri);
		pri->RC++;
		q921_setstate(pri, Q921_TIMER_RECOVERY);
		break;
	case Q921_TIMER_RECOVERY:
		/* SDL Flow Figure B.8/Q.921 Page 81 */
		if (pri->RC != pri->timers[PRI_TIMER_N200]) {
#if 0
			if (pri->v_s == pri->v_a) {
				transmit_enquiry(pri);
			}
#else
			/* We are chosing to enquiry by default (to reduce risk of T200 timer errors at the other
			 * side, instead of retransmission of the last I frame we sent */
			transmit_enquiry(pri);
#endif
			pri->RC++;
		} else {
			//pri_error(pri, "MDL-ERROR (I): T200 = N200 in timer recovery state\n");
			q921_mdl_error(pri, 'I');
			q921_establish_data_link(pri);
			pri->l3initiated = 0;
			q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		}
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		if (pri->RC != pri->timers[PRI_TIMER_N200]) {
			pri->RC++;
			q921_send_sabme(pri);
			start_t200(pri);
		} else {
			q921_discard_iqueue(pri);
			//pri_error(pri, "MDL-ERROR (G) : T200 expired N200 times in state %d\n", pri->q921_state);
			q921_mdl_error(pri, 'G');
			q921_setstate(pri, Q921_TEI_ASSIGNED);
			/* DL-RELEASE indication */
			q931_dl_indication(pri, PRI_EVENT_DCHAN_DOWN);
		}
		break;
	default:
		pri_error(pri, "Cannot handle T200 expire in state %d\n", pri->q921_state);
	}

}

/* This is sending a DL-UNIT-DATA request */
int q921_transmit_uiframe(struct pri *pri, void *buf, int len)
{
	uint8_t ubuf[512];
	q921_h *h = (void *)&ubuf[0];

	if (len >= 512) {
		pri_error(pri, "Requested to send UI frame larger than 512 bytes!\n");
		return -1;
	}

	memset(ubuf, 0, sizeof(ubuf));
	h->h.sapi = 0;
	h->h.ea1 = 0;
	h->h.ea2 = 1;
	h->h.tei = pri->tei;
	h->u.m3 = 0;
	h->u.m2 = 0;
	h->u.p_f = 0;	/* Poll bit set */
	h->u.ft = Q921_FRAMETYPE_U;

	switch(pri->localtype) {
	case PRI_NETWORK:
		h->h.c_r = 1;
		break;
	case PRI_CPE:
		h->h.c_r = 0;
		break;
	default:
		pri_error(pri, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return -1;
	}

	memcpy(h->u.data, buf, len);

	q921_transmit(pri, h, len + 3);

	return 0;
}

static struct pri * pri_find_tei(struct pri *vpri, int sapi, int tei)
{
	struct pri *pri;
	for (pri = PRI_MASTER(vpri); pri; pri = pri->subchannel) {
		if (pri->tei == tei && pri->sapi == sapi)
			return pri;
	}

	return NULL;
}

/* This is the equivalent of a DL-DATA request, as well as the I frame queued up outcome */
int q921_transmit_iframe(struct pri *vpri, int tei, void *buf, int len, int cr)
{
	q921_frame *f, *prev=NULL;
	struct pri *pri;

	if (BRI_NT_PTMP(vpri)) {
		if (tei == Q921_TEI_GROUP) {
			pri_error(vpri, "Huh?! For NT-PTMP, we shouldn't be sending I-frames out the group TEI\n");
			return 0;
		}

		pri = pri_find_tei(vpri, Q921_SAPI_CALL_CTRL, tei);
		if (!pri) {
			pri_error(vpri, "Huh?! Unable to locate PRI associated with TEI %d.  Did we have to ditch it due to error conditions?\n", tei);
			return 0;
		}
	} else if (BRI_TE_PTMP(vpri)) {
		/* We don't care what the tei is, since we only support one sub and one TEI */
		pri = PRI_MASTER(vpri)->subchannel;

		if (pri->q921_state == Q921_TEI_UNASSIGNED) {
			q921_tei_request(pri);
			q921_setstate(pri, Q921_ESTABLISH_AWAITING_TEI);
		}
	} else {
		/* Should just be PTP modes, which shouldn't have subs */
		pri = vpri;
	}

	/* Figure B.7/Q.921 Page 70 */
	switch (pri->q921_state) {
	case Q921_TEI_ASSIGNED:
		/* If we aren't in a state compatiable with DL-DATA requests, start getting us there here */
		q921_establish_data_link(pri);
		pri->l3initiated = 1;
		q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		/* For all rest, we've done the work to get us up prior to this and fall through */
	case Q921_TEI_UNASSIGNED:
	case Q921_ESTABLISH_AWAITING_TEI:
	case Q921_ASSIGN_AWAITING_TEI:
	case Q921_TIMER_RECOVERY:
	case Q921_AWAITING_ESTABLISHMENT:
	case Q921_MULTI_FRAME_ESTABLISHED:
		for (f=pri->txqueue; f; f = f->next) prev = f;
		f = calloc(1, sizeof(q921_frame) + len + 2);
		if (f) {
			Q921_INIT(pri, f->h);
			switch(pri->localtype) {
			case PRI_NETWORK:
				if (cr)
					f->h.h.c_r = 1;
				else
					f->h.h.c_r = 0;
			break;
			case PRI_CPE:
				if (cr)
					f->h.h.c_r = 0;
				else
					f->h.h.c_r = 1;
			break;
			}
			f->next = NULL;
			f->transmitted = 0;
			f->len = len + 4;
			memcpy(f->h.data, buf, len);
			if (prev)
				prev->next = f;
			else
				pri->txqueue = f;

			if (pri->q921_state != Q921_MULTI_FRAME_ESTABLISHED) {
				return 0;
			}

			if (pri->peer_rx_busy || (pri->v_s == Q921_ADD(pri->v_a, pri->timers[PRI_TIMER_K]))) {
				if (pri->debug & PRI_DEBUG_Q921_DUMP)
					pri_message(pri, "Couldn't transmit I frame at this time due to peer busy condition or window shut\n");
				return 0;
			}

			q921_send_queued_iframes(pri);

			return 0;
		} else {
			pri_error(pri, "!! Out of memory for Q.921 transmit\n");
			return -1;
		}
	default:
		pri_error(pri, "Cannot transmit frames in state %d\n", pri->q921_state);
	}
	return 0;
}

static void t203_expire(void *vpri)
{
	struct pri *pri = vpri;
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "%s\n", __FUNCTION__);
	pri->t203_timer = 0;
	switch (pri->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		transmit_enquiry(pri);
		pri->RC = 0;
		q921_setstate(pri, Q921_TIMER_RECOVERY);
		break;
	default:
		if (pri->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(pri, "T203 counter expired in weird state %d\n", pri->q921_state);
		pri->t203_timer = 0;
	}
}

static void q921_dump_iqueue_info(struct pri *pri, int force)
{
	struct q921_frame *f;
	int pending = 0, unacked = 0;

	unacked = pending = 0;

	for (f = pri->txqueue; f && f->next; f = f->next) {
		if (f->transmitted) {
			unacked++;
		} else {
			pending++;
		}
	}

	if (force)
		pri_error(pri, "Number of pending packets %d, sent but unacked %d\n", pending, unacked);

	return;
}

static void q921_dump_pri_by_h(struct pri *pri, q921_h *h);

void q921_dump(struct pri *pri, q921_h *h, int len, int showraw, int txrx)
{
	int x;
	char *type;
	char direction_tag;
	
	q921_dump_pri_by_h(pri, h);

	direction_tag = txrx ? '>' : '<';
	if (showraw) {
		char *buf = malloc(len * 3 + 1);
		int buflen = 0;
		if (buf) {
			pri_message(pri, "\n");
			for (x=0;x<len;x++) 
				buflen += sprintf(buf + buflen, "%02x ", h->raw[x]);
			pri_message(pri, "%c [ %s]\n", direction_tag, buf);
			free(buf);
		}
	}

	pri_message(pri, "\n");
	switch (h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		pri_message(pri, "%c Informational frame:\n", direction_tag);
		break;
	case 1:
		pri_message(pri, "%c Supervisory frame:\n", direction_tag);
		break;
	case 3:
		pri_message(pri, "%c Unnumbered frame:\n", direction_tag);
		break;
	}
	
	pri_message(pri, "%c SAPI: %02d  C/R: %d EA: %d\n",
		direction_tag,
		h->h.sapi, 
		h->h.c_r,
		h->h.ea1);
	pri_message(pri, "%c  TEI: %03d        EA: %d\n", 
		direction_tag,
		h->h.tei,
		h->h.ea2);

	switch (h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		/* Informational frame */
		pri_message(pri, "%c N(S): %03d   0: %d\n",
			direction_tag,
			h->i.n_s,
			h->i.ft);
		pri_message(pri, "%c N(R): %03d   P: %d\n",
			direction_tag,
			h->i.n_r,
			h->i.p_f);
		pri_message(pri, "%c %d bytes of data\n",
			direction_tag,
			len - 4);
		break;
	case 1:
		/* Supervisory frame */
		type = "???";
		switch (h->s.ss) {
		case 0:
			type = "RR (receive ready)";
			break;
		case 1:
			type = "RNR (receive not ready)";
			break;
		case 2:
			type = "REJ (reject)";
			break;
		}
		pri_message(pri, "%c Zero: %d     S: %d 01: %d  [ %s ]\n",
			direction_tag,
			h->s.x0,
			h->s.ss,
			h->s.ft,
			type);
		pri_message(pri, "%c N(R): %03d P/F: %d\n",
			direction_tag,
			h->s.n_r,
			h->s.p_f);
		pri_message(pri, "%c %d bytes of data\n",
			direction_tag,
			len - 4);
		break;
	case 3:		
		/* Unnumbered frame */
		type = "???";
		if (h->u.ft == 3) {
			switch (h->u.m3) {
			case 0:
				if (h->u.m2 == 3)
					type = "DM (disconnect mode)";
				else if (h->u.m2 == 0)
					type = "UI (unnumbered information)";
				break;
			case 2:
				if (h->u.m2 == 0)
					type = "DISC (disconnect)";
				break;
			case 3:
			       	if (h->u.m2 == 3)
					type = "SABME (set asynchronous balanced mode extended)";
				else if (h->u.m2 == 0)
					type = "UA (unnumbered acknowledgement)";
				break;
			case 4:
				if (h->u.m2 == 1)
					type = "FRMR (frame reject)";
				break;
			case 5:
				if (h->u.m2 == 3)
					type = "XID (exchange identification note)";
				break;
			}
		}
		pri_message(pri, "%c   M3: %d   P/F: %d M2: %d 11: %d  [ %s ]\n",
			direction_tag,
			h->u.m3,
			h->u.p_f,
			h->u.m2,
			h->u.ft,
			type);
		pri_message(pri, "%c %d bytes of data\n",
			direction_tag,
			len - 3);
		break;
	};

	if ((h->u.ft == 3) && (h->u.m3 == 0) && (h->u.m2 == 0) && (h->u.data[0] == 0x0f)) {
		int ri;
		int tei;

		ri = (h->u.data[1] << 8) | h->u.data[2];
		tei = (h->u.data[4] >> 1);
		/* TEI assignment related */
		switch (h->u.data[3]) {
		case Q921_TEI_IDENTITY_REQUEST:
			type = "TEI Identity Request";
			break;
		case Q921_TEI_IDENTITY_ASSIGNED:
			type = "TEI Identity Assigned";
			break;
		case Q921_TEI_IDENTITY_CHECK_REQUEST:
			type = "TEI Identity Check Request";
			break;
		case Q921_TEI_IDENTITY_REMOVE:
			type = "TEI Identity Remove";
			break;
		case Q921_TEI_IDENTITY_DENIED:
			type = "TEI Identity Denied";
			break;
		case Q921_TEI_IDENTITY_CHECK_RESPONSE:
			type = "TEI Identity Check Response";
			break;
		case Q921_TEI_IDENTITY_VERIFY:
			type = "TEI Identity Verify";
			break;
		default:
			type = "Unknown";
			break;
		}
		pri_message(pri, "%c MDL Message: %s (%d)\n", direction_tag, type, h->u.data[3]);
		pri_message(pri, "%c RI: %d\n", direction_tag, ri);
		pri_message(pri, "%c Ai: %d E:%d\n", direction_tag, (h->u.data[4] >> 1) & 0x7f, h->u.data[4] & 1);
	}
}

static void q921_dump_pri(struct pri *pri)
{
	pri_message(pri, "TEI: %d State %d\n", pri->tei, pri->q921_state);
	pri_message(pri, "V(S) %d V(A) %d V(R) %d\n", pri->v_s, pri->v_a, pri->v_r);
	pri_message(pri, "K %d, RC %d, l3initiated %d, reject_except %d ack_pend %d\n", pri->timers[PRI_TIMER_K], pri->RC, pri->l3initiated, pri->reject_exception, pri->acknowledge_pending);
	pri_message(pri, "T200 %d, N200 %d, T203 %d\n", pri->t200_timer, 3, pri->t203_timer);
}

static void q921_dump_pri_by_h(struct pri *vpri, q921_h *h)
{
	struct pri *pri = NULL;

	if (!vpri) {
		return;
	}
	if (BRI_NT_PTMP(vpri)) {
		pri = pri_find_tei(vpri, h->h.sapi, h->h.tei);
	} else if (BRI_TE_PTMP(vpri)) {
		pri = PRI_MASTER(vpri)->subchannel;
	} else 
		pri = vpri;
	if (pri) {
		q921_dump_pri(pri);
	} else if (!PTMP_MODE(vpri)) {
		pri_error(vpri, "Huh.... no pri found to dump\n");
	}
}

static pri_event *q921_receive_MDL(struct pri *pri, q921_u *h, int len)
{
	int ri;
	struct pri *sub = pri;
	pri_event *res = NULL;
	int tei;

	if (!BRI_NT_PTMP(pri) && !BRI_TE_PTMP(pri)) {
		pri_error(pri, "Received MDL/TEI managemement message, but configured for mode other than PTMP!\n");
		return NULL;
	}

	if (pri->debug & PRI_DEBUG_Q921_STATE)
		pri_message(pri, "Received MDL message\n");
	if (h->data[0] != 0x0f) {
		pri_error(pri, "Received MDL with unsupported management entity %02x\n", h->data[0]);
		return NULL;
	}
	if (!(h->data[4] & 0x01)) {
		pri_error(pri, "Received MDL with multibyte TEI identifier\n");
		return NULL;
	}
	ri = (h->data[1] << 8) | h->data[2];
	tei = (h->data[4] >> 1);

	switch(h->data[3]) {
	case Q921_TEI_IDENTITY_REQUEST:
		if (!BRI_NT_PTMP(pri)) {
			return NULL;
		}

		if (tei != 127) {
			pri_error(pri, "Received TEI identity request with invalid TEI %d\n", tei);
			q921_send_tei(pri, Q921_TEI_IDENTITY_DENIED, ri, tei, 1);
		}
		tei = 64;
		while (sub->subchannel) {
			if (sub->subchannel->tei == tei)
				++tei;
			sub = sub->subchannel;
		}

		if (tei >= Q921_TEI_GROUP) {
			pri_error(pri, "Reached maximum TEI quota, cannot assign new TEI\n");
			return NULL;
		}
		sub->subchannel = __pri_new_tei(-1, pri->localtype, pri->switchtype, pri, NULL, NULL, NULL, tei, 1);
		
		if (!sub->subchannel) {
			pri_error(pri, "Unable to allocate D-channel for new TEI %d\n", tei);
			return NULL;
		}
		q921_setstate(sub->subchannel, Q921_TEI_ASSIGNED);
		if (pri->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(pri, "Allocating new TEI %d\n", tei);
		}
		q921_send_tei(pri, Q921_TEI_IDENTITY_ASSIGNED, ri, tei, 1);
		break;
	case Q921_TEI_IDENTITY_ASSIGNED:
		if (!BRI_TE_PTMP(pri))
			return NULL;

		/* Assuming we're operating on the sub here */
		pri = pri->subchannel;
		
		switch (pri->q921_state) {
		case Q921_ASSIGN_AWAITING_TEI:
		case Q921_ESTABLISH_AWAITING_TEI:
			break;
		default:
			pri_message(pri, "Ignoring unrequested TEI assign message\n");
			return NULL;
		}

		if (ri != pri->ri) {
			pri_message(pri, "TEI assignment received for invalid Ri %02x (our is %02x)\n", ri, pri->ri);
			return NULL;
		}

		pri_schedule_del(pri, pri->t202_timer);
		pri->t202_timer = 0;
		pri->tei = tei;

		switch (pri->q921_state) {
		case Q921_ASSIGN_AWAITING_TEI:
			q921_setstate(pri, Q921_TEI_ASSIGNED);
			pri->ev.gen.e = PRI_EVENT_DCHAN_UP;
			res = &pri->ev;
			break;
		case Q921_ESTABLISH_AWAITING_TEI:
			q921_establish_data_link(pri);
			pri->l3initiated = 1;
			q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
			break;
		default:
			pri_error(pri, "Error 3\n");
			return NULL;
		}

		break;
	case Q921_TEI_IDENTITY_CHECK_REQUEST:
		if (!BRI_TE_PTMP(pri))
			return NULL;

		if (pri->subchannel->q921_state < Q921_TEI_ASSIGNED)
			return NULL;

		/* If it's addressed to the group TEI or to our TEI specifically, we respond */
		if ((tei == Q921_TEI_GROUP) || (tei == pri->subchannel->tei))
			q921_send_tei(pri, Q921_TEI_IDENTITY_CHECK_RESPONSE, random() % 65535, pri->subchannel->tei, 1);

		break;
	case Q921_TEI_IDENTITY_REMOVE:
		if (!BRI_TE_PTMP(pri))
			return NULL;

		if ((tei == Q921_TEI_GROUP) || (tei == pri->subchannel->tei)) {
			q921_setstate(pri->subchannel, Q921_TEI_UNASSIGNED);
			q921_start(pri->subchannel);
		}
	}
	return res;	/* Do we need to return something??? */
}

static int is_command(struct pri *pri, q921_h *h)
{
	int command = 0;
	int c_r = h->s.h.c_r;

	if ((pri->localtype == PRI_NETWORK && c_r == 0) ||
		(pri->localtype == PRI_CPE && c_r == 1))
		command = 1;

	return command;
}

static void q921_clear_exception_conditions(struct pri *pri)
{
	pri->own_rx_busy = 0;
	pri->peer_rx_busy = 0;
	pri->reject_exception = 0;
	pri->acknowledge_pending = 0;
}

static pri_event * q921_sabme_rx(struct pri *pri, q921_h *h)
{
	pri_event *res = NULL;

	switch (pri->q921_state) {
	case Q921_TIMER_RECOVERY:
		/* Timer recovery state handling is same as multiframe established */
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* Send Unnumbered Acknowledgement */
		q921_send_ua(pri, h->u.p_f);
		q921_clear_exception_conditions(pri);
		//pri_error(pri, "MDL-ERROR (F), SABME in state %d\n", pri->q921_state);
		q921_mdl_error(pri, 'F');
		if (pri->v_s != pri->v_a) {
			q921_discard_iqueue(pri);
			/* DL-ESTABLISH indication */
			q931_dl_indication(pri, PRI_EVENT_DCHAN_UP);
		}
		stop_t200(pri);
		start_t203(pri);
		pri->v_s = pri->v_a = pri->v_r = 0;
		q921_setstate(pri, Q921_MULTI_FRAME_ESTABLISHED);
		break;
	case Q921_TEI_ASSIGNED:
		q921_send_ua(pri, h->u.p_f);
		q921_clear_exception_conditions(pri);
		pri->v_s = pri->v_a = pri->v_r = 0;
		/* DL-ESTABLISH indication */
		q931_dl_indication(pri, PRI_EVENT_DCHAN_UP);
		if (PTP_MODE(pri)) {
			pri->ev.gen.e = PRI_EVENT_DCHAN_UP;
			res = &pri->ev;
		}
		start_t203(pri);
		q921_setstate(pri, Q921_MULTI_FRAME_ESTABLISHED);
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		q921_send_ua(pri, h->u.p_f);
		break;
	case Q921_AWAITING_RELEASE:
	default:
		pri_error(pri, "Cannot handle SABME in state %d\n", pri->q921_state);
	}

	return res;
}

static pri_event *q921_disc_rx(struct pri *pri, q921_h *h)
{
	pri_event * res = NULL;

	switch (pri->q921_state) {
	case Q921_AWAITING_RELEASE:
		q921_send_ua(pri, h->u.p_f);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
	case Q921_TIMER_RECOVERY:
		q921_discard_iqueue(pri);
		q921_send_ua(pri, h->u.p_f);
		/* DL-RELEASE Indication */
		q931_dl_indication(pri, PRI_EVENT_DCHAN_DOWN);

		stop_t200(pri);
		if (pri->q921_state == Q921_MULTI_FRAME_ESTABLISHED)
			stop_t203(pri);
		q921_setstate(pri, Q921_TEI_ASSIGNED);
		break;
	default:
		pri_error(pri, "Don't know what to do with DISC in state %d\n", pri->q921_state);
		break;

	}

	return res;
}

static void q921_mdl_remove(struct pri *pri)
{
	switch (pri->q921_state) {
	case Q921_TEI_ASSIGNED:
		/* XXX: deviation! Since we don't have a UI queue, we just discard our I-queue */
		q921_discard_iqueue(pri);
		q921_setstate(pri, Q921_TEI_UNASSIGNED);
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		q921_discard_iqueue(pri);
		/* DL-RELEASE indication */
		q931_dl_indication(pri, PRI_EVENT_DCHAN_DOWN);

		stop_t200(pri);
		q921_setstate(pri, Q921_TEI_UNASSIGNED);
		break;
	case Q921_AWAITING_RELEASE:
		q921_discard_iqueue(pri);
		/* DL-RELEASE confirm */
		stop_t200(pri);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		q921_discard_iqueue(pri);
		/* DL-RELEASE indication */
		q931_dl_indication(pri, PRI_EVENT_DCHAN_DOWN);
		stop_t200(pri);
		stop_t203(pri);
		q921_setstate(pri, Q921_TEI_UNASSIGNED);
		break;
	case Q921_TIMER_RECOVERY:
		q921_discard_iqueue(pri);
		/* DL-RELEASE indication */
		q931_dl_indication(pri, PRI_EVENT_DCHAN_DOWN);
		stop_t200(pri);
		q921_setstate(pri, Q921_TEI_UNASSIGNED);
	default:
		pri_error(pri, "Cannot handle MDL remove when PRI is in state %d\n", pri->q921_state);
		break;
	}

	if (BRI_NT_PTMP(pri) && pri->q921_state == Q921_TEI_UNASSIGNED) {
		if (pri == PRI_MASTER(pri)) {
			pri_error(pri, "Bad bad bad!  Asked to free master\n");
			return;
		}
		pri->mdl_free_me = 1;
	}
}

static int q921_mdl_handle_network_error(struct pri *pri, char error)
{
	int handled = 0;
	switch (error) {
	case 'C':
	case 'D':
	case 'G':
	case 'H':
		q921_mdl_remove(pri);
		handled = 1;
		break;
	case 'A':
	case 'B':
	case 'E':
	case 'F':
	case 'I':
	default:
		pri_error(pri, "Network MDL can't handle error of type %c\n", error);
		break;
	}

	return handled;
}

static int q921_mdl_handle_cpe_error(struct pri *pri, char error)
{
	int handled = 0;

	switch (error) {
	case 'C':
	case 'D':
	case 'G':
	case 'H':
		q921_mdl_remove(pri);
		handled = 1;
		break;
	case 'A':
	case 'B':
	case 'E':
	case 'F':
	case 'I':
		break;
	default:
		pri_error(pri, "CPE MDL can't handle error of type %c\n", error);
		break;
	}

	return handled;
}

static int q921_mdl_handle_ptp_error(struct pri *pri, char error)
{
	int handled = 0;
	/* This is where we act a bit like L3 instead of L2, since we've got an L3 that depends on us
	 * keeping L2 automatically alive and happy for point to point links */
	switch (error) {
	case 'G':
		/* We pick it back up and put it back together for this case */
		q921_discard_iqueue(pri);
		q921_establish_data_link(pri);
		q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		pri->l3initiated = 1;

		pri->schedev = 1;
		pri->ev.gen.e = PRI_EVENT_DCHAN_DOWN;

		handled = 1;
		break;
	default:
		pri_error(pri, "PTP MDL can't handle error of type %c\n", error);
	}

	return handled;
}

static void q921_mdl_handle_error(struct pri *pri, char error, int errored_state)
{
	int handled = 0;
	if (PTP_MODE(pri)) {
		handled = q921_mdl_handle_ptp_error(pri, error);
	} else {
		if (pri->localtype == PRI_NETWORK) {
			handled = q921_mdl_handle_network_error(pri, error);
		} else {
			handled = q921_mdl_handle_cpe_error(pri, error);
		}
	}

	if (handled)
		return;

	switch (error) {
	case 'C':
		pri_error(pri, "MDL-ERROR (C): UA in state %d\n", errored_state);
		break;
	case 'D':
		pri_error(pri, "MDL-ERROR (D): UA in state %d\n", errored_state);
		break;
	case 'A':
		pri_error(pri, "MDL-ERROR (A): Got supervisory frame with p_f bit set to 1 in state %d\n", errored_state);
		break;
	case 'I':
		pri_error(pri, "MDL-ERROR (I): T200 = N200 in timer recovery state %d\n", errored_state);
		break;
	case 'G':
		pri_error(pri, "MDL-ERROR (G) : T200 expired N200 times in state %d\n", errored_state);
		break;
	case 'F':
		pri_error(pri, "MDL-ERROR (F), SABME in state %d\n", errored_state);
		break;
	case 'H':
	case 'B':
	case 'E':
	case 'J':
	default:
		pri_error(pri, "MDL-ERROR (%c) in state %d\n", error, errored_state);
	
	}

	return;
}

static void q921_mdl_handle_error_callback(void *vpri)
{
	struct pri *pri = vpri;

	q921_mdl_handle_error(pri, pri->mdl_error, pri->mdl_error_state);

	pri->mdl_error = 0;
	pri->mdl_timer = 0;

	if (pri->mdl_free_me) {
		struct pri *master = PRI_MASTER(pri);
		struct pri *freep = NULL, *prev, *cur;
		prev = master;
		cur = master->subchannel;

		while (cur) {
			if (cur == pri) {
				prev->subchannel = cur->subchannel;
				freep = cur;
				break;
			}
			prev = cur;
			cur = cur->subchannel;
		}

		if (freep == NULL) {
			pri_error(pri, "Huh!? no match found in list for TEI %d\n", pri->tei);
			return;
		}

		if (pri->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(pri, "Freeing TEI of %d\n", freep->tei);
		}

		__pri_free_tei(freep);
	}

	return;
}

static void q921_mdl_error(struct pri *pri, char error)
{
	if (pri->mdl_error) {
		pri_error(pri, "Trying to queue an MDL error when one is already scheduled\n");
		return;
	}
	pri->mdl_error = error;
	pri->mdl_error_state = pri->q921_state;
	pri->mdl_timer = pri_schedule_event(pri, 0, q921_mdl_handle_error_callback, pri);
}

static pri_event *q921_ua_rx(struct pri *pri, q921_h *h)
{
	pri_event * res = NULL;

	switch (pri->q921_state) {
	case Q921_TEI_ASSIGNED:
		//pri_error(pri, "MDL-ERROR (C, D): UA received in state %d\n", pri->q921_state);
		if (h->u.p_f)
			q921_mdl_error(pri, 'C');
		else
			q921_mdl_error(pri, 'D');
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		if (!h->u.p_f) {
			pri_error(pri, "MDL-ERROR: Received UA with F = 0 while awaiting establishment\n");
			break;
		}

		if (!pri->l3initiated) {
			if (pri->v_s != pri->v_a) {
				q921_discard_iqueue(pri);
				/* return DL-ESTABLISH-INDICATION */
				q931_dl_indication(pri, PRI_EVENT_DCHAN_UP);
			}
		} else {
			/* Might not want this... */
			pri->l3initiated = 0;
			/* return DL-ESTABLISH-CONFIRM */
		}

		if (PTP_MODE(pri)) {
			pri->ev.gen.e = PRI_EVENT_DCHAN_UP;
			res = &pri->ev;
		}

		stop_t200(pri);
		start_t203(pri);

		pri->v_r = pri->v_s = pri->v_a = 0;

		q921_setstate(pri, Q921_MULTI_FRAME_ESTABLISHED);
		break;
	case Q921_AWAITING_RELEASE:
		if (!h->u.p_f) {
			//pri_error(pri, "MDL-ERROR (D): UA in state %d w with P_F bit 0\n", pri->q921_state);
			q921_mdl_error(pri, 'D');
		} else {
			/* return DL-RELEASE-CONFIRM */
			stop_t200(pri);
			q921_setstate(pri, Q921_TEI_ASSIGNED);
		}
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
	case Q921_TIMER_RECOVERY:
		//pri_error(pri, "MDL-ERROR (C, D) UA in state %d\n", pri->q921_state);
		q921_mdl_error(pri, 'C');
		break;
	default:
		pri_error(pri, "Don't know what to do with UA in state %d\n", pri->q921_state);
		break;

	}

	return res;
}

static void q921_enquiry_response(struct pri *pri)
{
	if (pri->own_rx_busy) {
		/* XXX : TODO later sometime */
		pri_error(pri, "Implement me %s: own_rx_busy\n", __FUNCTION__);
		//q921_rnr(pri);
	} else {
		q921_rr(pri, 1, 0);
	}

	pri->acknowledge_pending = 0;
}

static void n_r_error_recovery(struct pri *pri)
{
	q921_mdl_error(pri, 'J');

	q921_establish_data_link(pri);

	pri->l3initiated = 0;
}

static void update_v_a(struct pri *pri, int n_r)
{
	int idealcnt = 0, realcnt = 0;
	int x;

	/* Cancel each packet as necessary */
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "-- ACKing all packets from %d to (but not including) %d\n", pri->v_a, n_r);
	for (x = pri->v_a; x != n_r; Q921_INC(x)) {
		idealcnt++;
		realcnt += q921_ack_packet(pri, x);	
	}
	if (idealcnt != realcnt) {
		pri_error(pri, "Ideally should have ack'd %d frames, but actually ack'd %d.  This is not good.\n", idealcnt, realcnt);
		q921_dump_iqueue_info(pri, 1);
	} else {
		q921_dump_iqueue_info(pri, 0);
	}

	pri->v_a = n_r;
}

static int n_r_is_valid(struct pri *pri, int n_r)
{
	int x;

	for (x=pri->v_a; (x != pri->v_s) && (x != n_r); Q921_INC(x));
	if (x != n_r) {
		pri_error(pri, "N(R) %d not within ack window!  Bad Bad Bad!\n", n_r);
		return 0;
	} else {
		return 1;
	}
}

static int q921_invoke_retransmission(struct pri *pri, int n_r);

static pri_event * timer_recovery_rr_rej_rx(struct pri *pri, q921_h *h)
{
	/* Figure B.7/Q.921 Page 74 */
	pri->peer_rx_busy = 0;

	if (is_command(pri, h)) {
		if (h->s.p_f) {
			/* Enquiry response */
			q921_enquiry_response(pri);
		}
		if (n_r_is_valid(pri, h->s.n_r)) {
			update_v_a(pri, h->s.n_r);
		} else {
			goto n_r_error_out;
		}
	} else {
		if (!h->s.p_f) {
			if (n_r_is_valid(pri, h->s.n_r)) {
				update_v_a(pri, h->s.n_r);
			} else {
				goto n_r_error_out;
			}
		} else {
			if (n_r_is_valid(pri, h->s.n_r)) {
				update_v_a(pri, h->s.n_r);
				stop_t200(pri);
				start_t203(pri);
				q921_invoke_retransmission(pri, h->s.n_r);
				q921_setstate(pri, Q921_MULTI_FRAME_ESTABLISHED);
			} else {
				goto n_r_error_out;
			}
		}
	}
	return NULL;
n_r_error_out:
	n_r_error_recovery(pri);
	q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
	return NULL;
}

static pri_event *q921_rr_rx(struct pri *pri, q921_h *h)
{
	pri_event * res = NULL;
	switch (pri->q921_state) {
	case Q921_TIMER_RECOVERY:
		res = timer_recovery_rr_rej_rx(pri, h);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* Figure B.7/Q.921 Page 74 */
		pri->peer_rx_busy = 0;

		if (is_command(pri, h)) {
			if (h->s.p_f) {
				/* Enquiry response */
				q921_enquiry_response(pri);
			}
		} else {
			if (h->s.p_f) {
				//pri_message(pri, "MDL-ERROR (A): Got RR response with p_f bit set to 1 in state %d\n", pri->q921_state);
				q921_mdl_error(pri, 'A');
			}
		}

		if (!n_r_is_valid(pri, h->s.n_r)) {
			n_r_error_recovery(pri);
			q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		} else {
			if (h->s.n_r == pri->v_s) {
				update_v_a(pri, h->s.n_r);
				stop_t200(pri);
				start_t203(pri);
			} else {
				if (h->s.n_r != pri->v_a) {
					/* Need to check the validity of n_r as well.. */
					update_v_a(pri, h->s.n_r);
					restart_t200(pri);
				}
			}
		}
		break;
	default:
		pri_error(pri, "Don't know what to do with RR in state %d\n", pri->q921_state);
		break;
	}

	return res;
}

/* TODO: Look at this code more... */
static int q921_invoke_retransmission(struct pri *pri, int n_r)
{
	int frames_txd = 0;
	int frames_supposed_to_tx = 0;
	q921_frame *f;
	unsigned int local_v_s = pri->v_s;


	for (f = pri->txqueue; f && (f->h.n_s != n_r); f = f->next);
	while (f) {
		if (f->transmitted) {
 			if (pri->debug & PRI_DEBUG_Q921_STATE)
				pri_error(pri, "!! Got reject for frame %d, retransmitting frame %d now, updating n_r!\n", n_r, f->h.n_s);
			f->h.n_r = pri->v_r;
			f->h.p_f = 0;
			q921_transmit(pri, (q921_h *)(&f->h), f->len);
			frames_txd++;
		}
		f = f->next; 
	} 

	while (local_v_s != n_r) {
		Q921_DEC(local_v_s);
		frames_supposed_to_tx++;
	}

	if (frames_supposed_to_tx != frames_txd) {
		pri_error(pri, "!!!!!!!!!!!! Should have only transmitted %d frames!\n", frames_supposed_to_tx);
	}

	return frames_txd;
}

static pri_event *q921_rej_rx(struct pri *pri, q921_h *h)
{
	pri_event * res = NULL;

	if (pri->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(pri, "!! Got reject for frame %d in state %d\n", h->s.n_r, pri->q921_state);
	}

	switch (pri->q921_state) {
	case Q921_TIMER_RECOVERY:
		res = timer_recovery_rr_rej_rx(pri, h);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* Figure B.7/Q.921 Page 74 */
		pri->peer_rx_busy = 0;

		if (is_command(pri, h)) {
			if (h->s.p_f) {
				/* Enquiry response */
				q921_enquiry_response(pri);
			}
		} else {
			if (h->s.p_f) {
				//pri_message(pri, "MDL-ERROR (A): Got REJ response with p_f bit set to 1 in state %d\n", pri->q921_state);
				q921_mdl_error(pri, 'A');
			}
		}

		if (!n_r_is_valid(pri, h->s.n_r)) {
			n_r_error_recovery(pri);
			q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		} else {
			update_v_a(pri, h->s.n_r);
			stop_t200(pri);
			start_t203(pri);
			q921_invoke_retransmission(pri, h->s.n_r);
		}
		return NULL;
	default:
		pri_error(pri, "Don't know what to do with RR in state %d\n", pri->q921_state);
		return NULL;
	}

	return res;
}

static pri_event *q921_iframe_rx(struct pri *pri, q921_h *h, int len)
{
	pri_event * eres = NULL;
	int res = 0;

	switch (pri->q921_state) {
	case Q921_TIMER_RECOVERY:
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* FIXME: Verify that it's a command ... */
		if (pri->own_rx_busy) {
			/* XXX: Note: There's a difference in th P/F between both states */
			/* DEVIATION: Handle own rx busy */
		}

		if (h->i.n_s == pri->v_r) {
			Q921_INC(pri->v_r);

			pri->reject_exception = 0;

			//res = q931_receive(PRI_MASTER(pri), pri->tei, (q931_h *)h->i.data, len - 4);
			res = q931_receive(pri, pri->tei, (q931_h *)h->i.data, len - 4);
			if (res != -1 && (res & Q931_RES_HAVEEVENT)) {
				eres = &pri->ev;
			}

			if (h->i.p_f) {
				q921_rr(pri, 1, 0);
				pri->acknowledge_pending = 0;
			} else {
				if (!pri->acknowledge_pending) {
					/* XXX: Fix acknowledge_pending */
					pri->acknowledge_pending = 1;
				}
			}

		} else {
			if (pri->reject_exception) {
				if (h->i.p_f) {
					q921_rr(pri, 1, 0);
					pri->acknowledge_pending = 0;
				}
			} else {
				pri->reject_exception = 1;
				q921_reject(pri, h->i.p_f);
				pri->acknowledge_pending = 0;
			}
		}

		if (!n_r_is_valid(pri, h->i.n_r)) {
			n_r_error_recovery(pri);
			q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		} else {
			if (pri->q921_state == Q921_TIMER_RECOVERY) {
				update_v_a(pri, h->i.n_r);
			} else {
				if (pri->peer_rx_busy) {
					update_v_a(pri, h->i.n_r);
				} else {
					if (h->i.n_r == pri->v_s) {
						update_v_a(pri, h->i.n_r);
						stop_t200(pri);
						start_t203(pri);
					} else {
						if (h->i.n_r != pri->v_a) {
							update_v_a(pri, h->i.n_r);
							stop_t200(pri);
							start_t200(pri);
						}
					}
				}
			}
		}

		break;
	default:
		pri_error(pri, "Don't know what to do with an I frame in state %d\n", pri->q921_state);
		break;
	}

	return eres;
}

static pri_event *q921_dm_rx(struct pri *pri, q921_h *h)
{
	pri_event * res = NULL;

	switch (pri->q921_state) {
	case Q921_TEI_ASSIGNED:
		if (h->u.p_f)
			break;
		/* else */
		q921_establish_data_link(pri);
		pri->l3initiated = 1;
		q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		if (!h->u.p_f)
			break;

		q921_discard_iqueue(pri);
		/* DL-RELEASE indication */
		q931_dl_indication(pri, PRI_EVENT_DCHAN_DOWN);
		stop_t200(pri);
		q921_setstate(pri, Q921_TEI_ASSIGNED);
		break;
	case Q921_AWAITING_RELEASE:
		if (!h->u.p_f)
			break;
		/* DL-RELEASE confirm */
		stop_t200(pri);
		q921_setstate(pri, Q921_TEI_ASSIGNED);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		if (h->u.p_f) {
			/* MDL-ERROR (B) indication */
			q921_mdl_error(pri, 'B');
			break;
		}

		q921_mdl_error(pri, 'E');
		q921_establish_data_link(pri);
		pri->l3initiated = 0;
		q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		break;
	case Q921_TIMER_RECOVERY:
		if (h->u.p_f) {
			/* MDL-ERROR (B) indication */
			q921_mdl_error(pri, 'B');
		} else {
			/* MDL-ERROR (E) indication */
			q921_mdl_error(pri, 'E');
		}
		q921_establish_data_link(pri);
		pri->l3initiated = 0;
		q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		break;
	default:
		pri_error(pri, "Don't know what to do with DM frame in state %d\n", pri->q921_state);
		break;
	}

	return res;
}

static pri_event *q921_rnr_rx(struct pri *pri, q921_h *h)
{
	pri_event * res = NULL;

	switch (pri->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		pri->peer_rx_busy = 1;
		if (!is_command(pri, h)) {
			if (h->s.p_f) {
				/* MDL-ERROR (A) indication */
				q921_mdl_error(pri, 'A');
			}
		} else {
			if (h->s.p_f) {
				q921_enquiry_response(pri);
			}
		}

		if (!n_r_is_valid(pri, h->s.n_r)) {
			n_r_error_recovery(pri);
			q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
		} else {
			update_v_a(pri, h->s.n_r);
			stop_t203(pri);
			restart_t200(pri);
		}
		break;
	case Q921_TIMER_RECOVERY:
		pri->peer_rx_busy = 1;
		if (is_command(pri, h)) {
			if (h->s.p_f) {
				q921_enquiry_response(pri);
				if (n_r_is_valid(pri, h->s.n_r)) {
					update_v_a(pri, h->s.n_r);
					break;
				} else {
					n_r_error_recovery(pri);
					q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
					break;
				}
			}
		} else {
			if (h->s.p_f) {
				if (n_r_is_valid(pri, h->s.n_r)) {
					update_v_a(pri, h->s.n_r);
					restart_t200(pri);
					q921_invoke_retransmission(pri, h->s.n_r);
					q921_setstate(pri, Q921_MULTI_FRAME_ESTABLISHED);
					break;
				} else {
					n_r_error_recovery(pri);
					q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
					break;
				}
			}
		}
		break;
	default:
		pri_error(pri, "Don't know what to do with RNR in state %d\n", pri->q921_state);
		break;
	}

	return res;
}

static void q921_acknowledge_pending_check(struct pri *pri)
{
	if (pri->acknowledge_pending) {
		pri->acknowledge_pending = 0;
		q921_rr(pri, 0, 0);
	}
}

static void q921_statemachine_check(struct pri *pri)
{
	switch (pri->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		q921_send_queued_iframes(pri);
		q921_acknowledge_pending_check(pri);
		break;
	case Q921_TIMER_RECOVERY:
		q921_acknowledge_pending_check(pri);
		break;
	default:
		break;
	}
}

static pri_event *__q921_receive_qualified(struct pri *pri, q921_h *h, int len)
{
	pri_event *ev = NULL;

	switch(h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		ev = q921_iframe_rx(pri, h, len);
		break;
	case 1:
		switch(h->s.ss) {
		case 0:
			ev =  q921_rr_rx(pri, h);
			break;
 		case 1:
			ev = q921_rnr_rx(pri, h);
			break;
 		case 2:
 			/* Just retransmit */
 			if (pri->debug & PRI_DEBUG_Q921_STATE)
 				pri_message(pri, "-- Got reject requesting packet %d...  Retransmitting.\n", h->s.n_r);
			ev = q921_rej_rx(pri, h);
			break;
		default:
			pri_error(pri, "!! XXX Unknown Supervisory frame ss=0x%02x,pf=%02xnr=%02x vs=%02x, va=%02x XXX\n", h->s.ss, h->s.p_f, h->s.n_r,
					pri->v_s, pri->v_a);
		}
		break;
	case 3:
		if (len < 3) {
			pri_error(pri, "!! Received short unnumbered frame\n");
			break;
		}
		switch(h->u.m3) {
		case 0:
			if (h->u.m2 == 3) {
				ev = q921_dm_rx(pri, h);
				break;
			} else if (!h->u.m2) {
				if ((pri->sapi == Q921_SAPI_LAYER2_MANAGEMENT) && (pri->tei == Q921_TEI_GROUP)) {

					pri_error(pri, "I should never be called\n");
					q921_receive_MDL(pri, (q921_u *)h, len);

				} else {
					int res;

					res = q931_receive(pri, pri->tei, (q931_h *) h->u.data, len - 3);
					if (res == -1) {
						ev = NULL;
					}
					if (res & Q931_RES_HAVEEVENT)
						ev = &pri->ev;
				}
			}
			break;
		case 2:
			ev = q921_disc_rx(pri, h);
			break;
		case 3:
			if (h->u.m2 == 3) {
				/* SABME */
				if (pri->debug & (PRI_DEBUG_Q921_STATE | PRI_DEBUG_Q921_DUMP)) {
					pri_message(pri, "-- Got SABME from %s peer.\n", h->h.c_r ? "network" : "cpe");
				}
				if (h->h.c_r) {
					pri->remotetype = PRI_NETWORK;
					if (pri->localtype == PRI_NETWORK) {
						/* We can't both be networks */
						ev = pri_mkerror(pri, "We think we're the network, but they think they're the network, too.");
						break;
					}
				} else {
					pri->remotetype = PRI_CPE;
					if (pri->localtype == PRI_CPE) {
						/* We can't both be CPE */
						ev = pri_mkerror(pri, "We think we're the CPE, but they think they're the CPE too.\n");
						break;
					}
				}
				ev = q921_sabme_rx(pri, h);
				break;
			} else if (h->u.m2 == 0) {
				ev = q921_ua_rx(pri, h);
				break;
			} else 
				pri_error(pri, "!! Weird frame received (m3=3, m2 = %d)\n", h->u.m2);
			break;
		case 4:
			pri_error(pri, "!! Frame got rejected!\n");
			break;
		case 5:
			pri_error(pri, "!! XID frames not supported\n");
			break;
		default:
			pri_error(pri, "!! Don't know what to do with M3=%d u-frames\n", h->u.m3);
		}
		break;
				
	}

	q921_statemachine_check(pri);

	return ev;
}

static pri_event *q921_handle_unmatched_frame(struct pri *pri, q921_h *h, int len)
{
	pri = PRI_MASTER(pri);

	if (h->h.tei < 64) {
		pri_error(pri, "Do not support manual TEI range. Discarding\n");
		return NULL;
	}

	if (h->h.sapi != Q921_SAPI_CALL_CTRL) {
		pri_error(pri, "Message with SAPI other than CALL CTRL is discarded\n");
		return NULL;
	}

	/* If we're NT-PTMP, this means an unrecognized TEI that we'll kill */
	if (BRI_NT_PTMP(pri)) {
		if (pri->debug & PRI_DEBUG_Q921_DUMP) {
			pri_message(pri,
				"Could not find candidate subchannel for received frame with SAPI/TEI of %d/%d.\n",
				h->h.sapi, h->h.tei);
			pri_message(pri, "Sending TEI release, in order to re-establish TEI state\n");
		}
	
		/* Q.921 says we should send the remove message twice, in case of link corruption */
		q921_send_tei(pri, Q921_TEI_IDENTITY_REMOVE, 0, h->h.tei, 1);
		q921_send_tei(pri, Q921_TEI_IDENTITY_REMOVE, 0, h->h.tei, 1);
	}

	return NULL;
}

/* This code assumes that the pri structure is the master pri */
static pri_event *__q921_receive(struct pri *pri, q921_h *h, int len)
{
	pri_event *ev = NULL;
	struct pri *tei;
	/* Discard FCS */
	len -= 2;
	
	if (pri->debug & (PRI_DEBUG_Q921_DUMP | PRI_DEBUG_Q921_RAW))
		q921_dump(pri, h, len, pri->debug & PRI_DEBUG_Q921_RAW, 0);

	/* Check some reject conditions -- Start by rejecting improper ea's */
	if (h->h.ea1 || !(h->h.ea2))
		return NULL;

	if ((h->h.sapi == Q921_SAPI_LAYER2_MANAGEMENT)) {
		return q921_receive_MDL(pri, &h->u, len);
	}

	if ((h->h.tei == Q921_TEI_GROUP) && (h->h.sapi != Q921_SAPI_CALL_CTRL)) {
		pri_error(pri, "Do not handle group messages to services other than MDL or CALL CTRL\n");
		return NULL;
	}

	if (BRI_TE_PTMP(pri)) {
		if (((pri->subchannel->q921_state >= Q921_TEI_ASSIGNED) && (h->h.tei == pri->subchannel->tei))
				|| (h->h.tei == Q921_TEI_GROUP)) {
			ev = __q921_receive_qualified(pri->subchannel, h, len);
		}
		/* Only support reception on our single subchannel */
	} else if (BRI_NT_PTMP(pri)) {
		tei = pri_find_tei(pri, h->h.sapi, h->h.tei);

		if (tei)
			ev = __q921_receive_qualified(tei, h, len);
		else
			ev = q921_handle_unmatched_frame(pri, h, len);

	} else if (PTP_MODE(pri) && (h->h.sapi == pri->sapi) && (h->h.tei == pri->tei)) {
		ev = __q921_receive_qualified(pri, h, len);
	} else {
		ev = NULL;
	}

	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(pri, "Handling message for SAPI/TEI=%d/%d\n", h->h.sapi, h->h.tei);

	return ev;
}

pri_event *q921_receive(struct pri *pri, q921_h *h, int len)
{
	pri_event *e;
	e = __q921_receive(pri, h, len);
#ifdef LIBPRI_COUNTERS
	pri->q921_rxcount++;
#endif
	return e;
}

static void q921_establish_data_link(struct pri *pri)
{
	q921_clear_exception_conditions(pri);
	pri->RC = 0;
	stop_t203(pri);
	reschedule_t200(pri);
	q921_send_sabme(pri);
}

static void nt_ptmp_dchannel_up(void *vpri)
{
	struct pri *pri = vpri;

	pri->schedev = 1;
	pri->ev.gen.e = PRI_EVENT_DCHAN_UP;
}

void q921_start(struct pri *pri)
{
	if (PTMP_MODE(pri)) {
		if (TE_MODE(pri)) {
			q921_setstate(pri, Q921_ASSIGN_AWAITING_TEI);
			q921_tei_request(pri);
		} else {
			q921_setstate(pri, Q921_TEI_UNASSIGNED);
			pri_schedule_event(pri, 0, nt_ptmp_dchannel_up, pri);
		}
	} else {
		/* PTP mode, no need for TEI management junk */
		q921_establish_data_link(pri);
		pri->l3initiated = 1;
		q921_setstate(pri, Q921_AWAITING_ESTABLISHMENT);
	}
}

