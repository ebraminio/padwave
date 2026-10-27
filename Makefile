# padwave — one binary (radio + player + input + panel), one process tree.
CXX ?= c++
CXXFLAGS ?= -std=c++23 -O2 -pthread -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra
PKGS := libavformat libavcodec libswresample libavutil
CPPFLAGS += -Iinclude -Ibuild $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lopus -lz -lm -pthread

SRC := src/padwave.cc src/padwave_native.cc src/padwave_opusdec.cc \
       src/padwave_input.cc src/padwave_server.cc src/padwave_app.cc
OBJ := $(SRC:src/%.cc=build/%.o)

all: bin/padwave

build/padwave_ui.hh: ui/index.html scripts/gen_ui.py
	@mkdir -p build
	python3 scripts/gen_ui.py ui/index.html $@

build/%.o: src/%.cc build/padwave_ui.hh $(wildcard include/*.hh)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

bin/padwave: $(OBJ)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

install: bin/padwave
	install -Dm755 bin/padwave $(HOME)/.local/bin/padwave

test: bin/padwave
	./tests/test_wire.sh

clean:
	rm -rf build bin

.PHONY: all install test clean
