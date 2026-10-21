#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

void* memcpy(void* destination, const void* source, size_t bytes) {
    volatile unsigned char* output = (volatile unsigned char*)destination;
    const volatile unsigned char* input =
        (const volatile unsigned char*)source;
    for (size_t index = 0; index < bytes; ++index) output[index] = input[index];
    return destination;
}

static int wide_equal(const wchar_t* left, const wchar_t* right) {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static int wide_contains(const wchar_t* text, const wchar_t* needle) {
    for (; *text; ++text) {
        const wchar_t* candidate = text;
        const wchar_t* expected = needle;
        while (*candidate && *expected && *candidate == *expected) {
            ++candidate;
            ++expected;
        }
        if (!*expected) return 1;
    }
    return 0;
}

static void make_child_command(wchar_t* command, const wchar_t* path) {
    unsigned long index = 0;
    command[index++] = L'"';
    for (unsigned long i = 0; path[i]; ++i) command[index++] = path[i];
    command[index++] = L'"';
    const wchar_t suffix[] = L" --child \"space value\" \"quote\\\"value\"";
    for (unsigned long i = 0; suffix[i]; ++i) command[index++] = suffix[i];
    command[index] = 0;
}

static void append_decimal(wchar_t* text, unsigned long* length,
                           unsigned long long value) {
    wchar_t reversed[24];
    unsigned long digits = 0;
    do {
        reversed[digits++] = (wchar_t)(L'0' + value % 10);
        value /= 10;
    } while (value);
    while (digits) text[(*length)++] = reversed[--digits];
}

static int child(void) {
    wchar_t environment[MAX_PATH];
    wchar_t directory[MAX_PATH];
    wchar_t path[MAX_PATH];
    wchar_t expected_command[MAX_PATH + 80];
    if (GetModuleFileNameW(0, path, MAX_PATH) == 0) return 40;
    make_child_command(expected_command, path);
    if (!wide_equal(GetCommandLineW(), expected_command))
        return 41;
    if (GetEnvironmentVariableW(L"TRANSMUTE_CHILD_VALUE", environment, 32) != 7 ||
        !wide_equal(environment, L"check-7"))
        return 42;
    if (GetCurrentDirectoryW(MAX_PATH, directory) == 0 ||
        GetEnvironmentVariableW(L"TRANSMUTE_CHILD_DIRECTORY", environment,
                                MAX_PATH) == 0 ||
        !wide_equal(directory, environment)) {
        return 43;
    }
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetEnvironmentVariableW(L"TRANSMUTE_CHILD_HANDLE", environment,
                                MAX_PATH) == 0)
        return 45;
    unsigned long long expected_handle = 0;
    for (unsigned long i = 0; environment[i]; ++i) {
        if (environment[i] < L'0' || environment[i] > L'9') return 46;
        expected_handle = expected_handle * 10 +
            (unsigned long long)(environment[i] - L'0');
    }
    if ((unsigned long long)(ULONG_PTR)output != expected_handle) return 47;
    const char message[] = "child-pipe-ok";
    DWORD written = 0;
    if (!output || output == INVALID_HANDLE_VALUE ||
        !WriteFile(output, message, sizeof(message) - 1, &written, 0) ||
        written != sizeof(message) - 1)
        return 44;
    return 37;
}

int guest_entry(void) {
    if (wide_contains(GetCommandLineW(), L" --child "))
        return child();

    wchar_t path[MAX_PATH];
    const DWORD path_length = GetModuleFileNameW(0, path, MAX_PATH);
    if (path_length == 0 || path_length >= MAX_PATH) return 1;

    SECURITY_ATTRIBUTES security = {sizeof(security), 0, TRUE};
    HANDLE reader = 0;
    HANDLE writer = 0;
    if (!CreatePipe(&reader, &writer, &security, 0)) return 2;
    if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) return 3;

    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(0, 1, 0, &attribute_bytes);
    if (attribute_bytes == 0 || attribute_bytes > 1024) return 4;
    __declspec(align(16)) unsigned char attributes[1024];
    LPPROC_THREAD_ATTRIBUTE_LIST list =
        (LPPROC_THREAD_ATTRIBUTE_LIST)attributes;
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_bytes))
        return 5;
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &writer, sizeof(writer), 0, 0))
        return 6;

    wchar_t command[MAX_PATH + 80];
    make_child_command(command, path);
    wchar_t current_directory[MAX_PATH];
    const DWORD directory_length = GetCurrentDirectoryW(
        MAX_PATH, current_directory);
    if (directory_length == 0 || directory_length >= MAX_PATH) return 13;
    wchar_t environment[MAX_PATH + 160];
    const wchar_t first[] = L"TRANSMUTE_CHILD_VALUE=check-7";
    const wchar_t second[] = L"TRANSMUTE_CHILD_DIRECTORY=";
    const wchar_t third[] = L"TRANSMUTE_CHILD_HANDLE=";
    unsigned long environment_index = 0;
    for (unsigned long i = 0; i < sizeof(first) / sizeof(first[0]); ++i)
        environment[environment_index++] = first[i];
    for (unsigned long i = 0; i < sizeof(second) / sizeof(second[0]) - 1; ++i)
        environment[environment_index++] = second[i];
    for (unsigned long i = 0; i < directory_length; ++i)
        environment[environment_index++] = current_directory[i];
    environment[environment_index++] = 0;
    for (unsigned long i = 0; i < sizeof(third) / sizeof(third[0]) - 1; ++i)
        environment[environment_index++] = third[i];
    append_decimal(environment, &environment_index,
                   (unsigned long long)(ULONG_PTR)writer);
    environment[environment_index++] = 0;
    environment[environment_index] = 0;
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writer;
    startup.StartupInfo.hStdError = writer;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process = {0};
    const BOOL launched = CreateProcessW(
        path, command, 0, 0, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
            CREATE_NO_WINDOW,
        (void*)environment, current_directory,
        &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(list);
    CloseHandle(writer);
    if (!launched) return 7;
    if (WaitForSingleObject(process.hProcess, 20000) != WAIT_OBJECT_0)
        return 8;
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) return 9;
    char output[32] = {0};
    DWORD received = 0;
    const BOOL read = ReadFile(reader, output, sizeof(output), &received, 0);
    CloseHandle(reader);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 37) return 10 + (int)exit_code;
    const char expected[] = "child-pipe-ok";
    if (!read || received != sizeof(expected) - 1) return 11;
    for (unsigned long i = 0; i < sizeof(expected) - 1; ++i)
        if (output[i] != expected[i]) return 12;
    return 0;
}
