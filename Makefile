CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -pedantic
BUILD ?= build
# TLS only when the OpenSSL development files are present (no new system packages are installed by this POC).
HAVE_SSL := $(shell test -f /usr/include/openssl/ssl.h && (test -f /usr/lib/x86_64-linux-gnu/libssl.so || test -f /usr/lib/libssl.so) && echo yes)
ifeq ($(HAVE_SSL),yes)
TLSFLAGS := -DHAVE_OPENSSL
TLSLIBS := -lssl -lcrypto
endif

$(BUILD)/curlhost: cpp/curlhost.cpp cpp/http.hpp cpp/dslvm.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(TLSFLAGS) -o $@ cpp/curlhost.cpp $(TLSLIBS)

clean:
	rm -rf $(BUILD)

.PHONY: clean
