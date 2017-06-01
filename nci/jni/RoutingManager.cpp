/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2015 NXP Semiconductors
 * The original Work has been changed by NXP Semiconductors.
 *
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 *  Manage the listen-mode routing table.
 */

#include <cutils/log.h>
#include <ScopedLocalRef.h>
#include <JNIHelp.h>
#include "config.h"
#include "JavaClassConstants.h"
#include "RoutingManager.h"
#include "SecureElement.h"
#if(NXP_EXTNS == TRUE)
extern "C"{
#include "phNxpConfig.h"
#include "nfc_api.h"
#include "nfa_api.h"
}

extern INT32 gSeDiscoverycount;
extern SyncEvent gNfceeDiscCbEvent;

uint8_t nfcee_swp_discovery_status;
extern INT32 gActualSeCount;
extern INT32 gdisc_timeout;
extern UINT16 sCurrentSelectedUICCSlot;
static void LmrtRspTimerCb(union sigval);
#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE)
static jint getUiccRoute(jint uicc_slot);
#endif
int gUICCVirtualWiredProtectMask = 0;
int gEseVirtualWiredProtectMask = 0;
int gWiredModeRfFieldEnable = 0;
#endif
extern bool sHCEEnabled;

const JNINativeMethod RoutingManager::sMethods [] =
{
    {"doGetDefaultRouteDestination", "()I", (void*) RoutingManager::com_android_nfc_cardemulation_doGetDefaultRouteDestination},
    {"doGetDefaultOffHostRouteDestination", "()I", (void*) RoutingManager::com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination},
    {"doGetAidMatchingMode", "()I", (void*) RoutingManager::com_android_nfc_cardemulation_doGetAidMatchingMode},
    {"doGetAidMatchingPlatform", "()I", (void*) RoutingManager::com_android_nfc_cardemulation_doGetAidMatchingPlatform}
};

static UINT16 rdr_req_handling_timeout = 50;


#if((NFC_NXP_ESE == TRUE) && (NXP_EXTNS == TRUE) && (NXP_ESE_ETSI_READER_ENABLE == TRUE))
Rdr_req_ntf_info_t swp_rdr_req_ntf_info;
static IntervalTimer swp_rd_req_timer;
#endif

UINT16 lastcehandle = 0;

namespace android
{
    extern void checkforTranscation(UINT8 connEvent, void* eventData );
#if (NXP_EXTNS == TRUE)
#if (NXP_NFCC_EMPTY_DATA_PACKET == TRUE)
    extern bool nfcManager_sendEmptyDataMsg();
    extern bool gIsEmptyRspSentByHceFApk;
#endif
    extern UINT16 sRoutingBuffLen;
    extern bool  rfActivation;
#if (NXP_NFCEE_REMOVED_NTF_RECOVERY == TRUE)
    extern bool isNfcInitializationDone();
#endif
    extern void startRfDiscovery (bool isStart);
    extern bool isDiscoveryStarted();
    extern int getScreenState();
#if(NXP_NFCC_HCE_F == TRUE)
    extern bool nfcManager_getTransanctionRequest(int t3thandle, bool registerRequest);
#endif
#endif
}

#if (NXP_EXTNS == TRUE)
static RouteInfo_t gRouteInfo;
#endif

RoutingManager::RoutingManager ()
: mNativeData(NULL),
  mDefaultEe (NFA_HANDLE_INVALID),
  mHostListnTechMask (0),
  mUiccListnTechMask (0),
  mFwdFuntnEnable (true),
  mAddAid(0),
  mDefaultHCEFRspTimeout (5000)
{
    static const char fn [] = "RoutingManager::RoutingManager()";
    unsigned long num = 0;
    ALOGD ("%s:enter", fn);
    // Get the active SE
    if (GetNumValue("ACTIVE_SE", &num, sizeof(num)))
        mActiveSe = num;
    else
        mActiveSe = 0x00;
    // Get the active SE for Nfc-F
    if (GetNumValue("ACTIVE_SE_NFCF", &num, sizeof(num)))
        mActiveSeNfcF = num;
    else
        mActiveSeNfcF = 0x00;
    // Get the "default" route
    if (GetNumValue("DEFAULT_ISODEP_ROUTE", &num, sizeof(num)))
    {
#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE)
        if((num == 0xF4 || num == 0xF8) && sCurrentSelectedUICCSlot)
        {
            mDefaultEe = (sCurrentSelectedUICCSlot != 0x02) ? 0xF4 : 0xF8;
        }
        else
        {
            mDefaultEe = num;
        }
        ALOGD("%s: DEFAULT_ISODEP_ROUTE mDefaultEe : %d", fn, mDefaultEe);
#else
        mDefaultEe = num;
#endif
    }
    else
    {
        mDefaultEe = 0x00;
    }
    // Get the "default" route for Nfc-F
    if (GetNumValue("DEFAULT_NFCF_ROUTE", &num, sizeof(num)))
      mDefaultEeNfcF = num;
    else
      mDefaultEeNfcF = 0x00;
    // Get the default "off-host" route.  This is hard-coded at the Java layer
    // but we can override it here to avoid forcing Java changes.
    if (GetNumValue("DEFAULT_OFFHOST_ROUTE", &num, sizeof(num)))
        mOffHostEe = num;
    else
        mOffHostEe = 0x02;
    if (GetNumValue("AID_MATCHING_MODE", &num, sizeof(num)))
        mAidMatchingMode = num;
    else
        mAidMatchingMode = AID_MATCHING_EXACT_ONLY;
    if (GetNxpNumValue("AID_MATCHING_PLATFORM", &num, sizeof(num)))
        mAidMatchingPlatform = num;
    else
        mAidMatchingPlatform = AID_MATCHING_L;

    mSeTechMask = 0x00; //unused
    mNfcFOnDhHandle = NFA_HANDLE_INVALID;
    ALOGD ("%s:exit", fn);
}

int RoutingManager::mChipId = 0;
#if (NXP_EXTNS == TRUE)
#if (NXP_NFCEE_REMOVED_NTF_RECOVERY == TRUE)
bool recovery;
#endif
#endif

#if((NFC_NXP_ESE == TRUE) && (NXP_EXTNS == TRUE) && (NXP_ESE_ETSI_READER_ENABLE == TRUE))
void reader_req_event_ntf (union sigval);
#endif
RoutingManager::~RoutingManager ()
{
    NFA_EeDeregister (nfaEeCallback);
}

bool RoutingManager::initialize (nfc_jni_native_data* native)
{
    static const char fn [] = "RoutingManager::initialize()";
    unsigned long num = 0, tech = 0;
    mNativeData = native;
    UINT8 ActualNumEe = SecureElement::MAX_NUM_EE;
    tNFA_EE_INFO mEeInfo [ActualNumEe];

    ALOGD ("%s: enter", fn);
#if (NXP_EXTNS == TRUE)
    memset(&gRouteInfo, 0x00, sizeof(RouteInfo_t));
    nfcee_swp_discovery_status = SWP_DEFAULT;
    if ((GetNumValue(NAME_HOST_LISTEN_TECH_MASK, &tech, sizeof(tech))))
        mHostListnTechMask = tech;
    else
        mHostListnTechMask = 0x07;

    if ((GetNumValue(NAME_UICC_LISTEN_TECH_MASK, &tech, sizeof(tech))))
        mUiccListnTechMask = tech;
    else
        mUiccListnTechMask = 0x07;

    if ((GetNumValue(NAME_NXP_FWD_FUNCTIONALITY_ENABLE, &tech, sizeof(tech))))
        mFwdFuntnEnable = tech;
    else
        mFwdFuntnEnable = 0x01;

    if (GetNxpNumValue (NAME_NXP_DEFAULT_SE, (void*)&num, sizeof(num)))
        mDefaultEe = num;
    else
        mDefaultEe = 0x02;

    if (GetNxpNumValue (NAME_NXP_ENABLE_ADD_AID, (void*)&num, sizeof(num)))
        mAddAid = num;
    else
        mAddAid = 0x01;

#if (NFC_NXP_ESE == TRUE && (NFC_NXP_CHIP_TYPE != PN547C2))
    if (GetNxpNumValue (NAME_NXP_ESE_WIRED_PRT_MASK, (void*)&num, sizeof(num)))
        gEseVirtualWiredProtectMask = num;
    else
        gEseVirtualWiredProtectMask = 0x00;

    if (GetNxpNumValue (NAME_NXP_UICC_WIRED_PRT_MASK, (void*)&num, sizeof(num)))
        gUICCVirtualWiredProtectMask = num;
    else
        gUICCVirtualWiredProtectMask = 0x00;

    if (GetNxpNumValue (NAME_NXP_WIRED_MODE_RF_FIELD_ENABLE, (void*)&num, sizeof(num)))
        gWiredModeRfFieldEnable = num;
    else
        gWiredModeRfFieldEnable = 0x00;
#endif
#if(NXP_ESE_FELICA_CLT == TRUE)
    if (GetNxpNumValue (NAME_DEFAULT_FELICA_CLT_ROUTE, (void*)&num, sizeof(num)))
    {
#if((NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE) && (NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE))
        if((num == 0x02 || num == 0x03) && sCurrentSelectedUICCSlot)
        {
            mDefaultTechFSeID = getUiccRoute(sCurrentSelectedUICCSlot);
        }
        else
        {
            mDefaultTechFSeID = ( (num == 0x01) ? ROUTE_LOC_ESE_ID : ((num == 0x02) ? ROUTE_LOC_UICC1_ID : ROUTE_LOC_UICC2_ID) );
        }
#else
        mDefaultTechFSeID = ( (num == 0x01) ? ROUTE_LOC_ESE_ID : ((num == 0x02) ? ROUTE_LOC_UICC1_ID : ROUTE_LOC_UICC2_ID) );
#endif
    }
#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE)
    else
    {
        mDefaultTechFSeID = getUiccRoute(sCurrentSelectedUICCSlot);
    }
#endif

    if (GetNxpNumValue (NAME_DEFAULT_FELICA_CLT_PWR_STATE, (void*)&num, sizeof(num)))
        mDefaultTechFPowerstate = num;
    else
        mDefaultTechFPowerstate = 0x1F;
#else
    mDefaultTechFSeID = ROUTE_LOC_UICC1_ID;
    mDefaultTechFPowerstate = 0x1F;
#endif
    if (GetNxpNumValue (NAME_NXP_HCEF_CMD_RSP_TIMEOUT_VALUE, (void*)&num, sizeof(num)))
    {
        if(num > 0)
        {
            mDefaultHCEFRspTimeout = num;
        }
    }
#endif
    if ((GetNxpNumValue(NAME_NXP_NFC_CHIP, &num, sizeof(num))))
    {
        mChipId = num;
    }

    tNFA_STATUS nfaStat;
    {
        SyncEventGuard guard (mEeRegisterEvent);
        ALOGD ("%s: try ee register", fn);
        nfaStat = NFA_EeRegister (nfaEeCallback);
        if (nfaStat != NFA_STATUS_OK)
        {
            ALOGE ("%s: fail ee register; error=0x%X", fn, nfaStat);
            return false;
        }
        mEeRegisterEvent.wait ();
    }

#if(NXP_EXTNS == TRUE)
    if (mHostListnTechMask)
    {
        // Tell the host-routing to only listen on Nfc-A/Nfc-B
        nfaStat = NFA_CeSetIsoDepListenTech(mHostListnTechMask & 0xB);
        if (nfaStat != NFA_STATUS_OK)
            ALOGE ("Failed to configure CE IsoDep technologies");

        // Tell the host-routing to only listen on Nfc-A/Nfc-B
        nfaStat = NFA_CeRegisterAidOnDH (NULL, 0, stackCallback);
        if (nfaStat != NFA_STATUS_OK)
            ALOGE ("Failed to register wildcard AID for DH");
    }
    mRxDataBuffer.clear ();
#else
//    setDefaultRouting();
#endif

    if ((nfaStat = NFA_AllEeGetInfo (&ActualNumEe, mEeInfo)) != NFA_STATUS_OK)
    {
        ALOGE ("%s: fail get info; error=0x%X", fn, nfaStat);
        ActualNumEe = 0;
    }
    else
    {
        //gSeDiscoverycount = ActualNumEe;
        SecureElement::getInstance().updateNfceeDiscoverInfo();
        ALOGD ("%s:gSeDiscoverycount=0x%lX;", __FUNCTION__, gSeDiscoverycount);
#if 0
        if(mChipId == 0x02 || mChipId == 0x04)
        {
            for(int xx = 0; xx <  ActualNumEe; xx++)
            {
                ALOGE("xx=%d, ee_handle=0x0%x, status=0x0%x", xx, mEeInfo[xx].ee_handle,mEeInfo[xx].ee_status);
                if ((mEeInfo[xx].ee_handle == 0x4C0) &&
                        (mEeInfo[xx].ee_status == 0x02))
                {
                    ee_removed_disc_ntf_handler(mEeInfo[xx].ee_handle, mEeInfo[xx].ee_status);
                    break;
                }
            }
        }
#endif
    }

#if((NFC_NXP_ESE == TRUE) && (NXP_EXTNS == TRUE) && (NXP_ESE_ETSI_READER_ENABLE == TRUE))
    swp_rdr_req_ntf_info.mMutex.lock();
    memset(&(swp_rdr_req_ntf_info.swp_rd_req_info),0x00,sizeof(rd_swp_req_t));
    memset(&(swp_rdr_req_ntf_info.swp_rd_req_current_info),0x00,sizeof(rd_swp_req_t));
    swp_rdr_req_ntf_info.swp_rd_req_current_info.src = NFA_HANDLE_INVALID;
    swp_rdr_req_ntf_info.swp_rd_req_info.src = NFA_HANDLE_INVALID;
    swp_rdr_req_ntf_info.swp_rd_state = STATE_SE_RDR_MODE_STOPPED;
    swp_rdr_req_ntf_info.mMutex.unlock();
#endif

    printMemberData();

    ALOGD ("%s: exit", fn);
    return true;
}
#if(NXP_EXTNS == TRUE)
void RoutingManager::registerProtoRouteEntry(tNFA_HANDLE     ee_handle,
                                         tNFA_PROTOCOL_MASK  protocols_switch_on,
                                         tNFA_PROTOCOL_MASK  protocols_switch_off,
                                         tNFA_PROTOCOL_MASK  protocols_battery_off,
                                         tNFA_PROTOCOL_MASK  protocols_screen_lock,
                                         tNFA_PROTOCOL_MASK  protocols_screen_off
                                         )
{
    static const char fn [] = "RoutingManager::registerProtoRouteEntry";
    bool new_entry = true;
    UINT8 i = 0;
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

    if(gRouteInfo.num_entries == 0)
    {
        ALOGD ("%s: enter, first entry :%x", fn, ee_handle);
        gRouteInfo.protoInfo[0].ee_handle = ee_handle;
        gRouteInfo.protoInfo[0].protocols_switch_on = protocols_switch_on;
        gRouteInfo.protoInfo[0].protocols_switch_off = protocols_switch_off;
        gRouteInfo.protoInfo[0].protocols_battery_off = protocols_battery_off;
        gRouteInfo.protoInfo[0].protocols_screen_lock = protocols_screen_lock;
        gRouteInfo.protoInfo[0].protocols_screen_off = protocols_screen_off;
        gRouteInfo.num_entries = 1;
    }
    else
    {
        for (i = 0;i < gRouteInfo.num_entries; i++)
        {
            if(gRouteInfo.protoInfo[i].ee_handle == ee_handle)
            {
                ALOGD ("%s: enter, proto handle match found :%x", fn, ee_handle);
                gRouteInfo.protoInfo[i].protocols_switch_on |= protocols_switch_on;
                gRouteInfo.protoInfo[i].protocols_switch_off |= protocols_switch_off;
                gRouteInfo.protoInfo[i].protocols_battery_off |= protocols_battery_off;
                gRouteInfo.protoInfo[i].protocols_screen_lock |= protocols_screen_lock;
                gRouteInfo.protoInfo[i].protocols_screen_off |= protocols_screen_off;
                new_entry = false;
                break;
            }
        }
        if(new_entry)
        {
            ALOGD ("%s: enter,new proto handle entry :%x", fn, ee_handle);
            i = gRouteInfo.num_entries;
            gRouteInfo.protoInfo[i].ee_handle = ee_handle;
            gRouteInfo.protoInfo[i].protocols_switch_on = protocols_switch_on;
            gRouteInfo.protoInfo[i].protocols_switch_off = protocols_switch_off;
            gRouteInfo.protoInfo[i].protocols_battery_off = protocols_battery_off;
            gRouteInfo.protoInfo[i].protocols_screen_lock = protocols_screen_lock;
            gRouteInfo.protoInfo[i].protocols_screen_off = protocols_screen_off;
            gRouteInfo.num_entries++;
        }
    }
    for (i = 0;i < gRouteInfo.num_entries; i++)
    {
        nfaStat = NFA_EeSetDefaultProtoRouting (gRouteInfo.protoInfo[i].ee_handle,
                                                gRouteInfo.protoInfo[i].protocols_switch_on,
                                                gRouteInfo.protoInfo[i].protocols_switch_off,
                                                gRouteInfo.protoInfo[i].protocols_battery_off,
                                                gRouteInfo.protoInfo[i].protocols_screen_lock,
                                                gRouteInfo.protoInfo[i].protocols_screen_off);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("tech routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default tech routing");
        }
    }
}
#endif

