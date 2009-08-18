/*
 * libpri: An implementation of Primary Rate ISDN
 *
 * Written by Matthew Fredrickson <creslin@digium.com>
 *
 * Copyright (C) 2004-2005, Digium, Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static short get_invokeid(struct pri *ctrl)
{
	return ++ctrl->last_invoke;
}

static int redirectingreason_from_q931(struct pri *ctrl, int redirectingreason)
{
	int value;

	switch (ctrl->switchtype) {
	case PRI_SWITCH_QSIG:
		switch (redirectingreason) {
		case PRI_REDIR_UNKNOWN:
			value = QSIG_DIVERT_REASON_UNKNOWN;
			break;
		case PRI_REDIR_FORWARD_ON_BUSY:
			value = QSIG_DIVERT_REASON_CFB;
			break;
		case PRI_REDIR_FORWARD_ON_NO_REPLY:
			value = QSIG_DIVERT_REASON_CFNR;
			break;
		case PRI_REDIR_UNCONDITIONAL:
			value = QSIG_DIVERT_REASON_CFU;
			break;
		case PRI_REDIR_DEFLECTION:
		case PRI_REDIR_DTE_OUT_OF_ORDER:
		case PRI_REDIR_FORWARDED_BY_DTE:
			pri_message(ctrl,
				"!! Don't know how to convert Q.931 redirection reason %d to Q.SIG\n",
				redirectingreason);
			/* Fall through */
		default:
			value = QSIG_DIVERT_REASON_UNKNOWN;
			break;
		}
		break;
	default:
		switch (redirectingreason) {
		case PRI_REDIR_UNKNOWN:
			value = Q952_DIVERT_REASON_UNKNOWN;
			break;
		case PRI_REDIR_FORWARD_ON_BUSY:
			value = Q952_DIVERT_REASON_CFB;
			break;
		case PRI_REDIR_FORWARD_ON_NO_REPLY:
			value = Q952_DIVERT_REASON_CFNR;
			break;
		case PRI_REDIR_DEFLECTION:
			value = Q952_DIVERT_REASON_CD;
			break;
		case PRI_REDIR_UNCONDITIONAL:
			value = Q952_DIVERT_REASON_CFU;
			break;
		case PRI_REDIR_DTE_OUT_OF_ORDER:
		case PRI_REDIR_FORWARDED_BY_DTE:
			pri_message(ctrl,
				"!! Don't know how to convert Q.931 redirection reason %d to Q.952\n",
				redirectingreason);
			/* Fall through */
		default:
			value = Q952_DIVERT_REASON_UNKNOWN;
			break;
		}
		break;
	}

	return value;
}

static int redirectingreason_for_q931(struct pri *ctrl, int redirectingreason)
{
	int value;

	switch (ctrl->switchtype) {
	case PRI_SWITCH_QSIG:
		switch (redirectingreason) {
		case QSIG_DIVERT_REASON_UNKNOWN:
			value = PRI_REDIR_UNKNOWN;
			break;
		case QSIG_DIVERT_REASON_CFU:
			value = PRI_REDIR_UNCONDITIONAL;
			break;
		case QSIG_DIVERT_REASON_CFB:
			value = PRI_REDIR_FORWARD_ON_BUSY;
			break;
		case QSIG_DIVERT_REASON_CFNR:
			value = PRI_REDIR_FORWARD_ON_NO_REPLY;
			break;
		default:
			pri_message(ctrl, "!! Unknown Q.SIG diversion reason %d\n",
				redirectingreason);
			value = PRI_REDIR_UNKNOWN;
			break;
		}
		break;
	default:
		switch (redirectingreason) {
		case Q952_DIVERT_REASON_UNKNOWN:
			value = PRI_REDIR_UNKNOWN;
			break;
		case Q952_DIVERT_REASON_CFU:
			value = PRI_REDIR_UNCONDITIONAL;
			break;
		case Q952_DIVERT_REASON_CFB:
			value = PRI_REDIR_FORWARD_ON_BUSY;
			break;
		case Q952_DIVERT_REASON_CFNR:
			value = PRI_REDIR_FORWARD_ON_NO_REPLY;
			break;
		case Q952_DIVERT_REASON_CD:
			value = PRI_REDIR_DEFLECTION;
			break;
		case Q952_DIVERT_REASON_IMMEDIATE:
			pri_message(ctrl,
				"!! Dont' know how to convert Q.952 diversion reason IMMEDIATE to PRI analog\n");
			value = PRI_REDIR_UNKNOWN;	/* ??? */
			break;
		default:
			pri_message(ctrl, "!! Unknown Q.952 diversion reason %d\n",
				redirectingreason);
			value = PRI_REDIR_UNKNOWN;
			break;
		}
		break;
	}

	return value;
}

/*!
 * \brief Convert the Q.931 type-of-number field to facility.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param ton Q.931 ton/plan octet.
 *
 * \return PartyNumber enumeration value.
 */
static int typeofnumber_from_q931(struct pri *ctrl, int ton)
{
	int value;

	switch ((ton >> 4) & 0x03) {
	default:
		pri_message(ctrl, "!! Unsupported Q.931 TypeOfNumber value (%d)\n", ton);
		/* fall through */
	case PRI_TON_UNKNOWN:
		value = Q932_TON_UNKNOWN;
		break;
	case PRI_TON_INTERNATIONAL:
		value = Q932_TON_INTERNATIONAL;
		break;
	case PRI_TON_NATIONAL:
		value = Q932_TON_NATIONAL;
		break;
	case PRI_TON_NET_SPECIFIC:
		value = Q932_TON_NET_SPECIFIC;
		break;
	case PRI_TON_SUBSCRIBER:
		value = Q932_TON_SUBSCRIBER;
		break;
	case PRI_TON_ABBREVIATED:
		value = Q932_TON_ABBREVIATED;
		break;
	}

	return value;
}

static int typeofnumber_for_q931(struct pri *ctrl, int ton)
{
	int value;

	switch (ton) {
	default:
		pri_message(ctrl, "!! Invalid TypeOfNumber %d\n", ton);
		/* fall through */
	case Q932_TON_UNKNOWN:
		value = PRI_TON_UNKNOWN;
		break;
	case Q932_TON_INTERNATIONAL:
		value = PRI_TON_INTERNATIONAL;
		break;
	case Q932_TON_NATIONAL:
		value = PRI_TON_NATIONAL;
		break;
	case Q932_TON_NET_SPECIFIC:
		value = PRI_TON_NET_SPECIFIC;
		break;
	case Q932_TON_SUBSCRIBER:
		value = PRI_TON_SUBSCRIBER;
		break;
	case Q932_TON_ABBREVIATED:
		value = PRI_TON_ABBREVIATED;
		break;
	}

	return value << 4;
}

/*!
 * \internal
 * \brief Convert the Q.931 numbering plan field to facility.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param plan Q.931 ton/plan octet.
 *
 * \return PartyNumber enumeration value.
 */
static int numbering_plan_from_q931(struct pri *ctrl, int plan)
{
	int value;

	switch (plan & 0x0F) {
	default:
		pri_message(ctrl, "!! Unsupported Q.931 numbering plan value (%d)\n", plan);
		/* fall through */
	case PRI_NPI_UNKNOWN:
		value = 0;	/* unknown */
		break;
	case PRI_NPI_E163_E164:
		value = 1;	/* public */
		break;
	case PRI_NPI_X121:
		value = 3;	/* data */
		break;
	case PRI_NPI_F69:
		value = 4;	/* telex */
		break;
	case PRI_NPI_NATIONAL:
		value = 8;	/* nationalStandard */
		break;
	case PRI_NPI_PRIVATE:
		value = 5;	/* private */
		break;
	}

	return value;
}

/*!
 * \internal
 * \brief Convert the PartyNumber numbering plan to Q.931 plan field value.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param plan PartyNumber enumeration value.
 *
 * \return Q.931 plan field value.
 */
static int numbering_plan_for_q931(struct pri *ctrl, int plan)
{
	int value;

	switch (plan) {
	default:
		pri_message(ctrl,
			"!! Unsupported PartyNumber to Q.931 numbering plan value (%d)\n", plan);
		/* fall through */
	case 0:	/* unknown */
		value = PRI_NPI_UNKNOWN;
		break;
	case 1:	/* public */
		value = PRI_NPI_E163_E164;
		break;
	case 3:	/* data */
		value = PRI_NPI_X121;
		break;
	case 4:	/* telex */
		value = PRI_NPI_F69;
		break;
	case 5:	/* private */
		value = PRI_NPI_PRIVATE;
		break;
	case 8:	/* nationalStandard */
		value = PRI_NPI_NATIONAL;
		break;
	}

	return value;
}

/*!
 * \internal
 * \brief Convert the Q.931 number presentation field to facility.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param presentation Q.931 presentation/screening octet.
 * \param number_present Non-zero if the number is available.
 *
 * \return Presented<Number/Address><Screened/Unscreened> enumeration value.
 */
static int presentation_from_q931(struct pri *ctrl, int presentation, int number_present)
{
	int value;

	switch (presentation & PRI_PRES_RESTRICTION) {
	case PRI_PRES_ALLOWED:
		value = 0;	/* presentationAllowed<Number/Address> */
		break;
	default:
		pri_message(ctrl, "!! Unsupported Q.931 number presentation value (%d)\n",
			presentation);
		/* fall through */
	case PRI_PRES_RESTRICTED:
		if (number_present) {
			value = 3;	/* presentationRestricted<Number/Address> */
		} else {
			value = 1;	/* presentationRestricted */
		}
		break;
	case PRI_PRES_UNAVAILABLE:
		value = 2;	/* numberNotAvailableDueToInterworking */
		break;
	}

	return value;
}

/*!
 * \internal
 * \brief Convert the Presented<Number/Address><Screened/Unscreened> presentation
 * to Q.931 presentation field value.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param presentation Presented<Number/Address><Screened/Unscreened> value.
 *
 * \return Q.931 presentation field value.
 */
static int presentation_for_q931(struct pri *ctrl, int presentation)
{
	int value;

	switch (presentation) {
	case 0:	/* presentationAllowed<Number/Address> */
		value = PRI_PRES_ALLOWED;
		break;
	default:
		pri_message(ctrl,
			"!! Unsupported Presented<Number/Address><Screened/Unscreened> to Q.931 value (%d)\n",
			presentation);
		/* fall through */
	case 1:	/* presentationRestricted */
	case 3:	/* presentationRestricted<Number/Address> */
		value = PRI_PRES_RESTRICTED;
		break;
	case 2:	/* numberNotAvailableDueToInterworking */
		value = PRI_PRES_UNAVAILABLE;
		break;
	}

	return value;
}

