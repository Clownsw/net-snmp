#ifndef _net_snmp_transform_oids_h
#define _net_snmp_transform_oids_h
/*
 * transform_oids.h
 *
 * Numeric MIB names for auth and priv transforms.
 */

const oid usmNoAuthProtocol[10];	/* == { 1,3,6,1,6,3,10,1,1,1 }; */
const oid usmHMACMD5AuthProtocol[10];	/* == { 1,3,6,1,6,3,10,1,1,2 }; */
const oid usmHMACSHA1AuthProtocol[10]; 	/* == { 1,3,6,1,6,3,10,1,1,3 }; */
const oid usmNoPrivProtocol[10];	/* == { 1,3,6,1,6,3,10,1,2,1 }; */
const oid usmDESPrivProtocol[10];	/* == { 1,3,6,1,6,3,10,1,2,2 }; */

#endif
