// This file is part of PinballY
// Copyright 2018 Michael J Roberts | GPL v3 or later | NO WARRANTY
//
// PinballY Admin Host
// 
// The Admin Host is a separate process from the main PinballY program
// that can be used to provide elevated (Admin mode) process launching to
// the PinballY UI.  The Admin Host process runs in elevated mode, which
// gives it the necessary privileges to launch elevated child processes
// without UAC intervention.  
// 
// The goal is to fix one of the bad features of the PinballX setup,
// which is that many users are forced to run just about everything in
// Admin mode, or turn off UAC entirely, to deal with a few errant
// programs that demand Admin mode to run.  One notable example is the
// last VP 9.2 release, 9.2.1, which is marked in the program manifest
// with "highestAvailable" privilege mode.  That triggers process
// creation errors when a non-elevated parent program tries to launch it,
// or causes weird compatibility problems (e.g., a non-elevated PinballX
// can't monitor the keyboard while an elevated child is running, so the
// Exit Game key stops working).  Most users just give up and run
// everything in Admin mode when faced with this problem.  This is
// unfortunate because it forfeits the system integrity protection and
// and anti-malware benefits of the Windows security model.
// 
// The Admin Host tries to address this by letting users still run 
// programs that really need Admin mode in Admin mode, without any UAC
// hassles or compatibility issues, but also lets them run everything
// else in normal user mode.  This conforms to one one of the key
// principles of the Windows security model, which is that a task that
// requires elevated privileges should isolate the privileged work into
// separate components that run elevated, while the rest of the program
// runs at normal user level.
// 
// We use what is basically a client/server architecture.  The server is
// the Admin Host (PinballY Admin Mode.exe); this runs in an elevated
// (Admin mode) process.  The client is the main PinballY program, which
// runs in normal user mode.  The server launches the client as a
// separate process, and the two communicate via pipes that the server
// creates (it passes the client's handles to the pipes in the process
// startup data).  The client sends requests to the server via pipe
// messages, and the server sends the results back as pipe messages.
// Launching the whole client/server setup only requires a single user
// action, namely launching the Admin Host program.  That takes care of
// launching the client as well, so from the user's perspective, it's
// like any other program launch: you just double-click the .exe file on
// the desktop.  And the user doesn't have to take any other action to
// get the Admin Host to run in privileged mode - that's in our manifest,
// so the Windows desktop automatically launches us elevated.  There's no
// need to right-click on "Run as administrator" or anything like that.
// The only extra user step required is the normal UAC prompt required
// for any elevated launch.
// 
// The main PinballY.exe program, which the Admin Host launches
// automatically as it starts up, provides the UI and carries out most of
// the work, including launching most games.  This process runs in
// ordinary user mode (that is, not elevated), so the bulk of our code
// runs in user mode.  Any games we launch directly from the main UI
// likewise run in user mode.
// 
// In the main UI, when you launch a game, the program determines whether
// or not the game needs elevation.  If not, the UI runs the game
// directly, as a normal user-mode process.  If the game requires
// elevation, though, the UI process sends a message to the Admin Host
// process (via the pipe created at startup) telling it to launch the
// game in elevated mode.  The Admin Host can do this without UAC
// intervention, because the Admin Host is itself running elevated
// already.  Further, the Admin Host can monitor the keyboard while the
// elevated game process is running; this is impossible for a user-mode
// program, because Windows security rules prevent a non-elevated program
// from monitoring the keyboard when an elevated program is in the
// foreground, but it's allowed in this case because the Admin Host is
// elevated.  This means we can intercept the Exit Game key properly for
// elevated games.
// 
// The Admin Host automatically exits when the main PinballY process
// exits.  The Admin Host doesn't present any UI, so its presence is
// transparent at the UI level; it just looks like part of the main
// PinballY program, which it really is functionally.
// 
// 
// Security risks: Admin mode is inherently risky, so the safest thing to
// do is use the standalone PinballY.exe and never run the Admin Host.
// But if you do use the Admin Host, I've tried to minimize the risk of
// accidental or intentional misuse.
// 
// In particular, I've tried to make the client/server setup as
// impenetrable as possible.  Client/server always creates a risk that
// some interloper connects to the server, pretending to be a legitimate
// client, and tricks the server into providing the protected service for
// some illegitimate purpose.  In this case, that would mean tricking our
// server into launching a malware program in Admin mode.  In this case,
// though, I don't think that's much of a risk.  This isn't the kind of
// server that advertises its services or listens for connections from
// unrelated processes or network peers.  It's really only client/server
// in the technical sense that it consists of two separate processes
// communicating over a comms channel.  The channel in this case is
// strictly controlled by the server and captive to the two processes
// involved.  There's no provision for an unrelated process to connect;
// only the server and the client process that the server itself launches
// have access to the pipe handles.  So the only opportunity for an
// attack at the client/server level is for someone to replace the whole
// PinballY.exe executable with a malware interloper.  But a hacker with
// access to the file system could just as well replace the Admin Host
// program instead of the UI side, and that would be the way to go
// because it's much easier.  And *that* risk, or tampering with an .exe
// file that you trust to run in Admin mode, applies to anything you're
// running in Admin mode; this risk isn't at all specific to our setup.
// The main way to mitigate the risk of .exe tampering is to use code
// signing.  But code signing and open source don't mesh well, as they
// have rather opposite aims: code siging is all about preventing random
// third parties from creating their own builds of a program, whereas
// open source is all about enabling random third parties to create their
// own builds.  Even so, it could be worth considering at some point.
// 

