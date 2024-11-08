#include <Windows.h>
#include <Dbghelp.h>
#include <Psapi.h>
#include "detours/detours.h"
#include <stdio.h>
#include <string>
#include <map>

#pragma comment(lib, "Dbghelp")

#define SIG(bytes, mask) sig_t({#bytes, #mask, sizeof(#bytes) - 1, 0, 0})
#define SIG2(bytes, mask, offset, index) sig_t({#bytes, #mask, sizeof(#bytes) - 1, offset, index})
#define OFFSET(offset) sig_t({nullptr, nullptr, offset})
struct sig_t { const char* sig; const char* mask; size_t len; unsigned short offset; unsigned short index; };
struct segment_t { uintptr_t start, end; };

bool GetModuleSegment(HMODULE module, const char* name, segment_t& segment)
{
	int nNameLen = (int)strlen(name);
	char* pImageBase = (char*)module; 
	IMAGE_NT_HEADERS *pNtHdr = ImageNtHeader(module);
	IMAGE_SECTION_HEADER *pSectionHdr = (IMAGE_SECTION_HEADER *)(pNtHdr + 1);
	for (WORD i = 0; i < pNtHdr->FileHeader.NumberOfSections; i++)
	{
		if (memcmp(pSectionHdr->Name, name, nNameLen) == 0)
		{
			segment.start = (uintptr_t)(pImageBase + pSectionHdr->VirtualAddress);
			segment.end = segment.start + pSectionHdr->Misc.VirtualSize;
			return true;
		}
		pSectionHdr++;
	}
	return false;
}

static void* FindSignature(const segment_t& segment, const sig_t& sig)
{
	if (sig.sig)
	{
		int count = 0;
		uintptr_t end = segment.end - sig.len;
		for (uintptr_t i = segment.start; i < end; i++)
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
		return (void*)(segment.start + sig.len);
	}
}

inline void PatchMemory(void* Address, const void* Value, int Size)
{
	unsigned long PrevProtect, Dummy;
	VirtualProtect(Address, Size, PAGE_EXECUTE_READWRITE, &PrevProtect);
	memcpy(Address, Value, Size);
	VirtualProtect(Address, Size, PrevProtect, &Dummy);
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
	LARGE_INTEGER current_time;
	QueryPerformanceCounter(&current_time);
	return (double)(current_time.QuadPart - clock_start.QuadPart) * to_seconds;
}

static int g_LastPacifierDrawn = -1;

