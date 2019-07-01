#include "NodeCmd.h"
#include "SIOJConvert.h"


#define BUFSIZE 4096

//largely from https://stackoverflow.com/questions/14147138/capture-output-of-spawned-process-to-string

FNodeCmd::FNodeCmd()
{
	DefaultMainScript = TEXT("nodeWrapper.js");
	DefaultPort = 4269;
	bShouldMainRun = true;
	g_hChildStd_OUT_Rd = NULL;
	g_hChildStd_OUT_Wr = NULL;
	g_hChildStd_ERR_Rd = NULL;
	g_hChildStd_ERR_Wr = NULL;
	ProcessDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + "Plugins/nodejs-ue4/Source/ThirdParty/node");
	OnMainScriptEnd = nullptr;
	OnChildScriptEnd = nullptr;
	OnScriptError = nullptr;
	OnConsoleLog = nullptr;
	Socket = MakeShareable(new FSocketIONative);
}

FNodeCmd::~FNodeCmd()
{
	//todo: convert to listener & static alloc
	bShouldMainRun = false;

	//block until the other thread quits
	while (bIsMainRunning)
	{

	}

	Socket->Disconnect();
}

void FNodeCmd::StartupMainScriptIfNeeded()
{
	if (!bIsMainRunning) 
	{
		RunMainScript(DefaultMainScript, DefaultPort);
	}
}

bool FNodeCmd::RunMainScript(const FString& ScriptRelativePath, int32 Port)
{
	//Script already running? return false
	if (bIsMainRunning) 
	{
		return false;
	}

	FString NodeExe = TEXT("node.exe");
	if (Socket->bIsConnected) 
	{
		Socket->SyncDisconnect();	//this will block for ~1 sec
	}

	UE_LOG(LogTemp, Log, TEXT("RunScriptStart"));
	Socket->OnConnectedCallback = [&](const FString& InSessionId)
	{
		UE_LOG(LogTemp, Log, TEXT("Main script Connected."));
	};
	Socket->OnEvent(TEXT("console.log"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		//UE_LOG(LogTemp, Log, TEXT("console.log %s"), *USIOJConvert::ToJsonString(Message));
		if (OnConsoleLog)
		{
			OnConsoleLog(USIOJConvert::ToJsonString(Message));
		}
	});
	Socket->OnEvent(TEXT("mainScriptEnd"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		UE_LOG(LogTemp, Log, TEXT("mainScriptEnd %s"), *USIOJConvert::ToJsonString(Message));
		Socket->Disconnect();
		bShouldMainRun = false;
	});
	Socket->OnEvent(TEXT("childScriptEnd"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		const FString SafeChildPathMessage = USIOJConvert::ToJsonString(Message);
		if (OnChildScriptEnd)
		{
			OnChildScriptEnd(SafeChildPathMessage);
		}
	});
	Socket->OnEvent(TEXT("childScriptError"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		UE_LOG(LogTemp, Error, TEXT("Script Error: %s"), *USIOJConvert::ToJsonString(Message));
		const FString SafePath = ScriptRelativePath;
		const FString SafeErrorMessage = USIOJConvert::ToJsonString(Message);
		
		if (OnScriptError)
		{
			OnScriptError(SafePath, SafeErrorMessage);
		}
	});

	//NB: a new script run means events would need to be rebound... todo: keep a list of events bound and auto-rebind
	Socket->Connect(FString::Printf(TEXT("http://localhost:%d"), Port));


	TFunction<void()> Task = [&, ScriptRelativePath]
	{
		UE_LOG(LogTemp, Log, TEXT("node thread start"));
		bIsMainRunning = true;

		SECURITY_ATTRIBUTES sa;
		// Set the bInheritHandle flag so pipe handles are inherited. 
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = 1;
		sa.lpSecurityDescriptor = NULL;
		// Create a pipe for the child process's STDERR. 
		if (!CreatePipe(&g_hChildStd_ERR_Rd, &g_hChildStd_ERR_Wr, &sa, 0)) {
			return;
		}
		// Ensure the read handle to the pipe for STDERR is not inherited.
		if (!SetHandleInformation(g_hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0)) {
			return;
		}
		// Create a pipe for the child process's STDOUT. 
		if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &sa, 0)) {
			return;
		}
		// Ensure the read handle to the pipe for STDOUT is not inherited
		if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
			return;
		}

		bShouldMainRun = true;

		PROCESS_INFORMATION piProcInfo = CreateChildProcess(NodeExe, ScriptRelativePath);

		//ReadFromPipe();
		while (bShouldMainRun)
		{
			FPlatformProcess::Sleep(0.1f);
		}
		if (Socket->bIsConnected) 
		{

		}

		TerminateProcess(piProcInfo.hProcess, 1);

		UE_LOG(LogTemp, Log, TEXT("RunScriptTerminated"));
		const FString FinishPath = ScriptRelativePath;

		TFunction<void()> GTCallback = [this, FinishPath]
		{
			bIsMainRunning = false;
			if (OnMainScriptEnd)
			{
				OnMainScriptEnd(FinishPath);
			}
		};
		Async(EAsyncExecution::TaskGraph, GTCallback);
	};

	Async(EAsyncExecution::Thread, Task);

	return true;
}