RoutingManager& RoutingManager::getInstance ()
{
    static RoutingManager manager;
    return manager;
}

void RoutingManager::cleanRouting()
{
    tNFA_STATUS nfaStat;
    //tNFA_HANDLE seHandle = NFA_HANDLE_INVALID;        /*commented to eliminate unused variable warning*/
    tNFA_HANDLE ee_handleList[SecureElement::MAX_NUM_EE];
    UINT8 i, count;
   // static const char fn [] = "SecureElement::cleanRouting";   /*commented to eliminate unused variable warning*/
    SyncEventGuard guard (mRoutingEvent);
    SecureElement::getInstance().getEeHandleList(ee_handleList, &count);
    if (count > SecureElement::MAX_NUM_EE) {
        count = SecureElement::MAX_NUM_EE;
        ALOGD("Count is more than SecureElement::MAX_NUM_EE,Forcing to SecureElement::MAX_NUM_EE");
    }
    for ( i = 0; i < count; i++)
    {
#if(NXP_EXTNS == TRUE)
        nfaStat =  NFA_EeSetDefaultTechRouting(ee_handleList[i],0,0,0,0,0);
#else
        nfaStat =  NFA_EeSetDefaultTechRouting(ee_handleList[i],0,0,0);
#endif
        if(nfaStat == NFA_STATUS_OK)
        {
            mRoutingEvent.wait ();
        }
#if(NXP_EXTNS == TRUE)
        nfaStat =  NFA_EeSetDefaultProtoRouting(ee_handleList[i],0,0,0,0,0);
#else
        nfaStat =  NFA_EeSetDefaultProtoRouting(ee_handleList[i],0,0,0);
#endif
        if(nfaStat == NFA_STATUS_OK)
        {
            mRoutingEvent.wait ();
        }
    }
    //clean HOST
#if(NXP_EXTNS == TRUE)
    nfaStat =  NFA_EeSetDefaultTechRouting(NFA_EE_HANDLE_DH,0,0,0,0,0);
#else
    nfaStat =  NFA_EeSetDefaultTechRouting(NFA_EE_HANDLE_DH,0,0,0);
#endif
    if(nfaStat == NFA_STATUS_OK)
    {
        mRoutingEvent.wait ();
    }
#if(NXP_EXTNS == TRUE)
    nfaStat =  NFA_EeSetDefaultProtoRouting(NFA_EE_HANDLE_DH,0,0,0,0,0);
#else
    nfaStat =  NFA_EeSetDefaultProtoRouting(NFA_EE_HANDLE_DH,0,0,0);
#endif
    if(nfaStat == NFA_STATUS_OK)
    {
        mRoutingEvent.wait ();
    }
#if 0
    /*commented to avoid send LMRT command twice*/
    nfaStat = NFA_EeUpdateNow();
    if (nfaStat != NFA_STATUS_OK)
        ALOGE("Failed to commit routing configuration");
#endif
}

