#define VVIS
#include "../launcher.hpp"
#include "../patch.hpp"

int main(int argc, char* argv[])
{
	if (!CreateCmdLine(argc, argv))
	{
		Log("ERROR: Failed to setup tier0 commandline\n");
		return 1;
	}

	const char* pDLLName = "vvis_dll.dll";

	HMODULE module = LoadLibraryEx(pDLLName, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module)
	{
		Log("vvis launcher error: can't load %s\n%s", pDLLName, GetLastErrorString());
		return 1;
	}

	CreateInterfaceFn fn = (CreateInterfaceFn)GetProcAddress(module, CREATEINTERFACE_PROCNAME);
	if (!fn)
	{
		Log("vvis launcher error: can't get factory from %s\n", pDLLName);
		FreeLibrary(module);
		return 2;
	}

	ApplyThreadPatches(module);

	int retCode = 0;
	ILaunchableDLL* pDLL = (ILaunchableDLL*)fn(LAUNCHABLE_DLL_INTERFACE_VERSION, &retCode);
	if (!pDLL)
	{
		Log("vvis launcher error: can't get interface from %s\n", pDLLName);
		FreeLibrary(module);
		return 3;
	}

	int ret = pDLL->main(argc, argv);
	FreeLibrary(module);

	return ret;
}