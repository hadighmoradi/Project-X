extern "C" __declspec(dllimport) int N33Add(int left, int right);
extern "C" __declspec(dllimport) void ExitProcess(unsigned int exit_code);

extern "C" void n33_start() {
    const int result = N33Add(19, 23);
    ExitProcess(static_cast<unsigned int>(result));
}