/*!
 * \internal
 * \brief Convert the Q.931 number presentation field to Q.SIG name presentation.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param presentation Q.931 presentation/screening octet.
 * \param name_present Non-zero if the name is available.
 *
 * \return Name presentation enumeration value.
 */
static int qsig_name_presentation_from_q931(struct pri *ctrl, int presentation, int name_present)
{
	int value;

	switch (presentation & PRI_PRES_RESTRICTION) {
	case PRI_PRES_ALLOWED:
		if (name_present) {
			value = 1;	/* presentation_allowed */
		} else {
			value = 4;	/* name_not_available */
		}
		break;
	default:
		pri_message(ctrl, "!! Unsupported Q.931 number presentation value (%d)\n",
			presentation);
		/* fall through */
	case PRI_PRES_RESTRICTED:
		if (name_present) {
			value = 2;	/* presentation_restricted */
		} else {
			value = 3;	/* presentation_restricted_null */
		}
		break;
	case PRI_PRES_UNAVAILABLE:
		value = 4;	/* name_not_available */
		break;
	}

	return value;
}

/*!
 * \internal
 * \brief Convert the Q.SIG name presentation to Q.931 presentation field value.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param presentation Q.SIG name presentation value.
 *
 * \return Q.931 presentation field value.
 */
static int qsig_name_presentation_for_q931(struct pri *ctrl, int presentation)
{
	int value;

	switch (presentation) {
	case 1:	/* presentation_allowed */
		value = PRI_PRES_ALLOWED;
		break;
	default:
		pri_message(ctrl,
			"!! Unsupported Q.SIG name presentation to Q.931 value (%d)\n",
			presentation);
		/* fall through */
	case 2:	/* presentation_restricted */
	case 3:	/* presentation_restricted_null */
		value = PRI_PRES_RESTRICTED;
		break;
	case 0:	/* optional_name_not_present */
	case 4:	/* name_not_available */
		value = PRI_PRES_UNAVAILABLE;
		break;
	}

	return value;
}

/*!
 * \internal
 * \brief Convert number presentation to Q.SIG diversion subscription notification.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param presentation Number presentation value.
 *
 * \return Q.SIG diversion subscription notification value.
 */
static int presentation_to_subscription(struct pri *ctrl, int presentation)
{
	/* derive subscription value from presentation value */

	switch (presentation & PRI_PRES_RESTRICTION) {
	case PRI_PRES_ALLOWED:
		return QSIG_NOTIFICATION_WITH_DIVERTED_TO_NR;
	case PRI_PRES_RESTRICTED:
		return QSIG_NOTIFICATION_WITHOUT_DIVERTED_TO_NR;
	case PRI_PRES_UNAVAILABLE:	/* Number not available due to interworking */
		return QSIG_NOTIFICATION_WITHOUT_DIVERTED_TO_NR;	/* ?? QSIG_NO_NOTIFICATION */
	default:
		pri_message(ctrl, "!! Unknown Q.SIG presentationIndicator 0x%02x\n",
			presentation);
		return QSIG_NOTIFICATION_WITHOUT_DIVERTED_TO_NR;
	}
}

/*!
 * \internal
 * \brief Copy the given rose party number to the q931_party_number
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param q931_number Q.931 party number structure
 * \param rose_number ROSE party number structure
 *
 * \note It is assumed that the q931_number has been initialized before calling.
 *
 * \return Nothing
 */
static void rose_copy_number_to_q931(struct pri *ctrl,
	struct q931_party_number *q931_number, const struct rosePartyNumber *rose_number)
{
	libpri_copy_string(q931_number->str, (char *) rose_number->str,
		sizeof(q931_number->str));
	q931_number->plan = numbering_plan_for_q931(ctrl, rose_number->plan)
		| typeofnumber_for_q931(ctrl, rose_number->ton);
}

/*!
 * \internal
 * \brief Copy the given rose presented screened party number to the q931_party_number
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param q931_number Q.931 party number structure
 * \param rose_presented ROSE presented screened party number structure
 *
 * \return Nothing
 */
static void rose_copy_presented_number_screened_to_q931(struct pri *ctrl,
	struct q931_party_number *q931_number,
	const struct rosePresentedNumberScreened *rose_presented)
{
	q931_party_number_init(q931_number);
	q931_number->valid = 1;
	q931_number->presentation = presentation_for_q931(ctrl, rose_presented->presentation);
	switch (rose_presented->presentation) {
	case 0:	/* presentationAllowedNumber */
	case 3:	/* presentationRestrictedNumber */
		q931_number->presentation |=
			(rose_presented->screened.screening_indicator & PRI_PRES_NUMBER_TYPE);
		rose_copy_number_to_q931(ctrl, q931_number,
			&rose_presented->screened.number);
		break;
	default:
		q931_number->presentation |= PRI_PRES_USER_NUMBER_UNSCREENED;
		break;
	}
}

/*!
 * \internal
 * \brief Copy the given rose presented unscreened party number to the q931_party_number
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param q931_number Q.931 party number structure
 * \param rose_presented ROSE presented unscreened party number structure
 *
 * \return Nothing
 */
static void rose_copy_presented_number_unscreened_to_q931(struct pri *ctrl,
	struct q931_party_number *q931_number,
	const struct rosePresentedNumberUnscreened *rose_presented)
{
	q931_party_number_init(q931_number);
	q931_number->valid = 1;
	q931_number->presentation = presentation_for_q931(ctrl,
		rose_presented->presentation) | PRI_PRES_USER_NUMBER_UNSCREENED;
	switch (rose_presented->presentation) {
	case 0:	/* presentationAllowedNumber */
	case 3:	/* presentationRestrictedNumber */
		rose_copy_number_to_q931(ctrl, q931_number, &rose_presented->number);
		break;
	default:
		break;
	}
}

/*!
 * \internal
 * \brief Copy the given rose presented screened party address to the q931_party_number
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param q931_number Q.931 party number structure
 * \param rose_presented ROSE presented screened party address structure
 *
 * \return Nothing
 */
static void rose_copy_presented_address_screened_to_q931(struct pri *ctrl,
	struct q931_party_number *q931_number,
	const struct rosePresentedAddressScreened *rose_presented)
{
	q931_party_number_init(q931_number);
	q931_number->valid = 1;
	q931_number->presentation = presentation_for_q931(ctrl, rose_presented->presentation);
	switch (rose_presented->presentation) {
	case 0:	/* presentationAllowedAddress */
	case 3:	/* presentationRestrictedAddress */
		q931_number->presentation |=
			(rose_presented->screened.screening_indicator & PRI_PRES_NUMBER_TYPE);
		rose_copy_number_to_q931(ctrl, q931_number,
			&rose_presented->screened.number);
		break;
	default:
		q931_number->presentation |= PRI_PRES_USER_NUMBER_UNSCREENED;
		break;
	}
}

/*!
 * \internal
 * \brief Copy the given rose party name to the q931_party_name
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param qsig_name Q.SIG party name structure
 * \param rose_name Q.SIG ROSE party name structure
 *
 * \return Nothing
 */
static void rose_copy_name_to_q931(struct pri *ctrl,
	struct q931_party_name *qsig_name, const struct roseQsigName *rose_name)
{
	//q931_party_name_init(qsig_name);
	qsig_name->valid = 1;
	qsig_name->presentation = qsig_name_presentation_for_q931(ctrl,
		rose_name->presentation);
	qsig_name->char_set = rose_name->char_set;
	libpri_copy_string(qsig_name->str, (char *) rose_name->data, sizeof(qsig_name->str));
}

/*!
 * \internal
 * \brief Copy the given q931_party_number to the rose party number
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param rose_number ROSE party number structure
 * \param q931_number Q.931 party number structure
 *
 * \return Nothing
 */
static void q931_copy_number_to_rose(struct pri *ctrl,
	struct rosePartyNumber *rose_number, const struct q931_party_number *q931_number)
{
	rose_number->plan = numbering_plan_from_q931(ctrl, q931_number->plan);
	rose_number->ton = typeofnumber_from_q931(ctrl, q931_number->plan);
	/* Truncate the q931_number->str if necessary. */
	libpri_copy_string((char *) rose_number->str, q931_number->str,
		sizeof(rose_number->str));
	rose_number->length = strlen((char *) rose_number->str);
}

/*!
 * \internal
 * \brief Copy the given q931_party_number to the rose presented screened party number
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param rose_presented ROSE presented screened party number structure
 * \param q931_number Q.931 party number structure
 *
 * \return Nothing
 */
static void q931_copy_presented_number_screened_to_rose(struct pri *ctrl,
	struct rosePresentedNumberScreened *rose_presented,
	const struct q931_party_number *q931_number)
{
	if (q931_number->valid) {
		rose_presented->presentation =
			presentation_from_q931(ctrl, q931_number->presentation, q931_number->str[0]);
		rose_presented->screened.screening_indicator =
			q931_number->presentation & PRI_PRES_NUMBER_TYPE;
		q931_copy_number_to_rose(ctrl, &rose_presented->screened.number, q931_number);
	} else {
		rose_presented->presentation = 2;/* numberNotAvailableDueToInterworking */
	}
}

/*!
 * \internal
 * \brief Copy the given q931_party_number to the rose presented unscreened party number
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param rose_presented ROSE presented unscreened party number structure
 * \param q931_number Q.931 party number structure
 *
 * \return Nothing
 */
static void q931_copy_presented_number_unscreened_to_rose(struct pri *ctrl,
	struct rosePresentedNumberUnscreened *rose_presented,
	const struct q931_party_number *q931_number)
{
	if (q931_number->valid) {
		rose_presented->presentation =
			presentation_from_q931(ctrl, q931_number->presentation, q931_number->str[0]);
		q931_copy_number_to_rose(ctrl, &rose_presented->number, q931_number);
	} else {
		rose_presented->presentation = 2;/* numberNotAvailableDueToInterworking */
	}
}

#if 0	/* In case it is needed in the future */
/*!
 * \internal
 * \brief Copy the given q931_party_number to the rose presented screened party address
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param rose_presented ROSE presented screened party address structure
 * \param q931_number Q.931 party number structure
 *
 * \return Nothing
 */
