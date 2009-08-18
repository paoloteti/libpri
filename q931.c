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

#include "compat.h"
#include "libpri.h"
#include "pri_internal.h"
#include "pri_q921.h"
#include "pri_q931.h"
#include "pri_facility.h"
#include "rose.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <limits.h>

#define MAX_MAND_IES 10

struct msgtype {
	int msgnum;
	char *name;
	int mandies[MAX_MAND_IES];
};

static struct msgtype msgs[] = {
	/* Call establishment messages */
	{ Q931_ALERTING, "ALERTING" },
	{ Q931_CALL_PROCEEDING, "CALL PROCEEDING" },
	{ Q931_CONNECT, "CONNECT" },
	{ Q931_CONNECT_ACKNOWLEDGE, "CONNECT ACKNOWLEDGE" },
	{ Q931_PROGRESS, "PROGRESS", { Q931_PROGRESS_INDICATOR } },
	{ Q931_SETUP, "SETUP", { Q931_BEARER_CAPABILITY, Q931_CHANNEL_IDENT } },
	{ Q931_SETUP_ACKNOWLEDGE, "SETUP ACKNOWLEDGE" },
	
	/* Call disestablishment messages */
	{ Q931_DISCONNECT, "DISCONNECT", { Q931_CAUSE } },
	{ Q931_RELEASE, "RELEASE" },
	{ Q931_RELEASE_COMPLETE, "RELEASE COMPLETE" },
	{ Q931_RESTART, "RESTART", { Q931_RESTART_INDICATOR } },
	{ Q931_RESTART_ACKNOWLEDGE, "RESTART ACKNOWLEDGE", { Q931_RESTART_INDICATOR } },

	/* Miscellaneous */
	{ Q931_STATUS, "STATUS", { Q931_CAUSE, Q931_CALL_STATE } },
	{ Q931_STATUS_ENQUIRY, "STATUS ENQUIRY" },
	{ Q931_USER_INFORMATION, "USER_INFORMATION" },
	{ Q931_SEGMENT, "SEGMENT" },
	{ Q931_CONGESTION_CONTROL, "CONGESTION CONTROL" },
	{ Q931_INFORMATION, "INFORMATION" },
	{ Q931_FACILITY, "FACILITY" },
	{ Q931_NOTIFY, "NOTIFY", { Q931_IE_NOTIFY_IND } },

	/* Call Management */
	{ Q931_HOLD, "HOLD" },
	{ Q931_HOLD_ACKNOWLEDGE, "HOLD ACKNOWLEDGE" },
	{ Q931_HOLD_REJECT, "HOLD REJECT" },
	{ Q931_RETRIEVE, "RETRIEVE" },
	{ Q931_RETRIEVE_ACKNOWLEDGE, "RETRIEVE ACKNOWLEDGE" },
	{ Q931_RETRIEVE_REJECT, "RETRIEVE REJECT" },
	{ Q931_RESUME, "RESUME" },
	{ Q931_RESUME_ACKNOWLEDGE, "RESUME ACKNOWLEDGE", { Q931_CHANNEL_IDENT } },
	{ Q931_RESUME_REJECT, "RESUME REJECT", { Q931_CAUSE } },
	{ Q931_SUSPEND, "SUSPEND" },
	{ Q931_SUSPEND_ACKNOWLEDGE, "SUSPEND ACKNOWLEDGE" },
	{ Q931_SUSPEND_REJECT, "SUSPEND REJECT" },
};
static int post_handle_q931_message(struct pri *ctrl, struct q931_mh *mh, struct q931_call *c, int missingmand);

struct msgtype maintenance_msgs[] = {
	{ NATIONAL_SERVICE, "SERVICE", { Q931_CHANNEL_IDENT } },
	{ NATIONAL_SERVICE_ACKNOWLEDGE, "SERVICE ACKNOWLEDGE", { Q931_CHANNEL_IDENT } },
};
static int post_handle_maintenance_message(struct pri *ctrl, struct q931_mh *mh, struct q931_call *c);

static struct msgtype causes[] = {
	{ PRI_CAUSE_UNALLOCATED, "Unallocated (unassigned) number" },
	{ PRI_CAUSE_NO_ROUTE_TRANSIT_NET, "No route to specified transmit network" },
	{ PRI_CAUSE_NO_ROUTE_DESTINATION, "No route to destination" },
	{ PRI_CAUSE_CHANNEL_UNACCEPTABLE, "Channel unacceptable" },
	{ PRI_CAUSE_CALL_AWARDED_DELIVERED, "Call awarded and being delivered in an established channel" },
	{ PRI_CAUSE_NORMAL_CLEARING, "Normal Clearing" },
	{ PRI_CAUSE_USER_BUSY, "User busy" },
	{ PRI_CAUSE_NO_USER_RESPONSE, "No user responding" },
	{ PRI_CAUSE_NO_ANSWER, "User alerting, no answer" },
	{ PRI_CAUSE_CALL_REJECTED, "Call Rejected" },
	{ PRI_CAUSE_NUMBER_CHANGED, "Number changed" },
	{ PRI_CAUSE_DESTINATION_OUT_OF_ORDER, "Destination out of order" },
	{ PRI_CAUSE_INVALID_NUMBER_FORMAT, "Invalid number format" },
	{ PRI_CAUSE_FACILITY_REJECTED, "Facility rejected" },
	{ PRI_CAUSE_RESPONSE_TO_STATUS_ENQUIRY, "Response to STATus ENQuiry" },
	{ PRI_CAUSE_NORMAL_UNSPECIFIED, "Normal, unspecified" },
	{ PRI_CAUSE_NORMAL_CIRCUIT_CONGESTION, "Circuit/channel congestion" },
	{ PRI_CAUSE_NETWORK_OUT_OF_ORDER, "Network out of order" },
	{ PRI_CAUSE_NORMAL_TEMPORARY_FAILURE, "Temporary failure" },
	{ PRI_CAUSE_SWITCH_CONGESTION, "Switching equipment congestion" },
	{ PRI_CAUSE_ACCESS_INFO_DISCARDED, "Access information discarded" },
	{ PRI_CAUSE_REQUESTED_CHAN_UNAVAIL, "Requested channel not available" },
	{ PRI_CAUSE_PRE_EMPTED, "Pre-empted" },
	{ PRI_CAUSE_FACILITY_NOT_SUBSCRIBED, "Facility not subscribed" },
	{ PRI_CAUSE_OUTGOING_CALL_BARRED, "Outgoing call barred" },
	{ PRI_CAUSE_INCOMING_CALL_BARRED, "Incoming call barred" },
	{ PRI_CAUSE_BEARERCAPABILITY_NOTAUTH, "Bearer capability not authorized" },
	{ PRI_CAUSE_BEARERCAPABILITY_NOTAVAIL, "Bearer capability not available" },
	{ PRI_CAUSE_BEARERCAPABILITY_NOTIMPL, "Bearer capability not implemented" },
	{ PRI_CAUSE_SERVICEOROPTION_NOTAVAIL, "Service or option not available, unspecified" },
	{ PRI_CAUSE_CHAN_NOT_IMPLEMENTED, "Channel not implemented" },
	{ PRI_CAUSE_FACILITY_NOT_IMPLEMENTED, "Facility not implemented" },
	{ PRI_CAUSE_INVALID_CALL_REFERENCE, "Invalid call reference value" },
	{ PRI_CAUSE_IDENTIFIED_CHANNEL_NOTEXIST, "Identified channel does not exist" },
	{ PRI_CAUSE_INCOMPATIBLE_DESTINATION, "Incompatible destination" },
	{ PRI_CAUSE_INVALID_MSG_UNSPECIFIED, "Invalid message unspecified" },
	{ PRI_CAUSE_MANDATORY_IE_MISSING, "Mandatory information element is missing" },
	{ PRI_CAUSE_MESSAGE_TYPE_NONEXIST, "Message type nonexist." },
	{ PRI_CAUSE_WRONG_MESSAGE, "Wrong message" },
	{ PRI_CAUSE_IE_NONEXIST, "Info. element nonexist or not implemented" },
	{ PRI_CAUSE_INVALID_IE_CONTENTS, "Invalid information element contents" },
	{ PRI_CAUSE_WRONG_CALL_STATE, "Message not compatible with call state" },
	{ PRI_CAUSE_RECOVERY_ON_TIMER_EXPIRE, "Recover on timer expiry" },
	{ PRI_CAUSE_MANDATORY_IE_LENGTH_ERROR, "Mandatory IE length error" },
	{ PRI_CAUSE_PROTOCOL_ERROR, "Protocol error, unspecified" },
	{ PRI_CAUSE_INTERWORKING, "Interworking, unspecified" },
};

static struct msgtype facilities[] = {
       { PRI_NSF_SID_PREFERRED, "CPN (SID) preferred" },
       { PRI_NSF_ANI_PREFERRED, "BN (ANI) preferred" },
       { PRI_NSF_SID_ONLY, "CPN (SID) only" },
       { PRI_NSF_ANI_ONLY, "BN (ANI) only" },
       { PRI_NSF_CALL_ASSOC_TSC, "Call Associated TSC" },
       { PRI_NSF_NOTIF_CATSC_CLEARING, "Notification of CATSC Clearing or Resource Unavailable" },
       { PRI_NSF_OPERATOR, "Operator" },
       { PRI_NSF_PCCO, "Pre-subscribed Common Carrier Operator (PCCO)" },
       { PRI_NSF_SDN, "SDN (including GSDN)" },
       { PRI_NSF_TOLL_FREE_MEGACOM, "Toll Free MEGACOM" },
       { PRI_NSF_MEGACOM, "MEGACOM" },
       { PRI_NSF_ACCUNET, "ACCUNET Switched Digital Service" },
       { PRI_NSF_LONG_DISTANCE_SERVICE, "Long Distance Service" },
       { PRI_NSF_INTERNATIONAL_TOLL_FREE, "International Toll Free Service" },
       { PRI_NSF_ATT_MULTIQUEST, "AT&T MultiQuest" },
       { PRI_NSF_CALL_REDIRECTION_SERVICE, "Call Redirection Service" }
};

#define FLAG_PREFERRED 2
#define FLAG_EXCLUSIVE 4

#define RESET_INDICATOR_CHANNEL	0
#define RESET_INDICATOR_DS1		6
#define RESET_INDICATOR_PRI		7

#define TRANS_MODE_64_CIRCUIT	0x10
#define TRANS_MODE_2x64_CIRCUIT	0x11
#define TRANS_MODE_384_CIRCUIT	0x13
#define TRANS_MODE_1536_CIRCUIT	0x15
#define TRANS_MODE_1920_CIRCUIT	0x17
#define TRANS_MODE_MULTIRATE	0x18
#define TRANS_MODE_PACKET		0x40

#define RATE_ADAPT_56K			0x0f

#define LAYER_2_LAPB			0x46

#define LAYER_3_X25				0x66

/* The 4ESS uses a different audio field */
#define PRI_TRANS_CAP_AUDIO_4ESS	0x08

/* Don't forget to update PRI_PROG_xxx at libpri.h */
#define Q931_PROG_CALL_NOT_E2E_ISDN						0x01
#define Q931_PROG_CALLED_NOT_ISDN						0x02
#define Q931_PROG_CALLER_NOT_ISDN						0x03
#define Q931_PROG_CALLER_RETURNED_TO_ISDN					0x04
#define Q931_PROG_INBAND_AVAILABLE						0x08
#define Q931_PROG_DELAY_AT_INTERF						0x0a
#define Q931_PROG_INTERWORKING_WITH_PUBLIC				0x10
#define Q931_PROG_INTERWORKING_NO_RELEASE				0x11
#define Q931_PROG_INTERWORKING_NO_RELEASE_PRE_ANSWER	0x12
#define Q931_PROG_INTERWORKING_NO_RELEASE_POST_ANSWER	0x13

#define CODE_CCITT					0x0
#define CODE_INTERNATIONAL 			0x1
#define CODE_NATIONAL 				0x2
#define CODE_NETWORK_SPECIFIC		0x3

#define LOC_USER					0x0
#define LOC_PRIV_NET_LOCAL_USER		0x1
#define LOC_PUB_NET_LOCAL_USER		0x2
#define LOC_TRANSIT_NET				0x3
#define LOC_PUB_NET_REMOTE_USER		0x4
#define LOC_PRIV_NET_REMOTE_USER	0x5
#define LOC_INTERNATIONAL_NETWORK	0x7
#define LOC_NETWORK_BEYOND_INTERWORKING	0xa

static char *ie2str(int ie);
static char *msg2str(int msg);


#define FUNC_DUMP(name) void (name)(int full_ie, struct pri *pri, q931_ie *ie, int len, char prefix)
#define FUNC_RECV(name) int (name)(int full_ie, struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len)
#define FUNC_SEND(name) int (name)(int full_ie, struct pri *pri, q931_call *call, int msgtype, q931_ie *ie, int len, int order)

#if 1
/* Update call state with transition trace. */
#define UPDATE_OURCALLSTATE(ctrl,c,newstate) do {\
	if (ctrl->debug & (PRI_DEBUG_Q931_STATE) && c->ourcallstate != newstate) \
		pri_message(ctrl, DBGHEAD "call %d on channel %d enters state %d (%s)\n", DBGINFO, \
			c->cr, c->channelno, newstate, q931_call_state_str(newstate)); \
		c->ourcallstate = newstate; \
	} while (0)
#else
/* Update call state with no trace. */
#define UPDATE_OURCALLSTATE(ctrl,c,newstate) c->ourcallstate = newstate
#endif

struct ie {
	/* Maximal count of same IEs at the message (0 - any, 1..n - limited) */
	int max_count;
	/* IE code */
	int ie;
	/* IE friendly name */
	char *name;
	/* Dump an IE for debugging (preceed all lines by prefix) */
	FUNC_DUMP(*dump);
	/* Handle IE  returns 0 on success, -1 on failure */
	FUNC_RECV(*receive);
	/* Add IE to a message, return the # of bytes added or -1 on failure */
	FUNC_SEND(*transmit);
};

/*!
 * \brief Determine if layer 2 is in PTMP mode.
 *
 * \param ctrl D channel controller.
 *
 * \retval TRUE if in PTMP mode.
 * \retval FALSE otherwise.
 */
int q931_is_ptmp(struct pri *ctrl)
{
	return ctrl->tei == Q921_TEI_GROUP;
}

/*!
 * \brief Initialize the given struct q931_party_name
 *
 * \param name Structure to initialize
 *
 * \return Nothing
 */
void q931_party_name_init(struct q931_party_name *name)
{
	name->valid = 0;
	name->presentation = PRI_PRES_UNAVAILABLE;
	name->char_set = PRI_CHAR_SET_ISO8859_1;
	name->str[0] = '\0';
}

/*!
 * \brief Initialize the given struct q931_party_number
 *
 * \param number Structure to initialize
 *
 * \return Nothing
 */
void q931_party_number_init(struct q931_party_number *number)
{
	number->valid = 0;
	number->presentation = PRI_PRES_UNAVAILABLE | PRI_PRES_USER_NUMBER_UNSCREENED;
	number->plan = (PRI_TON_UNKNOWN << 4) | PRI_NPI_E163_E164;
	number->str[0] = '\0';
}

/*!
 * \brief Initialize the given struct q931_party_address
 *
 * \param address Structure to initialize
 *
 * \return Nothing
 */
void q931_party_address_init(struct q931_party_address *address)
{
	q931_party_number_init(&address->number);
}

/*!
 * \brief Initialize the given struct q931_party_id
 *
 * \param id Structure to initialize
 *
 * \return Nothing
 */
void q931_party_id_init(struct q931_party_id *id)
{
	q931_party_name_init(&id->name);
	q931_party_number_init(&id->number);
}

/*!
 * \brief Initialize the given struct q931_party_redirecting
 *
 * \param redirecting Structure to initialize
 *
 * \return Nothing
 */
void q931_party_redirecting_init(struct q931_party_redirecting *redirecting)
{
	q931_party_id_init(&redirecting->from);
	q931_party_id_init(&redirecting->to);
	q931_party_id_init(&redirecting->orig_called);
	redirecting->state = Q931_REDIRECTING_STATE_IDLE;
	redirecting->count = 0;
	redirecting->orig_reason = PRI_REDIR_UNKNOWN;
	redirecting->reason = PRI_REDIR_UNKNOWN;
}

/*!
 * \brief Compare the left and right party name.
 *
 * \param left Left parameter party name.
 * \param right Right parameter party name.
 *
 * \retval < 0 when left < right.
 * \retval == 0 when left == right.
 * \retval > 0 when left > right.
 */
int q931_party_name_cmp(const struct q931_party_name *left, const struct q931_party_name *right)
{
	int cmp;

	if (!left->valid) {
		if (!right->valid) {
			return 0;
		}
		return -1;
	}
	cmp = left->char_set - right->char_set;
	if (cmp) {
		return cmp;
	}
	cmp = strcmp(left->str, right->str);
	if (cmp) {
		return cmp;
	}
	cmp = left->presentation - right->presentation;
	return cmp;
}

/*!
 * \brief Compare the left and right party number.
 *
 * \param left Left parameter party number.
 * \param right Right parameter party number.
 *
 * \retval < 0 when left < right.
 * \retval == 0 when left == right.
 * \retval > 0 when left > right.
 */
int q931_party_number_cmp(const struct q931_party_number *left, const struct q931_party_number *right)
{
	int cmp;

	if (!left->valid) {
		if (!right->valid) {
			return 0;
		}
		return -1;
	}
	cmp = left->plan - right->plan;
	if (cmp) {
		return cmp;
	}
	cmp = strcmp(left->str, right->str);
	if (cmp) {
		return cmp;
	}
	cmp = left->presentation - right->presentation;
	return cmp;
}

/*!
 * \brief Compare the left and right party id.
 *
 * \param left Left parameter party id.
 * \param right Right parameter party id.
 *
 * \retval < 0 when left < right.
 * \retval == 0 when left == right.
 * \retval > 0 when left > right.
 */
int q931_party_id_cmp(const struct q931_party_id *left, const struct q931_party_id *right)
{
	int cmp;

	cmp = q931_party_number_cmp(&left->number, &right->number);
	if (cmp) {
		return cmp;
	}
	cmp = q931_party_name_cmp(&left->name, &right->name);
	return cmp;
}

/*!
 * \brief Copy the Q.931 party name to the PRI party name structure.
 *
 * \param pri_name PRI party name structure
 * \param q931_name Q.931 party name structure
 *
 * \return Nothing
 */
void q931_party_name_copy_to_pri(struct pri_party_name *pri_name, const struct q931_party_name *q931_name)
{
	if (q931_name->valid) {
		pri_name->valid = 1;
		pri_name->presentation = q931_name->presentation;
		pri_name->char_set = q931_name->char_set;
		libpri_copy_string(pri_name->str, q931_name->str, sizeof(pri_name->str));
	} else {
		pri_name->valid = 0;
		pri_name->presentation = PRI_PRES_UNAVAILABLE;
		pri_name->char_set = PRI_CHAR_SET_ISO8859_1;
		pri_name->str[0] = 0;
	}
}

/*!
 * \brief Copy the Q.931 party number to the PRI party number structure.
 *
 * \param pri_number PRI party number structure
 * \param q931_number Q.931 party number structure
 *
 * \return Nothing
 */
void q931_party_number_copy_to_pri(struct pri_party_number *pri_number, const struct q931_party_number *q931_number)
{
	if (q931_number->valid) {
		pri_number->valid = 1;
		pri_number->presentation = q931_number->presentation;
		pri_number->plan = q931_number->plan;
		libpri_copy_string(pri_number->str, q931_number->str, sizeof(pri_number->str));
	} else {
		pri_number->valid = 0;
		pri_number->presentation = PRI_PRES_UNAVAILABLE | PRI_PRES_USER_NUMBER_UNSCREENED;
		pri_number->plan = (PRI_TON_UNKNOWN << 4) | PRI_NPI_E163_E164;
		pri_number->str[0] = 0;
	}
}

/*!
 * \brief Copy the Q.931 party id to the PRI party id structure.
 *
 * \param pri_id PRI party id structure
 * \param q931_id Q.931 party id structure
 *
 * \return Nothing
 */
void q931_party_id_copy_to_pri(struct pri_party_id *pri_id, const struct q931_party_id *q931_id)
{
	q931_party_name_copy_to_pri(&pri_id->name, &q931_id->name);
	q931_party_number_copy_to_pri(&pri_id->number, &q931_id->number);
}

/*!
 * \brief Copy the Q.931 redirecting data to the PRI redirecting structure.
 *
 * \param pri_redirecting PRI redirecting structure
 * \param q931_redirecting Q.931 redirecting structure
 *
 * \return Nothing
 */
void q931_party_redirecting_copy_to_pri(struct pri_party_redirecting *pri_redirecting, const struct q931_party_redirecting *q931_redirecting)
{
	q931_party_id_copy_to_pri(&pri_redirecting->from, &q931_redirecting->from);
	q931_party_id_copy_to_pri(&pri_redirecting->to, &q931_redirecting->to);
	q931_party_id_copy_to_pri(&pri_redirecting->orig_called,
		&q931_redirecting->orig_called);
	pri_redirecting->count = q931_redirecting->count;
	pri_redirecting->orig_reason = q931_redirecting->orig_reason;
	pri_redirecting->reason = q931_redirecting->reason;
}

/*!
 * \brief Fixup some values in the q931_party_id that may be objectionable by switches.
 *
 * \param ctrl D channel controller.
 * \param id Party ID to tweak.
 *
 * \return Nothing
 */
void q931_party_id_fixup(const struct pri *ctrl, struct q931_party_id *id)
{
	switch (ctrl->switchtype) {
	case PRI_SWITCH_DMS100:
	case PRI_SWITCH_ATT4ESS:
		/* Doesn't like certain presentation types */
		if (id->number.valid && !(id->number.presentation & 0x7c)) {
			/* i.e., If presentation is allowed it must be a network number */
			id->number.presentation = PRES_ALLOWED_NETWORK_NUMBER;
		}
		break;
	default:
		break;
	}
}

/*!
 * \brief Determine the overall presentation value for the given party.
 *
 * \param id Party to determine the overall presentation value.
 *
 * \return Overall presentation value for the given party.
 */
int q931_party_id_presentation(const struct q931_party_id *id)
{
	int number_priority;
	int number_value;
	int number_screening;
	int name_priority;
	int name_value;

	/* Determine name presentation priority. */
	if (!id->name.valid) {
		name_value = PRI_PRES_UNAVAILABLE;
		name_priority = 3;
	} else {
		name_value = id->name.presentation & PRI_PRES_RESTRICTION;
		switch (name_value) {
		case PRI_PRES_RESTRICTED:
			name_priority = 0;
			break;
		case PRI_PRES_ALLOWED:
			name_priority = 1;
			break;
		case PRI_PRES_UNAVAILABLE:
			name_priority = 2;
			break;
		default:
			name_value = PRI_PRES_UNAVAILABLE;
			name_priority = 3;
			break;
		}
	}

	/* Determine number presentation priority. */
	if (!id->number.valid) {
		number_screening = PRI_PRES_USER_NUMBER_UNSCREENED;
		number_value = PRI_PRES_UNAVAILABLE;
		number_priority = 3;
	} else {
		number_screening = id->number.presentation & PRI_PRES_NUMBER_TYPE;
		number_value = id->number.presentation & PRI_PRES_RESTRICTION;
		switch (number_value) {
		case PRI_PRES_RESTRICTED:
			number_priority = 0;
			break;
		case PRI_PRES_ALLOWED:
			number_priority = 1;
			break;
		case PRI_PRES_UNAVAILABLE:
			number_priority = 2;
			break;
		default:
			number_screening = PRI_PRES_USER_NUMBER_UNSCREENED;
			number_value = PRI_PRES_UNAVAILABLE;
			number_priority = 3;
			break;
		}
	}

	/* Select the wining presentation value. */
	if (name_priority < number_priority) {
		number_value = name_value;
	}

	return number_value | number_screening;
}

static char *code2str(int code, struct msgtype *codes, int max)
{
	int x;
	for (x=0;x<max; x++) 
		if (codes[x].msgnum == code)
			return codes[x].name;
	return "Unknown";
}

