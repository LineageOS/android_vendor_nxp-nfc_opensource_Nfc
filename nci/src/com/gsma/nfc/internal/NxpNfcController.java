/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2015 NXP Semiconductors
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
package com.gsma.nfc.internal;

import android.content.Context;
import android.content.Intent;

import java.util.List;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

import android.content.BroadcastReceiver;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.util.Log;

import com.nxp.nfc.gsma.internal.INxpNfcController;
import com.android.nfc.cardemulation.CardEmulationManager;
import com.android.nfc.cardemulation.RegisteredAidCache;

import android.nfc.cardemulation.NQApduServiceInfo;
import android.os.Binder;
import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;

import com.android.nfc.NfcPermissions;
import com.android.nfc.NfcService;
import com.nxp.nfc.NxpConstants;


public class NxpNfcController {

    private static int ROUTING_TABLE_EE_MAX_AID_CFG_LEN = 580;
    public static final int PN65T_ID = 2;
    public static final int PN66T_ID = 4;


    private Context mContext;
    final NxpNfcControllerInterface mNxpNfcControllerInterface;
    final RegisteredNxpServicesCache mServiceCache;
    private RegisteredAidCache mRegisteredAidCache;
    private CardEmulationManager mCardEmulationManager;
    private boolean mGsmaCommitOffhostService = false;
    static final String TAG = "NxpNfcControllerService";
    boolean DBG = true;

    public ArrayList<String> mEnabledMultiEvts = new ArrayList<String>();
    public final HashMap<String, Boolean> mMultiReceptionMap = new HashMap<String, Boolean>();
    private Object mWaitX509CheckCert = null;
    private boolean mHasX509Cert = false;
    private ComponentName unicastPkg = null;

    static final int X509_WAIT_TIMEOUT = 10000;

    public NxpNfcController(Context context, CardEmulationManager cardEmulationManager) {
        mContext = context;
        mCardEmulationManager = cardEmulationManager;
        mServiceCache = cardEmulationManager.getRegisteredNxpServicesCache();
        mRegisteredAidCache = cardEmulationManager.getRegisteredAidCache();
        mNxpNfcControllerInterface = new NxpNfcControllerInterface();
    }

    public INxpNfcController getNxpNfcControllerInterface() {
        if(mNxpNfcControllerInterface != null) {
            if(DBG) Log.d(TAG, "GSMA: mNxpNfcControllerInterface is not Null");
            return mNxpNfcControllerInterface;
        }
        return null;
    }

    public ArrayList<String> getEnabledMultiEvtsPackageList() {
        return mEnabledMultiEvts;
    }

    private boolean checkCertificatesFromUICC(String pkg, String seName) {
        // FIXME: Should this be ACTION_CHECK_CERT ?
        return checkX509CertificatesFromSim(pkg, seName);
   }

    public void setResultForCertificates(boolean result) {
        // FIXME: Might expect ACTION_CHECK_CERT
        setResultForX509Certificates(result);
    }

    // "org.simalliance.openmobileapi.service.ACTION_CHECK_X509
    private boolean checkX509CertificatesFromSim (String pkg, String seName) {
        if (DBG) Log.d(TAG, "checkX509CertificatesFromSim() " + pkg + ", " + seName);

        Intent checkX509CertificateIntent = new Intent();
        checkX509CertificateIntent.setAction(NxpConstants.ACTION_CHECK_X509);
        checkX509CertificateIntent.setPackage(NxpConstants.SET_PACKAGE_NAME);
        checkX509CertificateIntent.putExtra(NxpConstants.EXTRA_SE_NAME, seName);
        checkX509CertificateIntent.putExtra(NxpConstants.EXTRA_PKG, pkg);
        mContext.sendBroadcast(checkX509CertificateIntent);

        mWaitX509CheckCert = new Object();
        mHasX509Cert = false;
        try {
            synchronized (mWaitX509CheckCert) {
                mWaitX509CheckCert.wait(X509_WAIT_TIMEOUT); //add timeout 10s
            }
        } catch (InterruptedException e) {
            // Should not happen; fall-through to abort.
            Log.w(TAG, "checkX509CertificatesFromSim(): interrupted");
        }
        mWaitX509CheckCert = null;

        if (mHasX509Cert) {
            return true;
        } else {
            return false;
        }
    }