#if(NXP_EXTNS == TRUE)
void RoutingManager::setRouting(bool isHCEEnabled)
{
    tNFA_STATUS nfaStat;
    tNFA_HANDLE defaultHandle = NFA_HANDLE_INVALID;
    tNFA_HANDLE ee_handleList[SecureElement::MAX_NUM_EE];
    UINT8 i = 0, count = 0;
    static const char fn [] = "SecureElement::setRouting";
    unsigned long num = 0;

    if ((GetNumValue(NAME_UICC_LISTEN_TECH_MASK, &num, sizeof(num))))
    {
        ALOGE ("%s:UICC_LISTEN_MASK=0x0%lu;", __FUNCTION__, num);
    }

    if (isHCEEnabled)
    {
        defaultHandle = NFA_EE_HANDLE_DH;
    }
    else
    {
        SecureElement::getInstance().getEeHandleList(ee_handleList, &count);
        for ( ; i < count; i++)
        {
            if (defaultHandle == NFA_HANDLE_INVALID)
            {
                defaultHandle = ee_handleList[i];
                break;
            }
        }
    }
    ALOGD ("%s: defaultHandle %u = 0x%X", fn, i, defaultHandle);

    if (defaultHandle != NFA_HANDLE_INVALID)
    {
        {
            SyncEventGuard guard (mRoutingEvent);

            tNFA_STATUS status =  NFA_EeSetDefaultTechRouting(0x402,0,0,0,0,0); //UICC clear

            if(status == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
        }

        {
            SyncEventGuard guard (mRoutingEvent);

            tNFA_STATUS status =  NFA_EeSetDefaultProtoRouting(0x402,0,0,0,0,0); //UICC clear

            if(status == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
        }

        {
            SyncEventGuard guard (mRoutingEvent);

            tNFA_STATUS status =  NFA_EeSetDefaultTechRouting(0x4C0,0,0,0,0,0); //SMX clear

            if(status == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
        }

        {
            SyncEventGuard guard (mRoutingEvent);

            tNFA_STATUS status =  NFA_EeSetDefaultProtoRouting(0x4C0,0,0,0,0,0); //SMX clear

            if(status == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
        }

        {
            SyncEventGuard guard (mRoutingEvent);

            tNFA_STATUS status =  NFA_EeSetDefaultTechRouting(0x400,0,0,0,0,0); //HOST clear

            if(status == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
        }

        {
            SyncEventGuard guard (mRoutingEvent);

            tNFA_STATUS status =  NFA_EeSetDefaultProtoRouting(0x400,0,0,0,0,0); //HOST clear

            if(status == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
        }
        if(defaultHandle == NFA_EE_HANDLE_DH)
        {
            SyncEventGuard guard (mRoutingEvent);
            // Default routing for NFC-A technology
            if(mCeRouteStrictDisable == 0x01)
            {
                nfaStat = NFA_EeSetDefaultTechRouting (defaultHandle, 0x01, 0, 0, 0x01, 0);
            }else
            {
                nfaStat = NFA_EeSetDefaultTechRouting (defaultHandle, 0x01, 0, 0, 0, 0);
            }

            if (nfaStat == NFA_STATUS_OK)
                mRoutingEvent.wait ();
            else
                ALOGE ("Fail to set default tech routing");
        }
        else
        {
            SyncEventGuard guard (mRoutingEvent);
            // Default routing for NFC-A technology
            if(mCeRouteStrictDisable == 0x01)
            {
                nfaStat = NFA_EeSetDefaultTechRouting (defaultHandle, num, num, num, num, num);
            }else{
                nfaStat = NFA_EeSetDefaultTechRouting (defaultHandle, num, num, num, 0, 0);
            }
            if (nfaStat == NFA_STATUS_OK)
                mRoutingEvent.wait ();
            else
                ALOGE ("Fail to set default tech routing");
        }

        if(defaultHandle == NFA_EE_HANDLE_DH)
        {
            SyncEventGuard guard (mRoutingEvent);
            // Default routing for IsoDep protocol
            if(mCeRouteStrictDisable == 0x01)
            {
                nfaStat = NFA_EeSetDefaultProtoRouting(defaultHandle, NFA_PROTOCOL_MASK_ISO_DEP, 0, 0, NFA_PROTOCOL_MASK_ISO_DEP, 0);
            }
            else
            {
                nfaStat = NFA_EeSetDefaultProtoRouting(defaultHandle, NFA_PROTOCOL_MASK_ISO_DEP, 0, 0, 0 ,0);
            }
            if (nfaStat == NFA_STATUS_OK)
                mRoutingEvent.wait ();
            else
                ALOGE ("Fail to set default proto routing");
        }
        else
        {
            SyncEventGuard guard (mRoutingEvent);
            // Default routing for IsoDep protocol
            if(mCeRouteStrictDisable == 0x01)
            {
                nfaStat = NFA_EeSetDefaultProtoRouting(defaultHandle,
                                                       NFA_PROTOCOL_MASK_ISO_DEP,
                                                       NFA_PROTOCOL_MASK_ISO_DEP,
                                                       NFA_PROTOCOL_MASK_ISO_DEP,
                                                       NFA_PROTOCOL_MASK_ISO_DEP,
                                                       NFA_PROTOCOL_MASK_ISO_DEP);
            }
            else
            {
                nfaStat = NFA_EeSetDefaultProtoRouting(defaultHandle,
                                                       NFA_PROTOCOL_MASK_ISO_DEP,
                                                       NFA_PROTOCOL_MASK_ISO_DEP,
                                                       NFA_PROTOCOL_MASK_ISO_DEP,
                                                       0,
                                                       0);
            }
            if (nfaStat == NFA_STATUS_OK)
                mRoutingEvent.wait ();
            else
                ALOGE ("Fail to set default proto routing");
        }

        if(defaultHandle != NFA_EE_HANDLE_DH)
        {
            {
                SyncEventGuard guard (SecureElement::getInstance().mUiccListenEvent);
                nfaStat = NFA_CeConfigureUiccListenTech (defaultHandle, 0x00);
                if (nfaStat == NFA_STATUS_OK)
                {
                    SecureElement::getInstance().mUiccListenEvent.wait ();
                }
                else
                    ALOGE ("fail to start UICC listen");
            }

            {
                SyncEventGuard guard (SecureElement::getInstance().mUiccListenEvent);
                nfaStat = NFA_CeConfigureUiccListenTech (defaultHandle, (num & 0x07));
                if(nfaStat == NFA_STATUS_OK)
                {
                    SecureElement::getInstance().mUiccListenEvent.wait ();
                }
                else
                    ALOGE ("fail to start UICC listen");
            }
        }
    }

    // Commit the routing configuration
    nfaStat = NFA_EeUpdateNow();
    if (nfaStat != NFA_STATUS_OK)
        ALOGE("Failed to commit routing configuration");
}
/*This function takes the default AID route, protocol(ISO-DEP) route and Tech(A&B) route as arguments in following format
* -----------------------------------------------------------------------------------------------------------
* | RFU(TechA/B) | RouteLocBit1 | RouteLocBit0 | ScreenOff | ScreenLock | BatteryOff | SwitchOff | SwitchOn |
* -----------------------------------------------------------------------------------------------------------
* Route location is set as below
* ----------------------------------------------
* | RouteLocBit1 | RouteLocBit0 | RouteLocation|
* ----------------------------------------------
* |       0      |      0       |    Host      |
* ----------------------------------------------
* |       0      |      1       |    eSE       |
* ----------------------------------------------
* |       1      |      0       |    Uicc1     |
* ----------------------------------------------
* |       1      |      1       |    Uicc2     | => Valid if DYNAMIC_DUAL_UICC is enabled
* ----------------------------------------------
* Based on these parameters, this function creates the protocol route entries/ technology route entries
* which are required to be pushed to listen mode routing table using NFA_EeSetDefaultProtoRouting/TechRouting
*/
bool RoutingManager::setDefaultRoute(const UINT8 defaultRoute, const UINT8 protoRoute, const UINT8 techRoute)
{
    static const char fn []   = "RoutingManager::setDefaultRoute";
    tNFA_STATUS       nfaStat = NFA_STATUS_FAILED;

    ALOGD ("%s: enter; defaultRoute:0x%2X protoRoute:0x%2X TechRoute:0x%2X HostListenMask:0x%X", fn, defaultRoute, protoRoute, techRoute, mHostListnTechMask);

    extractRouteLocationAndPowerStates(defaultRoute,protoRoute,techRoute);

    if (mHostListnTechMask)
    {
       nfaStat = NFA_CeSetIsoDepListenTech(mHostListnTechMask & 0xB);
       if (nfaStat != NFA_STATUS_OK)
           ALOGE ("Failed to configure CE IsoDep technologies");
       nfaStat = NFA_CeRegisterAidOnDH (NULL, 0, stackCallback);
       if (nfaStat != NFA_STATUS_OK)
           ALOGE ("Failed to register wildcard AID for DH");
    }

    checkProtoSeID();

    initialiseTableEntries ();

    compileProtoEntries ();

    consolidateProtoEntries ();

    setProtoRouting ();

    compileTechEntries ();

    consolidateTechEntries ();

    setTechRouting ();

    configureOffHostNfceeTechMask();

    ALOGD ("%s: exit", fn);

    return true;
}

void RoutingManager::setCeRouteStrictDisable(UINT32 state)
{
    ALOGD ("%s: mCeRouteScreenLock = 0x%lX", __FUNCTION__, state);
    mCeRouteStrictDisable = state;
}

void RoutingManager::printMemberData()
{
    ALOGD("%s: ACTIVE_SE = 0x%0X", __FUNCTION__, mActiveSe);
    ALOGD("%s: ACTIVE_SE_NFCF = 0x%0X", __FUNCTION__, mActiveSeNfcF);
    ALOGD("%s: AID_MATCHING_MODE = 0x%0X", __FUNCTION__, mAidMatchingMode);
    ALOGD("%s: DEFAULT_NFCF_ROUTE = 0x%0X", __FUNCTION__, mDefaultEeNfcF);
    ALOGD("%s: DEFAULT_ISODEP_ROUTE = 0x%0X", __FUNCTION__, mDefaultEe);
    ALOGD("%s: DEFAULT_OFFHOST_ROUTE = 0x%0X", __FUNCTION__, mOffHostEe);
    ALOGD("%s: AID_MATCHING_PLATFORM = 0x%0X", __FUNCTION__, mAidMatchingPlatform);
    ALOGD("%s: HOST_LISTEN_TECH_MASK = 0x%0X;", __FUNCTION__, mHostListnTechMask);
    ALOGD("%s: UICC_LISTEN_TECH_MASK = 0x%0X;", __FUNCTION__, mUiccListnTechMask);
    ALOGD("%s: DEFAULT_FELICA_CLT_ROUTE = 0x%0lX;", __FUNCTION__, mDefaultTechFSeID);
    ALOGD("%s: DEFAULT_FELICA_CLT_PWR_STATE = 0x%0lX;", __FUNCTION__, mDefaultTechFPowerstate);

    ALOGD("%s: NXP_NFC_CHIP = 0x%0X;", __FUNCTION__, mChipId);
    ALOGD("%s: NXP_DEFAULT_SE = 0x%0X;", __FUNCTION__, mDefaultEe);
    ALOGD("%s: NXP_ENABLE_ADD_AID = 0x%0lX;", __FUNCTION__, mAddAid);
    ALOGD("%s: NXP_ESE_WIRED_PRT_MASK = 0x%0X;", __FUNCTION__, gEseVirtualWiredProtectMask);
    ALOGD("%s: NXP_UICC_WIRED_PRT_MASK = 0x%0X;", __FUNCTION__, gUICCVirtualWiredProtectMask);
    ALOGD("%s: NXP_FWD_FUNCTIONALITY_ENABLE = 0x%0X;", __FUNCTION__, mFwdFuntnEnable);
    ALOGD("%s: NXP_WIRED_MODE_RF_FIELD_ENABLE = 0x%0X;", __FUNCTION__, gWiredModeRfFieldEnable);

}
/* extract route location and power states in defaultRoute,protoRoute & techRoute in the following format
 * -----------------------------------------------------------------------------------------------------------
 * | RFU(TechA/B) | RouteLocBit1 | RouteLocBit0 | ScreenOff | ScreenLock | BatteryOff | SwitchOff | SwitchOn |
 * -----------------------------------------------------------------------------------------------------------
 * to mDefaultIso7816SeID & mDefaultIso7816Powerstate
 *    mDefaultIsoDepSeID  & mDefaultIsoDepPowerstate
 *    mDefaultTechASeID   & mDefaultTechAPowerstate
 */
void RoutingManager::extractRouteLocationAndPowerStates(const UINT8 defaultRoute, const UINT8 protoRoute, const UINT8 techRoute)
{
    static const char fn []   = "RoutingManager::extractRouteLocationAndPowerStates";
    mDefaultIso7816SeID = ((((defaultRoute & 0x60) >> 5) == 0x00) ? ROUTE_LOC_HOST_ID : ((((defaultRoute & 0x60)>>5 )== 0x01 ) ? ROUTE_LOC_ESE_ID : getUiccRouteLocId(defaultRoute)));
    mDefaultIso7816Powerstate = defaultRoute & 0x1F;
    ALOGD ("%s:mDefaultIso7816SeID:0x%2lX mDefaultIso7816Powerstate:0x%lX", fn, mDefaultIso7816SeID, mDefaultIso7816Powerstate);
    mDefaultIsoDepSeID = ((((protoRoute & 0x60) >> 5) == 0x00) ? ROUTE_LOC_HOST_ID : ((((protoRoute & 0x60)>>5 )== 0x01 ) ? ROUTE_LOC_ESE_ID : getUiccRouteLocId(protoRoute)));
    mDefaultIsoDepPowerstate = protoRoute & 0x1F;
    ALOGD ("%s:mDefaultIsoDepSeID:0x%2lX mDefaultIsoDepPowerstate:0x%2lX", fn, mDefaultIsoDepSeID,mDefaultIsoDepPowerstate);
    mDefaultTechASeID = ((((techRoute & 0x60) >> 5) == 0x00) ? ROUTE_LOC_HOST_ID : ((((techRoute & 0x60)>>5 )== 0x01 ) ? ROUTE_LOC_ESE_ID : getUiccRouteLocId(techRoute)));
    mDefaultTechAPowerstate = techRoute & 0x1F;
    ALOGD ("%s:mDefaultTechASeID:0x%2lX mDefaultTechAPowerstate:0x%2lX", fn, mDefaultTechASeID,mDefaultTechAPowerstate);

}
/* Based on the features enabled :- NXP_NFCC_DYNAMIC_DUAL_UICC, NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH & NFC_NXP_STAT_DUAL_UICC_EXT_SWITCH,
 * Calculate the UICC route location ID.
 * For DynamicDualUicc,Route location is based on the user configuration(6th & 7th bit) of route
 * For StaticDualUicc without External Switch(with DynamicDualUicc enabled), Route location is based on user selection from selectUicc() API
 * For StaticDualUicc(With External Switch), Route location is always ROUTE_LOC_UICC1_ID
 */
UINT16 RoutingManager::getUiccRouteLocId(const UINT8 route)
{
#if((NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE) && (NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE))
    return getUiccRoute(sCurrentSelectedUICCSlot);
#elif(NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE)
    return ((((route & 0x60)>>5 )== 0x02 ) ? ROUTE_LOC_UICC1_ID : ROUTE_LOC_UICC2_ID);
#else /*#if (NFC_NXP_STAT_DUAL_UICC_EXT_SWITCH == TRUE)*/
    return ROUTE_LOC_UICC1_ID;
#endif
}

/* To check whether the route location for ISO-DEP protocol defined by user in config file is actually connected or not
 * If not connected then set it to HOST by default*/
void RoutingManager::checkProtoSeID(void)
{
    static const char fn []                         = "RoutingManager::checkProtoSeID";
    UINT8             isDefaultIsoDepSeIDPresent    = 0;
    tNFA_HANDLE       ActDevHandle                  = NFA_HANDLE_INVALID;
    unsigned long     check_default_proto_se_id_req = 0;

    ALOGD ("%s: enter", fn);

    if (GetNxpNumValue(NAME_CHECK_DEFAULT_PROTO_SE_ID, &check_default_proto_se_id_req, sizeof(check_default_proto_se_id_req)))
    {
        ALOGD("%s: CHECK_DEFAULT_PROTO_SE_ID - 0x%2lX ",fn,check_default_proto_se_id_req);
    }
    else
    {
        ALOGE("%s: CHECK_DEFAULT_PROTO_SE_ID not defined. Taking default value - 0x%2lX",fn,check_default_proto_se_id_req);
    }

    if(check_default_proto_se_id_req == 0x01)
    {
        UINT8 count,seId=0;
        tNFA_HANDLE ee_handleList[SecureElement::MAX_NUM_EE];
        SecureElement::getInstance().getEeHandleList(ee_handleList, &count);
        ALOGD ("%s: count : %d", fn, count);
        for (int  i = 0; ((count != 0 ) && (i < count)); i++)
        {
            seId = SecureElement::getInstance().getGenericEseId(ee_handleList[i]);
            ALOGD ("%s: seId : %d", fn, seId);
            ActDevHandle = SecureElement::getInstance().getEseHandleFromGenericId(seId);
            ALOGD ("%s: ActDevHandle : 0x%X", fn, ActDevHandle);
            if (mDefaultIsoDepSeID == ActDevHandle)
            {
                isDefaultIsoDepSeIDPresent = 1;
                break;
            }
        }

        ALOGD ("%s:isDefaultIsoDepSeIDPresent:0x%X", fn, isDefaultIsoDepSeIDPresent);
        if(!isDefaultIsoDepSeIDPresent)
        {
            mDefaultIsoDepSeID = ROUTE_LOC_HOST_ID;
            mDefaultIsoDepPowerstate = PWR_SWTCH_ON_SCRN_UNLCK_MASK | PWR_SWTCH_ON_SCRN_LOCK_MASK;
        }
    }

    ALOGD ("%s: exit", fn);
}

void RoutingManager::configureOffHostNfceeTechMask(void)
{
    static const char fn []           = "RoutingManager::configureOffHostNfceeTechMask";
    tNFA_STATUS       nfaStat         = NFA_STATUS_FAILED;
    UINT8             seId            = 0x00;
    UINT8             count           = 0x00;
    tNFA_HANDLE       preferredHandle = ROUTE_LOC_UICC1_ID;
    tNFA_HANDLE       defaultHandle   = NFA_HANDLE_INVALID;
    tNFA_HANDLE       ee_handleList[SecureElement::MAX_NUM_EE];

    ALOGD ("%s: enter", fn);

    if (mDefaultEe & SecureElement::ESE_ID) //eSE
    {
        preferredHandle = ROUTE_LOC_ESE_ID;
    }
    else if (mDefaultEe & SecureElement::UICC_ID) //UICC
    {
        preferredHandle = ROUTE_LOC_UICC1_ID;
    }
#if((NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE) && (NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE))
    else if (mDefaultEe & SecureElement::UICC2_ID) //UICC
    {
        preferredHandle = ROUTE_LOC_UICC2_ID;
    }
#endif

    SecureElement::getInstance().getEeHandleList(ee_handleList, &count);

    for (UINT8 i = 0; ((count != 0 ) && (i < count)); i++)
    {
        seId = SecureElement::getInstance().getGenericEseId(ee_handleList[i]);
        defaultHandle = SecureElement::getInstance().getEseHandleFromGenericId(seId);
        ALOGD ("%s: ee_handleList[%d] : 0x%X", fn, i,ee_handleList[i]);
        if (preferredHandle == defaultHandle)
        {
            break;
        }
        defaultHandle   = NFA_HANDLE_INVALID;
    }

    if((defaultHandle != NFA_HANDLE_INVALID)  &&  (0 != mUiccListnTechMask))
    {
        {
            SyncEventGuard guard (SecureElement::getInstance().mUiccListenEvent);
            nfaStat = NFA_CeConfigureUiccListenTech (defaultHandle, 0x00);
            if (nfaStat == NFA_STATUS_OK)
            {
                 SecureElement::getInstance().mUiccListenEvent.wait ();
            }
            else
                 ALOGE ("fail to start UICC listen");
        }
        {
            SyncEventGuard guard (SecureElement::getInstance().mUiccListenEvent);
            nfaStat = NFA_CeConfigureUiccListenTech (defaultHandle, (mUiccListnTechMask & 0x07));
            if(nfaStat == NFA_STATUS_OK)
            {
                 SecureElement::getInstance().mUiccListenEvent.wait ();
            }
            else
                 ALOGE ("fail to start UICC listen");
        }
    }

    ALOGD ("%s: exit", fn);
}

void RoutingManager::initialiseTableEntries(void)
{
    static const char fn [] = "RoutingManager::initialiseTableEntries";

    ALOGD ("%s: enter", fn);

    /* Defined Protocol Masks
    * T1T      0x01
    * T2T      0x02
    * T3T      0x04
    * ISO-DEP  0x08
    * NFC-DEP  0x10
    * ISO-7816 0x20
    */

    mProtoTableEntries[PROTO_T3T_IDX].protocol     = NFA_PROTOCOL_MASK_T3T;
    mProtoTableEntries[PROTO_ISODEP_IDX].protocol  = NFA_PROTOCOL_MASK_ISO_DEP;
    mProtoTableEntries[PROTO_ISO7816_IDX].protocol = NFC_PROTOCOL_MASK_ISO7816;

    mTechTableEntries[TECH_A_IDX].technology       = NFA_TECHNOLOGY_MASK_A;
    mTechTableEntries[TECH_B_IDX].technology       = NFA_TECHNOLOGY_MASK_B;
    mTechTableEntries[TECH_F_IDX].technology       = NFA_TECHNOLOGY_MASK_F;

    for(int xx = 0; xx < MAX_PROTO_ENTRIES; xx++)
    {
        mProtoTableEntries[xx].routeLoc = mTechTableEntries[xx].routeLoc = 0x00;
        mProtoTableEntries[xx].power    = mTechTableEntries[xx].power    = 0x00;
        mProtoTableEntries[xx].enable   = mTechTableEntries[xx].enable   = FALSE;
    }

    mLmrtEntries[ROUTE_LOC_HOST_ID_IDX].nfceeID    = ROUTE_LOC_HOST_ID;
    mLmrtEntries[ROUTE_LOC_ESE_ID_IDX].nfceeID     = ROUTE_LOC_ESE_ID;
    mLmrtEntries[ROUTE_LOC_UICC1_ID_IDX].nfceeID   = ROUTE_LOC_UICC1_ID;
    mLmrtEntries[ROUTE_LOC_UICC2_ID_IDX].nfceeID   = ROUTE_LOC_UICC2_ID;

    /*Initialize the table for all route location nfceeID*/
    for(int xx=0;xx<MAX_ROUTE_LOC_ENTRIES;xx++)
    {
        mLmrtEntries[xx].proto_switch_on   = mLmrtEntries[xx].tech_switch_on   = 0x00;
        mLmrtEntries[xx].proto_switch_off  = mLmrtEntries[xx].tech_switch_off  = 0x00;
        mLmrtEntries[xx].proto_battery_off = mLmrtEntries[xx].tech_battery_off = 0x00;
        mLmrtEntries[xx].proto_screen_lock = mLmrtEntries[xx].tech_screen_lock = 0x00;
        mLmrtEntries[xx].proto_screen_off  = mLmrtEntries[xx].tech_screen_off  = 0x00;
    }
    /*Get all the technologies supported by all the execution environments*/
     mTechSupportedByEse   = SecureElement::getInstance().getSETechnology(ROUTE_LOC_ESE_ID);
     mTechSupportedByUicc1 = SecureElement::getInstance().getSETechnology(ROUTE_LOC_UICC1_ID);
     mTechSupportedByUicc2 = SecureElement::getInstance().getSETechnology(ROUTE_LOC_UICC2_ID);
     ALOGD ("%s: exit; mTechSupportedByEse:0x%0lX mTechSupportedByUicc1:0x%0lX mTechSupportedByUicc2:0x%0lX", fn, mTechSupportedByEse, mTechSupportedByUicc1, mTechSupportedByUicc2);
}

/* Compilation of Proto Table entries strictly based on config file parameters
 * Each entry in proto table consistes of route location, protocol and power state
 * */
void RoutingManager::compileProtoEntries(void)
{
    static const char fn [] = "RoutingManager::compileProtoEntries";

    ALOGD ("%s: enter", fn);

    /*Populate the entries on  protocol table*/
    mProtoTableEntries[PROTO_T3T_IDX].routeLoc = ROUTE_LOC_HOST_ID;//T3T Proto always to HOST. For other EE used Tech F routing
    mProtoTableEntries[PROTO_T3T_IDX].power    = PWR_SWTCH_ON_SCRN_UNLCK_MASK; //Only Screen ON UNLOCK allowed
    mProtoTableEntries[PROTO_T3T_IDX].enable   = ((mHostListnTechMask & 0x04) != 0x00) ? TRUE : FALSE;

    mProtoTableEntries[PROTO_ISODEP_IDX].routeLoc = mDefaultIsoDepSeID;
    mProtoTableEntries[PROTO_ISODEP_IDX].power    = mCeRouteStrictDisable ? mDefaultIsoDepPowerstate : (mDefaultIsoDepPowerstate & 0xE7);
    mProtoTableEntries[PROTO_ISODEP_IDX].enable   = ((mHostListnTechMask & 0x03) != 0x00) ? TRUE : FALSE;

    mProtoTableEntries[PROTO_ISO7816_IDX].routeLoc = mDefaultIso7816SeID;
    mProtoTableEntries[PROTO_ISO7816_IDX].power    = mCeRouteStrictDisable ? mDefaultIso7816Powerstate : (mDefaultIso7816Powerstate & 0xE7);
    mProtoTableEntries[PROTO_ISO7816_IDX].enable   = TRUE;

    dumpTables(1);

    ALOGD ("%s: exit", fn);
}

/* libnfc-nci takes protocols for each power-state for single route location
 * The previous protocols set will be overwritten by new protocols set by NFA_EeSetDefaultProtoRouting
 * So consolidate all the protocols/power state for a given NFCEE ID's
 * For example:
 * When PROTOCOL(ISO-DEP) and  AID default route(ISO7816) set to same EE then set (ISO-DEP | ISO-7816) to that EE.
 */
void RoutingManager::consolidateProtoEntries(void)
{
    static const char fn [] = "RoutingManager::consolidateProtoEntries";

    ALOGD ("%s: enter", fn);

    int index = -1;

    for(int xx=0;xx<MAX_PROTO_ENTRIES;xx++)
    {
        if(mProtoTableEntries[xx].enable)
        {
            switch(mProtoTableEntries[xx].routeLoc)
            {
                case ROUTE_LOC_HOST_ID:
                        index = ROUTE_LOC_HOST_ID_IDX;
                    break;
                case ROUTE_LOC_ESE_ID:
                        index = ROUTE_LOC_ESE_ID_IDX;
                    break;
                case ROUTE_LOC_UICC1_ID:
                        index = ROUTE_LOC_UICC1_ID_IDX;
                    break;
                case ROUTE_LOC_UICC2_ID:
                        index = ROUTE_LOC_UICC2_ID_IDX;
                    break;
            }
            if(index != -1)
            {
                mLmrtEntries[index].proto_switch_on    = (mLmrtEntries[index].proto_switch_on)   |
                                                         ((mProtoTableEntries[xx].power & PWR_SWTCH_ON_SCRN_UNLCK_MASK) ? mProtoTableEntries[xx].protocol:0);
                mLmrtEntries[index].proto_switch_off   = (mLmrtEntries[index].proto_switch_off)  |
                                                         ((mProtoTableEntries[xx].power & PWR_SWTCH_OFF_MASK) ? mProtoTableEntries[xx].protocol:0);
                mLmrtEntries[index].proto_battery_off  = (mLmrtEntries[index].proto_battery_off) |
                                                         ((mProtoTableEntries[xx].power & PWR_BATT_OFF_MASK) ? mProtoTableEntries[xx].protocol:0);
                mLmrtEntries[index].proto_screen_lock  = (mLmrtEntries[index].proto_screen_lock) |
                                                         ((mProtoTableEntries[xx].power & PWR_SWTCH_ON_SCRN_LOCK_MASK) ? mProtoTableEntries[xx].protocol:0);
                mLmrtEntries[index].proto_screen_off   = (mLmrtEntries[index].proto_screen_off)  |
                                                         ((mProtoTableEntries[xx].power & PWR_SWTCH_ON_SCRN_OFF_MASK) ? mProtoTableEntries[xx].protocol:0);
            }
        }
    }

    dumpTables(2);

    ALOGD ("%s: exit", fn);
}

void RoutingManager::setProtoRouting()
{
    static const char fn [] = "RoutingManager::setProtoRouting";
    tNFA_STATUS nfaStat     = NFA_STATUS_FAILED;

    ALOGD ("%s: enter", fn);
    SyncEventGuard guard (mRoutingEvent);
    for(int xx=0;xx<MAX_ROUTE_LOC_ENTRIES;xx++)
    {
        ALOGD ("%s: nfceeID:0x%X", fn, mLmrtEntries[xx].nfceeID);
        if( mLmrtEntries[xx].nfceeID           &&
           (mLmrtEntries[xx].proto_switch_on   ||
            mLmrtEntries[xx].proto_switch_off  ||
            mLmrtEntries[xx].proto_battery_off ||
            mLmrtEntries[xx].proto_screen_lock ||
            mLmrtEntries[xx].proto_screen_off) )
        {
            /*Clear protocols for NFCEE ID control block */
            ALOGD ("%s: Clear Proto Routing Entries for nfceeID:0x%X", fn, mLmrtEntries[xx].nfceeID);
            nfaStat = NFA_EeSetDefaultProtoRouting(mLmrtEntries[xx].nfceeID,0,0,0,0,0);
            if(nfaStat == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
            else
            {
                ALOGE ("Fail to clear proto routing to 0x%X",mLmrtEntries[xx].nfceeID);
            }
            /*Set Required protocols for NFCEE ID control block in libnfc-nci*/
            nfaStat = NFA_EeSetDefaultProtoRouting(mLmrtEntries[xx].nfceeID,
                                                   mLmrtEntries[xx].proto_switch_on,
                                                   mLmrtEntries[xx].proto_switch_off,
                                                   mLmrtEntries[xx].proto_battery_off,
                                                   mLmrtEntries[xx].proto_screen_lock,
                                                   mLmrtEntries[xx].proto_screen_off);
            if(nfaStat == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
            else
            {
                ALOGE ("Fail to set proto routing to 0x%X",mLmrtEntries[xx].nfceeID);
            }
        }
    }
    ALOGD ("%s: exit", fn);
}

/* Compilation of Tech Table entries strictly based on config file parameters
 * Each entry in tech table consistes of route location, technology and power state
 * */
void RoutingManager::compileTechEntries(void)
{
    static const char fn []          = "RoutingManager::compileTechEntries";
    UINT32 techSupportedBySelectedEE = 0;
#if(NXP_ESE_FELICA_CLT == TRUE)
    unsigned long num = 0;
#endif
    ALOGD ("%s: enter", fn);

    /*Check technologies supported by EE selected in conf file*/
    if(mDefaultTechASeID == ROUTE_LOC_UICC1_ID)
        techSupportedBySelectedEE = mTechSupportedByUicc1;
    else if(mDefaultTechASeID == ROUTE_LOC_UICC2_ID)
        techSupportedBySelectedEE = mTechSupportedByUicc2;
    else if(mDefaultTechASeID == ROUTE_LOC_ESE_ID)
        techSupportedBySelectedEE = mTechSupportedByEse;
    else
        techSupportedBySelectedEE = 0; /*For Host, no tech based route supported as Host always reads protocol data*/

    /*Populate the entries on  tech route table*/
    mTechTableEntries[TECH_A_IDX].routeLoc = mDefaultTechASeID;
    mTechTableEntries[TECH_A_IDX].power    = mCeRouteStrictDisable ? mDefaultTechAPowerstate : (mDefaultTechAPowerstate & 0xE7);
    mTechTableEntries[TECH_A_IDX].enable   = (techSupportedBySelectedEE & NFA_TECHNOLOGY_MASK_A)? TRUE : FALSE;

    /*Reuse the same power state and route location used for A*/
    mTechTableEntries[TECH_B_IDX].routeLoc = mDefaultTechASeID;
    mTechTableEntries[TECH_B_IDX].power    = mCeRouteStrictDisable ? mDefaultTechAPowerstate : (mDefaultTechAPowerstate & 0xE7);
    mTechTableEntries[TECH_B_IDX].enable   = (techSupportedBySelectedEE & NFA_TECHNOLOGY_MASK_B)? TRUE : FALSE;

    /*Update Tech F Route in case there is switch between uicc's*/
#if(NXP_ESE_FELICA_CLT == TRUE)
    if (GetNxpNumValue (NAME_DEFAULT_FELICA_CLT_ROUTE, (void*)&num, sizeof(num)))
    {
#if((NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE) && (NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE))
        if((num == 0x02 || num == 0x03) && sCurrentSelectedUICCSlot)
        {
            mDefaultTechFSeID = getUiccRoute(sCurrentSelectedUICCSlot);
        }
        else
        {
            mDefaultTechFSeID = ( (num == 0x01) ? ROUTE_LOC_ESE_ID : ((num == 0x02) ? ROUTE_LOC_UICC1_ID : ROUTE_LOC_UICC2_ID) );
        }
#else
        mDefaultTechFSeID = ( (num == 0x01) ? ROUTE_LOC_ESE_ID : ((num == 0x02) ? ROUTE_LOC_UICC1_ID : ROUTE_LOC_UICC2_ID) );
#endif
    }
#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE)
    else
    {
        mDefaultTechFSeID = getUiccRoute(sCurrentSelectedUICCSlot);
    }
#endif
#else
    mDefaultTechFSeID = ROUTE_LOC_UICC1_ID;
#endif

    /*Check technologies supported by EE selected in conf file - For TypeF*/
    if(mDefaultTechFSeID == ROUTE_LOC_UICC1_ID)
        techSupportedBySelectedEE = mTechSupportedByUicc1;
    else if(mDefaultTechFSeID == ROUTE_LOC_UICC2_ID)
        techSupportedBySelectedEE = mTechSupportedByUicc2;
    else if(mDefaultTechFSeID == ROUTE_LOC_ESE_ID)
        techSupportedBySelectedEE = mTechSupportedByEse;
    else
        techSupportedBySelectedEE = 0;/*For Host, no tech based route supported as Host always reads protocol data*/

    mTechTableEntries[TECH_F_IDX].routeLoc = mDefaultTechFSeID;
    mTechTableEntries[TECH_F_IDX].power    = mCeRouteStrictDisable ? mDefaultTechFPowerstate : (mDefaultTechFPowerstate & 0xE7);
    mTechTableEntries[TECH_F_IDX].enable   = (techSupportedBySelectedEE & NFA_TECHNOLOGY_MASK_F)? TRUE : FALSE;

    dumpTables(3);
    if(((mHostListnTechMask) && (mHostListnTechMask != 0X04)) && (mFwdFuntnEnable == TRUE))
    {
        processTechEntriesForFwdfunctionality();
    }
    ALOGD ("%s: exit", fn);
}

/* Forward Functionality is to handle either technology which is supported by UICC
 * We are handling it by setting the alternate technology(A/B) to HOST
 * */
void RoutingManager::processTechEntriesForFwdfunctionality(void)
{
    static const char fn []    = "RoutingManager::processTechEntriesForFwdfunctionality";
    UINT32 techSupportedByUICC = mTechSupportedByUicc1;
#if((NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE) && (NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE))
    techSupportedByUICC = (getUiccRoute(sCurrentSelectedUICCSlot) == ROUTE_LOC_UICC1_ID)?
        mTechSupportedByUicc1 : mTechSupportedByUicc2;
#elif (NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE)
    techSupportedByUICC = (mDefaultTechASeID == ROUTE_LOC_UICC1_ID)?
        mTechSupportedByUicc1:mTechSupportedByUicc2;
#endif
    ALOGD ("%s: enter", fn);

    switch(mHostListnTechMask)
    {
        case 0x01://Host wants to listen ISO-DEP in A tech only then following cases will arises:-
                  //i.Tech A only UICC present(Dont route Tech B to HOST),
                  //ii.Tech B only UICC present(Route Tech A to HOST),
                  //iii.Tech AB UICC present(Dont route any tech to HOST)
            if(((mTechTableEntries[TECH_B_IDX].routeLoc == ROUTE_LOC_UICC1_ID) || (mTechTableEntries[TECH_B_IDX].routeLoc == ROUTE_LOC_UICC2_ID)) &&
                ((((techSupportedByUICC & NFA_TECHNOLOGY_MASK_B) == 0) && (techSupportedByUICC & NFA_TECHNOLOGY_MASK_A) != 0)))//Tech A only supported UICC
            {
                //Tech A will goto UICC according to previous table
                //Disable Tech B entry as host wants to listen A only
                mTechTableEntries[TECH_B_IDX].enable   = FALSE;
            }
            if(((mTechTableEntries[TECH_A_IDX].routeLoc == ROUTE_LOC_UICC1_ID) || (mTechTableEntries[TECH_A_IDX].routeLoc == ROUTE_LOC_UICC2_ID)) &&
                ((((techSupportedByUICC & NFA_TECHNOLOGY_MASK_A) == 0) && (techSupportedByUICC & NFA_TECHNOLOGY_MASK_B) != 0)))//Tech B only supported UICC
            {
                //Tech B will goto UICC according to previous table
                //Route Tech A to HOST as Host wants to listen A only
                mTechTableEntries[TECH_A_IDX].routeLoc = ROUTE_LOC_HOST_ID;
                /*Allow only (screen On+unlock) and (screen On+lock) power state when routing to HOST*/
                mTechTableEntries[TECH_A_IDX].power    = (mTechTableEntries[TECH_A_IDX].power & 0x09);
                mTechTableEntries[TECH_A_IDX].enable   = TRUE;
            }
            if((techSupportedByUICC & 0x03) == 0x03)//AB both supported UICC
            {
                //Do Nothing
                //Tech A and Tech B will goto according to previous table
                //HCE A only / HCE-B only functionality wont work in this case
            }
            break;
        case 0x02://Host wants to listen ISO-DEP in B tech only then if Cases: Tech A only UICC present(Route Tech B to HOST), Tech B only UICC present(Dont route Tech A to HOST), Tech AB UICC present(Dont route any tech to HOST)
            if(((mTechTableEntries[TECH_B_IDX].routeLoc == ROUTE_LOC_UICC1_ID) || (mTechTableEntries[TECH_B_IDX].routeLoc == ROUTE_LOC_UICC2_ID)) &&
                ((((techSupportedByUICC & NFA_TECHNOLOGY_MASK_B) == 0) && (techSupportedByUICC & NFA_TECHNOLOGY_MASK_A) != 0)))//Tech A only supported UICC
            {
                //Tech A will goto UICC according to previous table
                //Route Tech B to HOST as host wants to listen B only
                mTechTableEntries[TECH_B_IDX].routeLoc = ROUTE_LOC_HOST_ID;
                /*Allow only (screen On+unlock) and (screen On+lock) power state when routing to HOST*/
                mTechTableEntries[TECH_B_IDX].power    = (mTechTableEntries[TECH_A_IDX].power & 0x09);
                mTechTableEntries[TECH_B_IDX].enable   = TRUE;
            }
            if(((mTechTableEntries[TECH_A_IDX].routeLoc == ROUTE_LOC_UICC1_ID) || (mTechTableEntries[TECH_A_IDX].routeLoc == ROUTE_LOC_UICC2_ID)) &&
                ((((techSupportedByUICC & NFA_TECHNOLOGY_MASK_A) == 0) && (techSupportedByUICC & NFA_TECHNOLOGY_MASK_B) != 0)))//Tech B only supported UICC
            {
                //Tech B will goto UICC according to previous table
                //Disable Tech A to HOST as host wants to listen B only
                mTechTableEntries[TECH_A_IDX].enable   = FALSE;
            }
            if((techSupportedByUICC & 0x03) == 0x03)//AB both supported UICC
            {
                //Do Nothing
                //Tech A and Tech B will goto UICC
                //HCE A only / HCE-B only functionality wont work in this case
            }
            break;
        case 0x03:
        case 0x07://Host wants to listen ISO-DEP in AB both tech then if Cases: Tech A only UICC present(Route Tech B to HOST), Tech B only UICC present(Route Tech A to HOST), Tech AB UICC present(Dont route any tech to HOST)
            /*If selected EE is UICC and it supports Bonly , then Set Tech A to Host */
            /*Host doesn't support Tech Routing, To enable FWD functionality enabling tech route to Host.*/
            if(((mTechTableEntries[TECH_A_IDX].routeLoc == ROUTE_LOC_UICC1_ID) || (mTechTableEntries[TECH_A_IDX].routeLoc == ROUTE_LOC_UICC2_ID)) &&
                ((((techSupportedByUICC & NFA_TECHNOLOGY_MASK_A) == 0) && (techSupportedByUICC & NFA_TECHNOLOGY_MASK_B) != 0)))
            {
                mTechTableEntries[TECH_A_IDX].routeLoc = ROUTE_LOC_HOST_ID;
                /*Allow only (screen On+unlock) and (screen On+lock) power state when routing to HOST*/
                mTechTableEntries[TECH_A_IDX].power    = (mTechTableEntries[TECH_A_IDX].power & 0x09);
                mTechTableEntries[TECH_A_IDX].enable   = TRUE;
            }
            /*If selected EE is UICC and it supports Aonly , then Set Tech B to Host*/
            if(((mTechTableEntries[TECH_B_IDX].routeLoc == ROUTE_LOC_UICC1_ID) || (mTechTableEntries[TECH_B_IDX].routeLoc == ROUTE_LOC_UICC2_ID)) &&
                ((((techSupportedByUICC & NFA_TECHNOLOGY_MASK_B) == 0) && (techSupportedByUICC & NFA_TECHNOLOGY_MASK_A) != 0)))
            {
                mTechTableEntries[TECH_B_IDX].routeLoc = ROUTE_LOC_HOST_ID;
                /*Allow only (screen On+unlock) and (screen On+lock) power state when routing to HOST*/
                mTechTableEntries[TECH_B_IDX].power    = (mTechTableEntries[TECH_A_IDX].power & 0x09);
                mTechTableEntries[TECH_B_IDX].enable   = TRUE;
            }
            if((techSupportedByUICC & 0x03) == 0x03)//AB both supported UICC
            {
                //Do Nothing
                //Tech A and Tech B will goto UICC
                //HCE A only / HCE-B only functionality wont work in this case
            }
            break;
    }
    dumpTables(3);
    ALOGD ("%s: exit", fn);
}

/* libnfc-nci takes technologies for each power-state for single route location
 * The previous technologies set will be overwritten by new technologies set by NFA_EeSetDefaultTechRouting
 * So consolidate all the techs/power state for a given NFCEE ID's
 * For example:
 * When Tech A and Tech F set to same EE then set (TechA | Tech F) to that EE.
 */
void RoutingManager::consolidateTechEntries(void)
{
    static const char fn [] = "RoutingManager::consolidateTechEntries";
    ALOGD ("%s: enter", fn);
    int index=-1;
    for(int xx=0;xx<MAX_TECH_ENTRIES;xx++)
    {
        if(mTechTableEntries[xx].enable)
        {
            switch(mTechTableEntries[xx].routeLoc)
            {
                case ROUTE_LOC_HOST_ID:
                        index = ROUTE_LOC_HOST_ID_IDX;
                    break;
                case ROUTE_LOC_ESE_ID:
                        index = ROUTE_LOC_ESE_ID_IDX;
                    break;
                case ROUTE_LOC_UICC1_ID:
                        index = ROUTE_LOC_UICC1_ID_IDX;
                    break;
                case ROUTE_LOC_UICC2_ID:
                        index = ROUTE_LOC_UICC2_ID_IDX;
                    break;
            }
            if(index != -1)
            {
                mLmrtEntries[index].tech_switch_on    = mLmrtEntries[index].tech_switch_on |
                                                        ((mTechTableEntries[xx].power & PWR_SWTCH_ON_SCRN_UNLCK_MASK)? mTechTableEntries[xx].technology:0);
                mLmrtEntries[index].tech_switch_off   = mLmrtEntries[index].tech_switch_off |
                                                        ((mTechTableEntries[xx].power & PWR_SWTCH_OFF_MASK)? mTechTableEntries[xx].technology:0);
                mLmrtEntries[index].tech_battery_off  = mLmrtEntries[index].tech_battery_off |
                                                        ((mTechTableEntries[xx].power & PWR_BATT_OFF_MASK)? mTechTableEntries[xx].technology:0);
                mLmrtEntries[index].tech_screen_lock  = mLmrtEntries[index].tech_screen_lock |
                                                        ((mTechTableEntries[xx].power & PWR_SWTCH_ON_SCRN_LOCK_MASK)? mTechTableEntries[xx].technology:0);
                mLmrtEntries[index].tech_screen_off   = mLmrtEntries[index].tech_screen_off |
                                                        ((mTechTableEntries[xx].power & PWR_SWTCH_ON_SCRN_OFF_MASK)? mTechTableEntries[xx].technology:0);
            }
        }
    }
    dumpTables(4);
    ALOGD ("%s: exit", fn);
}

void RoutingManager::setTechRouting(void)
{
    static const char fn [] = "RoutingManager::setTechRouting";
    tNFA_STATUS nfaStat     = NFA_STATUS_FAILED;
    ALOGD ("%s: enter", fn);
    SyncEventGuard guard (mRoutingEvent);
    for(int xx=0;xx<MAX_ROUTE_LOC_ENTRIES;xx++)
   {
       if( mLmrtEntries[xx].nfceeID          &&
           (mLmrtEntries[xx].tech_switch_on   ||
            mLmrtEntries[xx].tech_switch_off  ||
            mLmrtEntries[xx].tech_battery_off ||
            mLmrtEntries[xx].tech_screen_lock ||
            mLmrtEntries[xx].tech_screen_off) )
        {
            /*Clear technologies for NFCEE ID control block */
            ALOGD ("%s: Clear Routing Entries for nfceeID:0x%X", fn, mLmrtEntries[xx].nfceeID);
            nfaStat = NFA_EeSetDefaultTechRouting(mLmrtEntries[xx].nfceeID, 0, 0, 0, 0, 0);
            if(nfaStat == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
            else
            {
                ALOGE ("Fail to clear tech routing to 0x%x",mLmrtEntries[xx].nfceeID);
            }

            /*Set Required technologies for NFCEE ID control block */
            nfaStat = NFA_EeSetDefaultTechRouting(mLmrtEntries[xx].nfceeID,
                                                  mLmrtEntries[xx].tech_switch_on,
                                                  mLmrtEntries[xx].tech_switch_off,
                                                  mLmrtEntries[xx].tech_battery_off,
                                                  mLmrtEntries[xx].tech_screen_lock,
                                                  mLmrtEntries[xx].tech_screen_off);
            if(nfaStat == NFA_STATUS_OK)
            {
                mRoutingEvent.wait ();
            }
            else
            {
                ALOGE ("Fail to set tech routing to 0x%x",mLmrtEntries[xx].nfceeID);
            }
        }
    }
    ALOGD ("%s: exit", fn);
}

void RoutingManager::dumpTables(int xx)
{
    switch(xx)
    {
    case 1://print only proto table
        ALOGD ("--------------------Proto Table Entries------------------" );
        for(int xx=0;xx<MAX_PROTO_ENTRIES;xx++)
        {
            ALOGD ("|Index=%d|RouteLoc=0x%03X|Proto=0x%02X|Power=0x%02X|Enable=0x%01X|",
                    xx,mProtoTableEntries[xx].routeLoc,
                    mProtoTableEntries[xx].protocol,
                    mProtoTableEntries[xx].power,
                    mProtoTableEntries[xx].enable);
        }
        ALOGD ("---------------------------------------------------------" );
        break;
    case 2://print Lmrt proto table
        ALOGD ("----------------------------------------Lmrt Proto Entries------------------------------------" );
        for(int xx=0;xx<MAX_PROTO_ENTRIES;xx++)
        {
            ALOGD ("|Index=%d|nfceeID=0x%03X|SWTCH-ON=0x%02X|SWTCH-OFF=0x%02X|BAT-OFF=0x%02X|SCRN-LOCK=0x%02X|SCRN-OFF=0x%02X|",
                    xx,
                    mLmrtEntries[xx].nfceeID,
                    mLmrtEntries[xx].proto_switch_on,
                    mLmrtEntries[xx].proto_switch_off,
                    mLmrtEntries[xx].proto_battery_off,
                    mLmrtEntries[xx].proto_screen_lock,
                    mLmrtEntries[xx].proto_screen_off);
        }
        ALOGD ("----------------------------------------------------------------------------------------------" );
        break;
    case 3://print only tech table
        ALOGD ("--------------------Tech Table Entries------------------" );
        for(int xx=0;xx<MAX_TECH_ENTRIES;xx++)
        {
            ALOGD ("|Index=%d|RouteLoc=0x%03X|Tech=0x%02X|Power=0x%02X|Enable=0x%01X|",
                    xx,
                    mTechTableEntries[xx].routeLoc,
                    mTechTableEntries[xx].technology,
                    mTechTableEntries[xx].power,
                    mTechTableEntries[xx].enable);
        }
        ALOGD ("--------------------------------------------------------" );
        break;
    case 4://print Lmrt tech table
        ALOGD ("-----------------------------------------Lmrt Tech Entries------------------------------------" );
        for(int xx=0;xx<MAX_TECH_ENTRIES;xx++)
        {
            ALOGD ("|Index=%d|nfceeID=0x%03X|SWTCH-ON=0x%02X|SWTCH-OFF=0x%02X|BAT-OFF=0x%02X|SCRN-LOCK=0x%02X|SCRN-OFF=0x%02X|",
                    xx,
                    mLmrtEntries[xx].nfceeID,
                    mLmrtEntries[xx].tech_switch_on,
                    mLmrtEntries[xx].tech_switch_off,
                    mLmrtEntries[xx].tech_battery_off,
                    mLmrtEntries[xx].tech_screen_lock,
                    mLmrtEntries[xx].tech_screen_off);
        }
        ALOGD ("----------------------------------------------------------------------------------------------" );
        break;
    }
}
#endif

#if(NXP_EXTNS == TRUE)
bool RoutingManager::setRoutingEntry(int type, int value, int route, int power)
{
    static const char fn [] = "RoutingManager::setRoutingEntry";
    ALOGD ("%s: enter, type:0x%x value =0x%x route:%x power:0x%x", fn, type, value ,route, power);
    unsigned long max_tech_mask = 0x03;
    unsigned long uiccListenTech = 0;
    max_tech_mask = SecureElement::getInstance().getSETechnology(0x402);
    ALOGD ("%s: enter,max_tech_mask :%lx", fn, max_tech_mask);

    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    tNFA_HANDLE ee_handle = NFA_HANDLE_INVALID;
    SyncEventGuard guard (mRoutingEvent);
    UINT8 switch_on_mask = 0x00;
    UINT8 switch_off_mask   = 0x00;
    UINT8 battery_off_mask = 0x00;
    UINT8 screen_lock_mask = 0x00;
    UINT8 screen_off_mask = 0x00;
    UINT8 protocol_mask = 0x00;

    ee_handle = (( route == 0x01)? 0x4C0 : (( route == 0x02)? 0x402 : NFA_HANDLE_INVALID));
    if(0x00 == route)
    {
        ee_handle = 0x400;
    }
    if(ee_handle == NFA_HANDLE_INVALID )
    {
        ALOGD ("%s: enter, handle:%x invalid", fn, ee_handle);
        return nfaStat;
    }

    tNFA_HANDLE ActDevHandle = NFA_HANDLE_INVALID;
    UINT8 count,seId=0;
    UINT8 isSeIDPresent = 0;
    tNFA_HANDLE ee_handleList[SecureElement::MAX_NUM_EE];
    SecureElement::getInstance().getEeHandleList(ee_handleList, &count);


    for (int  i = 0; ((count != 0 ) && (i < count)); i++)
    {
        seId = SecureElement::getInstance().getGenericEseId(ee_handleList[i]);
        ActDevHandle = SecureElement::getInstance().getEseHandleFromGenericId(seId);
        ALOGD ("%s: enter, ee_handleList[%d]:%x", fn, i,ee_handleList[i]);
        if ((ee_handle != 0x400) &&
            (ee_handle == ActDevHandle))
        {
            isSeIDPresent =1;
            break;
        }
    }

    if(!isSeIDPresent)
    {
        ee_handle = 0x400;
    }

    if(NFA_SET_TECHNOLOGY_ROUTING == type)
    {
        switch_on_mask    = (power & 0x01) ? value : 0;
        switch_off_mask   = (power & 0x02) ? value : 0;
        battery_off_mask  = (power & 0x04) ? value : 0;
        screen_off_mask   = (power & 0x08) ? value : 0;
        screen_lock_mask  = (power & 0x10) ? value : 0;


        if(mHostListnTechMask > 0 && mFwdFuntnEnable == TRUE)
        {
            if((max_tech_mask != 0x01) && (max_tech_mask == 0x02))
            {
                switch_on_mask &= ~NFA_TECHNOLOGY_MASK_A;
                switch_off_mask &= ~NFA_TECHNOLOGY_MASK_A;
                battery_off_mask &= ~NFA_TECHNOLOGY_MASK_A;
                screen_off_mask &= ~NFA_TECHNOLOGY_MASK_A;
                screen_lock_mask &= ~NFA_TECHNOLOGY_MASK_A;

                if(mCeRouteStrictDisable == 0x01)
                {
                    nfaStat =  NFA_EeSetDefaultTechRouting (0x400,
                                                            NFA_TECHNOLOGY_MASK_A,
                                                            0,
                                                            0,
                                                            NFA_TECHNOLOGY_MASK_A,
                                                            0 );
                }else{
                    nfaStat =  NFA_EeSetDefaultTechRouting (0x400,
                                                            NFA_TECHNOLOGY_MASK_A,
                                                            0, 0, 0, 0 );
                }
                if (nfaStat == NFA_STATUS_OK)
                   mRoutingEvent.wait ();
                else
                {
                    ALOGE ("Fail to set tech routing");
                }
            }
            else if((max_tech_mask == 0x01) && (max_tech_mask != 0x02))
            {
                switch_on_mask &= ~NFA_TECHNOLOGY_MASK_B;
                switch_off_mask &= ~NFA_TECHNOLOGY_MASK_B;
                battery_off_mask &= ~NFA_TECHNOLOGY_MASK_B;
                screen_off_mask &= ~NFA_TECHNOLOGY_MASK_B;
                screen_lock_mask &= ~NFA_TECHNOLOGY_MASK_B;

                if(mCeRouteStrictDisable == 0x01)
                {
                    nfaStat =  NFA_EeSetDefaultTechRouting (0x400,
                                                           NFA_TECHNOLOGY_MASK_B,
                                                           0,
                                                           0,
                                                           NFA_TECHNOLOGY_MASK_B,
                                                           0 );
                }else{
                    nfaStat =  NFA_EeSetDefaultTechRouting (0x400,
                                                           NFA_TECHNOLOGY_MASK_B,
                                                           0, 0, 0, 0 );
                }
                if (nfaStat == NFA_STATUS_OK)
                   mRoutingEvent.wait ();
                else
                {
                    ALOGE ("Fail to set tech routing");
                }
            }
        }

        nfaStat = NFA_EeSetDefaultTechRouting (ee_handle, switch_on_mask, switch_off_mask, battery_off_mask, screen_lock_mask, screen_off_mask);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("tech routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default tech routing");
        }
    }else if(NFA_SET_PROTOCOL_ROUTING == type)
    {
        if( value == 0x01)
            protocol_mask = NFA_PROTOCOL_MASK_ISO_DEP;
        if( value == 0x02)
            protocol_mask = NFA_PROTOCOL_MASK_NFC_DEP;
        if( value == 0x04)
            protocol_mask = NFA_PROTOCOL_MASK_T3T;

        switch_on_mask     = (power & 0x01) ? protocol_mask : 0;
        switch_off_mask    = (power & 0x02) ? protocol_mask : 0;
        battery_off_mask   = (power & 0x04) ? protocol_mask : 0;
        screen_lock_mask   = (power & 0x10) ? protocol_mask : 0;
        screen_off_mask    = (power & 0x08) ? protocol_mask : 0;
        registerProtoRouteEntry(ee_handle, switch_on_mask, switch_off_mask, battery_off_mask, screen_lock_mask, screen_off_mask);
    }

    if ((GetNumValue(NAME_UICC_LISTEN_TECH_MASK, &uiccListenTech, sizeof(uiccListenTech))))
    {
         ALOGD ("%s:UICC_TECH_MASK=0x0%lu;", __FUNCTION__, uiccListenTech);
    }
    if((ActDevHandle != NFA_HANDLE_INVALID)  &&  (0 != uiccListenTech))
    {
         {
               SyncEventGuard guard (SecureElement::getInstance().mUiccListenEvent);
               nfaStat = NFA_CeConfigureUiccListenTech (ActDevHandle, 0x00);
               if (nfaStat == NFA_STATUS_OK)
               {
                     SecureElement::getInstance().mUiccListenEvent.wait ();
               }
               else
                     ALOGE ("fail to start UICC listen");
         }
         {
               SyncEventGuard guard (SecureElement::getInstance().mUiccListenEvent);
               nfaStat = NFA_CeConfigureUiccListenTech (ActDevHandle, (uiccListenTech & 0x07));
               if(nfaStat == NFA_STATUS_OK)
               {
                     SecureElement::getInstance().mUiccListenEvent.wait ();
               }
               else
                     ALOGE ("fail to start UICC listen");
         }
    }
    return nfaStat;
}

bool RoutingManager::clearRoutingEntry(int type)
{
    static const char fn [] = "RoutingManager::clearRoutingEntry";
    ALOGD ("%s: enter, type:0x%x", fn, type );
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    //tNFA_HANDLE ee_handle = NFA_HANDLE_INVLAID;

    memset(&gRouteInfo, 0x00, sizeof(RouteInfo_t));
    SyncEventGuard guard (mRoutingEvent);
    if(NFA_SET_TECHNOLOGY_ROUTING & type)
    {
        nfaStat = NFA_EeSetDefaultTechRouting (0x400, 0x00, 0x00, 0x00, 0x00, 0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("tech routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default tech routing");
        }
        nfaStat = NFA_EeSetDefaultTechRouting (0x402, 0x00, 0x00, 0x00, 0x00, 0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("tech routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default tech routing");
        }
        nfaStat = NFA_EeSetDefaultTechRouting (0x4C0, 0x00, 0x00, 0x00, 0x00, 0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("tech routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default tech routing");
        }
#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE || NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE)
        nfaStat = NFA_EeSetDefaultTechRouting (0x481, 0x00, 0x00, 0x00, 0x00, 0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("tech routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default tech routing");
        }
#endif
    }

    if(NFA_SET_PROTOCOL_ROUTING & type)
    {
        nfaStat = NFA_EeSetDefaultProtoRouting (0x400, 0x00, 0x00, 0x00, 0x00 ,0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("protocol routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default protocol routing");
        }
        nfaStat = NFA_EeSetDefaultProtoRouting (0x402, 0x00, 0x00, 0x00, 0x00, 0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("protocol routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default protocol routing");
        }
        nfaStat = NFA_EeSetDefaultProtoRouting (0x4C0, 0x00, 0x00, 0x00, 0x00, 0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("protocol routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default protocol routing");
        }
#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE || NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE)
        nfaStat = NFA_EeSetDefaultProtoRouting (0x481, 0x00, 0x00, 0x00, 0x00, 0x00);
        if(nfaStat == NFA_STATUS_OK){
            mRoutingEvent.wait ();
            ALOGD ("protocol routing SUCCESS");
        }
        else{
            ALOGE ("Fail to set default protocol routing");
        }
#endif
    }

    if (NFA_SET_AID_ROUTING & type)
    {
        clearAidTable();
    }
    return nfaStat;
}
#endif
#if(NXP_EXTNS == TRUE)
bool RoutingManager::addAidRouting(const UINT8* aid, UINT8 aidLen, int route, int power, bool isprefix)
#else
bool RoutingManager::addAidRouting(const UINT8* aid, UINT8 aidLen, int route)
#endif
{
    static const char fn [] = "RoutingManager::addAidRouting";
    ALOGD ("%s: enter", fn);
#if(NXP_EXTNS == TRUE)
    tNFA_HANDLE handle;
    tNFA_HANDLE current_handle;

    ALOGD ("%s: enter, route:%x power:0x%x isprefix:%x", fn, route, power, isprefix);
    handle = SecureElement::getInstance().getEseHandleFromGenericId(route);
    ALOGD ("%s: enter, route:%x", fn, handle);
    if (handle  == NFA_HANDLE_INVALID)
    {
        return false;
    }
    if(mAddAid == 0x00)
    {
        ALOGD ("%s: enter, mAddAid set to 0 from config file, ignoring all aids", fn);
        return false;
    }
#if(NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE)
    current_handle = ((handle == 0x4C0)?0xF3:(handle == 0x402)?SecureElement::UICC_ID:SecureElement::UICC2_ID);

#else
    current_handle = ((handle == 0x4C0)?SecureElement::ESE_ID:SecureElement::UICC_ID);
#endif
    if(handle == 0x400)
        current_handle = 0x00;

    ALOGD ("%s: enter, mDefaultEe:%x", fn, current_handle);
    //SecureElement::getInstance().activate(current_handle);
    // Set power config


    SyncEventGuard guard(SecureElement::getInstance().mAidAddRemoveEvent);
    UINT8 vs_info = 0x00;
    if(isprefix) {
        vs_info = NFA_EE_AE_NXP_PREFIX_MATCH;
    }
    tNFA_STATUS nfaStat = NFA_EeAddAidRouting(handle, aidLen, (UINT8*) aid, power, vs_info);
#else
    tNFA_STATUS nfaStat = NFA_EeAddAidRouting(route, aidLen, (UINT8*) aid, 0x01);
#endif
    if (nfaStat == NFA_STATUS_OK)
    {
//        ALOGD ("%s: routed AID", fn);
#if(NXP_EXTNS == TRUE)
        SecureElement::getInstance().mAidAddRemoveEvent.wait();
#endif
        return true;
    } else
    {
        ALOGE ("%s: failed to route AID",fn);
        return false;
    }
}

bool RoutingManager::removeAidRouting(const UINT8* aid, UINT8 aidLen)
{
    static const char fn [] = "RoutingManager::removeAidRouting";
    ALOGD ("%s: enter", fn);
    SyncEventGuard guard(SecureElement::getInstance().mAidAddRemoveEvent);
    tNFA_STATUS nfaStat = NFA_EeRemoveAidRouting(aidLen, (UINT8*) aid);
    if (nfaStat == NFA_STATUS_OK)
    {
        SecureElement::getInstance().mAidAddRemoveEvent.wait();
        ALOGD ("%s: removed AID", fn);
        return true;
    } else
    {
        ALOGE ("%s: failed to remove AID",fn);
        return false;
    }
}

#if(NXP_EXTNS == TRUE)
void RoutingManager::setDefaultTechRouting (int seId, int tech_switchon,int tech_switchoff)
{
    ALOGD ("ENTER setDefaultTechRouting");
    tNFA_STATUS nfaStat;
    /*// !!! CLEAR ALL REGISTERED TECHNOLOGIES !!!*/
    {
        SyncEventGuard guard (mRoutingEvent);
        tNFA_STATUS status =  NFA_EeSetDefaultTechRouting(0x400,0,0,0,0,0); //HOST clear
        if(status == NFA_STATUS_OK)
        {
            mRoutingEvent.wait ();
        }
    }

    {
        SyncEventGuard guard (mRoutingEvent);
        tNFA_STATUS status =  NFA_EeSetDefaultTechRouting(0x402,0,0,0,0,0); //UICC clear
        if(status == NFA_STATUS_OK)
        {
           mRoutingEvent.wait ();
        }
    }

    {
        SyncEventGuard guard (mRoutingEvent);
        tNFA_STATUS status =  NFA_EeSetDefaultTechRouting(0x4C0,0,0,0,0,0); //SMX clear
        if(status == NFA_STATUS_OK)
        {
           mRoutingEvent.wait ();
        }
    }

#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE || NXP_NFCC_DYNAMIC_DUAL_UICC == TRUE)
    {
        SyncEventGuard guard (mRoutingEvent);
        tNFA_STATUS status =  NFA_EeSetDefaultTechRouting(0x481,0,0,0,0,0); //SMX clear
        if(status == NFA_STATUS_OK)
        {
            mRoutingEvent.wait ();
        }
    }
#endif

    {
        SyncEventGuard guard (mRoutingEvent);
        if(mCeRouteStrictDisable == 0x01)
        {
            nfaStat = NFA_EeSetDefaultTechRouting (seId, tech_switchon, tech_switchoff, 0, NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B, NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B);
        }else{
            nfaStat = NFA_EeSetDefaultTechRouting (seId, tech_switchon, tech_switchoff, 0, 0, 0);
        }
        if(nfaStat == NFA_STATUS_OK)
        {
           mRoutingEvent.wait ();
           ALOGD ("tech routing SUCCESS");
        }
        else
        {
            ALOGE ("Fail to set default tech routing");
        }
    }

    nfaStat = NFA_EeUpdateNow();
    if (nfaStat != NFA_STATUS_OK){
        ALOGE("Failed to commit routing configuration");
    }
}

void RoutingManager::setDefaultProtoRouting (int seId, int proto_switchon,int proto_switchoff)
{
    tNFA_STATUS nfaStat;
    ALOGD ("ENTER setDefaultProtoRouting");
    SyncEventGuard guard (mRoutingEvent);
    if(mCeRouteStrictDisable == 0x01)
    {
        nfaStat = NFA_EeSetDefaultProtoRouting (seId, proto_switchon, proto_switchoff, 0, NFA_PROTOCOL_MASK_ISO_DEP, NFA_PROTOCOL_MASK_ISO_DEP);
    }else{
        nfaStat = NFA_EeSetDefaultProtoRouting (seId, proto_switchon, proto_switchoff, 0, 0, 0);
    }
    if(nfaStat == NFA_STATUS_OK){
        mRoutingEvent.wait ();
        ALOGD ("proto routing SUCCESS");
    }
    else{
        ALOGE ("Fail to set default proto routing");
    }
//    nfaStat = NFA_EeUpdateNow();
//    if (nfaStat != NFA_STATUS_OK){
//        ALOGE("Failed to commit routing configuration");
//    }
}

bool RoutingManager::clearAidTable ()
{
    static const char fn [] = "RoutingManager::clearAidTable";
    ALOGD ("%s: enter", fn);
    SyncEventGuard guard(SecureElement::getInstance().mAidAddRemoveEvent);
    tNFA_STATUS nfaStat = NFA_EeRemoveAidRouting(NFA_REMOVE_ALL_AID_LEN, (UINT8*) NFA_REMOVE_ALL_AID);
    if (nfaStat == NFA_STATUS_OK)
    {
        SecureElement::getInstance().mAidAddRemoveEvent.wait();
        ALOGD ("%s: removed AID", fn);
        return true;
    } else
    {
        ALOGE ("%s: failed to remove AID", fn);
        return false;
    }
}


#endif

bool RoutingManager::commitRouting()
{
    static const char fn [] = "RoutingManager::commitRouting";
    tNFA_STATUS nfaStat = 0;
    ALOGD ("%s", fn);
    {
        RoutingManager::getInstance().LmrtRspTimer.set(1000, LmrtRspTimerCb);
        SyncEventGuard guard (mEeUpdateEvent);
        nfaStat = NFA_EeUpdateNow();
        if (nfaStat == NFA_STATUS_OK)
        {
            mEeUpdateEvent.wait (); //wait for NFA_EE_UPDATED_EVT
        }
    }
    return (nfaStat == NFA_STATUS_OK);
}

void RoutingManager::onNfccShutdown ()
{
    static const char fn [] = "RoutingManager:onNfccShutdown";
    tNFA_STATUS nfaStat     = NFA_STATUS_FAILED;
    UINT8 actualNumEe       = SecureElement::MAX_NUM_EE;
    tNFA_EE_INFO eeInfo[actualNumEe];

    if (mActiveSe == 0x00)
        return;

    memset (&eeInfo, 0, sizeof(eeInfo));

    if ((nfaStat = NFA_EeGetInfo (&actualNumEe, eeInfo)) != NFA_STATUS_OK)
    {
        ALOGE ("%s: fail get info; error=0x%X", fn, nfaStat);
        return;
    }
    if (actualNumEe != 0)
    {
        for (UINT8 xx = 0; xx < actualNumEe; xx++)
        {
            if ((eeInfo[xx].num_interface != 0)
                && (eeInfo[xx].ee_interface[0] != NCI_NFCEE_INTERFACE_HCI_ACCESS)
                && (eeInfo[xx].ee_status == NFA_EE_STATUS_ACTIVE))
            {
                ALOGD ("%s: Handle: 0x%04x Change Status Active to Inactive", fn, eeInfo[xx].ee_handle);
#if(NXP_EXTNS == TRUE)
                if ((nfaStat = SecureElement::getInstance().SecElem_EeModeSet (eeInfo[xx].ee_handle, NFA_EE_MD_DEACTIVATE)) != NFA_STATUS_OK)
#endif
                {
                    ALOGE ("Failed to set EE inactive");
                }
            }
        }
    }
    else
    {
        ALOGD ("%s: No active EEs found", fn);
    }
}

void RoutingManager::notifyActivated (UINT8 technology)
{
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE ("jni env is null");
        return;
    }

    e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyHostEmuActivated, (int)technology);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE ("fail notify");
    }
}

void RoutingManager::notifyDeactivated (UINT8 technology)
{
    SecureElement::getInstance().notifyListenModeState (false);
    mRxDataBuffer.clear();
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE ("jni env is null");
        return;
    }

    e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyHostEmuDeactivated, (int)technology);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE ("fail notify");
    }
}

void RoutingManager::notifyLmrtFull ()
{
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE ("jni env is null");
        return;
    }

    e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyAidRoutingTableFull);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE ("fail notify");
    }
}
#if((NXP_EXTNS == TRUE) && (NXP_NFCC_EMPTY_DATA_PACKET == TRUE))
void RoutingManager::nfcFRspTimerCb(union sigval)
{
    static const char fn[] = "RoutingManager::nfcFRspTimerCb";
    ALOGD("%s; enter", fn);
    if(android::gIsEmptyRspSentByHceFApk)
        android::gIsEmptyRspSentByHceFApk = FALSE;
    else
        android::nfcManager_sendEmptyDataMsg();
    ALOGD("%s; exit", fn);
}
#endif

