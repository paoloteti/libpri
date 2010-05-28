/*
 * libpri: An implementation of Primary Rate ISDN
 *
 * Copyright (C) 2010 Digium, Inc.
 *
 * Richard Mudgett <rmudgett@digium.com>
 *
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

/*!
 * \file
 * \brief Advice Of Charge (AOC) facility support.
 *
 * \author Richard Mudgett <rmudgett@digium.com>
 */


#include "compat.h"
#include "libpri.h"
#include "pri_internal.h"
#include "pri_facility.h"


/* ------------------------------------------------------------------- */

/*!
 * \internal
 * \brief Fill in the AOC subcmd amount from the ETSI amount.
 *
 * \param subcmd_amount AOC subcmd amount.
 * \param etsi_amount AOC ETSI amount.
 *
 * \return Nothing
 */
static void aoc_etsi_subcmd_amount(struct pri_aoc_amount *subcmd_amount, const struct roseEtsiAOCAmount *etsi_amount)
{
	subcmd_amount->cost = etsi_amount->currency;
	subcmd_amount->multiplier = etsi_amount->multiplier;
}

/*!
 * \internal
 * \brief Fill in the AOC subcmd time from the ETSI time.
 *
 * \param subcmd_time AOC subcmd time.
 * \param etsi_time AOC ETSI time.
 *
 * \return Nothing
 */
static void aoc_etsi_subcmd_time(struct pri_aoc_time *subcmd_time, const struct roseEtsiAOCTime *etsi_time)
{
	subcmd_time->length = etsi_time->length;
	subcmd_time->scale = etsi_time->scale;
}

/*!
 * \internal
 * \brief Fill in the AOC subcmd recorded currency from the ETSI recorded currency.
 *
 * \param subcmd_recorded AOC subcmd recorded currency.
 * \param etsi_recorded AOC ETSI recorded currency.
 *
 * \return Nothing
 */
static void aoc_etsi_subcmd_recorded_currency(struct pri_aoc_recorded_currency *subcmd_recorded, const struct roseEtsiAOCRecordedCurrency *etsi_recorded)
{
	aoc_etsi_subcmd_amount(&subcmd_recorded->amount, &etsi_recorded->amount);
	libpri_copy_string(subcmd_recorded->currency, (char *) etsi_recorded->currency,
		sizeof(subcmd_recorded->currency));
}

/*!
 * \internal
 * \brief Fill in the AOC subcmd recorded units from the ETSI recorded units.
 *
 * \param subcmd_recorded AOC subcmd recorded units list.
 * \param etsi_recorded AOC ETSI recorded units list.
 *
 * \return Nothing
 */
static void aoc_etsi_subcmd_recorded_units(struct pri_aoc_recorded_units *subcmd_recorded, const struct roseEtsiAOCRecordedUnitsList *etsi_recorded)
{
	int idx;

	/* Fill in the itemized list of recorded units. */
	for (idx = 0; idx < etsi_recorded->num_records
		&& idx < ARRAY_LEN(subcmd_recorded->item); ++idx) {
		if (etsi_recorded->list[idx].not_available) {
			subcmd_recorded->item[idx].number = -1;
		} else {
			subcmd_recorded->item[idx].number = etsi_recorded->list[idx].number_of_units;
		}
		if (etsi_recorded->list[idx].type_of_unit_present) {
			subcmd_recorded->item[idx].type = etsi_recorded->list[idx].type_of_unit;
		} else {
			subcmd_recorded->item[idx].type = -1;
		}
	}
	subcmd_recorded->num_items = idx;
}

/*!
 * \internal
 * \brief Fill in the AOC-S subcmd currency info list of chargeable items.
 *
 * \param aoc_s AOC-S info list of chargeable items.
 * \param info ETSI info list of chargeable items.
 *
 * \return Nothing
 */