static char *pritype(int type)
{
	switch (type) {
	case PRI_CPE:
		return "CPE";
		break;
	case PRI_NETWORK:
		return "NET";
		break;
	default:
		return "UNKNOWN";
	}
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

static int receive_channel_id(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{	
	int x;
	int pos=0;
#ifdef NO_BRI_SUPPORT
 	if (!ie->data[0] & 0x20) {
		pri_error(ctrl, "!! Not PRI type!?\n");
 		return -1;
 	}
#endif
#ifndef NOAUTO_CHANNEL_SELECTION_SUPPORT
	if (ctrl->bri) {
		if (!(ie->data[0] & 3))
			call->justsignalling = 1;
		else
			call->channelno = ie->data[0] & 3;
	} else {
		switch (ie->data[0] & 3) {
			case 0:
				call->justsignalling = 1;
				break;
			case 1:
				break;
			default:
				pri_error(ctrl, "!! Unexpected Channel selection %d\n", ie->data[0] & 3);
				return -1;
		}
	}
#endif
	if (ie->data[0] & 0x08)
		call->chanflags = FLAG_EXCLUSIVE;
	else
		call->chanflags = FLAG_PREFERRED;
	pos++;
	if (ie->data[0] & 0x40) {
		/* DS1 specified -- stop here */
		call->ds1no = ie->data[1] & 0x7f;
		call->ds1explicit = 1;
		pos++;
	} else
		call->ds1explicit = 0;

	if (pos+2 < len) {
		/* More coming */
		if ((ie->data[pos] & 0x0f) != 3) {
			pri_error(ctrl, "!! Unexpected Channel Type %d\n", ie->data[1] & 0x0f);
			return -1;
		}
		if ((ie->data[pos] & 0x60) != 0) {
			pri_error(ctrl, "!! Invalid CCITT coding %d\n", (ie->data[1] & 0x60) >> 5);
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
			if (ctrl->chan_mapping_logical && call->channelno > 15)
				call->channelno++;
			return 0;
		}
	} else
		return 0;
	return -1;
}

static int transmit_channel_id(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	int pos=0;

	
	/* We are ready to transmit single IE only */
	if (order > 1)
		return 0;
	
	if (call->justsignalling) {
		ie->data[pos++] = 0xac; /* Read the standards docs to figure this out
					   ECMA-165 section 7.3 */
		return pos + 2;
	}
		
	/* Start with standard stuff */
	if (ctrl->switchtype == PRI_SWITCH_GR303_TMC)
		ie->data[pos] = 0x69;
	else if (ctrl->bri) {
		ie->data[pos] = 0x80;
		if (call->channelno > -1)
			ie->data[pos] |= (call->channelno & 0x3);
	} else
		ie->data[pos] = 0xa1;
	/* Add exclusive flag if necessary */
	if (call->chanflags & FLAG_EXCLUSIVE)
		ie->data[pos] |= 0x08;
	else if (!(call->chanflags & FLAG_PREFERRED)) {
		/* Don't need this IE */
		return 0;
	}

	if (((ctrl->switchtype != PRI_SWITCH_QSIG) && (call->ds1no > 0)) || call->ds1explicit) {
		/* Note that we are specifying the identifier */
		ie->data[pos++] |= 0x40;
		/* We need to use the Channel Identifier Present thingy.  Just specify it and we're done */
		ie->data[pos++] = 0x80 | call->ds1no;
	} else
		pos++;

	if (ctrl->bri)
		return pos + 2;

	if ((call->channelno > -1) || (call->slotmap != -1)) {
		/* We'll have the octet 8.2 and 8.3's present */
		ie->data[pos++] = 0x83;
		if (call->channelno > -1) {
			/* Channel number specified */
			if (ctrl->chan_mapping_logical && call->channelno > 16)
				ie->data[pos++] = 0x80 | (call->channelno - 1);
			else
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
	if (call->ds1no > 0) {
		/* We're done */
		return pos + 2;
	}
	pri_error(ctrl, "!! No channel map, no channel, and no ds1?  What am I supposed to identify?\n");
	return -1;
}

static void dump_channel_id(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int pos=0;
	int x;
	int res = 0;
	static const char*	msg_chan_sel[] = {
		"No channel selected", "B1 channel", "B2 channel","Any channel selected",  
		"No channel selected", "As indicated in following octets", "Reserved","Any channel selected"
	};

	pri_message(ctrl, "%c Channel ID (len=%2d) [ Ext: %d  IntID: %s  %s  Spare: %d  %s  Dchan: %d\n",
		prefix, len, (ie->data[0] & 0x80) ? 1 : 0, (ie->data[0] & 0x40) ? "Explicit" : "Implicit",
		(ie->data[0] & 0x20) ? "PRI" : "Other", (ie->data[0] & 0x10) ? 1 : 0,
		(ie->data[0] & 0x08) ? "Exclusive" : "Preferred",  (ie->data[0] & 0x04) ? 1 : 0);
	pri_message(ctrl, "%c                        ChanSel: %s\n",
		prefix, msg_chan_sel[(ie->data[0] & 0x3) + ((ie->data[0]>>3) & 0x4)]);
	pos++;
	len--;
	if (ie->data[0] &  0x40) {
		/* Explicitly defined DS1 */
		pri_message(ctrl, "%c                       Ext: %d  DS1 Identifier: %d  \n", prefix, (ie->data[pos] & 0x80) >> 7, ie->data[pos] & 0x7f);
		pos++;
	} else {
		/* Implicitly defined DS1 */
	}
	if (pos+2 < len) {
		/* Still more information here */
		pri_message(ctrl, "%c                       Ext: %d  Coding: %d  %s Specified  Channel Type: %d\n",
				prefix, (ie->data[pos] & 0x80) >> 7, (ie->data[pos] & 60) >> 5, 
				(ie->data[pos] & 0x10) ? "Slot Map" : "Number", ie->data[pos] & 0x0f);
		if (!(ie->data[pos] & 0x10)) {
			/* Number specified */
			pos++;
			pri_message(ctrl, "%c                       Ext: %d  Channel: %d Type: %s]\n", prefix, (ie->data[pos] & 0x80) >> 7, 
				(ie->data[pos]) & 0x7f, pritype(ctrl->localtype));
		} else {
			pos++;
			/* Map specified */
			for (x=0;x<3;x++) {
				res <<= 8;
				res |= ie->data[pos++];
			}
			pri_message(ctrl, "%c                       Map: %s ]\n", prefix, binary(res, 24));
		}
	} else pri_message(ctrl, "                         ]\n");				
}

static char *ri2str(int ri)
{
	static struct msgtype ris[] = {
		{ 0, "Indicated Channel" },
		{ 6, "Single DS1 Facility" },
		{ 7, "All DS1 Facilities" },
	};
	return code2str(ri, ris, sizeof(ris) / sizeof(ris[0]));
}

static void dump_restart_indicator(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Restart Indentifier (len=%2d) [ Ext: %d  Spare: %d  Resetting %s (%d) ]\n", 
		prefix, len, (ie->data[0] & 0x80) >> 7, (ie->data[0] & 0x78) >> 3, ri2str(ie->data[0] & 0x7), ie->data[0] & 0x7);
}

static int receive_restart_indicator(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	/* Pretty simple */
	call->ri = ie->data[0] & 0x7;
	return 0;
}

static int transmit_restart_indicator(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
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
		pri_error(ctrl, "!! Invalid restart indicator value %d\n", call->ri);
		return-1;
	}
	return 3;
}

static char *redirection_reason2str(int mode)
{
	static struct msgtype modes[] = {
		{ PRI_REDIR_UNKNOWN, "Unknown" },
		{ PRI_REDIR_FORWARD_ON_BUSY, "Forwarded on busy" },
		{ PRI_REDIR_FORWARD_ON_NO_REPLY, "Forwarded on no reply" },
		{ PRI_REDIR_DEFLECTION, "Call deflected" },
		{ PRI_REDIR_DTE_OUT_OF_ORDER, "Called DTE out of order" },
		{ PRI_REDIR_FORWARDED_BY_DTE, "Forwarded by called DTE" },
		{ PRI_REDIR_UNCONDITIONAL, "Forwarded unconditionally" },
	};
	return code2str(mode, modes, sizeof(modes) / sizeof(modes[0]));
}

static char *cap2str(int mode)
{
	static struct msgtype modes[] = {
		{ PRI_TRANS_CAP_SPEECH, "Speech" },
		{ PRI_TRANS_CAP_DIGITAL, "Unrestricted digital information" },
		{ PRI_TRANS_CAP_RESTRICTED_DIGITAL, "Restricted digital information" },
		{ PRI_TRANS_CAP_3_1K_AUDIO, "3.1kHz audio" },
		{ PRI_TRANS_CAP_DIGITAL_W_TONES, "Unrestricted digital information with tones/announcements" },
		{ PRI_TRANS_CAP_VIDEO, "Video" },
		{ PRI_TRANS_CAP_AUDIO_4ESS, "3.1khz audio (4ESS)" },
	};
	return code2str(mode, modes, sizeof(modes) / sizeof(modes[0]));
}

static char *mode2str(int mode)
{
	static struct msgtype modes[] = {
		{ TRANS_MODE_64_CIRCUIT, "64kbps, circuit-mode" },
		{ TRANS_MODE_2x64_CIRCUIT, "2x64kbps, circuit-mode" },
		{ TRANS_MODE_384_CIRCUIT, "384kbps, circuit-mode" },
		{ TRANS_MODE_1536_CIRCUIT, "1536kbps, circuit-mode" },
		{ TRANS_MODE_1920_CIRCUIT, "1920kbps, circuit-mode" },
		{ TRANS_MODE_MULTIRATE, "Multirate (Nx64kbps)" },
		{ TRANS_MODE_PACKET, "Packet Mode" },
	};
	return code2str(mode, modes, sizeof(modes) / sizeof(modes[0]));
}

static char *l12str(int proto)
{
	static struct msgtype protos[] = {
 		{ PRI_LAYER_1_ITU_RATE_ADAPT, "V.110 Rate Adaption" },
		{ PRI_LAYER_1_ULAW, "u-Law" },
		{ PRI_LAYER_1_ALAW, "A-Law" },
		{ PRI_LAYER_1_G721, "G.721 ADPCM" },
		{ PRI_LAYER_1_G722_G725, "G.722/G.725 7kHz Audio" },
 		{ PRI_LAYER_1_H223_H245, "H.223/H.245 Multimedia" },
		{ PRI_LAYER_1_NON_ITU_ADAPT, "Non-ITU Rate Adaption" },
		{ PRI_LAYER_1_V120_RATE_ADAPT, "V.120 Rate Adaption" },
		{ PRI_LAYER_1_X31_RATE_ADAPT, "X.31 Rate Adaption" },
	};
	return code2str(proto, protos, sizeof(protos) / sizeof(protos[0]));
}

static char *ra2str(int proto)
{
	static struct msgtype protos[] = {
		{ PRI_RATE_ADAPT_9K6, "9.6 kbit/s" },
	};
	return code2str(proto, protos, sizeof(protos) / sizeof(protos[0]));
}

static char *l22str(int proto)
{
	static struct msgtype protos[] = {
		{ LAYER_2_LAPB, "LAPB" },
	};
	return code2str(proto, protos, sizeof(protos) / sizeof(protos[0]));
}

static char *l32str(int proto)
{
	static struct msgtype protos[] = {
		{ LAYER_3_X25, "X.25" },
	};
	return code2str(proto, protos, sizeof(protos) / sizeof(protos[0]));
}

static char *int_rate2str(int proto)
{
    static struct msgtype protos[] = {
		{ PRI_INT_RATE_8K, "8 kbit/s" },
		{ PRI_INT_RATE_16K, "16 kbit/s" },
		{ PRI_INT_RATE_32K, "32 kbit/s" },
    };
    return code2str(proto, protos, sizeof(protos) / sizeof(protos[0]));
}

static void dump_bearer_capability(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int pos=2;
	pri_message(ctrl, "%c Bearer Capability (len=%2d) [ Ext: %d  Q.931 Std: %d  Info transfer capability: %s (%d)\n",
		prefix, len, (ie->data[0] & 0x80 ) >> 7, (ie->data[0] & 0x60) >> 5, cap2str(ie->data[0] & 0x1f), (ie->data[0] & 0x1f));
	pri_message(ctrl, "%c                              Ext: %d  Trans mode/rate: %s (%d)\n", prefix, (ie->data[1] & 0x80) >> 7, mode2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f);

	/* octet 4.1 exists iff mode/rate is multirate */
	if ((ie->data[1] & 0x7f) == 0x18) {
	    pri_message(ctrl, "%c                              Ext: %d  Transfer rate multiplier: %d x 64\n", prefix, (ie->data[2] & 0x80) >> 7, ie->data[2] & 0x7f);
		pos++;
	}

	/* don't count the IE num and length as part of the data */
	len -= 2;
	
	/* Look for octet 5; this is identified by bits 5,6 == 01 */
     	if (pos < len &&
		(ie->data[pos] & 0x60) == 0x20) {

		/* although the layer1 is only the bottom 5 bits of the byte,
		   previous versions of this library passed bits 5&6 through
		   too, so we have to do the same for binary compatability */
		u_int8_t layer1 = ie->data[pos] & 0x7f;

		pri_message(ctrl, "%c                                User information layer 1: %s (%d)\n",
		            prefix, l12str(layer1), layer1);
		pos++;
		
		/* octet 5a? */
		if (pos < len && !(ie->data[pos-1] & 0x80)) {
			int ra = ie->data[pos] & 0x7f;

			pri_message(ctrl, "%c                                Async: %d, Negotiation: %d, "
				"User rate: %s (%#x)\n", 
				prefix,
				ra & PRI_RATE_ADAPT_ASYNC ? 1 : 0,
				ra & PRI_RATE_ADAPT_NEGOTIATION_POSS ? 1 : 0,
				ra2str(ra & PRI_RATE_USER_RATE_MASK),
				ra & PRI_RATE_USER_RATE_MASK);
			pos++;
		}
		
		/* octet 5b? */
		if (pos < len && !(ie->data[pos-1] & 0x80)) {
			u_int8_t data = ie->data[pos];
			if (layer1 == PRI_LAYER_1_ITU_RATE_ADAPT) {
				pri_message(ctrl, "%c                                Intermediate rate: %s (%d), "
					"NIC on Tx: %d, NIC on Rx: %d, "
					"Flow control on Tx: %d, "
					"Flow control on Rx: %d\n",
					prefix, int_rate2str((data & 0x60)>>5),
					(data & 0x60)>>5,
					(data & 0x10)?1:0,
					(data & 0x08)?1:0,
					(data & 0x04)?1:0,
					(data & 0x02)?1:0);
			} else if (layer1 == PRI_LAYER_1_V120_RATE_ADAPT) {
				pri_message(ctrl, "%c                                Hdr: %d, Multiframe: %d, Mode: %d, "
					"LLI negot: %d, Assignor: %d, "
					"In-band neg: %d\n", prefix,
					(data & 0x40)?1:0,
					(data & 0x20)?1:0,
					(data & 0x10)?1:0,
					(data & 0x08)?1:0,
					(data & 0x04)?1:0,
					(data & 0x02)?1:0);
			} else {
				pri_message(ctrl, "%c                                Unknown octet 5b: 0x%x\n",
					prefix, data);
			}
			pos++;
		}

		/* octet 5c? */
		if (pos < len && !(ie->data[pos-1] & 0x80)) {
			u_int8_t data = ie->data[pos];
			const char *stop_bits[] = {"?","1","1.5","2"};
			const char *data_bits[] = {"?","5","7","8"};
			const char *parity[] = {"Odd","?","Even","None",
				       "zero","one","?","?"};
	
			pri_message(ctrl, "%c                                Stop bits: %s, data bits: %s, "
			    "parity: %s\n", prefix,
			    stop_bits[(data & 0x60) >> 5],
			    data_bits[(data & 0x18) >> 3],
			    parity[(data & 0x7)]);
	
			pos++;
		}
	
			/* octet 5d? */
		if (pos < len && !(ie->data[pos-1] & 0x80)) {
			u_int8_t data = ie->data[pos];
			pri_message(ctrl, "%c                                Duplex mode: %d, modem type: %d\n",
				prefix, (data & 0x40) ? 1 : 0,data & 0x3F);
 			pos++;
		}
 	}


	/* Look for octet 6; this is identified by bits 5,6 == 10 */
	if (pos < len && 
		(ie->data[pos] & 0x60) == 0x40) {
		pri_message(ctrl, "%c                                User information layer 2: %s (%d)\n",
			prefix, l22str(ie->data[pos] & 0x1f),
			ie->data[pos] & 0x1f);
		pos++;
	}

	/* Look for octet 7; this is identified by bits 5,6 == 11 */
	if (pos < len && (ie->data[pos] & 0x60) == 0x60) {
		pri_message(ctrl, "%c                                User information layer 3: %s (%d)\n",
			prefix, l32str(ie->data[pos] & 0x1f),
			ie->data[pos] & 0x1f);
		pos++;

		/* octets 7a and 7b? */
		if (pos + 1 < len && !(ie->data[pos-1] & 0x80) &&
			!(ie->data[pos] & 0x80)) {
			unsigned int proto;
			proto = ((ie->data[pos] & 0xF) << 4 ) | 
			         (ie->data[pos+1] & 0xF);

			pri_message(ctrl, "%c                                Network layer: 0x%x\n", prefix,
			            proto );
			pos += 2;
		}
	}
}

static int receive_bearer_capability(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int pos=2;
	if (ie->data[0] & 0x60) {
		pri_error(ctrl, "!! non-standard Q.931 standard field\n");
		return -1;
	}
	call->transcapability = ie->data[0] & 0x1f;
	call->transmoderate = ie->data[1] & 0x7f;
   
	/* octet 4.1 exists iff mode/rate is multirate */
	if (call->transmoderate == TRANS_MODE_MULTIRATE) {
		call->transmultiple = ie->data[pos++] & 0x7f;
	}

	/* Look for octet 5; this is identified by bits 5,6 == 01 */
	if (pos < len && 
	     (ie->data[pos] & 0x60) == 0x20 ) {
		/* although the layer1 is only the bottom 5 bits of the byte,
		   previous versions of this library passed bits 5&6 through
		   too, so we have to do the same for binary compatability */
		call->userl1 = ie->data[pos] & 0x7f;
		pos++;
		
		/* octet 5a? */
		if (pos < len && !(ie->data[pos-1] & 0x80)) {
			call->rateadaption = ie->data[pos] & 0x7f;
			pos++;
 		}
		
		/* octets 5b through 5d? */
		while (pos < len && !(ie->data[pos-1] & 0x80)) {
			pos++;
		}
		
	}

	/* Look for octet 6; this is identified by bits 5,6 == 10 */
     	if (pos < len && 
             (ie->data[pos] & 0x60) == 0x40) {
		call->userl2 = ie->data[pos++] & 0x1f;
	}

	/* Look for octet 7; this is identified by bits 5,6 == 11 */
     	if (pos < len && 
             (ie->data[pos] & 0x60) == 0x60) {
		call->userl3 = ie->data[pos++] & 0x1f;
	}
	return 0;
}

static int transmit_bearer_capability(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	int tc;
	int pos;

	/* We are ready to transmit single IE only */	
	if(order > 1)
		return 0;

	tc = call->transcapability;
	if (ctrl->subchannel && !ctrl->bri) {
		/* Bearer capability is *hard coded* in GR-303 */
		ie->data[0] = 0x88;
		ie->data[1] = 0x90;
		return 4;
	}
	
	if (call->justsignalling) {
		ie->data[0] = 0xa8;
		ie->data[1] = 0x80;
		return 4;
	}
	
	ie->data[0] = 0x80 | tc;
	ie->data[1] = call->transmoderate | 0x80;

 	pos = 2;
 	/* octet 4.1 exists iff mode/rate is multirate */
 	if (call->transmoderate == TRANS_MODE_MULTIRATE ) {
 		ie->data[pos++] = call->transmultiple | 0x80;
	}

	if ((tc & PRI_TRANS_CAP_DIGITAL) && (ctrl->switchtype == PRI_SWITCH_EUROISDN_E1) &&
		(call->transmoderate == TRANS_MODE_PACKET)) {
		/* Apparently EuroISDN switches don't seem to like user layer 2/3 */
		return 4;
	}

	if ((tc & PRI_TRANS_CAP_DIGITAL) && (call->transmoderate == TRANS_MODE_64_CIRCUIT)) {
		/* Unrestricted digital 64k data calls don't use user layer 2/3 */
		return 4;
	}

	if (call->transmoderate != TRANS_MODE_PACKET) {
		/* If you have an AT&T 4ESS, you don't send any more info */
		if ((ctrl->switchtype != PRI_SWITCH_ATT4ESS) && (call->userl1 > -1)) {
			ie->data[pos++] = call->userl1 | 0x80; /* XXX Ext bit? XXX */
			if (call->userl1 == PRI_LAYER_1_ITU_RATE_ADAPT) {
				ie->data[pos++] = call->rateadaption | 0x80;
			}
			return pos + 2;
 		}
 
 		ie->data[pos++] = 0xa0 | (call->userl1 & 0x1f);
 
 		if (call->userl1 == PRI_LAYER_1_ITU_RATE_ADAPT) {
 		    ie->data[pos-1] &= ~0x80; /* clear EXT bit in octet 5 */
 		    ie->data[pos++] = call->rateadaption | 0x80;
 		}
 	}
 	
 	
 	if (call->userl2 != -1)
 		ie->data[pos++] = 0xc0 | (call->userl2 & 0x1f);
 
 	if (call->userl3 != -1)
 		ie->data[pos++] = 0xe0 | (call->userl3 & 0x1f);
 
 	return pos + 2;
}

char *pri_plan2str(int plan)
{
	static struct msgtype plans[] = {
		{ PRI_INTERNATIONAL_ISDN, "International number in ISDN" },
		{ PRI_NATIONAL_ISDN, "National number in ISDN" },
		{ PRI_LOCAL_ISDN, "Local number in ISDN" },
		{ PRI_PRIVATE, "Private numbering plan" },
		{ PRI_UNKNOWN, "Unknown numbering plan" },
	};
	return code2str(plan, plans, sizeof(plans) / sizeof(plans[0]));
}

static char *npi2str(int plan)
{
	static struct msgtype plans[] = {
		{ PRI_NPI_UNKNOWN, "Unknown Number Plan" },
		{ PRI_NPI_E163_E164, "ISDN/Telephony Numbering Plan (E.164/E.163)" },
		{ PRI_NPI_X121, "Data Numbering Plan (X.121)" },
		{ PRI_NPI_F69, "Telex Numbering Plan (F.69)" },
		{ PRI_NPI_NATIONAL, "National Standard Numbering Plan" },
		{ PRI_NPI_PRIVATE, "Private Numbering Plan" },
		{ PRI_NPI_RESERVED, "Reserved Number Plan" },
	};
	return code2str(plan, plans, sizeof(plans) / sizeof(plans[0]));
}

static char *ton2str(int plan)
{
	static struct msgtype plans[] = {
		{ PRI_TON_UNKNOWN, "Unknown Number Type" },
		{ PRI_TON_INTERNATIONAL, "International Number" },
		{ PRI_TON_NATIONAL, "National Number" },
		{ PRI_TON_NET_SPECIFIC, "Network Specific Number" },
		{ PRI_TON_SUBSCRIBER, "Subscriber Number" },
		{ PRI_TON_ABBREVIATED, "Abbreviated number" },
		{ PRI_TON_RESERVED, "Reserved Number" },
	};
	return code2str(plan, plans, sizeof(plans) / sizeof(plans[0]));
}

static char *subaddrtype2str(int plan)
{
	static struct msgtype plans[] = {
		{ 0, "NSAP (X.213/ISO 8348 AD2)" },
		{ 2, "User Specified" },
	};
	return code2str(plan, plans, sizeof(plans) / sizeof(plans[0]));
}

char *pri_pres2str(int pres)
{
	static struct msgtype press[] = {
		{ PRES_ALLOWED_USER_NUMBER_NOT_SCREENED, "Presentation permitted, user number not screened" },
		{ PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN, "Presentation permitted, user number passed network screening" },
		{ PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN, "Presentation permitted, user number failed network screening" },
		{ PRES_ALLOWED_NETWORK_NUMBER, "Presentation allowed of network provided number" },
		{ PRES_PROHIB_USER_NUMBER_NOT_SCREENED, "Presentation prohibited, user number not screened" },
		{ PRES_PROHIB_USER_NUMBER_PASSED_SCREEN, "Presentation prohibited, user number passed network screening" },
		{ PRES_PROHIB_USER_NUMBER_FAILED_SCREEN, "Presentation prohibited, user number failed network screening" },
		{ PRES_PROHIB_NETWORK_NUMBER, "Presentation prohibited of network provided number" },
		{ PRES_NUMBER_NOT_AVAILABLE, "Number not available" },
	};
	return code2str(pres, press, sizeof(press) / sizeof(press[0]));
}

static void q931_get_number(unsigned char *num, int maxlen, unsigned char *src, int len)
{
	if ((len < 0) || (len > maxlen - 1)) {
		num[0] = 0;
		return;
	}
	memcpy(num, src, len);
	num[len] = 0;
}

static void dump_called_party_number(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];

	q931_get_number(cnum, sizeof(cnum), ie->data + 1, len - 3);
	pri_message(ctrl, "%c Called Number (len=%2d) [ Ext: %d  TON: %s (%d)  NPI: %s (%d)  '%s' ]\n",
		prefix, len, ie->data[0] >> 7, ton2str((ie->data[0] >> 4) & 0x07), (ie->data[0] >> 4) & 0x07, npi2str(ie->data[0] & 0x0f), ie->data[0] & 0x0f, cnum);
}

static void dump_called_party_subaddr(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];
	q931_get_number(cnum, sizeof(cnum), ie->data + 1, len - 3);
	pri_message(ctrl, "%c Called Sub-Address (len=%2d) [ Ext: %d  Type: %s (%d)  O: %d  '%s' ]\n",
		prefix, len, ie->data[0] >> 7,
		subaddrtype2str((ie->data[0] & 0x70) >> 4), (ie->data[0] & 0x70) >> 4,
		(ie->data[0] & 0x08) >> 3, cnum);
}