void RoutingManager::handleData (UINT8 technology, const UINT8* data, UINT32 dataLen, tNFA_STATUS status)
{

    if (status == NFA_STATUS_CONTINUE)
    {
        ALOGE ("jni env is null");
        if (dataLen > 0)
        {
            mRxDataBuffer.insert (mRxDataBuffer.end(), &data[0], &data[dataLen]); //append data; more to come
        }
        return; //expect another NFA_CE_DATA_EVT to come
    }
    else if (status == NFA_STATUS_OK)
    {
        if (dataLen > 0)
        {
            mRxDataBuffer.insert (mRxDataBuffer.end(), &data[0], &data[dataLen]); //append data
#if((NXP_EXTNS == TRUE) && (NXP_NFCC_EMPTY_DATA_PACKET == TRUE))
            if (technology == NFA_TECHNOLOGY_MASK_F)
            {
                bool ret = false;
                ret = mNfcFRspTimer.set(mDefaultHCEFRspTimeout, nfcFRspTimerCb);
                if(!ret)
                    ALOGD("%s; rsp timer create failed", __FUNCTION__);
            }
#endif
        }
        //entire data packet has been received; no more NFA_CE_DATA_EVT
    }
    else if (status == NFA_STATUS_FAILED)
    {
        ALOGE("RoutingManager::handleData: read data fail");
        goto TheEnd;
    }
    {
        JNIEnv* e = NULL;
        ScopedAttach attach(mNativeData->vm, &e);
        if (e == NULL)
        {
            ALOGE ("jni env is null");
            goto TheEnd;
        }

        ScopedLocalRef<jobject> dataJavaArray(e, e->NewByteArray(mRxDataBuffer.size()));
        if (dataJavaArray.get() == NULL)
        {
            ALOGE ("fail allocate array");
            goto TheEnd;
        }

        e->SetByteArrayRegion ((jbyteArray)dataJavaArray.get(), 0, mRxDataBuffer.size(),
                (jbyte *)(&mRxDataBuffer[0]));
        if (e->ExceptionCheck())
        {
            e->ExceptionClear();
            ALOGE ("fail fill array");
            goto TheEnd;
        }

        e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyHostEmuData,
             (int)technology, dataJavaArray.get());
        if (e->ExceptionCheck())
        {
            e->ExceptionClear();
            ALOGE ("fail notify");
        }

    }
