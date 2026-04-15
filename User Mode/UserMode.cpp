#include <windows.h>
#include <stdio.h>
#include <string>

//buffer between kernel mode and user mode
typedef struct _BUFFER {
    ULONG ProcessId;
    ULONG Type;
    WCHAR AppName[260];
} BUFFER, * PBUFFER;

#define IOCTL_Device_Function \
 CTL_CODE (0x22, 0x800, 0, 0) 

void timerMilestone(int milestoneCount, int time, std::string minuteHour);

int main()
{
    HANDLE WdfDevice = CreateFile(
        L"\\\\.\\WdfDevice", // \\.\ prefix for \DosDevices
        GENERIC_READ | GENERIC_WRITE, //read or write
        0, //no share
        NULL, //default security
        OPEN_EXISTING, //open existing file
        FILE_ATTRIBUTE_NORMAL, //normal file
        NULL); //no template file
    if (WdfDevice == INVALID_HANDLE_VALUE) {
        printf("Failed to create device handle\n");
        return 1;
    }

    printf("User mode has connected to WdfDevice\n");

    BUFFER buffer;
    DWORD outputBufferLen;
    BOOL event;
    int milestoneCount = 0;
    ULONG ProcessId;

    //listen to events
    while (1) {
           
        event = DeviceIoControl(
            WdfDevice, //device
            IOCTL_Device_Function, //io ctl code
            NULL, //ptr to input buffer
            0, //input buffer size
            &buffer, //ptr to output buffer
            sizeof(BUFFER), //output buffer size
            &outputBufferLen, //size of data in output buffer
            NULL //ptr to overlapped struct 
        );

        if (event == 1) {
            if (buffer.Type == 1) { //launch
                ProcessId = buffer.ProcessId;
                printf("%ls has launched with PID %lu\n", buffer.AppName, buffer.ProcessId);
                system("powershell -Command \""
                       "Add-Type -AssemblyName System.Windows.Forms;" //loads Windows.Forms for windows gui features
                       "Add-Type -AssemblyName System.Drawing;" //loads Drawing for windows gui features
                       "$notify = New-Object System.Windows.Forms.NotifyIcon;"
                       "$notify.Icon = [System.Drawing.SystemIcons]::Information;" //displays (i) icon in tray
                       "$notify.BalloonTipIcon = 'Info';" //displays (i) icon inside notification
                       "$notify.BalloonTipTitle = 'App Monitor';" //title of notification
                       "$notify.BalloonTipText = 'Notepad.exe has now launched';" //description of notification
                       "$notify.Visible = $true;" //display notification
                       "$notify.ShowBalloonTip(10000);" // for 10 seconds
                       "Start-Sleep -Seconds 15;" // keep script on for 15s
                       "\""
                );
                //printf("app has launched with PID %lu\n", buffer.ProcessId);
            } 
            else if (buffer.Type == 2) { //close
                printf("%ls has closed with PID %lu\n", buffer.AppName, ProcessId);
                system("powershell -Command \""
                       "Add-Type -AssemblyName System.Windows.Forms;"
                       "Add-Type -AssemblyName System.Drawing;"
                       "$notify = New-Object System.Windows.Forms.NotifyIcon;"
                       "$notify.Icon = [System.Drawing.SystemIcons]::Information;"
                       "$notify.BalloonTipIcon = 'Info';"
                       "$notify.BalloonTipTitle = 'App Monitor';"
                       "$notify.BalloonTipText = 'Notepad.exe has closed';"
                       "$notify.Visible = $true;"
                       "$notify.ShowBalloonTip(10000);"
                       "Start-Sleep -Seconds 15;"
                       "\""
                );
                //printf("app has closed with PID %lu\n", buffer.ProcessId);
            }
            else if (buffer.Type == 3) { //milestone
                milestoneCount++;
                int time = 0;
                std::string minuteHour = "minutes";
                switch (milestoneCount) {
                    case(1):
                        time = 1;
                        minuteHour = "minute";
                        break;
                    case(2):
                        time = 5;
                        break;
                    case(3):
                        time = 15;
                        break;
                    case(4):
                        time = 30;
                        break;
                    default:
                        time = milestoneCount - 4;
                        minuteHour = "hours";
                        break;
                }
                timerMilestone(milestoneCount, time, minuteHour);
            }
        }
        else {
            printf("DeviceIoControl failed: %lu\n", GetLastError());
        }
       
    }
}

void timerMilestone(int milestoneCount, int time, std::string minuteHour) {

    std::string printMilestone = "Notepad.exe has now ran for " + std::to_string(time) + " " + minuteHour;
    std::string command = "powershell -Command \""
                          "Add-Type -AssemblyName System.Windows.Forms;" 
                          "Add-Type -AssemblyName System.Drawing;" 
                          "$notify = New-Object System.Windows.Forms.NotifyIcon;"
                          "$notify.Icon = [System.Drawing.SystemIcons]::Information;" 
                          "$notify.BalloonTipIcon = 'Info';" 
                          "$notify.BalloonTipTitle = 'App Monitor';" 
                          "$notify.BalloonTipText =  '" + printMilestone + "';" 
                          "$notify.Visible = $true;" 
                          "$notify.ShowBalloonTip(10000);" 
                          "Start-Sleep -Seconds 15;" 
        "\"";
    system(command.c_str());
    return;
}