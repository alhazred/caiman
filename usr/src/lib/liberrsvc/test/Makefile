#
#CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

ARCH =		$(TARGET_ARCH:-%=%)

PROG =		terrsvc

include ../../Makefile.lib

OBJS =		terrsvc.o\
		test_create_err_info.o \
		test_get_errors_by_modid.o \
		test_get_errors_by_modid_inval_modid.o \
		test_get_all_errors.o \
		test_free_errors.o \
		test_free_err_info_list.o \
		test_set_err_data_int.o \
		test_set_err_data_str.o \
		test_get_error_type.o \
		test_get_errors_by_type.o \
		test_get_mod_id.o \
		test_get_err_data_int_by_type.o \
		test_get_err_data_str_by_type.o \
		test_with_args.o

SRCS =		$(OBJS:%.o=%.c)

INCLUDE =	-I. -I../ -I/usr/include/python2.6


DEPLIBS		= ../pics/$(ARCH)/liberrsvc.so.1
LDLIBS +=	-L/lib -L../pics/$(ARCH) -R ../pics/$(ARCH) -lerrsvc -lpython2.6 -Bdynamic

CPPFLAGS +=	-D_LARGEFILE64_SOURCE=1 -D_REENTRANT ${INCLUDE}
CFLAGS +=	-g -DDEBUG


$(PROG):	$(OBJS)
		$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
		$(POST_PROCESS)

all:		$(PROG)

install_h:

install:	all $(ROOTPROG)

lint:
	$(LINT.c) $(SRCS) $(LDLIBS)

clobber: clean

clean:
	$(RM) $(OBJS) $(PROG)

