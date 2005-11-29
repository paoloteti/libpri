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
 
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "libpri.h"
#include "pri_internal.h"
#include "pri_q921.h" 
#include "pri_q931.h"

#define Q921_INIT(hf) do { \
	(hf).h.sapi = 0; \
	(hf).h.ea1 = 0; \
	(hf).h.ea2 = 1; \
	(hf).h.tei = 0; \
} while(0)

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

static int q921_transmit(struct pri *pri, q921_h *h, int len) {
	int res;
	/* Just send it raw */
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		q921_dump(h, len, pri->debug & PRI_DEBUG_Q921_RAW, 1);
	/* Write an extra two bytes for the FCS */
	res = write(pri->fd, h, len + 2);
	if (res != (len + 2)) {
		fprintf(stderr, "Short write: %d/%d (%s)\n", res, len + 2, strerror(errno));
		return -1;
	}
	return 0;
}


static void q921_send_ua(struct pri *pri, int pfbit)
{
	q921_h h;
	Q921_INIT(h);
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
		fprintf(stderr, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return;
	}
	if (pri->debug & PRI_DEBUG_Q921_STATE)
		printf("Sending Unnumbered Acknowledgement\n");
	q921_transmit(pri, &h, 3);
}

static void q921_send_sabme(void *vpri)
{
	struct pri *pri = vpri;
	q921_h h;
	pri_schedule_del(pri, pri->sabme_timer);
	pri->sabme_timer = 0;
	pri->sabme_timer = pri_schedule_event(pri, T_200, q921_send_sabme, pri);
	Q921_INIT(h);
	h.u.m3 = 3;		/* M3 = 3 */
	h.u.m2 = 3;		/* M2 = 3 */
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
		fprintf(stderr, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return;
	}
	if (pri->debug & PRI_DEBUG_Q921_STATE)
		printf("Sending Set Asynchronous Balanced Mode Extended\n");
	q921_transmit(pri, &h, 3);
	pri->q921_state = Q921_AWAITING_ESTABLISH;
		
}

static int q921_ack_packet(struct pri *pri, int num)
{
	struct q921_frame *f, *prev = NULL;
	f = pri->txqueue;
	while(f) {
		if (f->h.n_s == num) {
			/* That's our packet */
			if (prev)
				prev->next = f->next;
			else
				pri->txqueue = f->next;
			free(f);
			/* Reset retransmission counter if we actually acked something */
			pri->retrans = 0;
			return 1;
		}
		f = f->next;
	}
	return 0;
}

static void t203_expire(void *);
static void t200_expire(void *);

static void q921_ack_rx(struct pri *pri, int ack)
{
	int x;
	int cnt=0;
	/* Make sure the ACK was within our window */
	for (x=pri->v_a; (x != pri->v_s) && (x != ack); Q921_INC(x));
	if (x != ack) {
		/* ACK was outside of our window --- ignore */
		fprintf(stderr, "ACK received outside of window, ignoring\n");
		return;
	}
	/* Cancel each packet as necessary */
	for (x=pri->v_a; x != ack; Q921_INC(x)) 
		cnt += q921_ack_packet(pri, x);	
	if (cnt) {
		if (pri->debug &  PRI_DEBUG_Q921_STATE)
			printf("-- Since there was something acked, stopping T200 counter\n");
		/* Something was ACK'd.  Stop T200 counter */
		pri_schedule_del(pri, pri->t200_timer);
		pri->t200_timer = 0;
	}
	if (pri->t203_timer) {
		if (pri->debug &  PRI_DEBUG_Q921_STATE)
			printf("-- Stopping T203 counter since we got an ACK\n");
		pri_schedule_del(pri, pri->t203_timer);
		pri->t203_timer = 0;
	}
	if (pri->txqueue) {
		/* Something left to transmit, Start the T200 counter again if we stopped it */
		if (pri->debug &  PRI_DEBUG_Q921_STATE)
			printf("-- Something left to transmit, restarting T200 counter\n");
		if (!pri->t200_timer)
			pri->t200_timer = pri_schedule_event(pri, T_200, t200_expire, pri);
	} else {
		if (pri->debug &  PRI_DEBUG_Q921_STATE)
			printf("-- Nothing left, starting T203 counter\n");
		/* Nothing to transmit, start the T203 counter instead */
		pri->t203_timer = pri_schedule_event(pri, T_203, t203_expire, pri);
	}
}

