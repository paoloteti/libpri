/*
 * libpri: An implementation of Primary Rate ISDN
 *
 * Written by Mark Spencer <markster@digium.com>
 *
 * Copyright (C) 2001, Digium, Inc.
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
 
#ifndef _PRI_INTERNAL_H
#define _PRI_INTERNAL_H

#include <stddef.h>
#include <sys/time.h>
#include "pri_q921.h"
#include "pri_q931.h"

#define ARRAY_LEN(arr)	(sizeof(arr) / sizeof((arr)[0]))

#define DBGHEAD __FILE__ ":%d %s: "
#define DBGINFO __LINE__,__PRETTY_FUNCTION__

struct pri_sched {
	struct timeval when;
	void (*callback)(void *data);
	void *data;
};

/*! Maximum number of scheduled events active at the same time. */
#define MAX_SCHED 128

/*! \brief D channel controller structure */
struct pri {
	int fd;				/* File descriptor for D-Channel */
	pri_io_cb read_func;		/* Read data callback */
	pri_io_cb write_func;		/* Write data callback */
	void *userdata;
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
	unsigned int bri:1;
	unsigned int acceptinbanddisconnect:1;	/* Should we allow inband progress after DISCONNECT? */
	unsigned int hold_support:1;/* TRUE if upper layer supports call hold. */
	unsigned int deflection_support:1;/* TRUE if upper layer supports call deflection/rerouting. */

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
	int sabme_count;	/* SABME retransmit counter for BRI */
	int t203_timer;		/* Max idle time */
	int t202_timer;
	int n202_counter;
	int ri;
	int t200_timer;		/* T-200 retransmission timer */
	/* All ISDN Timer values */
	int timers[PRI_MAX_TIMERS];

	/* Used by scheduler */
	struct timeval tv;
	int schedev;
	pri_event ev;		/* Static event thingy */
	/*! Subcommands for static event thingy. */
	struct pri_subcommands subcmds;
	
	/* Q.921 Re-transmission queue */
	struct q921_frame *txqueue;
	
	/* Q.931 calls */
	q931_call **callpool;
	q931_call *localpool;

	/* do we do overlap dialing */
	int overlapdial;

	/* do we support SERVICE messages */
	int service_message_support;

	/* do not skip channel 16 */
	int chan_mapping_logical;

#ifdef LIBPRI_COUNTERS
	/* q921/q931 packet counters */
	unsigned int q921_txcount;
	unsigned int q921_rxcount;
	unsigned int q931_txcount;
	unsigned int q931_rxcount;
#endif

	short last_invoke;	/* Last ROSE invoke ID */
	unsigned char sendfacility;
};

/*! \brief Maximum name length plus null terminator (From ECMA-164) */
#define PRI_MAX_NAME_LEN		(50 + 1)

/*! \brief Q.SIG name information. */
struct q931_party_name {
	/*! \brief TRUE if name data is valid */
	unsigned char valid;
	/*!
	 * \brief Q.931 presentation-indicator encoded field
	 * \note Must tollerate the Q.931 screening-indicator field values being present.
	 */
	unsigned char presentation;
	/*!
	 * \brief Character set the name is using.
	 * \details
	 * unknown(0),
	 * iso8859-1(1),
	 * enum-value-withdrawn-by-ITU-T(2)
	 * iso8859-2(3),
	 * iso8859-3(4),
	 * iso8859-4(5),
	 * iso8859-5(6),
	 * iso8859-7(7),
	 * iso10646-BmpString(8),
	 * iso10646-utf-8String(9)
	 */
	unsigned char char_set;
	/*! \brief Name data with null terminator. */
	char str[PRI_MAX_NAME_LEN];
};

/*! \brief Maximum phone number (address) length plus null terminator */
#define PRI_MAX_NUMBER_LEN		(31 + 1)

