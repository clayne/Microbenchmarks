#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/timeb.h>
#include <Windows.h>

#define CACHELINE_SIZE 64

struct BandwidthTestThreadData {
    uint64_t read_bytes;
    uint64_t arr_length_bytes;
    char* arr;
    volatile int* flag;
    HANDLE threadHandle;
};

struct LatencyTestData {
    uint32_t iterations;
    uint32_t* arr;
    float latency;
    HANDLE threadHandle;
};

extern "C" uint64_t asm_read(char* arr, uint64_t arr_length, volatile int* flag, int waitfactor);
DWORD ReadBandwidthTestThread(void* param);
DWORD FillBandwidthTestArr(void* param);
void FillPatternArr(uint32_t* pattern_arr, uint32_t list_size, uint32_t byte_increment);
DWORD RunLatencyTest(void* param);
bool GetPrivilege();
float RunTest(uint64_t latencyAffinityMask, uint64_t bwAffinityMask, int bwThreadCount, int hugepages, float* measuredBw);

void StartMonitoring();
void EndMonitoring();
void SetupMonitoring();
void CloseMonitoring();

uint64_t BandwidthTestMemoryKB = 1048576 * 4;
uint64_t LatencyTestMemoryKB = 1048576;
uint64_t LatencyTestIterations = 1e5;
uint64_t throttle = 0;

int main(int argc, char* argv[]) {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    int bwThreadCap = sysInfo.dwNumberOfProcessors - 1;
    int coreCount = sysInfo.dwNumberOfProcessors;
    int latencyCore = 0;
    int* customCores = NULL;
    if (argc == 1) {
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "-bwthreads [int]: Number of bandwidth test threads\n");
        fprintf(stderr, "-latencyaffinity [int]: Core to run latency test thread on\n");
        fprintf(stderr, "-bwcores [comma separated list]: Cores to run bandwidth load on\n");
        fprintf(stderr, "-scaleiterations [int]: Iterations scaling factor\n");
        fprintf(stderr, "-throttle [int]: Reduce bandwidth load per bandwidth test thread\n");
    }
    for (int argIdx = 1; argIdx < argc; argIdx++) {
        if (*(argv[argIdx]) == '-') {
            char* arg = argv[argIdx] + 1;
            if (strncmp(arg, "bwthreads", 9) == 0) {
                argIdx++;
                bwThreadCap = atoi(argv[argIdx]);
                fprintf(stderr, "Using up to %d bw threads\n", bwThreadCap);
            }
            else if (strncmp(arg, "latencyaffinity", 15) == 0) {
                argIdx++;
                latencyCore = atoi(argv[argIdx]);
                fprintf(stderr, "Latency test thread will run in core %d\n", latencyCore);
            }
            else if (strncmp(arg, "scaleiterations", 15) == 0) {
                argIdx++;
                int scaleFactor = atoi(argv[argIdx]);
                LatencyTestIterations *= scaleFactor;
                fprintf(stderr, "Scaling iterations up by a factor of %d\n", scaleFactor);
            }
            else if (strncmp(arg, "throttle", 8) == 0) {
                argIdx++;
                throttle = atoi(argv[argIdx]);
                fprintf(stderr, "Pulling memory bandwidth test threads back, factor of %lld\n", throttle);
            }
            else if (strncmp(arg, "bwcores", 7) == 0) {
                argIdx++;
                char* customCoreListStr = argv[argIdx];
                bwThreadCap = 1;
                for (int i = 0; customCoreListStr[i] != 0; i++) {   // shell should null terminate this
                    if (customCoreListStr[i] == ',') {
                        bwThreadCap++;
                    }
                }

                customCores = (int*)malloc(sizeof(int) * bwThreadCap);
                memset(customCores, 0, sizeof(int) * bwThreadCap);
                int commaIdx = 1;
                for (int i = 0; customCoreListStr[i] != 0; i++) {
                    if (customCoreListStr[i] == ',') {
                        customCores[commaIdx] = i + 1;
                        commaIdx++;
                        customCoreListStr[i] = '\0';
                    }
                }

                fprintf(stderr, "Cores used for bandwidth load:");
                for (int i = 0; i < bwThreadCap; i++) {
                    customCores[i] = atoi(customCoreListStr + customCores[i]);
                    fprintf(stderr, " %d", customCores[i]);
                }

                fprintf(stderr, "\n");
            }
        }
    }

    GetPrivilege();
    //SetupMonitoring();

    uint64_t latencyAffinityMask = 1UL << latencyCore;
    uint64_t bwAffinityMask = 0;

    fprintf(stderr, "%d cores, will use up to %d for BW threads\n", coreCount, bwThreadCap);
    float* latencies = (float*)malloc(sizeof(float) * bwThreadCap + 1);
    float* bandwidths = (float*)malloc(sizeof(float) * bwThreadCap + 1);
    for (int bwThreadCount = 0; bwThreadCount <= bwThreadCap; bwThreadCount++) {
        float bw;
        int nextCore;
        if (bwThreadCount > 0) {
            if (customCores == NULL) nextCore = coreCount - bwThreadCount - 1;
            else nextCore = customCores[bwThreadCount - 1];
            fprintf(stderr, "next core is %d\n", nextCore);
            bwAffinityMask |= 1UL << nextCore;
        }

        float latencyNs = RunTest(latencyAffinityMask, bwAffinityMask, bwThreadCount, 1, &bw);
        fprintf(stderr, "%d bw threads %f GB/s %f ns\n", bwThreadCount, bw, latencyNs);
        latencies[bwThreadCount] = latencyNs;
        bandwidths[bwThreadCount] = bw;
    }

    printf("BW Threads, Bandwidth (GB/s), Latency (ns)\n");
    for (int bwThreadCount = 0; bwThreadCount <= bwThreadCap; bwThreadCount++) {
        printf("%d, %f, %f\n", bwThreadCount, bandwidths[bwThreadCount], latencies[bwThreadCount]);
    }

    free(latencies);
    free(bandwidths);
    if (customCores != NULL) free(customCores);
    //CloseMonitoring();
    return 0;
}

