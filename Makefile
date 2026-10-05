CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
BUILD    := build

.PHONY: all test clean sanitize
all: $(BUILD)/test_protocol

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_protocol: tests/test_protocol.cpp src/protocol.cpp src/protocol.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_protocol.cpp src/protocol.cpp

test: $(BUILD)/test_protocol
	$(BUILD)/test_protocol

# Rebuild everything with AddressSanitizer and UBSan, then run the tests.
sanitize: clean
	$(MAKE) CXXFLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra" test

clean:
	rm -rf $(BUILD)