static void dump_calling_party_number(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];
	if (ie->data[0] & 0x80)
		q931_get_number(cnum, sizeof(cnum), ie->data + 1, len - 3);
	else
		q931_get_number(cnum, sizeof(cnum), ie->data + 2, len - 4);
	pri_message(ctrl, "%c Calling Number (len=%2d) [ Ext: %d  TON: %s (%d)  NPI: %s (%d)\n", prefix, len, ie->data[0] >> 7, ton2str((ie->data[0] >> 4) & 0x07), (ie->data[0] >> 4) & 0x07, npi2str(ie->data[0] & 0x0f), ie->data[0] & 0x0f);
	if (ie->data[0] & 0x80)
		pri_message(ctrl, "%c                           Presentation: %s (%d)  '%s' ]\n", prefix, pri_pres2str(0), 0, cnum);
	else
		pri_message(ctrl, "%c                           Presentation: %s (%d)  '%s' ]\n", prefix, pri_pres2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f, cnum);
}

static void dump_calling_party_subaddr(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];
	q931_get_number(cnum, sizeof(cnum), ie->data + 1, len - 3);
	pri_message(ctrl, "%c Calling Sub-Address (len=%2d) [ Ext: %d  Type: %s (%d)  O: %d  '%s' ]\n",
		prefix, len, ie->data[0] >> 7,
		subaddrtype2str((ie->data[0] & 0x70) >> 4), (ie->data[0] & 0x70) >> 4,
		(ie->data[0] & 0x08) >> 3, cnum);
}

static void dump_redirecting_number(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];
	int i = 0;
	/* To follow Q.931 (4.5.1), we must search for start of octet 4 by
	   walking through all bytes until one with ext bit (8) set to 1 */
	do {
		switch(i) {
		case 0:	/* Octet 3 */
			pri_message(ctrl, "%c Redirecting Number (len=%2d) [ Ext: %d  TON: %s (%d)  NPI: %s (%d)",
				prefix, len, ie->data[0] >> 7, ton2str((ie->data[0] >> 4) & 0x07), (ie->data[0] >> 4) & 0x07, npi2str(ie->data[0] & 0x0f), ie->data[0] & 0x0f);
			break;
		case 1: /* Octet 3a */
			pri_message(ctrl, "\n%c                               Ext: %d  Presentation: %s (%d)",
				prefix, ie->data[1] >> 7, pri_pres2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f);
			break;
		case 2: /* Octet 3b */
			pri_message(ctrl, "\n%c                               Ext: %d  Reason: %s (%d)",
				prefix, ie->data[2] >> 7, redirection_reason2str(ie->data[2] & 0x7f), ie->data[2] & 0x7f);
			break;
		}
	} while(!(ie->data[i++]& 0x80));
	q931_get_number(cnum, sizeof(cnum), ie->data + i, ie->len - i);
	pri_message(ctrl, "  '%s' ]\n", cnum);
}

static void dump_redirection_number(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];
	int i = 0;
	/* To follow Q.931 (4.5.1), we must search for start of octet 4 by
	   walking through all bytes until one with ext bit (8) set to 1 */
	do {
		switch (i) {
		case 0:	/* Octet 3 */
			pri_message(ctrl,
				"%c Redirection Number (len=%2d) [ Ext: %d  TON: %s (%d)  NPI: %s (%d)",
				prefix, len, ie->data[0] >> 7,
				ton2str((ie->data[0] >> 4) & 0x07), (ie->data[0] >> 4) & 0x07,
				npi2str(ie->data[0] & 0x0f), ie->data[0] & 0x0f);
			break;
		case 1: /* Octet 3a */
			pri_message(ctrl, "\n%c                               Ext: %d  Presentation: %s (%d)",
				prefix, ie->data[1] >> 7, pri_pres2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f);
			break;
		}
	} while (!(ie->data[i++] & 0x80));
	q931_get_number(cnum, sizeof(cnum), ie->data + i, ie->len - i);
	pri_message(ctrl, "  '%s' ]\n", cnum);
}

static int receive_connected_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int i = 0;

	call->remote_id.number.valid = 1;
	call->remote_id.number.presentation =
		PRI_PRES_ALLOWED | PRI_PRES_USER_NUMBER_UNSCREENED;
	/* To follow Q.931 (4.5.1), we must search for start of octet 4 by
	   walking through all bytes until one with ext bit (8) set to 1 */
	do {
		switch (i) {
		case 0:
			call->remote_id.number.plan = ie->data[i] & 0x7f;
			break;
		case 1:
			/* Keep only the presentation and screening fields */
			call->remote_id.number.presentation =
				ie->data[i] & (PRI_PRES_RESTRICTION | PRI_PRES_NUMBER_TYPE);
			break;
		}
	} while (!(ie->data[i++] & 0x80));
	q931_get_number((unsigned char *) call->remote_id.number.str, sizeof(call->remote_id.number.str), ie->data + i, ie->len - i);

	return 0;
}

static int transmit_connected_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	size_t datalen;

	if (!call->local_id.number.valid) {
		return 0;
	}

	datalen = strlen(call->local_id.number.str);
	ie->data[0] = call->local_id.number.plan;
	ie->data[1] = 0x80 | call->local_id.number.presentation;
	memcpy(ie->data + 2, call->local_id.number.str, datalen);
	return datalen + (2 + 2);
}

static void dump_connected_number(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];
	int i = 0;
	/* To follow Q.931 (4.5.1), we must search for start of octet 4 by
	   walking through all bytes until one with ext bit (8) set to 1 */
	do {
		switch(i) {
		case 0:	/* Octet 3 */
			pri_message(ctrl, "%c Connected Number (len=%2d) [ Ext: %d  TON: %s (%d)  NPI: %s (%d)",
				prefix, len, ie->data[0] >> 7, ton2str((ie->data[0] >> 4) & 0x07), (ie->data[0] >> 4) & 0x07, npi2str(ie->data[0] & 0x0f), ie->data[0] & 0x0f);
			break;
		case 1: /* Octet 3a */
			pri_message(ctrl, "\n%c                             Ext: %d  Presentation: %s (%d)",
				prefix, ie->data[1] >> 7, pri_pres2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f);
			break;
		}
	} while(!(ie->data[i++]& 0x80));
	q931_get_number(cnum, sizeof(cnum), ie->data + i, ie->len - i);
	pri_message(ctrl, "  '%s' ]\n", cnum);
}


static int receive_redirecting_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int i = 0;

	call->redirecting.from.number.valid = 1;
	call->redirecting.from.number.presentation =
		PRI_PRES_ALLOWED | PRI_PRES_USER_NUMBER_UNSCREENED;
	call->redirecting.reason = PRI_REDIR_UNKNOWN;
	/* To follow Q.931 (4.5.1), we must search for start of octet 4 by
	   walking through all bytes until one with ext bit (8) set to 1 */
	do {
		switch (i) {
		case 0:
			call->redirecting.from.number.plan = ie->data[i] & 0x7f;
			break;
		case 1:
			/* Keep only the presentation and screening fields */
			call->redirecting.from.number.presentation =
				ie->data[i] & (PRI_PRES_RESTRICTION | PRI_PRES_NUMBER_TYPE);
			break;
		case 2:
			call->redirecting.reason = ie->data[i] & 0x0f;
			break;
		}
	} while (!(ie->data[i++] & 0x80));
	q931_get_number((unsigned char *) call->redirecting.from.number.str, sizeof(call->redirecting.from.number.str), ie->data + i, ie->len - i);
	return 0;
}

static int transmit_redirecting_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	size_t datalen;

	if (order > 1)
		return 0;
	if (!call->redirecting.from.number.valid) {
		return 0;
	}

	datalen = strlen(call->redirecting.from.number.str);
	ie->data[0] = call->redirecting.from.number.plan;
#if 1
	/* ETSI and Q.952 do not define the screening field */
	ie->data[1] = call->redirecting.from.number.presentation & PRI_PRES_RESTRICTION;
#else
	/* Q.931 defines the screening field */
	ie->data[1] = call->redirecting.from.number.presentation;
#endif
	ie->data[2] = (call->redirecting.reason & 0x0f) | 0x80;
	memcpy(ie->data + 3, call->redirecting.from.number.str, datalen);
	return datalen + (3 + 2);
}

static void dump_redirecting_subaddr(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	unsigned char cnum[256];
	q931_get_number(cnum, sizeof(cnum), ie->data + 2, len - 4);
	pri_message(ctrl, "%c Redirecting Sub-Address (len=%2d) [ Ext: %d  Type: %s (%d)  O: %d  '%s' ]\n",
		prefix, len, ie->data[0] >> 7,
		subaddrtype2str((ie->data[0] & 0x70) >> 4), (ie->data[0] & 0x70) >> 4,
		(ie->data[0] & 0x08) >> 3, cnum);
}

static int receive_redirection_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int i = 0;

	call->redirection_number.valid = 1;
	call->redirection_number.presentation =
		PRI_PRES_ALLOWED | PRI_PRES_USER_NUMBER_UNSCREENED;
	/* To follow Q.931 (4.5.1), we must search for start of octet 4 by
	   walking through all bytes until one with ext bit (8) set to 1 */
	do {
		switch (i) {
		case 0:
			call->redirection_number.plan = ie->data[i] & 0x7f;
			break;
		case 1:
			/* Keep only the presentation and screening fields */
			call->redirection_number.presentation =
				ie->data[i] & (PRI_PRES_RESTRICTION | PRI_PRES_NUMBER_TYPE);
			break;
		}
	} while (!(ie->data[i++] & 0x80));
	q931_get_number((unsigned char *) call->redirection_number.str, sizeof(call->redirection_number.str), ie->data + i, ie->len - i);
	return 0;
}

static int transmit_redirection_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	size_t datalen;

	if (order > 1) {
		return 0;
	}
	if (!call->redirection_number.valid) {
		return 0;
	}

	datalen = strlen(call->redirection_number.str);
	ie->data[0] = call->redirection_number.plan;
	ie->data[1] = (call->redirection_number.presentation & PRI_PRES_RESTRICTION) | 0x80;
	memcpy(ie->data + 2, call->redirection_number.str, datalen);
	return datalen + (2 + 2);
}

static int receive_calling_party_subaddr(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	/* copy digits to call->callingsubaddr */
 	q931_get_number((unsigned char *) call->callingsubaddr, sizeof(call->callingsubaddr), ie->data + 1, len - 3);
	return 0;
}

static int receive_called_party_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	size_t called_len;
	size_t max_len;
	char *called_end;

	if (len < 3) {
		return -1;
	}

	call->called.number.valid = 1;
	call->called.number.plan = ie->data[0] & 0x7f;
	if (msgtype == Q931_SETUP) {
		q931_get_number((unsigned char *) call->called.number.str,
			sizeof(call->called.number.str), ie->data + 1, len - 3);
	} else if (call->ourcallstate == Q931_CALL_STATE_OVERLAP_RECEIVING) {
		/*
		 * Since we are receiving overlap digits now, we need to append
		 * them to any previously received digits in call->called.number.str.
		 */
		called_len = strlen(call->called.number.str);
		called_end = call->called.number.str + called_len;
		max_len = (sizeof(call->called.number.str) - 1) - called_len;
		if (max_len < len - 3) {
			called_len = max_len;
		} else {
			called_len = len - 3;
		}
		strncat(called_end, (char *) ie->data + 1, called_len);
	}

	q931_get_number((unsigned char *) call->overlap_digits, sizeof(call->overlap_digits),
		ie->data + 1, len - 3);
	return 0;
}

static int transmit_called_party_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	size_t datalen;

	if (!call->called.number.valid) {
		return 0;
	}

	datalen = strlen(call->overlap_digits);
	ie->data[0] = 0x80 | call->called.number.plan;
	memcpy(ie->data + 1, call->overlap_digits, datalen);
	return datalen + (1 + 2);
}

static int receive_calling_party_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int i = 0;

	call->remote_id.number.valid = 1;
	call->remote_id.number.presentation =
		PRI_PRES_ALLOWED | PRI_PRES_USER_NUMBER_UNSCREENED;
	/* To follow Q.931 (4.5.1), we must search for start of octet 4 by
	   walking through all bytes until one with ext bit (8) set to 1 */
	do {
		switch (i) {
		case 0:
			call->remote_id.number.plan = ie->data[i] & 0x7f;
			break;
		case 1:
			/* Keep only the presentation and screening fields */
			call->remote_id.number.presentation =
				ie->data[i] & (PRI_PRES_RESTRICTION | PRI_PRES_NUMBER_TYPE);
			break;
		}
	} while (!(ie->data[i++] & 0x80));
	q931_get_number((unsigned char *) call->remote_id.number.str,
		sizeof(call->remote_id.number.str), ie->data + i, ie->len - i);

	return 0;
}

static int transmit_calling_party_number(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	size_t datalen;

	if (!call->local_id.number.valid) {
		return 0;
	}

	datalen = strlen(call->local_id.number.str);
	ie->data[0] = call->local_id.number.plan;
	ie->data[1] = 0x80 | call->local_id.number.presentation;
	memcpy(ie->data + 2, call->local_id.number.str, datalen);
	return datalen + (2 + 2);
}

static void dump_user_user(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int x;
	pri_message(ctrl, "%c User-User Information (len=%2d) [", prefix, len);
	for (x=0;x<ie->len;x++)
		pri_message(ctrl, " %02x", ie->data[x] & 0x7f);
	pri_message(ctrl, " ]\n");
}


