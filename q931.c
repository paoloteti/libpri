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
 
#include "libpri.h"
#include "pri_internal.h"
#include "pri_q921.h"
#include "pri_q931.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct msgtype {
	int msgnum;
	unsigned char *name;
} msgs[] = {
	/* Call establishment messages */
	{ Q931_ALERTING, "ALERTING" },
	{ Q931_CALL_PROCEEDING, "CALL PROCEEDING" },
	{ Q931_CONNECT, "CONNECT" },
	{ Q931_CONNECT_ACKNOWLEDGE, "CONNECT ACKNOWLEDGE" },
	{ Q931_PROGRESS, "PROGRESS" },
	{ Q931_SETUP, "SETUP" },
	
	/* Call disestablishment messages */
	{ Q931_DISCONNECT, "DISCONNECT" },
	{ Q931_RELEASE, "RELEASE" },
	{ Q931_RELEASE_COMPLETE, "RELEASE COMPLETE" },
	{ Q931_RESTART, "RESTART" },
	{ Q931_RESTART_ACKNOWLEDGE, "RESTART ACKNOWLEDGE" },

	/* Miscellaneous */
	{ Q931_STATUS, "STATUS" },
	{ Q931_STATUS_ENQUIRY, "STATUS ENQUIRY" },

	/* Maintenance */
	{ NATIONAL_SERVICE, "SERVICE" },
	{ NATIONAL_SERVICE_ACKNOWLEDGE, "SERVICE ACKNOWLEDGE" },
};

#define FLAG_PREFERRED 2
#define FLAG_EXCLUSIVE 4

#define RESET_INDICATOR_CHANNEL	0
#define RESET_INDICATOR_DS1		6
#define RESET_INDICATOR_PRI		7

#define TRANS_MODE_64_CIRCUIT	0x10
#define TRANS_MODE_384_CIRCUIT	0x13
#define TRANS_MODE_1536_CIRCUIT	0x15
#define TRANS_MODE_MULTIRATE	0x18
#define TRANS_MODE_PACKET		0X40

#define LAYER_1_RATE_ADAPT		0x21
#define LAYER_1_ULAW			0x22

#define RATE_ADAPT_56K			0x0f

#define LAYER_2_LAPB			0x46

#define LAYER_3_X25				0x66

/* The 4ESS uses a different audio field */
#define PRI_TRANS_CAP_AUDIO_4ESS	0x08


#define Q931_CALL_NOT_E2E_ISDN	0x1
#define Q931_CALLED_NOT_ISDN	0x2
#define Q931_CALLER_NOT_ISDN	0x3
#define Q931_INBAND_AVAILABLE	0X8
#define Q931_DELAY_AT_INTERF	0xa

#define CODE_CCITT				0x0
#define CODE_NATIONAL 			0x2
#define CODE_NETWORK_SPECIFIC	0x3

#define LOC_USER					0x0
#define LOC_PRIV_NET_LOCAL_USER		0x1
#define LOC_PUB_NET_LOCAL_USER		0x2
#define LOC_TRANSIT_NET				0x3
#define LOC_PUB_NET_REMOTE_USER		0x4
#define LOC_PRIV_NET_REMOTE_USER	0X5
#define LOC_INTERNATIONAL_NETWORK	0x7
#define LOC_NETWORK_BEYOND_INTERWORKING	0xa

struct q931_call {
	int cr;		/* Call Reference */
	q931_call *next;
	/* Slotmap specified (bitmap of channels 24-1) (Channel Identifier IE) (-1 means not specified) */
	int slotmap;
	/* An explicit channel (Channel Identifier IE) (-1 means not specified) */
	int channelno;
	/* An explicit DS1 (-1 means not specified) */
	int ds1no;
	/* Channel flags (0 means none retrieved) */
	int chanflags;
	
	int alive;				/* Whether or not the call is alive */
	int proc;				/* Whether we've sent a call proceeding / alerting */
	
	int ri;			/* Restart Indicator (Restart Indicator IE) */

	/* Bearer Capability */
	int transcapability;
	int transmoderate;
	int transmultiple;
	int userl1;
	int userl2;
	int userl3;
	int rateadaption;
	
	int sentchannel;

	int progcode;			/* Progress coding */
	int progloc;			/* Progress Location */	
	int progress;			/* Progress indicator */
	
	int causecode;			/* Cause Coding */
	int causeloc;			/* Cause Location */
	int cause;				/* Cause of clearing */
	
	int callstate;			/* Call state of peer as reported */
	
	int callerplan;
	int callerpres;			/* Caller presentation */
	char callernum[256];	/* Caller */
	
	int  calledplan;
	int nonisdn;
	char callednum[256];	/* Called Number */
};

struct ie {
	int ie;
	char *name;
	/* Dump an IE for debugging (preceed all lines by prefix) */
	void (*dump)(q931_ie *ie, int len, char prefix);
	/* Handle IE  returns 0 on success, -1 on failure */
	int (*receive)(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len);
	/* Add IE to a message, return the # of bytes added or -1 on failure */
	int (*transmit)(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int maxlen);
};

static void call_init(struct q931_call *c)
{
	memset(c, 0, sizeof(*c));
	c->alive = 0;
	c->cr = -1;
	c->slotmap = -1;
	c->channelno = -1;
	c->ds1no = -1;
	c->chanflags = 0;
	c->next = NULL;
	c->sentchannel = 0;
}

static char *binary(int b, int len) {
	static char res[33];
	int x;
	memset(res, 0, sizeof(res));
	if (len > 32)
		len = 32;
	for (x=1;x<=len;x++)
		res[x-1] = b & (1 << (len - x)) ? '1' : '0';
	return res;
}

