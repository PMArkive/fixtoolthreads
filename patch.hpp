#include <Windows.h>
#include <Psapi.h>
#include "detours/detours.h"
#include <stdio.h>
#include <string>
#include <map>

#define SIG(bytes, mask) sig_t({#bytes, #mask, sizeof(#bytes) - 1, 0, 0})
#define SIG2(bytes, mask, offset, index) sig_t({#bytes, #mask, sizeof(#bytes) - 1, offset, index})
#define OFFSET(offset) sig_t({nullptr, nullptr, offset})
struct sig_t { const char* sig; const char* mask; size_t len; unsigned short offset; unsigned short index; };

static void* FindSignature(const MODULEINFO& info, const sig_t& sig)
{
	uintptr_t start = (uintptr_t)info.lpBaseOfDll;

	if (sig.sig)
	{
		int count = 0;
		uintptr_t end = start + info.SizeOfImage - sig.len;
		for (uintptr_t i = start; i < end; i++)
		{
			bool found = true;
			for (size_t j = 0; j < sig.len; j++)
				found &= (sig.mask[j] == '?') || (sig.sig[j] == *(char*)(i + j));
			if (found)
			{
				if (count++ == sig.index)
					return (void*)(i + sig.offset);
			}
		}
		return nullptr;
	}
	else
	{
		return (void*)(start + sig.len);
	}
}

template <typename T>
inline T PatchMemory(void* Address, T Value)
{
	T PrevMemory = *(T*)Address;
	unsigned long PrevProtect, Dummy;
	VirtualProtect(Address, sizeof(T), PAGE_EXECUTE_READWRITE, &PrevProtect);
	*(T*)Address = Value;
	VirtualProtect(Address, sizeof(T), PrevProtect, &Dummy);
	return PrevMemory;
}

double Plat_FloatTime()
{
	static bool init = false;
	static LARGE_INTEGER clock_start;
	static double to_seconds;
	if (!init)
	{
		init = true;
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&clock_start);
		to_seconds = 1.0 / frequency.QuadPart;
	}
	LARGE_INTEGER CurrentTime;
	QueryPerformanceCounter(&CurrentTime);
	return (double)(CurrentTime.QuadPart - clock_start.QuadPart) * to_seconds;
}

static int g_LastPacifierDrawn = -1;

void UpdatePacifier(float flPercent)
{
	int iCur = (int)(flPercent * 40.0f);
	if (iCur < g_LastPacifierDrawn) iCur = g_LastPacifierDrawn;
	if (iCur > 40) iCur = 40;

	if (iCur != g_LastPacifierDrawn)
	{
		for (int i = g_LastPacifierDrawn + 1; i <= iCur; i++)
		{
			if (!(i % 4))
			{
				printf("%d", i / 4);
			}
			else
			{
				if (i != 40)
				{
					printf(".");
				}
			}
		}
		g_LastPacifierDrawn = iCur;
	}
}

void StartPacifier(char const* pPrefix)
{
	printf("%s", pPrefix);
	g_LastPacifierDrawn = -1;
	UpdatePacifier(0.001f);
}

void EndPacifier(bool bCarriageReturn)
{
	UpdatePacifier(1);
	if (bCarriageReturn)
		printf("\n");
}

typedef void (*ThreadWorkerFn)(int iThread, int iWorkItem);
typedef void (*RunThreadsFn)(int iThread, void* pUserData);

#define ATOMIC

static int* g_pNumThreads = nullptr;
static ThreadWorkerFn workfunction;
static CRITICAL_SECTION crit;
static struct crit_init_t { crit_init_t() { InitializeCriticalSection(&crit); } } crit_init;
static int crit_enter = 0;
static int workcount;
static bool	pacifier;

#ifdef ATOMIC
#include <atomic>
static std::atomic_flag pacifierLock;
static std::atomic<int> dispatch;
#else
static int dispatch;
#endif

struct thread_data_t
{
	int          m_iThread;
	void*        m_pUserData;
	RunThreadsFn m_Fn;
};
static thread_data_t* g_RunThreadsData = nullptr;
static HANDLE*        g_ThreadHandles = nullptr;

#ifdef VRAD
struct DispTested_t
{
	int	m_Enum;
	int* m_pTested;
};
static DispTested_t* s_DispTested = nullptr;
static char* g_pDispTestedRefs[4] = { 0 };
#endif

