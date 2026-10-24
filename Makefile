# Kestrel: a UCI chess engine in C++17.
#
#   make           build ./kestrel
#   make test      build and run the unit tests (perft, search, eval, notation, UCI)
#   make bench     run the fixed-depth benchmark
#   make sanitize  build ./kestrel-sanitize with AddressSanitizer + UBSan and run the tests with it
#   make clean

CXX      ?= c++
CXXFLAGS ?= -O3
WARNINGS  = -std=c++17 -Wall -Wextra -Wpedantic -Wshadow
ifdef WERROR
WARNINGS += -Werror
endif
LDLIBS   += -pthread

# macOS: work around Command Line Tools installs whose default SDK can't be linked against.
ifeq ($(shell uname -s),Darwin)
ifeq ($(origin SDKROOT),undefined)
FALLBACK_SDK := $(shell scripts/macos-sdk.sh $(CXX))
ifneq ($(FALLBACK_SDK),)
export SDKROOT := $(FALLBACK_SDK)
endif
endif
endif

ENGINE_SRC = $(filter-out src/main.cpp,$(wildcard src/*.cpp))
HEADERS    = $(wildcard src/*.h)
ENGINE_OBJ = $(ENGINE_SRC:src/%.cpp=build/%.o)
TEST_OBJ   = build/tests/test_main.o

SANITIZE   = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

.PHONY: all test bench sanitize clean

all: kestrel

kestrel: $(ENGINE_OBJ) build/main.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

kestrel-tests: $(ENGINE_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(WARNINGS) -c -o $@ $<

build/tests/%.o: tests/%.cpp $(HEADERS) | build/tests
	$(CXX) $(CXXFLAGS) $(WARNINGS) -Isrc -c -o $@ $<

build build/tests:
	mkdir -p $@

test: kestrel kestrel-tests
	./kestrel-tests

bench: kestrel
	./kestrel bench

# A separate, unoptimised build with the sanitizers; it runs a lighter version of the test suite.
sanitize:
	$(CXX) $(SANITIZE) $(WARNINGS) -Isrc -o kestrel-sanitize-tests $(ENGINE_SRC) tests/test_main.cpp $(LDLIBS)
	KESTREL_QUICK_TESTS=1 ./kestrel-sanitize-tests

clean:
	rm -rf build kestrel kestrel-tests kestrel-sanitize-tests
