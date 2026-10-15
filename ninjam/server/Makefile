CFLAGS = -O2 -Wall -Wno-reorder

ifdef MAC
CFLAGS += -D_MAC
else
CFLAGS += -pthread
endif

CC=gcc
CXX=g++
CXXFLAGS = $(CFLAGS)

OBJS = ../../WDL/jnetlib/asyncdns.o
OBJS += ../../WDL/jnetlib/connection.o
OBJS += ../../WDL/jnetlib/listen.o
OBJS += ../../WDL/jnetlib/util.o
OBJS += ../../WDL/jnetlib/httpget.o
OBJS += ../../WDL/rng.o
OBJS += ../../WDL/sha.o
OBJS += ../mpb.o
OBJS += ../netmsg.o
OBJS += usercon.o
OBJS += portmap.o
OBJS += ninjamsrv.o

# PORTMAP=1 builds in automatic router port mapping (needs miniupnpc and libnatpmp,
# e.g. apt install libminiupnpc-dev libnatpmp-dev, or brew install miniupnpc libnatpmp)
ifdef PORTMAP
CPPFLAGS += -DNINJAM_PORTMAP
LIBS += -lminiupnpc -lnatpmp
portmap.o: CXXFLAGS += -std=c++11
endif


default: ninjamsrv

ninjamsrv: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -s -o ninjamsrv $(OBJS) $(LIBS)

clean:
	-rm $(OBJS) ninjamsrv
