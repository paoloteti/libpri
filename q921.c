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
//#define RANDOM_DROPS	1

#define Q921_INIT(ctrl, hf) do { \
	memset(&(hf),0,sizeof(hf)); \
	(hf).h.sapi = (ctrl)->sapi; \
	(hf).h.ea1 = 0; \
	(hf).h.ea2 = 1; \
	(hf).h.tei = (ctrl)->tei; \
} while(0)

static void reschedule_t200(struct pri *ctrl);
static void q921_dump_pri(struct pri *ctrl, char direction_tag);
static void q921_establish_data_link(struct pri *ctrl);
static void q921_mdl_error(struct pri *ctrl, char error);
static void q921_mdl_remove(struct pri *ctrl);
static void q921_restart_ptp_link_if_needed(struct pri *ctrl);

/*!
 * \internal
 * \brief Convert Q.921 state to a string.
 *
 * \param state Q.921 state to convert.
 *
 * \return State name string
 */
static const char *q921_state2str(enum q921_state state)
{
	switch (state) {
	case Q921_TEI_UNASSIGNED:
		return "TEI unassigned";
	case Q921_ASSIGN_AWAITING_TEI:
		return "Assign awaiting TEI";
	case Q921_ESTABLISH_AWAITING_TEI:
		return "Establish awaiting TEI";
	case Q921_TEI_ASSIGNED:
		return "TEI assigned";
	case Q921_AWAITING_ESTABLISHMENT:
		return "Awaiting establishment";
	case Q921_AWAITING_RELEASE:
		return "Awaiting release";
	case Q921_MULTI_FRAME_ESTABLISHED:
		return "Multi-frame established";
	case Q921_TIMER_RECOVERY:
		return "Timer recovery";
	}

	return "Unknown state";
}

static void q921_setstate(struct pri *ctrl, int newstate)
{
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		/*
		 * Suppress displaying these state transitions:
		 * Q921_MULTI_FRAME_ESTABLISHED <--> Q921_TIMER_RECOVERY
		 *
		 * Q921 keeps flipping back and forth between these two states
		 * when it has nothing better to do.
		 */
		switch (ctrl->q921_state) {
		case Q921_MULTI_FRAME_ESTABLISHED:
		case Q921_TIMER_RECOVERY:
			switch (newstate) {
			case Q921_MULTI_FRAME_ESTABLISHED:
			case Q921_TIMER_RECOVERY:
				/* Suppress displaying this state transition. */
				ctrl->q921_state = newstate;
				return;
			default:
				break;
			}
			break;
		default:
			break;
		}
		if (ctrl->q921_state != newstate) {
			pri_message(ctrl, "Changing from state %d(%s) to %d(%s)\n",
				ctrl->q921_state, q921_state2str(ctrl->q921_state),
				newstate, q921_state2str(newstate));
		}
	}
	ctrl->q921_state = newstate;
}

static void q921_discard_iqueue(struct pri *ctrl)
{
	struct q921_frame *f, *p;

	f = ctrl->txqueue;
	while (f) {
		p = f;
		f = f->next;
		/* Free frame */
		free(p);
	}
	ctrl->txqueue = NULL;
}

static int q921_transmit(struct pri *ctrl, q921_h *h, int len) 
{
	int res;

	ctrl = PRI_MASTER(ctrl);

#ifdef RANDOM_DROPS
   if (!(random() % 3)) {
         pri_message(ctrl, " === Dropping Packet ===\n");
         return 0;
   }
#endif   
#ifdef LIBPRI_COUNTERS
	ctrl->q921_txcount++;      
#endif
	/* Just send it raw */
	if (ctrl->debug & (PRI_DEBUG_Q921_DUMP | PRI_DEBUG_Q921_RAW))
		q921_dump(ctrl, h, len, ctrl->debug & PRI_DEBUG_Q921_RAW, 1);
	/* Write an extra two bytes for the FCS */
	res = ctrl->write_func ? ctrl->write_func(ctrl, h, len + 2) : 0;
	if (res != (len + 2)) {
		pri_error(ctrl, "Short write: %d/%d (%s)\n", res, len + 2, strerror(errno));
		return -1;
	}
	return 0;
}

static void q921_send_tei(struct pri *ctrl, int message, int ri, int ai, int iscommand)
{
	q921_u *f;

	if (!(f = calloc(1, sizeof(*f) + 5)))
		return;

	Q921_INIT(ctrl, *f);
	f->h.c_r = (ctrl->localtype == PRI_NETWORK) ? iscommand : !iscommand;
	f->ft = Q921_FRAMETYPE_U;
	f->data[0] = 0x0f;	/* Management entity */
	f->data[1] = (ri >> 8) & 0xff;
	f->data[2] = ri & 0xff;
	f->data[3] = message;
	f->data[4] = (ai << 1) | 1;
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "Sending TEI management message %d, TEI=%d\n", message, ai);
	}
	q921_transmit(ctrl, (q921_h *)f, 8);
	free(f);
}

static void t202_expire(void *vpri)
{
	struct pri *ctrl = (struct pri *)vpri;

	/* Start the TEI request timer. */
	pri_schedule_del(ctrl, ctrl->t202_timer);
	ctrl->t202_timer =
		pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T202], t202_expire, ctrl);

	++ctrl->n202_counter;
	if (!ctrl->t202_timer || ctrl->n202_counter > ctrl->timers[PRI_TIMER_N202]) {
		if (!ctrl->t202_timer) {
			pri_error(ctrl, "Could not start T202 timer.");
		} else {
			pri_schedule_del(ctrl, ctrl->t202_timer);
			ctrl->t202_timer = 0;
		}
		pri_error(ctrl, "Unable to receive TEI from network in state %d(%s)!\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		switch (ctrl->q921_state) {
		case Q921_ASSIGN_AWAITING_TEI:
			break;
		case Q921_ESTABLISH_AWAITING_TEI:
			q921_discard_iqueue(ctrl);
			/* DL-RELEASE indication */
			q931_dl_indication(ctrl, PRI_EVENT_DCHAN_DOWN);
			break;
		default:
			break;
		}
		q921_setstate(ctrl, Q921_TEI_UNASSIGNED);
		return;
	}

	/* Send TEI request */
	ctrl->ri = random() % 65535;
	q921_send_tei(PRI_MASTER(ctrl), Q921_TEI_IDENTITY_REQUEST, ctrl->ri, Q921_TEI_GROUP, 1);
}

static void q921_tei_request(struct pri *ctrl)
{
	ctrl->n202_counter = 0;
	t202_expire(ctrl);
}

static void q921_send_dm(struct pri *ctrl, int fbit)
{
	q921_h h;

	Q921_INIT(ctrl, h);
	h.u.m3 = 0;	/* M3 = 0 */
	h.u.m2 = 3;	/* M2 = 3 */
	h.u.p_f = fbit;	/* Final set appropriately */
	h.u.ft = Q921_FRAMETYPE_U;
	switch(ctrl->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 0;
		break;
	case PRI_CPE:
		h.h.c_r = 1;
		break;
	default:
		pri_error(ctrl, "Don't know how to DM on a type %d node\n", ctrl->localtype);
		return;
	}
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Sending DM\n", ctrl->tei);
	}
	q921_transmit(ctrl, &h, 4);
}

static void q921_send_disc(struct pri *ctrl, int pbit)
{
	q921_h h;

	Q921_INIT(ctrl, h);
	h.u.m3 = 2;	/* M3 = 2 */
	h.u.m2 = 0;	/* M2 = 0 */
	h.u.p_f = pbit;	/* Poll set appropriately */
	h.u.ft = Q921_FRAMETYPE_U;
	switch(ctrl->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 0;
		break;
	case PRI_CPE:
		h.h.c_r = 1;
		break;
	default:
		pri_error(ctrl, "Don't know how to DISC on a type %d node\n", ctrl->localtype);
		return;
	}
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Sending DISC\n", ctrl->tei);
	}
	q921_transmit(ctrl, &h, 4);
}

static void q921_send_ua(struct pri *ctrl, int fbit)
{
	q921_h h;
	Q921_INIT(ctrl, h);
	h.u.m3 = 3;		/* M3 = 3 */
	h.u.m2 = 0;		/* M2 = 0 */
	h.u.p_f = fbit;	/* Final set appropriately */
	h.u.ft = Q921_FRAMETYPE_U;
	switch(ctrl->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 0;
		break;
	case PRI_CPE:
		h.h.c_r = 1;
		break;
	default:
		pri_error(ctrl, "Don't know how to UA on a type %d node\n", ctrl->localtype);
		return;
	}
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Sending UA\n", ctrl->tei);
	}
	q921_transmit(ctrl, &h, 3);
}

static void q921_send_sabme(struct pri *ctrl)
{
	q921_h h;

	Q921_INIT(ctrl, h);
	h.u.m3 = 3;	/* M3 = 3 */
	h.u.m2 = 3;	/* M2 = 3 */
	h.u.p_f = 1;	/* Poll bit set */
	h.u.ft = Q921_FRAMETYPE_U;
	switch(ctrl->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 1;
		break;
	case PRI_CPE:
		h.h.c_r = 0;
		break;
	default:
		pri_error(ctrl, "Don't know how to SABME on a type %d node\n", ctrl->localtype);
		return;
	}
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Sending SABME\n", ctrl->tei);
	}
	q921_transmit(ctrl, &h, 3);
}

#if 0
static void q921_send_sabme_now(void *vpri)
{
	q921_send_sabme(vpri, 1);
}
#endif

