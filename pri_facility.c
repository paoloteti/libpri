/* 
   This file and it's contents are licensed under the terms and conditions
   of the GNU Public License.  See http://www.gnu.org for details.
   
   Routines for dealing with facility messages and their respective
   components (ROSE)

   by Matthew Fredrickson <creslin@digium.com>
   Copyright (C) 2004-2005 Digium, Inc
*/

#include "compat.h"
#include "libpri.h"
#include "pri_internal.h"
#include "pri_q921.h"
#include "pri_q931.h"
#include "pri_facility.h"

#include <stdlib.h>
#include <string.h>

#undef DEBUG

static unsigned char get_invokeid(struct pri *pri)
{
	return ++pri->last_invoke;
}

struct addressingdataelements_presentednumberunscreened {
	char partyaddress[21];
	char partysubaddress[21];
	int  npi;
	int  ton;
	int  pres;
};

int redirectingreason_from_q931(struct pri *pri, int redirectingreason)
{
	switch(pri->switchtype) {
	case PRI_SWITCH_QSIG:
		switch(redirectingreason) {
		case PRI_REDIR_UNKNOWN:
			return QSIG_DIVERT_REASON_UNKNOWN;
		case PRI_REDIR_FORWARD_ON_BUSY:
			return QSIG_DIVERT_REASON_CFB;
		case PRI_REDIR_FORWARD_ON_NO_REPLY:
			return QSIG_DIVERT_REASON_CFNR;
		case PRI_REDIR_UNCONDITIONAL:
			return QSIG_DIVERT_REASON_CFU;
		case PRI_REDIR_DEFLECTION:
		case PRI_REDIR_DTE_OUT_OF_ORDER:
		case PRI_REDIR_FORWARDED_BY_DTE:
			pri_message("!! Don't know how to convert Q.931 redirection reason %d to Q.SIG\n", redirectingreason);
			/* Fall through */
		default:
			return QSIG_DIVERT_REASON_UNKNOWN;
		}
	default:
		switch(redirectingreason) {
		case PRI_REDIR_UNKNOWN:
			return Q952_DIVERT_REASON_UNKNOWN;
		case PRI_REDIR_FORWARD_ON_BUSY:
			return Q952_DIVERT_REASON_CFB;
		case PRI_REDIR_FORWARD_ON_NO_REPLY:
			return Q952_DIVERT_REASON_CFNR;
		case PRI_REDIR_DEFLECTION:
			return Q952_DIVERT_REASON_CD;
		case PRI_REDIR_UNCONDITIONAL:
			return Q952_DIVERT_REASON_CFU;
		case PRI_REDIR_DTE_OUT_OF_ORDER:
		case PRI_REDIR_FORWARDED_BY_DTE:
			pri_message("!! Don't know how to convert Q.931 redirection reason %d to Q.952\n", redirectingreason);
			/* Fall through */
		default:
			return Q952_DIVERT_REASON_UNKNOWN;
		}
	}
}

static int redirectingreason_for_q931(struct pri *pri, int redirectingreason)
{
	switch(pri->switchtype) {
	case PRI_SWITCH_QSIG:
		switch(redirectingreason) {
		case QSIG_DIVERT_REASON_UNKNOWN:
			return PRI_REDIR_UNKNOWN;
		case QSIG_DIVERT_REASON_CFU:
			return PRI_REDIR_UNCONDITIONAL;
		case QSIG_DIVERT_REASON_CFB:
			return PRI_REDIR_FORWARD_ON_BUSY;
		case QSIG_DIVERT_REASON_CFNR:
			return PRI_REDIR_FORWARD_ON_NO_REPLY;
		default:
			pri_message("!! Unknown Q.SIG diversion reason %d\n", redirectingreason);
			return PRI_REDIR_UNKNOWN;
		}
	default:
		switch(redirectingreason) {
		case Q952_DIVERT_REASON_UNKNOWN:
			return PRI_REDIR_UNKNOWN;
		case Q952_DIVERT_REASON_CFU:
			return PRI_REDIR_UNCONDITIONAL;
		case Q952_DIVERT_REASON_CFB:
			return PRI_REDIR_FORWARD_ON_BUSY;
		case Q952_DIVERT_REASON_CFNR:
			return PRI_REDIR_FORWARD_ON_NO_REPLY;
		case Q952_DIVERT_REASON_CD:
			return PRI_REDIR_DEFLECTION;
		case Q952_DIVERT_REASON_IMMEDIATE:
			pri_message("!! Dont' know how to convert Q.952 diversion reason IMMEDIATE to PRI analog\n");
			return PRI_REDIR_UNKNOWN;	/* ??? */
		default:
			pri_message("!! Unknown Q.952 diversion reason %d\n", redirectingreason);
			return PRI_REDIR_UNKNOWN;
		}
	}
}

