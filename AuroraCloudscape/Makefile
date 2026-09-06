TARGET = AuroraCloudscape
APP_TYPE = BOOT_SRAM

CPP_SOURCES = \
AuroraCloudscape.cpp \
dsp/correlator.cpp \
dsp/granular_processor.cpp \
dsp/pvoc/frame_transformation.cpp \
dsp/pvoc/phase_vocoder.cpp \
dsp/pvoc/stft.cpp \
resources.cpp

AURORA_SDK_PATH ?= ../Aurora-SDK

C_INCLUDES += \
-I$(AURORA_SDK_PATH)/include/ \
-I. \
-Idsp \
-Idsp/pvoc \
-Idsp/fx

LIBDAISY_DIR = $(AURORA_SDK_PATH)/libs/libDaisy/
DAISYSP_DIR  = $(AURORA_SDK_PATH)/libs/DaisySP/
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
