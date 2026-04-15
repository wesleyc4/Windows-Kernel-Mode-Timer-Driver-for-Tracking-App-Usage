//#include <ntddk.h>
#include <ntifs.h>
#include <wdf.h>
//#include <wdm.h>
//#include <psapi.h>

/*#define CTL_CODE( DeviceType, Function, Method, Access ) (                 \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
)*/

//IOCTL control code
//DeviceType set to unknown (no hw), Function = 0x800 – 0xFFF , method buffered in same memory region for both i/o, access = read/write
#define IOCTL_Device_Function \
 CTL_CODE (0x22, 0x800, 0, 0) 

DRIVER_INITIALIZE DriverEntry; //contains ptr to driver object, ptr to unicode_string of driver's reg key path
WDFTIMER  timerHandle;
WDFQUEUE defaultQueue;
WDFQUEUE  manualQueue;
HANDLE app_pid = NULL;
LARGE_INTEGER start_time, end_time, run_time;
TIME_FIELDS start_tf, end_tf, run_tf;
int min_count = 0;
BOOLEAN unload = FALSE;


//redeclare as not detected by wdf.h
DECLARE_CONST_UNICODE_STRING(
    SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)"
);

//buffer between kernel mode and user mode
typedef struct _BUFFER {
    ULONG ProcessId;
    ULONG Type; // 1 = start, 2 = close
    WCHAR AppName[260]; //cant pass UNICODE_STRING between kernel and user mode
} BUFFER, * PBUFFER;

VOID EvtIoDeviceControl( //callback function for processing user mode requests
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode //IOCTL associated with the request
) {
    NTSTATUS status;
    UNREFERENCED_PARAMETER(InputBufferLength); //dont need to receive anything from user mode
    UNREFERENCED_PARAMETER(Queue);

    //receive IOCTL from user mode for driver to complete when event occurs (inverted call method)
    DbgPrint("IOCTL now received by EvtIoDeviceControl\n");
    switch (IoControlCode) {
    case(IOCTL_Device_Function):
        if (OutputBufferLength < sizeof(BUFFER)) { //error in output buffer as its too small
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        
        status = WdfRequestForwardToIoQueue(Request, manualQueue);
        DbgPrint("Request added to queue\n");
        if (!NT_SUCCESS(status)) {
            DbgPrint("WdfRequestForwardToIoQueue failed: 0x%X\n", status);
            WdfRequestComplete(Request, status);
        }
        break;
    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}

VOID CompleteNextRequest(ULONG ProcessId, ULONG Type, WCHAR AppName[]) {
    NTSTATUS status;
    WDFREQUEST request;
    size_t OutputBufferLength;
    PBUFFER buffer;

    if (manualQueue == NULL) {
        DbgPrint("CompleteNextRequest failed, manualQueue is NULL\n");
        return;
    }
    status = WdfIoQueueRetrieveNextRequest(
        manualQueue,
        &request
    );
    if (status == STATUS_NO_MORE_ENTRIES) {
        DbgPrint("No more entries in queue, exiting: 0x%X\n", status);
        return;
    }
    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfIoQueueRetrieveNextRequest failed: 0x%X\n", status);
        WdfRequestComplete(request, status);
        return;
    }
    DbgPrint("WdfIoQueueRetrieveNextRequest successful\n");

    //request buffer to send to user mode program
    status = WdfRequestRetrieveOutputBuffer(
        request,
        sizeof(BUFFER), //output buffer size
        &buffer, //ptr to output buffer
        &OutputBufferLength //size of data in output buffer
    );

    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfRequestRetrieveOutputBuffer failed: 0x%X\n", status);
        WdfRequestComplete(request, status); //error
        return;
    }
    else {
        //write to output buffer for user mode to receive
        DbgPrint("WdfRequestRetrieveOutputBuffer successful\n");
        buffer->ProcessId = ProcessId;
        buffer->Type = Type;

        RtlCopyMemory(
            buffer->AppName,
            AppName,
            (wcslen(AppName) + 1) * sizeof(WCHAR)
        );

        //successful completion and returns bytes written/read
        WdfRequestCompleteWithInformation(request, status, sizeof(BUFFER));

    }
}