static void q931_copy_presented_address_screened_to_rose(struct pri *ctrl,
	struct rosePresentedAddressScreened *rose_presented,
	const struct q931_party_number *q931_number)
{
	if (q931_number->valid) {
		rose_presented->presentation =
			presentation_from_q931(ctrl, q931_number->presentation, q931_number->str[0]);
		rose_presented->screened.screening_indicator =
			q931_number->presentation & PRI_PRES_NUMBER_TYPE;
		q931_copy_number_to_rose(ctrl, &rose_presented->screened.number, q931_number);
		rose_presented->screened.subaddress.length = 0;
	} else {
		rose_presented->presentation = 2;/* numberNotAvailableDueToInterworking */
	}
}
#endif	/* In case it is needed in the future */

/*!
 * \internal
 * \brief Copy the given q931_party_name to the rose party name
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param rose_name Q.SIG ROSE party name structure
 * \param qsig_name Q.SIG party name structure
 *
 * \return Nothing
 */
static void q931_copy_name_to_rose(struct pri *ctrl,
	struct roseQsigName *rose_name, const struct q931_party_name *qsig_name)
{
	if (qsig_name->valid) {
		rose_name->presentation = qsig_name_presentation_from_q931(ctrl,
			qsig_name->presentation, qsig_name->str[0]);
		rose_name->char_set = qsig_name->char_set;
		/* Truncate the qsig_name->str if necessary. */
		libpri_copy_string((char *) rose_name->data, qsig_name->str, sizeof(rose_name->data));
		rose_name->length = strlen((char *) rose_name->data);
	} else {
		rose_name->presentation = 4;/* name_not_available */
	}
}

/*!
 * \internal
 * \brief Encode the Q.SIG DivertingLegInformation1 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode diversion leg 1.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_diverting_leg_information1(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, q931_call *call)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_DivertingLegInformation1;
	msg.invoke_id = get_invokeid(ctrl);
	msg.args.qsig.DivertingLegInformation1.diversion_reason =
		redirectingreason_from_q931(ctrl, call->redirecting.reason);

	/* subscriptionOption is the redirecting.to.number.presentation */
	msg.args.qsig.DivertingLegInformation1.subscription_option =
		presentation_to_subscription(ctrl, call->redirecting.to.number.presentation);

	/* nominatedNr is the redirecting.to.number */
	q931_copy_number_to_rose(ctrl,
		&msg.args.qsig.DivertingLegInformation1.nominated_number,
		&call->redirecting.to.number);

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode the ETSI DivertingLegInformation1 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode diversion leg 1.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_etsi_diverting_leg_information1(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, q931_call *call)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_ETSI_DivertingLegInformation1;
	msg.invoke_id = get_invokeid(ctrl);
	msg.args.etsi.DivertingLegInformation1.diversion_reason =
		redirectingreason_from_q931(ctrl, call->redirecting.reason);

	if (call->redirecting.to.number.valid) {
		msg.args.etsi.DivertingLegInformation1.subscription_option = 2;

		/* divertedToNumber is the redirecting.to.number */
		msg.args.etsi.DivertingLegInformation1.diverted_to_present = 1;
		q931_copy_presented_number_unscreened_to_rose(ctrl,
			&msg.args.etsi.DivertingLegInformation1.diverted_to,
			&call->redirecting.to.number);
	} else {
		msg.args.etsi.DivertingLegInformation1.subscription_option = 1;
	}
	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \brief Encode and queue the DivertingLegInformation1 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode diversion leg 1.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int rose_diverting_leg_information1_encode(struct pri *ctrl, q931_call *call)
{
	unsigned char buffer[256];
	unsigned char *end;

	switch (ctrl->switchtype) {
	case PRI_SWITCH_EUROISDN_E1:
	case PRI_SWITCH_EUROISDN_T1:
		end = enc_etsi_diverting_leg_information1(ctrl, buffer, buffer + sizeof(buffer),
			call);
		break;
	case PRI_SWITCH_QSIG:
		end = enc_qsig_diverting_leg_information1(ctrl, buffer, buffer + sizeof(buffer),
			call);
		break;
	default:
		return -1;
	}
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, Q931_FACILITY, buffer, end - buffer, NULL, NULL);
}

/*!
 * \internal
 * \brief Encode the Q.SIG DivertingLegInformation2 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode diversion leg 2.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_diverting_leg_information2(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, q931_call *call)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_DivertingLegInformation2;
	msg.invoke_id = get_invokeid(ctrl);

	/* diversionCounter is the redirecting.count */
	msg.args.qsig.DivertingLegInformation2.diversion_counter = call->redirecting.count;

	msg.args.qsig.DivertingLegInformation2.diversion_reason =
		redirectingreason_from_q931(ctrl, call->redirecting.reason);

	/* divertingNr is the redirecting.from.number */
	msg.args.qsig.DivertingLegInformation2.diverting_present = 1;
	q931_copy_presented_number_unscreened_to_rose(ctrl,
		&msg.args.qsig.DivertingLegInformation2.diverting,
		&call->redirecting.from.number);

	/* redirectingName is the redirecting.from.name */
	if (call->redirecting.from.name.valid) {
		msg.args.qsig.DivertingLegInformation2.redirecting_name_present = 1;
		q931_copy_name_to_rose(ctrl,
			&msg.args.qsig.DivertingLegInformation2.redirecting_name,
			&call->redirecting.from.name);
	}

	if (1 < call->redirecting.count) {
		/* originalCalledNr is the redirecting.orig_called.number */
		msg.args.qsig.DivertingLegInformation2.original_called_present = 1;
		q931_copy_presented_number_unscreened_to_rose(ctrl,
			&msg.args.qsig.DivertingLegInformation2.original_called,
			&call->redirecting.orig_called.number);

		msg.args.qsig.DivertingLegInformation2.original_diversion_reason_present = 1;
		if (call->redirecting.orig_called.number.valid) {
			msg.args.qsig.DivertingLegInformation2.original_diversion_reason =
				redirectingreason_from_q931(ctrl, call->redirecting.orig_reason);
		} else {
			msg.args.qsig.DivertingLegInformation2.original_diversion_reason =
				QSIG_DIVERT_REASON_UNKNOWN;
		}

		/* originalCalledName is the redirecting.orig_called.name */
		if (call->redirecting.orig_called.name.valid) {
			msg.args.qsig.DivertingLegInformation2.original_called_name_present = 1;
			q931_copy_name_to_rose(ctrl,
				&msg.args.qsig.DivertingLegInformation2.original_called_name,
				&call->redirecting.orig_called.name);
		}
	}

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode the ETSI DivertingLegInformation2 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode diversion leg 2.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_etsi_diverting_leg_information2(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, q931_call *call)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_ETSI_DivertingLegInformation2;
	msg.invoke_id = get_invokeid(ctrl);

	/* diversionCounter is the redirecting.count */
	msg.args.etsi.DivertingLegInformation2.diversion_counter = call->redirecting.count;

	msg.args.etsi.DivertingLegInformation2.diversion_reason =
		redirectingreason_from_q931(ctrl, call->redirecting.reason);

	/* divertingNr is the redirecting.from.number */
	msg.args.etsi.DivertingLegInformation2.diverting_present = 1;
	q931_copy_presented_number_unscreened_to_rose(ctrl,
		&msg.args.etsi.DivertingLegInformation2.diverting,
		&call->redirecting.from.number);

	if (1 < call->redirecting.count) {
		/* originalCalledNr is the redirecting.orig_called.number */
		msg.args.etsi.DivertingLegInformation2.original_called_present = 1;
		q931_copy_presented_number_unscreened_to_rose(ctrl,
			&msg.args.etsi.DivertingLegInformation2.original_called,
			&call->redirecting.orig_called.number);
	}

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode and queue the DivertingLegInformation2 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode diversion leg 2.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int rose_diverting_leg_information2_encode(struct pri *ctrl, q931_call *call)
{
	unsigned char buffer[256];
	unsigned char *end;

	switch (ctrl->switchtype) {
	case PRI_SWITCH_EUROISDN_E1:
	case PRI_SWITCH_EUROISDN_T1:
		end = enc_etsi_diverting_leg_information2(ctrl, buffer, buffer + sizeof(buffer),
			call);
		break;
	case PRI_SWITCH_QSIG:
		end = enc_qsig_diverting_leg_information2(ctrl, buffer, buffer + sizeof(buffer),
			call);
		break;
	default:
		return -1;
	}
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, Q931_SETUP, buffer, end - buffer, NULL, NULL);
}

/*!
 * \internal
 * \brief Encode the Q.SIG DivertingLegInformation3 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode diversion leg 3.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_diverting_leg_information3(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, q931_call *call)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_DivertingLegInformation3;
	msg.invoke_id = get_invokeid(ctrl);

	/* redirecting.to.number.presentation also indicates if name presentation is allowed */
	if ((call->redirecting.to.number.presentation & PRI_PRES_RESTRICTION) == PRI_PRES_ALLOWED) {
		msg.args.qsig.DivertingLegInformation3.presentation_allowed_indicator = 1;	/* TRUE */

		/* redirectionName is the redirecting.to.name */
		if (call->redirecting.to.name.valid) {
			msg.args.qsig.DivertingLegInformation3.redirection_name_present = 1;
			q931_copy_name_to_rose(ctrl,
				&msg.args.qsig.DivertingLegInformation3.redirection_name,
				&call->redirecting.to.name);
		}
	}

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode the ETSI DivertingLegInformation3 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode diversion leg 3.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_etsi_diverting_leg_information3(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, q931_call *call)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_ETSI_DivertingLegInformation3;
	msg.invoke_id = get_invokeid(ctrl);

	if ((call->redirecting.to.number.presentation & PRI_PRES_RESTRICTION) == PRI_PRES_ALLOWED) {
		msg.args.etsi.DivertingLegInformation3.presentation_allowed_indicator = 1;	/* TRUE */
	}

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \brief Encode and queue the DivertingLegInformation3 invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode diversion leg 3.
 * \param messagetype Q.931 message type to add facility ie to.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int rose_diverting_leg_information3_encode(struct pri *ctrl, q931_call *call,
	int messagetype)
{
	unsigned char buffer[256];
	unsigned char *end;

	switch (ctrl->switchtype) {
	case PRI_SWITCH_EUROISDN_E1:
	case PRI_SWITCH_EUROISDN_T1:
		end = enc_etsi_diverting_leg_information3(ctrl, buffer, buffer + sizeof(buffer),
			call);
		break;
	case PRI_SWITCH_QSIG:
		end = enc_qsig_diverting_leg_information3(ctrl, buffer, buffer + sizeof(buffer),
			call);
		break;
	default:
		return -1;
	}
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, messagetype, buffer, end - buffer, NULL, NULL);
}