void UpdatePacifier(float flPercent)
{
	int iCur = (int)(flPercent * 40.0f);
	if (iCur < g_LastPacifierDrawn) iCur = g_LastPacifierDrawn;
	if (iCur >= 40) iCur = flPercent < 1.0 ? 39 : 40;

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
		PatchMemory(g_pDispTestedRefs[i] + 3, &s_DispTested, sizeof(s_DispTested));
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

void ComputeDetailPropLighting(int iThread);

struct detour_t
{
	void* func;
	void* hook;
	sig_t sig;
};
std::map<std::string, detour_t> g_Detours =
{																	   
	{ "ThreadSetDefault",          { nullptr, ThreadSetDefault,         SIG(\x55\x8B\xEC\xA1\x00\x00\x00\x00\x83\xEC\x00\x83\xF8, xxxx????xx?xx) } },
	{ "RunThreads_Start",          { nullptr, RunThreads_Start,         SIG(\x55\x8B\xEC\x51\xA1\x00\x00\x00\x00\xC7\x05, xxxxx????xx) } },
	{ "RunThreads_End",            { nullptr, RunThreads_End,           SIG(\xA1\x00\x00\x00\x00\x56\x6A\x00\x6A, x????xx?x) } },
	{ "RunThreadsOnIndividual",    { nullptr, RunThreadsOnIndividual,   SIG(\x55\x8B\xEC\x83\xEC\x00\x83\x3D\x00\x00\x00\x00\x00\x75, xxxxx?xx?????x) } },
	{ "RunThreadsOn",              { nullptr, RunThreadsOn,             SIG(\x55\x8B\xEC\x56\x57\xFF\x15, xxxxxxx) } },
							       									    
#if defined(VVIS)			       									    
	// x87					       									    
	{ "StartPacifier",             { nullptr, StartPacifier,            SIG(\x55\x8B\xEC\x8B\x45\x00\x50\x68\x00\x00\x00\x00\xFF\x15\x00\x00\x00\x00\xD9\x05, xxxxx?xx????xx????xx) } },
	{ "EndPacifier",               { nullptr, EndPacifier,              SIG(\x55\x8B\xEC\xD9\xE8, xxxxx) } },
#elif defined(VRAD)			       									    
	{ "GetThreadWork",             { nullptr, GetThreadWork,            SIG(\x83\x3D\x00\x00\x00\x00\x00\x53\x8B\x1D, xx?????xxx) } },
	{ "ThreadLock",                { nullptr, ThreadLock,               SIG2(\xCC\x83\x3D\x90\xCD\x20\x15\x00\x74\x2C, xxx????xxx, 1, 0) } },
	{ "ThreadUnlock",              { nullptr, ThreadUnlock,             SIG2(\xCC\x83\x3D\x90\xCD\x20\x15\x00\x74\x2C, xxx????xxx, 1, 1) } },
							       									    
	// SSE					       									    
	{ "StartPacifier",             { nullptr, StartPacifier,            SIG(\x55\x8B\xEC\x8B\x45\x00\x50\x68\x00\x00\x00\x00\xFF\x15\x00\x00\x00\x00\xF3\x0F\x10\x05, xxxxx?xx????xx????xxxx) } },
	{ "UpdatePacifier",            { nullptr, UpdatePacifier,           SIG(\x55\x8B\xEC\xF3\x0F\x10\x45\x00\xF3\x0F\x59\x05\x00\x00\x00\x00\x56, xxxxxxx?xxxx????x) } },
	{ "EndPacifier",               { nullptr, EndPacifier,              SIG(\x55\x8B\xEC\xF3\x0F\x10\x05\x00\x00\x00\x00\x51, xxxxxxx????x) } },

	{ "ComputeDetailPropLighting", { nullptr, ComputeDetailPropLighting, SIG(\x55\x8B\xEC\x83\xEC\x00\x8D\x45, xxxxx?xx) } },
#endif
};

#ifdef VRAD
void ComputeDetailPropLighting(int iThread)
{
	typedef void (*ComputeDetailPropLighting)(int);
	((ComputeDetailPropLighting)g_Detours["ComputeDetailPropLighting"].func)(*g_pNumThreads);
}

bool PatchDispTests(const segment_t& segment)
{
	// lea edi, addr[edi*8]
	g_pDispTestedRefs[0] = (char*)FindSignature(segment, SIG(\x8D\x3C\xFD\x38\x5C\x1D\x11, xxx????));
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
		void* disptestedref = FindSignature(segment, sig);
		if (!disptestedref)
		{
			printf(APP_PREFIX "ERROR: Failed to locate DispTested array\n");
			return false;
		}
		g_pDispTestedRefs[i] = (char*)disptestedref;
	}
	return true;
}

bool PatchMakeScales(const segment_t& segment)
{
	// mov ecx, [esi+0F8h]
	// add total_transfers, ecx
	char* addr = (char*)FindSignature(segment, SIG(\x8B\x8E\xF8\x00\x00\x00\x01, xxxxxxx));
	if (!addr)
	{
		printf(APP_PREFIX "ERROR: Failed to locate total_transfers add\n");
		// this isn't crucial
		return true;
	}
	addr -= 5;

	int* total_transfers = *(int**)(addr + 13);
	char mov[5];
	// replace call ThreadLock with mov eax, total_transfers
	mov[0] = '\xB8';
	*(int**)(&mov[1]) = total_transfers;
	PatchMemory(addr, mov, sizeof(mov));
	// skip mov ecx, [esi+0F8h]
	addr += sizeof(mov) + 6;
	// lock xadd dword ptr [eax],ecx 
	PatchMemory(addr, "\xF0\x0F\xC1\x08", 4);
	addr += 4;
	// nop call ThreadUnlock
	PatchMemory(addr, "\x90\x90\x90\x90\x90\x90\x90", 7);

	return true;
}
#endif

void ApplyThreadPatches(HMODULE module)
{
	printf(APP_PREFIX "(%s) Applying patches...\n", __DATE__);

	segment_t text_segment;
	GetModuleSegment(module, ".text", text_segment);

	bool fail = false;
	for (auto& pair : g_Detours)
	{
		detour_t& detour = pair.second;
		detour.func = FindSignature(text_segment, detour.sig);
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

#ifdef VRAD
	if (!PatchDispTests(text_segment))
		return;
	if (!PatchMakeScales(text_segment))
		return;
#endif

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