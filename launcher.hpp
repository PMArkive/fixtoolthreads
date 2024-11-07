#include <Windows.h>

#define APP_PREFIX "[FixToolThreads] "

char* GetLastErrorString()
{
	static char err[2048];
	LPVOID lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0,
		NULL
	);
	strncpy_s(err, (char*)lpMsgBuf, sizeof(err));
	LocalFree(lpMsgBuf);
	err[sizeof(err) - 1] = 0;
	return err;
}

struct ICommandLine
{
	virtual void CreateCmdLine(const char* commandline) = 0;
	virtual void CreateCmdLine(int argc, char** argv) = 0;
};
bool CreateCmdLine(int argc, char* argv[])
{
	HMODULE tier0 = LoadLibrary("tier0.dll");
	if (!tier0)
		return false;
	typedef ICommandLine* (*CommandLine_Tier0)();
	CommandLine_Tier0 cmdline = (CommandLine_Tier0)GetProcAddress(tier0, "CommandLine_Tier0");
	if (!cmdline)
		return false;
	cmdline()->CreateCmdLine(argc, argv);
	return true;
}

#define CREATEINTERFACE_PROCNAME	"CreateInterface"
typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);

#define LAUNCHABLE_DLL_INTERFACE_VERSION "launchable_dll_1"
struct ILaunchableDLL
{
	virtual int main(int argc, char** argv) = 0;
};