#include "stdafx.h"
#include <shellapi.h>
#include <Shlwapi.h>
#include "PinballY Admin Mode.h"
#include "../Utilities/InputManager.h"
#include "../Utilities/KeyInput.h"
#include "../Utilities/Joystick.h"

#pragma comment(lib, "Shlwapi.lib")

// -----------------------------------------------------------------------
// 
// Create a pseudo-anonymous pipe handle pair with overlapped I/O enabled
//
static BOOL APIENTRY CreatePipeEx(
	OUT LPHANDLE lpReadPipe, OUT LPHANDLE lpWritePipe,
	IN LPSECURITY_ATTRIBUTES lpPipeAttributes, IN DWORD nSize, 
	DWORD dwReadMode, DWORD dwWriteMode)
{
	HANDLE ReadPipeHandle, WritePipeHandle;
	DWORD dwError;
	CHAR PipeNameBuffer[MAX_PATH];
	static ULONG PipeSerialNumber;

	// Only one OpenMode flag is supported - FILE_FLAG_OVERLAPPED
	if ((dwReadMode | dwWriteMode) & (~FILE_FLAG_OVERLAPPED)) 
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	// Set the default buffer size
	if (nSize == 0)
		nSize = 4096;

	// synthesize a serial number
	InterlockedIncrement(&PipeSerialNumber);
	sprintf_s(PipeNameBuffer,	"\\\\.\\Pipe\\RemoteExeAnon.%08lx.%08lx",
		GetCurrentProcessId(), PipeSerialNumber++);

	// create the named pipe; this will be the read end
	if ((ReadPipeHandle = CreateNamedPipeA(
		PipeNameBuffer, PIPE_ACCESS_INBOUND | dwReadMode,
		PIPE_TYPE_BYTE | PIPE_WAIT, 1, nSize, nSize, 120 * 1000,
		lpPipeAttributes)) == NULL)
		return FALSE;

	// open the pipe to get the write end
	if ((WritePipeHandle = CreateFileA(
		PipeNameBuffer, GENERIC_WRITE, 0, lpPipeAttributes,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | dwWriteMode, NULL)) == INVALID_HANDLE_VALUE)
	{
		dwError = GetLastError();
		CloseHandle(ReadPipeHandle);
		SetLastError(dwError);
		return FALSE;
	}

	// pass back the handles
	*lpReadPipe = ReadPipeHandle;
	*lpWritePipe = WritePipeHandle;

	// success
	return TRUE;
}

// -----------------------------------------------------------------------
//
// Enable/disable a privilege in a token
//
static bool SetPrivilege(HANDLE hToken, LPCTSTR lpszPrivilege, BOOL bEnablePrivilege, ErrorHandler &eh)
{
	auto SysErr = [&eh](const TCHAR *where)
	{
		WindowsErrorMessage err;
		eh.SysError(LoadStringT(IDS_ERR_STARTUP),
			MsgFmt(_T("%s: system error %d, %s"), where, err.GetCode(), err.Get()));
		return false;
	};

	// look up the privilege name
	LUID luid;
	if (!LookupPrivilegeValue(NULL, lpszPrivilege, &luid))
		return SysErr(MsgFmt(_T("Setting privileges: LookupPrivilegeValue(%s) failed"), lpszPrivilege));

	// set up the token privilege descriptor
	TOKEN_PRIVILEGES tp;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	if (bEnablePrivilege)
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	else
		tp.Privileges[0].Attributes = 0;

	// enable/disable the privilege
	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
		return SysErr(MsgFmt(_T("Setting privileges: AdjustTokenPrivileges(%s) failed"), lpszPrivilege));

	if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
		return SysErr(MsgFmt(_T("Failed to set privilege %s"), lpszPrivilege));

	// success
	return true;
}


// -----------------------------------------------------------------------
//
// Pipe manager
//
class PipeManager
{
public:
	PipeManager()
	{
		// set up the OVERLAPPED object for reading the pipe
		hReadEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		ZeroMemory(&ovRead, sizeof(ovRead));
		ovRead.hEvent = hReadEvent;
	}

	bool StartRead(ErrorHandler &eh)
	{
		// start the first read on the pipe
		if (ReadFile(hPipeRead, readBuf, sizeof(readBuf), NULL, &ovRead)
			|| GetLastError() == ERROR_IO_PENDING)
			return true;

		return SysErr(eh, _T("Starting pipe read: ReadFile failed"));
	}

	bool FinishRead(std::unique_ptr<TCHAR> &data, size_t &charlen, ErrorHandler &eh)
	{
		// complete the read
		DWORD actual;
		if (!GetOverlappedResult(hPipeRead, &ovRead, &actual, FALSE))
		{
			// If the error is 109 (pipe ended), don't report it.  This means
			// that the client process has closed its end of the pipe, which 
			// happens when it's about to exit.  Simply return failure silently.
			// This effectively shuts down the protocol handler, since we don't
			// start another read, but it allows the main loop to finish any
			// other work that it does separately from the protocol processor.
			// The main loop will eventually terminate when the child process
			// exits.
			if (GetLastError() == ERROR_BROKEN_PIPE)
				return false;

			// report other errors
			return SysErr(eh, _T("Reading pipe: GetOverlappedResult failed"));
		}

		// copy the result to the caller's string
		data.reset(new TCHAR[actual]);
		memcpy(data.get(), readBuf, actual);

		// pass back the character length of the result
		charlen = actual / sizeof(TCHAR);

		// start a new read
		return StartRead(eh);
	}

	// write a null-terminated string
	bool Write(const TCHAR *msg)
	{
		DWORD byteLen = (DWORD)(_tcslen(msg) * sizeof(TCHAR));
		DWORD actual;
		return WriteFile(hPipeWrite, msg, byteLen, &actual, NULL) && actual == byteLen;
	}