static int receive_user_user(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{        
        call->useruserprotocoldisc = ie->data[0] & 0xff;
        if (call->useruserprotocoldisc == 4) /* IA5 */
          q931_get_number((unsigned char *) call->useruserinfo, sizeof(call->useruserinfo), ie->data + 1, len - 3);
	return 0;
}

static int transmit_user_user(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{        
	int datalen = strlen(call->useruserinfo);
	if (datalen > 0) {
		/* Restricted to 35 characters */
		if (msgtype == Q931_USER_INFORMATION) {
			if (datalen > 260)
				datalen = 260;
		} else {
			if (datalen > 35)
				datalen = 35;
		}
		ie->data[0] = 4; /* IA5 characters */
		memcpy(&ie->data[1], call->useruserinfo, datalen);
		call->useruserinfo[0] = '\0';
		return datalen + 3;
	}

	return 0;
}

static void dump_change_status(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int x;
	
	pri_message(ctrl, "%c Change Status Information (len=%2d) [", prefix, len);
	for (x=0; x<ie->len; x++) {
		pri_message(ctrl, " %02x", ie->data[x] & 0x7f);
	}
	pri_message(ctrl, " ]\n");
}

static int receive_change_status(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->changestatus = ie->data[0] & 0x0f;
	return 0;
}

static int transmit_change_status(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	ie->data[0] = 0xc0 | call->changestatus;
	return 3;
}

static char *prog2str(int prog)
{
	static struct msgtype progs[] = {
		{ Q931_PROG_CALL_NOT_E2E_ISDN, "Call is not end-to-end ISDN; further call progress information may be available inband." },
		{ Q931_PROG_CALLED_NOT_ISDN, "Called equipment is non-ISDN." },
		{ Q931_PROG_CALLER_NOT_ISDN, "Calling equipment is non-ISDN." },
		{ Q931_PROG_INBAND_AVAILABLE, "Inband information or appropriate pattern now available." },
		{ Q931_PROG_DELAY_AT_INTERF, "Delay in response at called Interface." },
		{ Q931_PROG_INTERWORKING_WITH_PUBLIC, "Interworking with a public network." },
		{ Q931_PROG_INTERWORKING_NO_RELEASE, "Interworking with a network unable to supply a release signal." },
		{ Q931_PROG_INTERWORKING_NO_RELEASE_PRE_ANSWER, "Interworking with a network unable to supply a release signal before answer." },
		{ Q931_PROG_INTERWORKING_NO_RELEASE_POST_ANSWER, "Interworking with a network unable to supply a release signal after answer." },
	};
	return code2str(prog, progs, sizeof(progs) / sizeof(progs[0]));
}

static char *coding2str(int cod)
{
	static struct msgtype cods[] = {
		{ CODE_CCITT, "CCITT (ITU) standard" },
		{ CODE_INTERNATIONAL, "Non-ITU international standard" }, 
		{ CODE_NATIONAL, "National standard" }, 
		{ CODE_NETWORK_SPECIFIC, "Network specific standard" },
	};
	return code2str(cod, cods, sizeof(cods) / sizeof(cods[0]));
}

static char *loc2str(int loc)
{
	static struct msgtype locs[] = {
		{ LOC_USER, "User" },
		{ LOC_PRIV_NET_LOCAL_USER, "Private network serving the local user" },
		{ LOC_PUB_NET_LOCAL_USER, "Public network serving the local user" },
		{ LOC_TRANSIT_NET, "Transit network" },
		{ LOC_PUB_NET_REMOTE_USER, "Public network serving the remote user" },
		{ LOC_PRIV_NET_REMOTE_USER, "Private network serving the remote user" },
		{ LOC_INTERNATIONAL_NETWORK, "International network" },
		{ LOC_NETWORK_BEYOND_INTERWORKING, "Network beyond the interworking point" },
	};
	return code2str(loc, locs, sizeof(locs) / sizeof(locs[0]));
}

static void dump_progress_indicator(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Progress Indicator (len=%2d) [ Ext: %d  Coding: %s (%d)  0: %d  Location: %s (%d)\n",
		prefix, len, ie->data[0] >> 7, coding2str((ie->data[0] & 0x60) >> 5), (ie->data[0] & 0x60) >> 5,
		(ie->data[0] & 0x10) >> 4, loc2str(ie->data[0] & 0xf), ie->data[0] & 0xf);
	pri_message(ctrl, "%c                               Ext: %d  Progress Description: %s (%d) ]\n",
		prefix, ie->data[1] >> 7, prog2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f);
}

static int receive_display(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	unsigned char *data;

	call->remote_id.name.valid = 1;

	data = ie->data;
	if (data[0] & 0x80) {
		/* Skip over character set */
		data++;
		len--;
	}
	call->remote_id.name.char_set = PRI_CHAR_SET_ISO8859_1;

	q931_get_number((unsigned char *) call->remote_id.name.str, sizeof(call->remote_id.name.str), data, len - 2);
	if (call->remote_id.name.str[0]) {
		call->remote_id.name.presentation = PRI_PRES_ALLOWED;
	} else {
		call->remote_id.name.presentation = PRI_PRES_RESTRICTED;
	}
	return 0;
}

static int transmit_display(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	size_t datalen;
	int i;

	i = 0;

	if (!call->local_id.name.valid || !call->local_id.name.str[0]) {
		return 0;
	}
	switch (ctrl->switchtype) {
	case PRI_SWITCH_QSIG:
		/* Q.SIG supports names */
		return 0;
	case PRI_SWITCH_EUROISDN_E1:
	case PRI_SWITCH_EUROISDN_T1:
		if (ctrl->localtype == PRI_CPE) {
			return 0;
		}
		break;
	default:
		/* Prefix name with character set indicator. */
		ie->data[0] = 0xb1;
		++i;
		break;
	}

	datalen = strlen(call->local_id.name.str);
	memcpy(ie->data + i, call->local_id.name.str, datalen);
	return 2 + i + datalen;
}

static int receive_progress_indicator(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->progloc = ie->data[0] & 0xf;
	call->progcode = (ie->data[0] & 0x60) >> 5;
	switch (call->progress = (ie->data[1] & 0x7f)) {
	case Q931_PROG_CALL_NOT_E2E_ISDN:
		call->progressmask |= PRI_PROG_CALL_NOT_E2E_ISDN;
		break;
	case Q931_PROG_CALLED_NOT_ISDN:
		call->progressmask |= PRI_PROG_CALLED_NOT_ISDN;
		break;
	case Q931_PROG_CALLER_NOT_ISDN:
		call->progressmask |= PRI_PROG_CALLER_NOT_ISDN;
		break;
	case Q931_PROG_CALLER_RETURNED_TO_ISDN:
		call->progressmask |= PRI_PROG_CALLER_RETURNED_TO_ISDN;
		break;
	case Q931_PROG_INBAND_AVAILABLE:
		call->progressmask |= PRI_PROG_INBAND_AVAILABLE;
		break;
	case Q931_PROG_DELAY_AT_INTERF:
		call->progressmask |= PRI_PROG_DELAY_AT_INTERF;
		break;
	case Q931_PROG_INTERWORKING_WITH_PUBLIC:
		call->progressmask |= PRI_PROG_INTERWORKING_WITH_PUBLIC;
		break;
	case Q931_PROG_INTERWORKING_NO_RELEASE:
		call->progressmask |= PRI_PROG_INTERWORKING_NO_RELEASE;
		break;
	case Q931_PROG_INTERWORKING_NO_RELEASE_PRE_ANSWER:
		call->progressmask |= PRI_PROG_INTERWORKING_NO_RELEASE_PRE_ANSWER;
		break;
	case Q931_PROG_INTERWORKING_NO_RELEASE_POST_ANSWER:
		call->progressmask |= PRI_PROG_INTERWORKING_NO_RELEASE_POST_ANSWER;
		break;
	default:
		pri_error(ctrl, "XXX Invalid Progress indicator value received: %02x\n",(ie->data[1] & 0x7f));
		break;
	}
	return 0;
}

static int transmit_facility(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	struct apdu_event *tmp;
	int i = 0;

	for (tmp = call->apdus; tmp; tmp = tmp->next) {
		if ((tmp->message == msgtype) && !tmp->sent)
			break;
	}
	if (!tmp)	/* No APDU found */
		return 0;

	if (ctrl->debug & PRI_DEBUG_APDU) {
		pri_message(ctrl, "Adding facility ie contents to send in %s message:\n",
			msg2str(msgtype));
		facility_decode_dump(ctrl, tmp->apdu, tmp->apdu_len);
	}

	if (tmp->apdu_len > 235) { /* TODO: find out how much space we can use */
		pri_message(ctrl, "Requested APDU (%d bytes) is too long\n", tmp->apdu_len);
		return 0;
	}

	memcpy(&ie->data[i], tmp->apdu, tmp->apdu_len);
	i += tmp->apdu_len;
	tmp->sent = 1;

	return i + 2;
}

static int receive_facility(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	struct fac_extension_header header;
	struct rose_message rose;
	const unsigned char *pos;
	const unsigned char *end;

	pos = ie->data;
	end = ie->data + ie->len;

	/* Make sure we have enough room for the protocol profile ie octet(s) */
	if (end < pos + 2) {
		return -1;
	}
	switch (*pos & Q932_PROTOCOL_MASK) {
	case Q932_PROTOCOL_ROSE:
	case Q932_PROTOCOL_EXTENSIONS:
		break;
	default:
	case Q932_PROTOCOL_CMIP:
	case Q932_PROTOCOL_ACSE:
		if (ctrl->debug & PRI_DEBUG_APDU) {
			pri_message(ctrl,
				"!! Don't know how to handle Q.932 Protocol Profile type 0x%X\n",
				*pos & Q932_PROTOCOL_MASK);
		}
		return -1;
	}
	if (!(*pos & 0x80)) {
		/* DMS-100 Service indicator octet - Just ignore for now */
		++pos;
	}
	++pos;

	if (ctrl->debug & PRI_DEBUG_APDU) {
		asn1_dump(ctrl, pos, end);
	}

	pos = fac_dec_extension_header(ctrl, pos, end, &header);
	if (!pos) {
		return -1;
	}
	if (header.npp_present) {
		if (ctrl->debug & PRI_DEBUG_APDU) {
			pri_message(ctrl,
				"!! Don't know how to handle Network Protocol Profile type 0x%X\n",
				header.npp);
		}
		return -1;
	}

	pos = rose_decode(ctrl, pos, end, &rose);
	if (!pos) {
		return -1;
	}
	switch (rose.type) {
	case ROSE_COMP_TYPE_INVOKE:
		rose_handle_invoke(ctrl, call, ie, &header, &rose.component.invoke);
		break;
	case ROSE_COMP_TYPE_RESULT:
		rose_handle_result(ctrl, call, ie, &header, &rose.component.result);
		break;
	case ROSE_COMP_TYPE_ERROR:
		rose_handle_error(ctrl, call, ie, &header, &rose.component.error);
		break;
	case ROSE_COMP_TYPE_REJECT:
		rose_handle_reject(ctrl, call, ie, &header, &rose.component.reject);
		break;
	default:
		return -1;
	}
	return 0;
}

static int transmit_progress_indicator(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	int code, mask;
	/* Can't send progress indicator on GR-303 -- EVER! */
	if (ctrl->subchannel && !ctrl->bri)
		return 0;
	if (call->progressmask > 0) {
		if (call->progressmask & (mask = PRI_PROG_CALL_NOT_E2E_ISDN))
			code = Q931_PROG_CALL_NOT_E2E_ISDN;
		else if (call->progressmask & (mask = PRI_PROG_CALLED_NOT_ISDN))
			code = Q931_PROG_CALLED_NOT_ISDN;
		else if (call->progressmask & (mask = PRI_PROG_CALLER_NOT_ISDN))
			code = Q931_PROG_CALLER_NOT_ISDN;
		else if (call->progressmask & (mask = PRI_PROG_INBAND_AVAILABLE))
			code = Q931_PROG_INBAND_AVAILABLE;
		else if (call->progressmask & (mask = PRI_PROG_DELAY_AT_INTERF))
			code = Q931_PROG_DELAY_AT_INTERF;
		else if (call->progressmask & (mask = PRI_PROG_INTERWORKING_WITH_PUBLIC))
			code = Q931_PROG_INTERWORKING_WITH_PUBLIC;
		else if (call->progressmask & (mask = PRI_PROG_INTERWORKING_NO_RELEASE))
			code = Q931_PROG_INTERWORKING_NO_RELEASE;
		else if (call->progressmask & (mask = PRI_PROG_INTERWORKING_NO_RELEASE_PRE_ANSWER))
			code = Q931_PROG_INTERWORKING_NO_RELEASE_PRE_ANSWER;
		else if (call->progressmask & (mask = PRI_PROG_INTERWORKING_NO_RELEASE_POST_ANSWER))
			code = Q931_PROG_INTERWORKING_NO_RELEASE_POST_ANSWER;
		else {
			code = 0;
			pri_error(ctrl, "XXX Undefined progress bit: %x\n", call->progressmask);
		}
		if (code) {
			ie->data[0] = 0x80 | (call->progcode << 5)  | (call->progloc);
			ie->data[1] = 0x80 | code;
			call->progressmask &= ~mask;
			return 4;
		}
	}
	/* Leave off */
	return 0;
}
static int transmit_call_state(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	if (call->ourcallstate > -1 ) {
		ie->data[0] = call->ourcallstate;
		return 3;
	}
	return 0;
}

static int receive_call_state(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->sugcallstate = ie->data[0] & 0x3f;
	return 0;
}

/*!
 * \brief Convert the internal Q.931 call state to a string.
 *
 * \param callstate Internal Q.931 call state.
 *
 * \return String equivalent of the given Q.931 call state.
 */
const char *q931_call_state_str(int callstate)
{
	static struct msgtype callstates[] = {
		{ 0, "Null" },
		{ 1, "Call Initiated" },
		{ 2, "Overlap sending" },
		{ 3, "Outgoing call  Proceeding" },
		{ 4, "Call Delivered" },
		{ 6, "Call Present" },
		{ 7, "Call Received" },
		{ 8, "Connect Request" },
		{ 9, "Incoming Call Proceeding" },
		{ 10, "Active" },
		{ 11, "Disconnect Request" },
		{ 12, "Disconnect Indication" },
		{ 15, "Suspend Request" },
		{ 17, "Resume Request" },
		{ 19, "Release Request" },
		{ 22, "Call Abort" },
		{ 25, "Overlap Receiving" },
		{ 61, "Restart Request" },
		{ 62, "Restart" },
	};
	return code2str(callstate, callstates, sizeof(callstates) / sizeof(callstates[0]));
}

static void dump_call_state(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Call State (len=%2d) [ Ext: %d  Coding: %s (%d)  Call state: %s (%d)\n",
		prefix, len, ie->data[0] >> 7, coding2str((ie->data[0] & 0xC0) >> 6), (ie->data[0] & 0xC0) >> 6,
		q931_call_state_str(ie->data[0] & 0x3f), ie->data[0] & 0x3f);
}

static void dump_call_identity(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int x;
	pri_message(ctrl, "%c Call Identity (len=%2d) [ ", prefix, len);
	for (x=0;x<ie->len;x++) 
		pri_message(ctrl, "0x%02X ", ie->data[x]);
	pri_message(ctrl, " ]\n");
}

static void dump_time_date(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Time Date (len=%2d) [ ", prefix, len);
	if (ie->len > 0)
		pri_message(ctrl, "%02d", ie->data[0]);
	if (ie->len > 1)
		pri_message(ctrl, "-%02d", ie->data[1]);
	if (ie->len > 2)
		pri_message(ctrl, "-%02d", ie->data[2]);
	if (ie->len > 3)
		pri_message(ctrl, " %02d", ie->data[3]);
	if (ie->len > 4)
		pri_message(ctrl, ":%02d", ie->data[4]);
	if (ie->len > 5)
		pri_message(ctrl, ":%02d", ie->data[5]);
	pri_message(ctrl, " ]\n");
}

static void dump_keypad_facility(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	char tmp[64];
	
	if (ie->len == 0 || ie->len > sizeof(tmp))
		return;
	
	memcpy(tmp, ie->data, ie->len);
	tmp[ie->len] = '\0';
	pri_message(ctrl, "%c Keypad Facility (len=%2d) [ %s ]\n", prefix, ie->len, tmp );
}

static int receive_keypad_facility(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int mylen;

	if (ie->len == 0)
		return -1;

	if (ie->len > (sizeof(call->keypad_digits) - 1))
		mylen = (sizeof(call->keypad_digits) - 1);
	else
		mylen = ie->len;

	memcpy(call->keypad_digits, ie->data, mylen);
	call->keypad_digits[mylen] = 0;

	return 0;
}

static int transmit_keypad_facility(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	int sublen;

	sublen = strlen(call->keypad_digits);
	if (sublen) {
		libpri_copy_string((char *) ie->data, call->keypad_digits, sizeof(call->keypad_digits));
		return sublen + 2;
	} else
		return 0;
}

static void dump_display(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int x, y;
	char *buf = malloc(len + 1);
	char tmp[80] = "";
	if (buf) {
		x=y=0;
		if ((x < ie->len) && (ie->data[x] & 0x80)) {
			sprintf(tmp, "Charset: %02x ", ie->data[x] & 0x7f);
			++x;
		}
		for (y=x; x<ie->len; x++) 
			buf[x] = ie->data[x] & 0x7f;
		buf[x] = '\0';
		pri_message(ctrl, "%c Display (len=%2d) %s[ %s ]\n", prefix, ie->len, tmp, &buf[y]);
		free(buf);
	}
}

#define CHECK_OVERFLOW(limit) \
	if (tmpptr - tmp + limit >= sizeof(tmp)) { \
		*tmpptr = '\0'; \
		pri_message(ctrl, "%s", tmpptr = tmp); \
	}

static void dump_ie_data(struct pri *ctrl, unsigned char *c, int len)
{
	static char hexs[16] = "0123456789ABCDEF";
	char tmp[1024], *tmpptr;
	int lastascii = 0;
	tmpptr = tmp;
	for (; len; --len, ++c) {
		CHECK_OVERFLOW(7);
		if (isprint(*c)) {
			if (!lastascii) {
				if (tmpptr != tmp) { 
					*tmpptr++ = ',';
					*tmpptr++ = ' ';
				}
				*tmpptr++ = '\'';
				lastascii = 1;
			}
			*tmpptr++ = *c;
		} else {
			if (lastascii) {
				*tmpptr++ = '\'';
				lastascii = 0;
			}
			if (tmpptr != tmp) { 
				*tmpptr++ = ',';
				*tmpptr++ = ' ';
			}
			*tmpptr++ = '0';
			*tmpptr++ = 'x';
			*tmpptr++ = hexs[(*c >> 4) & 0x0f];
			*tmpptr++ = hexs[(*c) & 0x0f];
		}
	}
	if (lastascii)
		*tmpptr++ = '\'';
	*tmpptr = '\0';
	pri_message(ctrl, "%s", tmp);
}

static void dump_facility(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int dataat = (ie->data[0] & 0x80) ? 1 : 2;

	pri_message(ctrl, "%c Facility (len=%2d, codeset=%d) [ ", prefix, len, Q931_IE_CODESET(full_ie));
	dump_ie_data(ctrl, ie->data, ie->len);
	pri_message(NULL, " ]\n");
	if (ie->len > 1) {
		pri_message(ctrl, "PROTOCOL %02X\n", ie->data[0] & Q932_PROTOCOL_MASK);
		asn1_dump(ctrl, ie->data + dataat, ie->data + ie->len);
	}

}

static void dump_network_spec_fac(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Network-Specific Facilities (len=%2d) [ ", prefix, ie->len);
	if (ie->data[0] == 0x00) {
 		pri_message(ctrl, "%s", code2str(ie->data[1], facilities, ARRAY_LEN(facilities)));
	}
	else
 		dump_ie_data(ctrl, ie->data, ie->len);
	pri_message(ctrl, " ]\n");
}

static int receive_network_spec_fac(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	return 0;
}

static int transmit_network_spec_fac(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	/* We are ready to transmit single IE only */
	if (order > 1)
		return 0;

	if (ctrl->nsf != PRI_NSF_NONE) {
		ie->data[0] = 0x00;
		ie->data[1] = ctrl->nsf;
		return 4;
	}
	/* Leave off */
	return 0;
}

char *pri_cause2str(int cause)
{
	return code2str(cause, causes, sizeof(causes) / sizeof(causes[0]));
}

static char *pri_causeclass2str(int cause)
{
	static struct msgtype causeclasses[] = {
		{ 0, "Normal Event" },
		{ 1, "Normal Event" },
		{ 2, "Network Congestion (resource unavailable)" },
		{ 3, "Service or Option not Available" },
		{ 4, "Service or Option not Implemented" },
		{ 5, "Invalid message (e.g. parameter out of range)" },
		{ 6, "Protocol Error (e.g. unknown message)" },
		{ 7, "Interworking" },
	};
	return code2str(cause, causeclasses, sizeof(causeclasses) / sizeof(causeclasses[0]));
}

static void dump_cause(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int x;
	pri_message(ctrl, "%c Cause (len=%2d) [ Ext: %d  Coding: %s (%d)  Spare: %d  Location: %s (%d)\n",
		prefix, len, ie->data[0] >> 7, coding2str((ie->data[0] & 0x60) >> 5), (ie->data[0] & 0x60) >> 5,
		(ie->data[0] & 0x10) >> 4, loc2str(ie->data[0] & 0xf), ie->data[0] & 0xf);
	pri_message(ctrl, "%c                  Ext: %d  Cause: %s (%d), class = %s (%d) ]\n",
		prefix, (ie->data[1] >> 7), pri_cause2str(ie->data[1] & 0x7f), ie->data[1] & 0x7f, 
			pri_causeclass2str((ie->data[1] & 0x7f) >> 4), (ie->data[1] & 0x7f) >> 4);
	if (ie->len < 3)
		return;
	/* Dump cause data in readable form */
	switch(ie->data[1] & 0x7f) {
	case PRI_CAUSE_IE_NONEXIST:
		for (x=2;x<ie->len;x++) 
			pri_message(ctrl, "%c              Cause data %d: %02x (%d, %s IE)\n", prefix, x-1, ie->data[x], ie->data[x], ie2str(ie->data[x]));
		break;
	case PRI_CAUSE_WRONG_CALL_STATE:
		for (x=2;x<ie->len;x++) 
			pri_message(ctrl, "%c              Cause data %d: %02x (%d, %s message)\n", prefix, x-1, ie->data[x], ie->data[x], msg2str(ie->data[x]));
		break;
	case PRI_CAUSE_RECOVERY_ON_TIMER_EXPIRE:
		pri_message(ctrl, "%c              Cause data:", prefix);
		for (x=2;x<ie->len;x++)
			pri_message(ctrl, " %02x", ie->data[x]);
		pri_message(ctrl, " (Timer T");
		for (x=2;x<ie->len;x++)
			pri_message(ctrl, "%c", ((ie->data[x] >= ' ') && (ie->data[x] < 0x7f)) ? ie->data[x] : '.');
		pri_message(ctrl, ")\n");
		break;
	default:
		for (x=2;x<ie->len;x++) 
			pri_message(ctrl, "%c              Cause data %d: %02x (%d)\n", prefix, x-1, ie->data[x], ie->data[x]);
		break;
	}
}

static int receive_cause(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->causeloc = ie->data[0] & 0xf;
	call->causecode = (ie->data[0] & 0x60) >> 5;
	call->cause = (ie->data[1] & 0x7f);
	return 0;
}

static int transmit_cause(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	/* We are ready to transmit single IE only */
	if (order > 1)
		return 0;

	if (call->cause > 0) {
		ie->data[0] = 0x80 | (call->causecode << 5)  | (call->causeloc);
		ie->data[1] = 0x80 | (call->cause);
		return 4;
	} else {
		/* Leave off */
		return 0;
	}
}

static void dump_sending_complete(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Sending Complete (len=%2d)\n", prefix, len);
}

static int receive_sending_complete(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	/* We've got a "Complete" message: Exect no further digits. */
	call->complete = 1; 
	return 0;
}

static int transmit_sending_complete(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	if ((ctrl->overlapdial && call->complete) || /* Explicit */
		(!ctrl->overlapdial && ((ctrl->switchtype == PRI_SWITCH_EUROISDN_E1) || 
		/* Implicit */   	   (ctrl->switchtype == PRI_SWITCH_EUROISDN_T1)))) {
		/* Include this single-byte IE */
		return 1;
	}
	return 0;
}

static char *notify2str(int info)
{
	/* ITU-T Q.763 */
	static struct msgtype notifies[] = {
		{ PRI_NOTIFY_USER_SUSPENDED, "User suspended" },
		{ PRI_NOTIFY_USER_RESUMED, "User resumed" },
		{ PRI_NOTIFY_BEARER_CHANGE, "Bearer service change (DSS1)" },
		{ PRI_NOTIFY_ASN1_COMPONENT, "ASN.1 encoded component (DSS1)" },
		{ PRI_NOTIFY_COMPLETION_DELAY, "Call completion delay" },
		{ PRI_NOTIFY_CONF_ESTABLISHED, "Conference established" },
		{ PRI_NOTIFY_CONF_DISCONNECTED, "Conference disconnected" },
		{ PRI_NOTIFY_CONF_PARTY_ADDED, "Other party added" },
		{ PRI_NOTIFY_CONF_ISOLATED, "Isolated" },
		{ PRI_NOTIFY_CONF_REATTACHED, "Reattached" },
		{ PRI_NOTIFY_CONF_OTHER_ISOLATED, "Other party isolated" },
		{ PRI_NOTIFY_CONF_OTHER_REATTACHED, "Other party reattached" },
		{ PRI_NOTIFY_CONF_OTHER_SPLIT, "Other party split" },
		{ PRI_NOTIFY_CONF_OTHER_DISCONNECTED, "Other party disconnected" },
		{ PRI_NOTIFY_CONF_FLOATING, "Conference floating" },
		{ PRI_NOTIFY_WAITING_CALL, "Call is waiting call" },
		{ PRI_NOTIFY_DIVERSION_ACTIVATED, "Diversion activated (DSS1)" },
		{ PRI_NOTIFY_TRANSFER_ALERTING, "Call transfer, alerting" },
		{ PRI_NOTIFY_TRANSFER_ACTIVE, "Call transfer, active" },
		{ PRI_NOTIFY_REMOTE_HOLD, "Remote hold" },
		{ PRI_NOTIFY_REMOTE_RETRIEVAL, "Remote retrieval" },
		{ PRI_NOTIFY_CALL_DIVERTING, "Call is diverting" },
	};
	return code2str(info, notifies, sizeof(notifies) / sizeof(notifies[0]));
}

static void dump_notify(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Notification indicator (len=%2d): Ext: %d  %s (%d)\n", prefix, len, ie->data[0] >> 7, notify2str(ie->data[0] & 0x7f), ie->data[0] & 0x7f);
}

static int receive_notify(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->notify = ie->data[0] & 0x7F;
	return 0;
}

static int transmit_notify(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	if (call->notify >= 0) {
		ie->data[0] = 0x80 | call->notify;
		return 3;
	}
	return 0;
}

static void dump_shift(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c %sLocking Shift (len=%02d): Requested codeset %d\n", prefix, (full_ie & 8) ? "Non-" : "", len, full_ie & 7);
}

static char *lineinfo2str(int info)
{
	/* NAPNA ANI II digits */
	static struct msgtype lineinfo[] = {
		{  0, "Plain Old Telephone Service (POTS)" },
		{  1, "Multiparty line (more than 2)" },
		{  2, "ANI failure" },
		{  6, "Station Level Rating" },
		{  7, "Special Operator Handling Required" },
		{ 20, "Automatic Identified Outward Dialing (AIOD)" },
		{ 23, "Coing or Non-Coin" },
		{ 24, "Toll free translated to POTS originated for non-pay station" },
		{ 25, "Toll free translated to POTS originated from pay station" },
		{ 27, "Pay station with coin control signalling" },
		{ 29, "Prison/Inmate Service" },
		{ 30, "Intercept (blank)" },
		{ 31, "Intercept (trouble)" },
		{ 32, "Intercept (regular)" },
		{ 34, "Telco Operator Handled Call" },
		{ 52, "Outward Wide Area Telecommunications Service (OUTWATS)" },
		{ 60, "TRS call from unrestricted line" },
		{ 61, "Cellular/Wireless PCS (Type 1)" },
		{ 62, "Cellular/Wireless PCS (Type 2)" },
		{ 63, "Cellular/Wireless PCS (Roaming)" },
		{ 66, "TRS call from hotel/motel" },
		{ 67, "TRS call from restricted line" },
		{ 70, "Line connected to pay station" },
		{ 93, "Private virtual network call" },
	};
	return code2str(info, lineinfo, sizeof(lineinfo) / sizeof(lineinfo[0]));
}

static void dump_line_information(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Originating Line Information (len=%02d): %s (%d)\n", prefix, len, lineinfo2str(ie->data[0]), ie->data[0]);
}

static int receive_line_information(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->ani2 = ie->data[0];
	return 0;
}

static int transmit_line_information(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
#if 0	/* XXX Is this IE possible for 4ESS only? XXX */
	if(ctrl->switchtype == PRI_SWITCH_ATT4ESS) {
		ie->data[0] = 0;
		return 3;
	}
#endif
	return 0;
}


static char *gdencoding2str(int encoding)
{
	static struct msgtype gdencoding[] = {
		{ 0, "BCD even" },
		{ 1, "BCD odd" },
		{ 2, "IA5" },
		{ 3, "Binary" },
	};
	return code2str(encoding, gdencoding, sizeof(gdencoding) / sizeof(gdencoding[0]));
}

static char *gdtype2str(int type)
{
	static struct msgtype gdtype[] = {
		{  0, "Account Code" },
		{  1, "Auth Code" },
		{  2, "Customer ID" },
		{  3, "Universal Access" },
		{  4, "Info Digits" },
		{  5, "Callid" },
		{  6, "Opart" },
		{  7, "TCN" },
		{  9, "Adin" },
	};
	return code2str(type, gdtype, sizeof(gdtype) / sizeof(gdtype[0]));
}

static void dump_generic_digits(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	int encoding;
	int type;
	int idx;
	int value;
	if (len < 3) {
		pri_message(ctrl, "%c Generic Digits (len=%02d): Invalid length\n", prefix, len);
		return;
	}
	encoding = (ie->data[0] >> 5) & 7;
	type = ie->data[0] & 0x1F;
	pri_message(ctrl, "%c Generic Digits (len=%02d): Encoding %s  Type %s\n", prefix, len, gdencoding2str(encoding), gdtype2str(type));
	if (encoding == 3) {	/* Binary */
		pri_message(ctrl, "%c                            Don't know how to handle binary encoding\n",
			prefix);
		return;
	}
	if (len == 3)	/* No number information */
		return;
	pri_message(ctrl, "%c                            Digits: ", prefix);
	value = 0;
	for(idx = 3; idx < len; ++idx) {
		switch(encoding) {
		case 0:		/* BCD even */
		case 1:		/* BCD odd */
			pri_message(ctrl, "%d", ie->data[idx-2] & 0x0f);
			value = value * 10 + (ie->data[idx-2] & 0x0f);
			if(!encoding || (idx+1 < len)) {	/* Special handling for BCD odd */
				pri_message(ctrl, "%d", (ie->data[idx-2] >> 4) & 0x0f);
				value = value * 10 + ((ie->data[idx-2] >> 4) & 0x0f);
			}
			break;
		case 2:		/* IA5 */
			pri_message(ctrl, "%c", ie->data[idx-2]);
			value = value * 10 + ie->data[idx-2] - '0';
			break;
		}
	}
	switch(type) {
		case 4:		/* Info Digits */
			pri_message(ctrl, " - %s", lineinfo2str(value));
			break;
	}
	pri_message(ctrl, "\n");
}

static int receive_generic_digits(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	int encoding;
	int type;
	int idx;
	int value;
	int num_idx;
	char number[260];

	if (len < 3) {
		pri_error(ctrl, "Invalid length of Generic Digits IE\n");
		return -1;
	}
	encoding = (ie->data[0] >> 5) & 7;
	type = ie->data[0] & 0x1F;
	if (encoding == 3) {	/* Binary */
		pri_message(ctrl, "!! Unable to handle binary encoded Generic Digits IE\n");
		return 0;
	}
	if (len == 3)	/* No number information */
		return 0;
	value = 0;
	switch(type) {
	/* Integer value handling */
	case 4:		/* Info Digits */
		for(idx = 3; idx < len; ++idx) {
			switch(encoding) {
			case 0:		/* BCD even */
			case 1:		/* BCD odd */
				value = value * 10 + (ie->data[idx-2] & 0x0f);
				if(!encoding || (idx+1 < len))	/* Special handling for BCD odd */
					value = value * 10 + ((ie->data[idx-2] >> 4) & 0x0f);
				break;
			case 2:		/* IA5 */
				value = value * 10 + (ie->data[idx-2] - '0');
				break;
			}
		}
		break;
	/* String value handling */
	case 5:		/* Callid */
		num_idx = 0;
		for(idx = 3; (idx < len) && (num_idx < sizeof(number) - 4); ++idx) {
			switch(encoding) {
			case 0:		/* BCD even */
			case 1:		/* BCD odd */
				number[num_idx++] = '0' + (ie->data[idx-2] & 0x0f);
				if(!encoding || (idx+1 < len))	/* Special handling for BCD odd */
					number[num_idx++] = '0' + ((ie->data[idx-2] >> 4) & 0x0f);
				break;
			case 2:
				number[num_idx++] = ie->data[idx-2];
				break;
			}
		}
		number[num_idx] = '\0';
		break;
	}
	switch(type) {
	case 4:		/* Info Digits */
		call->ani2 = value;
		break;
#if 0
	case 5:		/* Callid */
		if (!call->remote_id.number.valid) {
			call->remote_id.number.valid = 1;
			call->remote_id.number.presentation =
				PRI_PRES_ALLOWED | PRI_PRES_USER_NUMBER_UNSCREENED;
			call->remote_id.number.plan = PRI_UNKNOWN;
			libpri_copy_string(call->remote_id.number.str, number,
				sizeof(call->remote_id.number.str));
		}
		break;
#endif
	}
	return 0;
}

static int transmit_generic_digits(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
#if 0	/* XXX Is this IE possible for other switches? XXX */
	if (order > 1)
		return 0;

	if(ctrl->switchtype == PRI_SWITCH_NI1) {
		ie->data[0] = 0x04;	/* BCD even, Info Digits */
		ie->data[1] = 0x00;	/* POTS */
		return 4;
	}
#endif
	return 0;
}


static char *signal2str(int signal)
{
	/* From Q.931 4.5.8 Table 4-24 */
	static struct msgtype mtsignal[] = {
		{  0, "Dial tone" },
		{  1, "Ring back tone" },
		{  2, "Intercept tone" },
		{  3, "Network congestion tone" },
		{  4, "Busy tone" },
		{  5, "Confirm tone" },
		{  6, "Answer tone" },
		{  7, "Call waiting tone" },
		{  8, "Off-hook warning tone" },
		{  9, "Pre-emption tone" },
		{ 63, "Tones off" },
		{ 64, "Alerting on - pattern 0" },
		{ 65, "Alerting on - pattern 1" },
		{ 66, "Alerting on - pattern 2" },
		{ 67, "Alerting on - pattern 3" },
		{ 68, "Alerting on - pattern 4" },
		{ 69, "Alerting on - pattern 5" },
		{ 70, "Alerting on - pattern 6" },
		{ 71, "Alerting on - pattern 7" },
		{ 79, "Alerting off" },
	};
	return code2str(signal, mtsignal, sizeof(mtsignal) / sizeof(mtsignal[0]));
}


static void dump_signal(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Signal (len=%02d): ", prefix, len);
	if (len < 3) {
		pri_message(ctrl, "Invalid length\n");
		return;
	}
	pri_message(ctrl, "Signal %s (%d)\n", signal2str(ie->data[0]), ie->data[0]);
}

static void dump_transit_count(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	/* Defined in ECMA-225 */
	pri_message(ctrl, "%c Transit Count (len=%02d): ", prefix, len);
	if (len < 3) {
		pri_message(ctrl, "Invalid length\n");
		return;
	}
	pri_message(ctrl, "Count=%d (0x%02x)\n", ie->data[0] & 0x1f, ie->data[0] & 0x1f);
}

static void dump_reverse_charging_indication(int full_ie, struct pri *ctrl, q931_ie *ie, int len, char prefix)
{
	pri_message(ctrl, "%c Reverse Charging Indication (len=%02d): %d\n", prefix, len, ie->data[0] & 0x7);
}

static int receive_reverse_charging_indication(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len)
{
	call->reversecharge = ie->data[0] & 0x7;
	return 0;
}

static int transmit_reverse_charging_indication(int full_ie, struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, int len, int order)
{
	if (call->reversecharge != PRI_REVERSECHARGE_NONE) {
		ie->data[0] = 0x80 | (call->reversecharge & 0x7);
		return 3;
	}
	return 0;
}

static struct ie ies[] = {
	/* Codeset 0 - Common */
	{ 1, NATIONAL_CHANGE_STATUS, "Change Status", dump_change_status, receive_change_status, transmit_change_status },
	{ 0, Q931_LOCKING_SHIFT, "Locking Shift", dump_shift },
	{ 0, Q931_BEARER_CAPABILITY, "Bearer Capability", dump_bearer_capability, receive_bearer_capability, transmit_bearer_capability },
	{ 0, Q931_CAUSE, "Cause", dump_cause, receive_cause, transmit_cause },
	{ 1, Q931_CALL_STATE, "Call State", dump_call_state, receive_call_state, transmit_call_state },
	{ 0, Q931_CHANNEL_IDENT, "Channel Identification", dump_channel_id, receive_channel_id, transmit_channel_id },
	{ 0, Q931_PROGRESS_INDICATOR, "Progress Indicator", dump_progress_indicator, receive_progress_indicator, transmit_progress_indicator },
	{ 0, Q931_NETWORK_SPEC_FAC, "Network-Specific Facilities", dump_network_spec_fac, receive_network_spec_fac, transmit_network_spec_fac },
	{ 1, Q931_INFORMATION_RATE, "Information Rate" },
	{ 1, Q931_TRANSIT_DELAY, "End-to-End Transit Delay" },
	{ 1, Q931_TRANS_DELAY_SELECT, "Transmit Delay Selection and Indication" },
	{ 1, Q931_BINARY_PARAMETERS, "Packet-layer Binary Parameters" },
	{ 1, Q931_WINDOW_SIZE, "Packet-layer Window Size" },
	{ 1, Q931_CLOSED_USER_GROUP, "Closed User Group" },
	{ 1, Q931_REVERSE_CHARGE_INDIC, "Reverse Charging Indication", dump_reverse_charging_indication, receive_reverse_charging_indication, transmit_reverse_charging_indication },
	{ 1, Q931_CALLING_PARTY_NUMBER, "Calling Party Number", dump_calling_party_number, receive_calling_party_number, transmit_calling_party_number },
	{ 1, Q931_CALLING_PARTY_SUBADDR, "Calling Party Subaddress", dump_calling_party_subaddr, receive_calling_party_subaddr },
	{ 1, Q931_CALLED_PARTY_NUMBER, "Called Party Number", dump_called_party_number, receive_called_party_number, transmit_called_party_number },
	{ 1, Q931_CALLED_PARTY_SUBADDR, "Called Party Subaddress", dump_called_party_subaddr },
	{ 0, Q931_REDIRECTING_NUMBER, "Redirecting Number", dump_redirecting_number, receive_redirecting_number, transmit_redirecting_number },
	{ 1, Q931_REDIRECTING_SUBADDR, "Redirecting Subaddress", dump_redirecting_subaddr },
	{ 0, Q931_TRANSIT_NET_SELECT, "Transit Network Selection" },
	{ 1, Q931_RESTART_INDICATOR, "Restart Indicator", dump_restart_indicator, receive_restart_indicator, transmit_restart_indicator },
	{ 0, Q931_LOW_LAYER_COMPAT, "Low-layer Compatibility" },
	{ 0, Q931_HIGH_LAYER_COMPAT, "High-layer Compatibility" },
	{ 1, Q931_PACKET_SIZE, "Packet Size" },
	{ 0, Q931_IE_FACILITY, "Facility" , dump_facility, receive_facility, transmit_facility },
	{ 1, Q931_IE_REDIRECTION_NUMBER, "Redirection Number", dump_redirection_number, receive_redirection_number, transmit_redirection_number },
	{ 1, Q931_IE_REDIRECTION_SUBADDR, "Redirection Subaddress" },
	{ 1, Q931_IE_FEATURE_ACTIVATE, "Feature Activation" },
	{ 1, Q931_IE_INFO_REQUEST, "Feature Request" },
	{ 1, Q931_IE_FEATURE_IND, "Feature Indication" },
	{ 1, Q931_IE_SEGMENTED_MSG, "Segmented Message" },
	{ 1, Q931_IE_CALL_IDENTITY, "Call Identity", dump_call_identity },
	{ 1, Q931_IE_ENDPOINT_ID, "Endpoint Identification" },
	{ 1, Q931_IE_NOTIFY_IND, "Notification Indicator", dump_notify, receive_notify, transmit_notify },
	{ 1, Q931_DISPLAY, "Display", dump_display, receive_display, transmit_display },
	{ 1, Q931_IE_TIME_DATE, "Date/Time", dump_time_date },
	{ 1, Q931_IE_KEYPAD_FACILITY, "Keypad Facility", dump_keypad_facility, receive_keypad_facility, transmit_keypad_facility },
	{ 0, Q931_IE_SIGNAL, "Signal", dump_signal },
	{ 1, Q931_IE_SWITCHHOOK, "Switch-hook" },
	{ 1, Q931_IE_USER_USER, "User-User", dump_user_user, receive_user_user, transmit_user_user },
	{ 1, Q931_IE_ESCAPE_FOR_EXT, "Escape for Extension" },
	{ 1, Q931_IE_CALL_STATUS, "Call Status" },
	{ 1, Q931_IE_CHANGE_STATUS, "Change Status", dump_change_status, receive_change_status, transmit_change_status },
	{ 1, Q931_IE_CONNECTED_ADDR, "Connected Number", dump_connected_number },
	{ 1, Q931_IE_CONNECTED_NUM, "Connected Number", dump_connected_number, receive_connected_number, transmit_connected_number },
	{ 1, Q931_IE_ORIGINAL_CALLED_NUMBER, "Original Called Number", dump_redirecting_number, receive_redirecting_number, transmit_redirecting_number },
	{ 1, Q931_IE_USER_USER_FACILITY, "User-User Facility" },
	{ 1, Q931_IE_UPDATE, "Update" },
	{ 1, Q931_SENDING_COMPLETE, "Sending Complete", dump_sending_complete, receive_sending_complete, transmit_sending_complete },
	/* Codeset 4 - Q.SIG specific */
	{ 1, QSIG_IE_TRANSIT_COUNT | Q931_CODESET(4), "Transit Count", dump_transit_count },
	/* Codeset 6 - Network specific */
	{ 1, Q931_IE_ORIGINATING_LINE_INFO, "Originating Line Information", dump_line_information, receive_line_information, transmit_line_information },
	{ 1, Q931_IE_FACILITY | Q931_CODESET(6), "Facility", dump_facility, receive_facility, transmit_facility },
	{ 1, Q931_DISPLAY | Q931_CODESET(6), "Display (CS6)", dump_display, receive_display, transmit_display },
	{ 0, Q931_IE_GENERIC_DIGITS, "Generic Digits", dump_generic_digits, receive_generic_digits, transmit_generic_digits },
	/* Codeset 7 */
};

static char *ie2str(int ie) 
{
	unsigned int x;

	/* Special handling for Locking/Non-Locking Shifts */
	switch (ie & 0xf8) {
	case Q931_LOCKING_SHIFT:
		switch (ie & 7) {
		case 0:
			return "!! INVALID Locking Shift To Codeset 0";
		case 1:
			return "Locking Shift To Codeset 1";
		case 2:
			return "Locking Shift To Codeset 2";
		case 3:
			return "Locking Shift To Codeset 3";
		case 4:
			return "Locking Shift To Codeset 4";
		case 5:
			return "Locking Shift To Codeset 5";
		case 6:
			return "Locking Shift To Codeset 6";
		case 7:
			return "Locking Shift To Codeset 7";
		}
	case Q931_NON_LOCKING_SHIFT:
		switch (ie & 7) {
		case 0:
			return "Non-Locking Shift To Codeset 0";
		case 1:
			return "Non-Locking Shift To Codeset 1";
		case 2:
			return "Non-Locking Shift To Codeset 2";
		case 3:
			return "Non-Locking Shift To Codeset 3";
		case 4:
			return "Non-Locking Shift To Codeset 4";
		case 5:
			return "Non-Locking Shift To Codeset 5";
		case 6:
			return "Non-Locking Shift To Codeset 6";
		case 7:
			return "Non-Locking Shift To Codeset 7";
		}
	default:
		for (x=0;x<sizeof(ies) / sizeof(ies[0]); x++) 
			if (ie == ies[x].ie)
				return ies[x].name;
		return "Unknown Information Element";
	}
}	

static inline unsigned int ielen(q931_ie *ie)
{
	if ((ie->ie & 0x80) != 0)
		return 1;
	else
		return 2 + ie->len;
}

static char *msg2str(int msg)
{
	unsigned int x;
	for (x=0;x<sizeof(msgs) / sizeof(msgs[0]); x++) 
		if (msgs[x].msgnum == msg)
			return msgs[x].name;
	return "Unknown Message Type";
}

static char *maintenance_msg2str(int msg)
{
	unsigned int x;
	for (x=0; x<sizeof(maintenance_msgs)/sizeof(maintenance_msgs[0]); x++) {
		if (maintenance_msgs[x].msgnum == msg)
			return maintenance_msgs[x].name;
	}
	return "Unknown Message Type";
}

static inline int q931_cr(q931_h *h)
{
	int cr = 0;
	int x;
	if (h->crlen > 3) {
		pri_error(NULL, "Call Reference Length Too long: %d\n", h->crlen);
		return -1;
	}
	switch (h->crlen) {
		case 2: 
			for (x=0;x<h->crlen;x++) {
				cr <<= 8;
				cr |= h->crv[x];
			}
			break;
		case 1:
			cr = h->crv[0];
			if (cr & 0x80) {
				cr &= ~0x80;
				cr |= 0x8000;
			}
			break;
		default:
			pri_error(NULL, "Call Reference Length not supported: %d\n", h->crlen);
	}
	return cr;
}

static inline void q931_dumpie(struct pri *ctrl, int codeset, q931_ie *ie, char prefix)
{
	unsigned int x;
	int full_ie = Q931_FULL_IE(codeset, ie->ie);
	int base_ie;
	char *buf = malloc(ielen(ie) * 3 + 1);
	int buflen = 0;

	buf[0] = '\0';
	if (!(ie->ie & 0x80)) {
		buflen += sprintf(buf, " %02x", ielen(ie)-2);
		for (x = 0; x + 2 < ielen(ie); ++x)
			buflen += sprintf(buf + buflen, " %02x", ie->data[x]);
	}
	pri_message(ctrl, "%c [%02x%s]\n", prefix, ie->ie, buf);
	free(buf);

	/* Special treatment for shifts */
	if((full_ie & 0xf0) == Q931_LOCKING_SHIFT)
		full_ie &= 0xff;

	base_ie = (((full_ie & ~0x7f) == Q931_FULL_IE(0, 0x80)) && ((full_ie & 0x70) != 0x20)) ? full_ie & ~0x0f : full_ie;

	for (x = 0; x < ARRAY_LEN(ies); ++x)
		if (ies[x].ie == base_ie) {
			if (ies[x].dump)
				ies[x].dump(full_ie, ctrl, ie, ielen(ie), prefix);
			else
				pri_message(ctrl, "%c IE: %s (len = %d)\n", prefix, ies[x].name, ielen(ie));
			return;
		}
	
	pri_error(ctrl, "!! %c Unknown IE %d (cs%d, len = %d)\n", prefix, Q931_IE_IE(base_ie), Q931_IE_CODESET(base_ie), ielen(ie));
}

static q931_call *q931_getcall(struct pri *ctrl, int cr)
{
	q931_call *cur;
	q931_call *prev;
	struct pri *master;

	/* Find the master  - He has the call pool */
	if (ctrl->master) {
		master = ctrl->master;
	} else {
		master = ctrl;
	}

	cur = *master->callpool;
	prev = NULL;
	while (cur) {
		if (cur->cr == cr) {
			return cur;
		}
		prev = cur;
		cur = cur->next;
	}

	/* No call exists, make a new one */
	if (ctrl->debug & PRI_DEBUG_Q931_STATE) {
		pri_message(ctrl, "-- Making new call for cr %d\n", cr);
	}

	cur = calloc(1, sizeof(*cur));
	if (!cur) {
		return NULL;
	}

	/* Initialize call structure. */
	cur->cr = cr;
	cur->slotmap = -1;
	cur->channelno = -1;
	cur->newcall = 1;
	cur->ourcallstate = Q931_CALL_STATE_NULL;
	cur->peercallstate = Q931_CALL_STATE_NULL;
	cur->sugcallstate = -1;
	cur->ri = -1;
	cur->transcapability = -1;
	cur->transmoderate = -1;
	cur->transmultiple = -1;
	cur->userl1 = -1;
	cur->userl2 = -1;
	cur->userl3 = -1;
	cur->rateadaption = -1;
	cur->progress = -1;
	cur->causecode = -1;
	cur->causeloc = -1;
	cur->cause = -1;
	cur->useruserprotocoldisc = -1;
	cur->aoc_units = -1;
	cur->changestatus = -1;
	q931_party_number_init(&cur->redirection_number);
	q931_party_address_init(&cur->called);
	q931_party_id_init(&cur->local_id);
	q931_party_id_init(&cur->remote_id);
	q931_party_redirecting_init(&cur->redirecting);

	/* PRI is set to whoever called us */
	if (ctrl->bri && (ctrl->localtype == PRI_CPE)) {
		/*
		 * Point to the master to avoid stale pointer problems if
		 * the TEI is removed later.
		 */
		cur->pri = master;
	} else {
		cur->pri = ctrl;
	}

	/* Append to end of list */
	if (prev) {
		prev->next = cur;
	} else {
		*master->callpool = cur;
	}

	return cur;
}

q931_call *q931_new_call(struct pri *ctrl)
{
	q931_call *cur;

	do {
		cur = *ctrl->callpool;
		ctrl->cref++;
		if (!ctrl->bri) {
			if (ctrl->cref > 32767)
				ctrl->cref = 1;
		} else {
			if (ctrl->cref > 127)
				ctrl->cref = 1;
		}
		while(cur) {
			if (cur->cr == (0x8000 | ctrl->cref))
				break;
			cur = cur->next;
		}
	} while(cur);

	return q931_getcall(ctrl, ctrl->cref | 0x8000);
}

static void q931_destroy(struct pri *ctrl, int cr, q931_call *c)
{
	q931_call *cur, *prev;

	/* For destroying, make sure we are using the master span, since it maintains the call pool */
	for (;ctrl->master; ctrl = ctrl->master);

	prev = NULL;
	cur = *ctrl->callpool;
	while(cur) {
		if ((c && (cur == c)) || (!c && (cur->cr == cr))) {
			if (prev)
				prev->next = cur->next;
			else
				*ctrl->callpool = cur->next;
			if (ctrl->debug & PRI_DEBUG_Q931_STATE)
				pri_message(ctrl,
					"NEW_HANGUP DEBUG: Destroying the call, ourstate %s, peerstate %s\n",
					q931_call_state_str(cur->ourcallstate),
					q931_call_state_str(cur->peercallstate));
			if (cur->retranstimer)
				pri_schedule_del(ctrl, cur->retranstimer);
			pri_call_apdu_queue_cleanup(cur);
			free(cur);
			return;
		}
		prev = cur;
		cur = cur->next;
	}
	pri_error(ctrl, "Can't destroy call %d!\n", cr);
}

static void q931_destroycall(struct pri *ctrl, int cr)
{
	q931_destroy(ctrl, cr, NULL);
}


void __q931_destroycall(struct pri *ctrl, q931_call *call) 
{
	if (ctrl && call) {
		q931_destroy(ctrl, 0, call);
	}
}

static int add_ie(struct pri *ctrl, q931_call *call, int msgtype, int ie, q931_ie *iet, int maxlen, int *codeset)
{
	unsigned int x;
	int res, total_res;
	int have_shift;
	int ies_count, order;
	for (x=0;x<sizeof(ies) / sizeof(ies[0]);x++) {
		if (ies[x].ie == ie) {
			/* This is our baby */
			if (ies[x].transmit) {
				/* Prepend with CODE SHIFT IE if required */
				if (*codeset != Q931_IE_CODESET(ies[x].ie)) {
					/* Locking shift to codeset 0 isn't possible */
					iet->ie = Q931_IE_CODESET(ies[x].ie) | (Q931_IE_CODESET(ies[x].ie) ? Q931_LOCKING_SHIFT : Q931_NON_LOCKING_SHIFT);
					have_shift = 1;
					iet = (q931_ie *)((char *)iet + 1);
					maxlen--;
				}
				else
					have_shift = 0;
				ies_count = ies[x].max_count;
				if (ies_count == 0)
					ies_count = INT_MAX;
				order = 0;
				total_res = 0;
				do {
					iet->ie = ie;
					res = ies[x].transmit(ie, ctrl, call, msgtype, iet, maxlen, ++order);
					/* Error if res < 0 or ignored if res == 0 */
					if (res < 0)
						return res;
					if (res > 0) {
						if ((iet->ie & 0x80) == 0) /* Multibyte IE */
							iet->len = res - 2;
						total_res += res;
						maxlen -= res;
						iet = (q931_ie *)((char *)iet + res);
					}
				} while (res > 0 && order < ies_count);
				if (have_shift && total_res) {
					if (Q931_IE_CODESET(ies[x].ie))
						*codeset = Q931_IE_CODESET(ies[x].ie);
					return total_res + 1; /* Shift is single-byte IE */
				}
				return total_res;
			} else {
				pri_error(ctrl, "!! Don't know how to add an IE %s (%d)\n", ie2str(ie), ie);
				return -1;
			}
		}
	}
	pri_error(ctrl, "!! Unknown IE %d (%s)\n", ie, ie2str(ie));
	return -1;
}

static char *disc2str(int disc)
{
	static struct msgtype discs[] = {
		{ Q931_PROTOCOL_DISCRIMINATOR, "Q.931" },
		{ GR303_PROTOCOL_DISCRIMINATOR, "GR-303" },
		{ 0x3, "AT&T Maintenance" },
		{ 0x43, "New AT&T Maintenance" },
	};
	return code2str(disc, discs, sizeof(discs) / sizeof(discs[0]));
}

void q931_dump(struct pri *ctrl, q931_h *h, int len, int txrx)
{
	q931_mh *mh;
	char c;
	int x=0, r;
	int cur_codeset;
	int codeset;
	int cref;

	c = txrx ? '>' : '<';
	pri_message(ctrl, "%c Protocol Discriminator: %s (%d)  len=%d\n", c, disc2str(h->pd), h->pd, len);
	cref = q931_cr(h);
	pri_message(ctrl, "%c Call Ref: len=%2d (reference %d/0x%X) (%s)\n",
		c, h->crlen, cref & 0x7FFF, cref & 0x7FFF,
		(cref & 0x8000) ? "Terminator" : "Originator");

	/* Message header begins at the end of the call reference number */
	mh = (q931_mh *)(h->contents + h->crlen);
	if ((h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_1) || (h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_2)) {
		pri_message(ctrl, "%c Message Type: %s (%d)\n", c, maintenance_msg2str(mh->msg), mh->msg);
	} else {
		pri_message(ctrl, "%c Message Type: %s (%d)\n", c, msg2str(mh->msg), mh->msg);
	}
	/* Drop length of header, including call reference */
	len -= (h->crlen + 3);
	codeset = cur_codeset = 0;
	while(x < len) {
		r = ielen((q931_ie *)(mh->data + x));
		q931_dumpie(ctrl, cur_codeset, (q931_ie *)(mh->data + x), c);
		switch (mh->data[x] & 0xf8) {
		case Q931_LOCKING_SHIFT:
			if ((mh->data[x] & 7) > 0)
				codeset = cur_codeset = mh->data[x] & 7;
			break;
		case Q931_NON_LOCKING_SHIFT:
			cur_codeset = mh->data[x] & 7;
			break;
		default:
			/* Reset temporary codeset change */
			cur_codeset = codeset;
		}
		x += r;
	}
	if (x > len) 
		pri_error(ctrl, "XXX Message longer than it should be?? XXX\n");
}

static int q931_handle_ie(int codeset, struct pri *ctrl, q931_call *c, int msg, q931_ie *ie)
{
	unsigned int x;
	int full_ie = Q931_FULL_IE(codeset, ie->ie);
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl, "-- Processing IE %d (cs%d, %s)\n", ie->ie, codeset, ie2str(full_ie));
	for (x=0;x<sizeof(ies) / sizeof(ies[0]);x++) {
		if (full_ie == ies[x].ie) {
			if (ies[x].receive)
				return ies[x].receive(full_ie, ctrl, c, msg, ie, ielen(ie));
			else {
				if (ctrl->debug & PRI_DEBUG_Q931_ANOMALY)
					pri_error(ctrl, "!! No handler for IE %d (cs%d, %s)\n", ie->ie, codeset, ie2str(full_ie));
				return -1;
			}
		}
	}
	pri_message(ctrl, "!! Unknown IE %d (cs%d, %s)\n", ie->ie, codeset, ie2str(full_ie));
	return -1;
}

/* Returns header and message header and modifies length in place */
static void init_header(struct pri *ctrl, q931_call *call, unsigned char *buf, q931_h **hb, q931_mh **mhb, int *len, int protodisc)
{
	q931_h *h = (q931_h *) buf;
	q931_mh *mh;
	unsigned crv;

	if (protodisc) {
		h->pd = protodisc;
	} else {
		h->pd = ctrl->protodisc;
	}
	h->x0 = 0;		/* Reserved 0 */
	if (!ctrl->bri) {
		/* Two bytes of Call Reference. */
		h->crlen = 2;
		/* Invert the top bit to make it from our sense */
		crv = (unsigned) call->cr;
		h->crv[0] = ((crv >> 8) ^ 0x80) & 0xff;
		h->crv[1] = crv & 0xff;
		if (ctrl->subchannel && !ctrl->bri) {
			/* On GR-303, top bit is always 0 */
			h->crv[0] &= 0x7f;
		}
	} else {
		h->crlen = 1;
		/* Invert the top bit to make it from our sense */
		crv = (unsigned) call->cr;
		h->crv[0] = (((crv >> 8) ^ 0x80) & 0x80) | (crv & 0x7f);
	}
	*hb = h;

	*len -= 3;/* Protocol discriminator, call reference length, message type id */
	*len -= h->crlen;

	mh = (q931_mh *) (h->contents + h->crlen);
	mh->f = 0;
	*mhb = mh;
}

static int q931_xmit(struct pri *ctrl, q931_h *h, int len, int cr)
{
	q921_transmit_iframe(ctrl, h, len, cr);
	/* The transmit operation might dump the q921 header, so logging the q931
	   message body after the transmit puts the sections of the message in the
	   right order in the log */
	if (ctrl->debug & PRI_DEBUG_Q931_DUMP)
		q931_dump(ctrl, h, len, 1);
#ifdef LIBPRI_COUNTERS
	ctrl->q931_txcount++;
#endif
	return 0;
}

static int send_message(struct pri *ctrl, q931_call *call, int msgtype, int ies[])
{
	unsigned char buf[1024];
	q931_h *h;
	q931_mh *mh;
	int len;
	int res;
	int offset=0;
	int x;
	int codeset;

	memset(buf, 0, sizeof(buf));
	len = sizeof(buf);
	init_header(ctrl, call, buf, &h, &mh, &len, (msgtype >> 8));
	mh->msg = msgtype & 0x00ff;
	x=0;
	codeset = 0;
	while(ies[x] > -1) {
		res = add_ie(ctrl, call, mh->msg, ies[x], (q931_ie *)(mh->data + offset), len, &codeset);
		if (res < 0) {
			pri_error(ctrl, "!! Unable to add IE '%s'\n", ie2str(ies[x]));
			return -1;
		}

		offset += res;
		len -= res;
		x++;
	}
	/* Invert the logic */
	len = sizeof(buf) - len;

	ctrl = call->pri;
	if (ctrl->bri && (ctrl->localtype == PRI_CPE)) {
		/*
		 * Must use the BRI subchannel structure to send with the correct TEI.
		 * Note: If the subchannel is NULL then there is no TEI assigned and
		 * we should not be sending anything out at this time.
		 */
		ctrl = ctrl->subchannel;
	}
	if (ctrl) {
		q931_xmit(ctrl, h, len, 1);
	}
	call->acked = 1;
	return 0;
}

static int maintenance_service_ies[] = { Q931_IE_CHANGE_STATUS, Q931_CHANNEL_IDENT, -1 };

int maintenance_service_ack(struct pri *ctrl, q931_call *c)
{
	return send_message(ctrl, c, (MAINTENANCE_PROTOCOL_DISCRIMINATOR_1 << 8) | NATIONAL_SERVICE_ACKNOWLEDGE, maintenance_service_ies);
}

int maintenance_service(struct pri *ctrl, int span, int channel, int changestatus)
{
	struct q931_call *c;
	c = q931_getcall(ctrl, 0 | 0x8000);
	if (!c) {
		return -1;
	}
	if (channel > -1) {
		channel &= 0xff;
	}
	c->ds1no = span;
	c->channelno = channel;
	c->chanflags |= FLAG_EXCLUSIVE;
	c->changestatus = changestatus;
	return send_message(ctrl, c, (MAINTENANCE_PROTOCOL_DISCRIMINATOR_1 << 8) | NATIONAL_SERVICE, maintenance_service_ies);
}

static int status_ies[] = { Q931_CAUSE, Q931_CALL_STATE, -1 };

static int q931_status(struct pri *ctrl, q931_call *c, int cause)
{
	q931_call *cur = NULL;
	if (!cause)
		cause = PRI_CAUSE_RESPONSE_TO_STATUS_ENQUIRY;
	if (c->cr > -1)
		cur = *ctrl->callpool;
	while(cur) {
		if (cur->cr == c->cr) {
			cur->cause=cause;
			cur->causecode = CODE_CCITT;
			cur->causeloc = LOC_USER;
			break;
		}
		cur = cur->next;
	}
	if (!cur) {
		pri_message(ctrl, "YYY Here we get reset YYY\n");
		/* something went wrong, respond with "no such call" */
		c->ourcallstate = Q931_CALL_STATE_NULL;
		c->peercallstate = Q931_CALL_STATE_NULL;
		cur=c;
	}
	return send_message(ctrl, cur, Q931_STATUS, status_ies);
}

static int information_ies[] = { Q931_CALLED_PARTY_NUMBER, -1 };

int q931_information(struct pri *ctrl, q931_call *c, char digit)
{
	c->overlap_digits[0] = digit;
	c->overlap_digits[1] = '\0';

	/*
	 * Since we are doing overlap dialing now, we need to accumulate
	 * the digits into call->called.number.str.
	 */
	c->called.number.valid = 1;
	if (strlen(c->called.number.str) < sizeof(c->called.number.str) - 1) {
		/* There is enough room for the new digit. */
		strcat(c->called.number.str, c->overlap_digits);
	}

	return send_message(ctrl, c, Q931_INFORMATION, information_ies);
}

static int keypad_facility_ies[] = { Q931_IE_KEYPAD_FACILITY, -1 };

int q931_keypad_facility(struct pri *ctrl, q931_call *call, const char *digits)
{
	libpri_copy_string(call->keypad_digits, digits, sizeof(call->keypad_digits));
	return send_message(ctrl, call, Q931_INFORMATION, keypad_facility_ies);
}

static int restart_ack_ies[] = { Q931_CHANNEL_IDENT, Q931_RESTART_INDICATOR, -1 };

static int restart_ack(struct pri *ctrl, q931_call *c)
{
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_NULL);
	c->peercallstate = Q931_CALL_STATE_NULL;
	return send_message(ctrl, c, Q931_RESTART_ACKNOWLEDGE, restart_ack_ies);
}

static int facility_ies[] = { Q931_IE_FACILITY, -1 };

int q931_facility(struct pri*ctrl, q931_call *c)
{
	return send_message(ctrl, c, Q931_FACILITY, facility_ies);
}

static int notify_ies[] = { Q931_IE_NOTIFY_IND, Q931_IE_REDIRECTION_NUMBER, -1 };

/*!
 * \brief Send a NOTIFY message with optional redirection number.
 *
 * \param ctrl D channel controller.
 * \param call Q.931 call leg
 * \param notify Notification indicator
 * \param number Redirection number to send if not NULL.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int q931_notify_redirection(struct pri *ctrl, q931_call *call, int notify, const struct q931_party_number *number)
{
	if (number) {
		call->redirection_number = *number;
	} else {
		q931_party_number_init(&call->redirection_number);
	}
	call->notify = notify;
	return send_message(ctrl, call, Q931_NOTIFY, notify_ies);
}

int q931_notify(struct pri *ctrl, q931_call *c, int channel, int info)
{
	if ((ctrl->switchtype == PRI_SWITCH_EUROISDN_T1) || (ctrl->switchtype != PRI_SWITCH_EUROISDN_E1)) {
		if ((info > 0x2) || (info < 0x00)) {
			return 0;
		}
	}

	if (info >= 0) {
		info = info & 0x7F;
	} else {
		info = -1;
	}
	return q931_notify_redirection(ctrl, c, info, NULL);
}

#ifdef ALERTING_NO_PROGRESS
static int call_progress_ies[] = { -1 };
#else
static int call_progress_with_cause_ies[] = { Q931_PROGRESS_INDICATOR, Q931_CAUSE, -1 };

static int call_progress_ies[] = { Q931_PROGRESS_INDICATOR, -1 };
#endif

int q931_call_progress(struct pri *ctrl, q931_call *c, int channel, int info)
{
	if (channel) { 
		c->ds1no = (channel & 0xff00) >> 8;
		c->ds1explicit = (channel & 0x10000) >> 16;
		channel &= 0xff;
		c->channelno = channel;		
	}

	if (info) {
		c->progloc = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progressmask = PRI_PROG_INBAND_AVAILABLE;
	} else {
		/* PI is mandatory IE for PROGRESS message - Q.931 3.1.8 */
		pri_error(ctrl, "XXX Progress message requested but no information is provided\n");
		c->progressmask = 0;
	}

	c->alive = 1;
	return send_message(ctrl, c, Q931_PROGRESS, call_progress_ies);
}

