/*
 * libpri: An implementation of Primary Rate ISDN
 *
 * Written by Mark Spencer <markster@linux-suppot.net>
 *
 * This program is confidential
 *
 * Copyright (C) 2001, Linux Support Services, Inc.
 * All Rights Reserved.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/select.h>
#include <stdarg.h>
#include "libpri.h"
#include "pri_internal.h"
#include "pri_q921.h"
#include "pri_q931.h"

char *pri_node2str(int node)
{
	switch(node) {
	case PRI_UNKNOWN:
		return "Unknown node type";
	case PRI_NETWORK:
		return "Network";
	case PRI_CPE:
		return "CPE";
	default:
		return "Invalid value";
	}
}

char *pri_switch2str(int sw)
{
	switch(sw) {
	case PRI_SWITCH_NI2:
		return "National ISDN";
	case PRI_SWITCH_DMS100:
		return "Nortel DMS100";
	case PRI_SWITCH_LUCENT5E:
		return "Lucent 5E";
	case PRI_SWITCH_ATT4ESS:
		return "AT&T 4ESS";
	case PRI_SWITCH_NI1:
		return "National ISDN 1";
	case PRI_SWITCH_EUROISDN_E1:
		return "EuroISDN";
	case PRI_SWITCH_GR303_EOC:
		return "GR303 EOC";
	case PRI_SWITCH_GR303_TMC:
		return "GR303 TMC";
	default:
		return "Unknown switchtype";
	}
}

static struct pri *__pri_new(int fd, int node, int switchtype, struct pri *master)
{
	struct pri *p;
	p = malloc(sizeof(struct pri));
	if (p) {
		memset(p, 0, sizeof(struct pri));
		p->fd = fd;
		p->localtype = node;
		p->switchtype = switchtype;
		p->cref = 1;
		p->sapi = Q921_SAPI_CALL_CTRL;
		p->tei = 0;
		p->protodisc = Q931_PROTOCOL_DISCRIMINATOR;
		p->master = master;
		p->callpool = &p->localpool;
#ifdef LIBPRI_COUNTERS
		p->q921_rxcount = 0;
		p->q921_txcount = 0;
		p->q931_rxcount = 0;
		p->q931_txcount = 0;
#endif
		if (switchtype == PRI_SWITCH_GR303_EOC) {
			p->protodisc = GR303_PROTOCOL_DISCRIMINATOR;
			p->sapi = Q921_SAPI_GR303_EOC;
			p->tei = Q921_TEI_GR303_EOC_OPS;
			p->subchannel = __pri_new(-1, node, PRI_SWITCH_GR303_EOC_PATH, p);
			if (!p->subchannel) {
				free(p);
				p = NULL;
			}
		} else if (switchtype == PRI_SWITCH_GR303_TMC) {
			p->protodisc = GR303_PROTOCOL_DISCRIMINATOR;
			p->sapi = Q921_SAPI_GR303_TMC_CALLPROC;
			p->tei = Q921_TEI_GR303_TMC_CALLPROC;
			p->subchannel = __pri_new(-1, node, PRI_SWITCH_GR303_TMC_SWITCHING, p);
			if (!p->subchannel) {
				free(p);
				p = NULL;
			}
		} else if (switchtype == PRI_SWITCH_GR303_TMC_SWITCHING) {
			p->protodisc = GR303_PROTOCOL_DISCRIMINATOR;
			p->sapi = Q921_SAPI_GR303_TMC_SWITCHING;
			p->tei = Q921_TEI_GR303_TMC_SWITCHING;
		} else if (switchtype == PRI_SWITCH_GR303_EOC_PATH) {
			p->protodisc = GR303_PROTOCOL_DISCRIMINATOR;
			p->sapi = Q921_SAPI_GR303_EOC;
			p->tei = Q921_TEI_GR303_EOC_PATH;
		}
		/* Start Q.921 layer, Wait if we're the network */
		if (p)
			q921_start(p, p->localtype == PRI_CPE);
	}
	return p;
}

struct pri *pri_new(int fd, int node, int switchtype)
{
	return __pri_new(fd, node, switchtype, NULL);
}

void pri_set_nsf(struct pri *pri, int nsf)
{
	if (pri)
		pri->nsf = nsf;
}

char *pri_event2str(int id)
{
	switch(id) {
	case PRI_EVENT_DCHAN_UP:
		return "D-Channel Up";
	case PRI_EVENT_DCHAN_DOWN:
		return "D-channel Down";
	case PRI_EVENT_RESTART:
		return "Restart channel";
	case PRI_EVENT_RING:
		return "Ring";
	case PRI_EVENT_CONFIG_ERR:
		return "Configuration Error";
	default:
		return "Unknown Event";
	}
}

pri_event *pri_check_event(struct pri *pri)
{
	char buf[1024];
	int res;
	pri_event *e;
	res = read(pri->fd, buf, sizeof(buf));
	if (res < 0) {
		if (errno != EAGAIN)
			pri_error("Read on %d failed: %s\n", pri->fd, strerror(errno));
		return NULL;
	}
	if (!res)
		return NULL;
	/* Receive the q921 packet */
	e = q921_receive(pri, (q921_h *)buf, res);
	return e;
}