	// write a buffer
	bool Write(const void *msg, size_t byteLen)
	{
		DWORD actual;
		return WriteFile(hPipeWrite, msg, (DWORD)byteLen, &actual, NULL) && actual == byteLen;
	}

	// send an OK reply with various numbers of parameters
	bool WriteOk();
	bool WriteOk(const TCHAR *param0);
	bool WriteOk(const TCHAR *param0, const TCHAR *param1);
	bool WriteOk(const TCHAR *param0, const TCHAR *param1, const TCHAR *param2);
	bool WriteOk(const TCHAR *param0, const TCHAR *param1, const TCHAR *param2, const TCHAR *param3);

	// send an error reply
	bool WriteError(const TCHAR *message);

	// general reply writer - packs an array of strings into a buffer and
	// sends it as the reply message
	bool WriteReply(const TCHAR *const *reply, size_t nItems);

	// read pipe
	HandleHolder hPipeRead;

	// OVERLAPPED object for pipe reader
	OVERLAPPED ovRead;
	HandleHolder hReadEvent;

	// read buffer
	TCHAR readBuf[4096];
	
	// write pipe
	HandleHolder hPipeWrite;
	
	bool SysErr(ErrorHandler &eh, const TCHAR *where)
	{
		WindowsErrorMessage err;
		eh.SysError(LoadStringT(IDS_ERR_IPC), MsgFmt(_T("%s: error %d, %s"), where, err.GetCode(), err.Get()));
		return false;
	}
};

bool PipeManager::WriteReply(const TCHAR *const *reply, size_t nItems)
{
	// add up the space required
	size_t totalCharLen = 0;
	for (size_t i = 0; i < nItems; ++i)
		totalCharLen += _tcslen(reply[i]) + 1;
	
	// allocate a buffer
	std::unique_ptr<TCHAR> buf(new TCHAR[totalCharLen]);

	// copy the strings
	TCHAR *p = buf.get();
	for (size_t i = 0; i < nItems; ++i)
	{
		size_t charLen = _tcslen(reply[i]) + 1;
		memcpy(p, reply[i], charLen * sizeof(TCHAR));
		p += charLen;
	}

	// send the reply
	return Write(buf.get(), totalCharLen * sizeof(TCHAR));
}

bool PipeManager::WriteOk()
{
	const TCHAR *reply[] = { _T("ok") };
	return WriteReply(reply, countof(reply));
}

bool PipeManager::WriteOk(const TCHAR *param0)
{
	const TCHAR *reply[] = { _T("ok"), param0 };
	return WriteReply(reply, countof(reply));
}

bool PipeManager::WriteOk(const TCHAR *param0, const TCHAR *param1)
{
	const TCHAR *reply[] = { _T("ok"), param0, param1 };
	return WriteReply(reply, countof(reply));
}

bool PipeManager::WriteOk(const TCHAR *param0, const TCHAR *param1, const TCHAR *param2)
{
	const TCHAR *reply[] = { _T("ok"), param0, param1, param2 };
	return WriteReply(reply, countof(reply));
}

bool PipeManager::WriteOk(const TCHAR *param0, const TCHAR *param1, const TCHAR *param2, const TCHAR *param3)
{
	const TCHAR *reply[] = { _T("ok"), param0, param1, param2, param3 };
	return WriteReply(reply, countof(reply));
}

bool PipeManager::WriteError(const TCHAR *message)
{
	const TCHAR *reply[] = { _T("error"), message };
	return WriteReply(reply, countof(reply));
}

// -----------------------------------------------------------------------
// 
// Application context
//
class Application : public InputManager::RawInputReceiver, public JoystickManager::JoystickEventReceiver
{
public:
	Application()
	{
		lastInputEventTime = GetTickCount();
	}

	~Application()
	{
	}

	// InputManager::RawInputReceiver implementation
	virtual bool OnRawInputEvent(UINT rawInputCode, RAWINPUT *raw, DWORD dwSize) override;

	// JoystickManager::JoystickEventReceiver implementation
	virtual bool OnJoystickButtonChange(JoystickManager::PhysicalJoystick *js,
		int button, bool pressed, bool foreground) override;

	// Timer
	void OnGameTimeoutTimer();

	// timer IDs
	static const int gameTimeoutTimerID = 1;

	// subscribe to raw input and joystick events
	bool SubscribeInputEvents()
	{
		// initialize raw input
		auto im = InputManager::GetInstance();
		if (!im->InitRawInput(hwnd))
			return false;

		// subscribe to raw input events
		im->SubscribeRawInput(this);

		// subscribe to joystick events
		JoystickManager::GetInstance()->SubscribeJoystickEvents(this);

		// success
		return true;
	}

	// Launch the UI
	bool LaunchUI(int nCmdShow, ErrorHandler &eh);

	// Process input
	void ProcessInput(const TCHAR *input, size_t inputCharLen);

	// Launch a program
	void LaunchProgram(const std::vector<TSTRING> &request);

	// Terminate the last launched game program
	void KillGameProgram();

	// set the Exit Game keys
	void SetExitGameKeys(const std::vector<TSTRING> &request);

	// Create the message window
	void CreateMessageWindow();

	// message window proc
	static LRESULT CALLBACK MessageWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		
	// pipe manager
	PipeManager pipeMan;

	// Main UI process (PinballY.exe)
	HandleHolder hMainProc;

	// Current launched game process
	HandleHolder hGameProc;
	DWORD tidGameMainThread;