TheEnd:
    mRxDataBuffer.clear();
}

void RoutingManager::stackCallback (UINT8 event, tNFA_CONN_EVT_DATA* eventData)
{
    static const char fn [] = "RoutingManager::stackCallback";
    ALOGD("%s: event=0x%X", fn, event);
    RoutingManager& routingManager = RoutingManager::getInstance();

#if((NFC_NXP_ESE == TRUE)&&(NXP_NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION == TRUE))
    SecureElement& se = SecureElement::getInstance();
#endif

    switch (event)
    {
    case NFA_CE_REGISTERED_EVT:
        {
            tNFA_CE_REGISTERED& ce_registered = eventData->ce_registered;
            ALOGD("%s: NFA_CE_REGISTERED_EVT; status=0x%X; h=0x%X", fn, ce_registered.status, ce_registered.handle);
            SyncEventGuard guard (routingManager.mCeRegisterEvent);
            if(ce_registered.status == NFA_STATUS_OK)
            {
                lastcehandle = ce_registered.handle;
            }
            else
            {
                lastcehandle = 0xFF;
            }
            routingManager.mCeRegisterEvent.notifyOne();
        }
        break;

    case NFA_CE_DEREGISTERED_EVT:
        {
            tNFA_CE_DEREGISTERED& ce_deregistered = eventData->ce_deregistered;
            ALOGD("%s: NFA_CE_DEREGISTERED_EVT; h=0x%X", fn, ce_deregistered.handle);
            SyncEventGuard guard (routingManager.mCeDeRegisterEvent);
            routingManager.mCeDeRegisterEvent.notifyOne();
        }
        break;

    case NFA_CE_ACTIVATED_EVT:
        {
#if (NXP_EXTNS == TRUE)
            android::rfActivation = true;
#endif
            android::checkforTranscation(NFA_CE_ACTIVATED_EVT, (void *)eventData);
            routingManager.notifyActivated(NFA_TECHNOLOGY_MASK_A);
        }
        break;
    case NFA_DEACTIVATED_EVT:
    case NFA_CE_DEACTIVATED_EVT:
        {
            android::checkforTranscation(NFA_CE_DEACTIVATED_EVT, (void *)eventData);
            routingManager.notifyDeactivated(NFA_TECHNOLOGY_MASK_A);
#if((NFC_NXP_ESE == TRUE)&&(NXP_NFCC_ESE_UICC_CONCURRENT_ACCESS_PROTECTION == TRUE))
            if (se.mIsWiredModeOpen && se.mPassiveListenEnabled)
            {
                se.startThread(0x00);
            }
#endif
        }
#if (NXP_EXTNS == TRUE)
        android::rfActivation = false;
#endif
        break;
    case NFA_CE_DATA_EVT:
        {
#if ((NFC_NXP_ESE == TRUE) && (NXP_EXTNS == TRUE))
#if(NXP_ESE_DUAL_MODE_PRIO_SCHEME != NXP_ESE_WIRED_MODE_RESUME)
            se.setDwpTranseiveState(false, NFCC_CE_DATA_EVT);
#endif
#endif
            tNFA_CE_DATA& ce_data = eventData->ce_data;
            ALOGD("%s: NFA_CE_DATA_EVT; stat=0x%X; h=0x%X; data len=%u", fn, ce_data.status, ce_data.handle, ce_data.len);
            getInstance().handleData(NFA_TECHNOLOGY_MASK_A, ce_data.p_data, ce_data.len, ce_data.status);
        }
        break;
    }
}
/*******************************************************************************
**
** Function:        nfaEeCallback
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::nfaEeCallback (tNFA_EE_EVT event, tNFA_EE_CBACK_DATA* eventData)
{
    static const char fn [] = "RoutingManager::nfaEeCallback";

    SecureElement& se = SecureElement::getInstance();
    RoutingManager& routingManager = RoutingManager::getInstance();
    tNFA_EE_DISCOVER_REQ info;

    switch (event)
    {
    case NFA_EE_REGISTER_EVT:
        {
            SyncEventGuard guard (routingManager.mEeRegisterEvent);
            ALOGD ("%s: NFA_EE_REGISTER_EVT; status=%u", fn, eventData->ee_register);
            routingManager.mEeRegisterEvent.notifyOne();
        }
        break;

    case NFA_EE_MODE_SET_EVT:
        {
            SyncEventGuard guard (routingManager.mEeSetModeEvent);
            ALOGD ("%s: NFA_EE_MODE_SET_EVT; status: 0x%04X  handle: 0x%04X  mActiveEeHandle: 0x%04X", fn,
                    eventData->mode_set.status, eventData->mode_set.ee_handle, se.mActiveEeHandle);
            routingManager.mEeSetModeEvent.notifyOne();
            se.notifyModeSet(eventData->mode_set.ee_handle, !(eventData->mode_set.status),eventData->mode_set.ee_status );
        }
        break;
#if (NXP_EXTNS == TRUE)
    case NFA_EE_SET_MODE_INFO_EVT:
    {
        ALOGD ("%s: NFA_EE_SET_MODE_INFO_EVT; nfcee_id = 0x%02x, status: 0x%04X ", fn,
            eventData->ee_set_mode_info.nfcee_id, eventData->ee_set_mode_info.status);
        se.mModeSetInfo = eventData->ee_set_mode_info.status;
        if(eventData->ee_set_mode_info.nfcee_id == 0xC0)
        {
            recovery = FALSE;
            SyncEventGuard guard (se.mModeSetNtf);
            se.mModeSetNtf.notifyOne();
        }
    }
        break;
#if(NXP_WIRED_MODE_STANDBY == TRUE)
    case NFA_EE_PWR_LINK_CTRL_EVT:
        {
            ALOGD ("%s: NFA_EE_PWR_LINK_CTRL_EVT; status: 0x%04X ", fn,
                    eventData->pwr_lnk_ctrl.status);
            se.mPwrCmdstatus = eventData->pwr_lnk_ctrl.status;
            SyncEventGuard guard (se.mPwrLinkCtrlEvent);
            se.mPwrLinkCtrlEvent.notifyOne();
        }
        break;
#endif
#endif

    case NFA_EE_SET_TECH_CFG_EVT:
        {
            ALOGD ("%s: NFA_EE_SET_TECH_CFG_EVT; status=0x%X", fn, eventData->status);
            SyncEventGuard guard(routingManager.mRoutingEvent);
            routingManager.mRoutingEvent.notifyOne();
        }
        break;

    case NFA_EE_SET_PROTO_CFG_EVT:
        {
            ALOGD ("%s: NFA_EE_SET_PROTO_CFG_EVT; status=0x%X", fn, eventData->status);
            SyncEventGuard guard(routingManager.mRoutingEvent);
            routingManager.mRoutingEvent.notifyOne();
        }
        break;

    case NFA_EE_ACTION_EVT:
        {
            tNFA_EE_ACTION& action = eventData->action;
            tNFC_APP_INIT& app_init = action.param.app_init;
            android::checkforTranscation(NFA_EE_ACTION_EVT, (void *)eventData);

            if (action.trigger == NFC_EE_TRIG_SELECT)
            {
                ALOGD ("%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=select (0x%X); aid len=%u", fn, action.ee_handle, action.trigger, app_init.len_aid);
            }
            else if (action.trigger == NFC_EE_TRIG_APP_INIT)
            {
                ALOGD ("%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=app-init (0x%X); aid len=%u; data len=%u", fn,
                        action.ee_handle, action.trigger, app_init.len_aid, app_init.len_data);
                //if app-init operation is successful;
                //app_init.data[] contains two bytes, which are the status codes of the event;
                //app_init.data[] does not contain an APDU response;
                //see EMV Contactless Specification for Payment Systems; Book B; Entry Point Specification;
                //version 2.1; March 2011; section 3.3.3.5;
                if ( (app_init.len_data > 1) &&
                     (app_init.data[0] == 0x90) &&
                     (app_init.data[1] == 0x00) )
                {
                    se.notifyTransactionListenersOfAid (app_init.aid, app_init.len_aid, app_init.data, app_init.len_data, SecureElement::getInstance().getGenericEseId(action.ee_handle & ~NFA_HANDLE_GROUP_EE));
                }
            }
            else if (action.trigger == NFC_EE_TRIG_RF_PROTOCOL)
                ALOGD ("%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=rf protocol (0x%X)", fn, action.ee_handle, action.trigger);
            else if (action.trigger == NFC_EE_TRIG_RF_TECHNOLOGY)
                ALOGD ("%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=rf tech (0x%X)", fn, action.ee_handle, action.trigger);
            else
                ALOGE ("%s: NFA_EE_ACTION_EVT; h=0x%X; unknown trigger (0x%X)", fn, action.ee_handle, action.trigger);
#if ((NXP_EXTNS == TRUE) && (NFC_NXP_ESE == TRUE))
            if(action.ee_handle == 0x4C0)
            {
                ALOGE ("%s: NFA_EE_ACTION_EVT; h=0x%X;DWP CL activated (0x%X)", fn, action.ee_handle, action.trigger);
                se.setCLState(true);
            }


#if(NXP_ESE_DUAL_MODE_PRIO_SCHEME != NXP_ESE_WIRED_MODE_RESUME)
            if(action.ee_handle == 0x4C0 && (action.trigger != NFC_EE_TRIG_RF_TECHNOLOGY) &&
            ((se.mIsAllowWiredInDesfireMifareCE) || !(action.trigger == NFC_EE_TRIG_RF_PROTOCOL && action.param.protocol == NFA_PROTOCOL_ISO_DEP)))
            {
                ALOGE("%s,Allow wired mode connection", fn);
                se.setDwpTranseiveState(false, NFCC_ACTION_NTF);
            }
            else
            {
                ALOGE("%s,Blocked wired mode connection", fn);
                se.setDwpTranseiveState(true, NFCC_ACTION_NTF);
            }
#endif
#endif
        }
        break;

    case NFA_EE_DISCOVER_EVT:
        {
            UINT8 num_ee = eventData->ee_discover.num_ee;
            tNFA_EE_DISCOVER ee_disc_info = eventData->ee_discover;
            ALOGD ("%s: NFA_EE_DISCOVER_EVT; status=0x%X; num ee=%u", __FUNCTION__,eventData->status, eventData->ee_discover.num_ee);
#if (NXP_NFCEE_REMOVED_NTF_RECOVERY == TRUE)
            if(android::isNfcInitializationDone() == true)
            {
                if((mChipId == CHIP_ID_PN65T) || (mChipId == CHIP_ID_PN66T) ||
                    (mChipId == CHIP_ID_PN67T) || (mChipId == CHIP_ID_PN80T))
                {
                    for(int xx = 0; xx <  num_ee; xx++)
                    {
                        ALOGE("xx=%d, ee_handle=0x0%x, status=0x0%x", xx, ee_disc_info.ee_info[xx].ee_handle,ee_disc_info.ee_info[xx].ee_status);
                        if (ee_disc_info.ee_info[xx].ee_handle == 0x4C0)
                        {
                            if(ee_disc_info.ee_info[xx].ee_status == NFA_EE_STATUS_REMOVED)
                            {
                                recovery=TRUE;
                                routingManager.ee_removed_disc_ntf_handler(ee_disc_info.ee_info[xx].ee_handle, ee_disc_info.ee_info[xx].ee_status);
                                break;
                            }
                            else if((ee_disc_info.ee_info[xx].ee_status == NFA_EE_STATUS_ACTIVE) && (recovery == TRUE))
                            {
                                recovery = FALSE;
                                SyncEventGuard guard(se.mEEdatapacketEvent);
                                se.mEEdatapacketEvent.notifyOne();
                            }
                        }
                    }
                }
            }
#endif
            /*gSeDiscoverycount++ incremented for new NFCEE discovery;*/
            SecureElement::getInstance().updateNfceeDiscoverInfo();
            ALOGD(" gSeDiscoverycount = %ld gActualSeCount=%ld", gSeDiscoverycount,gActualSeCount);
            if(gSeDiscoverycount >= gActualSeCount)
            {
                SyncEventGuard g (gNfceeDiscCbEvent);
                ALOGD("%s: Sem Post for gNfceeDiscCbEvent", __FUNCTION__);
                //usleep(1000000); // wait for 1000 millisec
                //wait for atleast 1 sec to receive all ntf
                gNfceeDiscCbEvent.notifyOne ();
            }
        }
        break;

    case NFA_EE_DISCOVER_REQ_EVT:
        info = eventData->discover_req;
        ALOGD ("%s: NFA_EE_DISCOVER_REQ_EVT; status=0x%X; num ee=%u", __FUNCTION__,
                eventData->discover_req.status, eventData->discover_req.num_ee);