void ThreadSetDefault()
{
	SYSTEM_INFO info;

	int& numthreads = *g_pNumThreads;
	if (numthreads == -1)
	{
		GetSystemInfo(&info);
		numthreads = info.dwNumberOfProcessors;
	}

	delete[] g_RunThreadsData;
	delete[] g_ThreadHandles;
	g_RunThreadsData = new thread_data_t[numthreads];
	g_ThreadHandles  = new HANDLE[numthreads];

#ifdef VRAD
	s_DispTested     = new DispTested_t[numthreads + 1]();
	for (int i = 0; i < _ARRAYSIZE(g_pDispTestedRefs); i++)
		PatchMemory<DispTested_t*>(g_pDispTestedRefs[i] + 3, s_DispTested);
#endif

	printf("%i threads\n", numthreads);
}

void ThreadLock()
{
	EnterCriticalSection(&crit);
	if (crit_enter)
		printf("Recursive ThreadLock\n");
	crit_enter = 1;
}

void ThreadUnlock()
{
	if (!crit_enter)
		printf("ThreadUnlock without lock\n");
	crit_enter = 0;
	LeaveCriticalSection(&crit);
}

#ifdef ATOMIC
int	GetThreadWork()
{
	const int r = dispatch++;
	if (r >= workcount)
		return -1;
	if (!pacifierLock.test_and_set())
	{
		UpdatePacifier(dispatch / (float)workcount);
		pacifierLock.clear();
	}
	return r;
}
#else
int	GetThreadWork()
{
	int	r;

	ThreadLock();

	if (dispatch == workcount)
	{
		ThreadUnlock();
		return -1;
	}

	UpdatePacifier((float)dispatch / workcount);

	r = dispatch;
	dispatch++;
	ThreadUnlock();

	return r;
}
#endif

void ThreadWorkerFunction(int iThread, void* pUserData)
{
	int work;
	while (1)
	{
		work = GetThreadWork();
		if (work == -1)
			break;
		workfunction(iThread, work);
	}
}

DWORD WINAPI InternalRunThreadsFn(LPVOID pParameter)
{
	thread_data_t* pData = (thread_data_t*)pParameter;
	pData->m_Fn(pData->m_iThread, pData->m_pUserData);
	return 0;
}

void RunThreads_Start(RunThreadsFn fn, void* pUserData)
{
	int numthreads = *g_pNumThreads;
	for (int i = 0; i < numthreads; i++)
	{
		g_RunThreadsData[i].m_iThread = i;
		g_RunThreadsData[i].m_pUserData = pUserData;
		g_RunThreadsData[i].m_Fn = fn;

		DWORD dwDummy;
		g_ThreadHandles[i] = CreateThread(
			NULL,
			0,
			InternalRunThreadsFn,
			&g_RunThreadsData[i],
			0,
			&dwDummy);
	}
}

void RunThreads_End()
{
	int numthreads = *g_pNumThreads;
	WaitForMultipleObjects(numthreads, g_ThreadHandles, TRUE, INFINITE);
	for (int i = 0; i < numthreads; i++)
		CloseHandle(g_ThreadHandles[i]);
}

void RunThreadsOn(int workcnt, bool showpacifier, RunThreadsFn fn, void *pUserData)
{
	int	start, end;
	start = (int)Plat_FloatTime();
	dispatch = 0;
	workcount = workcnt;
	StartPacifier("");
	pacifier = showpacifier;
	
	RunThreads_Start( fn, pUserData );
	RunThreads_End();

	end = (int)Plat_FloatTime();
	if (pacifier)
	{
		EndPacifier(false);
		printf (" (%i)\n", end-start);
	}
}

void RunThreadsOnIndividual(int workcnt, bool showpacifier, ThreadWorkerFn func)
{
	workfunction = func;
	RunThreadsOn(workcnt, showpacifier, ThreadWorkerFunction, nullptr);
}

