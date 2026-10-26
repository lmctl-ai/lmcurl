BUILD_DIR ?= build
.PHONY: all test clean
all:
	@if [ "$$(uname -s)" = Linux ]; then scripts/sync-deps.sh; fi
	@scripts/sync-sdk.sh --verify
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --parallel 2
test: all
	BUILD_DIR=$(BUILD_DIR) bash test.sh
clean:
	cmake --build $(BUILD_DIR) --target clean