static void q921_rr(struct pri *pri, int pbit) {
	q921_h h;
	Q921_INIT(h);
	h.s.x0 = 0;	/* Always 0 */
	h.s.ss = 0; /* Receive Ready */
	h.s.ft = 1;	/* Frametype (01) */
	h.s.n_r = pri->v_r;	/* N/R */
	h.s.p_f = pbit;		/* Poll/Final set appropriately */
	switch(pri->localtype) {
	case PRI_NETWORK:
		h.h.c_r = 0;
		break;
	case PRI_CPE:
		h.h.c_r = 1;
		break;
	default:
		fprintf(stderr, "Don't know how to U/A on a type %d node\n", pri->localtype);
		return;
	}
	pri->v_na = pri->v_r;	/* Make a note that we've already acked this */
	if (pri->debug & PRI_DEBUG_Q921_STATE)
		printf("Sending Receiver Ready (%d)\n", pri->v_r);
	q921_transmit(pri, &h, 4);
}

static void t200_expire(void *vpri)
{
	struct pri *pri = vpri;
	if (pri->txqueue) {
		/* Retransmit first packet in the queue, setting the poll bit */
		if (pri->debug & PRI_DEBUG_Q921_STATE)
			printf("T200 counter expired, resending last frame and scheduling t200 again (retrans so far = %d)\n", pri->retrans);
		/* Force Poll bit */
		pri->txqueue->h.p_f = 1;	
		/* Update nr */
		pri->txqueue->h.n_r = pri->v_r;
		pri->v_na = pri->v_r;
		pri->solicitfbit = 1;
		pri->retrans++;
		q921_transmit(pri, (q921_h *)&pri->txqueue->h, pri->txqueue->len);
	} else {
		fprintf(stderr, "T200 counter expired, nothing to send...\n");
	}
	pri->t200_timer = 0;
}

int q921_transmit_iframe(struct pri *pri, void *buf, int len, int cr)
{
	q921_frame *f, *prev=NULL;
	for (f=pri->txqueue; f; f = f->next) prev = f;
	f = malloc(sizeof(q921_frame) + len + 2);
	if (f) {
		Q921_INIT(f->h);
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
		f->len = len + 4;
		memcpy(f->h.data, buf, len);
		f->h.n_s = pri->v_s;
		f->h.n_r = pri->v_r;
		pri->v_s++;
		pri->v_na = pri->v_r;
		f->h.p_f = 0;
		f->h.ft = 0;
		if (prev)
			prev->next = f;
		else
			pri->txqueue = f;
		/* Immediately transmit unless we're in a recovery state */
		if (!pri->retrans)
			q921_transmit(pri, (q921_h *)(&f->h), f->len);
		if (pri->t203_timer) {
			if (pri->debug & PRI_DEBUG_Q921_STATE)
				printf("Stopping T_203 timer\n");
			pri_schedule_del(pri, pri->t203_timer);
			pri->t203_timer = 0;
		}
		if (!pri->t200_timer) {
			if (pri->debug & PRI_DEBUG_Q921_STATE)
				printf("Starting T_200 timer\n");
			pri->t200_timer = pri_schedule_event(pri, T_200, t200_expire, pri);
		} else
			if (pri->debug & PRI_DEBUG_Q921_STATE)
				printf("T_200 timer already going (%d)\n", pri->t200_timer);
		
	} else {
		fprintf(stderr, "!! Out of memory for Q.921 transmit\n");
		return -1;
	}
	return 0;
}

static void t203_expire(void *vpri)
{
	struct pri *pri = vpri;
	if (pri->debug &  PRI_DEBUG_Q921_STATE)
		printf("T203 counter expired, sending RR and scheduling t203 again\n");
	/* Solicit an F-bit in the other's RR */
	pri->solicitfbit = 1;
	q921_rr(pri, 1);
	/* Restart ourselves */
	pri->t203_timer = pri_schedule_event(pri, T_203, t203_expire, pri);
}

static pri_event *q921_handle_iframe(struct pri *pri, q921_i *i, int len)
{
	int res;
	/* Make sure this is a valid packet */
	if (i->n_s == pri->v_r) {
		/* Increment next expected I-frame */
		Q921_INC(pri->v_r);
		/* Handle their ACK */
		q921_ack_rx(pri, i->n_r);
		if (i->p_f) {
			/* If the Poll/Final bit is set, immediate send the RR */
			q921_rr(pri, 1);
		}
		/* Receive Q.931 data */
		res = q931_receive(pri, (q931_h *)i->data, len - 4);
		/* Send an RR if one wasn't sent already */
		if (pri->v_na != pri->v_r) 
			q921_rr(pri, 0);
		if (res == -1) {
			return NULL;
		}
		if (res & Q931_RES_HAVEEVENT)
			return &pri->ev;
	} else {
		if (((pri->v_r - i->n_s) & 127) < pri->window) {
			/* It's within our window -- send back an RR */
			q921_rr(pri, 0);
		} else
			fprintf(stderr, "XXX I need to reject (expected %d, got %d) XXX\n", pri->v_r, i->n_s);
#if 0
		q931_reject(pri);
#endif		
	}
	return NULL;
}