struct q931_party_number {
	/*! \brief TRUE if number data is valid */
	unsigned char valid;
	/*! \brief Q.931 presentation-indicator and screening-indicator encoded fields */
	unsigned char presentation;
	/*! \brief Q.931 Type-Of-Number and numbering-plan encoded fields */
	unsigned char plan;
	/*! \brief Number data with terminator. */
	char str[PRI_MAX_NUMBER_LEN];
};

/*! \brief Maximum subaddress length plus null terminator */
#define PRI_MAX_SUBADDRESS_LEN	(20 + 1)

struct q931_party_subaddress {
	/*! \brief TRUE if the subaddress information is valid/present */
	unsigned char valid;
	/*!
	 * \brief Subaddress type.
	 * \details
	 * nsap(0),
	 * user_specified(2)
	 */
	unsigned char type;
	/*!
	 * \brief TRUE if odd number of address signals
	 * \note The odd/even indicator is used when the type of subaddress is
	 * user_specified and the coding is BCD.
	 */
	unsigned char odd_even_indicator;
	/*! \brief Length of the subaddress data */
	unsigned char length;
	/*!
	 * \brief Subaddress data with null terminator.
	 * \note The null terminator is a convenience only since the data could be
	 * BCD/binary and thus have a null byte as part of the contents.
	 */
	unsigned char data[PRI_MAX_SUBADDRESS_LEN];
};

struct q931_party_address {
	/*! \brief Subscriber phone number */
	struct q931_party_number number;
	/*! \brief Subscriber subaddress */
	struct q931_party_subaddress subaddress;
};

/*! \brief Information needed to identify an endpoint in a call. */
struct q931_party_id {
	/*! \brief Subscriber name */
	struct q931_party_name name;
	/*! \brief Subscriber phone number */
	struct q931_party_number number;
	/*! \brief Subscriber subaddress */
	struct q931_party_subaddress subaddress;
};

enum Q931_REDIRECTING_STATE {
	/*!
	 * \details
	 * CDO-Idle/CDF-Inv-Idle
	 */
	Q931_REDIRECTING_STATE_IDLE,
	/*!
	 * \details
	 * CDF-Inv-Wait - A DivLeg2 has been received and
	 * we are waiting for valid presentation restriction information to send.
	 */
	Q931_REDIRECTING_STATE_PENDING_TX_DIV_LEG_3,
	/*!
	 * \details
	 * CDO-Divert - A DivLeg1 has been received and
	 * we are waiting for the presentation restriction information to come in.
	 */
	Q931_REDIRECTING_STATE_EXPECTING_RX_DIV_LEG_3,
};

/*!
 * \brief Do not increment above this count.
 * \details
 * It is not our responsibility to enforce the maximum number of redirects.
 * However, we cannot allow an increment past this number without breaking things.
 * Besides, more than 255 redirects is probably not a good thing.
 */
#define PRI_MAX_REDIRECTS	0xFF

/*! \brief Redirecting information struct */
struct q931_party_redirecting {
	enum Q931_REDIRECTING_STATE state;
	/*! \brief Who is redirecting the call (Sent to the party the call is redirected toward) */
	struct q931_party_id from;
	/*! \brief Call is redirecting to a new party (Sent to the caller) */
	struct q931_party_id to;
	/*! Originally called party (in cases of multiple redirects) */
	struct q931_party_id orig_called;
	/*!
	 * \brief Number of times the call was redirected
	 * \note The call is being redirected if the count is non-zero.
	 */
	unsigned char count;
	/*! Original reason for redirect (in cases of multiple redirects) */
	unsigned char orig_reason;
	/*! \brief Redirection reasons */
	unsigned char reason;
};

/*! \brief New call setup parameter structure */
struct pri_sr {
	int transmode;
	int channel;
	int exclusive;
	int nonisdn;
	struct q931_party_redirecting redirecting;
	struct q931_party_id caller;
	struct q931_party_address called;
	int userl1;
	int numcomplete;
	int cis_call;
	int cis_auto_disconnect;
	const char *useruserinfo;
	const char *keypad_digits;
	int transferable;
	int reversecharge;
};

