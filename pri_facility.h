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

int pri_call_apdu_queue(q931_call *call, int messagetype, const unsigned char *apdu, int apdu_len);

/* Used by q931.c to cleanup the apdu queue upon destruction of a call */
int pri_call_apdu_queue_cleanup(q931_call *call);

/* Adds the "standard" APDUs to a call */
int pri_call_add_standard_apdus(struct pri *pri, q931_call *call);

void asn1_dump(struct pri *ctrl, const unsigned char *start_asn1, const unsigned char *end);

/* Forward declare some ROSE structures for the following prototypes */
struct fac_extension_header;
struct rose_msg_invoke;
struct rose_msg_result;
struct rose_msg_error;
struct rose_msg_reject;

void rose_handle_invoke(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_invoke *invoke);
void rose_handle_result(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_result *result);
void rose_handle_error(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_error *error);
void rose_handle_reject(struct pri *ctrl, q931_call *call, int msgtype, q931_ie *ie, const struct fac_extension_header *header, const struct rose_msg_reject *reject);

#endif /* _PRI_FACILITY_H */
