/*
   This file contains all data structures and definitions associated
   with facility message usage and the ROSE components included
   within those messages.

   by Matthew Fredrickson <creslin@digium.com>
   Copyright (C) Digium, Inc. 2004
*/

#ifndef _PRI_FACILITY_H
#define _PRI_FACILITY_H

/* Protocol Profile field */
#define ROSE_NETWORK_EXTENSION 0x9F

/* Argument values */
#define ROSE_NAME_PRESENTATION_ALLOWED_SIMPLE 0x80
#define ROSE_NAME_PRESENTATION_RESTRICTED_NULL 0x87
#define ROSE_NAME_NOT_AVAIL 0x84

/* Divert arguments */
#define ROSE_DIVERTING_LEG_INFORMATION2 0x15

/* Component types */
#define COMP_TYPE_INVOKE 0xA1
#define COMP_TYPE_INTERPRETATION 0x8B
#define COMP_TYPE_NETWORK_PROTOCOL_PROFILE 0x92
#define COMP_TYPE_RETURN_RESULT 0xA2
#define COMP_TYPE_RETURN_ERROR 0xA3
#define COMP_TYPE_REJECT 0xA4
#define COMP_TYPE_NFE 0xAA


/* ROSE definitions and data structures */
#define INVOKE_IDENTIFIER 0x02
#define INVOKE_LINKED_IDENTIFIER 0x80
#define INVOKE_NULL_IDENTIFIER __USE_ASN1_NULL

/* ASN.1 Data types */
#define ASN1_BOOLEAN 0x01
#define ASN1_INTEGER 0x02
#define ASN1_BITSTRING 0x03
#define ASN1_OCTETSTRING 0x04
#define ASN1_NULL 0x05
#define ASN1_OBJECTIDENTIFIER 0x06
#define ASN1_OBJECTDESCRIPTER 0x07
#define ASN1_UTF8STRING 0x0c
#define ASN1_SEQUENCE 0x10
#define ASN1_SET 0x11
#define ASN1_NUMERICSTRING 0x12
#define ASN1_PRINTABLESTRING 0x13
#define ASN1_TELETEXSTRING 0x14
#define ASN1_IA5STRING 0x16
#define ASN1_UTCTIME 0x17
#define ASN1_GENERALIZEDTIME 0x18


#define INVOKE_OPERATION_INT __USE_ASN1_INTEGER
#define INVOKE_OBJECT_ID __USE_ASN1_OBJECTIDENTIFIER

/* Divert cause */
#define DIVERT_REASON_UNKNOWN 0x00
#define DIVERT_REASON_CFU 0x01
#define DIVERT_REASON_CFB 0x02
#define DIVERT_REASON_CFNR 0x03

struct rose_component {
        u_int8_t type;
        u_int8_t len;
        u_int8_t data[0];
};


/* Decoder fo the invoke part of a ROSE request
   It currently only support calling name decode */
extern int rose_invoke_decode(struct q931_call *call, unsigned char *data, int len);

#endif /* _PRI_FACILITY_H */