/*!
 * \internal
 * \brief Encode the rltThirdParty invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param callwithid Call-ID information to encode.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_dms100_rlt_initiate_transfer(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, const q931_call *callwithid)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_DMS100_RLT_ThirdParty;
	msg.invoke_id = ROSE_DMS100_RLT_THIRD_PARTY;
	msg.args.dms100.RLT_ThirdParty.call_id = callwithid->rlt_call_id & 0xFFFFFF;
	msg.args.dms100.RLT_ThirdParty.reason = 0;	/* unused, set to 129 */
	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \brief Send the rltThirdParty: Invoke.
 *
 * \note For PRI_SWITCH_DMS100 only.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param c1 Q.931 call leg 1
 * \param c2 Q.931 call leg 2
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int rlt_initiate_transfer(struct pri *ctrl, q931_call *c1, q931_call *c2)
{
	unsigned char buffer[256];
	unsigned char *end;
	q931_call *apdubearer;
	q931_call *callwithid;

	if (c2->transferable) {
		apdubearer = c1;
		callwithid = c2;
	} else if (c1->transferable) {
		apdubearer = c2;
		callwithid = c1;
	} else {
		return -1;
	}

	end =
		enc_dms100_rlt_initiate_transfer(ctrl, buffer, buffer + sizeof(buffer),
		callwithid);
	if (!end) {
		return -1;
	}

	if (pri_call_apdu_queue(apdubearer, Q931_FACILITY, buffer, end - buffer, NULL, NULL)) {
		return -1;
	}

	if (q931_facility(apdubearer->pri, apdubearer)) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n",
			apdubearer->cr);
		return -1;
	}
	return 0;
}

/*!
 * \internal
 * \brief Encode the rltOperationInd invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_dms100_rlt_transfer_ability(struct pri *ctrl,
	unsigned char *pos, unsigned char *end)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_DMS100_RLT_OperationInd;
	msg.invoke_id = ROSE_DMS100_RLT_OPERATION_IND;
	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Send the rltOperationInd: Invoke.
 *
 * \note For PRI_SWITCH_DMS100 only.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Q.931 call leg
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int add_dms100_transfer_ability_apdu(struct pri *ctrl, q931_call *call)
{
	unsigned char buffer[256];
	unsigned char *end;

	end = enc_dms100_rlt_transfer_ability(ctrl, buffer, buffer + sizeof(buffer));
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, Q931_SETUP, buffer, end - buffer, NULL, NULL);
}

/*!
 * \internal
 * \brief Encode the NI2 InformationFollowing invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_ni2_information_following(struct pri *ctrl, unsigned char *pos,
	unsigned char *end)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_NI2_InformationFollowing;
	msg.invoke_id = get_invokeid(ctrl);
	msg.args.ni2.InformationFollowing.value = 0;
	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode the Q.SIG CallingName invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param name Name data which to encode name.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_calling_name(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, const struct q931_party_name *name)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	if (ctrl->switchtype == PRI_SWITCH_QSIG) {
		header.nfe_present = 1;
		header.nfe.source_entity = 0;	/* endPINX */
		header.nfe.destination_entity = 0;	/* endPINX */
	}
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_CallingName;
	msg.invoke_id = get_invokeid(ctrl);

	/* CallingName */
	q931_copy_name_to_rose(ctrl, &msg.args.qsig.CallingName.name, name);

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Send caller name information.
 *
 * \note For PRI_SWITCH_NI2 and PRI_SWITCH_QSIG.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode name.
 * \param cpe TRUE if we are the CPE side.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int add_callername_facility_ies(struct pri *ctrl, q931_call *call, int cpe)
{
	unsigned char buffer[256];
	unsigned char *end;
	int mymessage;

	if (!call->local_id.name.valid) {
		return 0;
	}

	if (ctrl->switchtype == PRI_SWITCH_NI2 && !cpe) {
		end = enc_ni2_information_following(ctrl, buffer, buffer + sizeof(buffer));
		if (!end) {
			return -1;
		}

		if (pri_call_apdu_queue(call, Q931_SETUP, buffer, end - buffer, NULL, NULL)) {
			return -1;
		}

		/*
		 * We can reuse the buffer since the queue function doesn't
		 * need it.
		 */
	}

	/* CallingName is the local_id.name */
	end = enc_qsig_calling_name(ctrl, buffer, buffer + sizeof(buffer),
		&call->local_id.name);
	if (!end) {
		return -1;
	}

	if (cpe) {
		mymessage = Q931_SETUP;
	} else {
		mymessage = Q931_FACILITY;
	}

	return pri_call_apdu_queue(call, mymessage, buffer, end - buffer, NULL, NULL);
}
/* End Callername */

/* MWI related encode and decode functions */
static void mwi_activate_encode_cb(void *data)
{
	return;
}

/*!
 * \internal
 * \brief Encode the Q.SIG MWIActivate invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param req Served user setup request information.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_mwi_activate_message(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, struct pri_sr *req)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_MWIActivate;
	msg.invoke_id = get_invokeid(ctrl);

	/* The called.number is the served user */
	q931_copy_number_to_rose(ctrl, &msg.args.qsig.MWIActivate.served_user_number,
		&req->called.number);
	/*
	 * For now, we will just force the numbering plan to unknown to preserve
	 * the original behaviour.
	 */
	msg.args.qsig.MWIActivate.served_user_number.plan = 0;	/* unknown */

	msg.args.qsig.MWIActivate.basic_service = 1;	/* speech */

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode the Q.SIG MWIDeactivate invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param req Served user setup request information.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_mwi_deactivate_message(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, struct pri_sr *req)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_MWIDeactivate;
	msg.invoke_id = get_invokeid(ctrl);

	/* The called.number is the served user */
	q931_copy_number_to_rose(ctrl, &msg.args.qsig.MWIDeactivate.served_user_number,
		&req->called.number);
	/*
	 * For now, we will just force the numbering plan to unknown to preserve
	 * the original behaviour.
	 */
	msg.args.qsig.MWIDeactivate.served_user_number.plan = 0;	/* unknown */

	msg.args.qsig.MWIDeactivate.basic_service = 1;	/* speech */

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \brief Encode and queue the Q.SIG MWIActivate/MWIDeactivate invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg to queue message.
 * \param req Served user setup request information.
 * \param activate Nonzero to do the activate message.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int mwi_message_send(struct pri *ctrl, q931_call *call, struct pri_sr *req, int activate)
{
	unsigned char buffer[255];
	unsigned char *end;

	if (!req->called.number.valid || !req->called.number.str[0]) {
		return -1;
	}

	if (activate) {
		end = enc_qsig_mwi_activate_message(ctrl, buffer, buffer + sizeof(buffer), req);
	} else {
		end =
			enc_qsig_mwi_deactivate_message(ctrl, buffer, buffer + sizeof(buffer), req);
	}
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, Q931_SETUP, buffer, end - buffer,
		mwi_activate_encode_cb, NULL);
}
/* End MWI */

/* EECT functions */
/*!
 * \internal
 * \brief Encode the NI2 InitiateTransfer invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode transfer information.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_ni2_initiate_transfer(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, q931_call *call)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_NI2_InitiateTransfer;
	msg.invoke_id = get_invokeid(ctrl);
	/* Let's do the trickery to make sure the flag is correct */
	msg.args.ni2.InitiateTransfer.call_reference = call->cr ^ 0x8000;
	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \brief Start a 2BCT
 *
 * \note Called for PRI_SWITCH_NI2, PRI_SWITCH_LUCENT5E, and PRI_SWITCH_ATT4ESS
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param c1 Q.931 call leg 1
 * \param c2 Q.931 call leg 2
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int eect_initiate_transfer(struct pri *ctrl, q931_call *c1, q931_call *c2)
{
	unsigned char buffer[255];
	unsigned char *end;

	end = enc_ni2_initiate_transfer(ctrl, buffer, buffer + sizeof(buffer), c2);
	if (!end) {
		return -1;
	}

	if (pri_call_apdu_queue(c1, Q931_FACILITY, buffer, end - buffer, NULL, NULL)) {
		pri_message(ctrl, "Could not queue APDU in facility message\n");
		return -1;
	}

	/* Remember that if we queue a facility IE for a facility message we
	 * have to explicitly send the facility message ourselves */

	if (q931_facility(c1->pri, c1)) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n", c1->cr);
		return -1;
	}

	return 0;
}
/* End EECT */