static int wait_pri(struct pri *pri)
{	
	struct timeval *tv, real;
	fd_set fds;
	int res;
	FD_ZERO(&fds);
	FD_SET(pri->fd, &fds);
	tv = pri_schedule_next(pri);
	if (tv) {
		gettimeofday(&real, NULL);
		real.tv_sec = tv->tv_sec - real.tv_sec;
		real.tv_usec = tv->tv_usec - real.tv_usec;
		if (real.tv_usec < 0) {
			real.tv_usec += 1000000;
			real.tv_sec -= 1;
		}
		if (real.tv_sec < 0) {
			real.tv_sec = 0;
			real.tv_usec = 0;
		}
	}
	res = select(pri->fd + 1, &fds, NULL, NULL, tv ? &real : tv);
	if (res < 0) 
		return -1;
	return res;
}

pri_event *pri_mkerror(struct pri *pri, char *errstr)
{
	/* Return a configuration error */
	pri->ev.err.e = PRI_EVENT_CONFIG_ERR;
	strncpy(pri->ev.err.err, errstr, sizeof(pri->ev.err.err) - 1);
	return &pri->ev;
}


pri_event *pri_dchannel_run(struct pri *pri, int block)
{
	pri_event *e;
	int res;
	if (!pri)
		return NULL;
	if (block) {
		do {
			e =  NULL;
			res = wait_pri(pri);
			/* Check for error / interruption */
			if (res < 0)
				return NULL;
			if (!res)
				e = pri_schedule_run(pri);
			else
				e = pri_check_event(pri);
		} while(!e);
	} else {
		e = pri_check_event(pri);
		return e;
	}
	return e;
}

void pri_set_debug(struct pri *pri, int debug)
{
	if (!pri)
		return;
	pri->debug = debug;
	if (pri->subchannel)
		pri_set_debug(pri->subchannel, debug);
}

int pri_acknowledge(struct pri *pri, q931_call *call, int channel, int info)
{
	if (!pri || !call)
		return -1;
	return q931_alerting(pri, call, channel, info);
}

int pri_proceeding(struct pri *pri, q931_call *call, int channel, int info)
{
	if (!pri || !call)
		return -1;
	return q931_call_proceeding(pri, call, channel, info);
}

int pri_progress(struct pri *pri, q931_call *call, int channel, int info)
{
	if (!pri || !call)
		return -1;
	return q931_call_progress(pri, call, channel, info);
}

int pri_information(struct pri *pri, q931_call *call, char digit)
{
	if (!pri || !call)
		return -1;
	return q931_information(pri, call, digit);
}

void pri_destroycall(struct pri *pri, q931_call *call)
{
	if (pri && call)
		__q931_destroycall(pri, call);
	return;
}

int pri_need_more_info(struct pri *pri, q931_call *call, int channel, int nonisdn)
{
	if (!pri || !call)
		return -1;
	return q931_setup_ack(pri, call, channel, nonisdn);
}

int pri_answer(struct pri *pri, q931_call *call, int channel, int nonisdn)
{
	if (!pri || !call)
		return -1;
	return q931_connect(pri, call, channel, nonisdn);
}

#if 0
/* deprecated routines, use pri_hangup */
int pri_release(struct pri *pri, q931_call *call, int cause)
{
	if (!pri || !call)
		return -1;
	return q931_release(pri, call, cause);
}

int pri_disconnect(struct pri *pri, q931_call *call, int cause)
{
	if (!pri || !call)
		return -1;
	return q931_disconnect(pri, call, cause);
}
#endif

int pri_hangup(struct pri *pri, q931_call *call, int cause)
{
	if (!pri || !call)
		return -1;
	if (cause == -1)
		/* normal clear cause */
		cause = 16;
	return q931_hangup(pri, call, cause);
}

int pri_reset(struct pri *pri, int channel)
{
	if (!pri)
		return -1;
	return q931_restart(pri, channel);
}

q931_call *pri_new_call(struct pri *pri)
{
	if (!pri)
		return NULL;
	return q931_new_call(pri);
}

void pri_dump_event(struct pri *pri, pri_event *e)
{
	if (!pri || !e)
		return;
	pri_message("Event type: %s (%d)\n", pri_event2str(e->gen.e), e->gen.e);
	switch(e->gen.e) {
	case PRI_EVENT_DCHAN_UP:
	case PRI_EVENT_DCHAN_DOWN:
		break;
	case PRI_EVENT_CONFIG_ERR:
		pri_message("Error: %s", e->err.err);
		break;
	case PRI_EVENT_RESTART:
		pri_message("Restart on channel %d\n", e->restart.channel);
	case PRI_EVENT_RING:
		pri_message("Calling number: %s (%s, %s)\n", e->ring.callingnum, pri_plan2str(e->ring.callingplan), pri_pres2str(e->ring.callingpres));
		pri_message("Called number: %s (%s)\n", e->ring.callednum, pri_plan2str(e->ring.calledplan));
		pri_message("Channel: %d (%s) Reference number: %d\n", e->ring.channel, e->ring.flexible ? "Flexible" : "Not Flexible", e->ring.cref);
		break;
	case PRI_EVENT_HANGUP:
		pri_message("Hangup, reference number: %d, reason: %s\n", e->hangup.cref, pri_cause2str(e->hangup.cause));
		break;
	default:
		pri_message("Don't know how to dump events of type %d\n", e->gen.e);
	}
}

