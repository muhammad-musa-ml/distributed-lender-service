# Plain GNU make, deliberately. CMake is the obvious reach, but this builds four
# self-contained translation units with no external dependency beyond libstdc++ and
# pthreads -- adding a build system would be the heaviest file in the project.
#
#   make            build everything into bin/
#   make test       build + run the ring correctness tests
#   make tsan       build + run those tests under ThreadSanitizer
#   make bench      build + run the false-sharing microbenchmark
#   make clean

CXX      ?= g++
CXXSTD   ?= -std=c++20
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
OPT      ?= -O2
CXXFLAGS := $(CXXSTD) $(OPT) $(WARN) -pthread
SRC      := src
BIN      := bin

BINARIES := $(BIN)/server $(BIN)/bench_client $(BIN)/bench_ring $(BIN)/test_ring

.PHONY: all test tsan bench clean
all: $(BINARIES)

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/server: $(SRC)/server.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN)/bench_client: $(SRC)/bench_client.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN)/bench_ring: $(SRC)/bench_ring.cpp $(SRC)/spsc_ring.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN)/test_ring: $(SRC)/test_ring.cpp $(SRC)/spsc_ring.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

test: $(BIN)/test_ring
	./$(BIN)/test_ring

# NOTE: setarch -R disables ASLR for the child. Without it ThreadSanitizer aborts on
# WSL2 with "FATAL: unexpected memory mapping" -- and it aborts BEFORE running anything,
# so a naive run prints "0 races" while having checked nothing. Check the exit code.
# The smaller n is deliberate: TSan reasons over the happens-before graph, so volume buys
# little, and it runs 20-50x slower.
tsan: $(SRC)/test_ring.cpp $(SRC)/spsc_ring.hpp | $(BIN)
	$(CXX) $(CXXSTD) -O1 -g -fsanitize=thread -pthread -o $(BIN)/test_ring_tsan $(SRC)/test_ring.cpp
	setarch -R ./$(BIN)/test_ring_tsan 20000

bench: $(BIN)/bench_ring
	./$(BIN)/bench_ring

clean:
	rm -rf $(BIN)
