# Command-line build of AMDHV.kext, NO Xcode project needed.

SDK     := $(shell xcrun --sdk macosx --show-sdk-path)
KFW     := $(SDK)/System/Library/Frameworks/Kernel.framework
OUT     := ../build
KEXT    := $(OUT)/AMDHV.kext
OBJDIR  := $(OUT)/kext-obj

# The v* git tag is the release version. Source archives made with `git archive`
# have no .git, so fall back to the version committed in Info.plist there.
GIT_VERSION  := $(shell git describe --tags --abbrev=0 --match 'v*' 2>/dev/null | sed 's/^v//')
PLIST_VERSION := $(shell /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' Info.plist 2>/dev/null)
VERSION      ?= $(if $(GIT_VERSION),$(GIT_VERSION),$(PLIST_VERSION))
PLIST        := $(KEXT)/Contents/Info.plist

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

# Clang static analysis, like the analyze-clang job in AppleALC's CI. The .d
# flags are dropped: --analyze does not produce objects or dependency files.
ANALYZE_CXX := $(filter-out -MMD -MP,$(CXXFLAGS))
ANALYZE_C   := $(filter-out -MMD -MP,$(CFLAGS))

.PHONY: all clean analyze

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
	cp Info.plist $(PLIST)
	/usr/libexec/PlistBuddy -c 'Set :CFBundleShortVersionString $(VERSION)' $(PLIST)
	/usr/libexec/PlistBuddy -c 'Set :CFBundleVersion $(VERSION)' $(PLIST)
	xcrun clang++ -arch x86_64 -isysroot $(SDK) -mkernel -nostdlib -fapple-kext \
		-Xlinker -kext -L$(SDK)/usr/lib \
		-o $(KEXT)/Contents/MacOS/AMDHV $(OBJS) \
		-lkmodc++ -lkmod -lcc_kext
	../scripts/check-kext-symbols.sh $(KEXT)
	@echo "Built $(KEXT), version $(VERSION)"
	@echo "OpenCore loads it unsigned via Kernel -> Add"

analyze:
	@mkdir -p $(OUT)/analyze
	@set -e; for f in $(CXXSRC) $(CSRC); do \
	  case "$$f" in \
	    *.cpp) xcrun clang++ --analyze -Xanalyzer -analyzer-output=text $(ANALYZE_CXX) "$$f" ;; \
	    *.c)   xcrun clang --analyze -Xanalyzer -analyzer-output=text $(ANALYZE_C) "$$f" ;; \
	  esac; \
	done > $(OUT)/analyze/analyzer.log 2>&1 || { cat $(OUT)/analyze/analyzer.log; exit 1; }
	@if grep -qE 'warning:|error:' $(OUT)/analyze/analyzer.log; then \
	  cat $(OUT)/analyze/analyzer.log; exit 1; \
	fi
	@echo "static analysis clean"

clean:
	rm -rf $(KEXT) $(OBJDIR)

-include $(DEPS)
