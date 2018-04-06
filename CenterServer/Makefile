CXX=g++

INCPATH=-I. -I.. -I../Include -IItem -I../NetWork -I$(BEHAVIORPATH) -I$(PROTOBUF_DIR)/include -I$(BOOST_ROOT) -I ../ThirdParty/spdlog/include -I ../ThirdParty/pbjson/src -I $(CHILKAT_DIR)/include
CXXFLAGS += $(OPT) -pipe -Wunused-variable -Wno-unused-local-typedefs -Wno-unused-but-set-variable -Wno-literal-suffix -Wall -std=c++11 -ggdb -fPIC -D_GNU_SOURCE -D__STDC_LIMIT_MACROS $(INCPATH)

LIBRARY=$(PROTOBUF_DIR)/lib/libprotobuf.a -L$(BOOST_ROOT)/stage/lib/ ../ThirdParty/cpp_redis/build/lib/libcpp_redis.a ../ThirdParty/cpp_redis/build/lib/libtacopie.a -L$(CHILKAT_DIR)/lib
LDFLAGS = -lboost_system -lboost_thread -lboost_filesystem -lboost_date_time -lpthread

PROTO_SRC=P_Command.proto P_Asset.proto P_Protocol.proto P_Server.proto
PROTO_OBJ=$(patsubst %.proto,%.pb.o,$(PROTO_SRC))
PROTO_OPTIONS=--proto_path=. --proto_path=$(PROTOBUF_DIR)/include

BASE_OBJ=WorldSession.o Protocol.o Player.o World.o Asset.o Config.o pbjson.o InnerProtocol.o GmtSession.o Clan.o

BIN=CenterServer

all: $(BIN)

clean:
	@rm -f $(BIN)  *.o *.pb.*

rebuild: clean all

CenterServer: $(PROTO_OBJ) $(BASE_OBJ) $(SUB_OBJ) Main.o
	$(CXX) $^ -o $@ $(LIBRARY) $(LDFLAGS)

%.pb.cc: %.proto
	protoc $(PROTO_OPTIONS) --cpp_out=. $<

%.pb.o: %.pb.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