VOID PcreateProcessNotifyRoutine( //process callback function
    HANDLE ParentId,
    HANDLE ProcessId,
    BOOLEAN Create 
) {
    //mark as unreference if not using
    UNREFERENCED_PARAMETER(ParentId);

    if (unload) {
        DbgPrint("Ending PcreateProcessNotifyRoutine");
        return;
    }

    if (Create) {
        //check if process created
        PEPROCESS process;
        NTSTATUS status = PsLookupProcessByProcessId(ProcessId, &process);

        if (NT_SUCCESS(status)) {
            PUNICODE_STRING appName = NULL;
            NTSTATUS appStatus = SeLocateProcessImageName(process, &appName); //get process path
            if (NT_SUCCESS(appStatus)) {

                UNICODE_STRING targetAppName;
                DbgPrint("Process NT path: %wZ\n", appName); //print NT path for every process that is opened
                RtlInitUnicodeString(&targetAppName, L"Notepad.exe"); //set target app name
              
                if (RtlSuffixUnicodeString(&targetAppName, appName, TRUE)) { //compare if opened app is the target app

                    app_pid = ProcessId;//save process id
                    
                    //check start time
                    KeQuerySystemTimePrecise(&start_time);
                    RtlTimeToTimeFields(&start_time, &start_tf);

                    //timer callback every 60s to track milestones (1min, 1hr)
                    if (!WdfTimerStart(timerHandle, WDF_REL_TIMEOUT_IN_SEC(60))) { //prevents restarting if multiple of app is opened
                        WdfTimerStart(timerHandle, WDF_REL_TIMEOUT_IN_SEC(60));
                    }
                    //display start time
                    DbgPrint("Notepad has launched with PID %llu at %02d:%02d:%02d UTC\n", (ULONGLONG)ProcessId, start_tf.Hour, start_tf.Minute, start_tf.Second);
                    CompleteNextRequest((ULONG)(ULONG_PTR) app_pid, 1, L"Notepad.exe"); 
                }

                //free block when done using
                ExFreePool(appName);
            }
            //free reference kernel obj
            ObDereferenceObject(process);
        }
        
    }
    else if (app_pid != NULL && ProcessId == app_pid){ //detect app close
        
        //check end time
        KeQuerySystemTimePrecise(&end_time);
        RtlTimeToTimeFields(&end_time, &end_tf);

        //calculate run time
        run_time.QuadPart = (end_time.QuadPart - start_time.QuadPart);
        RtlTimeToTimeFields(&run_time, &run_tf);

        //display end time and run time
        DbgPrint("Notepad (PID %llu) has closed at %02d:%02d:%02d UTC\n", (ULONGLONG)ProcessId, end_tf.Hour, end_tf.Minute, end_tf.Second);
        DbgPrint("Process ran for %02d:%02d:%02d\n", run_tf.Hour, run_tf.Minute, run_tf.Second);

        WdfTimerStop(timerHandle, TRUE); //stop wdf timer
        min_count = 0; //reset milestone
        app_pid = NULL; //reset process id to NULL

        CompleteNextRequest((ULONG)(ULONG_PTR)app_pid, 2, L"Notepad.exe");
    }
}

VOID EvtWdfTimer( //timer callback function
    WDFTIMER  Timer
) {
    UNREFERENCED_PARAMETER(Timer);
    min_count++; //increase minute counter

    //check if any milestones have been reached
    //DbgPrint("Timer callback function called\n");
    if (min_count % 60 == 0) {
        DbgPrint("%d hour milestone has been reached\n", min_count/60);
        CompleteNextRequest((ULONG)(ULONG_PTR)app_pid, 3, L"Notepad.exe");
    }
    else {
        switch (min_count) {
        case (1):
            DbgPrint("1 minute milestone has been reached\n");
            CompleteNextRequest((ULONG)(ULONG_PTR)app_pid, 3, L"Notepad.exe");
            break;
        case (5):
            DbgPrint("5 minute milestone has been reached\n");
            CompleteNextRequest((ULONG)(ULONG_PTR)app_pid, 3, L"Notepad.exe");
            break;
        case (15):
            DbgPrint("15 minute milestone has been reached\n");
            CompleteNextRequest((ULONG)(ULONG_PTR)app_pid, 3, L"Notepad.exe");
            break;
        case (30):
            DbgPrint("30 minute milestone has been reached\n");
            CompleteNextRequest((ULONG)(ULONG_PTR)app_pid, 3, L"Notepad.exe");
            break;
        }
    }

}

