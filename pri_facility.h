/*
   This file contains all data structures and definitions associated
   with facility message usage and the ROSE components included
   within those messages.

   by Matthew Fredrickson <creslin@digium.com>
   Copyright (C) Digium, Inc. 2004-2005
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

#ifndef _PRI_FACILITY_H
#define _PRI_FACILITY_H
#include "pri_q931.h"

/* Forward declare some structs */
struct fac_extension_header;
struct rose_msg_invoke;
struct rose_msg_result;
struct rose_msg_error;
struct rose_msg_reject;

/* Protocol Profile field */
#define Q932_PROTOCOL_MASK			0x1F
#define Q932_PROTOCOL_ROSE			0x11	/* X.219 & X.229 */
#define Q932_PROTOCOL_CMIP			0x12	/* Q.941 */
#define Q932_PROTOCOL_ACSE			0x13	/* X.217 & X.227 */
#define Q932_PROTOCOL_GAT			0x16
#define Q932_PROTOCOL_EXTENSIONS	0x1F

/* Q.952 Divert cause */
#define Q952_DIVERT_REASON_UNKNOWN		0x00
#define Q952_DIVERT_REASON_CFU			0x01
#define Q952_DIVERT_REASON_CFB			0x02
#define Q952_DIVERT_REASON_CFNR			0x03
#define Q952_DIVERT_REASON_CD			0x04
#define Q952_DIVERT_REASON_IMMEDIATE	0x05
/* Q.SIG Divert cause. Listed in ECMA-174 */
#define QSIG_DIVERT_REASON_UNKNOWN		0x00	/* Call forward unknown reason */
#define QSIG_DIVERT_REASON_CFU			0x01	/* Call Forward Unconditional (other reason) */
#define QSIG_DIVERT_REASON_CFB			0x02	/* Call Forward Busy */
#define QSIG_DIVERT_REASON_CFNR			0x03	/* Call Forward No Reply */

/* Q.932 Type of number */
#define Q932_TON_UNKNOWN				0x00
#define Q932_TON_INTERNATIONAL			0x01
#define Q932_TON_NATIONAL				0x02
#define Q932_TON_NET_SPECIFIC			0x03
#define Q932_TON_SUBSCRIBER				0x04
#define Q932_TON_ABBREVIATED			0x06

/* Q.SIG Subscription Option. Listed in ECMA-174 */
#define QSIG_NO_NOTIFICATION						0x00
#define QSIG_NOTIFICATION_WITHOUT_DIVERTED_TO_NR	0x01
#define QSIG_NOTIFICATION_WITH_DIVERTED_TO_NR		0x02

/*! Reasons an APDU callback is called. */
enum APDU_CALLBACK_REASON {
	/*!
	 * \brief Send setup error.  Abort and cleanup.
	 * \note The message may or may not actually get sent.
	 * \note The callback cannot generate an event subcmd.
	 * \note The callback should not send messages.  Out of order messages will result.
	 */
	APDU_CALLBACK_REASON_ERROR,
	/*!
	 * \brief Abort and cleanup.
	 * \note The APDU queue is being destroyed.
	 * \note The callback cannot generate an event subcmd.
	 * \note The callback cannot send messages as the call is likely being destroyed.
	 */
	APDU_CALLBACK_REASON_CLEANUP,
	/*!
	 * \brief Timeout waiting for responses to the message.
	 * \note The callback can generate an event subcmd.
	 * \note The callback can send messages.
	 */
	APDU_CALLBACK_REASON_TIMEOUT,
	/*!
	 * \brief Received a facility response message.
	 * \note The callback can generate an event subcmd.
	 * \note The callback can send messages.
	 */
	APDU_CALLBACK_REASON_MSG_RESULT,
	/*!
	 * \brief Received a facility error message.
	 * \note The callback can generate an event subcmd.
	 * \note The callback can send messages.
	 */
	APDU_CALLBACK_REASON_MSG_ERROR,
	/*!
	 * \brief Received a facility reject message.
	 * \note The callback can generate an event subcmd.
	 * \note The callback can send messages.
	 */
	APDU_CALLBACK_REASON_MSG_REJECT,
};

