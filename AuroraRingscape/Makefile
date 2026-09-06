TARGET = AuroraRingscape
APP_TYPE = BOOT_SRAM

CPP_SOURCES = AuroraRingscape.cpp \
	vendor/rings/dsp/part.cc \
	vendor/rings/dsp/resonator.cc \
	vendor/rings/dsp/string.cc \
	vendor/rings/dsp/fm_voice.cc \
	vendor/rings/resources.cc \
	vendor/stmlib/dsp/atan.cc \
	vendor/stmlib/dsp/units.cc \
	vendor/stmlib/utils/random.cc

AURORA_SDK_PATH ?= ../Aurora-SDK

C_INCLUDES += -I$(AURORA_SDK_PATH)/include/ -Ivendor/

LIBDAISY_DIR = $(AURORA_SDK_PATH)/libs/libDaisy/
DAISYSP_DIR  = $(AURORA_SDK_PATH)/libs/DaisySP/
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