int typeofnumber_from_q931(struct pri *pri, int ton)
{
	switch(ton) {
	case PRI_TON_INTERNATIONAL:
		return Q932_TON_INTERNATIONAL;
	case PRI_TON_NATIONAL:
		return Q932_TON_NATIONAL;
	case PRI_TON_NET_SPECIFIC:
		return Q932_TON_NET_SPECIFIC;
	case PRI_TON_SUBSCRIBER:
		return Q932_TON_SUBSCRIBER;
	case PRI_TON_ABBREVIATED:
		return Q932_TON_ABBREVIATED;
	case PRI_TON_RESERVED:
	default:
		pri_message("!! Unsupported Q.931 TypeOfNumber value (%d)\n", ton);
		/* fall through */
	case PRI_TON_UNKNOWN:
		return Q932_TON_UNKNOWN;
	}
}

static int typeofnumber_for_q931(struct pri *pri, int ton)
{
	switch (ton) {
	case Q932_TON_UNKNOWN:
		return PRI_TON_UNKNOWN;
	case Q932_TON_INTERNATIONAL:
		return PRI_TON_INTERNATIONAL;
	case Q932_TON_NATIONAL:
		return PRI_TON_NATIONAL;
	case Q932_TON_NET_SPECIFIC:
		return PRI_TON_NET_SPECIFIC;
	case Q932_TON_SUBSCRIBER:
		return PRI_TON_SUBSCRIBER;
	case Q932_TON_ABBREVIATED:
		return PRI_TON_ABBREVIATED;
	default:
		pri_message("!! Invalid Q.932 TypeOfNumber %d\n", ton);
		return PRI_TON_UNKNOWN;
	}
}

int asn1_string_encode(unsigned char asn1_type, void *data, int len, int max_len, void *src, int src_len)
{
	struct rose_component *comp = NULL;
	
	if (len < 2 + src_len)
		return -1;

	if (max_len && (src_len > max_len))
		src_len = max_len;

	comp = (struct rose_component *)data;
	comp->type = asn1_type;
	comp->len = src_len;
	memcpy(comp->data, src, src_len);
	
	return 2 + src_len;
}

static int rose_number_digits_decode(struct pri *pri, q931_call *call, unsigned char *data, int len, struct addressingdataelements_presentednumberunscreened *value)
{
	int i = 0;
	struct rose_component *comp = NULL;
	unsigned char *vdata = data;

	do {
		GET_COMPONENT(comp, i, vdata, len);
		CHECK_COMPONENT(comp, ASN1_NUMERICSTRING, "Don't know what to do with PublicPartyNumber ROSE component type 0x%x\n");
		if(comp->len > 20) {
			pri_message("!! Oversized NumberDigits component (%d)\n", comp->len);
			return -1;
		}
		memcpy(value->partyaddress, comp->data, comp->len);
		value->partyaddress[comp->len] = '\0';

		return 0;
	}
	while(0);
	
	return -1;
}

static int rose_public_party_number_decode(struct pri *pri, q931_call *call, unsigned char *data, int len, struct addressingdataelements_presentednumberunscreened *value)
{
	int i = 0;
	struct rose_component *comp = NULL;
	unsigned char *vdata = data;
	int ton;

	if (len < 2)
		return -1;

	do {
		GET_COMPONENT(comp, i, vdata, len);
		CHECK_COMPONENT(comp, ASN1_ENUMERATED, "Don't know what to do with PublicPartyNumber ROSE component type 0x%x\n");
		ASN1_GET_INTEGER(comp, ton);
		NEXT_COMPONENT(comp, i);
		ton = typeofnumber_for_q931(pri, ton);

		if(rose_number_digits_decode(pri, call, &vdata[i], len-i, value))
			return -1;
		value->ton = ton;

		return 0;

	} while(0);
	return -1;
}

