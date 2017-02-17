VOB_COMPONENTS := vendor/nxp-nfc/opensource/libnfc-nci/src
NFA := $(VOB_COMPONENTS)/nfa
NFC := $(VOB_COMPONENTS)/nfc

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
include $(call all-makefiles-under,$(LOCAL_PATH))

ifneq ($(NCI_VERSION),)
LOCAL_CFLAGS += -DNCI_VERSION=$(NCI_VERSION) -O0 -g
endif

LOCAL_CFLAGS += -Wall -Wextra -Wno-unused-parameter
#variables for NFC_NXP_CHIP_TYPE
PN547C2 := 1
PN548C2 := 2
PN551   := 3
PN553   := 4
NQ110 := $PN547C2
NQ120 := $PN547C2
NQ210 := $PN548C2
NQ220 := $PN548C2

#NXP chip type Enable
ifeq ($(PN547C2),1)
LOCAL_CFLAGS += -DPN547C2=1
endif
ifeq ($(PN548C2),2)
LOCAL_CFLAGS += -DPN548C2=2
endif
ifeq ($(PN551),3)
LOCAL_CFLAGS += -DPN551=3
endif
ifeq ($(PN553),4)
LOCAL_CFLAGS += -DPN553=4
endif

#NXP PN547 Enable
LOCAL_CFLAGS += -DNXP_EXTNS=TRUE
LOCAL_CFLAGS += -DNFC_NXP_NON_STD_CARD=FALSE
LOCAL_CFLAGS += -DNFC_NXP_HFO_SETTINGS=FALSE

#Enable HCE-F specific
LOCAL_CFLAGS += -DNXP_NFCC_HCE_F=TRUE

#### Select the JCOP OS Version ####
JCOP_VER_3_1 := 1
JCOP_VER_3_2 := 2
JCOP_VER_3_3 := 3
JCOP_VER_4_0 := 4

LOCAL_CFLAGS += -DJCOP_VER_3_1=$(JCOP_VER_3_1)
LOCAL_CFLAGS += -DJCOP_VER_3_2=$(JCOP_VER_3_2)
LOCAL_CFLAGS += -DJCOP_VER_3_3=$(JCOP_VER_3_3)
LOCAL_CFLAGS += -DJCOP_VER_4_0=$(JCOP_VER_4_0)

NFC_NXP_ESE:= TRUE
ifeq ($(NFC_NXP_ESE),TRUE)
LOCAL_CFLAGS += -DNFC_NXP_ESE=TRUE
LOCAL_CFLAGS += -DNFC_NXP_ESE_VER=$(JCOP_VER_4_0)
else
LOCAL_CFLAGS += -DNFC_NXP_ESE=FALSE
endif

#### Select the CHIP ####
ifeq ($(BOARD_NFC_CHIPSET),pn547)
NXP_CHIP_TYPE := $(PN547C2)
else
NXP_CHIP_TYPE := $(PN548C2)
endif

ifeq ($(NXP_CHIP_TYPE),$(PN547C2))
LOCAL_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN547C2
else ifeq ($(NXP_CHIP_TYPE),$(PN548C2))
LOCAL_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN548C2
else ifeq ($(NXP_CHIP_TYPE),$(PN551))
LOCAL_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN551
else ifeq ($(NXP_CHIP_TYPE),$(PN553))
LOCAL_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN553
endif

ifeq ($(NXP_CHIP_TYPE),$(PN553))
LOCAL_CFLAGS += -DJCOP_WA_ENABLE=FALSE
else
LOCAL_CFLAGS += -DJCOP_WA_ENABLE=TRUE
endif

NFC_POWER_MANAGEMENT:= TRUE
ifeq ($(NFC_POWER_MANAGEMENT),TRUE)
LOCAL_CFLAGS += -DNFC_POWER_MANAGEMENT=TRUE
else
LOCAL_CFLAGS += -DNFC_POWER_MANAGEMENT=FALSE
endif

ifeq ($(NFC_NXP_ESE),TRUE)
LOCAL_CFLAGS += -DNXP_LDR_SVC_VER_2=TRUE
else
LOCAL_CFLAGS += -DNXP_LDR_SVC_VER_2=FALSE
endif

LOCAL_SRC_FILES := $(call all-subdir-cpp-files) $(call all-subdir-c-files)

LOCAL_C_INCLUDES += \
    frameworks/native/include \
    libcore/include \
    $(NFA)/include \
    $(NFA)/brcm \
    $(NFC)/include \
    $(NFC)/brcm \
    $(NFC)/int \
    $(VOB_COMPONENTS)/hal/include \
    $(VOB_COMPONENTS)/hal/int \
    $(VOB_COMPONENTS)/include \
    $(VOB_COMPONENTS)/gki/ulinux \
    $(VOB_COMPONENTS)/gki/common

ifeq ($(NFC_NXP_ESE),TRUE)
LOCAL_C_INCLUDES += vendor/nxp-nfc/opensource/libnfc-nci/p61-jcop-kit/include

endif

LOCAL_SHARED_LIBRARIES := \
    libicuuc \
    libnativehelper \
    libcutils \
    libutils \
    liblog \
    libnqnfc-nci

ifeq ($(NFC_NXP_ESE),TRUE)
LOCAL_SHARED_LIBRARIES += libp61-jcop-kit
endif

#LOCAL_STATIC_LIBRARIES := libxml2
ifeq (true,$(TARGET_IS_64_BIT))
LOCAL_MULTILIB := 64
else
LOCAL_MULTILIB := 32
endif

LOCAL_MODULE := libnqnfc_nci_jni
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_OWNER := nxp
LOCAL_OVERRIDES_PACKAGES := libnfc_nci_jni

include $(BUILD_SHARED_LIBRARY)