	// Inactivity timeout for the running game, in milliseconds
	DWORD inactivityTimeout;

	// message window handle
	HWND hwnd;

	// Exit game keys, as VKEY (VK_xxx) values
	std::list<int> exitGameKeys;

	// Exit Game joystick buttons
	struct JSButton
	{
		JSButton(int unit, int btn) : unit(unit), btn(btn) { }

		int unit;       // logical unit; -1 means "any unit"
		int btn;		// button number
	};
	std::list<JSButton> exitGameButtons;

	// last keyboard or joystick event time, as a GetTickCount() value
	DWORD lastInputEventTime;
};

// -----------------------------------------------------------------------
//
// Launch the main PinballY process.  We want to run the main process
// with ordinary user privileges, which is complicated because we're
// running elevated (that is, as Administrator).  Launching a non-
// elevated process from an elevated process requires creating the
// new process with a non-elevated primary token.  You'd think this
// would just be a matter of stripping the Administrator privileges
// out of our own process token, but that's actually not a reliable
// way to do it, as explained by Microsoft's Raymond Chen, here:
// https://blogs.msdn.microsoft.com/oldnewthing/20131118-00/?p=2643.
// The method he recommends is to find another process running under 
// the same interactive session, and copy its token.  The process of
// choice is the Windows Explorer desktop shell process, as that's
// usually present in an interactive session and usually runs non-
// elevated.  So the procedure is as follows: get the shell window
// handle, get that window's owner process, get that process's token,
// duplicate that token into a new primary token, and launch the new
// process under the new primary token using CreateProcessWithToken().
// This will fail if the user has killed the Explorer.exe process,
// or is running it elevated, but those are probably rare enough
// cases that we can ignore them.  (One StackOverflow post suggests
// iterating over a process snapshot from ToolHelp32 to find a 
// suitable process to copy the token from, to remove the dependency
// on the shell window being present.  I'm not sure that approach
// would actually be more reliable: if we're running under such 
// unusual conditions that a non-elevated shell window isn't present,
// I don't think we can count on *any* non-elevated processes being
// present in the same interactive session.)
//
bool Application::LaunchUI(int nCmdShow, ErrorHandler &eh)
{
	auto RetErr = [&eh](int msgId, const TCHAR *where)
	{
		WindowsErrorMessage err;
		eh.SysError(LoadStringT(msgId),
			MsgFmt(_T("%s: system error %d, %s"), where, err.GetCode(), err.Get()));
		return false;
	};
	auto SysErr = [&eh, &RetErr](const TCHAR *where) { return RetErr(IDS_ERR_STARTUP, where); };
	auto ShellErr = [&eh, &RetErr](const TCHAR *where) { return RetErr(IDS_ERR_NOSHELL, where); };

	// get our process token 
	HandleHolder hToken;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
		return SysErr(_T("OpenProcessToken failed"));

	// enable SeIncreaseQuotaPrivilege in the process token - this is required
	// to launch a process with a specified token
	if (!SetPrivilege(hToken, SE_INCREASE_QUOTA_NAME, TRUE, eh))
		return false;

	// get the shell window handle
	HWND hwndShell = GetShellWindow();
	if (hwndShell == NULL)
		return ShellErr(_T("GetShellWindow() is NULL"));

	// get its process ID
	DWORD shellPid;
	DWORD shellTid = GetWindowThreadProcessId(hwndShell, &shellPid);

	// open the process
	HandleHolder hShellProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shellPid);
	if (hShellProc == NULL)
		return ShellErr(_T("OpenProcess(shellPid"));

	// get the shell process token
	HandleHolder hDupToken;
	if (!OpenProcessToken(hShellProc, TOKEN_DUPLICATE, &hDupToken))
		return ShellErr(_T("OpenProcessToken(shellPid)"));

	// duplicate the token into a new primary token
	HandleHolder hNewPrimaryToken;
	if (!DuplicateTokenEx(hDupToken,
		TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
		NULL, SecurityImpersonation, TokenPrimary, &hNewPrimaryToken))
		return SysErr(_T("DuplicateTokenEx(shell process token)"));

	// Create the pipes for communicating with the child process.
	// Note that most sample code you might see that creates pipes
	// to pass to a child process for redirection will also set up
	// a security descriptor here that specifies that the handles
	// are to be inheritable.  That's not necessary in this case
	// because of the unusual properties of CreateProcessWithToken
	// described below, specifically that handles can NEVER be
	// inherited via that API no matter what security attributes
	// they have when created.
	HandleHolder hChildInRead, hChildOutWrite;
	if (!CreatePipeEx(&pipeMan.hPipeRead, &hChildOutWrite, NULL, 0, FILE_FLAG_OVERLAPPED, 0)
		|| !CreatePipeEx(&hChildInRead, &pipeMan.hPipeWrite, NULL, 0, FILE_FLAG_OVERLAPPED, 0))
		return SysErr(_T("Creating pipes"));

	// get our application folder - this is the working folder for
	// the new child process, as well as the path to the .exe
	TCHAR dir[MAX_PATH], exe[MAX_PATH];
	GetModuleFileName(G_hInstance, dir, countof(dir));
	PathRemoveFileSpec(dir);

	// build the full .exe filename path
	PathCombine(exe, dir, _T("PinballY.exe"));

	// build the command line
	TSTRING cmdline = MsgFmt(_T(" /AdminHost:%ld"), (long)GetCurrentProcessId()).Get();

	// Set up the STARTUPINFO for the new process.  Note that we pass the
	// pipe handles in the standard input and output handles.  Note while
	// this LOOKS like the standard Unix-ish pattern for redirecting the
	// console handles, it's NOT why we're doing it in this case.  We'd
	// actually rather have the child just inherit the pipe handles and 
	// convey the handle IDs by some more general means, such as command 
	// line arguments.  But that's not possible with this form of process
	// creation!  CreateProcessWithTokenW() doesn't have the normal
	// 'bInheritHandles' flag that all of the other process creation
	// APIs have, because it CAN'T provide handle inheritance even if it
	// wanted to.  The "child" process we're creating isn't going to be
	// our child at all in any strict sense: it's actually going to be a
	// child of an unrelated system service process, which the API has
	// to go through to carry out this unusual form of process startup.
	// Fortunately, the API provides this special form of handle passing
	// through the hStdXxx handles in the STARTUPINFO struct.  What's
	// actually going on inside the API is that the service will dup the
	// hStdXxx handles into the new process, and then plug the dup'd
	// handle IDs into some special memory locations in the new proc's
	// memory space, where its startup code will find the handles and
	// provide them to the program code via the GetStdHandle() API.  So
	// it ends up looking like regular Unix-style redirect inheritance
	// even though it's not.  I only belabor this in case someone wants
	// to change this to normal handle inheritance so that they can
	// repurpose the hStdXxx slots for something else.  You can't do
	// that, beacuse regular handle inheritance is impossible.  If we
	// ever do need to pass down more than three handles, the way to do
	// it as follows: keep passing the pipe handles as they are now;
	// after the CreateProcessWithTokenW() call succeeds, use
	// DuplicateHandleEx() to explicitly create duplicates of the 
	// additional handles you want to pass in the new process's memory
	// space; then use a service call on the pipe (which you'll have 
	// to add to our pipe protocol) that marshalls the duplicated
	// handles into strings or packed byte arrays and passes them
	// across the 'child in' pipe.  Fortunately, the only handles we
	// need to pass for now are the pipe handles, so we get off easy
	// by passing them in the 'si' slots.
	STARTUPINFO si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	si.hStdInput = hChildInRead;
	si.hStdOutput = hChildOutWrite;
	si.hStdError = NULL;
	si.wShowWindow = nCmdShow;

	// launch the process using the new primary token
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));
	if (!CreateProcessWithTokenW(hNewPrimaryToken, 0, exe, cmdline.data(), 0, NULL, dir, &si, &pi))
		return SysErr(_T("CreateProcessWithTokenW() failed"));

	// we don't need the main thread token
	CloseHandle(pi.hThread);

	// remember the main UI process handle
	hMainProc = pi.hProcess;

	// success
	return TRUE;
}


