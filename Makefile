#
# libpri: An implementation of Primary Rate ISDN
#
# Written by Mark Spencer <markster@linux-support.net>
#
# Copyright (C) 2001, Linux Support Services, Inc.
# All Rights Reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
#
#
# Uncomment if you want libpri not send PROGRESS_INDICATOR w/ALERTING
#ALERTING=-DALERTING_NO_PROGRESS

# Uncomment if you want libpri to count number of Q921/Q931 sent/received
#LIBPRI_COUNTERS=-DLIBPRI_COUNTERS

OSARCH=$(shell uname -s)

TOBJS=testpri.o
T2OBJS=testprilib.o
STATIC_LIBRARY=libpri.a
DYNAMIC_LIBRARY=libpri.so.1.0
STATIC_OBJS=pri.o q921.o prisched.o q931.o
DYNAMIC_OBJS=pri.lo q921.lo prisched.lo q931.lo
CFLAGS=-Wall -Werror -Wstrict-prototypes -Wmissing-prototypes -g $(ALERTING) $(LIBPRI_COUNTERS)
INSTALL_PREFIX=
ifeq (${OSARCH},Linux)
LDCONFIG_FLAGS=-n
else
ifeq (${OSARCH},FreeBSD)
LDCONFIG_FLAGS=-m
CFLAGS += -I../zaptel -I../zapata
endif
endif

all: depend $(STATIC_LIBRARY) $(DYNAMIC_LIBRARY)

update:
	@echo "Updating from CVS"
	@cvs update -d

install: $(STATIC_LIBRARY) $(DYNAMIC_LIBRARY)
	mkdir -p $(INSTALL_PREFIX)/usr/lib
	mkdir -p $(INSTALL_PREFIX)/usr/include
	install -m 644 libpri.h $(INSTALL_PREFIX)/usr/include
	install -m 755 $(DYNAMIC_LIBRARY) $(INSTALL_PREFIX)/usr/lib
	( cd $(INSTALL_PREFIX)/usr/lib ; ln -sf libpri.so.1 libpri.so )
	install -m 644 $(STATIC_LIBRARY) $(INSTALL_PREFIX)/usr/lib
	/sbin/ldconfig

uninstall:
	@echo "Removing Libpri"
	rm -f $(INSTALL_PREFIX)/usr/lib/libpri.so.1.0
	rm -f $(INSTALL_PREFIX)/usr/lib/libpri.so
	rm -f $(INSTALL_PREFIX)/usr/lib/libpri.a
	rm -f $(INSTALL_PREFIX)/usr/include/libpri.h

pritest: pritest.o
	$(CC) -o pritest pritest.o -L. -lpri -lzap

testprilib.o: testprilib.c
	$(CC) $(CFLAGS) -D_REENTRANT -D_GNU_SOURCE -o $@ -c $<

testprilib: testprilib.o
	$(CC) -o testprilib testprilib.o -L. -lpri -lpthread

pridump: pridump.o
	$(CC) -o pridump pridump.o -L. -lpri -lzap

include .depend

%.lo : %.c
	$(CC) -fPIC $(CFLAGS) -o $@ -c $<

$(STATIC_LIBRARY): $(STATIC_OBJS)
	ar rcs $(STATIC_LIBRARY) $(STATIC_OBJS)
	ranlib $(STATIC_LIBRARY)

$(DYNAMIC_LIBRARY): $(DYNAMIC_OBJS)
	$(CC) -shared -Wl,-soname,libpri.so.1 -o $@ $(DYNAMIC_OBJS)
	/sbin/ldconfig $(LDCONFIG_FLAGS) .
	ln -sf libpri.so.1 libpri.so

clean:
	rm -f *.o *.so *.lo *.so.1 *.so.1.0
	rm -f testpri testprilib $(STATIC_LIBRARY) $(DYNAMIC_LIBRARY)
	rm -f pritest pridump
	rm -f .depend

depend: .depend

.depend: 
	./mkdep ${CFLAGS} `ls *.c`