/* QSIG CF CallRerouting */
/*!
 * \internal
 * \brief Encode the Q.SIG CallRerouting invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param calling Calling number.
 * \param dest Destination number.
 * \param original Original called number.
 * \param reason Rerouting reason: cfu, cfb, cfnr
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_call_rerouting(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, const char *calling, const char *dest, const char *original,
	const char *reason)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	static const unsigned char q931ie[] = {
		0x04,	/* Bearer Capability IE */
		0x03,	/* len */
		0x80,	/* ETSI Standard, Speech */
		0x90,	/* circuit mode, 64kbit/s */
		0xa3,	/* level1 protocol, a-law */
		0x95,	/* locking shift to codeset 5 (national use) */
		0x32,	/* Unknown ie */
		0x01,	/* Unknown ie len */
		0x81,	/* Unknown ie body */
	};

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 2;	/* rejectAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_CallRerouting;
	msg.invoke_id = get_invokeid(ctrl);

	/* The rerouting_reason defaults to unknown */
	if (reason) {
		if (!strcasecmp(reason, "cfu")) {
			msg.args.qsig.CallRerouting.rerouting_reason = 1;	/* cfu */
		} else if (!strcasecmp(reason, "cfb")) {
			msg.args.qsig.CallRerouting.rerouting_reason = 2;	/* cfb */
		} else if (!strcasecmp(reason, "cfnr")) {
			msg.args.qsig.CallRerouting.rerouting_reason = 3;	/* cfnr */
		}
	}

	/* calledAddress is the passed in dest number */
	msg.args.qsig.CallRerouting.called.number.plan = 1;	/* public */
	msg.args.qsig.CallRerouting.called.number.ton = 0;	/* unknown */
	libpri_copy_string((char *) msg.args.qsig.CallRerouting.called.number.str, dest,
		sizeof(msg.args.qsig.CallRerouting.called.number.str));
	msg.args.qsig.CallRerouting.called.number.length = strlen((char *)
		msg.args.qsig.CallRerouting.called.number.str);

	msg.args.qsig.CallRerouting.diversion_counter = 1;

	/* pSS1InfoElement */
	msg.args.qsig.CallRerouting.q931ie.length = sizeof(q931ie);
	memcpy(msg.args.qsig.CallRerouting.q931ie_contents, q931ie, sizeof(q931ie));

	/* lastReroutingNr is the passed in original number */
	msg.args.qsig.CallRerouting.last_rerouting.presentation = 0;	/* presentationAllowedNumber */
	msg.args.qsig.CallRerouting.last_rerouting.number.plan = 1;	/* public */
	msg.args.qsig.CallRerouting.last_rerouting.number.ton = 0;	/* unknown */
	libpri_copy_string((char *) msg.args.qsig.CallRerouting.last_rerouting.number.str,
		original, sizeof(msg.args.qsig.CallRerouting.last_rerouting.number.str));
	msg.args.qsig.CallRerouting.last_rerouting.number.length = strlen((char *)
		msg.args.qsig.CallRerouting.last_rerouting.number.str);

	msg.args.qsig.CallRerouting.subscription_option = 0;	/* noNotification */

	/* callingNumber is the passed in calling number */
	msg.args.qsig.CallRerouting.calling.presentation = 0;	/* presentationAllowedNumber */
	msg.args.qsig.CallRerouting.calling.screened.number.plan = 1;	/* public */
	msg.args.qsig.CallRerouting.calling.screened.number.ton = 0;	/* unknown */
	libpri_copy_string((char *) msg.args.qsig.CallRerouting.calling.screened.number.str,
		calling, sizeof(msg.args.qsig.CallRerouting.calling.screened.number.str));
	msg.args.qsig.CallRerouting.calling.screened.number.length = strlen((char *)
		msg.args.qsig.CallRerouting.calling.screened.number.str);
	msg.args.qsig.CallRerouting.calling.screened.screening_indicator = 3;	/* networkProvided */

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \brief Send the Q.SIG CallRerouting invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode name.
 * \param dest Destination number.
 * \param original Original called number.
 * \param reason Rerouting reason: cfu, cfb, cfnr
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int qsig_cf_callrerouting(struct pri *ctrl, q931_call *call, const char *dest,
	const char *original, const char *reason)
{
	unsigned char buffer[255];
	unsigned char *end;
	int res;

	/*
	 * We are deflecting an incoming call back to the network.
	 * Therefore, the Caller-ID is the remote party.
	 */
	end =
		enc_qsig_call_rerouting(ctrl, buffer, buffer + sizeof(buffer),
			call->remote_id.number.str, dest, original ? original :
			call->called.number.str, reason);
	if (!end) {
		return -1;
	}

	res = pri_call_apdu_queue(call, Q931_FACILITY, buffer, end - buffer, NULL, NULL);
	if (res) {
		pri_message(ctrl, "Could not queue ADPU in facility message\n");
		return -1;
	}

	/* Remember that if we queue a facility IE for a facility message we
	 * have to explicitly send the facility message ourselves */

	res = q931_facility(call->pri, call);
	if (res) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n", call->cr);
		return -1;
	}

	return 0;
}
/* End QSIG CC-CallRerouting */

/*
 * From Mantis issue 7778 description: (ETS 300 258, ISO 13863)
 * After both legs of the call are setup and Asterisk has a successful "tromboned" or bridged call ...
 * Asterisk sees both 'B' channels (from trombone) are on same PRI/technology and initiates "Path Replacement" events
 * a. Asterisk sends "Transfer Complete" messages to both call legs
 * b. QSIG Switch sends "PathReplacement" message on one of the legs (random 1-10sec timer expires - 1st leg to send is it!)
 * c. Asterisk rebroadcasts "PathReplacement" message to other call leg
 * d. QSIG Switch sends "Disconnect" message on one of the legs (same random timer sequence as above)
 * e. Asterisk rebroadcasts "Disconnect" message to other call leg
 * f. QSIG Switch disconnects Asterisk call legs - callers are now within QSIG switch
 *
 * Just need to resend the message to the other tromboned leg of the call.
 */
static int anfpr_pathreplacement_respond(struct pri *ctrl, q931_call *call, q931_ie *ie)
{
	int res;

	res = pri_call_apdu_queue_cleanup(call->bridged_call);
	if (res) {
		pri_message(ctrl, "Could not Clear queue ADPU\n");
		return -1;
	}

	/* Send message */
	res =
		pri_call_apdu_queue(call->bridged_call, Q931_FACILITY, ie->data, ie->len, NULL,
		NULL);
	if (res) {
		pri_message(ctrl, "Could not queue ADPU in facility message\n");
		return -1;
	}

	/* Remember that if we queue a facility IE for a facility message we
	 * have to explicitly send the facility message ourselves */

	res = q931_facility(call->bridged_call->pri, call->bridged_call);
	if (res) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n",
			call->bridged_call->cr);
		return -1;
	}

	return 0;
}

/* AFN-PR */
/*!
 * \brief Start a Q.SIG path replacement.
 *
 * \note Called for PRI_SWITCH_QSIG
 *
 * \note Did all the tests to see if we're on the same PRI and
 * are on a compatible switchtype.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param c1 Q.931 call leg 1
 * \param c2 Q.931 call leg 2
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int anfpr_initiate_transfer(struct pri *ctrl, q931_call *c1, q931_call *c2)
{
	unsigned char buffer[255];
	unsigned char *pos;
	unsigned char *end;
	int res;
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	end = buffer + sizeof(buffer);

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 2;	/* rejectAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, buffer, end, &header);
	if (!pos) {
		return -1;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_CallTransferComplete;
	msg.invoke_id = get_invokeid(ctrl);
	msg.args.qsig.CallTransferComplete.end_designation = 0;	/* primaryEnd */
	msg.args.qsig.CallTransferComplete.redirection.presentation = 1;	/* presentationRestricted */
	msg.args.qsig.CallTransferComplete.call_status = 1;	/* alerting */
	pos = rose_encode_invoke(ctrl, pos, end, &msg);
	if (!pos) {
		return -1;
	}

	res = pri_call_apdu_queue(c1, Q931_FACILITY, buffer, pos - buffer, NULL, NULL);
	if (res) {
		pri_message(ctrl, "Could not queue ADPU in facility message\n");
		return -1;
	}

	/* Remember that if we queue a facility IE for a facility message we
	 * have to explicitly send the facility message ourselves */

	res = q931_facility(c1->pri, c1);
	if (res) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n", c1->cr);
		return -1;
	}

	/* Reuse the previous message header */
	pos = facility_encode_header(ctrl, buffer, end, &header);
	if (!pos) {
		return -1;
	}

	/* Update the previous message */
	msg.invoke_id = get_invokeid(ctrl);
	msg.args.qsig.CallTransferComplete.end_designation = 1;	/* secondaryEnd */
	pos = rose_encode_invoke(ctrl, pos, end, &msg);
	if (!pos) {
		return -1;
	}

	res = pri_call_apdu_queue(c2, Q931_FACILITY, buffer, pos - buffer, NULL, NULL);
	if (res) {
		pri_message(ctrl, "Could not queue ADPU in facility message\n");
		return -1;
	}

	/* Remember that if we queue a facility IE for a facility message we
	 * have to explicitly send the facility message ourselves */

	res = q931_facility(c2->pri, c2);
	if (res) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n", c2->cr);
		return -1;
	}

	return 0;
}
/* End AFN-PR */

/* AOC */
/*!
 * \internal
 * \brief Encode the ETSI AOCEChargingUnit invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param chargedunits Number of units charged to encode.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_etsi_aoce_charging_unit(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, long chargedunits)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_ETSI_AOCEChargingUnit;
	msg.invoke_id = get_invokeid(ctrl);
	msg.args.etsi.AOCEChargingUnit.type = 1;	/* charging_unit */
	if (chargedunits <= 0) {
		msg.args.etsi.AOCEChargingUnit.charging_unit.free_of_charge = 1;
	} else {
		msg.args.etsi.AOCEChargingUnit.charging_unit.specific.recorded.num_records = 1;
		msg.args.etsi.AOCEChargingUnit.charging_unit.specific.recorded.list[0].
			number_of_units = chargedunits;
	}
	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Send the ETSI AOCEChargingUnit invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode AOC.
 * \param chargedunits Number of units charged to encode.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int aoc_aoce_charging_unit_encode(struct pri *ctrl, q931_call *call,
	long chargedunits)
{
	unsigned char buffer[255];
	unsigned char *end;

	/* sample data: [ 91 a1 12 02 02 3a 78 02 01 24 30 09 30 07 a1 05 30 03 02 01 01 ] */

	end =
		enc_etsi_aoce_charging_unit(ctrl, buffer, buffer + sizeof(buffer), chargedunits);
	if (!end) {
		return -1;
	}

	/* Remember that if we queue a facility IE for a facility message we
	 * have to explicitly send the facility message ourselves */
	if (pri_call_apdu_queue(call, Q931_FACILITY, buffer, end - buffer, NULL, NULL)
		|| q931_facility(call->pri, call)) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n", call->cr);
		return -1;
	}

	return 0;
}
/* End AOC */

/* ===== Call Transfer Supplementary Service (ECMA-178) ===== */

/*!
 * \internal
 * \brief Encode the Q.SIG CallTransferComplete invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode call transfer.
 * \param call_status TRUE if call is alerting.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_call_transfer_complete(struct pri *ctrl,
	unsigned char *pos, unsigned char *end, q931_call *call, int call_status)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_CallTransferComplete;
	msg.invoke_id = get_invokeid(ctrl);
	msg.args.qsig.CallTransferComplete.end_designation = 0;	/* primaryEnd */

	/* redirectionNumber is the local_id.number */
	q931_copy_presented_number_screened_to_rose(ctrl,
		&msg.args.qsig.CallTransferComplete.redirection, &call->local_id.number);

	/* redirectionName is the local_id.name */
	if (call->local_id.name.valid) {
		msg.args.qsig.CallTransferComplete.redirection_name_present = 1;
		q931_copy_name_to_rose(ctrl,
			&msg.args.qsig.CallTransferComplete.redirection_name,
			&call->local_id.name);
	}

	if (call_status) {
		msg.args.qsig.CallTransferComplete.call_status = 1;	/* alerting */
	}
	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode the ETSI EctInform invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param call Call leg from which to encode inform message.
 * \param call_status TRUE if call is alerting.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_etsi_ect_inform(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, q931_call *call, int call_status)
{
	struct rose_msg_invoke msg;

	pos = facility_encode_header(ctrl, pos, end, NULL);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_ETSI_EctInform;
	msg.invoke_id = get_invokeid(ctrl);

	if (!call_status) {
		msg.args.etsi.EctInform.status = 1;/* active */

		/*
		 * EctInform(active) contains the redirectionNumber
		 * redirectionNumber is the local_id.number
		 */
		msg.args.etsi.EctInform.redirection_present = 1;
		q931_copy_presented_number_unscreened_to_rose(ctrl,
			&msg.args.etsi.EctInform.redirection, &call->local_id.number);
	}

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode and queue the CallTransferComplete/EctInform invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode call transfer.
 * \param call_status TRUE if call is alerting.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int rose_call_transfer_complete_encode(struct pri *ctrl, q931_call *call,
	int call_status)
{
	unsigned char buffer[256];
	unsigned char *end;

	switch (ctrl->switchtype) {
	case PRI_SWITCH_EUROISDN_E1:
	case PRI_SWITCH_EUROISDN_T1:
		end =
			enc_etsi_ect_inform(ctrl, buffer, buffer + sizeof(buffer), call, call_status);
		break;
	case PRI_SWITCH_QSIG:
		end =
			enc_qsig_call_transfer_complete(ctrl, buffer, buffer + sizeof(buffer), call,
			call_status);
		break;
	default:
		return -1;
	}
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, Q931_FACILITY, buffer, end - buffer, NULL, NULL);
}