// -----------------------------------------------------------------------
//
// Launch a program.  This processes a "run" command from the client.
//
void Application::LaunchProgram(const std::vector<TSTRING> &params)
{
	// "run" request parameters:
	//
	// [0] = "run" verb
	//
	// [1] = exe file -  full path to the executable file
	//
	// [2] = working directory - the full path of the working directory 
	//       for the new process
	//
	// [3] = command line for the new process
	//
	// [4] = inactivity timeout, in milliseconds (zero if disabled)

	// make sure we have enough parameters
	if (params.size() < 5)
	{
		// send an error reply
		pipeMan.WriteError(_T("Invalid 'run' request syntax: not enough parameters"));
		return;
	}

	// Pull out the parameters.  (Note that we need a writable buffer
	// for the command line to conform to the CreateProcess() interface,
	// so copy that one out to a new TSTRING.  We just need const 
	// pointers to the rest.)
	const TCHAR *exe = params[1].c_str();
	const TCHAR *workingDir = params[2].c_str();
	TSTRING cmdline = params[3];
	inactivityTimeout = _ttoi(params[4].c_str());

	// set up the startup information struct
	STARTUPINFO startupInfo;
	ZeroMemory(&startupInfo, sizeof(startupInfo));
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.dwFlags = STARTF_USESHOWWINDOW;
	startupInfo.wShowWindow = SW_SHOW;

	// Try launching the new process
	PROCESS_INFORMATION procInfo;
	ZeroMemory(&procInfo, sizeof(procInfo));
	if (!CreateProcess(exe, cmdline.data(), 0, 0, false, 0, NULL,
		workingDir, &startupInfo, &procInfo))
	{
		// error launching
		WindowsErrorMessage err;
		pipeMan.WriteError(MsgFmt(_T("Admin Host CreateProcess() failed, error %d: %s"),
			err.GetCode(), err.Get()));
		return;
	}

	// Succesful launch - pass back the new process information to the client
	pipeMan.WriteOk(MsgFmt(_T("%ld"), procInfo.dwProcessId));

	// store the process handle and main thread of the current game
	hGameProc = procInfo.hProcess;
	tidGameMainThread = procInfo.dwThreadId;

	// we don't need the thread handle - close it 
	CloseHandle(procInfo.hThread);

	// wait for the new process's UI to initialize
	WaitForInputIdle(hGameProc, 5000);

	// Make sure it's in the foreground.  Windows treats elevated
	// processes differently from normal user processes, in that it
	// doesn't automatically bring them to the foreground when they
	// open their first window.  With an elevated process, Windows 
	// just does the blinky-orange thing in the task bar.  The 
	// supposition seems to be that all Admin processes are started 
	// by asynchronous system events rather than user actions, so 
	// you don't want them abruptly taking over the UI when the user
	// might be in the middle of something else.  You'd think they'd
	// just put something in the CreateProcess flags saying whether
	// the launch is due to user action or background events, but as
	// is so often the case in Windows, Microsoft prefers their too-
	// clever-by-half heuristics to complete APIs.  Anyway, as often
	// happens, the heuristics get it wrong in this case; this *is*
	// a user-initiated launch, and we *do* want the new window to
	// come to the foreground on launch.  Fortunately, we can make
	// accomplish that via SetForegroundWindow().  Note that this 
	// requires cooperation from the PinballY process, since it
	// owns the UI window that's presumably in the foreground at
	// the moment of the launch.  To do a cross-process SFW(),
	// the process with the current foreground window (our client
	// process in this case) needs to delegate foreground-setting
	// authority to us by calling AllowSetForegroundWindow() with
	// our PID.  It should do this just before the launch request 
	// is sent, since the authority only lasts until the next mouse
	// or keyboard event targeting the UI window.
	HWND hwnd = FindMainWindowForProcess(procInfo.dwProcessId, nullptr);
	if (hwnd != NULL)
		SetForegroundWindow(hwnd);

	// If there's an inactivity timeout, set a timer to go off in
	// that amount of time.
	if (inactivityTimeout != 0)
		SetTimer(this->hwnd, gameTimeoutTimerID, inactivityTimeout, NULL);
}

