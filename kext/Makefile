# Command-line build of AMDHV.kext, NO Xcode project needed.

SDK     := $(shell xcrun --sdk macosx --show-sdk-path)
KFW     := $(SDK)/System/Library/Frameworks/Kernel.framework
OUT     := ../build
KEXT    := $(OUT)/AMDHV.kext
OBJDIR  := $(OUT)/kext-obj

# Sources are grouped by responsibility under Source/. Find them recursively.
CXXSRC  := $(shell find Source -name '*.cpp' | sort)
CSRC    := $(shell find Source -name '*.c' | sort)
ASMSRC  := $(shell find Source -name '*.S' | sort)
OBJS    := $(patsubst Source/%.cpp,$(OBJDIR)/%.o,$(CXXSRC)) \
           $(patsubst Source/%.c,$(OBJDIR)/%.o,$(CSRC)) \
           $(patsubst Source/%.S,$(OBJDIR)/%.o,$(ASMSRC))

# Every group directory is on the include path, so headers keep short #includes.
INCDIRS := $(shell find Source -type d | sort)

# make DEBUG=1  ->  -O0 -g, AMDHV_DEBUG defined (kernel log level defaults to DEBUG)
DEBUG   ?= 0
ifeq ($(DEBUG),1)
OPT     := -O0 -g -DAMDHV_DEBUG=1
else
OPT     := -O2
endif

COMMON  := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=10.15 \
           -mkernel -nostdinc -nostdlibinc \
           -I$(KFW)/Headers -I$(KFW)/PrivateHeaders $(addprefix -I,$(INCDIRS)) \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -Wall -Wextra $(OPT)

CXXFLAGS := $(COMMON) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fno-builtin -MMD -MP
CFLAGS   := $(COMMON) -fno-builtin -MMD -MP

# Header dependencies: editing a .hpp/.h rebuilds every object that includes it.
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(KEXT)

$(OBJDIR)/%.o: Source/%.cpp
	@mkdir -p $(dir $@)
	xcrun clang++ $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: Source/%.c
	@mkdir -p $(dir $@)
	xcrun clang $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: Source/%.S
	@mkdir -p $(dir $@)
	xcrun clang -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=10.15 -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp Info.plist $(KEXT)/Contents/Info.plist
	xcrun clang++ -arch x86_64 -isysroot $(SDK) -mkernel -nostdlib -fapple-kext \
		-Xlinker -kext -L$(SDK)/usr/lib \
		-o $(KEXT)/Contents/MacOS/AMDHV $(OBJS) \
		-lkmodc++ -lkmod -lcc_kext
	../scripts/check-kext-symbols.sh $(KEXT)
	@echo "Built $(KEXT)"
	@echo "OpenCore loads it unsigned via Kernel -> Add"

clean:
	rm -rf $(KEXT) $(OBJDIR)

-include $(DEPS)
