# Top-level build. Run on the Hackintosh (needs Xcode command line tools).
#
#   make test     host-side unit tests, works on any OS with a C compiler
#   make tools    build/amd-info, build/hvf-smoke, build/hvf-probe
#   make kext     build/AMDHV.kext   (see kext/Makefile)
#   make all      tools + kext
#   make clean

BUILD := build
CC    ?= cc
CXX   ?= c++

WARNINGS := -Wall -Wextra -Werror
CXXSTD   := -std=c++17
SANITIZE := -fsanitize=address,undefined -g

# Host tests include kext headers directly; keep the short #include names working after the move into groups.
SRC_DIRS := $(shell find kext/Source -type d | sort)
TEST_INC := $(addprefix -I,$(SRC_DIRS))

COV := $(BUILD)/cov

.PHONY: all test tools kext clean coverage amd-info hvf-smoke hvf-probe

all: tools kext

$(BUILD):
	mkdir -p $(BUILD)

# Each host test is one binary; this list is the single source of truth.
# `make test` builds every binary, then runs all of them.
TESTS := host_test vmcb_layout npt_test vmx_map_test vmcs_state_test fpu_state_test \
         msr_map_test slots_test run_deadline_test asid_test tlb_policy_test \
         sleep_policy_test clean_bits_test space_pool_test hv_support_decode_test \
         tsc_offset_test apic_test apic_mmio_test
TEST_BINS := $(addprefix $(BUILD)/,$(TESTS))

# Sanitizers cover the allocator/rollback and layout tests; fpu_state_test also
# needs AVX2. These flags are part of each test's contract, not global defaults.
$(BUILD)/npt_test $(BUILD)/vmx_map_test $(BUILD)/vmcs_state_test \
$(BUILD)/msr_map_test $(BUILD)/slots_test: TEST_FLAGS := $(SANITIZE)
$(BUILD)/fpu_state_test: TEST_FLAGS := -mavx2 $(SANITIZE)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo $$t; $$t; done

$(TEST_BINS): Makefile

$(BUILD)/host_test: tests/host_test.c | $(BUILD)
	$(CC) $(WARNINGS) $(TEST_INC) -MMD -MP -o $@ $<

$(BUILD)/%: tests/%.cpp | $(BUILD)
	$(CXX) $(CXXSTD) $(WARNINGS) $(TEST_INC) $(TEST_FLAGS) -MMD -MP -o $@ $<

# Headers are the tests' real input; recompile a binary when one it includes changes.
-include $(TEST_BINS:=.d)

# Instrumented host tests. gcov is run once per binary into its own directory:
# without that, headers shared by several tests overwrite each other's .gcov files.
coverage: | $(BUILD)
	@rm -rf $(COV) && mkdir -p $(COV)/gcov
	@set -e; for t in $(TESTS); do \
	  case $$t in \
	    host_test)       $(CC) $(WARNINGS) $(TEST_INC) --coverage -o $(COV)/$$t tests/$$t.c ;; \
	    fpu_state_test)  $(CXX) $(CXXSTD) $(TEST_INC) -mavx2 --coverage -o $(COV)/$$t tests/$$t.cpp ;; \
	    *)               $(CXX) $(CXXSTD) $(TEST_INC) --coverage -o $(COV)/$$t tests/$$t.cpp ;; \
	  esac; \
	  $(COV)/$$t >/dev/null; \
	  gcov -p -b $(COV)/$$t-$$t.gcda >/dev/null 2>&1; \
	  mkdir -p $(COV)/gcov/$$t-$$t; \
	  mv ./*.gcov $(COV)/gcov/$$t-$$t/ 2>/dev/null || true; \
	done
	@echo "Coverage data: $(COV)/gcov"

# Each tool is its own target so a single one can be rebuilt while debugging.
TOOL_CFLAGS := -arch x86_64 -Wall -Wextra -O2
HV_CFLAGS   := $(TOOL_CFLAGS) -Wno-nullability-completeness
ENTITLEMENTS := tools/hvf-smoke/hypervisor.entitlements

tools: amd-info hvf-smoke hvf-probe

amd-info: | $(BUILD)
	$(CC) $(TOOL_CFLAGS) -o $(BUILD)/amd-info tools/amd-info/main.c \
		-framework IOKit -framework CoreFoundation

hvf-smoke: | $(BUILD)
	$(CC) $(HV_CFLAGS) -o $(BUILD)/hvf-smoke tools/hvf-smoke/main.c -framework Hypervisor
	codesign -s - --force --entitlements $(ENTITLEMENTS) $(BUILD)/hvf-smoke

hvf-probe: | $(BUILD)
	$(CC) $(HV_CFLAGS) -o $(BUILD)/hvf-probe tools/hvf-probe/main.c -framework Hypervisor
	codesign -s - --force --entitlements $(ENTITLEMENTS) $(BUILD)/hvf-probe

kext:
	$(MAKE) -C kext

clean:
	rm -rf $(BUILD)