VOID EvtIoStop(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    ULONG ActionFlags // > 0 if can suspend/remove/cancel
)
{
    UNREFERENCED_PARAMETER(Queue);
    //DbgPrint("EvtIoStop print test\n");
    //cancels all requests forwarded but not yet retrieved
    if (ActionFlags & (WdfRequestStopActionPurge || WdfRequestStopActionSuspend)) {
        DbgPrint("EvtIoStop running...\n");
        WdfRequestComplete(Request, STATUS_CANCELLED);
    }
}

VOID EvtUnload( //driver unload function
    WDFDRIVER Driver
)
{
    UNREFERENCED_PARAMETER(Driver);
    unload = TRUE;

    //stop process callback
    PsSetCreateProcessNotifyRoutine(PcreateProcessNotifyRoutine, TRUE);

    //stop timer if still on
    if (timerHandle) {
        WdfTimerStop(timerHandle, TRUE);
    }

    if (manualQueue) {
        DbgPrint("Cancelling all IOCTL requests in queue\n");
        //WdfIoQueuePurgeSynchronously(manualQueue); //mark queue as purge to call EvtIoStop
        WdfIoQueueStopAndPurgeSynchronously(manualQueue);
    }

    DbgPrint("Driver unloaded\n");
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT     DriverObject,
    _In_ PUNICODE_STRING    RegistryPath
)
{
    DbgPrint(("DriverEntry reached dbgprint\n"));

    //record DriverEntry success
    NTSTATUS status = STATUS_SUCCESS;

    //driver configuration object
    WDF_DRIVER_CONFIG config;

    //initialize config
    WDF_DRIVER_CONFIG_INIT(&config,
        WDF_NO_EVENT_CALLBACK
    ); 
    config.DriverInitFlags = WdfDriverInitNonPnpDriver; //no hw
    config.EvtDriverUnload = EvtUnload; //enable driver unload

    // create the driver object
    WDFDRIVER driver;
    status = WdfDriverCreate(DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        &driver //parent object for timer and io queue
    );

    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfDriverCreate failed\n");
        return status;
    }

    DbgPrint("WdfDriverCreate successful\n");
    
    //wdf timer create
    WDF_TIMER_CONFIG  timerConfig;
    WDF_OBJECT_ATTRIBUTES  timerAttributes;

    //initialize timer config to be periodic every 60s
    WDF_TIMER_CONFIG_INIT_PERIODIC(
        &timerConfig,
        EvtWdfTimer,
        60000
    );

    //create device manually due to no hw to use as parent for timerAttributes and ioQueue(requires device as parent instead of driver)
    WDFDEVICE device;
    PWDFDEVICE_INIT deviceInit;

    //enable complete control over device
    deviceInit = WdfControlDeviceInitAllocate(driver, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL); 

    //name device as required to create symbolic link
    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, L"\\Device\\WdfDevice");

    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //create device obj
    status = WdfDeviceCreate(&deviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    //create symbolic link to allow application to find driver since it is non-pnp
    UNICODE_STRING dosDeviceName;
    RtlInitUnicodeString(&dosDeviceName, L"\\DosDevices\\WdfDevice");
    WdfDeviceCreateSymbolicLink(
        device,
        &dosDeviceName
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //iniialize WDF object attributes struct to default
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    //inherit device from timer
    timerAttributes.ParentObject = device;

    //create timer
    status = WdfTimerCreate(
        &timerConfig,
        &timerAttributes,
        &timerHandle
    );

    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfTimerCreate failed: 0x%X\n", status);
        return status;
    }

    DbgPrint("WdfTimerCreate successful\n");

    //process callback
    status = PsSetCreateProcessNotifyRoutine(PcreateProcessNotifyRoutine, FALSE);

    if (!NT_SUCCESS(status)) {
        DbgPrint("PsSetCreateProcessNotifyRoutine failed\n");
        return status;
    }

    DbgPrint("PsSetCreateProcessNotifyRoutine successful\n");

    WDF_IO_QUEUE_CONFIG defaultQueueConfig;
    WDF_IO_QUEUE_CONFIG manualQueueConfig;
    //initialize queue to retrieve requests with WdfIoQueueRetrieveNextRequest

    //default queue (automatic)
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &defaultQueueConfig,
        WdfIoQueueDispatchSequential
    );

    defaultQueueConfig.EvtIoDeviceControl = EvtIoDeviceControl;


    status = WdfIoQueueCreate(
        device,
        &defaultQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &defaultQueue
    );

    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfIoQueueCreate failed (default queue): 0x%X\n", status);
        return status;
    }

    //manual queue
    WDF_IO_QUEUE_CONFIG_INIT(
        &manualQueueConfig,
        WdfIoQueueDispatchManual
    );

    //enable queue to be stopped
    manualQueueConfig.EvtIoStop = EvtIoStop;

    //create manual io queue
    status = WdfIoQueueCreate(
        device,
        &manualQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &manualQueue
    );

    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfIoQueueCreate failed (manual queue): 0x%X\n", status);
        return status;
    }

    DbgPrint("WdfIoQueueCreate successful\n");


    //complete device initializing
    WdfControlFinishInitializing(device);

    return status;
}