void FNodeCmd::RunChildScript(const FString& ScriptRelativePath)
{
	if (bIsMainRunning)
	{
		Socket->Emit(TEXT("runChildScript"), ScriptRelativePath);
	}
}

void FNodeCmd::Emit(const FString& Data)
{
	Socket->Emit(TEXT("stdin"), Data);
}

void FNodeCmd::StopMainScript()
{
	Socket->Emit(TEXT("stopMainScript"), TEXT("ForceStop"));
	Socket->Disconnect();
	bShouldMainRun = false;
}

void FNodeCmd::StopChildScript()
{
	if (bIsMainRunning) 
	{
		Socket->Emit(TEXT("stopChildScript"), TEXT("ForceStop"));
	}
}

bool FNodeCmd::IsMainScriptRunning()
{
	return bIsMainRunning;
}

PROCESS_INFORMATION FNodeCmd::CreateChildProcess(const FString& Process, const FString& Commands) {
	PROCESS_INFORMATION piProcInfo;
	STARTUPINFO siStartInfo;
	bool bSuccess = 0;

	// Set up members of the PROCESS_INFORMATION structure. 
	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

	// Set up members of the STARTUPINFO structure. 
	// This structure specifies the STDERR and STDOUT handles for redirection.
	ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
	siStartInfo.cb = sizeof(STARTUPINFO);
	siStartInfo.hStdError = g_hChildStd_ERR_Wr;
	siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
	//siStartInfo.hStdInput = g_hChildStd_IN_Rd;
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	FString ProcessPath = ProcessDirectory + TEXT("/") + Process;
	FString Command = Process + TEXT(" ") + Commands;

	// Create the child process. 
	bSuccess = CreateProcessW(*ProcessPath,
		(LPWSTR)*Command,		// command line 
		NULL,			// process security attributes 
		NULL,			// primary thread security attributes 
		1,				// handles are inherited 
		CREATE_NO_WINDOW,     // creation flags, no window
		NULL,				  // use parent's environment 
		*ProcessDirectory,       // use parent's current directory 
		&siStartInfo,  // STARTUPINFO pointer 
		&piProcInfo);  // receives PROCESS_INFORMATION
	CloseHandle(g_hChildStd_ERR_Wr);
	CloseHandle(g_hChildStd_OUT_Wr);
	// If an error occurs, exit the application. 
	if (!bSuccess) {
		exit(1);
	}
	return piProcInfo;
}


void FNodeCmd::ReadFromPipe() {
	DWORD dwRead;
	CHAR chBuf[BUFSIZE];
	bool bSuccess = false;
	std::string out = "", err = "";
	for (;;) 
	{
		bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0) break;

		std::string s(chBuf, dwRead);
		out += s;
	}
	dwRead = 0;
	for (;;) 
	{
		bSuccess = ReadFile(g_hChildStd_ERR_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0) break;

		std::string s(chBuf, dwRead);
		err += s;
	}

	if (out.length() > 0) 
	{
		UE_LOG(LogTemp, Log, TEXT("out: %s"), *FString(UTF8_TO_TCHAR(out.c_str())));
	}
}

void FNodeCmd::WriteToPipe(FString Data)
{
	//Currently broken

	/*DWORD dwWritten;
	CHAR chBuf[BUFSIZE];
	BOOL bSuccess = FALSE;

	for (;;)
	{
		bSuccess = WriteFile(g_hChildStd_OUT_Wr, *Data, Data.Len(), &dwWritten, NULL);
		if (!bSuccess) break;
	}

	// Close the pipe handle so the child process stops reading. 

	if (!CloseHandle(g_hChildStd_IN_Wr))
		ErrorExit(TEXT("StdInWr CloseHandle"));*/
}