void q921_dump(q921_h *h, int len, int showraw, int txrx)
{
	int x;
	if (showraw) {
		printf("%c   [", (txrx ? '>' : '<'));
		for (x=0;x<len;x++) 
			printf("%02x ",h->raw[x]);
		printf("]\n");
	}

	switch(h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		printf("\n  Informational Frame:\n");
		break;
	case 1:
		printf("\n  Supervisory frame:\n");
		break;
	case 3:
		printf("\n  Unnumbered frame:\n");
		break;
	}
	
	printf(
"%c SAPI: %02d  C/R: %d EA: %d\n"
"%c  TEI: %03d        EA: %d\n", 
    (txrx ? '>' : '<'),
	h->h.sapi, 
	h->h.c_r,
	h->h.ea1,
    (txrx ? '>' : '<'),
	h->h.tei,
	h->h.ea2);
	switch(h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		/* Informational frame */
		printf(
"%c N(S): %03d   0: %d\n"
"%c N(R): %03d   P: %d\n"
"%c %d bytes of data\n",
    (txrx ? '>' : '<'),
	h->i.n_s,
	h->i.ft,
    (txrx ? '>' : '<'),
	h->i.n_r,
	h->i.p_f, 
    (txrx ? '>' : '<'),
	len - 4);
		break;
	case 1:
		/* Supervisory frame */
		printf(
"%c Zeros: %d S: %d    01: %d\n"
"%c N(R): %03d  P/F: %d\n"
"%c %d bytes of data\n",
    (txrx ? '>' : '<'),
	h->s.x0,
	h->s.ss,
	h->s.ft,
    (txrx ? '>' : '<'),
	h->s.n_r,
	h->s.p_f, 
    (txrx ? '>' : '<'),
	len - 4);
		break;
	case 3:
		/* Unnumbered frame */
		printf(
"%c M3: %d P/F: %d M2: %d 11: %d\n"
"%c %d bytes of data\n",
    (txrx ? '>' : '<'),
	h->u.m3,
	h->u.p_f,
	h->u.m2,
	h->u.ft,
    (txrx ? '>' : '<'),
	len - 3);
		break;
	};
}

static pri_event *q921_dchannel_up(struct pri *pri)
{
	/* Reset counters, etc */
	q921_reset(pri);
	
	/* Stop any SABME retransmissions */
	pri_schedule_del(pri, pri->sabme_timer);
	pri->sabme_timer = 0;
	
	/* Go into connection established state */
	pri->q921_state = Q921_LINK_CONNECTION_ESTABLISHED;

	/* Start the T203 timer */
	pri->t203_timer = pri_schedule_event(pri, T_203, t203_expire, pri);
	
	/* Report event that D-Channel is now up */
	pri->ev.gen.e = PRI_EVENT_DCHAN_UP;
	return &pri->ev;
}

static pri_event *q921_dchannel_down(struct pri *pri)
{
	/* Reset counters, reset sabme timer etc */
	q921_reset(pri);
	
	/* Report event that D-Channel is now up */
	pri->ev.gen.e = PRI_EVENT_DCHAN_DOWN;
	return &pri->ev;
}

void q921_reset(struct pri *pri)
{
	/* Having gotten a SABME we MUST reset our entire state */
	pri->v_s = 0;
	pri->v_a = 0;
	pri->v_r = 0;
	pri->v_na = 0;
	pri->window = 7;
	pri_schedule_del(pri, pri->sabme_timer);
	pri_schedule_del(pri, pri->t203_timer);
	pri_schedule_del(pri, pri->t200_timer);
	pri->sabme_timer = 0;
	pri->t203_timer = 0;
	pri->t200_timer = 0;
	pri->busy = 0;
	pri->solicitfbit = 0;
	pri->q921_state = Q921_LINK_CONNECTION_RELEASED;
	pri->retrans = 0;
	/* Discard anything waiting to go out */
	q921_discard_retransmissions(pri);
}