// returns latency in ns
// sets measuredBw = measured bandwidth
float RunTest(uint64_t latencyAffinity, uint64_t bwAffinity, int bwThreadCount, int hugepages, float* measuredBw) {
    uint64_t perThreadArrSizeBytes = ceil((double)BandwidthTestMemoryKB / (double)bwThreadCount) * 1024;
    volatile int flag = 0;  // set 1 to stop
    struct timeb start, end;
    int map_failed = 0;

    // MT bw test array fill
    struct BandwidthTestThreadData* bandwidthTestData = (struct BandwidthTestThreadData*)malloc(sizeof(struct BandwidthTestThreadData) * bwThreadCount);
    HANDLE* threadHandles = (HANDLE*)malloc(sizeof(HANDLE) * bwThreadCount);
    for (int threadIdx = 0; threadIdx < bwThreadCount; threadIdx++) {
        bandwidthTestData[threadIdx].read_bytes = 0;
        bandwidthTestData[threadIdx].flag = &flag;
        bandwidthTestData[threadIdx].arr = (char*)malloc(perThreadArrSizeBytes);
        bandwidthTestData[threadIdx].arr_length_bytes = perThreadArrSizeBytes;
        threadHandles[threadIdx] = CreateThread(NULL, 0, FillBandwidthTestArr, bandwidthTestData + threadIdx, 0, NULL);
    }

    // set up latency test
    uint32_t* latencyArr;
    latencyArr = (uint32_t *)VirtualAlloc(NULL, LatencyTestMemoryKB * 1024, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (latencyArr == NULL) {  // MAP_FAILED
        fprintf(stderr, "Failed to get memory via VirtualAlloc. Using plain malloc\n");
        latencyArr = (uint32_t *)malloc(LatencyTestMemoryKB * 1024);
        map_failed = 1;
    }

    struct LatencyTestData latencyTestData;
    latencyTestData.iterations = LatencyTestIterations;
    latencyTestData.latency = 0.0f;
    latencyTestData.arr = latencyArr;
    FillPatternArr(latencyArr, (LatencyTestMemoryKB * 256), CACHELINE_SIZE);

    WaitForMultipleObjects(bwThreadCount, threadHandles, true, INFINITE);
    for (int threadIdx = 0; threadIdx < bwThreadCount; threadIdx++) threadHandles[threadIdx] = INVALID_HANDLE_VALUE;

    // create bw test threads
    for (int threadIdx = 0; threadIdx < bwThreadCount; threadIdx++)
    {
        threadHandles[threadIdx] = CreateThread(NULL, 0, ReadBandwidthTestThread, bandwidthTestData + threadIdx, CREATE_SUSPENDED, NULL);
        SetThreadAffinityMask(threadHandles[threadIdx], bwAffinity);
    }

    //StartMonitoring();
    ftime(&start);
    // start bw test threads
    for (int threadIdx = 0; threadIdx < bwThreadCount; threadIdx++) {
        ResumeThread(threadHandles[threadIdx]);
    }

    HANDLE latencyThreadHandle = CreateThread(NULL, 0, RunLatencyTest, (void*)&latencyTestData, CREATE_SUSPENDED, NULL);
    SetThreadAffinityMask(latencyThreadHandle, latencyAffinity);
    ResumeThread(latencyThreadHandle);
    WaitForSingleObject(latencyThreadHandle, INFINITE);
    flag = 1;

    WaitForMultipleObjects(bwThreadCount, threadHandles, true, INFINITE);
    ftime(&end);
    //EndMonitoring();

    // count on a cacheline basis even though the test only loads 4B at a time
    uint64_t latencyReadBytes = 64 * LatencyTestIterations;

    uint64_t time_diff_ms = 1000 * (end.time - start.time) + (end.millitm - start.millitm);
    float totalReadData = (float)latencyReadBytes;
    float bwReadBytes = 0.0f;
    for (int threadIdx = 0; threadIdx < bwThreadCount; threadIdx++) {
        free(bandwidthTestData[threadIdx].arr);
        totalReadData += (float)bandwidthTestData[threadIdx].read_bytes;
        bwReadBytes += (float)bandwidthTestData[threadIdx].read_bytes;
    }

    *measuredBw = 1000 * (totalReadData / (float)1e9) / (float)time_diff_ms;
    float bwBandwidth = 1000 * (bwReadBytes / (float)1e9) / (float)time_diff_ms;
    float latencyBandwidth = 1000 * (latencyReadBytes / (float)1e9) / (float)time_diff_ms;

    fprintf(stderr, "%d bw threads - %f BW bandwidth, %f latency bandwidth\n", bwThreadCount, bwBandwidth, latencyBandwidth);

    free(bandwidthTestData);
    if (map_failed) free(latencyArr);
    else VirtualFree(latencyArr, 0, MEM_RELEASE);
    return latencyTestData.latency;
}

void FillPatternArr(uint32_t* pattern_arr, uint32_t list_size, uint32_t byte_increment) {
    uint32_t increment = byte_increment / sizeof(uint32_t);
    uint32_t element_count = list_size / increment;
    for (int i = 0; i < element_count; i++) {
        pattern_arr[i * increment] = i * increment;
    }

    int iter = element_count;
    while (iter > 1) {
        iter -= 1;
        int j = iter - 1 == 0 ? 0 : rand() % (iter - 1);
        uint32_t tmp = pattern_arr[iter * increment];
        pattern_arr[iter * increment] = pattern_arr[j * increment];
        pattern_arr[j * increment] = tmp;
    }
}

// No need for simple addressing because this test should be operating well in DRAM
// where an extra cycle for indexed addressing should not make a big difference
// returns load to use latency in nanoseconds
// size_kb should be divisible by 2M, or whatever the hugepage size is
DWORD RunLatencyTest(void* param) {
    struct timeb start, end;
    struct LatencyTestData* testData = (struct LatencyTestData*)param;
    uint32_t* A = testData->arr;
    uint32_t iterations = testData->iterations;
    uint32_t sum = 0, current;

    // Run test
    ftime(&start);
    current = A[0];
    for (int i = 0; i < iterations; i++) {
        current = A[current];
        sum += current;
    }
    ftime(&end);
    uint64_t time_diff_ms = 1000 * (end.time - start.time) + (end.millitm - start.millitm);
    testData->latency = 1e6 * (float)time_diff_ms / (float)iterations;

    return sum;
}

DWORD FillBandwidthTestArr(void* param) {
    struct BandwidthTestThreadData* bwTestData = (struct BandwidthTestThreadData*)param;
    float* arr = (float*)bwTestData->arr;
    uint64_t float_elements = bwTestData->arr_length_bytes / 4;
    for (int i = 0; i < float_elements; i++) {
        arr[i] = (i + ((uint64_t)arr & 0x3)) + 0.2f;
    }

    return 0;
}

DWORD ReadBandwidthTestThread(void* param) {
    struct BandwidthTestThreadData* bwTestData = (struct BandwidthTestThreadData*)param;
    uint64_t totalDataBytes = asm_read(bwTestData->arr, bwTestData->arr_length_bytes, bwTestData->flag, throttle);
    bwTestData->read_bytes = totalDataBytes;
    return 0;
}

// For winring0
#define RDMSR_FUNCTION 0x821
#define WRMSR_FUNCTION 0x822
#define WINRING0_DEVICE_TYPE 40000
HANDLE driverHandle = INVALID_HANDLE_VALUE;

void SetupMonitoring() {
    driverHandle = CreateFileA("\\\\.\\WinRing0_1_2_0", FILE_SHARE_READ | FILE_SHARE_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (driverHandle == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Could not open WinRing0 driver: %d\n", GetLastError());
    }
}

uint64_t ReadMsr(uint32_t index)
{
    uint32_t code = (WINRING0_DEVICE_TYPE << 16) | (RDMSR_FUNCTION << 2);
    uint64_t rc;
    DWORD bytesReturned;
    if (!DeviceIoControl(driverHandle, code, &index, sizeof(uint32_t), &rc, sizeof(uint64_t), &bytesReturned, NULL))
    {
        fprintf(stderr, "ReadMsr failed (ioctl returned false)\n");
    }

    return rc;
}

void WriteMsr(uint32_t index, uint64_t value)
{
    uint32_t code = (WINRING0_DEVICE_TYPE << 16) | (WRMSR_FUNCTION << 2);
    char inputBuffer[sizeof(uint32_t) + sizeof(uint64_t)];
    *(uint32_t*)inputBuffer = index;
    *(uint64_t*)(inputBuffer + sizeof(uint32_t)) = value;
    DWORD bytesReturned;
    if (!DeviceIoControl(driverHandle, code, &inputBuffer, sizeof(uint32_t) + sizeof(uint64_t), NULL, 0, &bytesReturned, NULL))
    {
        fprintf(stderr, "WriteMsr failed (ioctl returned false)\n");
    }
}

#define L3_PERF_CTL0 0xC0010230
#define L3_PERF_CTL1 0xC0010232
#define L3_PERF_CTL2 0xC0010234
#define L3_PERF_CTL3 0xC0010236
#define L3_PERF_CTR0 0xC0010231
#define L3_PERF_CTR1 0xC0010233
#define L3_PERF_CTR2 0xC0010235
#define L3_PERF_CTR3 0xC0010237

void ClearL3Counters() {
    WriteMsr(L3_PERF_CTR0, 0);
    WriteMsr(L3_PERF_CTR1, 0);
    WriteMsr(L3_PERF_CTR2, 0);
    WriteMsr(L3_PERF_CTR3, 0);
}

void StartMonitoring() {
    uint64_t l3access = 0x0300c0000040ff04;
    uint64_t l3miss = 0x0300c00000400104;
    uint64_t l3miss_sampled_dram_req = 0x0303c000004003ad;
    uint64_t l3miss_sampled_dram_req_latency = 0x0303c000004003ac;

    SetThreadAffinityMask(GetCurrentThread(), 1); // use core 0 in ccd 0
    WriteMsr(L3_PERF_CTL0, l3access);
    WriteMsr(L3_PERF_CTL1, l3miss);
    WriteMsr(L3_PERF_CTL2, l3miss_sampled_dram_req);
    WriteMsr(L3_PERF_CTL3, l3miss_sampled_dram_req_latency);
    ClearL3Counters();

    SetThreadAffinityMask(GetCurrentThread(), 16); // use core 0 in ccd 1
    WriteMsr(L3_PERF_CTL0, l3access);
    WriteMsr(L3_PERF_CTL1, l3miss);
    WriteMsr(L3_PERF_CTL2, l3miss_sampled_dram_req);
    WriteMsr(L3_PERF_CTL3, l3miss_sampled_dram_req_latency);
    ClearL3Counters();
}

void EndMonitoring() {
    SetThreadAffinityMask(GetCurrentThread(), 1); // use core 0 in ccd 0
    uint64_t ccd0L3Access = ReadMsr(L3_PERF_CTR0);
    uint64_t ccd0L3Miss = ReadMsr(L3_PERF_CTR1);
    uint64_t ccd0L3SampledDramReq = ReadMsr(L3_PERF_CTR2);
    uint64_t ccd0L3SampledDramReqLatency = ReadMsr(L3_PERF_CTR3);
    float ccd0SampledLatencyNs = 10.0f * ccd0L3SampledDramReqLatency / ccd0L3SampledDramReq;
    ClearL3Counters();

    SetThreadAffinityMask(GetCurrentThread(), 16); // use core 0 in ccd 1
    uint64_t ccd1L3Access = ReadMsr(L3_PERF_CTR0);
    uint64_t ccd1L3Miss = ReadMsr(L3_PERF_CTR1);
    uint64_t ccd1L3SampledDramReq = ReadMsr(L3_PERF_CTR2);
    uint64_t ccd1L3SampledDramReqLatency = ReadMsr(L3_PERF_CTR3);
    float ccd1SampledLatencyNs = 10.0f * ccd1L3SampledDramReqLatency / ccd1L3SampledDramReq;
    ClearL3Counters();

    fprintf(stderr, "CCD 0: %f ns, CCD1: %f ns\n", ccd0SampledLatencyNs, ccd1SampledLatencyNs);
}

void CloseMonitoring()
{
    if (driverHandle != INVALID_HANDLE_VALUE) CloseHandle(driverHandle);
    driverHandle = INVALID_HANDLE_VALUE;
}

bool GetPrivilege()
{
    HANDLE           hToken;
    TOKEN_PRIVILEGES tp;
    BOOL             status;
    DWORD            error;

    // open process token
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        fprintf(stderr, "OpenProcessToken failed: %d\n", GetLastError());
        return false;
    }

    // get the luid
    if (!LookupPrivilegeValue(NULL, TEXT("SeLockMemoryPrivilege"), &tp.Privileges[0].Luid))
    {
        fprintf(stderr, "Could not get luid: %d\n", GetLastError());
        return false;
    }

    // enable privilege
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    status = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, (PTOKEN_PRIVILEGES)NULL, 0);

    // It is possible for AdjustTokenPrivileges to return TRUE and still not succeed.
    // So always check for the last error value.
    error = GetLastError();
    if (!status || (error != ERROR_SUCCESS))
    {
        fprintf(stderr, "AdjustTokenPrivileges failed with status %d, error %d\n", status, error);
        return false;
    }

    // close the handle
    if (!CloseHandle(hToken))
    {
        fprintf(stderr, "CloseHandle failed: %d\n", GetLastError());
        return false;
    }

    fprintf(stderr, "Got SeLockMemoryPrivilege\n");
}