static int rose_address_decode(struct pri *pri, q931_call *call, unsigned char *data, int len, struct addressingdataelements_presentednumberunscreened *value)
{
	int i = 0;
	struct rose_component *comp = NULL;
	unsigned char *vdata = data;

	do {
		GET_COMPONENT(comp, i, vdata, len);

		switch(comp->type) {
		case 0xA0:	/* unknownPartyNumber */
			if(rose_number_digits_decode(pri, call, comp->data, comp->len, value))
				return -1;
			value->npi = PRI_NPI_UNKNOWN;
			value->ton = PRI_TON_UNKNOWN;
			break;
		case 0xA1:	/* publicPartyNumber */
			if(rose_public_party_number_decode(pri, call, comp->data, comp->len, value) != 0)
				return -1;
			value->npi = PRI_NPI_E163_E164;
			break;
		case 0xA2:	/* nsapEncodedNumber */
			pri_message("!! NsapEncodedNumber isn't handled\n");
			return -1;
		case 0xA3:	/* dataPartyNumber */
			if(rose_number_digits_decode(pri, call, comp->data, comp->len, value))
				return -1;
			value->npi = PRI_NPI_X121 /* ??? */;
			value->ton = PRI_TON_UNKNOWN /* ??? */;
			pri_message("!! dataPartyNumber isn't handled\n");
			return -1;
		case 0xA4:	/* telexPartyNumber */
			if (rose_number_digits_decode(pri, call, comp->data, comp->len, value))
				return -1;
			value->npi = PRI_NPI_F69 /* ??? */;
			value->ton = PRI_TON_UNKNOWN /* ??? */;
			pri_message("!! telexPartyNumber isn't handled\n");
			return -1;
		case 0xA5:	/* priavePartyNumber */
			pri_message("!! privatePartyNumber isn't handled\n");
			value->npi = PRI_NPI_PRIVATE;
			return -1;
		case 0xA8:	/* nationalStandardPartyNumber */
			if (rose_number_digits_decode(pri, call, comp->data, comp->len, value))
				return -1;
			value->npi = PRI_NPI_NATIONAL;
			value->ton = PRI_TON_NATIONAL;
			break;
		default:
			pri_message("!! Unknown Party number component received 0x%X\n", comp->type);
			return -1;
		}
		NEXT_COMPONENT(comp, i);
		if(i < len)
			pri_message("!! not all information is handled from Address component\n");
		return 0;
	}
	while (0);

	return -1;
}

static int rose_presented_number_unscreened_decode(struct pri *pri, q931_call *call, unsigned char *data, int len, struct addressingdataelements_presentednumberunscreened *value)
{
	int i = 0;
	struct rose_component *comp = NULL;
	unsigned char *vdata = data;

	/* Fill in default values */
	value->ton = PRI_TON_UNKNOWN;
	value->npi = PRI_NPI_E163_E164;
	value->pres = -1;	/* Data is not available */

	do {
		GET_COMPONENT(comp, i, vdata, len);

		switch(comp->type) {
		case 0xA0:		/* [0] presentationAllowedNumber */
			value->pres = PRES_ALLOWED_USER_NUMBER_NOT_SCREENED;
			return rose_address_decode(pri, call, comp->data, comp->len, value);
		case 0x81:		/* [1] IMPLICIT presentationRestricted */
			if (comp->len != 0) { /* must be NULL */
				pri_error("!! Invalid PresentationRestricted component received (len != 0)\n");
				return -1;
			}
			value->pres = PRES_PROHIB_USER_NUMBER_NOT_SCREENED;
			return 0;
		case 0x82:		/* [2] IMPLICIT numberNotAvailableDueToInterworking */
			if (comp->len != 0) { /* must be NULL */
				pri_error("!! Invalid NumberNotAvailableDueToInterworking component received (len != 0)\n");
				return -1;
			}
			value->pres = PRES_NUMBER_NOT_AVAILABLE;
			return 0;
		case 0xA3:		/* [3] presentationRestrictedNumber */
			value->pres = PRES_PROHIB_USER_NUMBER_NOT_SCREENED;
			return rose_address_decode(pri, call, comp->data, comp->len, value);
		default:
			pri_message("Invalid PresentedNumberUnscreened component 0x%X\n", comp->type);
		}
		return -1;
	}
	while (0);

	return -1;
}

