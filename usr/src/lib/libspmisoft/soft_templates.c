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

#pragma ident	"@(#)soft_templates.c	1.4	07/11/12 SMI"


char *platgrp_softinfo[] = {
"PLATFORM_GROUP=@ISA@:@PLATGRP@",
"",
"DryRun",
"PLATFORM_GROUP=@ISA@:@PLATGRP@",
""
};


char *platmember_softinfo[] = {
"PLATFORM_MEMBER=@PLAT@",
"",
"DryRun",
"PLATFORM_MEMBER=@PLAT@",
""
};

char *end_platform_file[] = {
"EOF",
"	logprogress @SEQ@ none",
"fi",
"",
"DryRun",
"EOF",
""
};

char *start_platform[] = {
"if [ @SEQ@ -gt $resumecnt ] ; then",
"rm -f ${base}@ROOT@/var/sadm/system/admin/.platform",
"touch ${base}@ROOT@/var/sadm/system/admin/.platform",
"chmod 644 ${base}@ROOT@/var/sadm/system/admin/.platform",
"cat >> ${base}@ROOT@/var/sadm/system/admin/.platform << EOF",
"",
"DryRun",
"cat >> ${base}@ROOT@/var/sadm/system/admin/.platform << EOF",
""
};

char *generic[] = {
"@LINE@",
"",
"DryRun",
"@LINE@",
""
};