// Terminate the last launched program
void Application::KillGameProgram()
{
	// if the game process is still running, try to close its windows
	if (hGameProc != NULL && WaitForSingleObject(hGameProc, 0) == WAIT_TIMEOUT)
	{
		// Find the main window for our UI process
		DWORD tidMainUI;
		HWND hwndMainUI = FindMainWindowForProcess(GetProcessId(hMainProc), &tidMainUI);

		// Try closing one game window at a time.  Repeat until we
		// don't find any windows to close, or we reach a maximum
		// retry limit (so that we don't get stuck if the game 
		// refuses to close).
		for (int tries = 0; tries < 20; ++tries)
		{
			// look for a window to close
			struct CloseContext
			{
				CloseContext(HWND hwndMainUI) : found(false), hwndMainUI(hwndMainUI) { }
				bool found;
				HWND hwndMainUI;
			} closeCtx(hwndMainUI);
			EnumThreadWindows(tidGameMainThread, [](HWND hWnd, LPARAM lParam)
			{
				// get the context
				auto ctx = reinterpret_cast<CloseContext*>(lParam);

				// Try bringing our main window to the foreground before 
				// closing the game window, so that the taskbar doesn't
				// reappear between closing the game window and activating
				// our window, assuming we're in full-screen mode.  Explorer 
				// normally hides the taskbar when a full-screen window is
				// in front, but only when it's in front.
				if (ctx->hwndMainUI != NULL)
					SetForegroundWindow(ctx->hwndMainUI);

				// If the window is visible and enabled, close it.  Don't try 
				// to close hidden or disabled windows; doing so can crash VP
				// if it's showing a dialog.
				if (IsWindowVisible(hWnd) && IsWindowEnabled(hWnd))
				{
					// this window looks safe to close - try closing it
					SendMessage(hWnd, WM_SYSCOMMAND, SC_CLOSE, 0);

					// note that we found something to close, and stop the
					// enumeration
					ctx->found = true;
					return FALSE;
				}

				// continue the enumeration
				return TRUE;
			}, reinterpret_cast<LPARAM>(&closeCtx));

			// if we didn't find any windows to close on this pass, stop
			// looping
			if (!closeCtx.found)
				break;

			// pause briefly between iterations to give the program a chance
			// to update its windows; stop if the process exits
			if (hGameProc == NULL || WaitForSingleObject(hGameProc, 1000) != WAIT_TIMEOUT)
				break;
		}

		// close our handle
		hGameProc = NULL;
	}
}

//
// Set the EXIT GAME keys passed from the client
//
void Application::SetExitGameKeys(const std::vector<TSTRING> &params)
{
	// clear any previous settings
	exitGameKeys.clear();
	exitGameButtons.clear();

	// parse the parameters from the client
	std::basic_regex<TCHAR> kbPat(_T("kb (\\d+)"));
	std::basic_regex<TCHAR> anyJsPat(_T("js (\\d+)"));
	std::basic_regex<TCHAR> oneJsPat(_T("js (\\d+) ([a-f\\d]+) ([a-f\\d+]) (.*)"), std::regex_constants::icase);
	for (size_t i = 1; i < params.size(); ++i)
	{
		std::match_results<TSTRING::const_iterator> m;
		if (std::regex_match(params[i], m, kbPat))
		{
			// keyboard key - add the VKEY to the key list
			int vk = _ttoi(m[1].str().c_str());
			exitGameKeys.emplace_back(vk);
		}
		else if (std::regex_match(params[i], m, anyJsPat))
		{
			// joystick button, any unit
			int btn = _ttoi(m[1].str().c_str());
			exitGameButtons.emplace_back(-1, btn);
		}
		else if (std::regex_match(params[i], m, oneJsPat))
		{
			// joystick button, specific unit
			int btn = _ttoi(m[1].str().c_str());
			int vid = (int)_tcstol(m[2].str().c_str(), nullptr, 16);
			int pid = (int)_tcstol(m[3].str().c_str(), nullptr, 16);
			const TSTRING &prodName = m[4].str();

			// look up the unit
			if (auto js = JoystickManager::GetInstance()->AddLogicalJoystick(vid, pid, prodName.c_str()); js != nullptr)
				exitGameButtons.emplace_back(js->index, btn);
		}
	}
}

// Handle a timer event
void Application::OnGameTimeoutTimer()
{
	// Check the time since the last input event
	DWORD dt = GetTickCount() - lastInputEventTime;
	if (dt < inactivityTimeout)
	{
		// We've had an event within the inactivity timeout period.
		// Reset the timer for the remaining interval.
		SetTimer(hwnd, gameTimeoutTimerID, inactivityTimeout - dt, NULL);
	}
	else
	{
		// There's been no activity for at least the timeout period,
		// so kill the game.
		KillGameProgram();
	}
}

