CXX = g++
CXXFLAGS = -O2 -g -Wall -Wno-reorder

CLIENT_SRC = ../njclient.cpp ../mpb.cpp ../netmsg.cpp ../../WDL/jnetlib/asyncdns.cpp ../../WDL/jnetlib/connection.cpp \
  ../../WDL/jnetlib/listen.cpp ../../WDL/jnetlib/util.cpp ../../WDL/rng.cpp ../../WDL/sha.cpp ../../WDL/resample.cpp

default: opus_codec_test flac_codec_test njclient_e2e

opus_codec_test: opus_codec_test.cpp ../opusencdec.h ../../WDL/resample.cpp
	$(CXX) $(CXXFLAGS) -o $@ opus_codec_test.cpp ../../WDL/resample.cpp -lopus -logg -lm

flac_codec_test: flac_codec_test.cpp ../flacencdec.h
	$(CXX) $(CXXFLAGS) -o $@ flac_codec_test.cpp -lFLAC -lm

# a client with Opus and FLAC built in, driven headless against a running server
njclient_e2e: njclient_e2e.cpp $(CLIENT_SRC) ../opusencdec.h ../flacencdec.h ../njclient.h
	$(CXX) $(CXXFLAGS) -pthread -DNINJAM_OPUS -DNINJAM_FLAC -o $@ njclient_e2e.cpp $(CLIENT_SRC) -lopus -lFLAC -lvorbisenc -lvorbis -logg -lm

clean:
	-rm -f opus_codec_test flac_codec_test njclient_e2e
