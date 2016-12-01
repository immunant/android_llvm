LOCAL_PATH := $(call my-dir)
LLVM_ROOT_PATH := $(LOCAL_PATH)

FORCE_BUILD_LLVM_DISABLE_NDEBUG ?= false
# Legality check: FORCE_BUILD_LLVM_DISABLE_NDEBUG should consist of one word -- either "true" or "false".
ifneq "$(words $(FORCE_BUILD_LLVM_DISABLE_NDEBUG))$(words $(filter-out true false,$(FORCE_BUILD_LLVM_DISABLE_NDEBUG)))" "10"
  $(error FORCE_BUILD_LLVM_DISABLE_NDEBUG may only be true, false, or unset)
endif

FORCE_BUILD_LLVM_DEBUG ?= false
# Legality check: FORCE_BUILD_LLVM_DEBUG should consist of one word -- either "true" or "false".
ifneq "$(words $(FORCE_BUILD_LLVM_DEBUG))$(words $(filter-out true false,$(FORCE_BUILD_LLVM_DEBUG)))" "10"
  $(error FORCE_BUILD_LLVM_DEBUG may only be true, false, or unset)
endif

include $(CLEAR_VARS)

# LLVM Command Line Tools
subdirs := \
  tools/bugpoint \
  tools/bugpoint-passes \
  tools/dsymutil \
  tools/llc \
  tools/lli \
  tools/lli/ChildTarget \
  tools/llvm-ar \
  tools/llvm-as \
  tools/llvm-bcanalyzer \
  tools/llvm-c-test \
  tools/llvm-config \
  tools/llvm-cov \
  tools/llvm-cxxdump \
  tools/llvm-dis \
  tools/llvm-diff \
  tools/llvm-dwarfdump \
  tools/llvm-dwp \
  tools/llvm-extract \
  tools/llvm-link \
  tools/llvm-lto \
  tools/llvm-mc \
  tools/llvm-mcmarkup \
  tools/llvm-nm \
  tools/llvm-objdump \
  tools/llvm-pdbdump \
  tools/llvm-profdata \
  tools/llvm-readobj \
  tools/llvm-rtdyld \
  tools/llvm-size \
  tools/llvm-split \
  tools/llvm-symbolizer \
  tools/lto \
  tools/gold \
  tools/obj2yaml \
  tools/opt \
  tools/sancov \
  tools/sanstats \
  tools/verify-uselistorder \
  tools/yaml2obj \

# LLVM Command Line Utilities
subdirs += \
  utils/count \
  utils/FileCheck \
  utils/not \
  utils/yaml-bench \

include $(LOCAL_PATH)/llvm.mk

include $(addprefix $(LOCAL_PATH)/,$(addsuffix /Android.mk, $(subdirs)))