/* Internal switch types */
#define PRI_SWITCH_GR303_EOC_PATH	19
#define PRI_SWITCH_GR303_TMC_SWITCHING	20

#define Q931_MAX_TEI	8

struct apdu_event {
	int message;			/* What message to send the ADPU in */
	void (*callback)(void *data);	/* Callback function for when response is received */
	void *data;			/* Data to callback */
	unsigned char apdu[255];			/* ADPU to send */
	int apdu_len; 			/* Length of ADPU */
	int sent;  			/* Have we been sent already? */
	struct apdu_event *next;	/* Linked list pointer */
};

/*! \brief Incoming call transfer states. */
enum INCOMING_CT_STATE {
	/*!
	 * \details
	 * Incoming call transfer is not active.
	 */
	INCOMING_CT_STATE_IDLE,
	/*!
	 * \details
	 * We have seen an incoming CallTransferComplete(alerting)
	 * so we are waiting for the expected CallTransferActive
	 * before updating the connected line about the remote party id.
	 */
	INCOMING_CT_STATE_EXPECT_CT_ACTIVE,
	/*!
	 * \details
	 * A call transfer message came in that updated the remote party id
	 * that we need to post a connected line update.
	 */
	INCOMING_CT_STATE_POST_CONNECTED_LINE
};

/*! Call hold supplementary states. */
enum Q931_HOLD_STATE {
	/*! \brief No call hold activity. */
	Q931_HOLD_STATE_IDLE,
	/*! \brief Request made to hold call. */
	Q931_HOLD_STATE_HOLD_REQ,
	/*! \brief Request received to hold call. */
	Q931_HOLD_STATE_HOLD_IND,
	/*! \brief Call is held. */
	Q931_HOLD_STATE_CALL_HELD,
	/*! \brief Request made to retrieve call. */
	Q931_HOLD_STATE_RETRIEVE_REQ,
	/*! \brief Request received to retrieve call. */
	Q931_HOLD_STATE_RETRIEVE_IND,
};

/* q931_call datastructure */
struct q931_call {
	struct pri *pri;	/* PRI */
	int cr;				/* Call Reference */
	q931_call *next;
	/* Slotmap specified (bitmap of channels 31/24-1) (Channel Identifier IE) (-1 means not specified) */
	int slotmap;
	/* An explicit channel (Channel Identifier IE) (-1 means not specified) */
	int channelno;
	/* An explicit DS1 (-1 means not specified) */
	int ds1no;
	/* Whether or not the ds1 is explicitly identified or implicit.  If implicit
	   the bchan is on the same span as the current active dchan (NFAS) */
	int ds1explicit;
	/* Channel flags (0 means none retrieved) */
	int chanflags;
	
	int alive;			/* Whether or not the call is alive */
	int acked;			/* Whether setup has been acked or not */
	int sendhangupack;	/* Whether or not to send a hangup ack */
	int proc;			/* Whether we've sent a call proceeding / alerting */
	
	int ri;				/* Restart Indicator (Restart Indicator IE) */

	/* Bearer Capability */
	int transcapability;
	int transmoderate;
	int transmultiple;
	int userl1;
	int userl2;
	int userl3;
	int rateadaption;

	/*!
	 * \brief TRUE if the call is a Call Independent Signalling connection.
	 * \note The call has no B channel associated with it. (Just signalling)
	 */
	int cis_call;
	/*! \brief TRUE if we will auto disconnect the cis_call we originated. */
	int cis_auto_disconnect;

	int progcode;			/* Progress coding */
	int progloc;			/* Progress Location */	
	int progress;			/* Progress indicator */
	int progressmask;		/* Progress Indicator bitmask */
	
	int notify;				/* Notification indicator. */
	
