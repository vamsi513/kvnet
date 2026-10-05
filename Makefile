CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
BUILD    := build

.PHONY: all test clean sanitize
all: $(BUILD)/kvnet-server

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/kvnet-server: src/server.cpp src/protocol.cpp src/poller.cpp src/protocol.hpp src/poller.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ src/server.cpp src/protocol.cpp src/poller.cpp

$(BUILD)/test_protocol: tests/test_protocol.cpp src/protocol.cpp src/protocol.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_protocol.cpp src/protocol.cpp

test: $(BUILD)/test_protocol $(BUILD)/kvnet-server
	$(BUILD)/test_protocol
	python3 tests/integration.py $(BUILD)/kvnet-server

# Rebuild everything with AddressSanitizer and UBSan, then run the tests.
sanitize: clean
	$(MAKE) CXXFLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra" test

clean:
	rm -rf $(BUILD)
