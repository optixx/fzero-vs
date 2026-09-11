.DEFAULT_GOAL := help
ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PYTHON ?= python3
CMAKE ?= cmake
JOBS ?= 4
BUILD_TYPE ?= RelWithDebInfo
ARES_CMAKE_ARGS ?=
SERVER_CMAKE_ARGS ?=
RUN_ARGS ?=

.PHONY: help all fetch patch ares-fetch ares-patch ares-configure ares server-configure server test test-mcp test-2p test-4p dry-run-2p dry-run-4p clean

help:
	@echo 'F-Zero VS development targets:'
	@echo '  make fetch / patch    Fetch pinned Ares / apply patches/ares/series'
	@echo '  make ares             Configure and build the SNES-only Ares app'
	@echo '  make server           Configure and build the C11 server'
	@echo '  make all              Build both components'
	@echo '  make test             Run helpers, protocol, server and client memory/fault tests'
	@echo '  make test-mcp         Run the MCP sidecar against its fake ares bridge'
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

test: server test-protocol test-client test-session test-interpolation test-mcp
	$(PYTHON) "$(ROOT)/tests/test_server.py"
	$(PYTHON) "$(ROOT)/scripts/test_ares_source.py"
	$(PYTHON) "$(ROOT)/scripts/test_local_harness.py"

test-mcp:
	PYTHONPATH="$(ROOT)/mcp/src" $(PYTHON) -m unittest discover -s "$(ROOT)/mcp/tests" -v

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

.PHONY: test-session
test-session:
	@mkdir -p "$(ROOT)/build/tests"
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$(ROOT)/server/include" -I"$(ROOT)/shared" -c "$(ROOT)/server/src/server.c" -o "$(ROOT)/build/tests/server-core.o"
	$(CXX) -std=c++20 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$(ROOT)/client" -I"$(ROOT)/server/include" -I"$(ROOT)/shared" "$(ROOT)/tests/session_test.cpp" "$(ROOT)/client/fzvs_session.cpp" "$(ROOT)/build/tests/server-core.o" -o "$(ROOT)/build/tests/session-test"
	"$(ROOT)/build/tests/session-test"

.PHONY: test-client-lifecycle
test: test-client-lifecycle
test-client-lifecycle:
	@mkdir -p "$(ROOT)/build/tests"
	$(CXX) -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$(ROOT)/client" -I"$(ROOT)/shared" "$(ROOT)/tests/client_lifecycle_test.cpp" "$(ROOT)/client/fzvs_client.cpp" -o "$(ROOT)/build/tests/client-lifecycle-test"
	"$(ROOT)/build/tests/client-lifecycle-test"

.PHONY: test-interpolation
test-interpolation:
	@mkdir -p "$(ROOT)/build/tests"
	$(CXX) -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$(ROOT)/client" -I"$(ROOT)/shared" "$(ROOT)/tests/interpolation_test.cpp" "$(ROOT)/client/fzvs_client.cpp" -o "$(ROOT)/build/tests/interpolation-test"
	"$(ROOT)/build/tests/interpolation-test"

# Native Cocoa regression; requires the Ares build and a desktop session.
.PHONY: test-ui-tabs
test-ui-tabs: ares
	@mkdir -p "$(ROOT)/build/tests"
	$(CXX) -std=c++20 -DHIRO_COCOA -isystem "$(ROOT)/vendor/ares" -isystem "$(ROOT)/vendor/ares/nall" "$(ROOT)/tests/hiro_tabs_test.cpp" "$(ROOT)/build/ares/hiro/libhiro.a" "$(ROOT)/build/ares/nall/nall/CMakeFiles/nall.dir/nall.cpp.o" -framework Cocoa -framework Carbon -framework IOKit -framework Security -o "$(ROOT)/build/tests/hiro-tabs-test"
	"$(ROOT)/build/tests/hiro-tabs-test"

.PHONY: test-ux
test: test-ux
test-ux: server
	@mkdir -p "$(ROOT)/build/tests"
	$(CXX) -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$(ROOT)/client" -I"$(ROOT)/shared" "$(ROOT)/tests/presentation_test.cpp" -o "$(ROOT)/build/tests/presentation-test"
	"$(ROOT)/build/tests/presentation-test"
	$(CXX) -std=c++20 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined -I"$(ROOT)/client" -I"$(ROOT)/shared" -I"$(ROOT)/server/include" "$(ROOT)/tests/discovery_test.cpp" "$(ROOT)/build/server/libfzvs-server-core.a" -o "$(ROOT)/build/tests/discovery-test"
	"$(ROOT)/build/tests/discovery-test"

.PHONY: test-ui-text
test-ui-text: ares
	@mkdir -p "$(ROOT)/build/tests"
	$(CXX) -std=c++20 -DHIRO_COCOA -isystem "$(ROOT)/vendor/ares" -isystem "$(ROOT)/vendor/ares/nall" "$(ROOT)/tests/hiro_text_test.mm" "$(ROOT)/build/ares/hiro/libhiro.a" "$(ROOT)/build/ares/nall/nall/CMakeFiles/nall.dir/nall.cpp.o" -framework Cocoa -framework Carbon -framework IOKit -framework Security -o "$(ROOT)/build/tests/hiro-text-test"
	"$(ROOT)/build/tests/hiro-text-test"