    public void setResultForX509Certificates(boolean result) {
        Log.d(TAG, "setResultForX509Certificates() Start, result: " + result);
        if (mWaitX509CheckCert != null) {
            synchronized (mWaitX509CheckCert) {
                if (result) {
                    mHasX509Cert = true;
                } else {
                    mHasX509Cert = false;
                }
                mWaitX509CheckCert.notify();
            }
        }
        Log.d(TAG, "setResultForX509Certificates() End");
    }

    public boolean isGsmaCommitOffhostService() {
        return mGsmaCommitOffhostService;
    }

    static byte[] hexStringToBytes(String s) {
        if (s == null || s.length() == 0) return null;
        int len = s.length();
        if (len % 2 != 0) {
            s = '0' + s;
            len++;
        }
        byte[] data = new byte[len / 2];
        for (int i = 0; i < len; i += 2) {
            data[i / 2] = (byte) ((Character.digit(s.charAt(i), 16) << 4)
                    + Character.digit(s.charAt(i + 1), 16));
        }
        return data;
    }

    private long getApplicationInstallTime(String packageName) {
        PackageManager pm = mContext.getPackageManager();
        try {
            PackageInfo pInfo = pm.getPackageInfo(packageName ,0);
            return pInfo.firstInstallTime;
        }catch(NameNotFoundException exception) {
            Log.e(TAG, "Application install time not retrieved");
            return 0;
        }
    }

    private ComponentName getPackageListUnicastMode (Intent intent) {
        unicastPkg = null;
        List<NQApduServiceInfo> regServices = new ArrayList<NQApduServiceInfo>(mCardEmulationManager.getAllServices());
        if(DBG) Log.d(TAG, "getPackageListUnicastMode(): regServices.size() " + regServices.size());

        PackageManager pm = mContext.getPackageManager();
        /*
         * FIXME: we can use queryBroadcastReceivers to get partial matches
         * FIXME: we cannot use queryIntentActivities for partial match
        List<ResolveInfo> intentBroadcastReceivers = pm.queryBroadcastReceivers(
                new Intent(NxpConstants.ACTION_MULTI_EVT_TRANSACTION,
                        Uri.parse("nfc://secure:0/SIM")),
                        (PackageManager.MATCH_DEFAULT_ONLY |
                                PackageManager.GET_INTENT_FILTERS |
                                PackageManager.GET_RESOLVED_FILTER));
        if(DBG) Log.d(TAG, "getPackageListUnicastMode() : intentBroadcastReceivers.size() " + intentBroadcastReceivers.size());
         */
        List<ResolveInfo> intentReceivers = pm.queryIntentActivities(
                intent,
                (PackageManager.MATCH_DEFAULT_ONLY |
                        PackageManager.GET_INTENT_FILTERS |
                        PackageManager.GET_RESOLVED_FILTER));
        if(DBG) Log.d(TAG, "getPackageListUnicastMode() : intentReceivers.size() " + intentReceivers.size());

        ArrayList<String> apduResolvedServices = new ArrayList<String>();
        String packageName = null;
        String resolvedApduService = null;
        int highestPriority = -1000;
        long minInstallTime;
        ResolveInfo resolveInfoService = null;

        for(NQApduServiceInfo service : regServices) {
            packageName = service.getComponent().getPackageName();
            for(ResolveInfo resInfo : intentReceivers) {
                resolveInfoService = null;
                Log.d(TAG, "Registered Activity in resolved cache " + resInfo.activityInfo.packageName);
                if(resInfo.activityInfo.packageName.equals(packageName)) {
                    resolveInfoService = resInfo;
                    break;
                }
            }
            if(resolveInfoService == null) {
                Log.e(TAG, "Registered Activity is not found in cache");
                continue;
            }
            int priority = resolveInfoService.priority;
            if((pm.checkPermission(NxpConstants.PERMISSIONS_TRANSACTION_EVENT , packageName) == PackageManager.PERMISSION_GRANTED) &&
                    (pm.checkPermission(NxpConstants.PERMISSIONS_NFC , packageName) == PackageManager.PERMISSION_GRANTED))
            {
                if((checkCertificatesFromUICC(packageName, "SIM") == true) ||
                    (checkCertificatesFromUICC(packageName, "SIM1") == true))
                {
                    if(priority == highestPriority) {
                        apduResolvedServices.add(packageName);
                    } else if(highestPriority < priority) {
                        highestPriority = priority;
                        apduResolvedServices.clear();
                        apduResolvedServices.add(packageName);
                    }
                }
            }
        }
        if(apduResolvedServices.size() == 0x00) {
            Log.e(TAG, "No services to resolve, not starting the activity");
            return unicastPkg;
        }else if(apduResolvedServices.size() > 0x01) {
            Log.d(TAG, "apduResolvedServices.size(): " + apduResolvedServices.size());
            minInstallTime = getApplicationInstallTime(apduResolvedServices.get(0));
            for(String resolvedService : apduResolvedServices) {
                if(getApplicationInstallTime(resolvedService) <= minInstallTime ) {
                    minInstallTime = getApplicationInstallTime(resolvedService);
                    resolvedApduService = resolvedService;
                }
                Log.d(TAG, "Install time  of application"+ minInstallTime);
            }

        } else  resolvedApduService = apduResolvedServices.get(0);

        Log.d(TAG, "Final Resolved Service: " + resolvedApduService);
        if(resolvedApduService != null) {
            for(ResolveInfo resolve : intentReceivers) {
                if(resolve.activityInfo.packageName.equals(resolvedApduService)) {
                    unicastPkg = new ComponentName(resolvedApduService, resolve.activityInfo.name);
                    break;
                }
            }
        }
        return unicastPkg;
    }