union apdu_msg_data {
	const struct rose_msg_result *result;
	const struct rose_msg_error *error;
	const struct rose_msg_reject *reject;
};

union apdu_callback_param {
	void *ptr;
	long value;
	char pad[8];
};

struct apdu_callback_data {
	/*! APDU invoke id to match with any response messages. (Result/Error/Reject) */
	int invoke_id;
	/*!
	 * \brief Time to wait for responses to APDU in ms.
	 * \note Set to 0 if send the message only.
	 * \note Set to less than 0 for PRI_TIMER_T_RESPONSE time.
	 */
	int timeout_time;
	/*!
	 * \brief APDU callback function.
	 *
	 * \param reason Reason callback is called.
	 * \param ctrl D channel controller.
	 * \param call Q.931 call leg.
	 * \param apdu APDU queued entry.  Do not change!
	 * \param msg APDU response message data.  (NULL if was not the reason called.)
	 *
	 * \note
	 * A callback must be supplied if the sender cares about any APDU_CALLBACK_REASON.
	 *
	 * \return TRUE if no more responses are expected.
	 */
	int (*callback)(enum APDU_CALLBACK_REASON reason, struct pri *ctrl, struct q931_call *call, struct apdu_event *apdu, const union apdu_msg_data *msg);
	/*! \brief Sender data for the callback function to identify the particular APDU. */
	union apdu_callback_param user;
};

struct apdu_event {
	/*! Linked list pointer */
	struct apdu_event *next;
	/*! TRUE if this APDU has been sent. */
	int sent;
	/*! What message to send the ADPU in */
	int message;
	/*! Sender supplied information to handle APDU response messages. */
	struct apdu_callback_data response;
	/*! Q.931 call leg.  (Needed for the APDU timeout.) */
	struct q931_call *call;
	/*! Response timeout timer. */
	int timer;
	/*! Length of ADPU */
	int apdu_len;
	/*! ADPU to send */
	unsigned char apdu[255];
};

/* Queues an MWI apdu on a the given call */
int mwi_message_send(struct pri *pri, q931_call *call, struct pri_sr *req, int activate);

/* starts a 2BCT */
int eect_initiate_transfer(struct pri *pri, q931_call *c1, q931_call *c2);

int rlt_initiate_transfer(struct pri *pri, q931_call *c1, q931_call *c2);

int qsig_cf_callrerouting(struct pri *pri, q931_call *c, const char* dest, const char* original, const char* reason);
int send_reroute_request(struct pri *ctrl, q931_call *call, const struct q931_party_id *caller, const struct q931_party_redirecting *deflection, int subscription_option);

/* starts a QSIG Path Replacement */
int anfpr_initiate_transfer(struct pri *pri, q931_call *c1, q931_call *c2);

int send_call_transfer_complete(struct pri *pri, q931_call *call, int call_status);

int rose_diverting_leg_information1_encode(struct pri *pri, q931_call *call);
int rose_diverting_leg_information3_encode(struct pri *pri, q931_call *call, int messagetype);

int rose_connected_name_encode(struct pri *pri, q931_call *call, int messagetype);
int rose_called_name_encode(struct pri *pri, q931_call *call, int messagetype);

int pri_call_apdu_queue(q931_call *call, int messagetype, const unsigned char *apdu, int apdu_len, struct apdu_callback_data *response);
void pri_call_apdu_queue_cleanup(q931_call *call);
void pri_call_apdu_delete(struct q931_call *call, struct apdu_event *doomed);

/* Adds the "standard" APDUs to a call */
int pri_call_add_standard_apdus(struct pri *pri, q931_call *call);

void asn1_dump(struct pri *ctrl, const unsigned char *start_asn1, const unsigned char *end);

void rose_handle_invoke(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_invoke *invoke);
void rose_handle_result(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_result *result);
void rose_handle_error(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_error *error);
void rose_handle_reject(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_reject *reject);

#endif /* _PRI_FACILITY_H */