#if((NFC_NXP_ESE == TRUE) && (NXP_EXTNS == TRUE) && (NXP_ESE_ETSI_READER_ENABLE == TRUE))
        /* Handle Reader over SWP.
         * 1. Check if the event is for Reader over SWP.
         * 2. IF yes than send this info(READER_REQUESTED_EVENT) till FWK level.
         * 3. Stop the discovery.
         * 4. MAP the proprietary interface for Reader over SWP.NFC_DiscoveryMap, nfc_api.h
         * 5. start the discovery with reader req, type and DH configuration.
         *
         * 6. IF yes than send this info(STOP_READER_EVENT) till FWK level.
         * 7. MAP the DH interface for Reader over SWP. NFC_DiscoveryMap, nfc_api.h
         * 8. start the discovery with DH configuration.
         */
        swp_rdr_req_ntf_info.mMutex.lock ();
        for (UINT8 xx = 0; xx < info.num_ee; xx++)
        {
            //for each technology (A, B, F, B'), print the bit field that shows
            //what protocol(s) is support by that technology
            ALOGD ("%s   EE[%u] Handle: 0x%04x  PA: 0x%02x  PB: 0x%02x",
                    fn, xx, info.ee_disc_info[xx].ee_handle,
                    info.ee_disc_info[xx].pa_protocol,
                    info.ee_disc_info[xx].pb_protocol);

            ALOGD("%s, swp_rd_state=%x", fn, swp_rdr_req_ntf_info.swp_rd_state);
            if( (info.ee_disc_info[xx].ee_req_op == NFC_EE_DISC_OP_ADD) &&
                    (swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_STOPPED ||
                    swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_START_CONFIG ||
                    swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_STOP_CONFIG)&&
                    (info.ee_disc_info[xx].pa_protocol ==  0x04 || info.ee_disc_info[xx].pb_protocol == 0x04 ))
            {
                ALOGD ("%s NFA_RD_SWP_READER_REQUESTED  EE[%u] Handle: 0x%04x  PA: 0x%02x  PB: 0x%02x",
                        fn, xx, info.ee_disc_info[xx].ee_handle,
                        info.ee_disc_info[xx].pa_protocol,
                        info.ee_disc_info[xx].pb_protocol);

                swp_rdr_req_ntf_info.swp_rd_req_info.src = info.ee_disc_info[xx].ee_handle;
                swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask = 0;
                swp_rdr_req_ntf_info.swp_rd_req_info.reCfg = false;

                if( !(swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_A) )
                {
                    if(info.ee_disc_info[xx].pa_protocol ==  0x04)
                    {
                        swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask |= NFA_TECHNOLOGY_MASK_A;
                        swp_rdr_req_ntf_info.swp_rd_req_info.reCfg = true;
                    }
                }

                if( !(swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_B) )
                {
                    if(info.ee_disc_info[xx].pb_protocol ==  0x04)
                    {
                        swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask |= NFA_TECHNOLOGY_MASK_B;
                        swp_rdr_req_ntf_info.swp_rd_req_info.reCfg = true;
                    }
                }

                if(swp_rdr_req_ntf_info.swp_rd_req_info.reCfg)
                {
                    ALOGD("%s, swp_rd_state=%x  evt : NFA_RD_SWP_READER_REQUESTED swp_rd_req_timer start", fn, swp_rdr_req_ntf_info.swp_rd_state);

                    swp_rd_req_timer.kill();
                    if(swp_rdr_req_ntf_info.swp_rd_state != STATE_SE_RDR_MODE_STOP_CONFIG)
                    {
                        swp_rd_req_timer.set (rdr_req_handling_timeout, reader_req_event_ntf);
                        swp_rdr_req_ntf_info.swp_rd_state = STATE_SE_RDR_MODE_START_CONFIG;
                    }
                    /*RestartReadermode procedure special case should not de-activate*/
                    else if(swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_STOP_CONFIG)
                    {
                        swp_rdr_req_ntf_info.swp_rd_state = STATE_SE_RDR_MODE_STARTED;
                        /*RFDEACTIVATE_DISCOVERY*/
                        NFA_Deactivate(false);
                    }
                    swp_rdr_req_ntf_info.swp_rd_req_info.reCfg = false;
                }
                //Reader over SWP - Reader Requested.
                //se.handleEEReaderEvent(NFA_RD_SWP_READER_REQUESTED, tech, info.ee_disc_info[xx].ee_handle);
                break;
            }
            else if((info.ee_disc_info[xx].ee_req_op == NFC_EE_DISC_OP_REMOVE) &&
                    ((swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_STARTED) ||
                    (swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_START_CONFIG) ||
                    (swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_STOP_CONFIG) ||
                    (swp_rdr_req_ntf_info.swp_rd_state == STATE_SE_RDR_MODE_ACTIVATED)) &&
                    (info.ee_disc_info[xx].pa_protocol ==  0xFF || info.ee_disc_info[xx].pb_protocol == 0xFF))
            {
                ALOGD ("%s NFA_RD_SWP_READER_STOP  EE[%u] Handle: 0x%04x  PA: 0x%02x  PB: 0x%02x",
                        fn, xx, info.ee_disc_info[xx].ee_handle,
                        info.ee_disc_info[xx].pa_protocol,
                        info.ee_disc_info[xx].pb_protocol);

                if(swp_rdr_req_ntf_info.swp_rd_req_info.src == info.ee_disc_info[xx].ee_handle)
                {
                     if(info.ee_disc_info[xx].pa_protocol ==  0xFF)
                     {
                         if(swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_A)
                         {
                             swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask &= ~NFA_TECHNOLOGY_MASK_A;
                             swp_rdr_req_ntf_info.swp_rd_req_info.reCfg = true;
                             //swp_rdr_req_ntf_info.swp_rd_state = STATE_SE_RDR_MODE_STOP_CONFIG;
                         }
                     }

                     if(info.ee_disc_info[xx].pb_protocol ==  0xFF)
                     {
                         if(swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask & NFA_TECHNOLOGY_MASK_B)
                         {
                             swp_rdr_req_ntf_info.swp_rd_req_info.tech_mask &= ~NFA_TECHNOLOGY_MASK_B;
                             swp_rdr_req_ntf_info.swp_rd_req_info.reCfg = true;
                         }

                     }

                     if(swp_rdr_req_ntf_info.swp_rd_req_info.reCfg)
                     {
                         ALOGD("%s, swp_rd_state=%x  evt : NFA_RD_SWP_READER_STOP swp_rd_req_timer start", fn, swp_rdr_req_ntf_info.swp_rd_state);
                         swp_rdr_req_ntf_info.swp_rd_state = STATE_SE_RDR_MODE_STOP_CONFIG;
                         swp_rd_req_timer.kill();
                         swp_rd_req_timer.set (rdr_req_handling_timeout, reader_req_event_ntf);
                         swp_rdr_req_ntf_info.swp_rd_req_info.reCfg = false;
                     }
                }
                break;
            }
        }
        swp_rdr_req_ntf_info.mMutex.unlock();
        /*Set the configuration for UICC/ESE */
        se.storeUiccInfo (eventData->discover_req);