    public ComponentName getUnicastPackage(Intent intent) {
        return getPackageListUnicastMode(intent);
    }

    final class NxpNfcControllerInterface extends INxpNfcController.Stub {

        @Override
        public boolean deleteOffHostService(int userId, String packageName, NQApduServiceInfo service) {
            return mServiceCache.deleteApduService(userId, Binder.getCallingUid(), packageName, service);
        }

        @Override
        public ArrayList<NQApduServiceInfo> getOffHostServices(int userId, String packageName) {
            return mServiceCache.getApduServices(userId, Binder.getCallingUid(), packageName);
        }

        @Override
        public NQApduServiceInfo getDefaultOffHostService(int userId, String packageName) {
            if(DBG) Log.d(TAG, "getDefaultOffHostService: enter(), userId: " +userId + " packageName: " + packageName);
            HashMap<ComponentName, NQApduServiceInfo> mapServices = mServiceCache.getApduservicesMaps();
            ComponentName preferredPaymentService = mRegisteredAidCache.getPreferredPaymentService();
            if(preferredPaymentService != null) {
                if(preferredPaymentService.getPackageName() != null &&
                    !preferredPaymentService.getPackageName().equals(packageName)) {
                    Log.d(TAG, "getDefaultOffHostService: unregistered package Name");
                    return null;
                }
                String defaultservice = preferredPaymentService.getClassName();

                //If Default is Dynamic Service
                for (Map.Entry<ComponentName, NQApduServiceInfo> entry : mapServices.entrySet())
                {
                    if(defaultservice.equals(entry.getKey().getClassName())) {
                        Log.d(TAG, "getDefaultOffHostService: Dynamic: "+ entry.getValue().getAids().size());
                        return entry.getValue();
                    }
                }

                //If Default is Static Service
                HashMap<ComponentName, NQApduServiceInfo>  staticServices = mServiceCache.getInstalledStaticServices();
                for (Map.Entry<ComponentName, NQApduServiceInfo> entry : staticServices.entrySet()) {
                    if(defaultservice.equals(entry.getKey().getClassName())) {
                        Log.d(TAG, "getDefaultOffHostService: Static: "+ entry.getValue().getAids().size());
                        return entry.getValue();
                    }
                }
            }
            return null;
        }