int q931_call_progress_with_cause(struct pri *ctrl, q931_call *c, int channel, int info, int cause)
{
	if (channel) { 
		c->ds1no = (channel & 0xff00) >> 8;
		c->ds1explicit = (channel & 0x10000) >> 16;
		channel &= 0xff;
		c->channelno = channel;		
	}

	if (info) {
		c->progloc = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progressmask = PRI_PROG_INBAND_AVAILABLE;
	} else {
		/* PI is mandatory IE for PROGRESS message - Q.931 3.1.8 */
		pri_error(ctrl, "XXX Progress message requested but no information is provided\n");
		c->progressmask = 0;
	}

	c->cause = cause;
	c->causecode = CODE_CCITT;
	c->causeloc = LOC_PRIV_NET_LOCAL_USER;

	c->alive = 1;
	return send_message(ctrl, c, Q931_PROGRESS, call_progress_with_cause_ies);
}

#ifdef ALERTING_NO_PROGRESS
static int call_proceeding_ies[] = { Q931_CHANNEL_IDENT, -1 };
#else
static int call_proceeding_ies[] = { Q931_CHANNEL_IDENT, Q931_PROGRESS_INDICATOR, -1 };
#endif

int q931_call_proceeding(struct pri *ctrl, q931_call *c, int channel, int info)
{
	if (channel) { 
		c->ds1no = (channel & 0xff00) >> 8;
		c->ds1explicit = (channel & 0x10000) >> 16;
		channel &= 0xff;
		c->channelno = channel;		
	}
	c->chanflags &= ~FLAG_PREFERRED;
	c->chanflags |= FLAG_EXCLUSIVE;
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_INCOMING_CALL_PROCEEDING);
	c->peercallstate = Q931_CALL_STATE_OUTGOING_CALL_PROCEEDING;
	if (info) {
		c->progloc = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progressmask = PRI_PROG_INBAND_AVAILABLE;
	} else
		c->progressmask = 0;
	c->proc = 1;
	c->alive = 1;
	return send_message(ctrl, c, Q931_CALL_PROCEEDING, call_proceeding_ies);
}
#ifndef ALERTING_NO_PROGRESS
static int alerting_ies[] = { Q931_PROGRESS_INDICATOR, Q931_IE_USER_USER, Q931_IE_FACILITY, -1 };
#else
static int alerting_ies[] = { Q931_IE_FACILITY, -1 };
#endif