#endif
        break;

    case NFA_EE_NO_CB_ERR_EVT:
        ALOGD ("%s: NFA_EE_NO_CB_ERR_EVT  status=%u", fn, eventData->status);
        break;

    case NFA_EE_ADD_AID_EVT:
        {
            ALOGD ("%s: NFA_EE_ADD_AID_EVT  status=%u", fn, eventData->status);
            if(eventData->status == NFA_STATUS_BUFFER_FULL)
            {
                ALOGD ("%s: AID routing table is FULL!!!", fn);
                RoutingManager::getInstance().notifyLmrtFull();
            }
            SyncEventGuard guard(se.mAidAddRemoveEvent);
            se.mAidAddRemoveEvent.notifyOne();
        }
        break;

    case NFA_EE_REMOVE_AID_EVT:
        {
            ALOGD ("%s: NFA_EE_REMOVE_AID_EVT  status=%u", fn, eventData->status);
            SyncEventGuard guard(se.mAidAddRemoveEvent);
            se.mAidAddRemoveEvent.notifyOne();
        }
        break;

    case NFA_EE_NEW_EE_EVT:
        {
            ALOGD ("%s: NFA_EE_NEW_EE_EVT  h=0x%X; status=%u", fn,
                eventData->new_ee.ee_handle, eventData->new_ee.ee_status);
        }
        break;
    case NFA_EE_ROUT_ERR_EVT:
        {
            ALOGD ("%s: NFA_EE_ROUT_ERR_EVT  status=%u", fn,eventData->status);
        }
        break;
    case NFA_EE_UPDATED_EVT:
        {
            ALOGD("%s: NFA_EE_UPDATED_EVT", fn);
            SyncEventGuard guard(routingManager.mEeUpdateEvent);
            routingManager.mEeUpdateEvent.notifyOne();
            routingManager.LmrtRspTimer.kill();
        }
        break;
    default:
        ALOGE ("%s: unknown event=%u ????", fn, event);
        break;
    }
}

#if(NXP_EXTNS == TRUE)
#if(NXP_NFCC_HCE_F == TRUE)
void RoutingManager::notifyT3tConfigure()
{
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE ("jni env is null");
        return;
    }

    e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyT3tConfigure);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE ("fail notify");
    }
}
#endif
void RoutingManager::notifyReRoutingEntry()
{
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE ("jni env is null");
        return;
    }

    e->CallVoidMethod (mNativeData->manager, android::gCachedNfcManagerNotifyReRoutingEntry);
    if (e->ExceptionCheck())
    {
        e->ExceptionClear();
        ALOGE ("fail notify");
    }
}
#endif

int RoutingManager::registerT3tIdentifier(UINT8* t3tId, UINT8 t3tIdLen)
{
    static const char fn [] = "RoutingManager::registerT3tIdentifier";

    ALOGD ("%s: Start to register NFC-F system on DH", fn);

    if (t3tIdLen != (2 + NCI_RF_F_UID_LEN))
    {
        ALOGE ("%s: Invalid length of T3T Identifier", fn);
        return NFA_HANDLE_INVALID;
    }

#if(NXP_EXTNS == TRUE && NXP_NFCC_HCE_F == TRUE)
    if (android::nfcManager_getTransanctionRequest(0, true))
    {
        ALOGD ("%s: Busy in nfcManager_getTransanctionRequest", fn);
        return NFA_HANDLE_INVALID;
    }

    if (android::isDiscoveryStarted()) {
      // Stop RF discovery to reconfigure
      android::startRfDiscovery(false);
    }
#endif

    SyncEventGuard guard (mRoutingEvent);
    mNfcFOnDhHandle = NFA_HANDLE_INVALID;

    int systemCode;
    UINT8 nfcid2[NCI_RF_F_UID_LEN];

    systemCode = (((int)t3tId[0] << 8) | ((int)t3tId[1] << 0));
    memcpy(nfcid2, t3tId + 2, NCI_RF_F_UID_LEN);

    tNFA_STATUS nfaStat = NFA_CeRegisterFelicaSystemCodeOnDH (systemCode, nfcid2, nfcFCeCallback);
    if (nfaStat == NFA_STATUS_OK)
    {
        mRoutingEvent.wait ();
    }
    else
    {
        ALOGE ("%s: Fail to register NFC-F system on DH", fn);
        return NFA_HANDLE_INVALID;
    }

    ALOGD ("%s: Succeed to register NFC-F system on DH", fn);

    return mNfcFOnDhHandle;
}