static void pri_sr_init(struct pri_sr *req)
{
	memset(req, 0, sizeof(struct pri_sr));
	
}

int pri_setup(struct pri *pri, q931_call *c, struct pri_sr *req)
{
	if (!pri || !c)
		return -1;
	return q931_setup(pri, c, req);
}

int pri_call(struct pri *pri, q931_call *c, int transmode, int channel, int exclusive, 
					int nonisdn, char *caller, int callerplan, char *callername, int callerpres, char *called,
					int calledplan,int ulayer1)
{
	struct pri_sr req;
	if (!pri || !c)
		return -1;
	pri_sr_init(&req);
	req.transmode = transmode;
	req.channel = channel;
	req.exclusive = exclusive;
	req.nonisdn =  nonisdn;
	req.caller = caller;
	req.callerplan = callerplan;
	req.callername = callername;
	req.callerpres = callerpres;
	req.called = called;
	req.calledplan = calledplan;
	req.userl1 = ulayer1;
	return q931_setup(pri, c, &req);
}	

static void (*__pri_error)(char *stuff);
static void (*__pri_message)(char *stuff);

void pri_set_message(void (*func)(char *stuff))
{
	__pri_message = func;
}

void pri_set_error(void (*func)(char *stuff))
{
	__pri_error = func;
}

void pri_message(char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (__pri_message)
		__pri_message(tmp);
	else
		fprintf(stdout, tmp);
}

void pri_error(char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (__pri_error)
		__pri_error(tmp);
	else
		fprintf(stderr, tmp);
}
/* Set overlap mode */
void pri_set_overlapdial(struct pri *pri,int state)
{
	pri->overlapdial = state;
}

int pri_fd(struct pri *pri)
{
	return pri->fd;
}

void pri_dump_info(struct pri *pri)
{
#ifdef LIBPRI_COUNTERS
	struct q921_frame *f;
	int q921outstanding = 0;
#endif
	if (!pri)
		return;

	/* Might be nice to format these a little better */
	pri_message("Switchtype: %s\n", pri_switch2str(pri->switchtype));
	pri_message("Type: %s\n", pri_node2str(pri->localtype));
#ifdef LIBPRI_COUNTERS
	/* Remember that Q921 Counters include Q931 packets (and any retransmissions) */
	pri_message("Q931 RX: %d\n", pri->q931_rxcount);
	pri_message("Q931 TX: %d\n", pri->q931_txcount);
	pri_message("Q921 RX: %d\n", pri->q921_rxcount);
	pri_message("Q921 TX: %d\n", pri->q921_txcount);
	f = pri->txqueue;
	while (f) {
		q921outstanding++;
		f = f->next;
	}
	pri_message("Q921 Outstanding: %d\n", q921outstanding);
#endif
	pri_message("Window Length: %d/%d\n", pri->windowlen, pri->window);
	pri_message("Sentrej: %d\n", pri->sentrej);
	pri_message("SolicitFbit: %d\n", pri->solicitfbit);
	pri_message("Retrans: %d\n", pri->retrans);
	pri_message("Busy: %d\n", pri->busy);
	pri_message("Overlap Dial: %d\n", pri->overlapdial);
}

int pri_get_crv(struct pri *pri, q931_call *call, int *callmode)
{
	return q931_call_getcrv(pri, call, callmode);
}

int pri_set_crv(struct pri *pri, q931_call *call, int crv, int callmode)
{
	return q931_call_setcrv(pri, call, crv, callmode);
}

void pri_enslave(struct pri *master, struct pri *slave)
{
	if (master && slave)
		slave->callpool = &master->localpool;
}

struct pri_sr *pri_sr_new(void)
{
	struct pri_sr *req;
	req = malloc(sizeof(struct pri_sr));
	if (req) 
		pri_sr_init(req);
	return req;
}

void pri_sr_free(struct pri_sr *sr)
{
	free(sr);
}

int pri_sr_set_channel(struct pri_sr *sr, int channel, int exclusive, int nonisdn)
{
	sr->channel = channel;
	sr->exclusive = exclusive;
	sr->nonisdn = nonisdn;
	return 0;
}

int pri_sr_set_bearer(struct pri_sr *sr, int transmode, int userl1)
{
	sr->transmode = transmode;
	sr->userl1 = userl1;
	return 0;
}

int pri_sr_set_called(struct pri_sr *sr, char *called, int calledplan, int numcomplete)
{
	sr->called = called;
	sr->calledplan = calledplan;
	sr->numcomplete = numcomplete;
	return 0;
}

int pri_sr_set_caller(struct pri_sr *sr, char *caller, char *callername, int callerplan, int callerpres)
{
	sr->caller = caller;
	sr->callername = callername;
	sr->callerplan = callerplan;
	sr->callerpres = callerpres;
	return 0;
}
