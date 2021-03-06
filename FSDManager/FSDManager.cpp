// FSDManager.cpp : Defines the entry point for the console application.
//
#include "CFSDPortConnector.h"
#include "FSDCommonInclude.h"
#include "FSDCommonDefs.h"
#include "stdio.h"
#include "AutoPtr.h"
#include "FSDThreadUtils.h"
#include "Shlwapi.h"
#include <math.h>
#include <fstream>
#include <vector>
#include "CFSDDynamicByteBuffer.h"
#include <unordered_map>
#include "FSDUmFileUtils.h"
#include <iostream>
#include "Psapi.h"
#include "FSDThreadUtils.h"
#include "FSDProcess.h"
#include "FSDFileInformation.h"
#include "FSDFileExtension.h"

#define TIME_AFTER_KILL_BEFORE_REMOVE 20*1000

using namespace std;

HRESULT HrMain();

unordered_map<wstring, CFileInformation> gFiles;
unordered_map<ULONG, CProcess>           gProcesses;
bool                                     g_fKillMode = false;
bool                                     g_fClearHistory = false;
vector<ULONG>                            gKilledPids;
LARGE_INTEGER                            gLastKill;
LARGE_INTEGER                            gFrequency;

size_t                                   g_cPrintFrequency;
ULONG                                    g_uPid;

struct THREAD_CONTEXT
{
    bool               fExit;
    CFSDPortConnector* pConnector;
    CAutoStringW       wszScanDir;
};

LPCWSTR MajorTypeToString(ULONG uMajorType)
{
    switch (uMajorType)
    {
    case IRP_CREATE:
        return L"IRP_CREATE";
    case IRP_CLOSE:
        return L"IRP_CLOSE";
    case IRP_READ:
        return L"IRP_READ";
    case IRP_WRITE:
        return L"IRP_WRITE";
    case IRP_QUERY_INFORMATION:
        return L"IRP_QUERY_INFORMATION";
    case IRP_SET_INFORMATION:
        return L"IRP_SET_INFORMATION";
    case IRP_CLEANUP:
        return L"IRP_CLEANUP";
    }

    return L"IRP_UNKNOWN";
}

int main(int argc, char **argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    HRESULT hr = HrMain();
    if (FAILED(hr))
    {
        printf("Main failed with status 0x%x\n", hr);
        return 1;
    }

    return 0;
}

HRESULT ChangeDirectory(CFSDPortConnector* pConnector, THREAD_CONTEXT* pContext, LPCWSTR wszDirectory)
{
    HRESULT hr = S_OK;

    if (!PathFileExistsW(wszDirectory))
    {
        printf("Directory: %ls is not valid\n", wszDirectory);
        return S_OK;
    }

    CAutoStringW wszVolumePath = new WCHAR[50];
    hr = GetVolumePathNameW(wszDirectory, wszVolumePath.Get(), 50);
    RETURN_IF_FAILED(hr);

    size_t cVolumePath = wcslen(wszVolumePath.Get());

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_SCAN_DIRECTORY;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszDirectory + cVolumePath);

    printf("Changing directory to: %ls\n", wszDirectory);

    CAutoStringW wszScanDir;
    hr = NewCopyStringW(&wszScanDir, aMessage.wszFileName, MAX_FILE_NAME_LENGTH);
    RETURN_IF_FAILED(hr);

    wszScanDir.Detach(&pContext->wszScanDir);

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    return S_OK;
}

HRESULT OnChangeDirectoryCmd(CFSDPortConnector* pConnector, THREAD_CONTEXT* pContext)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls[/]", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    hr = ChangeDirectory(pConnector, pContext, wszParameter.Get());
    RETURN_IF_FAILED(hr);

    return S_OK;
}

static const char* szKillProcessLogo =
" -------------------------------------------------- \n"
"                                                    \n"
"                                                    \n"
"                 Process %u KILLED                  \n"
"                                                    \n"
"                                                    \n"
" -------------------------------------------------- \n";

HRESULT KillProcess(ULONG uPid)
{
    CAutoHandle hProcess = OpenProcess(PROCESS_TERMINATE, false, uPid);
    if (!hProcess)
    {
        return E_FAIL;
    }

    bool fSuccess = TerminateProcess(hProcess, 0);
    if (!fSuccess)
    {
        return E_FAIL;
    }

    return S_OK;
}

