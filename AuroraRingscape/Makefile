TARGET = AuroraRingscape
APP_TYPE = BOOT_SRAM

CPP_SOURCES = AuroraRingscape.cpp \
	part.cc \
	resonator.cc \
	string.cc \
	fm_voice.cc \
	resources.cc \
	atan.cc \
	units.cc \
	random.cc

AURORA_SDK_PATH ?= ../Aurora-SDK

C_INCLUDES += -I$(AURORA_SDK_PATH)/include/ -Ivendor/

LIBDAISY_DIR = $(AURORA_SDK_PATH)/libs/libDaisy/
DAISYSP_DIR  = $(AURORA_SDK_PATH)/libs/DaisySP/
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