static int rose_diverting_leg_information2_decode(struct pri *pri, q931_call *call, unsigned char *data, int len)
{
	int i = 0;
	int diversion_counter;
	int diversion_reason;
	struct addressingdataelements_presentednumberunscreened divertingnr;
 	struct addressingdataelements_presentednumberunscreened originalcallednr;
	struct rose_component *comp = NULL;
	unsigned char *vdata = data;

	do {
		/* diversionCounter stuff */
		GET_COMPONENT(comp, i, vdata, len);
		CHECK_COMPONENT(comp, ASN1_INTEGER, "Don't know what to do it diversionCounter is of type 0x%x\n");
		ASN1_GET_INTEGER(comp, diversion_counter);
		NEXT_COMPONENT(comp, i);

		/* diversionReason stuff */
		GET_COMPONENT(comp, i, vdata, len);
		CHECK_COMPONENT(comp, ASN1_ENUMERATED, "Invalid diversionReason type 0x%X of ROSE divertingLegInformation2 component received\n");
		ASN1_GET_INTEGER(comp, diversion_reason);
		NEXT_COMPONENT(comp, i);

		diversion_reason = redirectingreason_for_q931(pri, diversion_reason);
	
#ifdef DEBUG
		if(pri->debug)
			pri_message("    Redirection reason: %d, total diversions: %d\n", diversion_reason, diversion_counter);
#endif

		for(; i < len; NEXT_COMPONENT(comp, i)) {
			GET_COMPONENT(comp, i, vdata, len);
			switch(comp->type) {
			case 0xA1:		/* divertingnr: presentednumberunscreened */
				if(rose_presented_number_unscreened_decode(pri, call, comp->data, comp->len, &divertingnr) != 0)
					return -1;
#ifdef DEBUG
				if (pri->debug) {
					pri_message("    Received divertingNr '%s'\n", divertingnr.partyaddress);
					pri_message("      ton = %d, pres = %d, npi = %d\n", divertingnr.ton, divertingnr.pres, divertingnr.npi);
				}
#endif
				break;
			case 0xA2:		/* originalCalledNr: PresentedNumberUnscreened */
				if(rose_presented_number_unscreened_decode(pri, call, comp->data, comp->len, &originalcallednr) != 0)
					return -1;
#ifdef DEBUG
				if (pri->debug) {
					pri_message("    Received originalcallednr '%s'\n", originalcallednr.partyaddress);
					pri_message("      ton = %d, pres = %d, npi = %d\n", originalcallednr.ton, originalcallednr.pres, originalcallednr.npi);
				}
#endif
				break;
			default:
				pri_message("!! Invalid DivertingLegInformation2 component received 0x%X\n", comp->type);
				return -1;
			}
		}
		if (i < len)
			return -1;	/* Aborted before */

		if (divertingnr.pres >= 0) {
			call->redirectingplan = divertingnr.npi;
			call->redirectingpres = divertingnr.pres;
			call->redirectingreason = diversion_reason;
			strncpy(call->redirectingnum, divertingnr.partyaddress, sizeof(call->redirectingnum)-1);
			call->redirectingnum[sizeof(call->redirectingnum)-1] = '\0';
		}
		else if (originalcallednr.pres >= 0) {
			call->redirectingplan = originalcallednr.npi;
			call->redirectingpres = originalcallednr.pres;
			call->redirectingreason = diversion_reason;
			strncpy(call->redirectingnum, originalcallednr.partyaddress, sizeof(call->redirectingnum)-1);
			call->redirectingnum[sizeof(call->redirectingnum)-1] = '\0';
		}
		return 0;
	}
	while (0);

	return -1;
}

