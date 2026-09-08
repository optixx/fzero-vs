.DEFAULT_GOAL := help
ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PYTHON ?= python3
CMAKE ?= cmake
JOBS ?= 4
BUILD_TYPE ?= RelWithDebInfo
ARES_CMAKE_ARGS ?=
SERVER_CMAKE_ARGS ?=
RUN_ARGS ?=

.PHONY: help all fetch patch ares-fetch ares-patch ares-configure ares server-configure server test test-2p test-4p dry-run-2p dry-run-4p clean

help:
	@echo 'F-Zero VS development targets:'
	@echo '  make fetch / patch    Fetch pinned Ares / apply patches/ares/series'
	@echo '  make ares             Configure and build the SNES-only Ares app'
	@echo '  make server           Configure and build the C11 server'
	@echo '  make all              Build both components'
	@echo '  make test             Run helpers, protocol, server and client memory/fault tests'
	@echo '  make dry-run-2p       Preview local startup (also dry-run-4p)'
	@echo '  make test-2p          Build both and launch 2 clients (also test-4p)'
	@echo '  make clean            Clean compiled outputs; keep downloads, patches and logs'
	@echo 'Overrides: JOBS=8 BUILD_TYPE=Debug ARES_CMAKE_ARGS="..." SERVER_CMAKE_ARGS="..." RUN_ARGS="..."'

all: ares server
fetch: ares-fetch
patch: ares-patch

ares-fetch:
	@$(PYTHON) "$(ROOT)/scripts/ares_source.py" fetch

ares-patch: ares-fetch
	@$(PYTHON) "$(ROOT)/scripts/ares_source.py" patch

ares-configure: ares-patch
	$(CMAKE) -S "$(ROOT)/vendor/ares" -B "$(ROOT)/build/ares" -G Ninja \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DARES_CORES=sfc \
	  -DFZVS_PROJECT_DIR="$(ROOT)" \
	  -DARES_BUILD_LOCAL=ON -DARES_BUILD_OPTIONAL_TARGETS=OFF \
	  -DARES_ENABLE_CHD=OFF -DARES_ENABLE_LIBRASHADER=OFF $(ARES_CMAKE_ARGS)

ares: ares-configure
	$(CMAKE) --build "$(ROOT)/build/ares" --target desktop-ui --parallel $(JOBS)
	@$(PYTHON) "$(ROOT)/scripts/ares_source.py" link

server-configure:
	$(CMAKE) -S "$(ROOT)/server" -B "$(ROOT)/build/server" -G Ninja \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(SERVER_CMAKE_ARGS)

server: server-configure
	$(CMAKE) --build "$(ROOT)/build/server" --target fzvs-server --parallel $(JOBS)

test: server test-protocol test-client
	$(PYTHON) "$(ROOT)/tests/test_server.py"
	$(PYTHON) "$(ROOT)/scripts/test_ares_source.py"
	$(PYTHON) "$(ROOT)/scripts/test_local_harness.py"

test-2p: all
	@sh "$(ROOT)/scripts/test-2p.sh" $(RUN_ARGS)

test-4p: all
	@sh "$(ROOT)/scripts/test-4p.sh" $(RUN_ARGS)

dry-run-2p:
	@sh "$(ROOT)/scripts/test-2p.sh" --dry-run $(RUN_ARGS)

dry-run-4p:
	@sh "$(ROOT)/scripts/test-4p.sh" --dry-run $(RUN_ARGS)

clean:
	@if test -f "$(ROOT)/build/ares/build.ninja"; then $(CMAKE) --build "$(ROOT)/build/ares" --target clean; fi
	@if test -f "$(ROOT)/build/server/build.ninja"; then $(CMAKE) --build "$(ROOT)/build/server" --target clean; fi

.PHONY: test-protocol test-client
test-protocol:
	@mkdir -p "$(ROOT)/build/tests"
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$(ROOT)/shared" "$(ROOT)/tests/protocol_test.c" -o "$(ROOT)/build/tests/protocol-test"
	"$(ROOT)/build/tests/protocol-test"

test-client: server
	@mkdir -p "$(ROOT)/build/tests"
	$(CXX) -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$(ROOT)/client" -I"$(ROOT)/shared" "$(ROOT)/tests/client_test.cpp" "$(ROOT)/client/fzvs_client.cpp" -o "$(ROOT)/build/tests/client-test"
	$(PYTHON) "$(ROOT)/tests/test_client.py"