static int receive_channel_id(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{	
	int x;
	int pos=0;
	if (!ie->data[0] & 0x20) {
		fprintf(stderr, "!! Not PRI type!?\n");
		return -1;
	}
	if ((ie->data[0] & 3) != 1) {
		fprintf(stderr, "!! Unexpected Channel selection %d\n", ie->data[0] & 3);
		return -1;
	}
	if (ie->data[0] & 0x08)
		call->chanflags = FLAG_EXCLUSIVE;
	else
		call->chanflags = FLAG_PREFERRED;
	pos++;
	if (ie->data[0] & 0x40) {
		/* DS1 specified -- stop here */
		call->ds1no = ie->data[1] & 0x7f;
		pos++;
	} 
	if (pos < len) {
		/* More coming */
		if ((ie->data[pos] & 0x0f) != 3) {
			fprintf(stderr, "!! Unexpected Channel Type %d\n", ie->data[1] & 0x0f);
			return -1;
		}
		if ((ie->data[pos] & 0x60) != 0) {
			fprintf(stderr, "!! Invalid CCITT coding %d\n", (ie->data[1] & 0x60) >> 5);
			return -1;
		}
		if (ie->data[pos] & 0x10) {
			/* Expect Slot Map */
			call->slotmap = 0;
			pos++;
			for (x=0;x<3;x++) {
				call->slotmap <<= 8;
				call->slotmap |= ie->data[x + pos];
			}
			return 0;
		} else {
			pos++;
			/* Only expect a particular channel */
			call->channelno = ie->data[pos] & 0x7f;
			return 0;
		}
	} else
		return 0;
	return -1;
}

static int transmit_channel_id(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int pos=0;
	/* Start with standard stuff */
	ie->data[pos] = 0xa1;
	/* Add exclusive flag if necessary */
	if (call->chanflags & FLAG_EXCLUSIVE)
		ie->data[pos] |= 0x08;
	else if (!(call->chanflags & FLAG_PREFERRED)) {
		/* Don't need this IE */
		return 0;
	}

	if (call->ds1no > -1) {
		/* Note that we are specifying the identifier */
		ie->data[pos] |= 0x40;
		pos++;
		/* We need to use the Channel Identifier Present thingy.  Just specify it and we're done */
		ie->data[pos] = 0x80 | call->ds1no;
		pos++;
	} else
		pos++;
	if ((call->channelno > -1) || (call->slotmap != -1)) {
		/* We'll have the octet 8.2 and 8.3's present */
		ie->data[pos++] = 0x83;
		if (call->channelno > -1) {
			/* Channel number specified */
			ie->data[pos++] = 0x80 | call->channelno;
			return pos + 2;
		}
		/* We have to send a channel map */
		if (call->slotmap != -1) {
			ie->data[pos-1] |= 0x10;
			ie->data[pos++] = (call->slotmap & 0xff0000) >> 16;
			ie->data[pos++] = (call->slotmap & 0xff00) >> 8;
			ie->data[pos++] = (call->slotmap & 0xff);
			return pos + 2;
		}
	}
	if (call->ds1no > -1) {
		/* We're done */
		return pos + 2;
	}
	fprintf(stderr, "!! No channel map, no channel, and no ds1?  What am I supposed to identify?\n");
	return -1;
}

static void dump_channel_id(q931_ie *ie, int len, char prefix) 
{
	int pos=0;
	int x;
	int res = 0;
	printf("%c Channel ID len = %d [ Ext: %d  IntID: %s, %s Spare: %d, %s Dchan: %d, ChanSel: %d \n",
		prefix, len, (ie->data[0] & 0x80) ? 1 : 0, (ie->data[0] & 0x40) ? "Explicit" : "Implicit",
		(ie->data[0] & 0x20) ? "PRI" : "Other", (ie->data[0] & 0x10) ? 1 : 0,
		(ie->data[0] & 0x08) ? "Exclusive" : "Preferred",  (ie->data[0] & 0x04) ? 1 : 0,
		ie->data[0] & 0x3);
	pos++;
	len--;
	if (ie->data[0] &  0x40) {
		/* Explicitly defined DS1 */
		printf("%c                       Ext: %d   DS1 Identifier: %d  \n", prefix, (ie->data[pos] & 0x80) >> 7, ie->data[pos] & 0x7f);
		pos++;
		len--;
	} else {
		/* Implicitly defined DS1 */
	}
	if (pos < len) {
		/* Still more information here */
		printf("%c                       Ext: %d   Coding: %d   %s Specified   Channel Type: %d\n",
				prefix, (ie->data[pos] & 0x80) >> 7, (ie->data[pos] & 60) >> 5, 
				(ie->data[pos] & 0x10) ? "Slot Map" : "Number", ie->data[pos] & 0x0f);
		if (!(ie->data[pos] & 0x10)) {
			/* Number specified */
			pos++;
			printf("%c                       Ext: %d   Channel: %d ]\n", prefix, (ie->data[pos] & 0x80) >> 7, 
				(ie->data[pos]) & 0x7f);
		} else {
			pos++;
			/* Map specified */
			for (x=0;x<3;x++) {
				res <<= 8;
				res |= ie->data[pos++];
			}
			printf("%c                       Map: %s ]\n", prefix, binary(res, 24));
		}
	} else printf("                         ]\n");				
}

static char *ri2str(int ri)
{
	switch(ri) {
	case 0:
		return "Indicated Channel";
	case 6:
		return "Single DS1 Facility";
	case 7:
		return "All DS1 Facilities";
	default:
		return "Unknown";
	}
}

static void dump_restart_indicator(q931_ie *ie, int len, char prefix)
{
	printf("%c Restart Indentifier: [ Ext: %d  Spare: %d  Resetting %s (%d) ]\n", 
		prefix, (ie->data[0] & 0x80) >> 7, (ie->data[0] & 0x78) >> 3, ri2str(ie->data[0] & 0x7), ie->data[0] & 0x7);
}

static int receive_restart_indicator(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	/* Pretty simple */
	call->ri = ie->data[0] & 0x7;
	return 0;
}

static int transmit_restart_indicator(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int maxlen)
{
	/* Pretty simple */
	switch(call->ri) {
	case 0:
	case 6:
	case 7:
		ie->data[0] = 0x80 | (call->ri & 0x7);
		break;
	case 5:
		/* Switch compatibility */
		ie->data[0] = 0xA0 | (call->ri & 0x7);
		break;
	default:
		fprintf(stderr, "!! Invalid restart indicator value %d\n", call->ri);
		return-1;
	}
	return 3;
}

static char *cap2str(int mode)
{
	switch (mode) {
	case PRI_TRANS_CAP_SPEECH:
		return "Speech";
	case PRI_TRANS_CAP_DIGITAL:
		return "Unrestricted digital information";
	case PRI_TRANS_CAP_AUDIO:
		return "3.1-kHz audio";
	case PRI_TRANS_CAP_AUDIO_4ESS:
		return "3.1-khz audio (4ESS)";
	default:
		return "Unknown";
	}
}

static char *mode2str(int mode)
{
	switch(mode) {
	case TRANS_MODE_64_CIRCUIT:
		return "64-kbps, circuit-mode";
	case TRANS_MODE_384_CIRCUIT:
		return "384kbps, circuit-mode";
	case TRANS_MODE_1536_CIRCUIT:
		return "1536kbps, circuit-mode";
	case TRANS_MODE_MULTIRATE:
		return "Multirate (Nx64kbps)";
	case TRANS_MODE_PACKET:
		return "Packet Mode";
	}
	return "Unknown";
}

static char *l12str(int proto)
{
	switch(proto) {
	case LAYER_1_RATE_ADAPT:
		return "Rate Adaption";
	case LAYER_1_ULAW:
		return "u-Law";
	default:
		return "Unknown";
	}
}

static char *ra2str(int proto)
{
	switch(proto) {
	case RATE_ADAPT_56K:
		return "from 56kbps";
	default:
		return "Unknown";
	}
}



static char *l22str(int proto)
{
	switch(proto) {
	case LAYER_2_LAPB:
		return "LAPB";
	default:
		return "Unknown";
	}
}

static char *l32str(int proto)
{
	switch(proto) {
	case LAYER_3_X25:
		return "X.25";
	default:
		return "Unknown";
	}
}

static void dump_bearer_capability(q931_ie *ie, int len, char prefix)
{
	int pos=2;
	printf("%c Bearer Capability: [ Ext: %d   Q.931 Std: %d  Info transfer capability: %d (%s)\n",
		prefix, (ie->data[0] & 0x80 ) >> 7, (ie->data[0] & 0x60) >> 5, (ie->data[0] & 0x1f), cap2str(ie->data[0] & 0x1f));
	printf("%c                     Ext: %d   Trans Mode/Rate: %d (%s)\n", prefix, (ie->data[1] & 0x80) >> 7, ie->data[1] & 0x7f, mode2str(ie->data[1] & 0x7f));
	if ((ie->data[1] & 0x7f) == 0x18) {
		printf("%c                     Ext: %d   Transfer Rate Multiplier: %d x 64\n", prefix, (ie->data[2] & 0x80) >> 7, ie->data[2] & 0x7f);
		pos++;
	}
	/* Stop here if no more */
	if (pos >= len)
		return;
	if ((ie->data[1] & 0x7f) != TRANS_MODE_PACKET) {
		/* Look for octets 5 and 5.a if present */
		printf("%c                     Ext: %d   User Information Layer 1: %d (%s)\n", prefix, (ie->data[pos] >> 7), ie->data[pos] & 0x7f,l12str(ie->data[pos] & 0x7f));
		if ((ie->data[pos] & 0x7f) == LAYER_1_RATE_ADAPT)
			printf("%c                     Ext: %d   Rate Adaptatation: %d (%s)\n", prefix, ie->data[pos] >> 7, ie->data[pos] & 0x7f, ra2str(ie->data[pos] & 0x7f));
		pos++;
	} else {
		/* Look for octets 6 and 7 but not 5 and 5.a */
		printf("%c                     Ext: %d   User Information Layer 2: %d (%s)\n", prefix, ie->data[pos] >> 7, ie->data[pos] & 0x7f, l22str(ie->data[pos] & 0x7f));
		pos++;
		printf("%c                     Ext: %d   User Information Layer 3: %d (%s)\n", prefix, ie->data[pos] >> 7, ie->data[pos] & 0x7f, l32str(ie->data[pos] & 0x7f));
		pos++;
	}
}

static int receive_bearer_capability(struct pri* pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int pos=2;
	if (ie->data[0] & 0x60) {
		fprintf(stderr, "!! non-standard Q.931 standard field\n");
		return -1;
	}
	call->transcapability = ie->data[0] & 0x1f;
	call->transmoderate = ie->data[1] & 0x7f;
	if (call->transmoderate == PRI_TRANS_CAP_AUDIO_4ESS)
		call->transmoderate = PRI_TRANS_CAP_AUDIO;
	if (call->transmoderate != TRANS_MODE_PACKET) {
		call->userl1 = ie->data[pos] & 0x7f;
		if (call->userl1 == LAYER_1_RATE_ADAPT) {
			call->rateadaption = ie->data[++pos] & 0x7f;
		}
		pos++;
	} else {
		/* Get 6 and 7 */
		call->userl2 = ie->data[pos++] & 0x7f;
		call->userl3 = ie->data[pos] & 0x7f;
	}
	return 0;
}

static int transmit_bearer_capability(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int tc;
	tc = call->transcapability;
	if (pri->switchtype == PRI_SWITCH_ATT4ESS) {
		/* 4ESS uses a different trans capability for 3.1khz audio */
		if (tc == PRI_TRANS_CAP_AUDIO)
			tc = PRI_TRANS_CAP_AUDIO_4ESS;
	}
	ie->data[0] = 0x80 | tc;
	ie->data[1] = call->transmoderate | 0x80;
	if (call->transmoderate != TRANS_MODE_PACKET) {
		/* If you have an AT&T 4ESS, you don't send any more info */
		if (pri->switchtype == PRI_SWITCH_ATT4ESS)
			return 4;
		ie->data[2] = call->userl1 | 0x80; /* XXX Ext bit? XXX */
		if (call->userl1 == LAYER_1_RATE_ADAPT) {
			ie->data[3] = call->rateadaption | 0x80;
			return 6;
		}
		return 5;
	} else {
		ie->data[2] = 0x80 | call->userl2;
		ie->data[3] = 0x80 | call->userl3;
		return 6;
	}
}

char *pri_plan2str(int plan)
{
	switch(plan) {
	case PRI_INTERNATIONAL_ISDN:
		return "International number in ISDN";
	case PRI_NATIONAL_ISDN:
		return "National number in ISDN";
	case PRI_LOCAL_ISDN:
		return "Local number in ISDN";
	case PRI_PRIVATE:
		return "Private numbering plan";
	case PRI_UNKNOWN:
		return "Unknown numbering plan";
	default:
		return "Unknown";
	}
}

char *pri_pres2str(int pres)
{
	switch(pres) {
	case PRES_ALLOWED_USER_NUMBER_NOT_SCREENED:
		return "Presentation permitted, user number not screened";
	case PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN:
		return "Presentation permitted, user number passed network screening";
	case PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN:	
		return "Presentation permitted, user number failed network screening";
	case PRES_ALLOWED_NETWORK_NUMBER:
		return "Presentation allowed of network provided number";
	case PRES_PROHIB_USER_NUMBER_NOT_SCREENED:
		return "Presentation prohibited, user number not screened";
	case PRES_PROHIB_USER_NUMBER_PASSED_SCREEN:
		return "Presentation prohibited, user number passed network screening";
	case PRES_PROHIB_USER_NUMBER_FAILED_SCREEN:	
		return "Presentation prohibited, user number failed network screening";
	case PRES_PROHIB_NETWORK_NUMBER:
		return "Presentation prohibbited of network provided number";
	default:
		return "Unknown";
	}
}


static void q931_get_number(unsigned char *num, int maxlen, unsigned char *src, int len)
{
	if (len > maxlen - 1) {
		num[0] = 0;
		return;
	}
	memcpy(num, src, len);
	num[len] = 0;
}

static void dump_called_party_number(q931_ie *ie, int len, char prefix)
{
	char cnum[256];
	q931_get_number(cnum, sizeof(cnum), ie->data + 1, len - 3);
	printf("%c Called Number len = %d: [ Ext: %d  Type: %s (%d) '%s' ]\n", prefix, len, ie->data[0] >> 7, pri_plan2str(ie->data[0] & 0x7f), ie->data[0] & 0x7f, cnum);
}

static void dump_calling_party_number(q931_ie *ie, int len, char prefix)
{
	char cnum[256];


	q931_get_number(cnum, sizeof(cnum), ie->data + 2, len - 4);
	printf("%c Calling Number len = %d: [ Ext: %d  Type: %s (%d) ", prefix, len, ie->data[0] >> 7, pri_plan2str(ie->data[0] & 0x7f), ie->data[0] & 0x7f);
	printf("%c                            Presentation: %s (%d) '%s' ]\n", prefix, pri_pres2str(ie->data[1]), ie->data[1] & 0x7f, cnum);
}

static int receive_called_party_number(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	q931_get_number(call->callednum, sizeof(call->callednum), ie->data + 1, len - 3);
	call->calledplan = ie->data[0] & 0x7f;
	return 0;
}

static int transmit_called_party_number(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	ie->data[0] = 0x80 | call->calledplan;
	if (strlen(call->callednum)) 
		memcpy(ie->data + 1, call->callednum, strlen(call->callednum));
	return strlen(call->callednum) + 3;
}

static int receive_calling_party_number(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	q931_get_number(call->callernum, sizeof(call->callernum), ie->data + 2, len - 4);
	call->callerplan = ie->data[0] & 0x7f;
	call->callerpres = ie->data[1] & 0x7f;
	return 0;
}

static int transmit_calling_party_number(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	ie->data[0] = call->callerplan;
	ie->data[1] = 0x80 | call->callerpres;
	if (strlen(call->callednum)) 
		memcpy(ie->data + 2, call->callernum, strlen(call->callernum));
	return strlen(call->callernum) + 4;
}

static char *prog2str(int prog)
{
	switch(prog) {
	case Q931_CALL_NOT_E2E_ISDN:
		return "Call is not end-to-end ISDN; further call progress information may be available inband.";
	case Q931_CALLED_NOT_ISDN:
		return "Called equipment is non-ISDN.";
	case Q931_CALLER_NOT_ISDN:
		return "Calling equipment is non-ISDN.";
	case Q931_INBAND_AVAILABLE:
		return "Inband information or appropriate pattern now available.";
	case Q931_DELAY_AT_INTERF:
		return "Delay in response at called Interface.";
	default:
		return  "Unknown";
	}
}

static char *coding2str(int cod)
{
	switch(cod) {
	case CODE_CCITT:
		return "CCITT (ITU) standard";
	case CODE_NATIONAL:
		return "National standard";
	case CODE_NETWORK_SPECIFIC:
		return "Network specific standard";
	default:
		return "Unknown";
	}
}

static char *loc2str(int loc)
{
	switch(loc) {
	case LOC_USER:
		return "User";
	case LOC_PRIV_NET_LOCAL_USER:
		return "Private network serving the local user";
	case LOC_PUB_NET_LOCAL_USER:
		return "Public network serving the local user";
	case LOC_TRANSIT_NET:
		return "Transit network";
	case LOC_PUB_NET_REMOTE_USER:
		return "Public network serving the remote user";
	case LOC_PRIV_NET_REMOTE_USER:
		return "Private network serving the remote user";
	case LOC_INTERNATIONAL_NETWORK:
		return "International network";
	case LOC_NETWORK_BEYOND_INTERWORKING:
		return "Network beyond the interworking point";
	default:
		return "Unknown";
	}
}

static void dump_progress_indicator(q931_ie *ie, int len, char prefix)
{
	printf("%c Progress Indicator len = %d [ Ext: %d Coding standard: %s (%d) 0: %d   Location: %s (%d)\n",
		prefix, ie->len, ie->data[0] >> 7, coding2str((ie->data[0] & 0x6) >> 5), (ie->data[0] & 0x6) >> 5,
		(ie->data[0] & 0x10) >> 4, loc2str(ie->data[0] & 0xf), ie->data[0] & 0xf);
	printf("%c                               Ext: %d Progress Description: %s (%d) ]\n",
		prefix, ie->data[1] >> 7, prog2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f);
}

static int receive_progress_indicator(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->progloc = ie->data[0] & 0xf;
	call->progcode = (ie->data[0] & 0x6) >> 5;
	call->progress = (ie->data[1] & 0x7f);
	return 0;
}

static int transmit_progress_indicator(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	if (call->progress > 0) {
		ie->data[0] = 0x80 | (call->progcode << 5)  | (call->progloc);
		ie->data[1] = 0x80 | (call->progress);
		return 4;
	} else {
		/* Leave off */
		return 0;
	}
}

char *pri_cause2str(int cause)
{
	switch(cause) {
	case PRI_CAUSE_UNALLOCATED:
		return "Unallocated (unassigned) number";
	case PRI_CAUSE_NO_ROUTE_TRANSIT_NET:
		return "No route to specified transmit network";
	case PRI_CAUSE_NO_ROUTE_DESTINATION:
		return "No route to destination";
	case PRI_CAUSE_CHANNEL_UNACCEPTABLE:
		return "Channel unacceptable";
	case PRI_CAUSE_CALL_AWARDED_DELIVERED:
		return "Call awarded and being delivered in an established channel";
	case PRI_CAUSE_NORMAL_CLEARING:
		return "Normal Clearing";
	case PRI_CAUSE_USER_BUSY:
		return "User busy";
	case PRI_CAUSE_NO_USER_RESPONSE:
		return "No user responding";
	case PRI_CAUSE_NO_ANSWER:
		return "User alerting, no answer";
	case PRI_CAUSE_CALL_REJECTED:
		return "Call Rejected";
	case PRI_CAUSE_DESTINATION_OUT_OF_ORDER:
		return "Destination out of order";
	case PRI_CAUSE_INVALID_NUMBER_FORMAT:
		return "Invalid number format";
	case PRI_CAUSE_RESPONSE_TO_STATUS_ENQUIRY:
		return "Response to STATus ENQuiry";
	case PRI_CAUSE_NORMAL_UNSPECIFIED:
		return "Normal, unspecified";
	case PRI_CAUSE_NORMAL_CIRCUIT_CONGESTION:
		return "Circuit/channel congestion";
	case PRI_CAUSE_NORMAL_TEMPORARY_FAILURE:
		return "Temporary failure";
	case PRI_CAUSE_SWITCH_CONGESTION:
		return "Switching equipment congestion";
	case PRI_CAUSE_ACCESS_INFO_DISCARDED:
		return "Access information discarded";
	case PRI_CAUSE_REQUESTED_CHAN_UNAVAIL:
		return "Requested channel not available";
	case PRI_CAUSE_BEARERCAPABILITY_NOTAUTH:
		return "Bearer capability not authorized";
	case PRI_CAUSE_BEARERCAPABILITY_NOTIMPL:
		return "Bearer capability not implemented";
	case PRI_CAUSE_INVALID_CALL_REFERENCE:
		return "Invalid call reference value";
	case PRI_CAUSE_INCOMPATIBLE_DESTINATION:
		return "Incompatible destination";
	case PRI_CAUSE_MANDATORY_IE_MISSING:
		return "Mandatory information element is missing";
	case PRI_CAUSE_MESSAGE_TYPE_NONEXIST:
		return "Message type nonexist.";
	case PRI_CAUSE_IE_NONEXIST:
		return "Info. element nonexist or not implemented";
	case PRI_CAUSE_INVALID_IE_CONTENTS:
		return "Invalid information element contents";
	case PRI_CAUSE_WRONG_CALL_STATE:
		return "Message not compatible with call state";
	case PRI_CAUSE_RECOVERY_ON_TIMER_EXPIRE:
		return "Recover on timer expiry";
	case PRI_CAUSE_PROTOCOL_ERROR:
		return "Protocol error, unspecified";
	case PRI_CAUSE_INTERWORKING:
		return "Interworking, unspecified";
	default:
		return "Unknown";
	}
}

static char *pri_causeclass2str(int cause)
{
	switch(cause) {
	case 0:
	case 1:
		return "Normal Event";
	case 2:
		return "Network Congestion";
	case 3:
		return "Service or Option not Available";
	case 4:
		return "Service or Option not Implemented";
	case 5:
		return "Invalid message";
	case 6:
		return "Protocol Error";
	case 7:
		return "Interworking";
	default:
		return "Unknown";
	}
}

static void dump_cause(q931_ie *ie, int len, char prefix)
{
	int x;
	printf("%c Cause len = %d [ Ext: %d Coding standard: %s (%d) 0: %d   Location: %s (%d)\n",
		prefix, ie->len, ie->data[0] >> 7, coding2str((ie->data[0] & 0x6) >> 5), (ie->data[0] & 0x6) >> 5,
		(ie->data[0] & 0x10) >> 4, loc2str(ie->data[0] & 0xf), ie->data[0] & 0xf);
	printf("%c                  Ext: %d Cause: %s (%d), class = %s (%d) ]\n",
		prefix, (ie->data[1] >> 7), pri_cause2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f, 
			pri_causeclass2str((ie->data[1] & 0x7f) >> 4), (ie->data[1] & 0x7f) >> 4);
	for (x=4;x<len;x++) 
		printf("%c              Cause data %d: %02x (%d)\n", prefix, x-4, ie->data[x], ie->data[x]);
}

static int receive_cause(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->causeloc = ie->data[0] & 0xf;
	call->causecode = (ie->data[0] & 0x6) >> 5;
	call->cause = (ie->data[1] & 0x7f);
	return 0;
}

static int transmit_cause(struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	if (call->cause > 0) {
		ie->data[0] = 0x80 | (call->causecode << 5)  | (call->causeloc);
		ie->data[1] = 0x80 | (call->cause);
		return 4;
	} else {
		/* Leave off */
		return 0;
	}
}

struct ie ies[] = {
	{ NATIONAL_CHANGE_STATUS, "Change Status" },
	{ Q931_LOCKING_SHIFT, "Locking Shift" },
	{ Q931_BEARER_CAPABILITY, "Bearer Capability", dump_bearer_capability, receive_bearer_capability, transmit_bearer_capability },
	{ Q931_CAUSE, "Cause", dump_cause, receive_cause, transmit_cause },
	{ Q931_CALL_STATE, "Call State" },
	{ Q931_CHANNEL_IDENT, "Channel Identification", dump_channel_id, receive_channel_id, transmit_channel_id },
	{ Q931_PROGRESS_INDICATOR, "Progress Indicator", dump_progress_indicator, receive_progress_indicator, transmit_progress_indicator },
	{ Q931_NETWORK_SPEC_FAC, "Network-Specific Facilities" },
	{ Q931_INFORMATION_RATE, "Information Rate" },
	{ Q931_TRANSIT_DELAY, "End-to-End Transit Delay" },
	{ Q931_TRANS_DELAY_SELECT, "Transmit Delay Selection and Indication" },
	{ Q931_BINARY_PARAMETERS, "Packet-layer Binary Parameters" },
	{ Q931_WINDOW_SIZE, "Packet-layer Window Size" },
	{ Q931_CLOSED_USER_GROUP, "Closed User Group" },
	{ Q931_REVERSE_CHARGE_INDIC, "Reverse Charging Indication" },
	{ Q931_CALLING_PARTY_NUMBER, "Calling Party Number", dump_calling_party_number, receive_calling_party_number, transmit_calling_party_number },
	{ Q931_CALLING_PARTY_SUBADDR, "Calling Party Subaddress" },
	{ Q931_CALLED_PARTY_NUMBER, "Called Party Number", dump_called_party_number, receive_called_party_number, transmit_called_party_number },
	{ Q931_CALLED_PARTY_SUBADDR, "Called Party Subaddress" },
	{ Q931_REDIRECTING_NUMBER, "Redirecting Number" },
	{ Q931_REDIRECTING_SUBADDR, "Redirecting Subaddress" },
	{ Q931_TRANSIT_NET_SELECT, "Transit Network Selection" },
	{ Q931_RESTART_INDICATOR, "Restart Indicator", dump_restart_indicator, receive_restart_indicator, transmit_restart_indicator },
	{ Q931_LOW_LAYER_COMPAT, "Low-layer Compatibility" },
	{ Q931_HIGH_LAYER_COMPAT, "High-layer Compatibility" },
};

static char *ie2str(int ie) 
{
	int x;
	for (x=0;x<sizeof(ies) / sizeof(ies[0]); x++) 
		if (ies[x].ie == ie)
			return ies[x].name;
	return "Unknown Information Element";
}	

static inline int ielen(q931_ie *ie)
{
	if (ie->f)
		return 1;
	else
		return 2 + ie->len;
}

static char *msg2str(int msg)
{
	int x;
	for (x=0;x<sizeof(msgs) / sizeof(msgs[0]); x++) 
		if (msgs[x].msgnum == msg)
			return msgs[x].name;
	return "Unknown Message Type";
}

static inline int q931_cr(q931_h *h)
{
	int cr = 0;
	int x;
	if (h->crlen > 3) {
		fprintf(stderr, "Call Reference Length Too long: %d\n", h->crlen);
		return -1;
	}
	for (x=0;x<h->crlen;x++) {
		cr <<= 8;
		cr |= h->crv[x];
	}
	return cr;
}

static inline void q931_dumpie(q931_ie *ie, char prefix)
{
	int x;
	for (x=0;x<sizeof(ies) / sizeof(ies[0]); x++) 
		if (ies[x].ie == ie->ie) {
			if (ies[x].dump)
				ies[x].dump(ie, ielen(ie), prefix);
			else
				printf("%c IE: %s (len = %d)\n", prefix, ies[x].name, ielen(ie));
			return;
		}
	
	fprintf(stderr, "!! %c Unknown IE %d (len = %d)\n", prefix, ie->ie, ielen(ie));
}

static q931_call *q931_getcall(struct pri *pri, int cr)
{
	q931_call *cur, *prev;
	cur = pri->calls;
	prev = NULL;
	while(cur) {
		if (cur->cr == cr)
			return cur;
		prev = cur;
		cur = cur->next;
	}
	/* No call exists, make a new one */
	if (pri->debug & PRI_DEBUG_Q931_STATE)
		printf("-- Making new call for cr %d\n", cr);
	cur = malloc(sizeof(struct q931_call));
	if (cur) {
		call_init(cur);
		/* Call reference */
		cur->cr = cr;
		/* Append to end of list */
		if (prev)
			prev->next = cur;
		else
			pri->calls = cur;
	}
	return cur;
}

q931_call *q931_new_call(struct pri *pri)
{
	q931_call *cur;
	do {
		cur = pri->calls;
		pri->cref++;
		if (pri->cref > 32767)
			pri->cref = 1;
		while(cur) {
			if (cur->cr == (0x8000 | pri->cref))
				break;
			cur = cur->next;
		}
	} while(cur);
	return q931_getcall(pri, pri->cref | 0x8000);
}

static void q931_destroycall(struct pri *pri, int cr)
{
	q931_call *cur, *prev;
	prev = NULL;
	cur = pri->calls;
	while(cur) {
		if (cur->cr == cr) {
			if (prev)
				prev->next = cur->next;
			else
				pri->calls = cur->next;
			free(cur);
			return;
		}
		prev = cur;
		cur = cur->next;
	}
	fprintf(stderr, "Can't destroy call %d!\n", cr);
}

static int add_ie(struct pri *pri, q931_call *call, int msgtype, int ie, q931_ie *iet, int maxlen)
{
	int x;
	int res;
	for (x=0;x<sizeof(ies) / sizeof(ies[0]);x++) {
		if (ies[x].ie == ie) {
			/* This is our baby */
			iet->f = 0;
			iet->ie = ie;
			if (ies[x].transmit) {
				res = ies[x].transmit(pri, call, msgtype, iet, maxlen);
				if (res < 0)
					return res;
				iet->len = res - 2;
				return res;
			} else {
				fprintf(stderr, "!! Don't know how to add an IE %s (%d)\n", ie2str(ie), ie);
				return -1;
			}
		}
	}
	fprintf(stderr, "!! Unknown IE %d (%s)\n", ie, ie2str(ie));
	return -1;
			
				
}

static char *disc2str(int disc)
{
	switch(disc) {
	case Q931_PROTOCOL_DISCRIMINATOR:
		return "Q.931";
	case 0x3:
		return "AT&T Maintenance";
	default:
		return "Unknown";
	}
}

static void q931_dump(q931_h *h, int len, int txrx)
{
	q931_mh *mh;
	char c;
	int x=0, r;
	if (txrx)
		c = '>';
	else
		c = '<';
	printf("%c Protocol Discriminator: %d (%s)\n", c, h->pd, disc2str(h->pd));
	printf("%c Call Ref Len: %d (reference %d) (%s)\n", c, h->crlen, q931_cr(h), (h->crv[0] & 0x80) ? "Terminator" : "Originator");
	/* Message header begins at the end of the call reference number */
	mh = (q931_mh *)(h->contents + h->crlen);
	printf("%c Message type: %s (%d)\n", c, msg2str(mh->msg), mh->msg);
	/* Drop length of header, including call reference */
	len -= (h->crlen + 3);
	while(x < len) {
		r = ielen((q931_ie *)(mh->data + x));
		q931_dumpie((q931_ie *)(mh->data + x), c);
		x += r;
	}
	if (x > len) 
		fprintf(stderr, "XXX Message longer than it should be?? XXX\n");
}

static int q931_handle_ie(struct pri *pri, q931_call *c, int msg, q931_ie *ie)
{
	int x;
	if (pri->debug & PRI_DEBUG_Q931_STATE)
		printf("-- Processing IE %d (%s)\n", ie->ie, ie2str(ie->ie));
	for (x=0;x<sizeof(ies) / sizeof(ies[0]);x++) {
		if (ie->ie == ies[x].ie) {
			if (ies[x].receive)
				return ies[x].receive(pri, c, msg, ie, ielen(ie));
			else {
				fprintf(stderr, "!! No handler for IE %d (%s)\n", ie->ie, ie2str(ie->ie));
				return -1;
			}
		}
	}
	printf("!! Unknown IE %d (%s)\n", ie->ie, ie2str(ie->ie));
	return -1;
}

static void init_header(q931_call *call, char *buf, q931_h **hb, q931_mh **mhb, int *len)
{
	/* Returns header and message header and modifies length in place */
	q931_h *h = (q931_h *)buf;
	q931_mh * mh = (q931_mh *)(h->contents + 2);
	h->pd = Q931_PROTOCOL_DISCRIMINATOR;
	h->x0 = 0;		/* Reserved 0 */
	h->crlen = 2;	/* Two bytes of Call Reference.  Invert the top bit to make it from our sense */
	if (call->cr) {
		h->crv[0] = ((call->cr ^ 0x8000) & 0xff00) >> 8;
		h->crv[1] = (call->cr & 0xff);
	} else {
		/* Unless of course this has no call reference */
		h->crv[0] = 0;
		h->crv[1] = 0;
	}
	mh->f = 0;
	*hb = h;
	*mhb = mh;
	*len -= 5;
	
}

static int q931_xmit(struct pri *pri, q931_h *h, int len, int cr)
{
	if (pri->debug & PRI_DEBUG_Q931_DUMP)
		q931_dump(h, len, 1);
	q921_transmit_iframe(pri, h, len, cr);
	return 0;
}

static int send_message(struct pri *pri, q931_call *c, int msgtype, int ies[])
{
	unsigned char buf[1024];
	q931_h *h;
	q931_mh *mh;
	int len;
	int res;
	int offset=0;
	int x;
	memset(buf, 0, sizeof(buf));
	len = sizeof(buf);
	init_header(c, buf, &h, &mh, &len);
	mh->msg = msgtype;
	x=0;
	while(ies[x] > -1) {
		res = add_ie(pri, c, mh->msg, ies[x], (q931_ie *)(mh->data + offset), len);
		if (res < 0) {
			fprintf(stderr, "!! Unable to add IE '%s'\n", ie2str(ies[x]));
			return -1;
		}
		offset += res;
		len -= res;
		x++;
	}
	/* Invert the logic */
	len = sizeof(buf) - len;
	q931_xmit(pri, h, len, 1);
	return 0;
}

static int restart_ack_ies[] = { Q931_CHANNEL_IDENT, Q931_RESTART_INDICATOR, -1 };

static int restart_ack(struct pri *pri, q931_call *c)
{
	return send_message(pri, c, Q931_RESTART_ACKNOWLEDGE, restart_ack_ies);
}

static int call_proceeding_ies[] = { Q931_CHANNEL_IDENT, -1 };

int q931_call_proceeding(struct pri *pri, q931_call *c)
{
	c->proc = 1;
	return send_message(pri, c, Q931_CALL_PROCEEDING, call_proceeding_ies);
}

static int alerting_ies[] = { Q931_CHANNEL_IDENT, Q931_PROGRESS_INDICATOR, -1 };

int q931_alerting(struct pri *pri, q931_call *c, int channel, int info)
{
	if (channel) 
		c->channelno = channel;		
	c->chanflags &= ~FLAG_PREFERRED;
	c->chanflags |= FLAG_EXCLUSIVE;
	if (info) {
		c->progloc = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progress = Q931_INBAND_AVAILABLE;
	} else
		c->progress = -1;
	if (!c->proc)
		q931_call_proceeding(pri, c);
	return send_message(pri, c, Q931_ALERTING, alerting_ies);
}

static int connect_ies[] = {  Q931_CHANNEL_IDENT, Q931_PROGRESS_INDICATOR, -1 };

int q931_connect(struct pri *pri, q931_call *c, int channel, int nonisdn)
{
	if (channel)
		c->channelno = channel;
	c->chanflags &= ~FLAG_PREFERRED;
	c->chanflags |= FLAG_EXCLUSIVE;
	if (nonisdn && (pri->switchtype != PRI_SWITCH_DMS100)) {
		c->progloc  = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progress = Q931_CALLED_NOT_ISDN;
	} else
		c->progress = -1;
	return send_message(pri, c, Q931_CONNECT, connect_ies);
}

static int release_ies[] = { Q931_CAUSE, -1 };

int q931_release(struct pri *pri, q931_call *c, int cause)
{
	if (c->alive) {
		c->alive = 0;
		c->cause = cause;
		c->causecode = CODE_CCITT;
		c->causeloc = LOC_PRIV_NET_LOCAL_USER;
		return send_message(pri, c, Q931_RELEASE, release_ies);
	} else
		return 0;
}

static int disconnect_ies[] = { Q931_CAUSE, -1 };

int q931_disconnect(struct pri *pri, q931_call *c, int cause)
{
	if (c->alive) {
		c->alive = 0;
		c->cause = cause;
		c->causecode = CODE_CCITT;
		c->causeloc = LOC_PRIV_NET_LOCAL_USER;
		return send_message(pri, c, Q931_DISCONNECT, disconnect_ies);
	} else
		return 0;
}

static int setup_ies[] = { Q931_BEARER_CAPABILITY, Q931_CHANNEL_IDENT, Q931_PROGRESS_INDICATOR,
	Q931_CALLING_PARTY_NUMBER, Q931_CALLED_PARTY_NUMBER, -1 };

int q931_setup(struct pri *pri, q931_call *c, int transmode, int channel, int exclusive, 
					int nonisdn, char *caller, int callerplan, int callerpres, char *called,
					int calledplan)
{
	int res;
	
	
	if (!channel) 
		return -1;
	c->transcapability = transmode;
	c->transmoderate = TRANS_MODE_64_CIRCUIT;
	c->userl1 = LAYER_1_ULAW;
	c->channelno = channel;
	c->slotmap = -1;
	c->ds1no = -1;
	c->nonisdn = nonisdn;
		
	if (exclusive) 
		c->chanflags = FLAG_EXCLUSIVE;
	else
		c->chanflags = FLAG_PREFERRED;
	if (caller) {
		strncpy(c->callernum, caller, sizeof(c->callernum));
		c->callerplan = callerplan;
		if ((pri->switchtype == PRI_SWITCH_DMS100) ||
		    (pri->switchtype == PRI_SWITCH_ATT4ESS)) {
			/* Doesn't like certain presentation types */
			if (!(callerpres & 0x7c))
				callerpres = PRES_ALLOWED_NETWORK_NUMBER;
		}
		c->callerpres = callerpres;
	} else {
		strcpy(c->callernum, "");
		c->callerplan = PRI_UNKNOWN;
		c->callerpres = PRES_NUMBER_NOT_AVAILABLE;
	}
	if (called) {
		strncpy(c->callednum, called, sizeof(c->callednum));
		c->calledplan = calledplan;
	} else
		return -1;

	if (nonisdn && (pri->switchtype == PRI_SWITCH_NI2))
		c->progress = Q931_CALLER_NOT_ISDN;
	else
		c->progress = -1;

	res = send_message(pri, c, Q931_SETUP, setup_ies);
	if (!res)
		c->alive = 1;
	return res;
	
}

static int release_complete_ies[] = { -1 };

static int q931_release_complete(struct pri *pri, q931_call *c)
{
	return send_message(pri, c, Q931_RELEASE_COMPLETE, release_complete_ies);
}

static int connect_acknowledge_ies[] = { -1 };

static int q931_connect_acknowledge(struct pri *pri, q931_call *c)
{
	return send_message(pri, c, Q931_CONNECT_ACKNOWLEDGE, connect_acknowledge_ies);
}


int q931_receive(struct pri *pri, q931_h *h, int len)
{
	q931_mh *mh;
	q931_call *c;
	q931_ie *ie;
	int x;
	int res;
	int r;
	if (pri->debug & PRI_DEBUG_Q931_DUMP)
		q931_dump(h, len, 0);
	mh = (q931_mh *)(h->contents + h->crlen);
	if (h->pd == 0x3) {
		/* This is the weird maintenance stuff.  We majorly
		   KLUDGE this by changing byte 4 from a 0xf (SERVICE) 
		   to a 0x7 (SERVICE ACKNOWLEDGE) */
		h->raw[3] -= 0x8;
		q931_xmit(pri, h, len, 1);
		return 0;
	}
	c = q931_getcall(pri, q931_cr(h));
	if (!c) {
		fprintf(stderr, "Unable to locate call %d\n", q931_cr(h));
		return -1;
	}
	/* Preliminary handling */
	switch(mh->msg) {
	case Q931_RESTART:
		if (pri->debug & PRI_DEBUG_Q931_STATE)
			printf("-- Processing Q.931 Restart\n");
		/* Reset information */
		c->channelno = -1;
		c->slotmap = -1;
		c->chanflags = 0;
		c->ds1no = -1;
		c->ri = -1;
		break;
	case Q931_SETUP:
		if (pri->debug & PRI_DEBUG_Q931_STATE)
			printf("-- Processing Q.931 Call Setup\n");
		c->channelno = -1;
		c->slotmap = -1;
		c->chanflags = 0;
		c->ds1no = -1;
		c->ri = -1;
		c->transcapability = -1;
		c->transmoderate = -1;
		c->transmultiple = -1;
		c->userl1 = -1;
		c->userl2 = -1;
		c->userl3 = -1;
		c->rateadaption = -1;
		c->calledplan = -1;
		c->callerplan = -1;
		c->callerpres = -1;
		strcpy(c->callernum, "");
		strcpy(c->callednum, "");
		break;
	case Q931_CONNECT:
	case Q931_ALERTING:
	case Q931_PROGRESS:
		c->progress = -1;
		break;
	case Q931_CALL_PROCEEDING:
	case Q931_CONNECT_ACKNOWLEDGE:
		/* Do nothing */
		break;
	case Q931_RELEASE:
	case Q931_DISCONNECT:
		c->cause = -1;
		c->causecode = -1;
		c->causeloc = -1;
		break;
	case Q931_RELEASE_COMPLETE:
	case Q931_STATUS:
		c->cause = -1;
		c->causecode = -1;
		c->causeloc = -1;
		c->callstate = -1;
		break;
	default:
		fprintf(stderr, "!! Don't know how to pre-handle message type %s (%d)\n", msg2str(mh->msg), mh->msg);
		return -1;
	}
	x = 0;
	/* Do real IE processing */
	len -= (h->crlen + 3);
	while(len) {
		ie = (q931_ie *)(mh->data + x);
		r = ielen(ie);
		if (r > len) {
			fprintf(stderr, "XXX Message longer than it should be?? XXX\n");
			return -1;
		}
		q931_handle_ie(pri, c, mh->msg, ie);
		x += r;
		len -= r;
	}
	/* Post handling */
	switch(mh->msg) {
	case Q931_RESTART:
		/* Send back the Restart Acknowledge */
		restart_ack(pri, c);
		/* Notify user of restart event */
		pri->ev.e = PRI_EVENT_RESTART;
		pri->ev.restart.channel = c->channelno;
		return Q931_RES_HAVEEVENT;
		break;
	case Q931_SETUP:
		c->alive = 1;
		pri->ev.e = PRI_EVENT_RING;
		pri->ev.ring.channel = c->channelno;
		pri->ev.ring.callingpres = c->callerpres;
		pri->ev.ring.callingplan = c->callerplan;
		strncpy(pri->ev.ring.callingnum, c->callernum, sizeof(pri->ev.ring.callingnum));
		pri->ev.ring.calledplan = c->calledplan;
		strncpy(pri->ev.ring.callednum, c->callednum, sizeof(pri->ev.ring.callednum));
		pri->ev.ring.flexible = ! (c->chanflags & FLAG_EXCLUSIVE);
		pri->ev.ring.cref = c->cr;
		pri->ev.ring.call = c;
		if (c->transmoderate != TRANS_MODE_64_CIRCUIT) {
			q931_release(pri, c, PRI_CAUSE_BEARERCAPABILITY_NOTIMPL);
			break;
		}
		return Q931_RES_HAVEEVENT;
	case Q931_ALERTING:
		pri->ev.e = PRI_EVENT_RINGING;
		pri->ev.ringing.channel = c->channelno;
		pri->ev.ringing.cref = c->cr;
		pri->ev.ringing.call = c;
		return Q931_RES_HAVEEVENT;
	case Q931_CONNECT:
		pri->ev.e = PRI_EVENT_ANSWER;
		pri->ev.answer.channel = c->channelno;
		pri->ev.answer.cref = c->cr;
		pri->ev.answer.call = c;
		q931_connect_acknowledge(pri, c);
		return Q931_RES_HAVEEVENT;
	case Q931_PROGRESS:
	case Q931_CONNECT_ACKNOWLEDGE:
	case Q931_STATUS:
		/* Do nothing */
		if (c->cause != PRI_CAUSE_INTERWORKING)
			fprintf(stderr, "Received unsolicited status: %s\n", pri_cause2str(c->cause));
		break;
	case Q931_CALL_PROCEEDING:
		break;
	case Q931_RELEASE_COMPLETE:
		/* Free resources */
		if (c->alive) {
			pri->ev.e = PRI_EVENT_HANGUP;
			pri->ev.hangup.channel = c->channelno;
			pri->ev.hangup.cref = c->cr;
			pri->ev.hangup.cause = c->cause;
			pri->ev.hangup.call = c;
			res = Q931_RES_HAVEEVENT;
			c->alive = 0;
		} else
			res = 0;
		q931_destroycall(pri, c->cr);
		if (res)
			return res;
		break;
	case Q931_RELEASE:
		pri->ev.e = PRI_EVENT_HANGUP;
		pri->ev.hangup.channel = c->channelno;
		pri->ev.hangup.cref = c->cr;
		pri->ev.hangup.cause = c->cause;
		pri->ev.hangup.call = c;
		if (c->alive)
			res = Q931_RES_HAVEEVENT;
		else
			res = 0;
		q931_release_complete(pri, c);
		/* Free resources */
		q931_destroycall(pri, c->cr);
		if (res)
			return res;
		break;
	case Q931_DISCONNECT:
		/* Send a release with no cause */
		q931_release(pri, c, -1);
		/* Return such an event */
		pri->ev.e = PRI_EVENT_HANGUP;
		pri->ev.hangup.channel = c->channelno;
		pri->ev.hangup.cref = c->cr;
		pri->ev.hangup.cause = c->cause;
		pri->ev.hangup.call = c;
		return Q931_RES_HAVEEVENT;
		
	default:
		fprintf(stderr, "!! Don't know how to post-handle message type %s (%d)\n", msg2str(mh->msg), mh->msg);
		return -1;
	}
	return 0;
}
