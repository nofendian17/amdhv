# Top-level build. Run on the Hackintosh (needs Xcode command line tools).
#
#   make test     host-side unit tests, works on any OS with a C compiler
#   make tools    build/amd-info, build/hvf-smoke
#   make kext     build/AMDHV.kext   (see kext/Makefile)
#   make all      tools + kext
#   make clean

BUILD := build
CC    ?= cc
CXX   ?= c++

.PHONY: all test tools kext clean

all: tools kext

$(BUILD):
	mkdir -p $(BUILD)

test: | $(BUILD)
	$(CC) -Wall -Wextra -Werror -o $(BUILD)/host_test tests/host_test.c
	$(BUILD)/host_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/vmcb_layout tests/vmcb_layout.cpp
	$(BUILD)/vmcb_layout
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/npt_test tests/npt_test.cpp
	$(BUILD)/npt_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/vmx_map_test tests/vmx_map_test.cpp
	$(BUILD)/vmx_map_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/vmcs_state_test tests/vmcs_state_test.cpp
	$(BUILD)/vmcs_state_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -mavx2 -fsanitize=address,undefined -g -o $(BUILD)/fpu_state_test tests/fpu_state_test.cpp
	$(BUILD)/fpu_state_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/msr_map_test tests/msr_map_test.cpp
	$(BUILD)/msr_map_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/slots_test tests/slots_test.cpp
	$(BUILD)/slots_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/run_deadline_test tests/run_deadline_test.cpp
	$(BUILD)/run_deadline_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/asid_test tests/asid_test.cpp
	$(BUILD)/asid_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/tlb_policy_test tests/tlb_policy_test.cpp
	$(BUILD)/tlb_policy_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/clean_bits_test tests/clean_bits_test.cpp
	$(BUILD)/clean_bits_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/space_pool_test tests/space_pool_test.cpp
	$(BUILD)/space_pool_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/hv_support_decode_test tests/hv_support_decode_test.cpp
	$(BUILD)/hv_support_decode_test

tools: | $(BUILD)
	$(CC) -arch x86_64 -Wall -Wextra -O2 -o $(BUILD)/amd-info tools/amd-info/main.c \
		-framework IOKit -framework CoreFoundation
	$(CC) -arch x86_64 -Wall -Wextra -Wno-nullability-completeness -O2 -o $(BUILD)/hvf-smoke tools/hvf-smoke/main.c \
		-framework Hypervisor
	$(CC) -arch x86_64 -Wall -Wextra -Wno-nullability-completeness -O2 -o $(BUILD)/hvf-probe tools/hvf-probe/main.c \
		-framework Hypervisor
	codesign -s - --force --entitlements tools/hvf-smoke/hypervisor.entitlements $(BUILD)/hvf-smoke
	codesign -s - --force --entitlements tools/hvf-smoke/hypervisor.entitlements $(BUILD)/hvf-probe

kext:
	$(MAKE) -C kext

clean:
	rm -rf $(BUILD)