static int q921_ack_packet(struct pri *ctrl, int num)
{
	struct q921_frame *f;
	struct q921_frame *prev;

	for (prev = NULL, f = ctrl->txqueue; f; prev = f, f = f->next) {
		if (!f->transmitted) {
			break;
		}
		if (f->h.n_s == num) {
			/* Cancel each packet as necessary */
			/* That's our packet */
			if (prev)
				prev->next = f->next;
			else
				ctrl->txqueue = f->next;
			if (ctrl->debug & PRI_DEBUG_Q921_DUMP) {
				pri_message(ctrl,
					"-- ACKing N(S)=%d, txqueue head is N(S)=%d (-1 is empty, -2 is not transmitted)\n",
					f->h.n_s,
					ctrl->txqueue
						? ctrl->txqueue->transmitted
							? ctrl->txqueue->h.n_s
							: -2
						: -1);
			}
			/* Update v_a */
			free(f);
			return 1;
		}
	}
	return 0;
}

static void t203_expire(void *);
static void t200_expire(void *);

static void reschedule_t200(struct pri *ctrl)
{
	if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(ctrl, "-- Restarting T200 timer\n");
	pri_schedule_del(ctrl, ctrl->t200_timer);
	ctrl->t200_timer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T200], t200_expire, ctrl);
}

#define restart_t200(ctrl) reschedule_t200((ctrl))

#if 0
static void reschedule_t203(struct pri *ctrl)
{
	if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(ctrl, "-- Restarting T203 timer\n");
	pri_schedule_del(ctrl, ctrl->t203_timer);
	ctrl->t203_timer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T203], t203_expire, ctrl);
}
#endif

#if 0
static int q921_unacked_iframes(struct pri *ctrl)
{
	struct q921_frame *f = ctrl->txqueue;
	int cnt = 0;

	while(f) {
		if (f->transmitted)
			cnt++;
		f = f->next;
	}

	return cnt;
}
#endif

static void start_t203(struct pri *ctrl)
{
	if (ctrl->t203_timer) {
		if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(ctrl, "T203 requested to start without stopping first\n");
		pri_schedule_del(ctrl, ctrl->t203_timer);
	}
	if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(ctrl, "-- Starting T203 timer\n");
	ctrl->t203_timer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T203], t203_expire, ctrl);
}

static void stop_t203(struct pri *ctrl)
{
	if (ctrl->t203_timer) {
		if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(ctrl, "-- Stopping T203 timer\n");
		pri_schedule_del(ctrl, ctrl->t203_timer);
		ctrl->t203_timer = 0;
	} else {
		if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(ctrl, "-- T203 requested to stop when not started\n");
	}
}

static void start_t200(struct pri *ctrl)
{
	if (ctrl->t200_timer) {
		if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(ctrl, "T200 requested to start without stopping first\n");
		pri_schedule_del(ctrl, ctrl->t200_timer);
	}
	if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(ctrl, "-- Starting T200 timer\n");
	ctrl->t200_timer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T200], t200_expire, ctrl);
}

static void stop_t200(struct pri *ctrl)
{
	if (ctrl->t200_timer) {
		if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(ctrl, "-- Stopping T200 timer\n");
		pri_schedule_del(ctrl, ctrl->t200_timer);
		ctrl->t200_timer = 0;
	} else {
		if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
			pri_message(ctrl, "-- T200 requested to stop when not started\n");
	}
}

/* This is the equivalent of the I-Frame queued up path in Figure B.7 in MULTI_FRAME_ESTABLISHED */
static int q921_send_queued_iframes(struct pri *ctrl)
{
	struct q921_frame *f;
	int frames_txd = 0;

	for (f = ctrl->txqueue; f; f = f->next) {
		if (!f->transmitted) {
			/* This frame has not been sent yet. */
			break;
		}
	}
	if (!f) {
		/* The Tx queue has no pending frames. */
		return 0;
	}

	if (ctrl->peer_rx_busy
		|| (ctrl->v_s == Q921_ADD(ctrl->v_a, ctrl->timers[PRI_TIMER_K]))) {
		/* Don't flood debug trace if not really looking at Q.921 layer. */
		if (ctrl->debug & (/* PRI_DEBUG_Q921_STATE | */ PRI_DEBUG_Q921_DUMP)) {
			pri_message(ctrl,
				"TEI=%d Couldn't transmit I-frame at this time due to peer busy condition or window shut\n",
				ctrl->tei);
		}
		return 0;
	}

	/* Send all pending frames that fit in the window. */
	for (; f; f = f->next) {
		if (ctrl->v_s == Q921_ADD(ctrl->v_a, ctrl->timers[PRI_TIMER_K])) {
			/* The window is no longer open. */
			break;
		}

		/* Send it now... */
		++f->transmitted;

		/*
		 * Send the frame out on the assigned TEI.
		 * Done now because the frame may have been queued before we
		 * had an assigned TEI.
		 */
		f->h.h.tei = ctrl->tei;

		f->h.n_s = ctrl->v_s;
		f->h.n_r = ctrl->v_r;
		f->h.ft = 0;
		f->h.p_f = 0;
		if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(ctrl,
				"TEI=%d Transmitting N(S)=%d, window is open V(A)=%d K=%d\n",
				ctrl->tei, f->h.n_s, ctrl->v_a, ctrl->timers[PRI_TIMER_K]);
		}
		q921_transmit(ctrl, (q921_h *)(&f->h), f->len);
		Q921_INC(ctrl->v_s);
		++frames_txd;
	}

	if (frames_txd) {
		ctrl->acknowledge_pending = 0;
		if (!ctrl->t200_timer) {
			stop_t203(ctrl);
			start_t200(ctrl);
		}
	}

	return frames_txd;
}

static void q921_reject(struct pri *ctrl, int pf)
{
	q921_h h;

	Q921_INIT(ctrl, h);
	h.s.x0 = 0;	/* Always 0 */
	h.s.ss = 2;	/* Reject */
	h.s.ft = 1;	/* Frametype (01) */
	h.s.n_r = ctrl->v_r;	/* Where to start retransmission */
	h.s.p_f = pf;	
	switch(ctrl->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 0;
		break;
	case PRI_CPE:
		h.h.c_r = 1;
		break;
	default:
		pri_error(ctrl, "Don't know how to REJ on a type %d node\n", ctrl->localtype);
		return;
	}
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Sending REJ N(R)=%d\n", ctrl->tei, ctrl->v_r);
	}
	q921_transmit(ctrl, &h, 4);
}

static void q921_rr(struct pri *ctrl, int pbit, int cmd)
{
	q921_h h;

	Q921_INIT(ctrl, h);
	h.s.x0 = 0;	/* Always 0 */
	h.s.ss = 0; /* Receive Ready */
	h.s.ft = 1;	/* Frametype (01) */
	h.s.n_r = ctrl->v_r;	/* N/R */
	h.s.p_f = pbit;		/* Poll/Final set appropriately */
	switch(ctrl->localtype) {
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
		pri_error(ctrl, "Don't know how to RR on a type %d node\n", ctrl->localtype);
		return;
	}
#if 0	/* Don't flood debug trace with RR if not really looking at Q.921 layer. */
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Sending RR N(R)=%d\n", ctrl->tei, ctrl->v_r);
	}
#endif
	q921_transmit(ctrl, &h, 4);
}

static void transmit_enquiry(struct pri *ctrl)
{
	if (!ctrl->own_rx_busy) {
		q921_rr(ctrl, 1, 1);
		ctrl->acknowledge_pending = 0;
		start_t200(ctrl);
	} else {
		/* XXX: Implement me... */
	}
}