static void aoc_etsi_subcmd_aoc_s_currency_info(struct pri_subcmd_aoc_s *aoc_s, const struct roseEtsiAOCSCurrencyInfoList *info)
{
	int idx;

	/* Fill in the itemized list of chargeable items. */
	for (idx = 0; idx < info->num_records && idx < ARRAY_LEN(aoc_s->item); ++idx) {
		/* What is being charged. */
		switch (info->list[idx].charged_item) {
		case 0:/* basicCommunication */
			aoc_s->item[idx].chargeable = PRI_AOC_CHARGED_ITEM_BASIC_COMMUNICATION;
			break;
		case 1:/* callAttempt */
			aoc_s->item[idx].chargeable = PRI_AOC_CHARGED_ITEM_CALL_ATTEMPT;
			break;
		case 2:/* callSetup */
			aoc_s->item[idx].chargeable = PRI_AOC_CHARGED_ITEM_CALL_SETUP;
			break;
		case 3:/*  userToUserInfo */
			aoc_s->item[idx].chargeable = PRI_AOC_CHARGED_ITEM_USER_USER_INFO;
			break;
		case 4:/* operationOfSupplementaryServ */
			aoc_s->item[idx].chargeable = PRI_AOC_CHARGED_ITEM_SUPPLEMENTARY_SERVICE;
			break;
		default:
			aoc_s->item[idx].chargeable = PRI_AOC_CHARGED_ITEM_NOT_AVAILABLE;
			break;
		}

		/* Rate method being used. */
		switch (info->list[idx].currency_type) {
		case 0:/* specialChargingCode */
			aoc_s->item[idx].rate_type = PRI_AOC_RATE_TYPE_SPECIAL_CODE;
			aoc_s->item[idx].rate.special = info->list[idx].u.special_charging_code;
			break;
		case 1:/* durationCurrency */
			aoc_s->item[idx].rate_type = PRI_AOC_RATE_TYPE_DURATION;
			aoc_etsi_subcmd_amount(&aoc_s->item[idx].rate.duration.amount,
				&info->list[idx].u.duration.amount);
			aoc_etsi_subcmd_time(&aoc_s->item[idx].rate.duration.time,
				&info->list[idx].u.duration.time);
			if (info->list[idx].u.duration.granularity_present) {
				aoc_etsi_subcmd_time(&aoc_s->item[idx].rate.duration.granularity,
					&info->list[idx].u.duration.granularity);
			} else {
				aoc_s->item[idx].rate.duration.granularity.length = 0;
				aoc_s->item[idx].rate.duration.granularity.scale =
					PRI_AOC_TIME_SCALE_HUNDREDTH_SECOND;
			}
			aoc_s->item[idx].rate.duration.charging_type =
				info->list[idx].u.duration.charging_type;
			libpri_copy_string(aoc_s->item[idx].rate.duration.currency,
				(char *) info->list[idx].u.duration.currency,
				sizeof(aoc_s->item[idx].rate.duration.currency));
			break;
		case 2:/* flatRateCurrency */
			aoc_s->item[idx].rate_type = PRI_AOC_RATE_TYPE_FLAT;
			aoc_etsi_subcmd_amount(&aoc_s->item[idx].rate.flat.amount,
				&info->list[idx].u.flat_rate.amount);
			libpri_copy_string(aoc_s->item[idx].rate.flat.currency,
				(char *) info->list[idx].u.flat_rate.currency,
				sizeof(aoc_s->item[idx].rate.flat.currency));
			break;
		case 3:/* volumeRateCurrency */
			aoc_s->item[idx].rate_type = PRI_AOC_RATE_TYPE_VOLUME;
			aoc_etsi_subcmd_amount(&aoc_s->item[idx].rate.volume.amount,
				&info->list[idx].u.volume_rate.amount);
			aoc_s->item[idx].rate.volume.unit = info->list[idx].u.volume_rate.unit;
			libpri_copy_string(aoc_s->item[idx].rate.volume.currency,
				(char *) info->list[idx].u.volume_rate.currency,
				sizeof(aoc_s->item[idx].rate.volume.currency));
			break;
		case 4:/* freeOfCharge */
			aoc_s->item[idx].rate_type = PRI_AOC_RATE_TYPE_FREE;
			break;
		default:
		case 5:/* currencyInfoNotAvailable */
			aoc_s->item[idx].rate_type = PRI_AOC_RATE_TYPE_NOT_AVAILABLE;
			break;
		}
	}
	aoc_s->num_items = idx;
}

