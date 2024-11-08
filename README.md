# FixToolThreads
Modified launcher for VVIS and VRAD that patches several issues.

# Fixes
- Allows compilers to use unlimited amount of threads, instead of a maximum of 16
- Fixes thread count being set to 1 if the CPU has more than 32 threads
- Fixes two instances of operations locking all threads rather than using atomic operations (slightly improves performance)
- Enables Large Address Awareness (allows compilers to use 4GB instead of 2GB, prevents out of memory crashing)
- Prevents compiling -both (as L4D2 is HDR-only, so compiling LDR+HDR is a waste of time)

# Installation
Currently, this only supports Left 4 Dead 2 map compilers.

Download the `vvis.exe` and `vrad.exe` from [Releases](https://github.com/ficool2/fixtoolthreads/releases).

Replace the executables in `../steamapps/common/Left 4 Dead 2/bin/` with these.
	
Backup them first before replacing incase you need to revert the patch.