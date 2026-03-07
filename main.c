//#include <ntddk.h>
#include <ntifs.h>
#include <wdf.h>
//#include <wdm.h>
//#include <psapi.h>

DRIVER_INITIALIZE DriverEntry; //contains ptr to driver object, ptr to unicode_string of driver's reg key path
//EVT_WDF_DRIVER_UNLOAD EvtUnload;
//EVT_WDF_TIMER EvtWdfTimer;
WDFTIMER  timerHandle;
HANDLE app_pid = NULL;
LARGE_INTEGER start_time, end_time, run_time;
TIME_FIELDS start_tf, end_tf, run_tf;
int min_count = 0;

//redeclare as not detected by wdf.h
DECLARE_CONST_UNICODE_STRING(
    SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)"
);

VOID PcreateProcessNotifyRoutine( //process callback function
    HANDLE ParentId,
    HANDLE ProcessId,
    BOOLEAN Create 
) {
    //mark as unreference if not using
    UNREFERENCED_PARAMETER(ParentId);
    //UNREFERENCED_PARAMETER(ProcessId);

    if (Create) {
        //check if process created
        PEPROCESS process;
        NTSTATUS status = PsLookupProcessByProcessId(ProcessId, &process);

        if (NT_SUCCESS(status)) {
            PUNICODE_STRING appName = NULL;
            NTSTATUS appStatus = SeLocateProcessImageName(process, &appName); //get process path
            if (NT_SUCCESS(appStatus)) {
                UNICODE_STRING targetAppName;
                DbgPrint("Process NT path: %wZ\n", appName);
                RtlInitUnicodeString(&targetAppName, L"Notepad.exe");
                
                //RtlInitUnicodeString(&targetAppName, L"\\Device\\HarddiskVolume3\\Microsoft\\Edge\\Application\\msedge.exe"); //set target 
                //if (RtlCompareUnicodeString(appName, &targetAppName, TRUE) == 0) { //compare if opened app is the target app
                if (RtlSuffixUnicodeString(&targetAppName, appName, TRUE)) { //compare if opened app is the target app

                    app_pid = ProcessId;//save process id
                    
                    //check start time
                    KeQuerySystemTimePrecise(&start_time);
                    RtlTimeToTimeFields(&start_time, &start_tf);

                    //timer callback every 60s to track milestones (1min, 1hr)
                    WdfTimerStart(timerHandle, WDF_REL_TIMEOUT_IN_SEC(60));
                    //display start time
                    DbgPrint("Notepad has launched with PID %llu at %02d:%02d:%02d UTC\n", (ULONGLONG)ProcessId, start_tf.Hour, start_tf.Minute, start_tf.Second);
                }

                //free block when done using
                ExFreePool(appName);
            }
            //deference obj when done using
            ObDereferenceObject(process);
        }
        
    }
    else if (ProcessId == app_pid){ //detect app close
        
        //check end time
        KeQuerySystemTimePrecise(&end_time);
        RtlTimeToTimeFields(&end_time, &end_tf);

        //calculate run time
        run_time.QuadPart = (end_time.QuadPart - start_time.QuadPart);
        RtlTimeToTimeFields(&run_time, &run_tf);

        //display end time and run time
        DbgPrint("Notepad (PID %llu) has closed at %02d:%02d:%02d UTC\n", (ULONGLONG)ProcessId, end_tf.Hour, end_tf.Minute, end_tf.Second);
        DbgPrint("Process ran for %02d:%02d:%02d", run_tf.Hour, run_tf.Minute, run_tf.Second);

        WdfTimerStop(timerHandle, TRUE); //stop wdf timer
        min_count = 0; //reset milestone
        app_pid = NULL; //reset process id to NULL
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
    }
    else {
        switch (min_count) {
        case (1):
            DbgPrint("1 minute milestone has been reached\n");
            break;
        case (5):
            DbgPrint("5 minute milestone has been reached\n");
            break;
        case (15):
            DbgPrint("15 minute milestone has been reached\n");
            break;
        case (30):
            DbgPrint("30 minute milestone has been reached\n");
            break;
        }
    }

}

VOID EvtUnload( //driver unload function
    WDFDRIVER Driver
)
{
    UNREFERENCED_PARAMETER(Driver);
    PsSetCreateProcessNotifyRoutine(PcreateProcessNotifyRoutine, TRUE);
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

    //DriverObject->DriverUnload = DriverUnload;

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
        &driver //only used as a parent object for timer
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

    //create device manually due to no hw to use as parent for timerAttributes (requires device as parent instead of driver)
    WDFDEVICE device;
    PWDFDEVICE_INIT deviceInit;

    //enable complete control over device
    deviceInit = WdfControlDeviceInitAllocate(driver, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL); 

    //create device obj
    status = WdfDeviceCreate(&deviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfControlFinishInitializing(device);

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

    return status;
}


//VOID DriverUnload(PDRIVER_OBJECT DriverObject)
//{
//    UNREFERENCED_PARAMETER(DriverObject);
//    PsSetCreateProcessNotifyRoutine(PcreateProcessNotifyRoutine, TRUE);
//    DbgPrint("Process monitor driver unloaded\n");
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