/*!
 * \brief Handle the ETSI AOCSCurrency message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param invoke Decoded ROSE invoke message contents.
 *
 * \return Nothing
 */
void aoc_etsi_aoc_s_currency(struct pri *ctrl, const struct rose_msg_invoke *invoke)
{
	struct pri_subcommand *subcmd;

	if (!PRI_MASTER(ctrl)->aoc_support) {
		return;
	}
	subcmd = q931_alloc_subcommand(ctrl);
	if (!subcmd) {
		return;
	}

	subcmd->cmd = PRI_SUBCMD_AOC_S;
	if (!invoke->args.etsi.AOCSCurrency.type) {
		subcmd->u.aoc_s.num_items = 0;
		return;
	}

	/* Fill in the itemized list of chargeable items. */
	aoc_etsi_subcmd_aoc_s_currency_info(&subcmd->u.aoc_s,
		&invoke->args.etsi.AOCSCurrency.currency_info);
}

/*!
 * \brief Handle the ETSI AOCSSpecialArr message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param invoke Decoded ROSE invoke message contents.
 *
 * \return Nothing
 */
void aoc_etsi_aoc_s_special_arrangement(struct pri *ctrl, const struct rose_msg_invoke *invoke)
{
	struct pri_subcommand *subcmd;

	if (!PRI_MASTER(ctrl)->aoc_support) {
		return;
	}
	subcmd = q931_alloc_subcommand(ctrl);
	if (!subcmd) {
		return;
	}
	subcmd->cmd = PRI_SUBCMD_AOC_S;
	if (!invoke->args.etsi.AOCSSpecialArr.type) {
		subcmd->u.aoc_s.num_items = 0;
		return;
	}
	subcmd->u.aoc_s.num_items = 1;
	subcmd->u.aoc_s.item[0].chargeable = PRI_AOC_CHARGED_ITEM_SPECIAL_ARRANGEMENT;
	subcmd->u.aoc_s.item[0].rate_type = PRI_AOC_RATE_TYPE_SPECIAL_CODE;
	subcmd->u.aoc_s.item[0].rate.special =
		invoke->args.etsi.AOCSSpecialArr.special_arrangement;
}

/*!
 * \internal
 * \brief Determine the AOC-D subcmd billing_id value.
 *
 * \param billing_id_present TRUE if billing_id valid.
 * \param billing_id ETSI billing id from ROSE.
 *
 * \return enum PRI_AOC_D_BILLING_ID value
 */
static enum PRI_AOC_D_BILLING_ID aoc_etsi_subcmd_aoc_d_billing_id(int billing_id_present, int billing_id)
{
	enum PRI_AOC_D_BILLING_ID value;

	if (billing_id_present) {
		switch (billing_id) {
		case 0:/* normalCharging */
			value = PRI_AOC_D_BILLING_ID_NORMAL;
			break;
		case 1:/* reverseCharging */
			value = PRI_AOC_D_BILLING_ID_REVERSE;
			break;
		case 2:/* creditCardCharging */
			value = PRI_AOC_D_BILLING_ID_CREDIT_CARD;
			break;
		default:
			value = PRI_AOC_D_BILLING_ID_NOT_AVAILABLE;
			break;
		}
	} else {
		value = PRI_AOC_D_BILLING_ID_NOT_AVAILABLE;
	}
	return value;
}

/*!
 * \brief Handle the ETSI AOCDCurrency message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param invoke Decoded ROSE invoke message contents.
 *
 * \return Nothing
 */
