/* 
   Routines for dealing with facility messages and their respective
   components (ROSE)

   by Matthew Fredrickson <creslin@digium.com>
   Copyright (C) 2004 Digium, Inc
*/

#include "compat.h"
#include "libpri.h"
#include "pri_internal.h"
#include "pri_q921.h"
#include "pri_q931.h"
#include "pri_facility.h"

#include <stdlib.h>
#include <string.h>

int rose_invoke_decode(q931_call *call, unsigned char *data, int len)
{
	int i = 0;
	unsigned char *vdata = data;
	struct rose_component *comp = NULL;

	if (len <= 0) return -1;

	/* Invoke ID stuff */
	if (&vdata[i])
		comp = (struct rose_component*)&vdata[i];
	else return -1;

	if (i+1 >= len) return -1;
	if (comp->type && comp->type != ASN1_INTEGER) {
		pri_message("Don't know what to do if first ROSE component is of type 0x%x\n",comp->type);
		return -1;
	}
	i += comp->len + 2;

	/* Operation Tag */
	if (&vdata[i])
		comp = (struct rose_component*)&vdata[i];
	else return -1;

	if (i+1 >= len) return -1;
	if (comp->type && comp->type != ASN1_INTEGER) {
		pri_message("Don't know what to do if second ROSE component is of type 0x%x\n",comp->type);
		return -1;
	}
	i += comp->len + 2;

	if (i >= len) 
		return -1;

	/* Arguement Tag */
	if (&vdata[i])
		comp = (struct rose_component*)&vdata[i];
	else return -1;

	if (comp->type) {
		switch (comp->type) {
			case ROSE_NAME_PRESENTATION_ALLOWED_SIMPLE:
				memcpy(call->callername, comp->data, comp->len);
				call->callername[comp->len] = 0;
				return 0;
			default:
				pri_message("Do not handle argument of type 0x%X\n", comp->type);
				return -1;
		}
	} else return -1;
}

