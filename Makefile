# padwave — one binary from one translation unit.
CXX ?= c++
CXXFLAGS ?= -std=c++23 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra
PKGS := libavformat libavcodec libswresample libavutil
CPPFLAGS += -Iinclude -Ibuild $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lopus -lz -lm

all: bin/padwave

build/padwave_ui.hh: ui/index.html scripts/gen_ui.py
	@mkdir -p build
	python3 scripts/gen_ui.py ui/index.html $@

build/padwave.o: src/padwave.cc build/padwave_ui.hh $(wildcard include/*.hh)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c src/padwave.cc -o $@

bin/padwave: build/padwave.o
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ build/padwave.o $(LDLIBS)

install: bin/padwave
	install -Dm755 bin/padwave $(HOME)/.local/bin/padwave

test: bin/padwave
	./tests/test_wire.sh

clean:
	rm -rf build bin

.PHONY: all install test clean