void aoc_etsi_aoc_d_currency(struct pri *ctrl, const struct rose_msg_invoke *invoke)
{
	struct pri_subcommand *subcmd;

	if (!PRI_MASTER(ctrl)->aoc_support) {
		return;
	}
	subcmd = q931_alloc_subcommand(ctrl);
	if (!subcmd) {
		return;
	}

	subcmd->cmd = PRI_SUBCMD_AOC_D;
	switch (invoke->args.etsi.AOCDCurrency.type) {
	default:
	case 0:/* charge_not_available */
		subcmd->u.aoc_d.charge = PRI_AOC_DE_CHARGE_NOT_AVAILABLE;
		break;
	case 1:/* free_of_charge */
		subcmd->u.aoc_d.charge = PRI_AOC_DE_CHARGE_FREE;
		break;
	case 2:/* specific_currency */
		subcmd->u.aoc_d.charge = PRI_AOC_DE_CHARGE_CURRENCY;
		aoc_etsi_subcmd_recorded_currency(&subcmd->u.aoc_d.recorded.money,
			&invoke->args.etsi.AOCDCurrency.specific.recorded);
		subcmd->u.aoc_d.billing_accumulation =
			invoke->args.etsi.AOCDCurrency.specific.type_of_charging_info;
		subcmd->u.aoc_d.billing_id = aoc_etsi_subcmd_aoc_d_billing_id(
			invoke->args.etsi.AOCDCurrency.specific.billing_id_present,
			invoke->args.etsi.AOCDCurrency.specific.billing_id);
		break;
	}
}

/*!
 * \brief Handle the ETSI AOCDChargingUnit message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param invoke Decoded ROSE invoke message contents.
 *
 * \return Nothing
 */
void aoc_etsi_aoc_d_charging_unit(struct pri *ctrl, const struct rose_msg_invoke *invoke)
{
	struct pri_subcommand *subcmd;

	if (!PRI_MASTER(ctrl)->aoc_support) {
		return;
	}
	subcmd = q931_alloc_subcommand(ctrl);
	if (!subcmd) {
		return;
	}

	subcmd->cmd = PRI_SUBCMD_AOC_D;
	switch (invoke->args.etsi.AOCDChargingUnit.type) {
	default:
	case 0:/* charge_not_available */
		subcmd->u.aoc_d.charge = PRI_AOC_DE_CHARGE_NOT_AVAILABLE;
		break;
	case 1:/* free_of_charge */
		subcmd->u.aoc_d.charge = PRI_AOC_DE_CHARGE_FREE;
		break;
	case 2:/* specific_charging_units */
		subcmd->u.aoc_d.charge = PRI_AOC_DE_CHARGE_UNITS;
		aoc_etsi_subcmd_recorded_units(&subcmd->u.aoc_d.recorded.unit,
			&invoke->args.etsi.AOCDChargingUnit.specific.recorded);
		subcmd->u.aoc_d.billing_accumulation =
			invoke->args.etsi.AOCDChargingUnit.specific.type_of_charging_info;
		subcmd->u.aoc_d.billing_id = aoc_etsi_subcmd_aoc_d_billing_id(
			invoke->args.etsi.AOCDChargingUnit.specific.billing_id_present,
			invoke->args.etsi.AOCDChargingUnit.specific.billing_id);
		break;
	}
}

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
	if (pri_call_apdu_queue(call, Q931_FACILITY, buffer, end - buffer, NULL)
		|| q931_facility(call->pri, call)) {
		pri_message(ctrl, "Could not schedule facility message for call %d\n", call->cr);
		return -1;
	}

	return 0;
}

/*!
 * \internal
 * \brief Fill in the AOC-E subcmd charging association from the ETSI charging association.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param subcmd_association AOC-E subcmd charging association.
 * \param etsi_association AOC-E ETSI charging association.
 *
 * \return Nothing
 */
