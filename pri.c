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
	default:
		return "Unknown switchtype";
	}
}

struct pri *pri_new(int fd, int node, int switchtype)
{
	struct pri *p;
	p = malloc(sizeof(struct pri));
	if (p) {
		memset(p, 0, sizeof(struct pri));
		p->fd = fd;
		p->localtype = node;
		p->switchtype = switchtype;
		p->cref = 1;
		/* Start Q.921 layer */
		q921_start(p, 1);
	}
	return p;
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
}

int pri_acknowledge(struct pri *pri, q931_call *call, int channel, int info)
{
	if (!pri || !call)
		return -1;
	return q931_alerting(pri, call, channel, info);
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

int pri_call(struct pri *pri, q931_call *c, int transmode, int channel, int exclusive, 
					int nonisdn, char *caller, int callerplan, char *callername, int callerpres, char *called,
					int calledplan,int ulayer1)
{
	if (!pri || !c)
		return -1;
	return q931_setup(pri, c, transmode, channel, exclusive, nonisdn, caller, callerplan, callername, callerpres, called, calledplan, ulayer1);
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