struct detour_t
{
	void* func;
	void* hook;
	sig_t sig;
};
std::map<std::string, detour_t> g_Detours =
{
	{ "ThreadSetDefault",       { nullptr, ThreadSetDefault,       SIG(\x55\x8B\xEC\xA1\x00\x00\x00\x00\x83\xEC\x00\x83\xF8, xxxx????xx?xx) } },
	{ "RunThreads_Start",       { nullptr, RunThreads_Start,       SIG(\x55\x8B\xEC\x51\xA1\x00\x00\x00\x00\xC7\x05, xxxxx????xx) } },
	{ "RunThreads_End",         { nullptr, RunThreads_End,         SIG(\xA1\x00\x00\x00\x00\x56\x6A\x00\x6A, x????xx?x) } },
	{ "RunThreadsOnIndividual", { nullptr, RunThreadsOnIndividual, SIG(\x55\x8B\xEC\x83\xEC\x00\x83\x3D\x00\x00\x00\x00\x00\x75, xxxxx?xx?????x) } },
	{ "RunThreadsOn",           { nullptr, RunThreadsOn,           SIG(\x55\x8B\xEC\x56\x57\xFF\x15, xxxxxxx) } },

#if defined(VVIS)
	// x87
	{ "StartPacifier",          { nullptr, StartPacifier,          SIG(\x55\x8B\xEC\x8B\x45\x00\x50\x68\x00\x00\x00\x00\xFF\x15\x00\x00\x00\x00\xD9\x05, xxxxx?xx????xx????xx) } },
	{ "EndPacifier",            { nullptr, EndPacifier,            SIG(\x55\x8B\xEC\xD9\xE8, xxxxx) } },
#elif defined(VRAD)
	{ "GetThreadWork",          { nullptr, GetThreadWork,          SIG(\x83\x3D\x00\x00\x00\x00\x00\x53\x8B\x1D, xx?????xxx) } },
	{ "ThreadLock",             { nullptr, ThreadLock,             SIG2(\xCC\x83\x3D\x90\xCD\x20\x15\x00\x74\x2C, xxx????xxx, 1, 0) } },
	{ "ThreadUnlock",           { nullptr, ThreadUnlock,           SIG2(\xCC\x83\x3D\x90\xCD\x20\x15\x00\x74\x2C, xxx????xxx, 1, 1) } },

	// SSE
	{ "StartPacifier",          { nullptr, StartPacifier,          SIG(\x55\x8B\xEC\x8B\x45\x00\x50\x68\x00\x00\x00\x00\xFF\x15\x00\x00\x00\x00\xF3\x0F\x10\x05, xxxxx?xx????xx????xxxx) } },
	{ "EndPacifier",            { nullptr, EndPacifier,            SIG(\x55\x8B\xEC\xF3\x0F\x10\x05\x00\x00\x00\x00\x51, xxxxxxx????x) } },
#endif
};

bool PatchDispTests(const MODULEINFO& info)
{
#ifdef VRAD
	// lea edi, addr[edi*8]
	g_pDispTestedRefs[0] = (char*)FindSignature(info, SIG(\x8D\x3C\xFD\x38\x5C\x1D\x11, xxx????));
	if (!g_pDispTestedRefs[0])
	{
		printf(APP_PREFIX "ERROR: Failed to locate DispTested array\n");
		return false;
	}

	char bytes[7];
	// lea ecx, addr[ecx*8]
	bytes[0] = '\x8D';
	bytes[1] = '\x0C';
	bytes[2] = '\xCD';
	*(int*)(bytes + 3) = *(int*)(g_pDispTestedRefs[0] + 3);

	sig_t sig;
	sig.sig = bytes;
	sig.mask = "xxxxxxx";
	sig.len = strlen(sig.mask);
	sig.offset = 0;

	for (int i = 1; i < 4; i++)
	{
		sig.index = i - 1;
		void* disptestedref = FindSignature(info, sig);
		if (!disptestedref)
		{
			printf(APP_PREFIX "ERROR: Failed to locate DispTested array\n");
			return false;
		}
		g_pDispTestedRefs[i] = (char*)disptestedref;
	}
#endif
	return true;
}

void ApplyThreadPatches(HMODULE module)
{
	printf(APP_PREFIX "(%s) Applying patches...\n", __DATE__);

	MODULEINFO info;
	GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));

	bool fail = false;
	for (auto& pair : g_Detours)
	{
		detour_t& detour = pair.second;
		detour.func = FindSignature(info, detour.sig);
		if (!detour.func)
		{
			printf(APP_PREFIX "ERROR: Failed to locate function %s\n", pair.first.c_str());
			fail = true;
		}
	}

	if (fail)
	{
		printf(APP_PREFIX "ERROR: One or more functions failed, unloading patch...\n");
		return;
	}

	g_pNumThreads = *(int**)((unsigned char*)(g_Detours["RunThreadsOnIndividual"].func) + 0x8);
	if (*g_pNumThreads != -1)
	{
		printf(APP_PREFIX "ERROR: Failed to locate numthreads variable, unloading patch...\n");
		return;
	}

	if (!PatchDispTests(info))
		return;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	for (auto& pair : g_Detours)
	{
		detour_t& detour = pair.second;
		DetourAttach(&detour.func, detour.hook);
	}

	DetourTransactionCommit();

	printf(APP_PREFIX "Thread patch applied successfully\n");
}