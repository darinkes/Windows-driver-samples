/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    wmi.c

Abstract: This module demonstrates how to receive WMI notification fired by
          another driver. The code here basically registers for toaster
          device arrival WMI notification fired by the toaster function driver.
          You can use similar technique to receive media sense notification
          (GUID_NDIS_STATUS_MEDIA_CONNECT/GUID_NDIS_STATUS_MEDIA_DISCONNECT)
          fired by NDIS whenever the network cable is plugged or unplugged.

Environment:

    Kernel mode

--*/

#include "toastmon.h"
#include "public.h"
#include <wmistr.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, RegisterForWMINotification)
#pragma alloc_text(PAGE, UnregisterForWMINotification)
#pragma alloc_text(PAGE, GetTargetFriendlyName)
#pragma alloc_text(PAGE, WmiNotificationCallback)
#endif

NTSTATUS
RegisterForWMINotification(
    PDEVICE_EXTENSION DeviceExt
    )
{
    NTSTATUS           status = STATUS_SUCCESS;
    GUID               wmiGuid;

    PAGED_CODE();

    //
    // Check to make sure we are not called multiple times.
    //
    ASSERT(DeviceExt->WMIDeviceArrivalNotificationObject == NULL);

    //
    // Register WMI callbacks for toaster device arrival events
    //
    wmiGuid = TOASTER_NOTIFY_DEVICE_ARRIVAL_EVENT;

    status = IoWMIOpenBlock(
                 &wmiGuid,
                 WMIGUID_NOTIFICATION,
                 &DeviceExt->WMIDeviceArrivalNotificationObject
                 );
    if (!NT_SUCCESS(status)) {

        KdPrint(("Unable to open wmi data block status 0x%x\n", status));
        DeviceExt->WMIDeviceArrivalNotificationObject = NULL;

    } else {

        status = IoWMISetNotificationCallback(
                     DeviceExt->WMIDeviceArrivalNotificationObject,
                     WmiNotificationCallback,
                     DeviceExt
                     );
        if (!NT_SUCCESS(status)) {
            KdPrint(("Unable to register for wmi notification 0x%x\n", status));
            ObDereferenceObject(DeviceExt->WMIDeviceArrivalNotificationObject);
            DeviceExt->WMIDeviceArrivalNotificationObject = NULL;
        }
    }

    return status;
}


VOID
UnregisterForWMINotification(
    PDEVICE_EXTENSION DeviceExt
    )
{
    PAGED_CODE();

    if (DeviceExt->WMIDeviceArrivalNotificationObject != NULL) {
        ObDereferenceObject(DeviceExt->WMIDeviceArrivalNotificationObject);
        DeviceExt->WMIDeviceArrivalNotificationObject = NULL;
    }
}

_Use_decl_annotations_
NTSTATUS
GetTargetFriendlyName(
    WDFIOTARGET Target,
    WDFMEMORY*  TargetName
    )
/*++

Routine Description:

    Return the friendly name associated with the given device object.

Arguments:

Return Value:

    NT status

--*/
{
    NTSTATUS status;

    PAGED_CODE();

    //
    // First get the length of the string. If the FriendlyName
    // is not there then get the lenght of device description.
    //
    status = WdfIoTargetAllocAndQueryTargetProperty(Target,
                                                    DevicePropertyFriendlyName,
                                                    NonPagedPoolNx,
                                                    WDF_NO_OBJECT_ATTRIBUTES,
                                                    TargetName);

    if (!NT_SUCCESS(status) && status != STATUS_INSUFFICIENT_RESOURCES) {
        status = WdfIoTargetAllocAndQueryTargetProperty(Target,
                                                        DevicePropertyDeviceDescription,
                                                        NonPagedPoolNx,
                                                        WDF_NO_OBJECT_ATTRIBUTES,
                                                        TargetName);

    }

    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDeviceQueryProperty returned %x\n", status));
    }

    return status;
}

VOID
WmiNotificationCallback(
    PVOID Wnode,
    PVOID Context
    )
/*++

Routine Description:

    WMI calls this function to notify the caller that the specified event has occurred.

Arguments:

    Wnode - points to the WNODE_EVENT_ITEM structure returned by the driver triggering the event.

    Context - points to the value specified in the Context parameter of the
                    IoWMISetNotificationCallback routine.

Return Value:

    NT status

--*/
{
    PWNODE_SINGLE_INSTANCE  wnode = (PWNODE_SINGLE_INSTANCE) Wnode;
    WDFMEMORY               memory;
    UNICODE_STRING          deviceName;
    PDEVICE_OBJECT          devobj;
    NTSTATUS                status;
    PDEVICE_EXTENSION       deviceExt = Context;
    WDFCOLLECTION           hCollection = deviceExt->TargetDeviceCollection;
    WDFIOTARGET             ioTarget;
    WDF_IO_TARGET_STATE     ioTargetState;
    ULONG                   i;

    PAGED_CODE();

    WdfWaitLockAcquire(deviceExt->TargetDeviceCollectionLock, NULL);

    for(i=0; i< WdfCollectionGetCount(hCollection); i++){

        ioTarget = WdfCollectionGetItem(hCollection, i);

        //
        // Before calling WdfIoTargetWdmGetTargetDeviceObject, make sure the target is still open.
        // The WdfIoTargetWdmGetXxxDeviceObject APIs can only be called while the target is opened, otherwise
        // they can return undefined values.
        //
        ioTargetState = WdfIoTargetGetState(ioTarget);
        if (ioTargetState != WdfIoTargetStarted) {
            KdPrint(("WDFIOTARGET %p not in an opened state.\n", ioTarget));
            continue;
        }

        devobj = WdfIoTargetWdmGetTargetDeviceObject(ioTarget);

        if(devobj &&
            IoWMIDeviceObjectToProviderId(devobj) == wnode->WnodeHeader.ProviderId) {

            if( IsEqualGUID( (LPGUID)&(wnode->WnodeHeader.Guid),
                          (LPGUID)&TOASTER_NOTIFY_DEVICE_ARRIVAL_EVENT) ) {
                //
                // found the device. Just for demonstration, get the friendlyname of the
                // target device and print it.
                //
                status = GetTargetFriendlyName(ioTarget, &memory);
                if (!NT_SUCCESS(status)) {
                    KdPrint(("GetDeviceFriendlyName returned %x\n", status));
                    break;
                }

                RtlInitUnicodeString(&deviceName, (PWSTR) WdfMemoryGetBuffer(memory, NULL));
                KdPrint(("%wZ fired a device arrival event\n", &deviceName));

                //
                // Free the memory allocated by GetDeviceFriendlyName.
                //
                WdfObjectDelete(memory);

                break;

            } else {
                KdPrint(("Unknown event.\n"));
            }
        }

    }

    WdfWaitLockRelease(deviceExt->TargetDeviceCollectionLock);
}