HRESULT OnSendMessageCmd(CFSDPortConnector* pConnector)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_PRINT_STRING;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszParameter.Get());

    printf("Sending message: %ls\n", wszParameter.Get());

    BYTE pReply[MAX_STRING_LENGTH];
    DWORD dwReplySize = sizeof(pReply);
    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pReply, &dwReplySize);
    RETURN_IF_FAILED(hr);

    if (dwReplySize > 0)
    {
        printf("Recieved response: %ls\n", (WCHAR*)pReply);
    }

    return S_OK;
}

void ManagerKillProcess(CProcess* pProcess)
{
    if (g_fKillMode && !pProcess->IsKilled())
    {
        HRESULT hr = KillProcess(pProcess->GetPid());
        if (FAILED(hr))
        {
            printf(
                "------------------------------------------\n"
                "       Failed to kill process %u          \n"
                "       Reason: 0x%x                       \n"
                "------------------------------------------\n"
                , pProcess->GetPid(), hr);
            return;
        }

        printf(szKillProcessLogo, pProcess->GetPid());
        pProcess->PrintInfo(true);
        
        pProcess->Kill();

        QueryPerformanceCounter(&gLastKill);
        gKilledPids.push_back(pProcess->GetPid());
    }
}

void ProcessIrp(FSD_OPERATION_DESCRIPTION* pOperation, THREAD_CONTEXT* pContext)
{
    //printf("PID: %u MJ: %ls MI: %u\n", pOperation->uPid, MajorTypeToString(pOperation->uMajorType), pOperation->uMinorType);
    
    // Find process in global hash or add if does not exist
    auto process = gProcesses.insert({ pOperation->uPid , CProcess(pOperation->uPid) });
    CProcess* pProcess = &process.first->second;
    
    if (pProcess->IsKilled())
    {
        return;
    }
 
    if (pOperation->uMajorType == IRP_SET_INFORMATION && !pOperation->fCheckForDelete)
    {
        // Rename or move operation
        pProcess->SetFileInfo(pOperation, pContext->wszScanDir.Get());
    }
    else
    {
        // Delete operation and other operations
        auto file = gFiles.insert({ pOperation->GetFileName(), CFileInformation(pOperation->GetFileName()) });
        file.first->second.RegisterAccess(pOperation, pProcess, pContext->wszScanDir.Get());
    }

    if (pProcess->IsMalicious())
    {
        ManagerKillProcess(pProcess);
    }
}

HRESULT FSDIrpSniffer(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    CFSDDynamicByteBuffer pBuffer;
    hr = pBuffer.Initialize(1024*8);
    RETURN_IF_FAILED(hr);

    size_t cTotalIrpsRecieved = 0;
    while (!pContext->fExit)
    {
        FSD_MESSAGE_FORMAT aMessage;
        aMessage.aType = MESSAGE_TYPE_QUERY_NEW_OPS;

        BYTE* pResponse = pBuffer.Get();
        DWORD dwReplySize = numeric_cast<DWORD>(pBuffer.ReservedSize());
        hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pBuffer.Get(), &dwReplySize);
        RETURN_IF_FAILED(hr);

        if (dwReplySize == 0)
        {
            continue;
        }

        FSD_OPERATION_DESCRIPTION* pOpDescription = ((FSD_QUERY_NEW_OPS_RESPONSE_FORMAT*)(PVOID)pResponse)->GetFirst();
        size_t cbData = 0;
        size_t cCurrentIrpsRecieved = 0;
        for (;;)
        {
            if (cbData >= dwReplySize)
            {
                ASSERT(cbData == dwReplySize);
                break;
            }

            try
            {
                ProcessIrp(pOpDescription, pContext);
            }
            catch (...)
            {
                printf("Exception in ProcessIrp!!!\n");
                return S_OK;
            }

            cbData += pOpDescription->PureSize();
            cCurrentIrpsRecieved++;
            pOpDescription = pOpDescription->GetNext();
        }

        cTotalIrpsRecieved += cCurrentIrpsRecieved;

        printf("Total IRPs: %Iu Current Irps: %Iu Recieve size: %Iu Buffer size: %Iu Buffer utilization: %.2lf%%\n", 
            cTotalIrpsRecieved, cCurrentIrpsRecieved, cbData, pBuffer.ReservedSize(), ((double)cbData / pBuffer.ReservedSize() ) * 100);

        if (pBuffer.ReservedSize() < MAX_BUFFER_SIZE && cbData >= pBuffer.ReservedSize()*2/3)
        {
            pBuffer.Grow();
        }

        if (cbData < pBuffer.ReservedSize()/10)
        {
            Sleep(1000);
        }

        LARGE_INTEGER aCurrent;
        QueryPerformanceCounter(&aCurrent);

        LONGLONG llTimeDiff = gLastKill.QuadPart - aCurrent.QuadPart;
        double dftDuration = (double)llTimeDiff * 1000.0 / (double)gFrequency.QuadPart;

        if (dftDuration > TIME_AFTER_KILL_BEFORE_REMOVE)
        {
            for (ULONG uPid : gKilledPids)
            {
                gProcesses.erase(uPid);
            }
        }

        if (g_fClearHistory)
        {
            gProcesses.clear();
            gFiles.clear();

            g_fClearHistory = false;
        }

        if (g_cPrintFrequency != 0)
        {
            auto it = gProcesses.find(g_uPid); 
            ASSERT(it != gProcesses.end());
            it->second.SetPrintFrequency(g_cPrintFrequency);

            g_cPrintFrequency = 0;
        }
    }

    return S_OK;
}