int q931_alerting(struct pri *ctrl, q931_call *c, int channel, int info)
{
	if (!c->proc) 
		q931_call_proceeding(ctrl, c, channel, 0);
	if (info) {
		c->progloc = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progressmask = PRI_PROG_INBAND_AVAILABLE;
	} else
		c->progressmask = 0;
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_CALL_RECEIVED);
	c->peercallstate = Q931_CALL_STATE_CALL_DELIVERED;
	c->alive = 1;

	switch (ctrl->switchtype) {
	case PRI_SWITCH_QSIG:
		if (c->local_id.name.valid) {
			/* Send calledName with ALERTING */
			rose_called_name_encode(ctrl, c, Q931_ALERTING);
		}
		break;
	default:
		break;
	}

	return send_message(ctrl, c, Q931_ALERTING, alerting_ies);
}

static int connect_ies[] = {  Q931_CHANNEL_IDENT, Q931_PROGRESS_INDICATOR, Q931_IE_CONNECTED_NUM, Q931_IE_FACILITY, -1 };
 
int q931_setup_ack(struct pri *ctrl, q931_call *c, int channel, int nonisdn)
{
	if (channel) { 
		c->ds1no = (channel & 0xff00) >> 8;
		c->ds1explicit = (channel & 0x10000) >> 16;
		channel &= 0xff;
		c->channelno = channel;		
	}
	c->chanflags &= ~FLAG_PREFERRED;
	c->chanflags |= FLAG_EXCLUSIVE;
	if (nonisdn && (ctrl->switchtype != PRI_SWITCH_DMS100)) {
		c->progloc  = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progressmask = PRI_PROG_CALLED_NOT_ISDN;
	} else
		c->progressmask = 0;
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_OVERLAP_RECEIVING);
	c->peercallstate = Q931_CALL_STATE_OVERLAP_SENDING;
	c->alive = 1;
	return send_message(ctrl, c, Q931_SETUP_ACKNOWLEDGE, connect_ies);
}

/* T313 expiry, first time */
static void pri_connect_timeout(void *data)
{
	struct q931_call *c = data;
	struct pri *ctrl = c->pri;
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl, "Timed out looking for connect acknowledge\n");
	q931_disconnect(ctrl, c, PRI_CAUSE_NORMAL_CLEARING);
	
}

/* T308 expiry, first time */
static void pri_release_timeout(void *data)
{
	struct q931_call *c = data;
	struct pri *ctrl = c->pri;
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl, "Timed out looking for release complete\n");
	c->t308_timedout++;
	c->alive = 1;

	/* The call to q931_release will re-schedule T308 */
	q931_release(ctrl, c, c->cause);
}

/* T308 expiry, second time */
static void pri_release_finaltimeout(void *data)
{
	struct q931_call *c = data;
	struct pri *ctrl = c->pri;
	c->alive = 1;
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl, "Final time-out looking for release complete\n");
	c->t308_timedout++;
	c->ourcallstate = Q931_CALL_STATE_NULL;
	c->peercallstate = Q931_CALL_STATE_NULL;
	ctrl->schedev = 1;
	ctrl->ev.e = PRI_EVENT_HANGUP_ACK;
	ctrl->ev.hangup.subcmds = &ctrl->subcmds;
	ctrl->ev.hangup.channel = c->channelno;
	ctrl->ev.hangup.cause = c->cause;
	ctrl->ev.hangup.cref = c->cr;
	ctrl->ev.hangup.call = c;
	ctrl->ev.hangup.aoc_units = c->aoc_units;
	libpri_copy_string(ctrl->ev.hangup.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.hangup.useruserinfo));
	q931_hangup(ctrl, c, c->cause);
}

/* T305 expiry, first time */
static void pri_disconnect_timeout(void *data)
{
	struct q931_call *c = data;
	struct pri *ctrl = c->pri;
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl, "Timed out looking for release\n");
	c->alive = 1;
	q931_release(ctrl, c, PRI_CAUSE_NORMAL_CLEARING);
}

int q931_connect(struct pri *ctrl, q931_call *c, int channel, int nonisdn)
{
	if (channel) { 
		c->ds1no = (channel & 0xff00) >> 8;
		c->ds1explicit = (channel & 0x10000) >> 16;
		channel &= 0xff;
		c->channelno = channel;		
	}
	c->chanflags &= ~FLAG_PREFERRED;
	c->chanflags |= FLAG_EXCLUSIVE;
	if (nonisdn && (ctrl->switchtype != PRI_SWITCH_DMS100)) {
		c->progloc  = LOC_PRIV_NET_LOCAL_USER;
		c->progcode = CODE_CCITT;
		c->progressmask = PRI_PROG_CALLED_NOT_ISDN;
	} else
		c->progressmask = 0;
	if(ctrl->localtype == PRI_NETWORK || ctrl->switchtype == PRI_SWITCH_QSIG)
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_ACTIVE);
	else
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_CONNECT_REQUEST);
	c->peercallstate = Q931_CALL_STATE_ACTIVE;
	c->alive = 1;
	/* Connect request timer */
	if (c->retranstimer)
		pri_schedule_del(ctrl, c->retranstimer);
	c->retranstimer = 0;
	if ((c->ourcallstate == Q931_CALL_STATE_CONNECT_REQUEST) && (ctrl->bri || (!ctrl->subchannel)))
		c->retranstimer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T313], pri_connect_timeout, c);

	if (c->redirecting.state == Q931_REDIRECTING_STATE_PENDING_TX_DIV_LEG_3) {
		c->redirecting.state = Q931_REDIRECTING_STATE_IDLE;
		/* Send DivertingLegInformation3 with CONNECT. */
		c->redirecting.to = c->local_id;
		if (!c->redirecting.to.number.valid) {
			q931_party_number_init(&c->redirecting.to.number);
			c->redirecting.to.number.valid = 1;
			c->redirecting.to.number.presentation =
				PRI_PRES_RESTRICTED | PRI_PRES_USER_NUMBER_UNSCREENED;
		}
		rose_diverting_leg_information3_encode(ctrl, c, Q931_CONNECT);
	}
	switch (ctrl->switchtype) {
	case PRI_SWITCH_QSIG:
		if (c->local_id.name.valid) {
			/* Send connectedName with CONNECT */
			rose_connected_name_encode(ctrl, c, Q931_CONNECT);
		}
		break;
	default:
		break;
	}
	return send_message(ctrl, c, Q931_CONNECT, connect_ies);
}

static int release_ies[] = { Q931_CAUSE, Q931_IE_USER_USER, -1 };

int q931_release(struct pri *ctrl, q931_call *c, int cause)
{
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_RELEASE_REQUEST);
	/* c->peercallstate stays the same */
	if (c->alive) {
		c->alive = 0;
		c->cause = cause;
		c->causecode = CODE_CCITT;
		c->causeloc = LOC_PRIV_NET_LOCAL_USER;
		if (c->acked) {
			if (c->retranstimer)
				pri_schedule_del(ctrl, c->retranstimer);
			if (!c->t308_timedout) {
				c->retranstimer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T308], pri_release_timeout, c);
			} else {
				c->retranstimer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T308], pri_release_finaltimeout, c);
			}
			return send_message(ctrl, c, Q931_RELEASE, release_ies);
		} else
			return send_message(ctrl, c, Q931_RELEASE_COMPLETE, release_ies); /* Yes, release_ies, not release_complete_ies */
	} else
		return 0;
}

static int restart_ies[] = { Q931_CHANNEL_IDENT, Q931_RESTART_INDICATOR, -1 };

int q931_restart(struct pri *ctrl, int channel)
{
	struct q931_call *c;
	c = q931_getcall(ctrl, 0 | 0x8000);
	if (!c)
		return -1;
	if (!channel)
		return -1;
	c->ri = 0;
	c->ds1no = (channel & 0xff00) >> 8;
	c->ds1explicit = (channel & 0x10000) >> 16;
	channel &= 0xff;
	c->channelno = channel;		
	c->chanflags &= ~FLAG_PREFERRED;
	c->chanflags |= FLAG_EXCLUSIVE;
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_RESTART);
	c->peercallstate = Q931_CALL_STATE_RESTART_REQUEST;
	return send_message(ctrl, c, Q931_RESTART, restart_ies);
}

static int disconnect_ies[] = { Q931_CAUSE, Q931_IE_USER_USER, -1 };

int q931_disconnect(struct pri *ctrl, q931_call *c, int cause)
{
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_DISCONNECT_REQUEST);
	c->peercallstate = Q931_CALL_STATE_DISCONNECT_INDICATION;
	if (c->alive) {
		c->alive = 0;
		c->cause = cause;
		c->causecode = CODE_CCITT;
		c->causeloc = LOC_PRIV_NET_LOCAL_USER;
		c->sendhangupack = 1;
		if (c->retranstimer)
			pri_schedule_del(ctrl, c->retranstimer);
		c->retranstimer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T305], pri_disconnect_timeout, c);
		return send_message(ctrl, c, Q931_DISCONNECT, disconnect_ies);
	} else
		return 0;
}

static int setup_ies[] = { Q931_BEARER_CAPABILITY, Q931_CHANNEL_IDENT, Q931_IE_FACILITY, Q931_PROGRESS_INDICATOR, Q931_NETWORK_SPEC_FAC, Q931_DISPLAY,
	Q931_REVERSE_CHARGE_INDIC, Q931_CALLING_PARTY_NUMBER, Q931_CALLED_PARTY_NUMBER, Q931_REDIRECTING_NUMBER, Q931_IE_USER_USER,
	Q931_SENDING_COMPLETE, Q931_IE_ORIGINATING_LINE_INFO, Q931_IE_GENERIC_DIGITS, -1 };

static int gr303_setup_ies[] =  { Q931_BEARER_CAPABILITY, Q931_CHANNEL_IDENT, -1 };

static int cis_setup_ies[] = { Q931_BEARER_CAPABILITY, Q931_CHANNEL_IDENT, Q931_IE_FACILITY, Q931_CALLED_PARTY_NUMBER, -1 };

int q931_setup(struct pri *ctrl, q931_call *c, struct pri_sr *req)
{
	int res;
	
	
	c->transcapability = req->transmode;
	c->transmoderate = TRANS_MODE_64_CIRCUIT;
	if (!req->userl1)
		req->userl1 = PRI_LAYER_1_ULAW;
	c->userl1 = req->userl1;
	c->userl2 = -1;
	c->userl3 = -1;
	c->ds1no = (req->channel & 0xff00) >> 8;
	c->ds1explicit = (req->channel & 0x10000) >> 16;
	req->channel &= 0xff;
	if ((ctrl->localtype == PRI_CPE) && ctrl->subchannel && !ctrl->bri) {
		req->channel = 0;
		req->exclusive = 0;
	}
		
	c->channelno = req->channel;		
	c->slotmap = -1;
	c->nonisdn = req->nonisdn;
	c->newcall = 0;
	c->justsignalling = req->justsignalling;		
	c->complete = req->numcomplete; 
	if (req->exclusive) 
		c->chanflags = FLAG_EXCLUSIVE;
	else if (c->channelno)
		c->chanflags = FLAG_PREFERRED;

	if (req->caller.number.valid) {
		c->local_id = req->caller;
		q931_party_id_fixup(ctrl, &c->local_id);
	}

	if (req->redirecting.from.number.valid) {
		c->redirecting = req->redirecting;
		q931_party_id_fixup(ctrl, &c->redirecting.from);
		q931_party_id_fixup(ctrl, &c->redirecting.to);
		q931_party_id_fixup(ctrl, &c->redirecting.orig_called);
	}

	if (req->called.number.valid) {
		c->called = req->called;
		libpri_copy_string(c->overlap_digits, req->called.number.str, sizeof(c->overlap_digits));
	} else
		return -1;

	if (req->useruserinfo)
		libpri_copy_string(c->useruserinfo, req->useruserinfo, sizeof(c->useruserinfo));
	else
		c->useruserinfo[0] = '\0';

	if (req->nonisdn && (ctrl->switchtype == PRI_SWITCH_NI2))
		c->progressmask = PRI_PROG_CALLER_NOT_ISDN;
	else
		c->progressmask = 0;

	c->reversecharge = req->reversecharge;

	pri_call_add_standard_apdus(ctrl, c);

	if (ctrl->subchannel && !ctrl->bri)
		res = send_message(ctrl, c, Q931_SETUP, gr303_setup_ies);
	else if (c->justsignalling)
		res = send_message(ctrl, c, Q931_SETUP, cis_setup_ies);
	else
		res = send_message(ctrl, c, Q931_SETUP, setup_ies);
	if (!res) {
		c->alive = 1;
		/* make sure we call PRI_EVENT_HANGUP_ACK once we send/receive RELEASE_COMPLETE */
		c->sendhangupack = 1;
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_CALL_INITIATED);
		c->peercallstate = Q931_CALL_STATE_OVERLAP_SENDING;	
	}
	return res;
	
}

static int release_complete_ies[] = { Q931_IE_USER_USER, -1 };

static int q931_release_complete(struct pri *ctrl, q931_call *c, int cause)
{
	int res = 0;
	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_NULL);
	c->peercallstate = Q931_CALL_STATE_NULL;
	if (cause > -1) {
		c->cause = cause;
		c->causecode = CODE_CCITT;
		c->causeloc = LOC_PRIV_NET_LOCAL_USER;
		/* release_ies has CAUSE in it */
		res = send_message(ctrl, c, Q931_RELEASE_COMPLETE, release_ies);
	} else
		res = send_message(ctrl, c, Q931_RELEASE_COMPLETE, release_complete_ies);
	c->alive = 0;
	/* release the structure */
	res += q931_hangup(ctrl,c,cause);
	return res;
}

static int connect_acknowledge_ies[] = { -1 };

static int gr303_connect_acknowledge_ies[] = { Q931_CHANNEL_IDENT, -1 };

static int q931_connect_acknowledge(struct pri *ctrl, q931_call *c)
{
	if (ctrl->subchannel && !ctrl->bri) {
		if (ctrl->localtype == PRI_CPE)
			return send_message(ctrl, c, Q931_CONNECT_ACKNOWLEDGE, gr303_connect_acknowledge_ies);
	} else
		return send_message(ctrl, c, Q931_CONNECT_ACKNOWLEDGE, connect_acknowledge_ies);
	return 0;
}