// -----------------------------------------------------------------------
//
// Process a pipe input message.  Our standard message format consists
// of one or more null-terminated strings packed into a buffer.  The
// first string is the "verb", which specifies what type of action we're
// to carry out.  Any additional strings are parameters, whose meaning
// depends on the verb.
//
void Application::ProcessInput(const TCHAR *input, size_t inputCharLen)
{
	// break the buffer into strings at the null separators
	std::vector<TSTRING> request;
	const TCHAR *p = input;
	const TCHAR *start = p;
	const TCHAR *endp = p + inputCharLen;
	for (; p != endp; ++p)
	{
		if (*p == 0)
		{
			request.emplace_back(start, p - start);
			start = p + 1;
		}
	}

	// if there's a final non-null-terminated fragment, add it
	if (start != p)
		request.emplace_back(start, p - start);

	// check which verb we have
	if (request[0] == _T("run"))
		LaunchProgram(request);
	else if (request[0] == _T("kill"))
		KillGameProgram();
	else if (request[0] == _T("exitGameKeys"))
		SetExitGameKeys(request);
}

// -----------------------------------------------------------------------
//
// Message handler window.  We don't present any UI of our own (other than 
// message boxes for prompts and errors), but we need a window internally 
// to can receive Raw Input events and timer events.  We set this up as a 
// hidden window so that it can receive messages but doesn't have any UI
// presence.
//

// message handler window proc
LRESULT CALLBACK Application::MessageWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// Get the Application object.  If this is the initial WM_NCCREATE,
	// it's stashed in the lpCreateParams element of the CREATESTRUCT
	// in the LPARAM, per normal Windows conventions.  In processing
	// WM_NCCREATE, we retrieve it from the CREATESTRUCT and stash it
	// in the private window data at Window Long slot #0, so for any
	// other message, we can recover it from the window data.
	Application *app;
	if (message == WM_NCCREATE)
	{
		// get our 'self' parameter
		const CREATESTRUCT *cs = (const CREATESTRUCT *)lParam;
		app = static_cast<Application*>(cs->lpCreateParams);

		// store the pointer in the window extra data slot, so that
		// can look it up for future messages
		SetWindowLongPtr(hWnd, 0, reinterpret_cast<LONG_PTR>(app));

		// store our window handle internally
		app->hwnd = hWnd;
	}
	else
	{
		// the Application object is in window long #0
		app = reinterpret_cast<Application*>(GetWindowLongPtr(hWnd, 0));
	}

	// check the message type
	switch (message)
	{
	case WM_INPUT:
		// Raw Input event - pass it to the Input Manager
		InputManager::GetInstance()->ProcessRawInput(
			GET_RAWINPUT_CODE_WPARAM(wParam),
			reinterpret_cast<HRAWINPUT>(lParam));

		// note that continuing to the DefWindowProc is mandatory, since
		// the system releases buffer resource there
		break;
		
	case WM_INPUT_DEVICE_CHANGE:
		// Raw input device change event - pass it to the Input Manager
		InputManager::GetInstance()->ProcessDeviceChange(
			(USHORT)wParam, reinterpret_cast<HANDLE>(lParam));
		break;  

	case WM_TIMER:
		if (app != nullptr)
		{
			switch (wParam)
			{
			case gameTimeoutTimerID:
				app->OnGameTimeoutTimer();
				break;
			}
		}
		break;
	}

	// call the default window proc if we didn't intercept the message
	return DefWindowProc(hWnd, message, wParam, lParam);
}


// create our message handler window
void Application::CreateMessageWindow()
{
	// set up our class descriptor
	const TCHAR *wndClassName = _T("PinballY.AdminMode.MesageWindow");
	WNDCLASSEX wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = MessageWindowProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = sizeof(LONG_PTR);
	wcex.hInstance = G_hInstance;
	wcex.hIcon = 0;
	wcex.hIconSm = 0;
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
	wcex.lpszMenuName = 0;
	wcex.lpszClassName = wndClassName;

	// register the class
	RegisterClassEx(&wcex);

	// Create our window
	hwnd = CreateWindow(wndClassName, _T("PinballY Admin Host"),
		WS_OVERLAPPEDWINDOW,
		0, 0, 100, 100, NULL, NULL, G_hInstance, this);
}


// -----------------------------------------------------------------------
//
// Raw input processing
//
bool Application::OnRawInputEvent(UINT rawInputCode, RAWINPUT *raw, DWORD dwSize)
{
	// If this is a keyboard event, and it's a "make" (key down)
	// event, check to see if it's an Exit Game key.  Note that
	// the proper test for "key down" (or "make") is "NOT key up"
	// (aka "break") because of the peculiar choice of bit field 
	// definitions in the Windows SDK headers.
	if (raw->header.dwType == RIM_TYPEKEYBOARD)
	{
		// note the event time
		lastInputEventTime = GetTickCount();

		// check for 'make' (key down) mode
		if (!(raw->data.keyboard.Flags & RI_KEY_BREAK))
		{
			// get the vkey
			USHORT vkey = raw->data.keyboard.VKey;

			// check for a match to an Exit Game key
			if (auto it = findex(exitGameKeys, vkey); it != exitGameKeys.end())
				KillGameProgram();
		}
	}

	// allow forwarding the event to other subscribers
	return false;
}