/* ===== End Call Transfer Supplementary Service (ECMA-178) ===== */

/*!
 * \internal
 * \brief Encode the Q.SIG CalledName invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param name Name data which to encode name.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_called_name(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, const struct q931_party_name *name)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_CalledName;
	msg.invoke_id = get_invokeid(ctrl);

	/* CalledName */
	q931_copy_name_to_rose(ctrl, &msg.args.qsig.CalledName.name, name);

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode and queue the Q.SIG CalledName invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode name.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int rose_called_name_encode(struct pri *ctrl, q931_call *call, int messagetype)
{
	unsigned char buffer[256];
	unsigned char *end;

	/* CalledName is the local_id.name */
	end = enc_qsig_called_name(ctrl, buffer, buffer + sizeof(buffer),
		&call->local_id.name);
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, messagetype, buffer, end - buffer, NULL, NULL);
}

/*!
 * \internal
 * \brief Encode the Q.SIG ConnectedName invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param pos Starting position to encode the facility ie contents.
 * \param end End of facility ie contents encoding data buffer.
 * \param name Name data which to encode name.
 *
 * \retval Start of the next ASN.1 component to encode on success.
 * \retval NULL on error.
 */
static unsigned char *enc_qsig_connected_name(struct pri *ctrl, unsigned char *pos,
	unsigned char *end, const struct q931_party_name *name)
{
	struct fac_extension_header header;
	struct rose_msg_invoke msg;

	memset(&header, 0, sizeof(header));
	header.nfe_present = 1;
	header.nfe.source_entity = 0;	/* endPINX */
	header.nfe.destination_entity = 0;	/* endPINX */
	header.interpretation_present = 1;
	header.interpretation = 0;	/* discardAnyUnrecognisedInvokePdu */
	pos = facility_encode_header(ctrl, pos, end, &header);
	if (!pos) {
		return NULL;
	}

	memset(&msg, 0, sizeof(msg));
	msg.operation = ROSE_QSIG_ConnectedName;
	msg.invoke_id = get_invokeid(ctrl);

	/* ConnectedName */
	q931_copy_name_to_rose(ctrl, &msg.args.qsig.ConnectedName.name, name);

	pos = rose_encode_invoke(ctrl, pos, end, &msg);

	return pos;
}

/*!
 * \internal
 * \brief Encode and queue the Q.SIG ConnectedName invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode name.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int rose_connected_name_encode(struct pri *ctrl, q931_call *call, int messagetype)
{
	unsigned char buffer[256];
	unsigned char *end;

	/* ConnectedName is the local_id.name */
	end = enc_qsig_connected_name(ctrl, buffer, buffer + sizeof(buffer),
		&call->local_id.name);
	if (!end) {
		return -1;
	}

	return pri_call_apdu_queue(call, messagetype, buffer, end - buffer, NULL, NULL);
}

/*!
 * \brief Put the APDU on the call queue.
 *
 * \param call Call to enqueue message.
 * \param messagetype Q.931 message type.
 * \param apdu Facility ie contents buffer.
 * \param apdu_len Length of the contents buffer.
 * \param function Callback function for when response is received. (Not implemented)
 * \param data Parameter to callback function.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int pri_call_apdu_queue(q931_call *call, int messagetype, void *apdu, int apdu_len,
	void (*function)(void *data), void *data)
{
	struct apdu_event *cur = NULL;
	struct apdu_event *new_event = NULL;

	if (!call || !messagetype || !apdu || (apdu_len < 1) || (apdu_len > 255))
		return -1;

	if (!(new_event = calloc(1, sizeof(*new_event)))) {
		pri_error(call->pri, "!! Malloc failed!\n");
		return -1;
	}

	new_event->message = messagetype;
	new_event->callback = function;
	new_event->data = data;
	memcpy(new_event->apdu, apdu, apdu_len);
	new_event->apdu_len = apdu_len;

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

int pri_call_apdu_queue_cleanup(q931_call *call)
{
	struct apdu_event *cur_event = NULL, *free_event = NULL;

	if (call && call->apdus) {
		cur_event = call->apdus;
		while (cur_event) {
			/* TODO: callbacks, some way of giving return res on status of apdu */
			free_event = cur_event;
			cur_event = cur_event->next;
			free(free_event);
		}
		call->apdus = NULL;
	}

	return 0;
}

/*! \note Only called when sending the SETUP message. */
int pri_call_add_standard_apdus(struct pri *ctrl, q931_call *call)
{
	if (!ctrl->sendfacility) {
		return 0;
	}

	switch (ctrl->switchtype) {
	case PRI_SWITCH_EUROISDN_E1:
	case PRI_SWITCH_EUROISDN_T1:
		if (q931_is_ptmp(ctrl)) {
			/* PTMP mode */
			break;
		}
		/* PTP mode */
		if (call->redirecting.count) {
			rose_diverting_leg_information2_encode(ctrl, call);

			/*
			 * Expect a DivertingLegInformation3 to update the COLR of the
			 * redirecting-to party we are attempting to call now.
			 */
			call->redirecting.state = Q931_REDIRECTING_STATE_EXPECTING_RX_DIV_LEG_3;
		}
		break;
	case PRI_SWITCH_QSIG:
		/* For Q.SIG it does network and cpe operations */
		if (call->redirecting.count) {
			rose_diverting_leg_information2_encode(ctrl, call);

			/*
			 * Expect a DivertingLegInformation3 to update the COLR of the
			 * redirecting-to party we are attempting to call now.
			 */
			call->redirecting.state = Q931_REDIRECTING_STATE_EXPECTING_RX_DIV_LEG_3;
		}
		add_callername_facility_ies(ctrl, call, 1);
		break;
	case PRI_SWITCH_NI2:
		add_callername_facility_ies(ctrl, call, (ctrl->localtype == PRI_CPE));
		break;
	case PRI_SWITCH_DMS100:
		if (ctrl->localtype == PRI_CPE) {
			add_dms100_transfer_ability_apdu(ctrl, call);
		}
		break;
	default:
		break;
	}

	return 0;
}

/*!
 * \brief Send the CallTransferComplete/EctInform invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which to encode call transfer.
 * \param call_status TRUE if call is alerting.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int send_call_transfer_complete(struct pri *ctrl, q931_call *call, int call_status)
{
	if (rose_call_transfer_complete_encode(ctrl, call, call_status)
		|| q931_facility(ctrl, call)) {
		pri_message(ctrl,
			"Could not schedule facility message for call transfer completed.\n");
		return -1;
	}

	return 0;
}

/*!
 * \brief Handle the ROSE reject message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which the message came.
 * \param ie Raw ie contents.
 * \param header Decoded facility header before ROSE.
 * \param reject Decoded ROSE reject message contents.
 *
 * \return Nothing
 */
void rose_handle_reject(struct pri *ctrl, q931_call *call, q931_ie *ie,
	const struct fac_extension_header *header, const struct rose_msg_reject *reject)
{
	pri_error(ctrl, "ROSE REJECT:\n");
	if (reject->invoke_id_present) {
		pri_error(ctrl, "\tINVOKE ID: %d\n", reject->invoke_id);
	}
	pri_error(ctrl, "\tPROBLEM: %s\n", rose_reject2str(reject->code));
}

/*!
 * \brief Handle the ROSE error message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which the message came.
 * \param ie Raw ie contents.
 * \param header Decoded facility header before ROSE.
 * \param error Decoded ROSE error message contents.
 *
 * \return Nothing
 */
void rose_handle_error(struct pri *ctrl, q931_call *call, q931_ie *ie,
	const struct fac_extension_header *header, const struct rose_msg_error *error)
{
	const char *dms100_operation;

	pri_error(ctrl, "ROSE RETURN ERROR:\n");
	switch (ctrl->switchtype) {
	case PRI_SWITCH_DMS100:
		switch (error->invoke_id) {
		case ROSE_DMS100_RLT_OPERATION_IND:
			dms100_operation = "RLT_OPERATION_IND";
			break;
		case ROSE_DMS100_RLT_THIRD_PARTY:
			dms100_operation = "RLT_THIRD_PARTY";
			break;
		default:
			dms100_operation = NULL;
			break;
		}
		if (dms100_operation) {
			pri_error(ctrl, "\tOPERATION: %s\n", dms100_operation);
			break;
		}
		/* fall through */
	default:
		pri_error(ctrl, "\tINVOKE ID: %d\n", error->invoke_id);
		break;
	}
	pri_error(ctrl, "\tERROR: %s\n", rose_error2str(error->code));
}

/*!
 * \brief Handle the ROSE result message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which the message came.
 * \param ie Raw ie contents.
 * \param header Decoded facility header before ROSE.
 * \param result Decoded ROSE result message contents.
 *
 * \return Nothing
 */