int q931_hangup(struct pri *ctrl, q931_call *c, int cause)
{
	int disconnect = 1;
	int release_compl = 0;
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl,
			"NEW_HANGUP DEBUG: Calling q931_hangup, ourstate %s, peerstate %s\n",
			q931_call_state_str(c->ourcallstate),
			q931_call_state_str(c->peercallstate));
	if (!ctrl || !c)
		return -1;
	/* If mandatory IE was missing, insist upon that cause code */
	if (c->cause == PRI_CAUSE_MANDATORY_IE_MISSING)
		cause = c->cause;
	if (cause == 34 || cause == 44 || cause == 82 || cause == 1 || cause == 81) {
		/* We'll send RELEASE_COMPLETE with these causes */
		disconnect = 0;
		release_compl = 1;
	}
	if (cause == 6 || cause == 7 || cause == 26) {
		/* We'll send RELEASE with these causes */
		disconnect = 0;
	}
	/* All other causes we send with DISCONNECT */
	switch(c->ourcallstate) {
	case Q931_CALL_STATE_NULL:
		if (c->peercallstate == Q931_CALL_STATE_NULL)
			/* free the resources if we receive or send REL_COMPL */
			q931_destroycall(ctrl, c->cr);
		else if (c->peercallstate == Q931_CALL_STATE_RELEASE_REQUEST)
			q931_release_complete(ctrl,c,cause);
		break;
	case Q931_CALL_STATE_CALL_INITIATED:
		/* we sent SETUP */
	case Q931_CALL_STATE_OVERLAP_SENDING:
		/* received SETUP_ACKNOWLEDGE */
	case Q931_CALL_STATE_OUTGOING_CALL_PROCEEDING:
		/* received CALL_PROCEEDING */
	case Q931_CALL_STATE_CALL_DELIVERED:
		/* received ALERTING */
	case Q931_CALL_STATE_CALL_PRESENT:
		/* received SETUP */
	case Q931_CALL_STATE_CALL_RECEIVED:
		/* sent ALERTING */
	case Q931_CALL_STATE_CONNECT_REQUEST:
		/* sent CONNECT */
	case Q931_CALL_STATE_INCOMING_CALL_PROCEEDING:
		/* we sent CALL_PROCEEDING */
	case Q931_CALL_STATE_OVERLAP_RECEIVING:
		/* received SETUP_ACKNOWLEDGE */
		/* send DISCONNECT in general */
		if (c->peercallstate != Q931_CALL_STATE_NULL && c->peercallstate != Q931_CALL_STATE_DISCONNECT_REQUEST && c->peercallstate != Q931_CALL_STATE_DISCONNECT_INDICATION && c->peercallstate != Q931_CALL_STATE_RELEASE_REQUEST && c->peercallstate != Q931_CALL_STATE_RESTART_REQUEST && c->peercallstate != Q931_CALL_STATE_RESTART) {
			if (disconnect)
				q931_disconnect(ctrl,c,cause);
			else if (release_compl)
				q931_release_complete(ctrl,c,cause);
			else
				q931_release(ctrl,c,cause);
		} else 
			pri_error(ctrl,
				"Wierd, doing nothing but this shouldn't happen, ourstate %s, peerstate %s\n",
				q931_call_state_str(c->ourcallstate),
				q931_call_state_str(c->peercallstate));
		break;
	case Q931_CALL_STATE_ACTIVE:
		/* received CONNECT */
		q931_disconnect(ctrl,c,cause);
		break;
	case Q931_CALL_STATE_DISCONNECT_REQUEST:
		/* sent DISCONNECT */
		q931_release(ctrl,c,cause);
		break;
	case Q931_CALL_STATE_DISCONNECT_INDICATION:
		/* received DISCONNECT */
		if (c->peercallstate == Q931_CALL_STATE_DISCONNECT_REQUEST) {
			c->alive = 1;
			q931_release(ctrl,c,cause);
		}
		break;
	case Q931_CALL_STATE_RELEASE_REQUEST:
		/* sent RELEASE */
		/* don't do anything, waiting for RELEASE_COMPLETE */
		break;
	case Q931_CALL_STATE_RESTART:
	case Q931_CALL_STATE_RESTART_REQUEST:
		/* sent RESTART */
		pri_error(ctrl,
			"q931_hangup shouldn't be called in this state, ourstate %s, peerstate %s\n",
			q931_call_state_str(c->ourcallstate),
			q931_call_state_str(c->peercallstate));
		break;
	default:
		pri_error(ctrl,
			"We're not yet handling hanging up when our state is %d, contact support@digium.com, ourstate %s, peerstate %s\n",
			c->ourcallstate,
			q931_call_state_str(c->ourcallstate),
			q931_call_state_str(c->peercallstate));
		return -1;
	}
	/* we did handle hangup properly at this point */
	return 0;
}

static void q931_clr_subcommands(struct pri *ctrl)
{
	ctrl->subcmds.counter_subcmd = 0;
}

struct pri_subcommand *q931_alloc_subcommand(struct pri *ctrl)
{
	if (ctrl->subcmds.counter_subcmd < PRI_MAX_SUBCOMMANDS) {
		return &ctrl->subcmds.subcmd[ctrl->subcmds.counter_subcmd++];
	}

	return NULL;
}

static int prepare_to_handle_maintenance_message(struct pri *ctrl, q931_mh *mh, q931_call *c)
{
	if ((!ctrl) || (!mh) || (!c)) {
		return -1;
	}
	/* SERVICE messages are a superset of messages that can take b-channels
 	 * or entire d-channels in and out of service */
	switch(mh->msg) {
		case NATIONAL_SERVICE:
		case NATIONAL_SERVICE_ACKNOWLEDGE:
			c->channelno = -1;
			c->slotmap = -1;
			c->chanflags = 0;
			c->ds1no = 0;
			c->ri = -1;
			c->changestatus = -1;
			break;
		default:
			pri_error(ctrl, "!! Don't know how to pre-handle maintenance message type '%s' (%d)\n", maintenance_msg2str(mh->msg), mh->msg);
			return -1;
	}
	return 0;
}

static int prepare_to_handle_q931_message(struct pri *ctrl, q931_mh *mh, q931_call *c)
{
	if ((!ctrl) || (!mh) || (!c)) {
		return -1;
	}
	
	switch(mh->msg) {
	case Q931_RESTART:
		if (ctrl->debug & PRI_DEBUG_Q931_STATE)
			pri_message(ctrl, "-- Processing Q.931 Restart\n");
		/* Reset information */
		c->channelno = -1;
		c->slotmap = -1;
		c->chanflags = 0;
		c->ds1no = 0;
		c->ri = -1;
		break;
	case Q931_FACILITY:
		break;
	case Q931_SETUP:
		if (ctrl->debug & PRI_DEBUG_Q931_STATE)
			pri_message(ctrl, "-- Processing Q.931 Call Setup\n");
		c->channelno = -1;
		c->slotmap = -1;
		c->chanflags = 0;
		c->ds1no = 0;
		c->ri = -1;
		c->transcapability = -1;
		c->transmoderate = -1;
		c->transmultiple = -1;
		c->userl1 = -1;
		c->userl2 = -1;
		c->userl3 = -1;
		c->rateadaption = -1;

		q931_party_address_init(&c->called);
		q931_party_id_init(&c->local_id);
		q931_party_id_init(&c->remote_id);
		q931_party_redirecting_init(&c->redirecting);

		c->useruserprotocoldisc = -1; 
		c->useruserinfo[0] = '\0';
		c->complete = 0;
		c->nonisdn = 0;
		c->aoc_units = -1;
		c->reversecharge = -1;
		/* Fall through */
	case Q931_CONNECT:
	case Q931_ALERTING:
	case Q931_PROGRESS:
		c->useruserinfo[0] = '\0';
		c->cause = -1;
		/* Fall through */
	case Q931_CALL_PROCEEDING:
		c->progress = -1;
		c->progressmask = 0;
		break;
	case Q931_CONNECT_ACKNOWLEDGE:
		if (c->retranstimer)
			pri_schedule_del(ctrl, c->retranstimer);
		c->retranstimer = 0;
		break;
	case Q931_RELEASE:
	case Q931_DISCONNECT:
		c->cause = -1;
		c->causecode = -1;
		c->causeloc = -1;
		c->aoc_units = -1;
		if (c->retranstimer)
			pri_schedule_del(ctrl, c->retranstimer);
		c->retranstimer = 0;
		c->useruserinfo[0] = '\0';
		break;
	case Q931_RELEASE_COMPLETE:
		if (c->retranstimer)
			pri_schedule_del(ctrl, c->retranstimer);
		c->retranstimer = 0;
		c->useruserinfo[0] = '\0';
		/* Fall through */
	case Q931_STATUS:
		c->cause = -1;
		c->causecode = -1;
		c->causeloc = -1;
		c->sugcallstate = -1;
		c->aoc_units = -1;
		break;
	case Q931_RESTART_ACKNOWLEDGE:
		c->channelno = -1;
		break;
	case Q931_INFORMATION:
		/*
		 * Make sure that keypad and overlap digit buffers are empty in
		 * case they are not in the message.
		 */
		c->keypad_digits[0] = '\0';
		c->overlap_digits[0] = '\0';
		break;
	case Q931_STATUS_ENQUIRY:
		break;
	case Q931_SETUP_ACKNOWLEDGE:
		break;
	case Q931_NOTIFY:
		q931_party_number_init(&c->redirection_number);
		break;
	case Q931_USER_INFORMATION:
	case Q931_SEGMENT:
	case Q931_CONGESTION_CONTROL:
	case Q931_HOLD:
	case Q931_HOLD_ACKNOWLEDGE:
	case Q931_HOLD_REJECT:
	case Q931_RETRIEVE:
	case Q931_RETRIEVE_ACKNOWLEDGE:
	case Q931_RETRIEVE_REJECT:
	case Q931_RESUME:
	case Q931_RESUME_ACKNOWLEDGE:
	case Q931_RESUME_REJECT:
	case Q931_SUSPEND:
	case Q931_SUSPEND_ACKNOWLEDGE:
	case Q931_SUSPEND_REJECT:
		pri_error(ctrl, "!! Not yet handling pre-handle message type %s (%d)\n", msg2str(mh->msg), mh->msg);
		/* Fall through */
	default:
		pri_error(ctrl, "!! Don't know how to pre-handle message type %s (%d)\n", msg2str(mh->msg), mh->msg);
		q931_status(ctrl,c, PRI_CAUSE_MESSAGE_TYPE_NONEXIST);
		if (c->newcall) 
			q931_destroycall(ctrl,c->cr);
		return -1;
	}
	return 0;
}

int q931_receive(struct pri *ctrl, q931_h *h, int len)
{
	q931_mh *mh;
	q931_call *c;
	q931_ie *ie;
	unsigned int x;
	int y;
	int res;
	int r;
	int mandies[MAX_MAND_IES];
	int missingmand;
	int codeset, cur_codeset;
	int last_ie[8];
	int cref;

	memset(last_ie, 0, sizeof(last_ie));
	if (ctrl->debug & PRI_DEBUG_Q931_DUMP)
		q931_dump(ctrl, h, len, 0);
#ifdef LIBPRI_COUNTERS
	ctrl->q931_rxcount++;
#endif
	mh = (q931_mh *)(h->contents + h->crlen);
	if ((h->pd != ctrl->protodisc) && (h->pd != MAINTENANCE_PROTOCOL_DISCRIMINATOR_1) && (h->pd != MAINTENANCE_PROTOCOL_DISCRIMINATOR_2)) {
		pri_error(ctrl, "Warning: unknown/inappropriate protocol discriminator received (%02x/%d)\n", h->pd, h->pd);
		return 0;
	}
	if (((h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_1) || (h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_2)) && (!ctrl->service_message_support)) {
		/* Real service message support has not been enabled (and is OFF in libpri by default),
 		 * so we have to revert to the 'traditional' KLUDGE of changing byte 4 from a 0xf (SERVICE)
 		 * to a 0x7 (SERVICE ACKNOWLEDGE) */
		/* This is the weird maintenance stuff.  We majorly
		   KLUDGE this by changing byte 4 from a 0xf (SERVICE) 
		   to a 0x7 (SERVICE ACKNOWLEDGE) */
		h->raw[h->crlen + 2] -= 0x8;
		q931_xmit(ctrl, h, len, 1);
		return 0;
	}
	cref = q931_cr(h);
	c = q931_getcall(ctrl, cref);
	if (!c) {
		pri_error(ctrl, "Unable to locate call %d\n", cref);
		return -1;
	}
	/* Preliminary handling */
	if ((h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_1) || (h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_2)) {
		prepare_to_handle_maintenance_message(ctrl, mh, c);
	} else {
		prepare_to_handle_q931_message(ctrl, mh, c);
	}
	q931_clr_subcommands(ctrl);
	
	/* Handle IEs */
	memset(mandies, 0, sizeof(mandies));
	missingmand = 0;
	for (x=0;x<sizeof(msgs) / sizeof(msgs[0]); x++)  {
		if (msgs[x].msgnum == mh->msg) {
			memcpy(mandies, msgs[x].mandies, sizeof(mandies));
		}
	}
	x = 0;
	/* Do real IE processing */
	len -= (h->crlen + 3);
	codeset = cur_codeset = 0;
	while(len) {
		ie = (q931_ie *)(mh->data + x);
		for (y=0;y<MAX_MAND_IES;y++) {
			if (mandies[y] == Q931_FULL_IE(cur_codeset, ie->ie))
				mandies[y] = 0;
		}
		r = ielen(ie);
		if (r > len) {
			pri_error(ctrl, "XXX Message longer than it should be?? XXX\n");
			return -1;
		}
		/* Special processing for codeset shifts */
		switch (ie->ie & 0xf8) {
		case Q931_LOCKING_SHIFT:
			y = ie->ie & 7;	/* Requested codeset */
			/* Locking shifts couldn't go to lower codeset, and couldn't follows non-locking shifts - verify this */
			if ((cur_codeset != codeset) && (ctrl->debug & PRI_DEBUG_Q931_ANOMALY))
				pri_message(ctrl, "XXX Locking shift immediately follows non-locking shift (from %d through %d to %d) XXX\n", codeset, cur_codeset, y);
			if (y > 0) {
				if ((y < codeset) && (ctrl->debug & PRI_DEBUG_Q931_ANOMALY))
					pri_error(ctrl, "!! Trying to locked downshift codeset from %d to %d !!\n", codeset, y);
				codeset = cur_codeset = y;
			}
			else {
				/* Locking shift to codeset 0 is forbidden by all specifications */
				pri_error(ctrl, "!! Invalid locking shift to codeset 0 !!\n");
			}
			break;
		case Q931_NON_LOCKING_SHIFT:
			cur_codeset = ie->ie & 7;
			break;
		default:
			/* Sanity check for IE code order */
			if (!(ie->ie & 0x80)) {
				if (last_ie[cur_codeset] > ie->ie) {
					if ((ctrl->debug & PRI_DEBUG_Q931_ANOMALY))
						pri_message(ctrl, "XXX Out-of-order IE %d at codeset %d (last was %d)\n", ie->ie, cur_codeset, last_ie[cur_codeset]);
				}
				else
					last_ie[cur_codeset] = ie->ie;
			}
			/* Ignore non-locking shifts for TR41459-based signalling */
			switch (ctrl->switchtype) {
			case PRI_SWITCH_LUCENT5E:
			case PRI_SWITCH_ATT4ESS:
				if (cur_codeset != codeset) {
					if ((ctrl->debug & PRI_DEBUG_Q931_DUMP))
						pri_message(ctrl, "XXX Ignoring IE %d for temporary codeset %d XXX\n", ie->ie, cur_codeset);
					break;
				}
				/* Fall through */
			default:
				y = q931_handle_ie(cur_codeset, ctrl, c, mh->msg, ie);
				/* XXX Applicable to codeset 0 only? XXX */
				if (!cur_codeset && !(ie->ie & 0xf0) && (y < 0))
					mandies[MAX_MAND_IES - 1] = Q931_FULL_IE(cur_codeset, ie->ie);
			}
			/* Reset current codeset */
			cur_codeset = codeset;
		}
		x += r;
		len -= r;
	}
	missingmand = 0;
	for (x=0;x<MAX_MAND_IES;x++) {
		if (mandies[x]) {
			/* check if there is no channel identification when we're configured as network -> that's not an error */
			if (((ctrl->localtype != PRI_NETWORK) || (mh->msg != Q931_SETUP) || (mandies[x] != Q931_CHANNEL_IDENT)) &&
			     ((mh->msg != Q931_PROGRESS) || (mandies[x] != Q931_PROGRESS_INDICATOR))) {
				pri_error(ctrl, "XXX Missing handling for mandatory IE %d (cs%d, %s) XXX\n", Q931_IE_IE(mandies[x]), Q931_IE_CODESET(mandies[x]), ie2str(mandies[x]));
				missingmand++;
			}
		}
	}
	
	/* Post handling */
	if ((h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_1) || (h->pd == MAINTENANCE_PROTOCOL_DISCRIMINATOR_2)) {
		res = post_handle_maintenance_message(ctrl, mh, c);
	} else {
		res = post_handle_q931_message(ctrl, mh, c, missingmand);
	}
	return res;
}

static int post_handle_maintenance_message(struct pri *ctrl, struct q931_mh *mh, struct q931_call *c)
{
	/* Do some maintenance stuff */
	switch (mh->msg) {
	case NATIONAL_SERVICE:	
		if (c->channelno > 0) {
			ctrl->ev.e = PRI_EVENT_SERVICE;
			ctrl->ev.service.channel = c->channelno | (c->ds1no << 8);
			ctrl->ev.service.changestatus = 0x0f & c->changestatus;
		} else {
			switch (0x0f & c->changestatus) {
			case SERVICE_CHANGE_STATUS_INSERVICE:
				ctrl->ev.e = PRI_EVENT_DCHAN_UP;
				q921_dchannel_up(ctrl);
				break;
			case SERVICE_CHANGE_STATUS_OUTOFSERVICE:
				ctrl->ev.e = PRI_EVENT_DCHAN_DOWN;
				q921_dchannel_down(ctrl);
				break;
			default:
				pri_error(ctrl, "!! Don't know how to handle span service change status '%d'\n", (0x0f & c->changestatus));
				return -1;
			}
		}
		maintenance_service_ack(ctrl, c);
		return Q931_RES_HAVEEVENT;
	case NATIONAL_SERVICE_ACKNOWLEDGE:
		if (c->channelno > 0) {
			ctrl->ev.e = PRI_EVENT_SERVICE_ACK;
			ctrl->ev.service_ack.channel = c->channelno | (c->ds1no << 8);
			ctrl->ev.service_ack.changestatus = 0x0f & c->changestatus;
		} else {
			switch (0x0f & c->changestatus) {
			case SERVICE_CHANGE_STATUS_INSERVICE:
				ctrl->ev.e = PRI_EVENT_DCHAN_UP;
				q921_dchannel_up(ctrl);
				break;
			case SERVICE_CHANGE_STATUS_OUTOFSERVICE:
				ctrl->ev.e = PRI_EVENT_DCHAN_DOWN;
				q921_dchannel_down(ctrl);
				break;
			default:
				pri_error(ctrl, "!! Don't know how to handle span service change status '%d'\n", (0x0f & c->changestatus));
				return -1;
			}
		}
		return Q931_RES_HAVEEVENT;
	default:
		pri_error(ctrl, "!! Don't know how to post-handle maintenance message type %s (%d)\n", maintenance_msg2str(mh->msg), mh->msg);
	}
	return -1;
}

/*!
 * \internal
 * \brief Fill in the FACILITY event fields.
 *
 * \param ctrl D channel controller.
 * \param call Q.931 call leg
 *
 * \return Nothing
 */
static void q931_fill_facility_event(struct pri *ctrl, struct q931_call *call)
{
	ctrl->ev.e = PRI_EVENT_FACILITY;
	ctrl->ev.facility.subcmds = &ctrl->subcmds;
	ctrl->ev.facility.channel =
		call->channelno | (call->ds1no << 8) | (call->ds1explicit << 16);
	ctrl->ev.facility.cref = call->cr;
	ctrl->ev.facility.call = call;

	/* Need to do this for backward compatibility with struct pri_event_facname */
	libpri_copy_string(ctrl->ev.facility.callingname, call->remote_id.name.str,
		sizeof(ctrl->ev.facility.callingname));
	libpri_copy_string(ctrl->ev.facility.callingnum, call->remote_id.number.str,
		sizeof(ctrl->ev.facility.callingnum));
	ctrl->ev.facility.callingpres = q931_party_id_presentation(&call->remote_id);
	ctrl->ev.facility.callingplan = call->remote_id.number.plan;
}

/*!
 * \internal
 * \brief Process the decoded information in the Q.931 message.
 *
 * \param ctrl D channel controller.
 * \param mh Q.931 message header.
 * \param c Q.931 call leg.
 * \param missingmand Number of missing mandatory ie's.
 *
 * \retval 0 if no error or event.
 * \retval Q931_RES_HAVEEVENT if have an event.
 * \retval -1 on error.
 */