        @Override
        public boolean commitOffHostService(int userId, String packageName, String serviceName, NQApduServiceInfo service) {
            int aidLength = 0;
            boolean is_table_size_required = true;
            List<String>  newAidList = new ArrayList<String>();
            List<String>  oldAidList = new ArrayList<String>();

            for (int i=0; i<service.getAids().size(); i++){   // Convering String AIDs to Aids Length
                aidLength = aidLength + hexStringToBytes(service.getAids().get(i)).length;
            }
            Log.d(TAG, "Total commiting aids Length:  "+ aidLength);

            ArrayList<NQApduServiceInfo> serviceList = mServiceCache.getApduServices(userId, Binder.getCallingUid(), packageName);
           for(int i=0; i< serviceList.size(); i++) {
                Log.d(TAG, "All Service Names["+i +"] "+ serviceList.get(i).getComponent().getClassName());
                if(serviceName.equalsIgnoreCase(serviceList.get(i).getComponent().getClassName())) {
                    oldAidList = serviceList.get(i).getAids();
                    newAidList = service.getAids();
                    Log.d(TAG, "Commiting Existing Service:  "+ serviceName);
                    break;
                }
           }

           int newAidListSize;
           for(newAidListSize = 0; newAidListSize < newAidList.size(); newAidListSize++) {
               if(!oldAidList.contains(newAidList.get(newAidListSize))) {
                   is_table_size_required = true;             // Need to calculate Roting table Size, if New Aids Added
                   Log.d(TAG, "New Aids Added  ");
                   break;
               }
           }

           if((newAidList.size() != 0) && (newAidListSize == newAidList.size())) {
               is_table_size_required = false;        // No Need to calculate Routing size
           }

           Log.d(TAG, "is routing Table size calcution required :  "+ is_table_size_required);
           /* Checking for table size availability is not required here as the default AID route may be changed by routing manager
            * to accomodate more AID's.
            * if((is_table_size_required == true) && NfcService.getInstance().getRemainingAidTableSize() < aidLength) {
               return false;
           }*/

           Log.d(TAG, "Commiting :  ");
           mGsmaCommitOffhostService = true;
           mServiceCache.registerApduService(userId, Binder.getCallingUid(), packageName, serviceName, service);
           /*After commitRouting is done, status shall be updated by AidRoutingManager.
            * if there is any overflow of RoutingTable, status shall be false & the same is returned to caller*/
           mGsmaCommitOffhostService = false;
           boolean isCommitSuccess = NfcService.getInstance().getLastCommitRoutingStatus();
           Log.d(TAG, "CommitStatus : "+isCommitSuccess);
           return isCommitSuccess;
        }

        @Override
        public boolean enableMultiEvt_NxptransactionReception(String packageName, String seName) {
            boolean result = false,resolveStat = false;
            PackageManager pm = mContext.getPackageManager();
            List<ResolveInfo> intentServices = pm.queryIntentActivities(
                    new Intent(NxpConstants.ACTION_MULTI_EVT_TRANSACTION),
                    PackageManager.GET_INTENT_FILTERS| PackageManager.GET_RESOLVED_FILTER);

            for(ResolveInfo resInfo : intentServices){
                Log.e(TAG, " Registered Service in resolved cache"+resInfo.activityInfo.packageName);
                if(resInfo.activityInfo.packageName.equals(packageName)) {
                    resolveStat = true;
                    break;
                }
            }

            if((resolveStat) && (pm.checkPermission(NxpConstants.PERMISSIONS_TRANSACTION_EVENT , packageName) == PackageManager.PERMISSION_GRANTED) &&
                    (pm.checkPermission(NxpConstants.PERMISSIONS_NFC , packageName) == PackageManager.PERMISSION_GRANTED) &&
                    checkCertificatesFromUICC(packageName, seName) == true) {
                mEnabledMultiEvts.add(packageName);
                result = true;
            } else {
                result = false;
            }

            return result;
        }

        @Override
        public void enableMultiReception(String pkg, String seName) {
            if (DBG) Log.d(TAG, "enableMultiReception() " + pkg + " " + seName);

            if (seName.startsWith("SIM")) {
                if (checkX509CertificatesFromSim (pkg, seName) == false) {
                    throw new SecurityException("No cerficates from " + seName);
                }
            } else {
                NfcService.getInstance().enforceNfceeAdminPerm(pkg);
                //NfcPermissions.enforceAdminPermissions(mContext);
            }

            mMultiReceptionMap.remove(seName);
            mMultiReceptionMap.put(seName, Boolean.TRUE);
        }
    }
}
