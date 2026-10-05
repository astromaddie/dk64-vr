# sfvr - `make test` builds and runs everything; see README.md.
#   make test              clang, strict C99, C++17 header check, test run
#   make test CC=gcc       same with gcc for the C parts
#   make sanitize          the C tests under ASan + UBSan
CC := clang
CXX := clang++
BUILD := build

CWARN := -std=c99 -Wall -Wextra -Werror -pedantic
CXXWARN := -std=c++17 -Wall -Wextra -Werror
CFLAGS += $(CWARN) -O2 -Iinclude
CXXFLAGS += $(CXXWARN) -O2 -Iinclude

HEADERS := $(wildcard include/sfvr/*.h)
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
TESTS := view turn haptics room perf settings
TEST_BINS := $(addprefix $(BUILD)/test_,$(TESTS))
CXX_HEADER_TUS := $(patsubst include/sfvr/%.h,$(BUILD)/cxx_%.cpp,$(HEADERS))

.PHONY: all lib test sanitize clean run-tests cxx-headers
all: lib

lib: $(BUILD)/libsfvr.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libsfvr.a: $(OBJS)
	rm -f $@
	ar rcs $@ $(OBJS)

$(BUILD)/test_%: tests/test_%.c tests/sfvr_test.h $(BUILD)/libsfvr.a
	$(CC) $(CFLAGS) $< $(BUILD)/libsfvr.a -lm -o $@

$(BUILD)/test_cxx: tests/test_cxx.cpp $(HEADERS) $(BUILD)/libsfvr.a
	$(CXX) $(CXXFLAGS) $< $(BUILD)/libsfvr.a -o $@

# every public header compiled on its own as a C++ TU (proves extern "C" guards,
# self-containedness and C++ usability); -fsyntax-only, nothing is linked
$(BUILD)/cxx_%.cpp: include/sfvr/%.h | $(BUILD)
	printf '#include "sfvr/%s.h"\n#include "sfvr/%s.h"\n' $* $* > $@

cxx-headers: $(CXX_HEADER_TUS)
	@for tu in $(CXX_HEADER_TUS); do \
		echo "$(CXX) $(CXXWARN) -fsyntax-only $$tu"; \
		$(CXX) $(CXXWARN) -Iinclude -fsyntax-only $$tu || exit 1; \
	done

# each header also compiled alone as C, so none depends on include order
c-headers: $(HEADERS) | $(BUILD)
	@for h in $(HEADERS); do \
		printf '#include "sfvr/%s"\n' $$(basename $$h) > $(BUILD)/c_header.c; \
		$(CC) $(CWARN) -Iinclude -fsyntax-only $(BUILD)/c_header.c || exit 1; \
	done

run-tests: $(TEST_BINS) $(BUILD)/test_cxx
	@fail=0; for t in $(TEST_BINS) $(BUILD)/test_cxx; do $$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "ALL PASSED"; else echo "FAILURES"; exit 1; fi

test: cxx-headers c-headers run-tests

sanitize:
	rm -rf $(BUILD)/san && mkdir -p $(BUILD)/san
	@for t in $(TESTS); do \
		$(CC) $(CWARN) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude \
			tests/test_$$t.c $(SRCS) -lm -o $(BUILD)/san/test_$$t || exit 1; \
		$(BUILD)/san/test_$$t || exit 1; \
	done

clean:
	rm -rf $(BUILD)
