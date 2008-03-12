/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)svc_global.c	1.5	07/10/09 SMI"


/*
 * Module:	svc_global.c
 * Group:	libspmisvc
 * Description:	This module contains functions which manipulate the global
 *		attribute value pairs
 */
#include "spmisvc_lib.h"
#include "spmicommon_api.h"

/* private prototypes */

static int	coreGlobalSetAttribute(GlobalAttr_t, void *, int);
static int	coreGlobalGetAttribute(GlobalAttr_t, void *, int);

/* local globals and constants */

static void *	_GlobalAttributes[(int)GLOBALOBJ_LAST] = {
		(void *)VAL_UNSPECIFIED,	/* GLOBALOBJ_UNDEFINED */
		(void *)VAL_UNSPECIFIED };	/* GLOBALOBJ_SWAP */

/* ----------------------- public functions ----------------------- */

/*
 * Function:	GlobalGetAttribute
 * Description:	Public interface to retrieve the value associated with a
 *		given global attribute.
 * Scope:	public
 * Parameters:	type	[RO] (GlobalAttr_t)
 *			Global attribute type specifier.
 *		value	[RO] (void *)
 *			Address of value specifier used to retrieve value.
 *
 *		Valid values are:
 *
 *		Attribute	  Value		Description
 *		-----------	  -------	-------------
 *		GLOBALOBJ_SWAP	  int *		explicitly specified swap in
 *						sectors
 *
 * Return:	D_OK	    global attribute data retrieved successfully
 *		D_BADARG    invalid argument specified
 */
int
GlobalGetAttribute(GlobalAttr_t type, void *value)
{
	int	status;

	/* validate parameters */
	if (value == NULL)
		return (D_BADARG);

	status = coreGlobalGetAttribute(type, value, NOPRIVILEGE);
	return (status);
}

/*
 * Function:	GlobalSetAttribute
 * Description:	Public interface to update the value associated with a
 *		given global attribute.
 * Scope:	public
 * Parameters:	type	[RO] (GlobalAttr_t)
 *			Global attribute type specifier.
 *		value	[RO] (void *)
 *			Value associated with attribute used in update.
 *
 *		Valid values are:
 *
 *		Attribute	  Value		Description
 *		-----------	  -------	-------------
 *		GLOBALOBJ_SWAP	  int 		explicitly specified swap
 *						in sectors
 *
 * Return:	D_OK	    global attribute data updated successfully
 *		D_BADARG    invalid argument specified
 */
int
GlobalSetAttribute(GlobalAttr_t type, void *value)
{
	int	status;

	status = coreGlobalSetAttribute(type, value, NOPRIVILEGE);
	return (status);
}

/* ----------------------- internal functions ----------------------- */

/*
 * Function:	GlobalSetAttributePriv
 * Description:	Privileged interface to update the value associated with a
 *		given global attribute.
 * Scope:	internal
 * Parameters:	type	[RO] (GlobalAttr_t)
 *			Global attribute type specifier.
 *		value	[RO] (void *)
 *			Value associated with attribute used in update.
 *
 *		Valid values are:
 *
 *		Attribute	  Value		Description
 *		-----------	  -------	-------------
 *		GLOBALOBJ_SWAP	  int 		explicitly specified swap
 *						in sectors
 *
 * Return:	D_OK	    global attribute data updated successfully
 *		D_BADARG    invalid argument specified
 */
int
GlobalSetAttributePriv(GlobalAttr_t type, void *value)
{
	int	status;

	status = coreGlobalSetAttribute(type, value, PRIVILEGE);
	return (status);
}

/*
 * Function:	GlobalGetAttributePriv
 * Description:	Privileged interface to retrieve the value associated with a
 *		given global attribute.
 * Scope:	internal
 * Parameters:	type	[RO] (GlobalAttr_t)
 *			Global attribute type specifier.
 *		value	[RO] (void *)
 *			Address of value specifier used to retrieve value.
 *
 *		Valid values are:
 *
 *		Attribute	  Value		Description
 *		-----------	  -------	-------------
 *		GLOBALOBJ_SWAP	  int *		explicitly specified swap in
 *						sectors
 *
 * Return:	D_OK	    global attribute data retrieved successfully
 *		D_BADARG    invalid argument specified
 */
int
GlobalGetAttributePriv(GlobalAttr_t type, void *value)
{
	int	status;

	/* validate parameters */
	if (value == NULL)
		return (D_BADARG);

	status = coreGlobalGetAttribute(type, value, PRIVILEGE);
	return (status);
}

/* ----------------------- private functions ----------------------- */

/*
 * Function:	coreGlobalSetAttribute
 * Description:	Low level function used to update the values associated
 *		with global attributes.
 * Scope:	private
 * Parameters:	type	   [RO, *RO] (GlobalAttr_t)
 *			   Global attribute specifier.
 *		value	   [RO, *RO] (void *)
 *			   Value specifier used to update.
 *		privilege  [RO] (int)
 *			   Indicate if call if privileged. Valid values
 *			   are:
 *				PRIVILEGE
 *				NOPRIVILEGE
 * Return:	D_OK	    global attribute data updated successfully
 *		D_BADARG    invalid argument specified
 */
/*ARGSUSED2*/
static int
coreGlobalSetAttribute(GlobalAttr_t type, void *value, int privilege)
{
	int	swap = VAL_UNSPECIFIED;

	switch ((int) type) {
	    case GLOBALOBJ_SWAP:
		swap = (int)value;
		if (swap < 0 && swap != VAL_UNSPECIFIED)
			return (D_BADARG);
		else
			_GlobalAttributes[GLOBALOBJ_SWAP] =
					(void *)swap;
		break;

	    default:
		return (D_BADARG);
	}

	return (D_OK);
}

/*
 * Function:	coreGlobalGetAttribute
 * Description:	Low level function used to retrieve the values associated
 *		with global attributes.
 * Scope:	private
 * Parameters:	type	   [RO, *RO] (GlobalAttr_t)
 *			   Global attribute specifier.
 *		value	   [RO, *RO] (void *)
 *			   Address of identifier to retrieve value.
 *		privilege  [RO] (int)
 *			   Indicate if call if privileged. Valid values are:
 *				PRIVILEGE	privileged call
 *				NOPRIVILEGE	unprivileged call
 * Return:	D_OK	    global attribute data retrieved successfully
 *		D_BADARG    invalid argument specified
 */
/*ARGSUSED2*/
static int
coreGlobalGetAttribute(GlobalAttr_t type, void *value, int privilege)
{
	int *	vip;
	int	status = D_OK;

	switch ((int) type) {
	    case GLOBALOBJ_SWAP:
		if (value == NULL) {
			status = D_BADARG;
		} else {
			vip = (int *)value;
			if (vip == NULL)
				status = D_BADARG;
			else
				*vip = (int)_GlobalAttributes[GLOBALOBJ_SWAP];
		}
		break;

	    default:
		status = D_BADARG;
		break;
	}

	return (status);
}