	int causecode;			/* Cause Coding */
	int causeloc;			/* Cause Location */
	int cause;				/* Cause of clearing */
	
	enum Q931_CALL_STATE peercallstate;	/* Call state of peer as reported */
	enum Q931_CALL_STATE ourcallstate;	/* Our call state */
	enum Q931_CALL_STATE sugcallstate;	/* Status call state */

	int ani2;               /* ANI II */

	/*! Buffer for digits that come in KEYPAD_FACILITY */
	char keypad_digits[32 + 1];

	/*! Current dialed digits to be sent or just received. */
	char overlap_digits[PRI_MAX_NUMBER_LEN];

	/*!
	 * \brief Local party ID
	 * \details
	 * The Caller-ID and connected-line ID are just roles the local and remote party
	 * play while a call is being established.  Which roll depends upon the direction
	 * of the call.
	 * Outgoing party info is to identify the local party to the other end.
	 *    (Caller-ID for originated or connected-line for answered calls.)
	 * Incoming party info is to identify the remote party to us.
	 *    (Caller-ID for answered or connected-line for originated calls.)
	 */
	struct q931_party_id local_id;
	/*!
	 * \brief Remote party ID
	 * \details
	 * The Caller-ID and connected-line ID are just roles the local and remote party
	 * play while a call is being established.  Which roll depends upon the direction
	 * of the call.
	 * Outgoing party info is to identify the local party to the other end.
	 *    (Caller-ID for originated or connected-line for answered calls.)
	 * Incoming party info is to identify the remote party to us.
	 *    (Caller-ID for answered or connected-line for originated calls.)
	 */
	struct q931_party_id remote_id;

	/*!
	 * \brief Staging place for the Q.931 redirection number ie.
	 * \note
	 * The number could be the remote_id.number or redirecting.to.number
	 * depending upon the notification indicator.
	 */
	struct q931_party_number redirection_number;

	/*!
	 * \brief Called party address.
	 * \note The called.number.str is the accumulated overlap dial digits
	 * and enbloc digits.
	 * \note The called.number.presentation value is not used.
	 */
	struct q931_party_address called;
	int nonisdn;
	int complete;			/* no more digits coming */
	int newcall;			/* if the received message has a new call reference value */

	int retranstimer;		/* Timer for retransmitting DISC */
	int t308_timedout;		/* Whether t308 timed out once */

	struct q931_party_redirecting redirecting;

	/*! \brief Incoming call transfer state. */
	enum INCOMING_CT_STATE incoming_ct_state;
	/*! Call hold supplementary state. */
	enum Q931_HOLD_STATE hold_state;
	/*! Call hold event timer */
	int hold_timer;

	int deflection_in_progress;	/*!< CallDeflection for NT PTMP in progress. */

	int useruserprotocoldisc;
	char useruserinfo[256];
	char callingsubaddr[PRI_MAX_SUBADDRESS_LEN];	/* Calling party subaddress */
	
	long aoc_units;				/* Advice of Charge Units */

	struct apdu_event *apdus;	/* APDU queue for call */

	int transferable;			/* RLT call is transferable */
	unsigned int rlt_call_id;	/* RLT call id */

	/* Bridged call info */
	q931_call *bridged_call;        /* Pointer to other leg of bridged call (Used by Q.SIG when eliminating tromboned calls) */

	int changestatus;		/* SERVICE message changestatus */
	int reversecharge;		/* Reverse charging indication:
							   -1 - No reverse charging
							    1 - Reverse charging
							0,2-7 - Reserved for future use */
	int t303_timer;
	int t303_expirycnt;

	int hangupinitiated;
	/*! \brief TRUE if we broadcast this call's SETUP message. */
	int outboundbroadcast;
	int performing_fake_clearing;
	/*!
	 * \brief Master call controlling this call.
	 * \note Always valid.  Master and normal calls point to self.
	 */
	struct q931_call *master_call;