/*
cd C:\DriverTest\Drivers
sc create TestDriverRun1 binPath= "C:\DriverTest\Drivers\TestDriverRun1.sys" type= kernel start= demand
sc start TestDriverRun1
sc stop TestDriverRun1
sc delete TestDriverRun1
*/


//old stuff not used

//VOID DriverUnload(PDRIVER_OBJECT DriverObject)
//{
//    UNREFERENCED_PARAMETER(DriverObject);
//    PsSetCreateProcessNotifyRoutine(PcreateProcessNotifyRoutine, TRUE);
//    DbgPrint("Process monitor driver unloaded\n");
//}
//DriverObject->DriverUnload = DriverUnload;

//UCHAR* appName = PsGetProcessImageFileName(process);
////case insensitive file suffix compare to determine matching name
//if ((_stricmp((char*) appName, "msedge.exe") == 0) || (_stricmp((char*)appName, "Microsoft Edge.lnk") == 0)){
//    app_pid = ProcessId;
//    DbgPrint("Microsoft Edge has launched with PID %llu\n", (ULONGLONG)ProcessId);
//}

////release reference kernel obj
//ObDereferenceObject(process);

//ptr to string literal
//RtlInitUnicodeString(&appName, L"msedge.exe");
//RtlInitUnicodeString(&appShortcut, L"Microsoft Edge.lnk");

//compare file suffix to determine matching name
//if (RtlSuffixUnicodeString(&appName, CreateInfo->ImageFileName, TRUE) ||
//    RtlSuffixUnicodeString(&appShortcut, CreateInfo->ImageFileName, TRUE)) {
//    app_pid = ProcessId;
//    DbgPrint("Microsoft Edge has launched with PID %llu\n", (ULONGLONG)ProcessId);
//}

//typedef struct _PS_CREATE_NOTIFY_INFO {
//    SIZE_T              Size;
//    union {
//        ULONG Flags;
//        struct {
//            ULONG FileOpenNameAvailable : 1; //if ImageFileName contains exact name for .exe
//            ULONG IsSubsystemProcess : 1; //if not win32
//            ULONG Reserved : 30;
//        };
//    };
//    HANDLE              ParentProcessId; //parent process id of new process
//    CLIENT_ID           CreatingThreadId; //process id (CreatingThreadId->UniqueProcess) and thread id of process(CreatingThreadId->UniqueThread) that created new process
//    struct _FILE_OBJECT* FileObject; //ptr to file object of process executable file
//    PCUNICODE_STRING    ImageFileName; //ptr to unicode_string of file name
//    PCUNICODE_STRING    CommandLine; //ptr to unicode_string with command that runs process
//    NTSTATUS            CreationStatus; //NTSTATUS return value
//} PS_CREATE_NOTIFY_INFO, * PPS_CREATE_NOTIFY_INFO;

/*
NTSTATUS - standard return type for success, error, info, warnings,
WDF_DRIVER_CONFIG - gets initialized by WDF_DRIVER_CONFIG_INIT
WDF_DRIVER_CONFIG_INIT - initializes with an optional PFN_WDF_DRIVER_DEVICE_ADD
NTSTATUS WdfDriverCreate(
  [in]            PDRIVER_OBJECT         DriverObject, //ptr to driver_object
  [in]            PCUNICODE_STRING       RegistryPath, //unicode path to reg key
  [in, optional]  PWDF_OBJECT_ATTRIBUTES DriverAttributes, //ptr to WDF_OBJECT_ATTRIBUTES structure
  [in]            PWDF_DRIVER_CONFIG     DriverConfig, //ptr to WDF_DRIVER_CONFIG
  [out, optional] WDFDRIVER              *Driver //ptr to location that receives handle to driver obj
);
*/