static int rose_diverting_leg_information2_encode(struct pri *pri, q931_call *call)
{
	int i = 0, j, compsp = 0;
	struct rose_component *comp, *compstk[10];
	unsigned char buffer[256];
	int len = 253;
	
	if (!strlen(call->callername)) {
		return -1;
	}

	buffer[i] = 0x80 | Q932_PROTOCOL_EXTENSIONS;
	i++;
	/* Interpretation component */
	ASN1_ADD_BYTECOMP(comp, COMP_TYPE_INTERPRETATION, buffer, i, 0x00 /* Discard unrecognized invokes */);
	
	ASN1_ADD_SIMPLE(comp, COMP_TYPE_INVOKE, buffer, i);
	
	ASN1_PUSH(compstk, compsp, comp);
	/* Invoke component contents */
	/*	Invoke ID */
	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, get_invokeid(pri));
	/*	Operation Tag */
	
	/* ROSE operationId component */
	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, ROSE_DIVERTING_LEG_INFORMATION2);

	/* ROSE ARGUMENT component */
	ASN1_ADD_SIMPLE(comp, 0x30, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	/* ROSE DivertingLegInformation2.diversionCounter component */
	/* Always is 1 because other isn't available in the current design */
	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, 1);
	
	/* ROSE DivertingLegInformation2.diversionReason component */
	ASN1_ADD_BYTECOMP(comp, ASN1_ENUMERATED, buffer, i, redirectingreason_from_q931(pri, call->redirectingreason));
		
	/* ROSE DivertingLegInformation2.divertingNr component */
	ASN1_ADD_SIMPLE(comp, 0xA1, buffer, i);
	
	ASN1_PUSH(compstk, compsp, comp);
		/* Redirecting information always not screened */
	
	switch(call->redirectingpres) {
		case PRES_ALLOWED_USER_NUMBER_NOT_SCREENED:
		case PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN:
			if (call->redirectingnum && strlen(call->redirectingnum)) {
				ASN1_ADD_SIMPLE(comp, 0xA0, buffer, i);
				ASN1_PUSH(compstk, compsp, comp);
					/* NPI of redirected number is not supported in the current design */
				ASN1_ADD_SIMPLE(comp, 0xA1, buffer, i);
				ASN1_PUSH(compstk, compsp, comp);
					ASN1_ADD_BYTECOMP(comp, ASN1_ENUMERATED, buffer, i, typeofnumber_from_q931(pri, call->redirectingplan >> 4));
					j = asn1_string_encode(ASN1_NUMERICSTRING, &buffer[i], len - i, 20, call->redirectingnum, strlen(call->redirectingnum));
				if (j < 0)
					return -1;
					
				i += j;
				ASN1_FIXUP(compstk, compsp, buffer, i);
				ASN1_FIXUP(compstk, compsp, buffer, i);
				break;
			}
			/* fall through */
		case PRES_PROHIB_USER_NUMBER_PASSED_SCREEN:
		case PRES_PROHIB_USER_NUMBER_NOT_SCREENED:
			ASN1_ADD_SIMPLE(comp, 0x81, buffer, i);
			break;
		/* Don't know how to handle this */
		case PRES_ALLOWED_NETWORK_NUMBER:
		case PRES_PROHIB_NETWORK_NUMBER:
		case PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN:
		case PRES_PROHIB_USER_NUMBER_FAILED_SCREEN:
			ASN1_ADD_SIMPLE(comp, 0x81, buffer, i);
			break;
		default:
			pri_message("!! Undefined presentation value for redirecting number: %d\n", call->redirectingpres);
		case PRES_NUMBER_NOT_AVAILABLE:
			ASN1_ADD_SIMPLE(comp, 0x82, buffer, i);
			break;
	}
	ASN1_FIXUP(compstk, compsp, buffer, i);

	/* ROSE DivertingLegInformation2.originalCalledNr component */
	/* This information isn't supported by current design - duplicate divertingNr */
	ASN1_ADD_SIMPLE(comp, 0xA2, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
		/* Redirecting information always not screened */
	switch(call->redirectingpres) {
		case PRES_ALLOWED_USER_NUMBER_NOT_SCREENED:
		case PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN:
			if (call->redirectingnum && strlen(call->redirectingnum)) {
				ASN1_ADD_SIMPLE(comp, 0xA0, buffer, i);
				ASN1_PUSH(compstk, compsp, comp);
				ASN1_ADD_SIMPLE(comp, 0xA1, buffer, i);
				ASN1_PUSH(compstk, compsp, comp);
				ASN1_ADD_BYTECOMP(comp, ASN1_ENUMERATED, buffer, i, typeofnumber_from_q931(pri, call->redirectingplan >> 4));
	
				j = asn1_string_encode(ASN1_NUMERICSTRING, &buffer[i], len - i, 20, call->redirectingnum, strlen(call->redirectingnum));
				if (j < 0)
					return -1;
				
				i += j;
				ASN1_FIXUP(compstk, compsp, buffer, i);
				ASN1_FIXUP(compstk, compsp, buffer, i);
				break;
			}
				/* fall through */
		case PRES_PROHIB_USER_NUMBER_PASSED_SCREEN:
		case PRES_PROHIB_USER_NUMBER_NOT_SCREENED:
			ASN1_ADD_SIMPLE(comp, 0x81, buffer, i);
			break;
		/* Don't know how to handle this */
		case PRES_ALLOWED_NETWORK_NUMBER:
		case PRES_PROHIB_NETWORK_NUMBER:
		case PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN:
		case PRES_PROHIB_USER_NUMBER_FAILED_SCREEN:
			ASN1_ADD_SIMPLE(comp, 0x81, buffer, i);
			break;
		default:
			pri_message("!! Undefined presentation value for redirecting number: %d\n", call->redirectingpres);
		case PRES_NUMBER_NOT_AVAILABLE:
			ASN1_ADD_SIMPLE(comp, 0x82, buffer, i);
			break;
	}
	ASN1_FIXUP(compstk, compsp, buffer, i);
		
	/* Fix length of stacked components */
	while(compsp > 0) {
		ASN1_FIXUP(compstk, compsp, buffer, i);
	}
	
	if (pri_call_apdu_queue(call, Q931_SETUP, buffer, i, NULL, NULL))
		return -1;
		
	return 0;
}