	/* These valid in master call only */
	struct q931_call *subcalls[Q931_MAX_TEI];
	int pri_winner;
};

extern int pri_schedule_event(struct pri *pri, int ms, void (*function)(void *data), void *data);

extern pri_event *pri_schedule_run(struct pri *pri);

extern void pri_schedule_del(struct pri *pri, int ev);

extern pri_event *pri_mkerror(struct pri *pri, char *errstr);

void pri_message(struct pri *ctrl, char *fmt, ...) __attribute__((format(printf, 2, 3)));
void pri_error(struct pri *ctrl, char *fmt, ...) __attribute__((format(printf, 2, 3)));

void libpri_copy_string(char *dst, const char *src, size_t size);

struct pri *__pri_new_tei(int fd, int node, int switchtype, struct pri *master, pri_io_cb rd, pri_io_cb wr, void *userdata, int tei, int bri);

void __pri_free_tei(struct pri *p);

void q931_party_name_init(struct q931_party_name *name);
void q931_party_number_init(struct q931_party_number *number);
void q931_party_subaddress_init(struct q931_party_subaddress *subaddr);
#define q931_party_address_to_id(q931_id, q931_address)			\
	do {														\
		(q931_id)->number = (q931_address)->number;				\
		/*(q931_id)->subaddress = (q931_address)->subaddress;*/	\
	} while (0)
void q931_party_address_init(struct q931_party_address *address);
void q931_party_id_init(struct q931_party_id *id);
void q931_party_redirecting_init(struct q931_party_redirecting *redirecting);

int q931_party_name_cmp(const struct q931_party_name *left, const struct q931_party_name *right);
int q931_party_number_cmp(const struct q931_party_number *left, const struct q931_party_number *right);
int q931_party_subaddress_cmp(const struct q931_party_subaddress *left, const struct q931_party_subaddress *right);
int q931_party_id_cmp(const struct q931_party_id *left, const struct q931_party_id *right);

void q931_party_name_copy_to_pri(struct pri_party_name *pri_name, const struct q931_party_name *q931_name);
void q931_party_number_copy_to_pri(struct pri_party_number *pri_number, const struct q931_party_number *q931_number);
void q931_party_subaddress_copy_to_pri(struct pri_party_subaddress *pri_subaddress, const struct q931_party_subaddress *q931_subaddress);
void q931_party_id_copy_to_pri(struct pri_party_id *pri_id, const struct q931_party_id *q931_id);
void q931_party_redirecting_copy_to_pri(struct pri_party_redirecting *pri_redirecting, const struct q931_party_redirecting *q931_redirecting);

void q931_party_id_fixup(const struct pri *ctrl, struct q931_party_id *id);
int q931_party_id_presentation(const struct q931_party_id *id);

const char *q931_call_state_str(enum Q931_CALL_STATE callstate);

int q931_is_ptmp(const struct pri *ctrl);
int q931_master_pass_event(struct pri *ctrl, struct q931_call *subcall, int msg_type);
struct pri_subcommand *q931_alloc_subcommand(struct pri *ctrl);

int q931_notify_redirection(struct pri *ctrl, q931_call *call, int notify, const struct q931_party_number *number);

static inline struct pri * PRI_MASTER(struct pri *mypri)
{
	struct pri *pri = mypri;
	
	if (!pri)
		return NULL;

	while (pri->master)
		pri = pri->master;

	return pri;
}

static inline int BRI_NT_PTMP(struct pri *mypri)
{
	struct pri *pri;

	pri = PRI_MASTER(mypri);

	return pri->bri && (((pri)->localtype == PRI_NETWORK) && ((pri)->tei == Q921_TEI_GROUP));
}

static inline int BRI_TE_PTMP(struct pri *mypri)
{
	struct pri *pri;

	pri = PRI_MASTER(mypri);

	return pri->bri && (((pri)->localtype == PRI_CPE) && ((pri)->tei == Q921_TEI_GROUP));
}

#endif
