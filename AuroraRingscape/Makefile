TARGET = AuroraRingscape
APP_TYPE = BOOT_SRAM

CPP_SOURCES = AuroraRingscape.cpp \
	part.cpp \
	resonator.cpp \
	string.cpp \
	fm_voice.cpp \
	resources.cpp \
	atan.cpp \
	units.cpp \
	random.cpp

AURORA_SDK_PATH ?= ../Aurora-SDK

C_INCLUDES += -I$(AURORA_SDK_PATH)/include/ -Ivendor/

LIBDAISY_DIR = $(AURORA_SDK_PATH)/libs/libDaisy/
DAISYSP_DIR  = $(AURORA_SDK_PATH)/libs/DaisySP/
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