void RoutingManager::deregisterT3tIdentifier(int handle)
{
    static const char fn [] = "RoutingManager::deregisterT3tIdentifier";
    ALOGD ("%s: Start to deregister NFC-F system on DH", fn);
#if(NXP_EXTNS == TRUE && NXP_NFCC_HCE_F == TRUE)
    bool enable = false;
    if (android::nfcManager_getTransanctionRequest(handle, false))
    {
        ALOGD ("%s: Busy in nfcManager_getTransanctionRequest", fn);
        return;
    }
    else if (android::isDiscoveryStarted())
    {
        // Stop RF discovery to reconfigure
        android::startRfDiscovery(false);
        enable = true;
    }
#endif
    SyncEventGuard guard (mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_CeDeregisterFelicaSystemCodeOnDH (handle);
    if (nfaStat == NFA_STATUS_OK)
    {
        mRoutingEvent.wait ();
        ALOGD ("%s: Succeeded in deregistering NFC-F system on DH", fn);
    }
    else
    {
        ALOGE ("%s: Fail to deregister NFC-F system on DH", fn);
    }
#if(NXP_EXTNS == TRUE && NXP_NFCC_HCE_F == TRUE)
    if(enable)
    {
        // Stop RF discovery to reconfigure
        android::startRfDiscovery(true);
    }
#endif
}

void RoutingManager::nfcFCeCallback (UINT8 event, tNFA_CONN_EVT_DATA* eventData)
{
    static const char fn [] = "RoutingManager::nfcFCeCallback";
    RoutingManager& routingManager = RoutingManager::getInstance();

#if ((NXP_EXTNS == TRUE) && (NFC_NXP_ESE == TRUE)) && (NXP_ESE_DUAL_MODE_PRIO_SCHEME != NXP_ESE_WIRED_MODE_RESUME)
    SecureElement& se = SecureElement::getInstance();
#endif

    ALOGD("%s: 0x%x", __FUNCTION__, event);

    switch (event)
    {
    case NFA_CE_REGISTERED_EVT:
        {
            ALOGD ("%s: registerd event notified", fn);
           routingManager.mNfcFOnDhHandle = eventData->ce_registered.handle;
            SyncEventGuard guard(routingManager.mRoutingEvent);
            routingManager.mRoutingEvent.notifyOne();
        }
        break;
    case NFA_CE_DEREGISTERED_EVT:
        {
            ALOGD ("%s: deregisterd event notified", fn);
            SyncEventGuard guard(routingManager.mRoutingEvent);
            routingManager.mRoutingEvent.notifyOne();
        }
        break;
   case NFA_CE_ACTIVATED_EVT:
        {
            ALOGD ("%s: activated event notified", fn);
#if (NXP_EXTNS == TRUE)
            android::checkforTranscation(NFA_CE_ACTIVATED_EVT, (void *)eventData);
#endif
            routingManager.notifyActivated(NFA_TECHNOLOGY_MASK_F);
        }
        break;
    case NFA_CE_DEACTIVATED_EVT:
        {
            ALOGD ("%s: deactivated event notified", fn);
#if (NXP_EXTNS == TRUE)
            android::checkforTranscation(NFA_CE_DEACTIVATED_EVT, (void *)eventData);
#endif
            routingManager.notifyDeactivated(NFA_TECHNOLOGY_MASK_F);
        }
        break;
    case NFA_CE_DATA_EVT:
        {
            ALOGD ("%s: data event notified", fn);
#if ((NXP_EXTNS == TRUE) && (NFC_NXP_ESE == TRUE))
#if(NXP_ESE_DUAL_MODE_PRIO_SCHEME != NXP_ESE_WIRED_MODE_RESUME)
            se.setDwpTranseiveState(false, NFCC_CE_DATA_EVT);
#endif
#endif
            tNFA_CE_DATA& ce_data = eventData->ce_data;
            routingManager.handleData(NFA_TECHNOLOGY_MASK_F, ce_data.p_data, ce_data.len, ce_data.status);
        }
        break;
    default:
        {
            ALOGE ("%s: unknown event=%u ????", fn, event);
        }
        break;
    }
}

int RoutingManager::registerJniFunctions (JNIEnv* e)
{
    static const char fn [] = "RoutingManager::registerJniFunctions";
    ALOGD ("%s", fn);
    return jniRegisterNativeMethods (e, "com/android/nfc/cardemulation/AidRoutingManager", sMethods, NELEM(sMethods));
}

int RoutingManager::com_android_nfc_cardemulation_doGetDefaultRouteDestination (JNIEnv*)
{
    return getInstance().mDefaultEe;
}

int RoutingManager::com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination (JNIEnv*)
{
    return getInstance().mOffHostEe;
}

int RoutingManager::com_android_nfc_cardemulation_doGetAidMatchingMode (JNIEnv*)
{
    return getInstance().mAidMatchingMode;
}


int RoutingManager::com_android_nfc_cardemulation_doGetAidMatchingPlatform(JNIEnv*)
{
    return getInstance().mAidMatchingPlatform;
}

/*
*This fn gets called when timer gets expired.
*When reader requested events (add for polling tech - tech A/tech B)comes it is expected to come back to back with in timer expiry value(50ms)
*case 1:If all the add request comes before the timer expiry , poll request for all isn handled
*case 2:If the second add request comes after timer expiry, it is not handled

*When reader requested events (remove polling tech - tech A/tech B)comes it is expected to come back to back for the add requestes before
 timer expiry happens(50ms)
*case 1:If all the removal request comes before the timer expiry , poll removal  request for all is handled
*case 2:If the only one of the removal request is reached before timer expiry, it is not handled
           :When ever the second removal request is also reached , it is handled.

*/
#if((NFC_NXP_ESE == TRUE) && (NXP_EXTNS == TRUE) && (NXP_ESE_ETSI_READER_ENABLE == TRUE))
void reader_req_event_ntf (union sigval)
{
    static const char fn [] = "RoutingManager::reader_req_event_ntf";
    ALOGD ("%s:  ", fn);
    JNIEnv* e = NULL;
    int disc_ntf_timeout = 10;
    ScopedAttach attach(RoutingManager::getInstance().mNativeData->vm, &e);
    if (e == NULL)
    {
        ALOGE ("%s: jni env is null", fn);
        return;
    }

    GetNumValue ( NAME_NFA_DM_DISC_NTF_TIMEOUT, &disc_ntf_timeout, sizeof ( disc_ntf_timeout ) );

    Rdr_req_ntf_info_t mSwp_info = RoutingManager::getInstance().getSwpRrdReqInfo();

    ALOGD ("%s: swp_rdr_req_ntf_info.swp_rd_req_info.src = 0x%4x ", fn,mSwp_info.swp_rd_req_info.src);

    if(RoutingManager::getInstance().getEtsiReaederState() == STATE_SE_RDR_MODE_START_CONFIG)
    {
        e->CallVoidMethod (RoutingManager::getInstance().mNativeData->manager, android::gCachedNfcManagerNotifyETSIReaderModeStartConfig, (UINT16)mSwp_info.swp_rd_req_info.src);
    }
    else if(RoutingManager::getInstance().getEtsiReaederState() == STATE_SE_RDR_MODE_STOP_CONFIG)
    {
        ALOGD ("%s: sSwpReaderTimer.kill() ", fn);
        SecureElement::getInstance().sSwpReaderTimer.kill();
        e->CallVoidMethod (RoutingManager::getInstance().mNativeData->manager, android::gCachedNfcManagerNotifyETSIReaderModeStopConfig,disc_ntf_timeout);
    }
}
#endif
#if(NXP_EXTNS == TRUE) && (NFC_NXP_ESE == TRUE)
extern int active_ese_reset_control;
#endif
void *ee_removed_ntf_handler_thread(void* /* data */)
{
    static const char fn [] = "ee_removed_ntf_handler_thread";
    tNFA_STATUS stat = NFA_STATUS_FAILED;
    SecureElement &se = SecureElement::getInstance();
    RoutingManager &rm = RoutingManager::getInstance();
    ALOGD ("%s: Enter: ", fn);
    rm.mResetHandlerMutex.lock();
    ALOGD ("%s: enter sEseRemovedHandlerMutex lock", fn);
#if (NXP_NFCEE_REMOVED_NTF_RECOVERY == TRUE)
    NFA_HciW4eSETransaction_Complete(Release);
#endif
#if((NFC_NXP_ESE == TRUE) && (NXP_WIRED_MODE_STANDBY == TRUE))
    if(se.mIsWiredModeOpen)
    {
        stat = se.setNfccPwrConfig(se.NFCC_DECIDES);
        if(stat != NFA_STATUS_OK)
        {
            ALOGD("%s: power link command failed", __FUNCTION__);
        }
    }
#endif
    stat = NFA_EeModeSet(0x4c0, NFA_EE_MD_DEACTIVATE);

    if(stat == NFA_STATUS_OK)
    {
        SyncEventGuard guard (se.mEeSetModeEvent);
        se.mEeSetModeEvent.wait ();
    }
#if((NFC_NXP_ESE == TRUE)&&(NXP_EXTNS == TRUE))
    se.NfccStandByOperation(STANDBY_GPIO_LOW);
    usleep(10*1000);
    se.NfccStandByOperation(STANDBY_GPIO_HIGH);
#if(NXP_WIRED_MODE_STANDBY == TRUE)
    if(se.mIsWiredModeOpen)
    {
        stat = se.setNfccPwrConfig(se.POWER_ALWAYS_ON);
        if(stat != NFA_STATUS_OK)
        {
            ALOGD("%s: power link command failed", __FUNCTION__);
        }
    }
#endif
#endif
    stat = NFA_EeModeSet(0x4c0, NFA_EE_MD_ACTIVATE);

    if(stat == NFA_STATUS_OK)
    {
        SyncEventGuard guard(se.mModeSetNtf);
        if(se.mModeSetNtf.wait (500) == FALSE)
        {
            ALOGD("%s:SetMode ntf timeout", __FUNCTION__);
        }
    }
    rm.mResetHandlerMutex.unlock();
#if(NXP_EXTNS == TRUE) && (NFC_NXP_ESE == TRUE)
    if(active_ese_reset_control & TRANS_WIRED_ONGOING)
    {
        SyncEventGuard guard(se.mTransceiveEvent);
        se.mTransceiveEvent.notifyOne();
    }
#if (NXP_ESE_DWP_SPI_SYNC_ENABLE == TRUE)
    /* restart the discovery */
    usleep(100 * 100);
    if (android::isDiscoveryStarted() == true)
    {

        android::startRfDiscovery(false);
        usleep(100 * 100);
        android::startRfDiscovery(true);
    }
#endif
#endif
    ALOGD ("%s: exit sEseRemovedHandlerMutex lock ", fn);
    ALOGD ("%s: exit ", fn);
    pthread_exit(NULL);
    return NULL;
}

void RoutingManager::ee_removed_disc_ntf_handler(tNFA_HANDLE handle, tNFA_EE_STATUS status)
{
    static const char fn [] = "RoutingManager::ee_disc_ntf_handler";
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ALOGE("%s; ee_handle=0x0%x, status=0x0%x", fn, handle, status);
    if (pthread_create (&thread, &attr,  &ee_removed_ntf_handler_thread, (void*)NULL) < 0)
    {
        ALOGD("Thread creation failed");
    }
    else
    {
        ALOGD("Thread creation success");
    }
    pthread_attr_destroy(&attr);
}
#if((NFC_NXP_ESE == TRUE )&& (NXP_EXTNS == TRUE) && (NXP_ESE_ETSI_READER_ENABLE == TRUE))
/*******************************************************************************
**
** Function:        getEtsiReaederState
**
** Description:     Get the current ETSI Reader state
**
** Returns:         Current ETSI state
**
*******************************************************************************/
se_rd_req_state_t RoutingManager::getEtsiReaederState()
{
    return swp_rdr_req_ntf_info.swp_rd_state;
}

/*******************************************************************************
**
** Function:        setEtsiReaederState
**
** Description:     Set the current ETSI Reader state
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::setEtsiReaederState(se_rd_req_state_t newState)
{
    swp_rdr_req_ntf_info.mMutex.lock();
    if(newState == STATE_SE_RDR_MODE_STOPPED)
    {
        swp_rdr_req_ntf_info.swp_rd_req_current_info.tech_mask &= ~NFA_TECHNOLOGY_MASK_A;
        swp_rdr_req_ntf_info.swp_rd_req_current_info.tech_mask &= ~NFA_TECHNOLOGY_MASK_B;

        //If all the requested tech are removed, set the hande to invalid , so that next time poll add request can be handled

        swp_rdr_req_ntf_info.swp_rd_req_current_info.src = NFA_HANDLE_INVALID;
        swp_rdr_req_ntf_info.swp_rd_req_info = swp_rdr_req_ntf_info.swp_rd_req_current_info;
    }
    swp_rdr_req_ntf_info.swp_rd_state = newState;
    swp_rdr_req_ntf_info.mMutex.unlock();
}

/*******************************************************************************
**
** Function:        getSwpRrdReqInfo
**
** Description:     get swp_rdr_req_ntf_info
**
** Returns:         swp_rdr_req_ntf_info
**
*******************************************************************************/
Rdr_req_ntf_info_t RoutingManager::getSwpRrdReqInfo()
{
    ALOGE("%s Enter",__FUNCTION__);
    return swp_rdr_req_ntf_info;
}
#endif

#if(NXP_EXTNS == TRUE)
#if (NXP_NFCEE_REMOVED_NTF_RECOVERY == TRUE)
bool RoutingManager::is_ee_recovery_ongoing()
{
    static const char fn [] = "RoutingManager::is_ee_recovery_ongoing";
    ALOGD("%s := %s", fn, ((recovery==true) ? "TRUE" : "FALSE" ));
    return recovery;
}
#endif
void RoutingManager::nfaEEConnect()
{
    /*This function is invoked in case of
     * eSE session reset, in this case we already discovered eSE before
     * hence decrement eSE count from gSeDiscoveryCount so that only
     * pending NFCEE(UICC1 & UICC2) would be rediscovered
     * */
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    nfaStat = NFA_EeConnect(EE_HCI_DEFAULT_HANDLE, NFC_NFCEE_INTERFACE_HCI_ACCESS, nfaEeCallback);
    if(nfaStat == NFA_STATUS_OK)
    {
        SyncEventGuard g (gNfceeDiscCbEvent);
        ALOGD("%s: Sem wait for gNfceeDiscCbEvent", __FUNCTION__);
        gNfceeDiscCbEvent.wait (gdisc_timeout);
    }
}
/*******************************************************************************
**
** Function:        getRouting
**
** Description:     Send GET_LISTEN_MODE_ROUTING command
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::getRouting()
{
    tNFA_STATUS nfcStat;
    nfcStat = NFC_GetRouting();
    if(nfcStat == NFA_STATUS_OK)
    {
        ALOGE ("getRouting failed. status=0x0%x", nfcStat);
    }
}

/*******************************************************************************
**
** Function:        processGetRouting
**
** Description:     Process the eventData(current routing info) received during
**                  getRouting
**                  eventData : eventData
**                  sRoutingBuff : Array containing processed data
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::processGetRoutingRsp(tNFA_DM_CBACK_DATA* eventData, UINT8* sRoutingBuff)
{
    ALOGD ("%s : Enter", __FUNCTION__);
    UINT8 xx=0,numTLVs = 0,currPos = 0,curTLVLen = 0;
    UINT8 sRoutingCurrent[256];
    numTLVs = *(eventData->get_routing.param_tlvs+1);
    /*Copying only routing Entries.
    Skipping fields,
    More                  : 1Byte
    No of Routing Entries : 1Byte*/
    memcpy(sRoutingCurrent,eventData->get_routing.param_tlvs+2,eventData->get_routing.tlv_size-2);

    while(xx < numTLVs)
    {
        curTLVLen = *(sRoutingCurrent+currPos+1);
        /*Filtering out Routing Entry corresponding to PROTOCOL_NFC_DEP*/
        if((*(sRoutingCurrent+currPos) == PROTOCOL_BASED_ROUTING)&&(*(sRoutingCurrent+currPos+(curTLVLen+1))==NFA_PROTOCOL_NFC_DEP))
        {
            currPos = currPos + curTLVLen+TYPE_LENGTH_SIZE;
        }
        else
        {
            memcpy(sRoutingBuff+android::sRoutingBuffLen,sRoutingCurrent+currPos,curTLVLen+TYPE_LENGTH_SIZE);
            currPos = currPos + curTLVLen+TYPE_LENGTH_SIZE;
            android::sRoutingBuffLen = android::sRoutingBuffLen + curTLVLen+TYPE_LENGTH_SIZE;
        }
        xx++;
    }
}
#if (NXP_NFCEE_REMOVED_NTF_RECOVERY == TRUE)
/*******************************************************************************
**
** Function:        handleSERemovedNtf()
**
** Description:     The Function checks whether eSE is Removed Ntf
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::handleSERemovedNtf()
{
    static const char fn [] = "RoutingManager::handleSERemovedNtf()";
    UINT8 ActualNumEe = SecureElement::MAX_NUM_EE;
    tNFA_EE_INFO mEeInfo [ActualNumEe];
    tNFA_STATUS nfaStat;
    ALOGE ("%s:Enter", __FUNCTION__);
    if ((nfaStat = NFA_AllEeGetInfo (&ActualNumEe, mEeInfo)) != NFA_STATUS_OK)
    {
        ALOGE ("%s: fail get info; error=0x%X", fn, nfaStat);
        ActualNumEe = 0;
    }
    else
    {
        if(( mChipId == CHIP_ID_PN65T) || (mChipId == CHIP_ID_PN66T) ||
           (mChipId == CHIP_ID_PN67T) || (mChipId == CHIP_ID_PN80T))
        {
            for(int xx = 0; xx <  ActualNumEe; xx++)
            {
               ALOGE("xx=%d, ee_handle=0x0%x, status=0x0%x", xx, mEeInfo[xx].ee_handle,mEeInfo[xx].ee_status);
                if ((mEeInfo[xx].ee_handle == 0x4C0) &&
                    (mEeInfo[xx].ee_status == 0x02))
                {
                    recovery = TRUE;
                    ee_removed_disc_ntf_handler(mEeInfo[xx].ee_handle, mEeInfo[xx].ee_status);
                    break;
                }
            }
        }
    }
}
#endif
/*******************************************************************************
**
** Function:        LmrtRspTimerCb
**
** Description:     Routing Timer callback
**
*******************************************************************************/
static void LmrtRspTimerCb(union sigval)
{
   static const char fn [] = "LmrtRspTimerCb";
   ALOGD ("%s:  ", fn);
    SyncEventGuard guard(RoutingManager::getInstance().mEeUpdateEvent);
    RoutingManager::getInstance().mEeUpdateEvent.notifyOne();
}

#if(NFC_NXP_STAT_DUAL_UICC_WO_EXT_SWITCH == TRUE)
/*******************************************************************************
 **
 ** Function:        getUiccRoute
 **
 ** Description:     returns EE Id corresponding to slot number
 **
 ** Returns:         route location
 **
 *******************************************************************************/
static jint getUiccRoute(jint uicc_slot)
{
    ALOGD ("%s: Enter slot num = %d", __FUNCTION__,uicc_slot);
    if((uicc_slot == 0x00) || (uicc_slot == 0x01))
    {
        return 0x402;
    }
    else if(uicc_slot == 0x02)
    {
        return 0x481;
    }
    else
    {
        return 0xFF;
    }
}
#endif
#endif