pri_event *q921_receive(struct pri *pri, q921_h *h, int len)
{
	/* Discard FCS */
	len -= 2;
	
	if (pri->debug & PRI_DEBUG_Q921_DUMP)
		q921_dump(h, len, pri->debug & PRI_DEBUG_Q921_RAW, 0);

	/* Check some reject conditions -- Start by rejecting improper ea's */
	if (h->h.ea1 || !(h->h.ea2))
		return NULL;

	/* Check for broadcasts - not yet handled */
	if (h->h.tei == Q921_TEI_GROUP)
		return NULL;
	
	/* Check for SAPIs we don't yet handle */
	if (h->h.sapi != Q921_SAPI_CALL_CTRL)
		return NULL;

	switch(h->h.data[0] & Q921_FRAMETYPE_MASK) {
	case 0:
	case 2:
		if (pri->q921_state != Q921_LINK_CONNECTION_ESTABLISHED) {
			fprintf(stderr, "!! Got I-frame while link state %d\n", pri->q921_state);
			return NULL;
		}
		/* Informational frame */
		if (len < 4) {
			fprintf(stderr, "!! Received short I-frame\n");
			break;
		}
		return q921_handle_iframe(pri, &h->i, len);	
		break;
	case 1:
		if (pri->q921_state != Q921_LINK_CONNECTION_ESTABLISHED) {
			fprintf(stderr, "!! Got S-frame while link down\n");
			return NULL;
		}
		if (len < 4) {
			fprintf(stderr, "!! Received short S-frame\n");
			break;
		}
		switch(h->s.ss) {
		case 0:
			/* Receiver Ready */
			pri->busy = 0;
			/* Acknowledge frames as necessary */
			q921_ack_rx(pri, h->s.n_r);
			if (h->s.p_f) {
				/* If it's a p/f one then send back a RR in return with the p/f bit set */
				if (pri->solicitfbit) {
					if (pri->debug & PRI_DEBUG_Q921_STATE) 
						printf("-- Got RR response to our frame\n");
				} else {
					if (pri->debug & PRI_DEBUG_Q921_STATE) 
						printf("-- Unsolicited RR with P/F bit, responding\n");
						q921_rr(pri, 1);
				}
				pri->solicitfbit = 0;
			}
			break;
		default:
			fprintf(stderr, "!! XXX Unknown Supervisory frame XXX\n");
		}
		break;
	case 3:
		if (len < 3) {
			fprintf(stderr, "!! Received short unnumbered frame\n");
			break;
		}
		switch(h->u.m3) {
		case 0:
			if (h->u.m2 == 3) {
				if (h->u.p_f) {
					if (pri->debug & PRI_DEBUG_Q921_STATE)
						printf("-- Got Unconnected Mode from peer.\n");
					/* Disconnected mode */
					if (pri->q921_state != Q921_LINK_CONNECTION_RELEASED)
						return q921_dchannel_down(pri);
				} else {
					if (pri->debug & PRI_DEBUG_Q921_STATE)
						printf("-- DM requesting SABME, starting.\n");
					/* Requesting that we start */
					q921_start(pri);
				}
				break;
			} else if (!h->u.m2) {
				printf("XXX Unnumbered Information not implemented XXX\n");
			}
			break;
		case 2:
			if (pri->debug &  PRI_DEBUG_Q921_STATE)
				printf("-- Got Disconnect from peer.\n");
			/* Acknowledge */
			q921_send_ua(pri, h->u.p_f);
			return q921_dchannel_down(pri);
		case 3:
			if (h->u.m2 == 3) {
				/* SABME */
				if (pri->debug & PRI_DEBUG_Q921_STATE) {
					printf("-- Got SABME from %s peer.\n", h->h.c_r ? "network" : "cpe");
				}
				if (h->h.c_r) {
					pri->remotetype = PRI_NETWORK;
					if (pri->localtype == PRI_NETWORK) {
						/* We can't both be networks */
						return pri_mkerror(pri, "We think we're the network, but they think they're the network, too.");
					}
				} else {
					pri->remotetype = PRI_CPE;
					if (pri->localtype == PRI_CPE) {
						/* We can't both be CPE */
						return pri_mkerror(pri, "We think we're the CPE, but they think they're the CPE too.\n");
					}
				}
				/* Send Unnumbered Acknowledgement */
				q921_send_ua(pri, h->u.p_f);
				return q921_dchannel_up(pri);
			} else if (h->u.m2 == 0) {
					/* It's a UA */
				if (pri->q921_state == Q921_AWAITING_ESTABLISH) {
					if (pri->debug & PRI_DEBUG_Q921_STATE) {
						printf("-- Got UA from %s peer  Link up.\n", h->h.c_r ? "cpe" : "network");
					}
					return q921_dchannel_up(pri);
				} else 
					fprintf(stderr, "!! Got a UA, but i'm in state %d\n", pri->q921_state);
			} else 
				fprintf(stderr, "!! Weird frame received (m3=3, m2 = %d)\n", h->u.m2);
			break;
		case 4:
			fprintf(stderr, "!! Frame got rejected!\n");
			break;
		case 5:
			fprintf(stderr, "!! XID frames not supported\n");
			break;
		default:
			fprintf(stderr, "!! Don't know what to do with M3=%d u-frames\n", h->u.m3);
		}
		break;
				
	}
	return NULL;
}

void q921_start(struct pri *pri)
{
	if (pri->q921_state != Q921_LINK_CONNECTION_RELEASED) {
		fprintf(stderr, "!! q921_start: Not in 'Link Connection Released' state\n");
		return;
	}
	/* Reset our interface */
	q921_reset(pri);
	/* Do the SABME XXX Maybe we should implement T_WAIT? XXX */
	q921_send_sabme(pri);
}