static int post_handle_q931_message(struct pri *ctrl, struct q931_mh *mh, struct q931_call *c, int missingmand)
{
	int res;
	struct apdu_event *cur = NULL;
	struct pri_subcommand *subcmd;

	switch(mh->msg) {
	case Q931_RESTART:
		if (missingmand) {
			q931_status(ctrl, c, PRI_CAUSE_MANDATORY_IE_MISSING);
			q931_destroycall(ctrl, c->cr);
			break;
		}
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_RESTART);
		c->peercallstate = Q931_CALL_STATE_RESTART_REQUEST;
		/* Send back the Restart Acknowledge */
		restart_ack(ctrl, c);
		/* Notify user of restart event */
		ctrl->ev.e = PRI_EVENT_RESTART;
		ctrl->ev.restart.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		return Q931_RES_HAVEEVENT;
	case Q931_SETUP:
		if (missingmand) {
			q931_release_complete(ctrl, c, PRI_CAUSE_MANDATORY_IE_MISSING);
			break;
		}
		/* Must be new call */
		if (!c->newcall) {
			break;
		}
		if (c->progressmask & PRI_PROG_CALLER_NOT_ISDN)
			c->nonisdn = 1;
		c->newcall = 0;
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_CALL_PRESENT);
		c->peercallstate = Q931_CALL_STATE_CALL_INITIATED;
		/* it's not yet a call since higher level can respond with RELEASE or RELEASE_COMPLETE */
		c->alive = 0;
		if (c->transmoderate != TRANS_MODE_64_CIRCUIT) {
			q931_release_complete(ctrl, c, PRI_CAUSE_BEARERCAPABILITY_NOTIMPL);
			break;
		}

		if (c->redirecting.from.number.valid && !c->redirecting.count) {
			/*
			 * This is most likely because the redirecting number came in
			 * with the redirecting ie only and not a DivertingLegInformation2.
			 */
			c->redirecting.count = 1;
		}
		if (c->redirecting.state == Q931_REDIRECTING_STATE_PENDING_TX_DIV_LEG_3) {
			/*
			 * Valid for Q.SIG and ETSI PRI/BRI-PTP modes:
			 * Setup the redirecting.to informtion so we can identify
			 * if the user wants to manually supply the COLR for this
			 * redirected to number if further redirects could happen.
			 *
			 * All the user needs to do is set the REDIRECTING(to-pres)
			 * to the COLR and REDIRECTING(to-num) = complete-dialed-number
			 * (i.e. CALLERID(dnid)) to be safe after determining that the
			 * incoming call was redirected by checking if the
			 * REDIRECTING(count) is nonzero.
			 */
			c->redirecting.to.number = c->called.number;
			c->redirecting.to.number.presentation =
				PRI_PRES_RESTRICTED | PRI_PRES_USER_NUMBER_UNSCREENED;
		}

		ctrl->ev.e = PRI_EVENT_RING;
		ctrl->ev.ring.subcmds = &ctrl->subcmds;
		ctrl->ev.ring.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);

		/* Calling party information */
		ctrl->ev.ring.callingpres = q931_party_id_presentation(&c->remote_id);
		ctrl->ev.ring.callingplan = c->remote_id.number.plan;
		if (c->remote_id.number.valid
			&& (c->remote_id.number.presentation == PRES_ALLOWED_NETWORK_NUMBER
				|| c->remote_id.number.presentation == PRES_PROHIB_NETWORK_NUMBER)) {
			ctrl->ev.ring.callingplanani = c->remote_id.number.plan;
			libpri_copy_string(ctrl->ev.ring.callingani, c->remote_id.number.str, sizeof(ctrl->ev.ring.callingani));
		} else {
			ctrl->ev.ring.callingplanani = -1;
			ctrl->ev.ring.callingani[0] = '\0';
		}
		libpri_copy_string(ctrl->ev.ring.callingnum, c->remote_id.number.str, sizeof(ctrl->ev.ring.callingnum));
		libpri_copy_string(ctrl->ev.ring.callingname, c->remote_id.name.str, sizeof(ctrl->ev.ring.callingname));
		libpri_copy_string(ctrl->ev.ring.callingsubaddr, c->callingsubaddr, sizeof(ctrl->ev.ring.callingsubaddr));

		ctrl->ev.ring.ani2 = c->ani2;

		/* Called party information */
		ctrl->ev.ring.calledplan = c->called.number.plan;
		libpri_copy_string(ctrl->ev.ring.callednum, c->called.number.str, sizeof(ctrl->ev.ring.callednum));

		/* Original called party information (For backward compatibility) */
		libpri_copy_string(ctrl->ev.ring.origcalledname, c->redirecting.orig_called.name.str, sizeof(ctrl->ev.ring.origcalledname));
		libpri_copy_string(ctrl->ev.ring.origcallednum, c->redirecting.orig_called.number.str, sizeof(ctrl->ev.ring.origcallednum));
		ctrl->ev.ring.callingplanorigcalled = c->redirecting.orig_called.number.plan;
		if (c->redirecting.orig_called.number.valid
			|| c->redirecting.orig_called.name.valid) {
			ctrl->ev.ring.origredirectingreason = c->redirecting.orig_reason;
		} else {
			ctrl->ev.ring.origredirectingreason = -1;
		}

		/* Redirecting from party information (For backward compatibility) */
		ctrl->ev.ring.callingplanrdnis = c->redirecting.from.number.plan;
		libpri_copy_string(ctrl->ev.ring.redirectingnum, c->redirecting.from.number.str, sizeof(ctrl->ev.ring.redirectingnum));
		libpri_copy_string(ctrl->ev.ring.redirectingname, c->redirecting.from.name.str, sizeof(ctrl->ev.ring.redirectingname));

		ctrl->ev.ring.redirectingreason = c->redirecting.reason;

		libpri_copy_string(ctrl->ev.ring.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.ring.useruserinfo));
		c->useruserinfo[0] = '\0';

		ctrl->ev.ring.flexible = ! (c->chanflags & FLAG_EXCLUSIVE);
		ctrl->ev.ring.cref = c->cr;
		ctrl->ev.ring.call = c;
		ctrl->ev.ring.layer1 = c->userl1;
		ctrl->ev.ring.complete = c->complete; 
		ctrl->ev.ring.ctype = c->transcapability;
		ctrl->ev.ring.progress = c->progress;
		ctrl->ev.ring.progressmask = c->progressmask;
		ctrl->ev.ring.reversecharge = c->reversecharge;

		if (c->redirecting.count) {
			subcmd = q931_alloc_subcommand(ctrl);
			if (subcmd) {
				/* Setup redirecting subcommand */
				subcmd->cmd = PRI_SUBCMD_REDIRECTING;
				q931_party_redirecting_copy_to_pri(&subcmd->u.redirecting,
					&c->redirecting);
			}
		}

		return Q931_RES_HAVEEVENT;
	case Q931_ALERTING:
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_CALL_DELIVERED);
		c->peercallstate = Q931_CALL_STATE_CALL_RECEIVED;
		ctrl->ev.e = PRI_EVENT_RINGING;
		ctrl->ev.ringing.subcmds = &ctrl->subcmds;
		ctrl->ev.ringing.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		ctrl->ev.ringing.cref = c->cr;
		ctrl->ev.ringing.call = c;
		ctrl->ev.ringing.progress = c->progress;
		ctrl->ev.ringing.progressmask = c->progressmask;

		libpri_copy_string(ctrl->ev.ringing.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.ringing.useruserinfo));
		c->useruserinfo[0] = '\0';

		cur = c->apdus;
		while (cur) {
			if (!cur->sent && cur->message == Q931_FACILITY) {
				q931_facility(ctrl, c);
				break;
			}
			cur = cur->next;
		}

		return Q931_RES_HAVEEVENT;
	case Q931_CONNECT:
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		if (c->ourcallstate == Q931_CALL_STATE_ACTIVE) {
			q931_status(ctrl, c, PRI_CAUSE_WRONG_MESSAGE);
			break;
		}
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_ACTIVE);
		c->peercallstate = Q931_CALL_STATE_CONNECT_REQUEST;

		ctrl->ev.e = PRI_EVENT_ANSWER;
		ctrl->ev.answer.subcmds = &ctrl->subcmds;
		ctrl->ev.answer.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		ctrl->ev.answer.cref = c->cr;
		ctrl->ev.answer.call = c;
		ctrl->ev.answer.progress = c->progress;
		ctrl->ev.answer.progressmask = c->progressmask;
		libpri_copy_string(ctrl->ev.answer.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.answer.useruserinfo));
		c->useruserinfo[0] = '\0';

		q931_connect_acknowledge(ctrl, c);

		if (c->justsignalling) {  /* Make sure WE release when we initiatie a signalling only connection */
			q931_release(ctrl, c, PRI_CAUSE_NORMAL_CLEARING);
			break;
		} else {
			c->incoming_ct_state = INCOMING_CT_STATE_IDLE;

			/* Setup connected line subcommand */
			subcmd = q931_alloc_subcommand(ctrl);
			if (subcmd) {
				subcmd->cmd = PRI_SUBCMD_CONNECTED_LINE;
				q931_party_id_copy_to_pri(&subcmd->u.connected_line.id, &c->remote_id);
			}

			return Q931_RES_HAVEEVENT;
		}
	case Q931_FACILITY:
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		switch (c->incoming_ct_state) {
		case INCOMING_CT_STATE_POST_CONNECTED_LINE:
			c->incoming_ct_state = INCOMING_CT_STATE_IDLE;
			subcmd = q931_alloc_subcommand(ctrl);
			if (subcmd) {
				subcmd->cmd = PRI_SUBCMD_CONNECTED_LINE;
				q931_party_id_copy_to_pri(&subcmd->u.connected_line.id, &c->remote_id);
			}
			break;
		default:
			break;
		}
		if (ctrl->subcmds.counter_subcmd) {
			q931_fill_facility_event(ctrl, c);
			return Q931_RES_HAVEEVENT;
		}
		break;
	case Q931_PROGRESS:
		if (missingmand) {
			q931_status(ctrl, c, PRI_CAUSE_MANDATORY_IE_MISSING);
			q931_destroycall(ctrl, c->cr);
			break;
		}
		ctrl->ev.e = PRI_EVENT_PROGRESS;
		ctrl->ev.proceeding.cause = c->cause;
		/* Fall through */
	case Q931_CALL_PROCEEDING:
		ctrl->ev.proceeding.subcmds = &ctrl->subcmds;
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		if ((c->ourcallstate != Q931_CALL_STATE_CALL_INITIATED) &&
		    (c->ourcallstate != Q931_CALL_STATE_OVERLAP_SENDING) && 
		    (c->ourcallstate != Q931_CALL_STATE_CALL_DELIVERED) && 
		    (c->ourcallstate != Q931_CALL_STATE_OUTGOING_CALL_PROCEEDING)) {
			q931_status(ctrl,c,PRI_CAUSE_WRONG_MESSAGE);
			break;
		}
		ctrl->ev.proceeding.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		if (mh->msg == Q931_CALL_PROCEEDING) {
			ctrl->ev.e = PRI_EVENT_PROCEEDING;
			UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_OUTGOING_CALL_PROCEEDING);
			c->peercallstate = Q931_CALL_STATE_INCOMING_CALL_PROCEEDING;
		}
		ctrl->ev.proceeding.progress = c->progress;
		ctrl->ev.proceeding.progressmask = c->progressmask;
		ctrl->ev.proceeding.cref = c->cr;
		ctrl->ev.proceeding.call = c;

		cur = c->apdus;
		while (cur) {
			if (!cur->sent && cur->message == Q931_FACILITY) {
				q931_facility(ctrl, c);
				break;
			}
			cur = cur->next;
		}
		return Q931_RES_HAVEEVENT;
	case Q931_CONNECT_ACKNOWLEDGE:
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		if (!(c->ourcallstate == Q931_CALL_STATE_CONNECT_REQUEST) &&
		    !(c->ourcallstate == Q931_CALL_STATE_ACTIVE &&
		      (ctrl->localtype == PRI_NETWORK || ctrl->switchtype == PRI_SWITCH_QSIG))) {
			q931_status(ctrl,c,PRI_CAUSE_WRONG_MESSAGE);
			break;
		}
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_ACTIVE);
		c->peercallstate = Q931_CALL_STATE_ACTIVE;
		break;
	case Q931_STATUS:
		if (missingmand) {
			q931_status(ctrl, c, PRI_CAUSE_MANDATORY_IE_MISSING);
			q931_destroycall(ctrl, c->cr);
			break;
		}
		if (c->newcall) {
			if (c->cr & 0x7fff)
				q931_release_complete(ctrl,c,PRI_CAUSE_WRONG_CALL_STATE);
			break;
		}
		/* Do nothing */
		/* Also when the STATUS asks for the call of an unexisting reference send RELEASE_COMPL */
		if ((ctrl->debug & PRI_DEBUG_Q931_ANOMALY) &&
		    (c->cause != PRI_CAUSE_INTERWORKING)) 
			pri_error(ctrl, "Received unsolicited status: %s\n", pri_cause2str(c->cause));
		/* Workaround for S-12 ver 7.3 - it responds for invalid/non-implemented IEs at SETUP with null call state */
#if 0
		if (!c->sugcallstate && (c->ourcallstate != Q931_CALL_STATE_CALL_INITIATED)) {
#else
		/* Remove "workaround" since it breaks certification testing.  If we receive a STATUS message of call state
		 * NULL and we are not in the call state NULL we must clear resources and return to the call state to pass
		 * testing.  See section 5.8.11 of Q.931 */

		if (!c->sugcallstate) {
#endif
			ctrl->ev.hangup.subcmds = &ctrl->subcmds;
			ctrl->ev.hangup.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
			ctrl->ev.hangup.cause = c->cause;
			ctrl->ev.hangup.cref = c->cr;
			ctrl->ev.hangup.call = c;
			ctrl->ev.hangup.aoc_units = c->aoc_units;
			libpri_copy_string(ctrl->ev.hangup.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.hangup.useruserinfo));
			/* Free resources */
			UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_NULL);
			c->peercallstate = Q931_CALL_STATE_NULL;
			if (c->alive) {
				ctrl->ev.e = PRI_EVENT_HANGUP;
				res = Q931_RES_HAVEEVENT;
				c->alive = 0;
			} else if (c->sendhangupack) {
				res = Q931_RES_HAVEEVENT;
				ctrl->ev.e = PRI_EVENT_HANGUP_ACK;
				q931_hangup(ctrl, c, c->cause);
			} else {
				q931_hangup(ctrl, c, c->cause);
				res = 0;
			}
			if (res)
				return res;
		}
		break;
	case Q931_RELEASE_COMPLETE:
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_NULL);
		c->peercallstate = Q931_CALL_STATE_NULL;
		ctrl->ev.hangup.subcmds = &ctrl->subcmds;
		ctrl->ev.hangup.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		ctrl->ev.hangup.cause = c->cause;
		ctrl->ev.hangup.cref = c->cr;
		ctrl->ev.hangup.call = c;
		ctrl->ev.hangup.aoc_units = c->aoc_units;
		libpri_copy_string(ctrl->ev.hangup.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.hangup.useruserinfo));
		c->useruserinfo[0] = '\0';
		/* Free resources */
		if (c->alive) {
			ctrl->ev.e = PRI_EVENT_HANGUP;
			res = Q931_RES_HAVEEVENT;
			c->alive = 0;
		} else if (c->sendhangupack) {
			res = Q931_RES_HAVEEVENT;
			ctrl->ev.e = PRI_EVENT_HANGUP_ACK;
			pri_hangup(ctrl, c, c->cause);
		} else
			res = 0;
		if (res)
			return res;
		else
			q931_hangup(ctrl,c,c->cause);
		break;
	case Q931_RELEASE:
		if (missingmand) {
			/* Force cause to be mandatory IE missing */
			c->cause = PRI_CAUSE_MANDATORY_IE_MISSING;
		}
		if (c->ourcallstate == Q931_CALL_STATE_RELEASE_REQUEST) 
			c->peercallstate = Q931_CALL_STATE_NULL;
		else {
			c->peercallstate = Q931_CALL_STATE_RELEASE_REQUEST;
		}
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_NULL);
		ctrl->ev.e = PRI_EVENT_HANGUP;
		ctrl->ev.hangup.subcmds = &ctrl->subcmds;
		ctrl->ev.hangup.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		ctrl->ev.hangup.cause = c->cause;
		ctrl->ev.hangup.cref = c->cr;
		ctrl->ev.hangup.call = c;
		ctrl->ev.hangup.aoc_units = c->aoc_units;
		libpri_copy_string(ctrl->ev.hangup.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.hangup.useruserinfo));
		c->useruserinfo[0] = '\0';
		/* Don't send release complete if they send us release 
		   while we sent it, assume a NULL state */
		if (c->newcall)
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
		else 
			return Q931_RES_HAVEEVENT;
		break;
	case Q931_DISCONNECT:
		if (missingmand) {
			/* Still let user call release */
			c->cause = PRI_CAUSE_MANDATORY_IE_MISSING;
		}
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_DISCONNECT_INDICATION);
		c->peercallstate = Q931_CALL_STATE_DISCONNECT_REQUEST;
		c->sendhangupack = 1;

		/* wait for a RELEASE so that sufficient time has passed
		   for the inband audio to be heard */
		if (ctrl->acceptinbanddisconnect && (c->progressmask & PRI_PROG_INBAND_AVAILABLE))
			break;

		/* Return such an event */
		ctrl->ev.e = PRI_EVENT_HANGUP_REQ;
		ctrl->ev.hangup.subcmds = &ctrl->subcmds;
		ctrl->ev.hangup.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		ctrl->ev.hangup.cause = c->cause;
		ctrl->ev.hangup.cref = c->cr;
		ctrl->ev.hangup.call = c;
		ctrl->ev.hangup.aoc_units = c->aoc_units;
		libpri_copy_string(ctrl->ev.hangup.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.hangup.useruserinfo));
		c->useruserinfo[0] = '\0';
		if (c->alive)
			return Q931_RES_HAVEEVENT;
		else
			q931_hangup(ctrl,c,c->cause);
		break;
	case Q931_RESTART_ACKNOWLEDGE:
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_NULL);
		c->peercallstate = Q931_CALL_STATE_NULL;
		ctrl->ev.e = PRI_EVENT_RESTART_ACK;
		ctrl->ev.restartack.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		return Q931_RES_HAVEEVENT;
	case Q931_INFORMATION:
		/* XXX We're handling only INFORMATION messages that contain
		       overlap dialing received digit
		       +  the "Complete" msg which is basically an EOF on further digits
		   XXX */
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		if (c->ourcallstate != Q931_CALL_STATE_OVERLAP_RECEIVING) {
			ctrl->ev.e = PRI_EVENT_KEYPAD_DIGIT;
			ctrl->ev.digit.subcmds = &ctrl->subcmds;
			ctrl->ev.digit.call = c;
			ctrl->ev.digit.channel = c->channelno | (c->ds1no << 8);
			libpri_copy_string(ctrl->ev.digit.digits, c->keypad_digits, sizeof(ctrl->ev.digit.digits));
			return Q931_RES_HAVEEVENT;
		}
		ctrl->ev.e = PRI_EVENT_INFO_RECEIVED;
		ctrl->ev.ring.subcmds = &ctrl->subcmds;
		ctrl->ev.ring.call = c;
		ctrl->ev.ring.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		libpri_copy_string(ctrl->ev.ring.callednum, c->overlap_digits, sizeof(ctrl->ev.ring.callednum));
		libpri_copy_string(ctrl->ev.ring.callingsubaddr, c->callingsubaddr, sizeof(ctrl->ev.ring.callingsubaddr));
		ctrl->ev.ring.complete = c->complete; 	/* this covers IE 33 (Sending Complete) */
		return Q931_RES_HAVEEVENT;
	case Q931_STATUS_ENQUIRY:
		if (c->newcall) {
			q931_release_complete(ctrl, c, PRI_CAUSE_INVALID_CALL_REFERENCE);
		} else
			q931_status(ctrl,c, 0);
		break;
	case Q931_SETUP_ACKNOWLEDGE:
		if (c->newcall) {
			q931_release_complete(ctrl,c,PRI_CAUSE_INVALID_CALL_REFERENCE);
			break;
		}
		UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_OVERLAP_SENDING);
		c->peercallstate = Q931_CALL_STATE_OVERLAP_RECEIVING;
		ctrl->ev.e = PRI_EVENT_SETUP_ACK;
		ctrl->ev.setup_ack.subcmds = &ctrl->subcmds;
		ctrl->ev.setup_ack.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
		ctrl->ev.setup_ack.call = c;

		cur = c->apdus;
		while (cur) {
			if (!cur->sent && cur->message == Q931_FACILITY) {
				q931_facility(ctrl, c);
				break;
			}
			cur = cur->next;
		}

		return Q931_RES_HAVEEVENT;
	case Q931_NOTIFY:
		res = 0;
		switch (c->notify) {
		case PRI_NOTIFY_CALL_DIVERTING:
			if (c->redirection_number.valid) {
				c->redirecting.to.number = c->redirection_number;
				if (c->redirecting.count < PRI_MAX_REDIRECTS) {
					++c->redirecting.count;
				}
				switch (c->ourcallstate) {
				case Q931_CALL_STATE_CALL_DELIVERED:
					/* Call is deflecting after we have seen an ALERTING message */
					c->redirecting.reason = PRI_REDIR_FORWARD_ON_NO_REPLY;
					break;
				default:
					/* Call is deflecting for call forwarding unconditional or busy reason. */
					c->redirecting.reason = PRI_REDIR_UNKNOWN;
					break;
				}

				/* Setup redirecting subcommand */
				subcmd = q931_alloc_subcommand(ctrl);
				if (subcmd) {
					subcmd->cmd = PRI_SUBCMD_REDIRECTING;
					q931_party_redirecting_copy_to_pri(&subcmd->u.redirecting,
						&c->redirecting);
				}
			}

			if (ctrl->subcmds.counter_subcmd) {
				q931_fill_facility_event(ctrl, c);
				res = Q931_RES_HAVEEVENT;
			}
			break;
		case PRI_NOTIFY_TRANSFER_ALERTING:
		case PRI_NOTIFY_TRANSFER_ACTIVE:
			if (c->redirection_number.valid
				&& q931_party_number_cmp(&c->remote_id.number, &c->redirection_number)) {
				/* The remote party information changed. */
				c->remote_id.number = c->redirection_number;

				/* Setup connected line subcommand */
				subcmd = q931_alloc_subcommand(ctrl);
				if (subcmd) {
					subcmd->cmd = PRI_SUBCMD_CONNECTED_LINE;
					q931_party_id_copy_to_pri(&subcmd->u.connected_line.id,
						&c->remote_id);
				}
			}

			if (ctrl->subcmds.counter_subcmd) {
				q931_fill_facility_event(ctrl, c);
				res = Q931_RES_HAVEEVENT;
			}
			break;
		default:
			ctrl->ev.e = PRI_EVENT_NOTIFY;
			ctrl->ev.notify.subcmds = &ctrl->subcmds;
			ctrl->ev.notify.channel = c->channelno;
			ctrl->ev.notify.info = c->notify;
			res = Q931_RES_HAVEEVENT;
			break;
		}
		return res;
	case Q931_USER_INFORMATION:
	case Q931_SEGMENT:
	case Q931_CONGESTION_CONTROL:
	case Q931_HOLD:
	case Q931_HOLD_ACKNOWLEDGE:
	case Q931_HOLD_REJECT:
	case Q931_RETRIEVE:
	case Q931_RETRIEVE_ACKNOWLEDGE:
	case Q931_RETRIEVE_REJECT:
	case Q931_RESUME:
	case Q931_RESUME_ACKNOWLEDGE:
	case Q931_RESUME_REJECT:
	case Q931_SUSPEND:
	case Q931_SUSPEND_ACKNOWLEDGE:
	case Q931_SUSPEND_REJECT:
		pri_error(ctrl, "!! Not yet handling post-handle message type %s (%d)\n", msg2str(mh->msg), mh->msg);
		/* Fall through */
	default:
		pri_error(ctrl, "!! Don't know how to post-handle message type %s (%d)\n", msg2str(mh->msg), mh->msg);
		q931_status(ctrl,c, PRI_CAUSE_MESSAGE_TYPE_NONEXIST);
		if (c->newcall) 
			q931_destroycall(ctrl,c->cr);
		return -1;
	}
	return 0;
}

/* Clear a call, although we did not receive any hangup notification. */
static int pri_internal_clear(void *data)
{
	struct q931_call *c = data;
	struct pri *ctrl = c->pri;
	int res;

	if (c->retranstimer)
		pri_schedule_del(ctrl, c->retranstimer);
	c->retranstimer = 0;
	c->useruserinfo[0] = '\0';
	c->cause = -1;
	c->causecode = -1;
	c->causeloc = -1;
	c->sugcallstate = -1;
	c->aoc_units = -1;

	UPDATE_OURCALLSTATE(ctrl, c, Q931_CALL_STATE_NULL);
	c->peercallstate = Q931_CALL_STATE_NULL;
	ctrl->ev.hangup.subcmds = &ctrl->subcmds;
	ctrl->ev.hangup.channel = c->channelno | (c->ds1no << 8) | (c->ds1explicit << 16);
	ctrl->ev.hangup.cause = c->cause;      		
	ctrl->ev.hangup.cref = c->cr;          		
	ctrl->ev.hangup.call = c;              		
	ctrl->ev.hangup.aoc_units = c->aoc_units;
	libpri_copy_string(ctrl->ev.hangup.useruserinfo, c->useruserinfo, sizeof(ctrl->ev.hangup.useruserinfo));

	/* Free resources */
	if (c->alive) {
		ctrl->ev.e = PRI_EVENT_HANGUP;
		res = Q931_RES_HAVEEVENT;
		c->alive = 0;
	} else if (c->sendhangupack) {
		res = Q931_RES_HAVEEVENT;
		ctrl->ev.e = PRI_EVENT_HANGUP_ACK;
		q931_hangup(ctrl, c, c->cause);
	} else {
		res = 0;
		q931_hangup(ctrl, c, c->cause);
	}

	return res;
}

/* Handle T309 timeout for an active call. */
static void pri_dl_down_timeout(void *data)
{
	struct q931_call *c = data;
	struct pri *ctrl = c->pri;
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl, DBGHEAD "Timed out waiting for data link re-establishment\n", DBGINFO);

	c->cause = PRI_CAUSE_DESTINATION_OUT_OF_ORDER;
	if (pri_internal_clear(c) == Q931_RES_HAVEEVENT)
		ctrl->schedev = 1;
}

/* Handle Layer 2 down event for a non active call. */
static void pri_dl_down_cancelcall(void *data)
{
	struct q931_call *c = data;
	struct pri *ctrl = c->pri;
	if (ctrl->debug & PRI_DEBUG_Q931_STATE)
		pri_message(ctrl, DBGHEAD "Cancel non active call after data link failure\n", DBGINFO);

	c->cause = PRI_CAUSE_DESTINATION_OUT_OF_ORDER;
	if (pri_internal_clear(c) == Q931_RES_HAVEEVENT)
		ctrl->schedev = 1;
}

/* Receive an indication from Layer 2 */
void q931_dl_indication(struct pri *ctrl, int event)
{
	q931_call *cur = NULL;

	/* Just return if T309 is not enabled. */
	if (!ctrl || ctrl->timers[PRI_TIMER_T309] < 0)
		return;

	switch (event) {
	case PRI_EVENT_DCHAN_DOWN:
		pri_message(ctrl, DBGHEAD "link is DOWN\n", DBGINFO);
		cur = *ctrl->callpool;
		while(cur) {
			if (cur->ourcallstate == Q931_CALL_STATE_ACTIVE) {
				/* For a call in Active state, activate T309 only if there is no timer already running. */
				if (!cur->retranstimer) {
					pri_message(ctrl, DBGHEAD "activate T309 for call %d on channel %d\n", DBGINFO, cur->cr, cur->channelno);
					cur->retranstimer = pri_schedule_event(ctrl, ctrl->timers[PRI_TIMER_T309], pri_dl_down_timeout, cur);
				}
			} else if (cur->ourcallstate != Q931_CALL_STATE_NULL) {
				/* For a call that is not in Active state, schedule internal clearing of the call 'ASAP' (delay 0). */
				pri_message(ctrl, DBGHEAD "cancel call %d on channel %d in state %d (%s)\n", DBGINFO,
					cur->cr, cur->channelno, cur->ourcallstate,
					q931_call_state_str(cur->ourcallstate));
				if (cur->retranstimer)
					pri_schedule_del(ctrl, cur->retranstimer);
				cur->retranstimer = pri_schedule_event(ctrl, 0, pri_dl_down_cancelcall, cur);
			}
			cur = cur->next;
		}
		break;
	case PRI_EVENT_DCHAN_UP:
		pri_message(ctrl, DBGHEAD "link is UP\n", DBGINFO);
		cur = *ctrl->callpool;
		while(cur) {
			if (cur->ourcallstate == Q931_CALL_STATE_ACTIVE && cur->retranstimer) {
				pri_message(ctrl, DBGHEAD "cancel T309 for call %d on channel %d\n", DBGINFO, cur->cr, cur->channelno);
				pri_schedule_del(ctrl, cur->retranstimer);
				cur->retranstimer = 0;
				q931_status(ctrl, cur, PRI_CAUSE_NORMAL_UNSPECIFIED);
			} else if (cur->ourcallstate != Q931_CALL_STATE_NULL &&
				cur->ourcallstate != Q931_CALL_STATE_DISCONNECT_REQUEST &&
				cur->ourcallstate != Q931_CALL_STATE_DISCONNECT_INDICATION &&
				cur->ourcallstate != Q931_CALL_STATE_RELEASE_REQUEST) {

				/* The STATUS message sent here is not required by Q.931, but it may help anyway. */
				q931_status(ctrl, cur, PRI_CAUSE_NORMAL_UNSPECIFIED);
			}
			cur = cur->next;
		}
		break;
	default:
		pri_message(ctrl, DBGHEAD "unexpected event %d.\n", DBGINFO, event);
	}
}

int q931_call_getcrv(struct pri *ctrl, q931_call *call, int *callmode)
{
	if (callmode)
		*callmode = call->cr & 0x7;
	return ((call->cr & 0x7fff) >> 3);
}

int q931_call_setcrv(struct pri *ctrl, q931_call *call, int crv, int callmode)
{
	call->cr = (crv << 3) & 0x7fff;
	call->cr |= (callmode & 0x7);
	return 0;
}