/* Sending callername information functions */
static int add_callername_facility_ies(struct pri *pri, q931_call *c)
{
	int res = 0;
	int i = 0;
	unsigned char buffer[256];
	unsigned char namelen = 0;
	struct rose_component *comp = NULL, *compstk[10];
	int compsp = 0;
	static unsigned char op_tag[] = { 
		0x2a, /* informationFollowing 42 */
		0x86,
		0x48,
		0xce,
		0x15,
		0x00,
		0x04
	};
		
	if (!strlen(c->callername)) {
		return -1;
	}

	buffer[i++] = 0x80 | Q932_PROTOCOL_EXTENSIONS;
	/* Interpretation component */

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_NFE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	ASN1_ADD_BYTECOMP(comp, 0x80, buffer, i, 0);
	ASN1_ADD_BYTECOMP(comp, 0x82, buffer, i, 0);
	ASN1_FIXUP(compstk, compsp, buffer, i);

	ASN1_ADD_BYTECOMP(comp, COMP_TYPE_INTERPRETATION, buffer, i, 0);

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_INVOKE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	/* Invoke ID */
	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, get_invokeid(pri));

	/* Operation Tag */
	res = asn1_string_encode(ASN1_OBJECTIDENTIFIER, &buffer[i], sizeof(buffer)-i, sizeof(op_tag), op_tag, sizeof(op_tag));
	if (res < 0)
		return -1;
	i += res;

	ASN1_ADD_BYTECOMP(comp, ASN1_ENUMERATED, buffer, i, 0);
	ASN1_FIXUP(compstk, compsp, buffer, i);

	if (pri_call_apdu_queue(c, Q931_SETUP, buffer, i, NULL, NULL))
		return -1;


	/* Now the ADPu that contains the information that needs sent.
	 * We can reuse the buffer since the queue function doesn't
	 * need it. */

	i = 0;
	namelen = strlen(c->callername);
	if (namelen > 50) {
		namelen = 50; /* truncate the name */
	}

	buffer[i++] = 0x80 | Q932_PROTOCOL_EXTENSIONS;
	/* Interpretation component */

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_NFE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	ASN1_ADD_BYTECOMP(comp, 0x80, buffer, i, 0);
	ASN1_ADD_BYTECOMP(comp, 0x82, buffer, i, 0);
	ASN1_FIXUP(compstk, compsp, buffer, i);

	ASN1_ADD_BYTECOMP(comp, COMP_TYPE_INTERPRETATION, buffer, i, 0);

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_INVOKE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);

	/* Invoke ID */
	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, get_invokeid(pri));

	/* Operation ID: Calling name */
	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, SS_CNID_CALLINGNAME);

	res = asn1_string_encode(0x80, &buffer[i], sizeof(buffer)-i,  50, c->callername, namelen);
	if (res < 0)
	  return -1;
	i += res;
	ASN1_FIXUP(compstk, compsp, buffer, i);

	if (pri_call_apdu_queue(c, Q931_FACILITY, buffer, i, NULL, NULL))
		return -1;
	
	return 0;
}

/* End Callername */

/* MWI related encode and decode functions */
static void mwi_activate_encode_cb(void *data)
{
	return;
}

