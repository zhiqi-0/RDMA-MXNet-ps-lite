ifdef config
include $(config)
endif

include make/ps.mk

ifndef CXX
CXX = g++
endif

ifndef DEPS_PATH
DEPS_PATH = $(shell pwd)/deps
endif


ifndef PROTOC
PROTOC = ${DEPS_PATH}/bin/protoc
endif

INCPATH = -I./src -I./include -I$(DEPS_PATH)/include
CFLAGS = -std=c++11 -msse2 -fPIC -O3 -ggdb -Wall -finline-functions $(INCPATH) $(ADD_CFLAGS)

all: ps test

include make/deps.mk

clean:
	rm -rf build $(TEST) tests/*.d
	find src -name "*.pb.[ch]*" -delete

lint:
	python tests/lint.py ps all include/ps src

ps: build/libps.a

OBJS = $(addprefix build/, customer.o postoffice.o van.o meta.pb.o)
build/libps.a: $(OBJS)
	ar crv $@ $(filter %.o, $?)

build/%.o: src/%.cc ${ZMQ} src/meta.pb.h
	@mkdir -p $(@D)
	$(CXX) $(INCPATH) -std=c++0x -MM -MT build/$*.o $< >build/$*.d
	$(CXX) $(CFLAGS) -c $< -o $@

src/%.pb.cc src/%.pb.h : src/%.proto ${PROTOBUF}
	$(PROTOC) --cpp_out=./src --proto_path=./src $<

-include build/*.d
-include build/*/*.d

include tests/test.mk
test: $(TEST)

runsch:
	~/SongXN/ps_dis/RDMA_dis_sche.sh 3 3 tests/test_kv_app
runsvr:
	~/SongXN/ps_dis/RDMA_dis_server.sh 3 3 tests/test_kv_app
runwkr:
	~/SongXN/ps_dis/RDMA_dis_worker.sh 3 3 tests/test_kv_app
num = 4
rundist: 
	~/SongXN/ps_dis/RDMA_dis_sche.sh $(num) $(num) ~/SongXN/mxnet/ps-lite/tests/test_kv_app & (ssh node6  ~/SongXN/ps_dis/RDMA_dis_server.sh $(num) $(num) ~/SongXN/mxnet/ps-lite/tests/test_kv_app) & (ssh node7  ~/SongXN/ps_dis/RDMA_dis_worker.sh $(num) $(num) ~/SongXN/mxnet/ps-lite/tests/test_kv_app)