void rose_handle_result(struct pri *ctrl, q931_call *call, q931_ie *ie,
	const struct fac_extension_header *header, const struct rose_msg_result *result)
{
	switch (ctrl->switchtype) {
	case PRI_SWITCH_DMS100:
		switch (result->invoke_id) {
		case ROSE_DMS100_RLT_OPERATION_IND:
			if (result->operation != ROSE_DMS100_RLT_OperationInd) {
				pri_message(ctrl, "Invalid Operation value in return result! %s\n",
					rose_operation2str(result->operation));
				break;
			}

			/* We have enough data to transfer the call */
			call->rlt_call_id = result->args.dms100.RLT_OperationInd.call_id;
			call->transferable = 1;
			break;
		case ROSE_DMS100_RLT_THIRD_PARTY:
			if (ctrl->debug & PRI_DEBUG_APDU) {
				pri_message(ctrl, "Successfully completed RLT transfer!\n");
			}
			break;
		default:
			pri_message(ctrl, "Could not parse invoke of type %d!\n", result->invoke_id);
			break;
		}
		return;
	default:
		break;
	}

	switch (result->operation) {
#if 0	/* Not handled yet */
	case ROSE_None:
		/*
		 * This is simply a positive ACK to the invoke request.
		 * The invoke ID must be used to distinguish between outstanding
		 * invoke requests.
		 */
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_ETSI_ActivationDiversion:
		break;
	case ROSE_ETSI_DeactivationDiversion:
		break;
	case ROSE_ETSI_InterrogationDiversion:
		break;
	case ROSE_ETSI_CallDeflection:
		break;
	case ROSE_ETSI_CallRerouting:
		break;
	case ROSE_ETSI_InterrogateServedUserNumbers:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_ETSI_ChargingRequest:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_ETSI_EctExecute:
		break;
	case ROSE_ETSI_ExplicitEctExecute:
		break;
	case ROSE_ETSI_EctLinkIdRequest:
		break;
	case ROSE_ETSI_EctLoopTest:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_QSIG_ChargeRequest:
		break;
	case ROSE_QSIG_AocComplete:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_QSIG_CallTransferIdentify:
		break;
	case ROSE_QSIG_CallTransferInitiate:
		break;
	case ROSE_QSIG_CallTransferSetup:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_QSIG_ActivateDiversionQ:
		break;
	case ROSE_QSIG_DeactivateDiversionQ:
		break;
	case ROSE_QSIG_InterrogateDiversionQ:
		break;
	case ROSE_QSIG_CheckRestriction:
		break;
#endif	/* Not handled yet */
	case ROSE_QSIG_CallRerouting:
		if (ctrl->debug & PRI_DEBUG_APDU) {
			pri_message(ctrl, "Successfully completed QSIG CF callRerouting!\n");
		}
		break;
#if 0	/* Not handled yet */
	case ROSE_QSIG_MWIActivate:
		break;
	case ROSE_QSIG_MWIDeactivate:
		break;
	case ROSE_QSIG_MWIInterrogate:
		break;
#endif	/* Not handled yet */
	default:
		if (ctrl->debug & PRI_DEBUG_APDU) {
			pri_message(ctrl, "!! ROSE result operation not handled! %s\n",
				rose_operation2str(result->operation));
		}
		break;
	}
}

/*!
 * \brief Handle the ROSE invoke message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Call leg from which the message came.
 * \param ie Raw ie contents.
 * \param header Decoded facility header before ROSE.
 * \param invoke Decoded ROSE invoke message contents.
 *
 * \return Nothing
 */