extern int mwi_message_send(struct pri* pri, q931_call *call, struct pri_sr *req, int activate)
{
	int i = 0;
	unsigned char buffer[255] = "";
	int destlen = strlen(req->called);
	struct rose_component *comp = NULL, *compstk[10];
	int compsp = 0;
	int res;

	if (destlen <= 0) {
		return -1;
	} else if (destlen > 20)
		destlen = 20;  /* Destination number cannot be greater then 20 digits */

	buffer[i++] = 0x80 | Q932_PROTOCOL_EXTENSIONS;
	/* Interpretation component */

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_NFE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	ASN1_ADD_BYTECOMP(comp, 0x80, buffer, i, 0);
	ASN1_ADD_BYTECOMP(comp, 0x82, buffer, i, 0);
	ASN1_FIXUP(compstk, compsp, buffer, i);

	ASN1_ADD_BYTECOMP(comp, COMP_TYPE_INTERPRETATION, buffer, i, 0);

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_INVOKE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);

	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, get_invokeid(pri));

	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, (activate) ? SS_MWI_ACTIVATE : SS_MWI_DEACTIVATE);
	ASN1_ADD_SIMPLE(comp, 0x30 /* Sequence */, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	/* PartyNumber */
	res = asn1_string_encode(0x80, &buffer[i], sizeof(buffer)-i, destlen, req->called, destlen);
	
	if (res < 0)
		return -1;
	i += res;

	/* Enumeration: basicService */
	ASN1_ADD_BYTECOMP(comp, ASN1_ENUMERATED, buffer, i, 1 /* contents: Voice */);
	ASN1_FIXUP(compstk, compsp, buffer, i);
	ASN1_FIXUP(compstk, compsp, buffer, i);

	return pri_call_apdu_queue(call, Q931_SETUP, buffer, i, mwi_activate_encode_cb, NULL);
}
/* End MWI */

/* EECT functions */
extern int eect_initiate_transfer(struct pri *pri, q931_call *c1, q931_call *c2)
{
	/* Did all the tests to see if we're on the same PRI and
	 * are on a compatible switchtype */
	/* TODO */
	int i = 0;
	int res = 0;
	unsigned char buffer[255] = "";
	unsigned short call_reference = c2->cr;
	struct rose_component *comp = NULL, *compstk[10];
	int compsp = 0;
	static unsigned char op_tag[] = {
		0x2A,
		0x86,
		0x48,
		0xCE,
		0x15,
		0x00,
		0x08,
	};

	buffer[i++] = 0x80 | Q932_PROTOCOL_EXTENSIONS;
	/* Interpretation component */

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_NFE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	ASN1_ADD_BYTECOMP(comp, 0x80, buffer, i, 0);
	ASN1_ADD_BYTECOMP(comp, 0x82, buffer, i, 0);
	ASN1_FIXUP(compstk, compsp, buffer, i);

	ASN1_ADD_BYTECOMP(comp, COMP_TYPE_INTERPRETATION, buffer, i, 0);

	ASN1_ADD_SIMPLE(comp, COMP_TYPE_INVOKE, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);

	ASN1_ADD_BYTECOMP(comp, ASN1_INTEGER, buffer, i, get_invokeid(pri));

	res = asn1_string_encode(ASN1_OBJECTIDENTIFIER, &buffer[i], sizeof(buffer)-i, sizeof(op_tag), op_tag, sizeof(op_tag));
	if (res < 0)
		return -1;
	i += res;

	ASN1_ADD_SIMPLE(comp, ASN1_SEQUENCE | 0x20, buffer, i);
	ASN1_PUSH(compstk, compsp, comp);
	ASN1_ADD_WORDCOMP(comp, 0x02, buffer, i, call_reference);
	ASN1_FIXUP(compstk, compsp, buffer, i);
	ASN1_FIXUP(compstk, compsp, buffer, i);

	res = pri_call_apdu_queue(c1, Q931_FACILITY, buffer, i, NULL, NULL);
	if (res) {
		pri_message("Could not queue ADPU in facility message\n");
		return -1;
	}

	/* Remember that if we queue a facility IE for a facility message we
	 * have to explicitly send the facility message ourselves */

	res = q931_facility(c1->pri, c1);
	if (res) {
		pri_message("Could not schedule facility message for call %d\n", c1->cr);
		return -1;
	}

	return 0;
}
/* End EECT */