static void aoc_etsi_subcmd_aoc_e_charging_association(struct pri *ctrl, struct pri_aoc_e_charging_association *subcmd_association, const struct roseEtsiAOCChargingAssociation *etsi_association)
{
	struct q931_party_number q931_number;

	switch (etsi_association->type) {
	case 0:/* charge_identifier */
		subcmd_association->charging_type = PRI_AOC_E_CHARGING_ASSOCIATION_ID;
		subcmd_association->charge.id = etsi_association->id;
		break;
	case 1:/* charged_number */
		subcmd_association->charging_type = PRI_AOC_E_CHARGING_ASSOCIATION_NUMBER;
		q931_party_number_init(&q931_number);
		rose_copy_number_to_q931(ctrl, &q931_number, &etsi_association->number);
		q931_party_number_copy_to_pri(&subcmd_association->charge.number, &q931_number);
		break;
	default:
		subcmd_association->charging_type = PRI_AOC_E_CHARGING_ASSOCIATION_NOT_AVAILABLE;
		break;
	}
}

/*!
 * \internal
 * \brief Determine the AOC-E subcmd billing_id value.
 *
 * \param billing_id_present TRUE if billing_id valid.
 * \param billing_id ETSI billing id from ROSE.
 *
 * \return enum PRI_AOC_E_BILLING_ID value
 */
static enum PRI_AOC_E_BILLING_ID aoc_etsi_subcmd_aoc_e_billing_id(int billing_id_present, int billing_id)
{
	enum PRI_AOC_E_BILLING_ID value;

	if (billing_id_present) {
		switch (billing_id) {
		case 0:/* normalCharging */
			value = PRI_AOC_E_BILLING_ID_NORMAL;
			break;
		case 1:/* reverseCharging */
			value = PRI_AOC_E_BILLING_ID_REVERSE;
			break;
		case 2:/* creditCardCharging */
			value = PRI_AOC_E_BILLING_ID_CREDIT_CARD;
			break;
		case 3:/* callForwardingUnconditional */
			value = PRI_AOC_E_BILLING_ID_CALL_FORWARDING_UNCONDITIONAL;
			break;
		case 4:/* callForwardingBusy */
			value = PRI_AOC_E_BILLING_ID_CALL_FORWARDING_BUSY;
			break;
		case 5:/* callForwardingNoReply */
			value = PRI_AOC_E_BILLING_ID_CALL_FORWARDING_NO_REPLY;
			break;
		case 6:/* callDeflection */
			value = PRI_AOC_E_BILLING_ID_CALL_DEFLECTION;
			break;
		case 7:/* callTransfer */
			value = PRI_AOC_E_BILLING_ID_CALL_TRANSFER;
			break;
		default:
			value = PRI_AOC_E_BILLING_ID_NOT_AVAILABLE;
			break;
		}
	} else {
		value = PRI_AOC_E_BILLING_ID_NOT_AVAILABLE;
	}
	return value;
}

/*!
 * \brief Handle the ETSI AOCECurrency message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Q.931 call leg.
 * \param invoke Decoded ROSE invoke message contents.
 *
 * \return Nothing
 */
void aoc_etsi_aoc_e_currency(struct pri *ctrl, q931_call *call, const struct rose_msg_invoke *invoke)
{
	struct pri_subcommand *subcmd;

	if (!PRI_MASTER(ctrl)->aoc_support) {
		return;
	}
	subcmd = q931_alloc_subcommand(ctrl);
	if (!subcmd) {
		return;
	}

	subcmd->cmd = PRI_SUBCMD_AOC_E;
	subcmd->u.aoc_e.associated.charging_type =
		PRI_AOC_E_CHARGING_ASSOCIATION_NOT_AVAILABLE;

	if (!invoke->args.etsi.AOCECurrency.type) {
		subcmd->u.aoc_e.charge = PRI_AOC_DE_CHARGE_NOT_AVAILABLE;
		return;
	}

	/* Fill in charging association if present. */
	if (invoke->args.etsi.AOCECurrency.currency_info.charging_association_present) {
		aoc_etsi_subcmd_aoc_e_charging_association(ctrl, &subcmd->u.aoc_e.associated,
			&invoke->args.etsi.AOCECurrency.currency_info.charging_association);
	}

	/* Call was free of charge. */
	if (invoke->args.etsi.AOCECurrency.currency_info.free_of_charge) {
		subcmd->u.aoc_e.charge = PRI_AOC_DE_CHARGE_FREE;
		return;
	}

	/* Fill in currency cost of call. */
	subcmd->u.aoc_e.charge = PRI_AOC_DE_CHARGE_CURRENCY;
	aoc_etsi_subcmd_recorded_currency(&subcmd->u.aoc_e.recorded.money,
		&invoke->args.etsi.AOCECurrency.currency_info.specific.recorded);
	subcmd->u.aoc_e.billing_id = aoc_etsi_subcmd_aoc_e_billing_id(
		invoke->args.etsi.AOCECurrency.currency_info.specific.billing_id_present,
		invoke->args.etsi.AOCECurrency.currency_info.specific.billing_id);
}