static void t200_expire(void *vpri)
{
	struct pri *ctrl = vpri;

	if (ctrl->debug & PRI_DEBUG_Q921_DUMP) {
		pri_message(ctrl, "%s\n", __FUNCTION__);
		q921_dump_pri(ctrl, ' ');
	}

	ctrl->t200_timer = 0;

	switch (ctrl->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		ctrl->RC = 0;
		transmit_enquiry(ctrl);
		ctrl->RC++;
		q921_setstate(ctrl, Q921_TIMER_RECOVERY);
		break;
	case Q921_TIMER_RECOVERY:
		/* SDL Flow Figure B.8/Q.921 Page 81 */
		if (ctrl->RC != ctrl->timers[PRI_TIMER_N200]) {
#if 0
			if (ctrl->v_s == ctrl->v_a) {
				transmit_enquiry(ctrl);
			}
#else
			/* We are chosing to enquiry by default (to reduce risk of T200 timer errors at the other
			 * side, instead of retransmission of the last I-frame we sent */
			transmit_enquiry(ctrl);
#endif
			ctrl->RC++;
		} else {
			//pri_error(ctrl, "MDL-ERROR (I): T200 = N200 in timer recovery state\n");
			q921_mdl_error(ctrl, 'I');
			q921_establish_data_link(ctrl);
			ctrl->l3initiated = 0;
			q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		}
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		if (ctrl->RC != ctrl->timers[PRI_TIMER_N200]) {
			ctrl->RC++;
			q921_send_sabme(ctrl);
			start_t200(ctrl);
		} else {
			q921_discard_iqueue(ctrl);
			//pri_error(ctrl, "MDL-ERROR (G) : T200 expired N200 times in state %d(%s)\n",
			//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
			q921_mdl_error(ctrl, 'G');
			q921_setstate(ctrl, Q921_TEI_ASSIGNED);
			/* DL-RELEASE indication */
			q931_dl_indication(ctrl, PRI_EVENT_DCHAN_DOWN);
		}
		break;
	case Q921_AWAITING_RELEASE:
		if (ctrl->RC != ctrl->timers[PRI_TIMER_N200]) {
			++ctrl->RC;
			q921_send_disc(ctrl, 1);
			start_t200(ctrl);
		} else {
			//pri_error(ctrl, "MDL-ERROR (H) : T200 expired N200 times in state %d(%s)\n",
			//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
			q921_mdl_error(ctrl, 'H');
			/* DL-RELEASE confirm */
			q921_setstate(ctrl, Q921_TEI_ASSIGNED);
		}
		break;
	default:
		/* Looks like someone forgot to stop the T200 timer. */
		pri_error(ctrl, "T200 expired in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

}

/* This is sending a DL-UNIT-DATA request */
int q921_transmit_uiframe(struct pri *ctrl, void *buf, int len)
{
	uint8_t ubuf[512];
	q921_h *h = (void *)&ubuf[0];

	if (len >= 512) {
		pri_error(ctrl, "Requested to send UI-frame larger than 512 bytes!\n");
		return -1;
	}

	memset(ubuf, 0, sizeof(ubuf));
	h->h.sapi = 0;
	h->h.ea1 = 0;
	h->h.ea2 = 1;
	h->h.tei = ctrl->tei;
	h->u.m3 = 0;
	h->u.m2 = 0;
	h->u.p_f = 0;	/* Poll bit set */
	h->u.ft = Q921_FRAMETYPE_U;

	switch(ctrl->localtype) {
	case PRI_NETWORK:
		h->h.c_r = 1;
		break;
	case PRI_CPE:
		h->h.c_r = 0;
		break;
	default:
		pri_error(ctrl, "Don't know how to UI-frame on a type %d node\n", ctrl->localtype);
		return -1;
	}

	memcpy(h->u.data, buf, len);

	q921_transmit(ctrl, h, len + 3);

	return 0;
}

static struct pri * pri_find_tei(struct pri *vpri, int sapi, int tei)
{
	struct pri *ctrl;
	for (ctrl = PRI_MASTER(vpri); ctrl; ctrl = ctrl->subchannel) {
		if (ctrl->tei == tei && ctrl->sapi == sapi)
			return ctrl;
	}

	return NULL;
}

/* This is the equivalent of a DL-DATA request, as well as the I-frame queued up outcome */
int q921_transmit_iframe(struct pri *vpri, int tei, void *buf, int len, int cr)
{
	q921_frame *f, *prev=NULL;
	struct pri *ctrl;

	if (BRI_NT_PTMP(vpri)) {
		if (tei == Q921_TEI_GROUP) {
			pri_error(vpri, "Huh?! For NT-PTMP, we shouldn't be sending I-frames out the group TEI\n");
			return 0;
		}

		ctrl = pri_find_tei(vpri, Q921_SAPI_CALL_CTRL, tei);
		if (!ctrl) {
			pri_error(vpri, "Huh?! Unable to locate PRI associated with TEI %d.  Did we have to ditch it due to error conditions?\n", tei);
			return 0;
		}
	} else if (BRI_TE_PTMP(vpri)) {
		/* We don't care what the tei is, since we only support one sub and one TEI */
		ctrl = PRI_MASTER(vpri)->subchannel;

		switch (ctrl->q921_state) {
		case Q921_TEI_UNASSIGNED:
			q921_setstate(ctrl, Q921_ESTABLISH_AWAITING_TEI);
			q921_tei_request(ctrl);
			break;
		case Q921_ASSIGN_AWAITING_TEI:
			q921_setstate(ctrl, Q921_ESTABLISH_AWAITING_TEI);
			break;
		default:
			break;
		}
	} else {
		/* Should just be PTP modes, which shouldn't have subs */
		ctrl = vpri;
	}

	/* Figure B.7/Q.921 Page 70 */
	switch (ctrl->q921_state) {
	case Q921_TEI_ASSIGNED:
		/* If we aren't in a state compatiable with DL-DATA requests, start getting us there here */
		q921_establish_data_link(ctrl);
		ctrl->l3initiated = 1;
		q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		/* For all rest, we've done the work to get us up prior to this and fall through */
	case Q921_ESTABLISH_AWAITING_TEI:
	case Q921_TIMER_RECOVERY:
	case Q921_AWAITING_ESTABLISHMENT:
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* Find queue tail. */
		for (f = ctrl->txqueue; f; f = f->next) {
			prev = f;
		}

		f = calloc(1, sizeof(q921_frame) + len + 2);
		if (f) {
			Q921_INIT(ctrl, f->h);
			switch(ctrl->localtype) {
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

			/* Put new frame on queue tail. */
			f->next = NULL;
			f->transmitted = 0;
			f->len = len + 4;
			memcpy(f->h.data, buf, len);
			if (prev)
				prev->next = f;
			else
				ctrl->txqueue = f;

			if (ctrl->q921_state != Q921_MULTI_FRAME_ESTABLISHED) {
				if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
					pri_message(ctrl,
						"TEI=%d Just queued I-frame since in state %d(%s)\n",
						ctrl->tei,
						ctrl->q921_state, q921_state2str(ctrl->q921_state));
				}
				return 0;
			}

			if (ctrl->peer_rx_busy || (ctrl->v_s == Q921_ADD(ctrl->v_a, ctrl->timers[PRI_TIMER_K]))) {
				if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
					pri_message(ctrl,
						"TEI=%d Just queued I-frame due to peer busy condition or window shut\n",
						ctrl->tei);
				}
				return 0;
			}

			q921_send_queued_iframes(ctrl);

			return 0;
		} else {
			pri_error(ctrl, "!! Out of memory for Q.921 transmit\n");
			return -1;
		}
	case Q921_TEI_UNASSIGNED:
	case Q921_ASSIGN_AWAITING_TEI:
	case Q921_AWAITING_RELEASE:
	default:
		pri_error(ctrl, "Cannot transmit frames in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}
	return 0;
}

static void t203_expire(void *vpri)
{
	struct pri *ctrl = vpri;

	if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(ctrl, "%s\n", __FUNCTION__);

	ctrl->t203_timer = 0;

	switch (ctrl->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		transmit_enquiry(ctrl);
		ctrl->RC = 0;
		q921_setstate(ctrl, Q921_TIMER_RECOVERY);
		break;
	default:
		/* Looks like someone forgot to stop the T203 timer. */
		pri_error(ctrl, "T203 expired in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}
}

static void q921_dump_iqueue_info(struct pri *ctrl)
{
	struct q921_frame *f;
	int pending = 0, unacked = 0;

	unacked = pending = 0;

	for (f = ctrl->txqueue; f; f = f->next) {
		if (f->transmitted) {
			unacked++;
		} else {
			pending++;
		}
	}

	pri_error(ctrl, "Number of pending packets %d, sent but unacked %d\n", pending, unacked);
}

static void q921_dump_pri_by_h(struct pri *ctrl, char direction_tag, q921_h *h);

void q921_dump(struct pri *ctrl, q921_h *h, int len, int showraw, int txrx)
{
	int x;
	char *type;
	char direction_tag;
	
	direction_tag = txrx ? '>' : '<';

	pri_message(ctrl, "\n");
	q921_dump_pri_by_h(ctrl, direction_tag, h);

	if (showraw) {
		char *buf = malloc(len * 3 + 1);
		int buflen = 0;
		if (buf) {
			for (x=0;x<len;x++) 
				buflen += sprintf(buf + buflen, "%02x ", h->raw[x]);
			pri_message(ctrl, "%c [ %s]\n", direction_tag, buf);
			free(buf);
		}
	}

	switch (h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		pri_message(ctrl, "%c Informational frame:\n", direction_tag);
		break;
	case 1:
		pri_message(ctrl, "%c Supervisory frame:\n", direction_tag);
		break;
	case 3:
		pri_message(ctrl, "%c Unnumbered frame:\n", direction_tag);
		break;
	}
	
	pri_message(ctrl, "%c SAPI: %02d  C/R: %d EA: %d\n",
		direction_tag,
		h->h.sapi, 
		h->h.c_r,
		h->h.ea1);
	pri_message(ctrl, "%c  TEI: %03d        EA: %d\n", 
		direction_tag,
		h->h.tei,
		h->h.ea2);

	switch (h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		/* Informational frame */
		pri_message(ctrl, "%c N(S): %03d   0: %d\n",
			direction_tag,
			h->i.n_s,
			h->i.ft);
		pri_message(ctrl, "%c N(R): %03d   P: %d\n",
			direction_tag,
			h->i.n_r,
			h->i.p_f);
		pri_message(ctrl, "%c %d bytes of data\n",
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
		pri_message(ctrl, "%c Zero: %d     S: %d 01: %d  [ %s ]\n",
			direction_tag,
			h->s.x0,
			h->s.ss,
			h->s.ft,
			type);
		pri_message(ctrl, "%c N(R): %03d P/F: %d\n",
			direction_tag,
			h->s.n_r,
			h->s.p_f);
		pri_message(ctrl, "%c %d bytes of data\n",
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
		pri_message(ctrl, "%c   M3: %d   P/F: %d M2: %d 11: %d  [ %s ]\n",
			direction_tag,
			h->u.m3,
			h->u.p_f,
			h->u.m2,
			h->u.ft,
			type);
		pri_message(ctrl, "%c %d bytes of data\n",
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
		pri_message(ctrl, "%c MDL Message: %s (%d)\n", direction_tag, type, h->u.data[3]);
		pri_message(ctrl, "%c RI: %d\n", direction_tag, ri);
		pri_message(ctrl, "%c Ai: %d E:%d\n", direction_tag, (h->u.data[4] >> 1) & 0x7f, h->u.data[4] & 1);
	}
}

static void q921_dump_pri(struct pri *ctrl, char direction_tag)
{
	pri_message(ctrl, "%c TEI: %d State %d(%s)\n",
		direction_tag, ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
	pri_message(ctrl, "%c V(A)=%d, V(S)=%d, V(R)=%d\n",
		direction_tag, ctrl->v_a, ctrl->v_s, ctrl->v_r);
	pri_message(ctrl, "%c K=%d, RC=%d, l3initiated=%d, reject_except=%d, ack_pend=%d\n",
		direction_tag, ctrl->timers[PRI_TIMER_K], ctrl->RC, ctrl->l3initiated,
		ctrl->reject_exception, ctrl->acknowledge_pending);
	pri_message(ctrl, "%c T200_id=%d, N200=%d, T203_id=%d\n",
		direction_tag, ctrl->t200_timer, ctrl->timers[PRI_TIMER_N200], ctrl->t203_timer);
}

static void q921_dump_pri_by_h(struct pri *vpri, char direction_tag, q921_h *h)
{
	struct pri *ctrl = NULL;

	if (!vpri) {
		return;
	}
	if (BRI_NT_PTMP(vpri)) {
		ctrl = pri_find_tei(vpri, h->h.sapi, h->h.tei);
	} else if (BRI_TE_PTMP(vpri)) {
		ctrl = PRI_MASTER(vpri)->subchannel;
	} else 
		ctrl = vpri;
	if (ctrl) {
		q921_dump_pri(ctrl, direction_tag);
	} else if (!PTMP_MODE(vpri)) {
		pri_error(vpri, "Huh.... no pri found to dump\n");
	}
}

static pri_event *q921_receive_MDL(struct pri *ctrl, q921_u *h, int len)
{
	int ri;
	struct pri *sub;
	pri_event *res = NULL;
	int tei;

	if (!BRI_NT_PTMP(ctrl) && !BRI_TE_PTMP(ctrl)) {
		return pri_mkerror(ctrl,
			"Received MDL/TEI managemement message, but configured for mode other than PTMP!\n");
	}

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "Received MDL message\n");
	}
	if (h->data[0] != 0x0f) {
		pri_error(ctrl, "Received MDL with unsupported management entity %02x\n", h->data[0]);
		return NULL;
	}
	if (!(h->data[4] & 0x01)) {
		pri_error(ctrl, "Received MDL with multibyte TEI identifier\n");
		return NULL;
	}
	ri = (h->data[1] << 8) | h->data[2];
	tei = (h->data[4] >> 1);

	switch(h->data[3]) {
	case Q921_TEI_IDENTITY_REQUEST:
		if (!BRI_NT_PTMP(ctrl)) {
			return NULL;
		}

		if (tei != Q921_TEI_GROUP) {
			pri_error(ctrl, "Received TEI identity request with invalid TEI %d\n", tei);
			q921_send_tei(ctrl, Q921_TEI_IDENTITY_DENIED, ri, tei, 1);
			return NULL;
		}

		/* Find a TEI that is not allocated. */
		tei = 64;
		do {
			for (sub = ctrl; sub->subchannel; sub = sub->subchannel) {
				if (sub->subchannel->tei == tei) {
					/* This TEI is already assigned, try next one. */
					++tei;
					if (tei < Q921_TEI_GROUP) {
						break;
					}
					/* XXX : TODO later sometime: Implement the TEI check procedure to reclaim some dead TEIs. */
					pri_error(ctrl, "Reached maximum TEI quota, cannot assign new TEI\n");
					return NULL;
				}
			}
		} while (sub->subchannel);

		if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(ctrl, "Allocating new TEI %d\n", tei);
		}
		sub->subchannel = __pri_new_tei(-1, ctrl->localtype, ctrl->switchtype, ctrl, NULL, NULL, NULL, tei, 1);
		if (!sub->subchannel) {
			pri_error(ctrl, "Unable to allocate D-channel for new TEI %d\n", tei);
			return NULL;
		}
		q921_setstate(sub->subchannel, Q921_TEI_ASSIGNED);
		q921_send_tei(ctrl, Q921_TEI_IDENTITY_ASSIGNED, ri, tei, 1);
		break;
	case Q921_TEI_IDENTITY_ASSIGNED:
		if (!BRI_TE_PTMP(ctrl))
			return NULL;

		/* Assuming we're operating on the sub here */
		ctrl = ctrl->subchannel;
		
		switch (ctrl->q921_state) {
		case Q921_ASSIGN_AWAITING_TEI:
		case Q921_ESTABLISH_AWAITING_TEI:
			break;
		default:
			pri_message(ctrl, "Ignoring unrequested TEI assign message\n");
			return NULL;
		}

		if (ri != ctrl->ri) {
			pri_message(ctrl, "TEI assignment received for invalid Ri %02x (our is %02x)\n", ri, ctrl->ri);
			return NULL;
		}

		pri_schedule_del(ctrl, ctrl->t202_timer);
		ctrl->t202_timer = 0;

		ctrl->tei = tei;
		if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(ctrl, "Got assigned TEI %d\n", tei);
		}

		switch (ctrl->q921_state) {
		case Q921_ASSIGN_AWAITING_TEI:
			q921_setstate(ctrl, Q921_TEI_ASSIGNED);
			ctrl->ev.gen.e = PRI_EVENT_DCHAN_UP;
			res = &ctrl->ev;
			break;
		case Q921_ESTABLISH_AWAITING_TEI:
			q921_establish_data_link(ctrl);
			ctrl->l3initiated = 1;
			q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
			ctrl->ev.gen.e = PRI_EVENT_DCHAN_UP;
			res = &ctrl->ev;
			break;
		default:
			break;
		}
		break;
	case Q921_TEI_IDENTITY_CHECK_REQUEST:
		if (!BRI_TE_PTMP(ctrl))
			return NULL;

		if (ctrl->subchannel->q921_state < Q921_TEI_ASSIGNED) {
			/* We do not have a TEI. */
			return NULL;
		}

		/* If it's addressed to the group TEI or to our TEI specifically, we respond */
		if ((tei == Q921_TEI_GROUP) || (tei == ctrl->subchannel->tei)) {
			q921_send_tei(ctrl, Q921_TEI_IDENTITY_CHECK_RESPONSE, random() % 65535, ctrl->subchannel->tei, 1);
		}
		break;
	case Q921_TEI_IDENTITY_REMOVE:
		if (!BRI_TE_PTMP(ctrl))
			return NULL;

		if (ctrl->subchannel->q921_state < Q921_TEI_ASSIGNED) {
			/* We do not have a TEI. */
			return NULL;
		}

		/* If it's addressed to the group TEI or to our TEI specifically, we respond */
		if ((tei == Q921_TEI_GROUP) || (tei == ctrl->subchannel->tei)) {
			q921_mdl_remove(ctrl->subchannel);
			q921_start(ctrl->subchannel);
		}
		break;
	}
	return res;	/* Do we need to return something??? */
}

static int is_command(struct pri *ctrl, q921_h *h)
{
	int command = 0;
	int c_r = h->s.h.c_r;

	if ((ctrl->localtype == PRI_NETWORK && c_r == 0) ||
		(ctrl->localtype == PRI_CPE && c_r == 1))
		command = 1;

	return command;
}

static void q921_clear_exception_conditions(struct pri *ctrl)
{
	ctrl->own_rx_busy = 0;
	ctrl->peer_rx_busy = 0;
	ctrl->reject_exception = 0;
	ctrl->acknowledge_pending = 0;
}

static pri_event * q921_sabme_rx(struct pri *ctrl, q921_h *h)
{
	pri_event *res = NULL;

	switch (ctrl->q921_state) {
	case Q921_TIMER_RECOVERY:
		/* Timer recovery state handling is same as multiframe established */
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* Send Unnumbered Acknowledgement */
		q921_send_ua(ctrl, h->u.p_f);
		q921_clear_exception_conditions(ctrl);
		//pri_error(ctrl, "MDL-ERROR (F), SABME in state %d(%s)\n",
		//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
		q921_mdl_error(ctrl, 'F');
		if (ctrl->v_s != ctrl->v_a) {
			q921_discard_iqueue(ctrl);
			/* DL-ESTABLISH indication */
			q931_dl_indication(ctrl, PRI_EVENT_DCHAN_UP);
		}
		stop_t200(ctrl);
		start_t203(ctrl);
		ctrl->v_s = ctrl->v_a = ctrl->v_r = 0;
		q921_setstate(ctrl, Q921_MULTI_FRAME_ESTABLISHED);
		break;
	case Q921_TEI_ASSIGNED:
		q921_send_ua(ctrl, h->u.p_f);
		q921_clear_exception_conditions(ctrl);
		ctrl->v_s = ctrl->v_a = ctrl->v_r = 0;
		/* DL-ESTABLISH indication */
		q931_dl_indication(ctrl, PRI_EVENT_DCHAN_UP);
		if (PTP_MODE(ctrl)) {
			ctrl->ev.gen.e = PRI_EVENT_DCHAN_UP;
			res = &ctrl->ev;
		}
		start_t203(ctrl);
		q921_setstate(ctrl, Q921_MULTI_FRAME_ESTABLISHED);
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		q921_send_ua(ctrl, h->u.p_f);
		break;
	case Q921_AWAITING_RELEASE:
		q921_send_dm(ctrl, h->u.p_f);
		break;
	default:
		pri_error(ctrl, "Cannot handle SABME in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static pri_event *q921_disc_rx(struct pri *ctrl, q921_h *h)
{
	pri_event * res = NULL;

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Got DISC\n", ctrl->tei);
	}

	switch (ctrl->q921_state) {
	case Q921_TEI_ASSIGNED:
	case Q921_AWAITING_ESTABLISHMENT:
		q921_send_dm(ctrl, h->u.p_f);
		break;
	case Q921_AWAITING_RELEASE:
		q921_send_ua(ctrl, h->u.p_f);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
	case Q921_TIMER_RECOVERY:
		q921_discard_iqueue(ctrl);
		q921_send_ua(ctrl, h->u.p_f);
		/* DL-RELEASE Indication */
		q931_dl_indication(ctrl, PRI_EVENT_DCHAN_DOWN);
		stop_t200(ctrl);
		if (ctrl->q921_state == Q921_MULTI_FRAME_ESTABLISHED)
			stop_t203(ctrl);
		q921_setstate(ctrl, Q921_TEI_ASSIGNED);
		q921_restart_ptp_link_if_needed(ctrl);
		break;
	default:
		pri_error(ctrl, "Don't know what to do with DISC in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static void q921_mdl_remove(struct pri *ctrl)
{
	int mdl_free_me;

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "MDL-REMOVE: Removing TEI %d\n", ctrl->tei);
	}
	if (BRI_NT_PTMP(ctrl)) {
		if (ctrl == PRI_MASTER(ctrl)) {
			pri_error(ctrl, "Bad bad bad!  Cannot MDL-REMOVE master\n");
			return;
		}
		mdl_free_me = 1;
	} else {
		mdl_free_me = 0;
	}

	switch (ctrl->q921_state) {
	case Q921_TEI_ASSIGNED:
		/* XXX: deviation! Since we don't have a UI queue, we just discard our I-queue */
		q921_discard_iqueue(ctrl);
		q921_setstate(ctrl, Q921_TEI_UNASSIGNED);
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		q921_discard_iqueue(ctrl);
		/* DL-RELEASE indication */
		q931_dl_indication(ctrl, PRI_EVENT_DCHAN_DOWN);
		stop_t200(ctrl);
		q921_setstate(ctrl, Q921_TEI_UNASSIGNED);
		break;
	case Q921_AWAITING_RELEASE:
		q921_discard_iqueue(ctrl);
		/* DL-RELEASE confirm */
		stop_t200(ctrl);
		q921_setstate(ctrl, Q921_TEI_UNASSIGNED);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		q921_discard_iqueue(ctrl);
		/* DL-RELEASE indication */
		q931_dl_indication(ctrl, PRI_EVENT_DCHAN_DOWN);
		stop_t200(ctrl);
		stop_t203(ctrl);
		q921_setstate(ctrl, Q921_TEI_UNASSIGNED);
		break;
	case Q921_TIMER_RECOVERY:
		q921_discard_iqueue(ctrl);
		/* DL-RELEASE indication */
		q931_dl_indication(ctrl, PRI_EVENT_DCHAN_DOWN);
		stop_t200(ctrl);
		q921_setstate(ctrl, Q921_TEI_UNASSIGNED);
		break;
	default:
		pri_error(ctrl, "MDL-REMOVE when in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		return;
	}

	/*
	 * Negate the TEI value so debug messages will display a
	 * negated TEI when it is actually unassigned.
	 */
	ctrl->tei = -ctrl->tei;

	ctrl->mdl_free_me = mdl_free_me;
}

static int q921_mdl_handle_network_error(struct pri *ctrl, char error)
{
	int handled = 0;
	switch (error) {
	case 'C':
	case 'D':
	case 'G':
	case 'H':
		q921_mdl_remove(ctrl);
		handled = 1;
		break;
	case 'A':
	case 'B':
	case 'E':
	case 'F':
	case 'I':
	case 'J':
	case 'K':
		break;
	default:
		pri_error(ctrl, "Network MDL can't handle error of type %c\n", error);
		break;
	}

	return handled;
}

static int q921_mdl_handle_cpe_error(struct pri *ctrl, char error)
{
	int handled = 0;

	switch (error) {
	case 'C':
	case 'D':
	case 'G':
	case 'H':
		q921_mdl_remove(ctrl);
		handled = 1;
		break;
	case 'A':
	case 'B':
	case 'E':
	case 'F':
	case 'I':
	case 'J':
	case 'K':
		break;
	default:
		pri_error(ctrl, "CPE MDL can't handle error of type %c\n", error);
		break;
	}

	return handled;
}

static int q921_mdl_handle_ptp_error(struct pri *ctrl, char error)
{
	int handled = 0;
	/* This is where we act a bit like L3 instead of L2, since we've got an L3 that depends on us
	 * keeping L2 automatically alive and happy for point to point links */
	switch (error) {
	case 'Z':
		/* This is a special MDL error that actually isn't a spec error, but just so we
		 * have an asynchronous context from the state machine to kick a PTP link back
		 * up after being requested to drop politely (using DISC or DM) */
	case 'G':
		/* We pick it back up and put it back together for this case */
		q921_discard_iqueue(ctrl);
		q921_establish_data_link(ctrl);
		q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		ctrl->l3initiated = 1;

		ctrl->schedev = 1;
		ctrl->ev.gen.e = PRI_EVENT_DCHAN_DOWN;

		handled = 1;
		break;
	case 'A':
	case 'B':
	case 'C':
	case 'D':
	case 'E':
	case 'F':
	case 'H':
	case 'I':
	case 'J':
	case 'K':
		break;
	default:
		pri_error(ctrl, "PTP MDL can't handle error of type %c\n", error);
		break;
	}

	return handled;
}

static void q921_restart_ptp_link_if_needed(struct pri *ctrl)
{
	if (PTP_MODE(ctrl)) {
		q921_mdl_error(ctrl, 'Z');
	}
}

static void q921_mdl_handle_error(struct pri *ctrl, char error, int errored_state)
{
	if (PTP_MODE(ctrl)) {
		q921_mdl_handle_ptp_error(ctrl, error);
	} else {
		if (ctrl->localtype == PRI_NETWORK) {
			q921_mdl_handle_network_error(ctrl, error);
		} else {
			q921_mdl_handle_cpe_error(ctrl, error);
		}
	}
}

static void q921_mdl_handle_error_callback(void *vpri)
{
	struct pri *ctrl = vpri;

	q921_mdl_handle_error(ctrl, ctrl->mdl_error, ctrl->mdl_error_state);

	ctrl->mdl_error = 0;
	ctrl->mdl_timer = 0;

	if (ctrl->mdl_free_me) {
		struct pri *master = PRI_MASTER(ctrl);
		struct pri *freep = NULL, *prev, *cur;
		prev = master;
		cur = master->subchannel;

		while (cur) {
			if (cur == ctrl) {
				prev->subchannel = cur->subchannel;
				freep = cur;
				break;
			}
			prev = cur;
			cur = cur->subchannel;
		}

		if (freep == NULL) {
			pri_error(ctrl, "Huh!? no match found in list for TEI %d\n", -ctrl->tei);
			return;
		}

		if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(ctrl, "Freeing TEI of %d\n", -freep->tei);
		}

		__pri_free_tei(freep);
	}

	return;
}

static void q921_mdl_error(struct pri *ctrl, char error)
{
	int is_debug_q921_state;

	/* Log the MDL-ERROR event when detected. */
	is_debug_q921_state = (ctrl->debug & PRI_DEBUG_Q921_STATE);
	switch (error) {
	case 'A':
		pri_message(ctrl,
			"TEI=%d MDL-ERROR (A): Got supervisory frame with F=1 in state %d(%s)\n",
			ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	case 'B':
	case 'E':
		pri_message(ctrl, "TEI=%d MDL-ERROR (%c): DM (F=%c) in state %d(%s)\n",
			ctrl->tei, error, (error == 'B') ? '1' : '0',
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	case 'C':
	case 'D':
		if (is_debug_q921_state || PTP_MODE(ctrl)) {
			pri_message(ctrl, "TEI=%d MDL-ERROR (%c): UA (F=%c) in state %d(%s)\n",
				ctrl->tei, error, (error == 'C') ? '1' : '0',
				ctrl->q921_state, q921_state2str(ctrl->q921_state));
		}
		break;
	case 'F':
		/*
		 * The peer is restarting the link.
		 * Some reasons this might happen:
		 * 1) Our link establishment requests collided.
		 * 2) They got reset.
		 * 3) They could not talk to us for some reason because
		 * their T200 timer expired N200 times.
		 * 4) They got an MDL-ERROR (J).
		 */
		if (is_debug_q921_state) {
			/*
			 * This message is rather annoying and is normal for
			 * reasons 1-3 above.
			 */
			pri_message(ctrl, "TEI=%d MDL-ERROR (F): SABME in state %d(%s)\n",
				ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		}
		break;
	case 'G':
		/* We could not get a response from the peer. */
		if (is_debug_q921_state) {
			pri_message(ctrl,
				"TEI=%d MDL-ERROR (G): T200 expired N200 times sending SABME in state %d(%s)\n",
				ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		}
		break;
	case 'H':
		/* We could not get a response from the peer. */
		if (is_debug_q921_state) {
			pri_message(ctrl,
				"TEI=%d MDL-ERROR (H): T200 expired N200 times sending DISC in state %d(%s)\n",
				ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		}
		break;
	case 'I':
		/* We could not get a response from the peer. */
		if (is_debug_q921_state) {
			pri_message(ctrl,
				"TEI=%d MDL-ERROR (I): T200 expired N200 times sending RR/RNR in state %d(%s)\n",
				ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		}
		break;
	case 'J':
		/* N(R) not within ack window. */
		pri_error(ctrl, "TEI=%d MDL-ERROR (J): N(R) error in state %d(%s)\n",
			ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	case 'K':
		/*
		 * Received a frame reject frame.
		 * The other end does not like what we are doing at all for some reason.
		 */
		pri_error(ctrl, "TEI=%d MDL-ERROR (K): FRMR in state %d(%s)\n",
			ctrl->tei, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	default:
		pri_message(ctrl, "TEI=%d MDL-ERROR (%c): in state %d(%s)\n",
			ctrl->tei, error, ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	if (ctrl->mdl_error) {
		/* This should not happen. */
		pri_error(ctrl,
			"Trying to queue MDL-ERROR (%c) when MDL-ERROR (%c) is already scheduled\n",
			error, ctrl->mdl_error);
		return;
	}
	ctrl->mdl_error = error;
	ctrl->mdl_error_state = ctrl->q921_state;
	ctrl->mdl_timer = pri_schedule_event(ctrl, 0, q921_mdl_handle_error_callback, ctrl);
}

static pri_event *q921_ua_rx(struct pri *ctrl, q921_h *h)
{
	pri_event * res = NULL;

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Got UA\n", ctrl->tei);
	}

	switch (ctrl->q921_state) {
	case Q921_TEI_ASSIGNED:
	case Q921_MULTI_FRAME_ESTABLISHED:
	case Q921_TIMER_RECOVERY:
		if (h->u.p_f) {
			//pri_error(ctrl, "MDL-ERROR (C): UA in state %d(%s) w with P_F bit 1\n",
			//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
			q921_mdl_error(ctrl, 'C');
		} else {
			//pri_error(ctrl, "MDL-ERROR (D): UA in state %d(%s) w with P_F bit 0\n",
			//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
			q921_mdl_error(ctrl, 'D');
		}
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		if (!h->u.p_f) {
			//pri_error(ctrl, "MDL-ERROR (D): UA in state %d(%s) w with P_F bit 0\n",
			//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
			q921_mdl_error(ctrl, 'D');
			break;
		}

		if (!ctrl->l3initiated) {
			if (ctrl->v_s != ctrl->v_a) {
				q921_discard_iqueue(ctrl);
				/* return DL-ESTABLISH-INDICATION */
				q931_dl_indication(ctrl, PRI_EVENT_DCHAN_UP);
			}
		} else {
			ctrl->l3initiated = 0;
			/* return DL-ESTABLISH-CONFIRM */
		}

		if (PTP_MODE(ctrl)) {
			ctrl->ev.gen.e = PRI_EVENT_DCHAN_UP;
			res = &ctrl->ev;
		}

		stop_t200(ctrl);
		start_t203(ctrl);

		ctrl->v_r = ctrl->v_s = ctrl->v_a = 0;

		q921_setstate(ctrl, Q921_MULTI_FRAME_ESTABLISHED);
		break;
	case Q921_AWAITING_RELEASE:
		if (!h->u.p_f) {
			//pri_error(ctrl, "MDL-ERROR (D): UA in state %d(%s) w with P_F bit 0\n",
			//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
			q921_mdl_error(ctrl, 'D');
		} else {
			/* return DL-RELEASE-CONFIRM */
			stop_t200(ctrl);
			q921_setstate(ctrl, Q921_TEI_ASSIGNED);
		}
		break;
	default:
		pri_error(ctrl, "Don't know what to do with UA in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static void q921_enquiry_response(struct pri *ctrl)
{
	if (ctrl->own_rx_busy) {
		/* XXX : TODO later sometime */
		pri_error(ctrl, "Implement me %s: own_rx_busy\n", __FUNCTION__);
		//q921_rnr(ctrl);
	} else {
		q921_rr(ctrl, 1, 0);
	}

	ctrl->acknowledge_pending = 0;
}

static void n_r_error_recovery(struct pri *ctrl)
{
	q921_mdl_error(ctrl, 'J');

	q921_establish_data_link(ctrl);

	ctrl->l3initiated = 0;
}

static void update_v_a(struct pri *ctrl, int n_r)
{
	int idealcnt = 0, realcnt = 0;
	int x;

	/* Cancel each packet as necessary */
	if (ctrl->debug & PRI_DEBUG_Q921_DUMP)
		pri_message(ctrl, "-- Got ACK for N(S)=%d to (but not including) N(S)=%d\n", ctrl->v_a, n_r);
	for (x = ctrl->v_a; x != n_r; Q921_INC(x)) {
		idealcnt++;
		realcnt += q921_ack_packet(ctrl, x);	
	}
	if (idealcnt != realcnt) {
		pri_error(ctrl, "Ideally should have ack'd %d frames, but actually ack'd %d.  This is not good.\n", idealcnt, realcnt);
		q921_dump_iqueue_info(ctrl);
	}

	ctrl->v_a = n_r;
}

static int n_r_is_valid(struct pri *ctrl, int n_r)
{
	int x;

	/* Is V(A) <= N(R) <= V(S) */
	for (x = ctrl->v_a; x != n_r && x != ctrl->v_s; Q921_INC(x)) {
	}
	if (x != n_r) {
		/* MDL-ERROR (J): N(R) is not within ack window. */
		return 0;
	} else {
		return 1;
	}
}

static int q921_invoke_retransmission(struct pri *ctrl, int n_r);

static pri_event * timer_recovery_rr_rej_rx(struct pri *ctrl, q921_h *h)
{
	/* Figure B.7/Q.921 Page 74 */
	ctrl->peer_rx_busy = 0;

	if (is_command(ctrl, h)) {
		if (h->s.p_f) {
			/* Enquiry response */
			q921_enquiry_response(ctrl);
		}
		if (n_r_is_valid(ctrl, h->s.n_r)) {
			update_v_a(ctrl, h->s.n_r);
		} else {
			goto n_r_error_out;
		}
	} else {
		if (!h->s.p_f) {
			if (n_r_is_valid(ctrl, h->s.n_r)) {
				update_v_a(ctrl, h->s.n_r);
			} else {
				goto n_r_error_out;
			}
		} else {
			if (n_r_is_valid(ctrl, h->s.n_r)) {
				update_v_a(ctrl, h->s.n_r);
				stop_t200(ctrl);
				start_t203(ctrl);
				q921_invoke_retransmission(ctrl, h->s.n_r);
				q921_setstate(ctrl, Q921_MULTI_FRAME_ESTABLISHED);
			} else {
				goto n_r_error_out;
			}
		}
	}
	return NULL;
n_r_error_out:
	n_r_error_recovery(ctrl);
	q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
	return NULL;
}

static pri_event *q921_rr_rx(struct pri *ctrl, q921_h *h)
{
	pri_event * res = NULL;

#if 0	/* Don't flood debug trace with RR if not really looking at Q.921 layer. */
	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Got RR N(R)=%d\n", ctrl->tei, h->s.n_r);
	}
#endif

	switch (ctrl->q921_state) {
	case Q921_TIMER_RECOVERY:
		res = timer_recovery_rr_rej_rx(ctrl, h);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* Figure B.7/Q.921 Page 74 */
		ctrl->peer_rx_busy = 0;

		if (is_command(ctrl, h)) {
			if (h->s.p_f) {
				/* Enquiry response */
				q921_enquiry_response(ctrl);
			}
		} else {
			if (h->s.p_f) {
				//pri_message(ctrl, "MDL-ERROR (A): Got RR response with p_f bit set to 1 in state %d(%s)\n",
				//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
				q921_mdl_error(ctrl, 'A');
			}
		}

		if (!n_r_is_valid(ctrl, h->s.n_r)) {
			n_r_error_recovery(ctrl);
			q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		} else {
			if (h->s.n_r == ctrl->v_s) {
				update_v_a(ctrl, h->s.n_r);
				stop_t200(ctrl);
				start_t203(ctrl);
			} else {
				if (h->s.n_r != ctrl->v_a) {
					/* Need to check the validity of n_r as well.. */
					update_v_a(ctrl, h->s.n_r);
					restart_t200(ctrl);
				}
			}
		}
		break;
	case Q921_TEI_ASSIGNED:
	case Q921_AWAITING_ESTABLISHMENT:
	case Q921_AWAITING_RELEASE:
		/*
		 * Ignore this frame.
		 * We likely got reset and the other end has not realized it yet.
		 */
		break;
	default:
		pri_error(ctrl, "Don't know what to do with RR in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static int q921_invoke_retransmission(struct pri *ctrl, int n_r)
{
	int frames_txd = 0;
	int frames_supposed_to_tx = 0;
	q921_frame *f;
	unsigned int local_v_s = ctrl->v_s;

	/* All acked frames should already have been removed from the queue. */
	for (f = ctrl->txqueue; f && f->transmitted; f = f->next) {
		if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(ctrl, "TEI=%d Retransmitting frame N(S)=%d now!\n",
				ctrl->tei, f->h.n_s);
		}

		/* Give the other side our current N(R) */
		f->h.n_r = ctrl->v_r;
		f->h.p_f = 0;
		q921_transmit(ctrl, (q921_h *)(&f->h), f->len);
		frames_txd++;
	} 

	while (local_v_s != n_r) {
		Q921_DEC(local_v_s);
		frames_supposed_to_tx++;
	}
	if (frames_supposed_to_tx != frames_txd) {
		pri_error(ctrl, "!!!!!!!!!!!! Should have only transmitted %d frames!\n", frames_supposed_to_tx);
	}

	return frames_txd;
}

static pri_event *q921_rej_rx(struct pri *ctrl, q921_h *h)
{
	pri_event * res = NULL;

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Got REJ N(R)=%d\n", ctrl->tei, h->s.n_r);
	}

	switch (ctrl->q921_state) {
	case Q921_TIMER_RECOVERY:
		res = timer_recovery_rr_rej_rx(ctrl, h);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* Figure B.7/Q.921 Page 74 */
		ctrl->peer_rx_busy = 0;

		if (is_command(ctrl, h)) {
			if (h->s.p_f) {
				/* Enquiry response */
				q921_enquiry_response(ctrl);
			}
		} else {
			if (h->s.p_f) {
				//pri_message(ctrl, "MDL-ERROR (A): Got REJ response with p_f bit set to 1 in state %d(%s)\n",
				//	ctrl->q921_state, q921_state2str(ctrl->q921_state));
				q921_mdl_error(ctrl, 'A');
			}
		}

		if (!n_r_is_valid(ctrl, h->s.n_r)) {
			n_r_error_recovery(ctrl);
			q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		} else {
			update_v_a(ctrl, h->s.n_r);
			stop_t200(ctrl);
			start_t203(ctrl);
			q921_invoke_retransmission(ctrl, h->s.n_r);
		}
		break;
	case Q921_TEI_ASSIGNED:
	case Q921_AWAITING_ESTABLISHMENT:
	case Q921_AWAITING_RELEASE:
		/*
		 * Ignore this frame.
		 * We likely got reset and the other end has not realized it yet.
		 */
		break;
	default:
		pri_error(ctrl, "Don't know what to do with REJ in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static pri_event *q921_frmr_rx(struct pri *ctrl, q921_h *h)
{
	pri_event *res = NULL;

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Got FRMR\n", ctrl->tei);
	}

	switch (ctrl->q921_state) {
	case Q921_TIMER_RECOVERY:
	case Q921_MULTI_FRAME_ESTABLISHED:
		q921_mdl_error(ctrl, 'K');
		q921_establish_data_link(ctrl);
		ctrl->l3initiated = 0;
		q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		break;
	case Q921_TEI_ASSIGNED:
	case Q921_AWAITING_ESTABLISHMENT:
	case Q921_AWAITING_RELEASE:
		/*
		 * Ignore this frame.
		 * We likely got reset and the other end has not realized it yet.
		 */
		if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(ctrl, "TEI=%d Ignoring FRMR.\n", ctrl->tei);
		}
		break;
	default:
		pri_error(ctrl, "Don't know what to do with FRMR in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static pri_event *q921_iframe_rx(struct pri *ctrl, q921_h *h, int len)
{
	pri_event * eres = NULL;
	int res = 0;

	switch (ctrl->q921_state) {
	case Q921_TIMER_RECOVERY:
	case Q921_MULTI_FRAME_ESTABLISHED:
		/* FIXME: Verify that it's a command ... */
		if (ctrl->own_rx_busy) {
			/* XXX: Note: There's a difference in th P/F between both states */
			/* DEVIATION: Handle own rx busy */
		} else if (h->i.n_s == ctrl->v_r) {
			Q921_INC(ctrl->v_r);

			ctrl->reject_exception = 0;

			//res = q931_receive(PRI_MASTER(ctrl), ctrl->tei, (q931_h *)h->i.data, len - 4);
			res = q931_receive(ctrl, ctrl->tei, (q931_h *)h->i.data, len - 4);
			if (res != -1 && (res & Q931_RES_HAVEEVENT)) {
				eres = &ctrl->ev;
			}

			if (h->i.p_f) {
				q921_rr(ctrl, 1, 0);
				ctrl->acknowledge_pending = 0;
			} else {
				if (!ctrl->acknowledge_pending) {
					ctrl->acknowledge_pending = 1;
				}
			}
		} else {
			if (ctrl->reject_exception) {
				if (h->i.p_f) {
					q921_rr(ctrl, 1, 0);
					ctrl->acknowledge_pending = 0;
				}
			} else {
				ctrl->reject_exception = 1;
				q921_reject(ctrl, h->i.p_f);
				ctrl->acknowledge_pending = 0;
			}
		}

		if (!n_r_is_valid(ctrl, h->i.n_r)) {
			n_r_error_recovery(ctrl);
			q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		} else {
			if (ctrl->q921_state == Q921_TIMER_RECOVERY) {
				update_v_a(ctrl, h->i.n_r);
			} else {
				if (ctrl->peer_rx_busy) {
					update_v_a(ctrl, h->i.n_r);
				} else {
					if (h->i.n_r == ctrl->v_s) {
						update_v_a(ctrl, h->i.n_r);
						stop_t200(ctrl);
						start_t203(ctrl);
					} else {
						if (h->i.n_r != ctrl->v_a) {
							update_v_a(ctrl, h->i.n_r);
							reschedule_t200(ctrl);
						}
					}
				}
			}
		}
		break;
	case Q921_TEI_ASSIGNED:
	case Q921_AWAITING_ESTABLISHMENT:
	case Q921_AWAITING_RELEASE:
		/*
		 * Ignore this frame.
		 * We likely got reset and the other end has not realized it yet.
		 */
		break;
	default:
		pri_error(ctrl, "Don't know what to do with an I-frame in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return eres;
}

static pri_event *q921_dm_rx(struct pri *ctrl, q921_h *h)
{
	pri_event * res = NULL;

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Got DM\n", ctrl->tei);
	}

	switch (ctrl->q921_state) {
	case Q921_TEI_ASSIGNED:
		if (h->u.p_f)
			break;
		/* else */
		q921_establish_data_link(ctrl);
		ctrl->l3initiated = 1;
		q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		break;
	case Q921_AWAITING_ESTABLISHMENT:
		if (!h->u.p_f)
			break;

		q921_discard_iqueue(ctrl);
		/* DL-RELEASE indication */
		q931_dl_indication(ctrl, PRI_EVENT_DCHAN_DOWN);
		stop_t200(ctrl);
		q921_setstate(ctrl, Q921_TEI_ASSIGNED);
		q921_restart_ptp_link_if_needed(ctrl);
		break;
	case Q921_AWAITING_RELEASE:
		if (!h->u.p_f)
			break;
		/* DL-RELEASE confirm */
		stop_t200(ctrl);
		q921_setstate(ctrl, Q921_TEI_ASSIGNED);
		break;
	case Q921_MULTI_FRAME_ESTABLISHED:
		if (h->u.p_f) {
			/* MDL-ERROR (B) indication */
			q921_mdl_error(ctrl, 'B');
			break;
		}

		q921_mdl_error(ctrl, 'E');
		q921_establish_data_link(ctrl);
		ctrl->l3initiated = 0;
		q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		break;
	case Q921_TIMER_RECOVERY:
		if (h->u.p_f) {
			/* MDL-ERROR (B) indication */
			q921_mdl_error(ctrl, 'B');
		} else {
			/* MDL-ERROR (E) indication */
			q921_mdl_error(ctrl, 'E');
		}
		q921_establish_data_link(ctrl);
		ctrl->l3initiated = 0;
		q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		break;
	default:
		pri_error(ctrl, "Don't know what to do with DM frame in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static pri_event *q921_rnr_rx(struct pri *ctrl, q921_h *h)
{
	pri_event * res = NULL;

	if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
		pri_message(ctrl, "TEI=%d Got RNR N(R)=%d\n", ctrl->tei, h->s.n_r);
	}

	switch (ctrl->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		ctrl->peer_rx_busy = 1;
		if (!is_command(ctrl, h)) {
			if (h->s.p_f) {
				/* MDL-ERROR (A) indication */
				q921_mdl_error(ctrl, 'A');
			}
		} else {
			if (h->s.p_f) {
				q921_enquiry_response(ctrl);
			}
		}

		if (!n_r_is_valid(ctrl, h->s.n_r)) {
			n_r_error_recovery(ctrl);
			q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
		} else {
			update_v_a(ctrl, h->s.n_r);
			stop_t203(ctrl);
			restart_t200(ctrl);
		}
		break;
	case Q921_TIMER_RECOVERY:
		/* Q.921 Figure B.8 Q921 (Sheet 6 of 9) Page 85 */
		ctrl->peer_rx_busy = 1;
		if (is_command(ctrl, h)) {
			if (h->s.p_f) {
				q921_enquiry_response(ctrl);
			}
			if (n_r_is_valid(ctrl, h->s.n_r)) {
				update_v_a(ctrl, h->s.n_r);
				break;
			} else {
				n_r_error_recovery(ctrl);
				q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
				break;
			}
		} else {
			if (h->s.p_f) {
				if (n_r_is_valid(ctrl, h->s.n_r)) {
					update_v_a(ctrl, h->s.n_r);
					restart_t200(ctrl);
					q921_invoke_retransmission(ctrl, h->s.n_r);
					q921_setstate(ctrl, Q921_MULTI_FRAME_ESTABLISHED);
					break;
				} else {
					n_r_error_recovery(ctrl);
					q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
					break;
				}
			} else {
				if (n_r_is_valid(ctrl, h->s.n_r)) {
					update_v_a(ctrl, h->s.n_r);
					break;
				} else {
					n_r_error_recovery(ctrl);
					q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
					break;
				}
			}
		}
		break;
	case Q921_TEI_ASSIGNED:
	case Q921_AWAITING_ESTABLISHMENT:
	case Q921_AWAITING_RELEASE:
		/*
		 * Ignore this frame.
		 * We likely got reset and the other end has not realized it yet.
		 */
		break;
	default:
		pri_error(ctrl, "Don't know what to do with RNR in state %d(%s)\n",
			ctrl->q921_state, q921_state2str(ctrl->q921_state));
		break;
	}

	return res;
}

static void q921_acknowledge_pending_check(struct pri *ctrl)
{
	if (ctrl->acknowledge_pending) {
		ctrl->acknowledge_pending = 0;
		q921_rr(ctrl, 0, 0);
	}
}

static void q921_statemachine_check(struct pri *ctrl)
{
	switch (ctrl->q921_state) {
	case Q921_MULTI_FRAME_ESTABLISHED:
		q921_send_queued_iframes(ctrl);
		q921_acknowledge_pending_check(ctrl);
		break;
	case Q921_TIMER_RECOVERY:
		q921_acknowledge_pending_check(ctrl);
		break;
	default:
		break;
	}
}

static pri_event *__q921_receive_qualified(struct pri *ctrl, q921_h *h, int len)
{
	int res;
	pri_event *ev = NULL;

	switch(h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		ev = q921_iframe_rx(ctrl, h, len);
		break;
	case 1:
		switch ((h->s.x0 << 2) | h->s.ss) {
		case 0x00:
			ev = q921_rr_rx(ctrl, h);
			break;
 		case 0x01:
			ev = q921_rnr_rx(ctrl, h);
			break;
 		case 0x02:
			ev = q921_rej_rx(ctrl, h);
			break;
		default:
			pri_error(ctrl,
				"!! XXX Unknown Supervisory frame x0=%d ss=%d, pf=%d, N(R)=%d, V(A)=%d, V(S)=%d XXX\n",
				h->s.x0, h->s.ss, h->s.p_f, h->s.n_r, ctrl->v_a, ctrl->v_s);
			break;
		}
		break;
	case 3:
		if (len < 3) {
			pri_error(ctrl, "!! Received short unnumbered frame\n");
			break;
		}
		switch ((h->u.m3 << 2) | h->u.m2) {
		case 0x03:
			ev = q921_dm_rx(ctrl, h);
			break;
		case 0x00:
			/* UI-frame */
			res = q931_receive(ctrl, ctrl->tei, (q931_h *) h->u.data, len - 3);
			if (res != -1 && (res & Q931_RES_HAVEEVENT)) {
				ev = &ctrl->ev;
			}
			break;
		case 0x08:
			ev = q921_disc_rx(ctrl, h);
			break;
		case 0x0F:
			/* SABME */
			if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
				pri_message(ctrl, "TEI=%d Got SABME from %s peer.\n",
					ctrl->tei, h->h.c_r ? "network" : "cpe");
			}
			if (h->h.c_r) {
				ctrl->remotetype = PRI_NETWORK;
				if (ctrl->localtype == PRI_NETWORK) {
					/* We can't both be networks */
					ev = pri_mkerror(ctrl, "We think we're the network, but they think they're the network, too.");
					break;
				}
			} else {
				ctrl->remotetype = PRI_CPE;
				if (ctrl->localtype == PRI_CPE) {
					/* We can't both be CPE */
					ev = pri_mkerror(ctrl, "We think we're the CPE, but they think they're the CPE too.\n");
					break;
				}
			}
			ev = q921_sabme_rx(ctrl, h);
			break;
		case 0x0C:
			ev = q921_ua_rx(ctrl, h);
			break;
		case 0x11:
			ev = q921_frmr_rx(ctrl, h);
			break;
		case 0x17:
			pri_error(ctrl, "!! XID frames not supported\n");
			break;
		default:
			pri_error(ctrl, "!! Don't know what to do with u-frame (m3=%d, m2=%d)\n",
				h->u.m3, h->u.m2);
			break;
		}
		break;
	}

	q921_statemachine_check(ctrl);

	return ev;
}

static pri_event *q921_handle_unmatched_frame(struct pri *ctrl, q921_h *h, int len)
{
	ctrl = PRI_MASTER(ctrl);

	if (h->h.tei < 64) {
		pri_error(ctrl, "Do not support manual TEI range. Discarding\n");
		return NULL;
	}

	if (h->h.sapi != Q921_SAPI_CALL_CTRL) {
		pri_error(ctrl, "Message with SAPI other than CALL CTRL is discarded\n");
		return NULL;
	}

	/* If we're NT-PTMP, this means an unrecognized TEI that we'll kill */
	if (BRI_NT_PTMP(ctrl)) {
		if (ctrl->debug & PRI_DEBUG_Q921_STATE) {
			pri_message(ctrl,
				"Could not find candidate subchannel for received frame with SAPI/TEI of %d/%d.\n",
				h->h.sapi, h->h.tei);
			pri_message(ctrl, "Sending TEI release, in order to re-establish TEI state\n");
		}
	
		/* Q.921 says we should send the remove message twice, in case of link corruption */
		q921_send_tei(ctrl, Q921_TEI_IDENTITY_REMOVE, 0, h->h.tei, 1);
		q921_send_tei(ctrl, Q921_TEI_IDENTITY_REMOVE, 0, h->h.tei, 1);
	}

	return NULL;
}

/* This code assumes that the pri structure is the master pri */
static pri_event *__q921_receive(struct pri *ctrl, q921_h *h, int len)
{
	pri_event *ev = NULL;
	struct pri *tei;

	/* Discard FCS */
	len -= 2;
	
	if (ctrl->debug & (PRI_DEBUG_Q921_DUMP | PRI_DEBUG_Q921_RAW)) {
		q921_dump(ctrl, h, len, ctrl->debug & PRI_DEBUG_Q921_RAW, 0);
	}

	/* Check some reject conditions -- Start by rejecting improper ea's */
	if (h->h.ea1 || !h->h.ea2) {
		return NULL;
	}

	if (h->h.sapi == Q921_SAPI_LAYER2_MANAGEMENT) {
		return q921_receive_MDL(ctrl, &h->u, len);
	}

	if (h->h.tei == Q921_TEI_GROUP && h->h.sapi != Q921_SAPI_CALL_CTRL) {
		pri_error(ctrl, "Do not handle group messages to services other than MDL or CALL CTRL\n");
		return NULL;
	}

	if (BRI_TE_PTMP(ctrl)) {
		if (h->h.sapi == ctrl->subchannel->sapi
			&& ((ctrl->subchannel->q921_state >= Q921_TEI_ASSIGNED
				&& h->h.tei == ctrl->subchannel->tei)
				|| h->h.tei == Q921_TEI_GROUP)) {
			ev = __q921_receive_qualified(ctrl->subchannel, h, len);
		}
		/* Only support reception on our single subchannel */
	} else if (BRI_NT_PTMP(ctrl)) {
		tei = pri_find_tei(ctrl, h->h.sapi, h->h.tei);
		if (tei) {
			ev = __q921_receive_qualified(tei, h, len);
		} else {
			ev = q921_handle_unmatched_frame(ctrl, h, len);
		}
	} else if (PTP_MODE(ctrl) && (h->h.sapi == ctrl->sapi) && (h->h.tei == ctrl->tei)) {
		ev = __q921_receive_qualified(ctrl, h, len);
	} else {
		ev = NULL;
	}

	if (ctrl->debug & PRI_DEBUG_Q921_DUMP) {
		pri_message(ctrl, "Done handling message for SAPI/TEI=%d/%d\n", h->h.sapi, h->h.tei);
	}

	return ev;
}

pri_event *q921_receive(struct pri *ctrl, q921_h *h, int len)
{
	pri_event *e;
	e = __q921_receive(ctrl, h, len);
#ifdef LIBPRI_COUNTERS
	ctrl->q921_rxcount++;
#endif
	return e;
}

static void q921_establish_data_link(struct pri *ctrl)
{
	q921_clear_exception_conditions(ctrl);
	ctrl->RC = 0;
	stop_t203(ctrl);
	reschedule_t200(ctrl);
	q921_send_sabme(ctrl);
}

static void nt_ptmp_dchannel_up(void *vpri)
{
	struct pri *ctrl = vpri;

	ctrl->schedev = 1;
	ctrl->ev.gen.e = PRI_EVENT_DCHAN_UP;
}

void q921_start(struct pri *ctrl)
{
	if (PTMP_MODE(ctrl)) {
		if (TE_MODE(ctrl)) {
			q921_setstate(ctrl, Q921_ASSIGN_AWAITING_TEI);
			q921_tei_request(ctrl);
		} else {
			q921_setstate(ctrl, Q921_TEI_UNASSIGNED);
			pri_schedule_event(ctrl, 0, nt_ptmp_dchannel_up, ctrl);
		}
	} else {
		/* PTP mode, no need for TEI management junk */
		q921_establish_data_link(ctrl);
		ctrl->l3initiated = 1;
		q921_setstate(ctrl, Q921_AWAITING_ESTABLISHMENT);
	}
}