extern int rose_invoke_decode(struct pri *pri, q931_call *call, unsigned char *data, int len)
{
	int i = 0;
	int operation_tag;
	unsigned char *vdata = data;
	struct rose_component *comp = NULL, *invokeid = NULL, *operationid = NULL;

	do {
		/* Invoke ID stuff */
		GET_COMPONENT(comp, i, vdata, len);
		CHECK_COMPONENT(comp, ASN1_INTEGER, "Don't know what to do if first ROSE component is of type 0x%x\n");
		invokeid = comp;
		NEXT_COMPONENT(comp, i);

		/* Operation Tag */
		GET_COMPONENT(comp, i, vdata, len);
		CHECK_COMPONENT(comp, ASN1_INTEGER, "Don't know what to do if second ROSE component is of type 0x%x\n");
		operationid = comp;
		ASN1_GET_INTEGER(comp, operation_tag);
		NEXT_COMPONENT(comp, i);

		/* No argument - return with error */
		if (i >= len) 
			return -1;

		/* Arguement Tag */
		GET_COMPONENT(comp, i, vdata, len);
		if (!comp->type)
			return -1;

#ifdef DEBUG
		pri_message("  [ Handling operation %d ]\n", operation_tag);
#endif
		switch (operation_tag) {
		case SS_CNID_CALLINGNAME:
#ifdef DEBUG
			if (pri->debug)
				pri_message("  Handle Name display operation\n");
#endif
			switch (comp->type) {
			case ROSE_NAME_PRESENTATION_ALLOWED_SIMPLE:
				memcpy(call->callername, comp->data, comp->len);
				call->callername[comp->len] = 0;
#ifdef DEBUG
				if (pri->debug)
				  pri_message("    Received caller name '%s'\n", call->callername);
#endif
				return 0;
			default:
				pri_message("Do not handle argument of type 0x%X\n", comp->type);
				return -1;
			}
			break;
		case ROSE_DIVERTING_LEG_INFORMATION2:
#ifdef DEBUG
			if (pri->debug)
				pri_message("  Handle DivertingLegInformation2\n");
#endif
			if (comp->type != 0x30) { /* Sequence */
				pri_message("Invalid DivertingLegInformation2Type argument\n");
				return -1;
			}
			return rose_diverting_leg_information2_decode(pri, call, comp->data, comp->len);
		default:
			pri_message("!! Unable to handle ROSE operation %d\n", operation_tag);
			return -1;
		}
	} while(0);
	
	return -1;
}

extern int pri_call_apdu_queue(q931_call *call, int messagetype, void *apdu, int apdu_len, void (*function)(void *data), void *data)
{
	struct apdu_event *cur = NULL;
	struct apdu_event *new_event = NULL;

	if (!call || !messagetype || !apdu || (apdu_len < 1) || (apdu_len > 255))
		return -1;

	new_event = malloc(sizeof(struct apdu_event));
	memset(new_event, 0, sizeof(struct apdu_event));

	if (new_event) {
		new_event->message = messagetype;
		new_event->callback = function;
		new_event->data = data;
		memcpy(new_event->apdu, apdu, apdu_len);
		new_event->apdu_len = apdu_len;
	} else {
		pri_error("malloc failed\n");
		return -1;
	}
	
	if (call->apdus) {
		cur = call->apdus;
		while (cur->next) {
			cur = cur->next;
		}
		cur->next = new_event;
	} else
		call->apdus = new_event;

	return 0;
}

extern int pri_call_apdu_queue_cleanup(q931_call *call)
{
	struct apdu_event *cur_event = NULL, *free_event = NULL;

	if (call && call->apdus) {
		cur_event = call->apdus;
		while (cur_event) {
			/* TODO: callbacks, some way of giving return res on status of apdu */
			free_event = cur_event;
			free(free_event);
			cur_event = cur_event->next;
		}
		call->apdus = NULL;
	}

	return 0;
}

extern int pri_call_add_standard_apdus(struct pri *pri, q931_call *call)
{
	if (pri->switchtype == PRI_SWITCH_QSIG) { /* For Q.SIG it does network and cpe operations */
		rose_diverting_leg_information2_encode(pri, call);
	}

#if 0
	if (pri->localtype == PRI_NETWORK) {
#endif
	if (1) {
		switch (pri->switchtype) {
			case PRI_SWITCH_NI2:
				add_callername_facility_ies(pri, call);
				break;
			default:
				break;
		}
	}
	return 0;
}