/*!
 * \brief Handle the ETSI AOCEChargingUnit message.
 *
 * \param ctrl D channel controller for diagnostic messages or global options.
 * \param call Q.931 call leg.
 * \param invoke Decoded ROSE invoke message contents.
 *
 * \return Nothing
 */
void aoc_etsi_aoc_e_charging_unit(struct pri *ctrl, q931_call *call, const struct rose_msg_invoke *invoke)
{
	struct pri_subcommand *subcmd;
	unsigned idx;

	/* Fill in legacy stuff. */
	call->aoc_units = 0;
	if (invoke->args.etsi.AOCEChargingUnit.type == 1
		&& !invoke->args.etsi.AOCEChargingUnit.charging_unit.free_of_charge) {
		for (idx =
			invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.recorded.num_records;
			idx--;) {
			if (!invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.recorded.
				list[idx].not_available) {
				call->aoc_units +=
					invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.
					recorded.list[idx].number_of_units;
			}
		}
	}
	/* the following function is currently not used - just to make the compiler happy */
	if (0) {
		/* use this function to forward the aoc-e on a bridged channel */
		aoc_aoce_charging_unit_encode(ctrl, call, call->aoc_units);
	}

	if (!PRI_MASTER(ctrl)->aoc_support) {
		return;
	}
	subcmd = q931_alloc_subcommand(ctrl);
	if (!subcmd) {
		return;
	}

	subcmd->cmd = PRI_SUBCMD_AOC_E;
	subcmd->u.aoc_e.associated.charging_type =
		PRI_AOC_E_CHARGING_ASSOCIATION_NOT_AVAILABLE;

	if (!invoke->args.etsi.AOCEChargingUnit.type) {
		subcmd->u.aoc_e.charge = PRI_AOC_DE_CHARGE_NOT_AVAILABLE;
		return;
	}

	/* Fill in charging association if present. */
	if (invoke->args.etsi.AOCEChargingUnit.charging_unit.charging_association_present) {
		aoc_etsi_subcmd_aoc_e_charging_association(ctrl, &subcmd->u.aoc_e.associated,
			&invoke->args.etsi.AOCEChargingUnit.charging_unit.charging_association);
	}

	/* Call was free of charge. */
	if (invoke->args.etsi.AOCEChargingUnit.charging_unit.free_of_charge) {
		subcmd->u.aoc_e.charge = PRI_AOC_DE_CHARGE_FREE;
		return;
	}

	/* Fill in unit cost of call. */
	subcmd->u.aoc_e.charge = PRI_AOC_DE_CHARGE_UNITS;
	aoc_etsi_subcmd_recorded_units(&subcmd->u.aoc_e.recorded.unit,
		&invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.recorded);
	subcmd->u.aoc_e.billing_id = aoc_etsi_subcmd_aoc_e_billing_id(
		invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.billing_id_present,
		invoke->args.etsi.AOCEChargingUnit.charging_unit.specific.billing_id);
}

void pri_aoc_events_enable(struct pri *ctrl, int enable)
{
	if (ctrl) {
		ctrl = PRI_MASTER(ctrl);
		ctrl->aoc_support = enable ? 1 : 0;
	}
}

/* ------------------------------------------------------------------- */
/* end pri_aoc.c */