void rose_handle_invoke(struct pri *ctrl, q931_call *call, q931_ie *ie,
	const struct fac_extension_header *header, const struct rose_msg_invoke *invoke)
{
	struct pri_subcommand *subcmd;
	struct q931_party_id party_id;

	switch (invoke->operation) {
#if 0	/* Not handled yet */
	case ROSE_ETSI_ActivationDiversion:
		break;
	case ROSE_ETSI_DeactivationDiversion:
		break;
	case ROSE_ETSI_ActivationStatusNotificationDiv:
		break;
	case ROSE_ETSI_DeactivationStatusNotificationDiv:
		break;
	case ROSE_ETSI_InterrogationDiversion:
		break;
	case ROSE_ETSI_DiversionInformation:
		break;
	case ROSE_ETSI_CallDeflection:
		break;
	case ROSE_ETSI_CallRerouting:
		break;
	case ROSE_ETSI_InterrogateServedUserNumbers:
		break;
#endif	/* Not handled yet */
	case ROSE_ETSI_DivertingLegInformation1:
		/* divertedToNumber is put in redirecting.to.number */
		switch (invoke->args.etsi.DivertingLegInformation1.subscription_option) {
		default:
		case 0:	/* noNotification */
		case 1:	/* notificationWithoutDivertedToNr */
			q931_party_number_init(&call->redirecting.to.number);
			call->redirecting.to.number.valid = 1;
			call->redirecting.to.number.presentation =
				PRI_PRES_RESTRICTED | PRI_PRES_USER_NUMBER_UNSCREENED;
			break;
		case 2:	/* notificationWithDivertedToNr */
			if (invoke->args.etsi.DivertingLegInformation1.diverted_to_present) {
				rose_copy_presented_number_unscreened_to_q931(ctrl,
					&call->redirecting.to.number,
					&invoke->args.etsi.DivertingLegInformation1.diverted_to);
			} else {
				q931_party_number_init(&call->redirecting.to.number);
				call->redirecting.to.number.valid = 1;
			}
			break;
		}

		call->redirecting.reason = redirectingreason_for_q931(ctrl,
			invoke->args.etsi.DivertingLegInformation1.diversion_reason);
		if (call->redirecting.count < PRI_MAX_REDIRECTS) {
			++call->redirecting.count;
		}
		call->redirecting.state = Q931_REDIRECTING_STATE_EXPECTING_RX_DIV_LEG_3;
		break;
	case ROSE_ETSI_DivertingLegInformation2:
		call->redirecting.state = Q931_REDIRECTING_STATE_PENDING_TX_DIV_LEG_3;
		call->redirecting.count =
			invoke->args.etsi.DivertingLegInformation2.diversion_counter;
		if (!call->redirecting.count) {
			/* To be safe, make sure that the count is non-zero. */
			call->redirecting.count = 1;
		}
		call->redirecting.reason = redirectingreason_for_q931(ctrl,
			invoke->args.etsi.DivertingLegInformation2.diversion_reason);

		/* divertingNr is put in redirecting.from.number */
		if (invoke->args.etsi.DivertingLegInformation2.diverting_present) {
			rose_copy_presented_number_unscreened_to_q931(ctrl,
				&call->redirecting.from.number,
				&invoke->args.etsi.DivertingLegInformation2.diverting);
		} else {
			q931_party_number_init(&call->redirecting.from.number);
			call->redirecting.from.number.valid = 1;
		}

		call->redirecting.orig_reason = PRI_REDIR_UNKNOWN;

		/* originalCalledNr is put in redirecting.orig_called.number */
		if (invoke->args.etsi.DivertingLegInformation2.original_called_present) {
			rose_copy_presented_number_unscreened_to_q931(ctrl,
				&call->redirecting.orig_called.number,
				&invoke->args.etsi.DivertingLegInformation2.original_called);
		} else {
			q931_party_number_init(&call->redirecting.orig_called.number);
		}
		break;
	case ROSE_ETSI_DivertingLegInformation3:
		if (!invoke->args.etsi.DivertingLegInformation3.presentation_allowed_indicator) {
			call->redirecting.to.number.presentation =
				PRI_PRES_RESTRICTED | PRI_PRES_USER_NUMBER_UNSCREENED;
		}

		switch (call->redirecting.state) {
		case Q931_REDIRECTING_STATE_EXPECTING_RX_DIV_LEG_3:
			call->redirecting.state = Q931_REDIRECTING_STATE_IDLE;
			subcmd = q931_alloc_subcommand(ctrl);
			if (!subcmd) {
				pri_error(ctrl, "ERROR: Too many facility subcommands\n");
				break;
			}
			/* Setup redirecting subcommand */
			subcmd->cmd = PRI_SUBCMD_REDIRECTING;
			q931_party_redirecting_copy_to_pri(&subcmd->u.redirecting,
				&call->redirecting);
			break;
		default:
			break;
		}
		break;
	case ROSE_ETSI_ChargingRequest:
		/* Ignore messsage */
		break;
#if 0	/* Not handled yet */
	case ROSE_ETSI_AOCSCurrency:
		break;
	case ROSE_ETSI_AOCSSpecialArr:
		break;
	case ROSE_ETSI_AOCDCurrency:
		break;
	case ROSE_ETSI_AOCDChargingUnit:
		break;
	case ROSE_ETSI_AOCECurrency:
		break;
#endif	/* Not handled yet */
	case ROSE_ETSI_AOCEChargingUnit:
		call->aoc_units = 0;
		if (invoke->args.etsi.AOCEChargingUnit.type == 1
			&& !invoke->args.etsi.AOCEChargingUnit.charging_unit.free_of_charge) {
			unsigned index;

			for (index =
				invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.recorded.
				num_records; index--;) {
				if (!invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.recorded.
					list[index].not_available) {
					call->aoc_units +=
						invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.
						recorded.list[index].number_of_units;
				}
			}
		}
		/* the following function is currently not used - just to make the compiler happy */
		if (0) {
			/* use this function to forward the aoc-e on a bridged channel */
			aoc_aoce_charging_unit_encode(ctrl, call, call->aoc_units);
		}
		break;
#if 0	/* Not handled yet */
	case ROSE_ITU_IdentificationOfCharge:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_ETSI_EctExecute:
		break;
	case ROSE_ETSI_ExplicitEctExecute:
		break;
#endif	/* Not handled yet */
	case ROSE_ETSI_RequestSubaddress:
		/* Ignore since we are not handling subaddresses yet. */
		break;
#if 0	/* Not handled yet */
	case ROSE_ETSI_SubaddressTransfer:
		break;
	case ROSE_ETSI_EctLinkIdRequest:
		break;
#endif	/* Not handled yet */
	case ROSE_ETSI_EctInform:
		/* redirectionNumber is put in remote_id.number */
		if (invoke->args.etsi.EctInform.redirection_present) {
			rose_copy_presented_number_unscreened_to_q931(ctrl,
				&call->remote_id.number, &invoke->args.etsi.EctInform.redirection);
		}
		if (!invoke->args.etsi.EctInform.status) {
			/* The remote party for the transfer has not answered yet. */
			call->incoming_ct_state = INCOMING_CT_STATE_EXPECT_CT_ACTIVE;
		} else {
			call->incoming_ct_state = INCOMING_CT_STATE_POST_CONNECTED_LINE;
		}
		break;
#if 0	/* Not handled yet */
	case ROSE_ETSI_EctLoopTest:
		break;
#endif	/* Not handled yet */
	case ROSE_QSIG_CallingName:
		/* CallingName is put in remote_id.name */
		rose_copy_name_to_q931(ctrl, &call->remote_id.name,
			&invoke->args.qsig.CallingName.name);
		break;
	case ROSE_QSIG_CalledName:
		/* CalledName is put in remote_id.name */
		rose_copy_name_to_q931(ctrl, &call->remote_id.name,
			&invoke->args.qsig.CalledName.name);

		/* Setup connected line subcommand */
		subcmd = q931_alloc_subcommand(ctrl);
		if (!subcmd) {
			pri_error(ctrl, "ERROR: Too many facility subcommands\n");
			break;
		}
		subcmd->cmd = PRI_SUBCMD_CONNECTED_LINE;
		q931_party_id_copy_to_pri(&subcmd->u.connected_line.id, &call->remote_id);
		break;
	case ROSE_QSIG_ConnectedName:
		/* ConnectedName is put in remote_id.name */
		rose_copy_name_to_q931(ctrl, &call->remote_id.name,
			&invoke->args.qsig.ConnectedName.name);
		break;
#if 0	/* Not handled yet */
	case ROSE_QSIG_BusyName:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_QSIG_ChargeRequest:
		break;
	case ROSE_QSIG_GetFinalCharge:
		break;
	case ROSE_QSIG_AocFinal:
		break;
	case ROSE_QSIG_AocInterim:
		break;
	case ROSE_QSIG_AocRate:
		break;
	case ROSE_QSIG_AocComplete:
		break;
	case ROSE_QSIG_AocDivChargeReq:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_QSIG_CallTransferIdentify:
		break;
	case ROSE_QSIG_CallTransferAbandon:
		break;
	case ROSE_QSIG_CallTransferInitiate:
		break;
	case ROSE_QSIG_CallTransferSetup:
		break;
#endif	/* Not handled yet */
	case ROSE_QSIG_CallTransferActive:
		call->incoming_ct_state = INCOMING_CT_STATE_POST_CONNECTED_LINE;

		/* connectedAddress is put in remote_id.number */
		rose_copy_presented_address_screened_to_q931(ctrl, &call->remote_id.number,
			&invoke->args.qsig.CallTransferActive.connected);

		/* connectedName is put in remote_id.name */
		if (invoke->args.qsig.CallTransferActive.connected_name_present) {
			rose_copy_name_to_q931(ctrl, &call->remote_id.name,
				&invoke->args.qsig.CallTransferActive.connected_name);
		}
		break;
	case ROSE_QSIG_CallTransferComplete:
		/* redirectionNumber is put in remote_id.number */
		rose_copy_presented_number_screened_to_q931(ctrl, &call->remote_id.number,
			&invoke->args.qsig.CallTransferComplete.redirection);

		/* redirectionName is put in remote_id.name */
		if (invoke->args.qsig.CallTransferComplete.redirection_name_present) {
			rose_copy_name_to_q931(ctrl, &call->remote_id.name,
				&invoke->args.qsig.CallTransferComplete.redirection_name);
		}

		if (invoke->args.qsig.CallTransferComplete.call_status == 1) {
			/* The remote party for the transfer has not answered yet. */
			call->incoming_ct_state = INCOMING_CT_STATE_EXPECT_CT_ACTIVE;
		} else {
			call->incoming_ct_state = INCOMING_CT_STATE_POST_CONNECTED_LINE;
		}
		break;
	case ROSE_QSIG_CallTransferUpdate:
		party_id = call->remote_id;

		/* redirectionNumber is put in party_id.number */
		rose_copy_presented_number_screened_to_q931(ctrl, &party_id.number,
			&invoke->args.qsig.CallTransferUpdate.redirection);

		/* redirectionName is put in party_id.name */
		if (invoke->args.qsig.CallTransferUpdate.redirection_name_present) {
			rose_copy_name_to_q931(ctrl, &party_id.name,
				&invoke->args.qsig.CallTransferUpdate.redirection_name);
		}

		if (q931_party_id_cmp(&party_id, &call->remote_id)) {
			/* The remote_id data has changed. */
			call->remote_id = party_id;
			switch (call->incoming_ct_state) {
			case INCOMING_CT_STATE_IDLE:
				call->incoming_ct_state = INCOMING_CT_STATE_POST_CONNECTED_LINE;
				break;
			default:
				break;
			}
		}
		break;
#if 0	/* Not handled yet */
	case ROSE_QSIG_SubaddressTransfer:
		break;
#endif	/* Not handled yet */
	case ROSE_QSIG_PathReplacement:
		anfpr_pathreplacement_respond(ctrl, call, ie);
		break;
#if 0	/* Not handled yet */
	case ROSE_QSIG_ActivateDiversionQ:
		break;
	case ROSE_QSIG_DeactivateDiversionQ:
		break;
	case ROSE_QSIG_InterrogateDiversionQ:
		break;
	case ROSE_QSIG_CheckRestriction:
		break;
	case ROSE_QSIG_CallRerouting:
		break;
#endif	/* Not handled yet */
	case ROSE_QSIG_DivertingLegInformation1:
		/* nominatedNr is put in redirecting.to.number */
		switch (invoke->args.qsig.DivertingLegInformation1.subscription_option) {
		default:
		case QSIG_NO_NOTIFICATION:
		case QSIG_NOTIFICATION_WITHOUT_DIVERTED_TO_NR:
			q931_party_number_init(&call->redirecting.to.number);
			call->redirecting.to.number.valid = 1;
			call->redirecting.to.number.presentation =
				PRI_PRES_RESTRICTED | PRI_PRES_USER_NUMBER_UNSCREENED;
			break;
		case QSIG_NOTIFICATION_WITH_DIVERTED_TO_NR:
			q931_party_number_init(&call->redirecting.to.number);
			call->redirecting.to.number.valid = 1;
			rose_copy_number_to_q931(ctrl, &call->redirecting.to.number,
				&invoke->args.qsig.DivertingLegInformation1.nominated_number);
			if (call->redirecting.to.number.str[0]) {
				call->redirecting.to.number.presentation =
					PRI_PRES_ALLOWED | PRI_PRES_USER_NUMBER_UNSCREENED;
			}
			break;
		}

		call->redirecting.reason = redirectingreason_for_q931(ctrl,
			invoke->args.qsig.DivertingLegInformation1.diversion_reason);
		if (call->redirecting.count < PRI_MAX_REDIRECTS) {
			++call->redirecting.count;
		}
		call->redirecting.state = Q931_REDIRECTING_STATE_EXPECTING_RX_DIV_LEG_3;
		break;
	case ROSE_QSIG_DivertingLegInformation2:
		call->redirecting.state = Q931_REDIRECTING_STATE_PENDING_TX_DIV_LEG_3;
		call->redirecting.count =
			invoke->args.qsig.DivertingLegInformation2.diversion_counter;
		if (!call->redirecting.count) {
			/* To be safe, make sure that the count is non-zero. */
			call->redirecting.count = 1;
		}
		call->redirecting.reason = redirectingreason_for_q931(ctrl,
			invoke->args.qsig.DivertingLegInformation2.diversion_reason);

		/* divertingNr is put in redirecting.from.number */
		if (invoke->args.qsig.DivertingLegInformation2.diverting_present) {
			rose_copy_presented_number_unscreened_to_q931(ctrl,
				&call->redirecting.from.number,
				&invoke->args.qsig.DivertingLegInformation2.diverting);
		} else {
			q931_party_number_init(&call->redirecting.from.number);
			call->redirecting.from.number.valid = 1;
		}

		/* redirectingName is put in redirecting.from.name */
		if (invoke->args.qsig.DivertingLegInformation2.redirecting_name_present) {
			rose_copy_name_to_q931(ctrl, &call->redirecting.from.name,
				&invoke->args.qsig.DivertingLegInformation2.redirecting_name);
		} else {
			q931_party_name_init(&call->redirecting.from.name);
		}

		call->redirecting.orig_reason = PRI_REDIR_UNKNOWN;
		if (invoke->args.qsig.DivertingLegInformation2.original_diversion_reason_present) {
			call->redirecting.orig_reason = redirectingreason_for_q931(ctrl,
				invoke->args.qsig.DivertingLegInformation2.original_diversion_reason);
		}

		/* originalCalledNr is put in redirecting.orig_called.number */
		if (invoke->args.qsig.DivertingLegInformation2.original_called_present) {
			rose_copy_presented_number_unscreened_to_q931(ctrl,
				&call->redirecting.orig_called.number,
				&invoke->args.qsig.DivertingLegInformation2.original_called);
		} else {
			q931_party_number_init(&call->redirecting.orig_called.number);
		}

		/* originalCalledName is put in redirecting.orig_called.name */
		if (invoke->args.qsig.DivertingLegInformation2.original_called_name_present) {
			rose_copy_name_to_q931(ctrl, &call->redirecting.orig_called.name,
				&invoke->args.qsig.DivertingLegInformation2.original_called_name);
		} else {
			q931_party_name_init(&call->redirecting.orig_called.name);
		}
		break;
	case ROSE_QSIG_DivertingLegInformation3:
		if (!invoke->args.qsig.DivertingLegInformation3.presentation_allowed_indicator) {
			call->redirecting.to.number.presentation =
				PRI_PRES_RESTRICTED | PRI_PRES_USER_NUMBER_UNSCREENED;
		}

		/* redirectionName is put in redirecting.to.name */
		if (invoke->args.qsig.DivertingLegInformation3.redirection_name_present) {
			rose_copy_name_to_q931(ctrl, &call->redirecting.to.name,
				&invoke->args.qsig.DivertingLegInformation3.redirection_name);
			if (!invoke->args.qsig.DivertingLegInformation3.presentation_allowed_indicator) {
				call->redirecting.to.name.presentation = PRI_PRES_RESTRICTED;
			}
		} else {
			q931_party_name_init(&call->redirecting.to.name);
		}

		switch (call->redirecting.state) {
		case Q931_REDIRECTING_STATE_EXPECTING_RX_DIV_LEG_3:
			call->redirecting.state = Q931_REDIRECTING_STATE_IDLE;
			subcmd = q931_alloc_subcommand(ctrl);
			if (!subcmd) {
				pri_error(ctrl, "ERROR: Too many facility subcommands\n");
				break;
			}
			/* Setup redirecting subcommand */
			subcmd->cmd = PRI_SUBCMD_REDIRECTING;
			q931_party_redirecting_copy_to_pri(&subcmd->u.redirecting,
				&call->redirecting);
			break;
		default:
			break;
		}
		break;
#if 0	/* Not handled yet */
	case ROSE_QSIG_CfnrDivertedLegFailed:
		break;
#endif	/* Not handled yet */
#if 0	/* Not handled yet */
	case ROSE_QSIG_MWIActivate:
		break;
	case ROSE_QSIG_MWIDeactivate:
		break;
	case ROSE_QSIG_MWIInterrogate:
		break;
#endif	/* Not handled yet */
	default:
		if (ctrl->debug & PRI_DEBUG_APDU) {
			pri_message(ctrl, "!! ROSE invoke operation not handled! %s\n",
				rose_operation2str(invoke->operation));
		}
		break;
	}
}