HRESULT UserInputParser(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);
    
    hr = ChangeDirectory(pConnector, pContext, L"C:\\Users\\User\\");
    RETURN_IF_FAILED(hr);

    int res = system("findstr /s /d:\"C:\\\\Users\\\\User\\\\\" \"123456\" *.gif *.groups *.hdd *.hpp *.log *.m2ts *.m4p *.mkv *.mpeg *.ndf *.nvram *.ogg *.ost *.pab *.pdb *.pif *.png *.qed *.qcow *.qcow2 *.rvt *.st7 *.stm *.vbox *.vdi *.vhd *.vhdx *.vmdk *.vmsd *.vmx *.vmxf *.3fr *.3pr *.ab4 *.accde *.accdr *.accdt *.ach *.acr *.adb *.ads *.agdl *.ait *.apj *.asm *.awg *.back *.backup *.backupdb *.bay *.bdb *.bgt *.bik *.bpw *.cdr3 *.cdr4 *.cdr5 *.cdr6 *.cdrw *.ce1 *.ce2 *.cib *.craw *.crw *.csh *.csl *.db_journal *.dc2 *.dcs *.ddoc *.ddrw *.der *.des *.dgc *.djvu *.dng *.drf *.dxg *.eml *.erbsql *.erf *.exf *.ffd *.fh *.fhd *.gray *.grey *.gry *.hbk *.ibd *.ibz *.iiq *.incpas *.jpe *.kc2 *.kdbx *.kdc *.kpdx *.lua *.mdc *.mef *.mfw *.mmw *.mny *.mrw *.myd *.ndd *.nef *.nk2 *.nop *.nrw *.ns2 *.ns3 *.ns4 *.nwb *.nx2 *.nxl *.nyf *.odb *.odf *.odg *.odm *.orf *.otg *.oth *.otp *.ots *.ott *.p12 *.p7b *.p7c *.pdd *.pem *.plus_muhd *.plc *.pot *.pptx *.psafe3 *.py *.qba *.qbr *.qbw *.qbx *.qby *.raf *.rat *.raw *.rdb *.rwl *.rwz *.s3db *.sd0 *.sda *.sdf *.sqlite *.sqlite3 *.sqlitedb *.sr2 *.srf *.srw *.st5 *.st8 *.std *.sti *.stw *.stx *.sxd *.sxg *.sxi *.sxm *.tex *.wallet *.wb2 *.wpd *.x11 *.x3f *.xis *.ycbcra *.yuv *.contact *.dbx *.doc *.docx *.jnt *.jpg *.msg *.oab *.ods *.pdf *.pps *.ppsm *.ppt *.pptm *.prf *.pst *.rar *.rtf *.txt *.wab *.xls *.xlsx *.xml *.zip *.1cd *.3ds *.3g2 *.3gp *.7z *.7zip *.accdb *.aoi *.asf *.asp *.aspx *.asx *.avi *.bak *.cer *.cfg *.class *.config *.css *.csv *.db *.dds *.dwg *.dxf *.flf *.flv *.html *.idx *.js *.key *.kwm *.laccdb *.ldf *.lit *.m3u *.mbx *.md *.mdf *.mid *.mlb *.mov *.mp3 *.mp4 *.mpg *.obj *.odt *.pages *.php *.psd *.pwm *.rm *.safe *.sav *.save *.sql *.srt *.swf *.thm *.vob *.wav *.wma *.wmv *.xlsb *.dm *.aac *.ai *.arw *.c *.cdr *.cls *.cpi *.cpp *.cs *.db3 *.docm *.dot *.dotm *.dotx *.drw *.dxb *.eps *.fla *.flac *.fxg *.java *.m *.m4v *.max *.mdb *.pcd *.pct *.pl *.potm *.potx *.ppam *.ppsm *.ppsx *.pptm *.ps *.r3d *.rw2 *.sldm *.sldx *.svg *.tga *.wps *.xla *.xlam *.xlm *.xlr *.xlsm *.xlt *.xltm *.xltx *.xlw *.act *.adp *.al *.bkp *.blend *.cdf *.cdx *.cgm *.cr2 *.crt *.dac *.dbf *.dcr *.ddd *.design *.dtd *.fdb *.fff *.fpx *.h *.iif *.indd *.jpeg *.mos *.nd *.nsd *.nsf *.nsg *.nsh *.odc *.odp *.oil *.pas *.pat *.pef *.pfx *.ptx *.qbb *.qbm *.sas7bdat *.say *.st4 *.st6 *.stc *.sxc *.sxw *.tlg *.wad *.xlk *.aiff *.bin *.bmp *.cmt *.dat *.dit *.edb *.flvv 1>NUL 2>NUL");
    if (res != 0)
    {
        printf("Failed to run initial scanning.\n");
    } 
    else
    {
        printf(
        "\n""\n"    
        "----------------------------------------\n"
        "        Finished Initial Scanning       \n"
        "----------------------------------------\n"
        "\n""\n");
    }

    CAutoStringW wszCommand = new WCHAR[MAX_COMMAND_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszCommand);

    while (!pContext->fExit)
    {
        printf("Input a command: ");
        wscanf_s(L"%ls", wszCommand.Get(), MAX_COMMAND_LENGTH);
        if (wcscmp(wszCommand.Get(), L"chdir") == 0)
        {
            hr = OnChangeDirectoryCmd(pConnector, pContext);
            RETURN_IF_FAILED(hr);
        } 
        else
        if (wcscmp(wszCommand.Get(), L"message") == 0)
        {
            hr = OnSendMessageCmd(pConnector);
            RETURN_IF_FAILED(hr);
        }
        else
        if (wcscmp(wszCommand.Get(), L"exit") == 0)
        {
            pContext->fExit = true;
            printf("Exiting FSDManager\n");
        }
        else
        if (wcscmp(wszCommand.Get(), L"kill") == 0)
        {
            ULONG uPid;
            if (wscanf_s(L"%u", &uPid))
            {
                printf("Killing process %u FSDManager\n", uPid);
                hr = KillProcess(uPid);
                RETURN_IF_FAILED_EX(hr);
            }
            else
            {
                printf("Failed to read PID\n");
            }
        }
        else
        if (wcscmp(wszCommand.Get(), L"killmode") == 0)
        {
            ULONG uKillMode;
            if (wscanf_s(L"%u", &uKillMode))
            {
                g_fKillMode = uKillMode > 0;
            }
            else
            {
                printf("Failed to read killmode flag\n");
            }
        }
        else
        if (wcscmp(wszCommand.Get(), L"clear") == 0)
        {
            g_fClearHistory = true;
        }
        else
        if (wcscmp(wszCommand.Get(), L"print") == 0)
        {
            ULONG uPid;
            size_t cFreq;
            if (wscanf_s(L"%u %Iu", &uPid, &cFreq))
            {
                g_cPrintFrequency = cFreq;
                g_uPid = uPid;
            }
            else
            {
                printf("Failed to read PID\n");
            }
        }
        else
        {
            printf("Invalid command: %ls\n", wszCommand.Get());
        }
    }

    return S_OK;
}

HRESULT HrMain()
{
    HRESULT hr = S_OK;

    bool res = SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    if (!res)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        RETURN_IF_FAILED_EX(hr);
    }

    CAutoPtr<CFSDPortConnector> pConnector;
    hr = NewInstanceOf<CFSDPortConnector>(&pConnector, g_wszFSDPortName);
    if (hr == E_FILE_NOT_FOUND)
    {
        printf("Failed to connect to FSDefender Kernel module. Try to load it.\n");
    }
    RETURN_IF_FAILED(hr);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_MANAGER_PID;
    aMessage.uPid = GetCurrentProcessId();

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    THREAD_CONTEXT aContext = {};
    aContext.fExit           = false;
    aContext.pConnector      = pConnector.Get();

    CAutoHandle hFSDIrpSnifferThread;
    hr = UtilCreateThreadSimple(&hFSDIrpSnifferThread, (LPTHREAD_START_ROUTINE)FSDIrpSniffer, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);
    
    CAutoHandle hUserInputParserThread;
    hr = UtilCreateThreadSimple(&hUserInputParserThread, (LPTHREAD_START_ROUTINE)UserInputParser, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hFSDIrpSnifferThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hUserInputParserThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    return S_OK;
}