//
// Joystick event processing
//
bool Application::OnJoystickButtonChange(
	JoystickManager::PhysicalJoystick *js,
	int button, bool pressed, bool foreground)
{
	// note the event time
	lastInputEventTime = GetTickCount();

	// if the button was pressed, check if it's an Exit Game button
	if (pressed)
	{
		// check for a match to an Exit Game button
		for (auto const &b : exitGameButtons)
		{
			// Get the logical joystick number that this button press came from
			int unitFrom = js->logjs != nullptr ? js->logjs->index : -1;

			// If the button number matches, and either the button assignment
			// is for "all units" (indicated by -1) or it's for the actual
			// unit the event is coming from, it's a match.
			if (b.btn == button && (b.unit == -1 || b.unit == js->logjs->index))
			{
				// kill the game
				KillGameProgram();

				// we only need to terminate it once, so no need to keep looking
				// for more button matches
				break;
			}
		}
	}

	// allow forwarding the event to other subscribers
	return false;
}


// -----------------------------------------------------------------------
//
// Main entrypoint
//
int APIENTRY wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE /*hPrevInstance*/,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	// save the instance handle globally
	G_hInstance = hInstance;

	// Check to see if we're running with admin privileges.  There's no
	// point in using this without, as we wouldn't add any capabilities
	// that the standalone PinballY doesn't already have.
	InteractiveErrorHandler eh;
	WellKnownSid sid(WellKnownSid::Administrators());
	BOOL isMember = FALSE;
	if (!CheckTokenMembership(NULL, &sid, &isMember) || !isMember)
	{
		// not in admin mode - show an error and abort
		eh.Error(LoadStringT(IDS_ERR_ADMINREQUIRED));
		return 0;
	}

	// If we haven't ever run on this system, check to see if the user
	// really wants to run in admin mode, showing a message explaining
	// that this should be avoided unless they're sure it's necessary.
	const TCHAR *confirmFile = _T(".AdminModeConfirmed");
	if (!PathFileExists(confirmFile))
	{
		if (MessageBox(NULL, LoadStringT(IDS_ERR_CONFIRM), _T("PinballY Admin Mode"), MB_YESNO | MB_ICONWARNING) == IDYES)
		{
			// "Yes" - create the confirmation file so that we don't repeat the question
			FILE *fp;
			if (_tfopen_s(&fp, confirmFile, _T("w")))
			{
				_ftprintf(fp, _T("Confirmed\n"));
				fclose(fp);
			}
		}
		else
		{
			// "No" - abort
			return 0;
		}
	}

	// Use an RAII object to sequence the creation and destruction of
	// the Application and Input Manager objects.
	struct Initializer
	{
		Initializer()
		{
			// initialize the input manager first
			ok = InputManager::Init();

			// create the app object
			app = new Application();

			// create its message handler window
			app->CreateMessageWindow();

			// subscribe the app object to raw input and joystick events
			if (ok)
				ok = app->SubscribeInputEvents();
		}

		~Initializer()
		{
			// the application subscribes to input manager and joystick
			// events, so delete it first
			delete app;
			app = nullptr;

			// now shut down the input manager
			InputManager::Shutdown();
		}

		bool ok;
		Application *app;
	} initializer;

	// if we didn't initialize those objects, abort
	if (!initializer.ok)
		return 0;

	// create our message handler window
	Application *app = initializer.app;

	// launch the main UI
	if (!app->LaunchUI(nCmdShow, eh))
		return 0;

	// start the asynchronous pipe read
	if (!app->pipeMan.StartRead(eh))
		return 0;

	// create a dummy event to use as a placeholder in the wait list 
	// when necessary
	HandleHolder hDummyEvent(CreateEvent(NULL, FALSE, FALSE, NULL));
	
	// Main message loop
	for (bool done = false; !done; )
	{
		// Set up the event list.  We might not have a game process running,
		// in which case that handle will be null.  But the wait API doesn't
		// like null handles, so substitute our dummy event in this case; it's
		// just a placeholder as it will never fire.
		HANDLE events[] = { 
			app->pipeMan.hReadEvent, 
			app->hMainProc, 
			app->hGameProc != NULL ? app->hGameProc : hDummyEvent
		};

		// wait for an event or message
		MSG msg;
		switch (MsgWaitForMultipleObjects(countof(events), events, FALSE, INFINITE, QS_ALLEVENTS))
		{
		case WAIT_OBJECT_0:
			// read pipe has data ready
			{
				std::unique_ptr<TCHAR> data;
				size_t charlen;
				if (app->pipeMan.FinishRead(data, charlen, eh))
					app->ProcessInput(data.get(), charlen);
			}
			break;

		case WAIT_OBJECT_0 + 1:
			// the main UI client program exited - terminate the server
			done = true;
			break;

		case WAIT_OBJECT_0 + 2:
			// the current running game exited - clear the handle
			app->hGameProc = NULL;

			// cancel any inactivity timer
			KillTimer(app->hwnd, app->gameTimeoutTimerID);
			break;

		case WAIT_OBJECT_0 + 3:
			// message available
			while (PeekMessage(&msg, nullptr, 0, 0, TRUE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			break;

		case WAIT_TIMEOUT:
			// not currently used - ignore and continue
			break;

		case WAIT_ABANDONED:
			// ignore and continue
			break;

		default:
			// error
			{
				WindowsErrorMessage err;
				eh.SysError(LoadStringT(IDS_ERR_GETEVENT),
					MsgFmt(_T("MsgWaitForMultipleObject failed, error %d, %s"), err.GetCode(), err.Get()));
			}
			done = true;
			break;
		}
	}

	// close the message handler window
	DestroyWindow(app->hwnd);

	// done
	return 0;
}
