// Static AOT executables enter through the CRT to initialize runtime globals.
// The negative argc requests Bolt's Unicode command-line processing.
extern "C" int bolt_program_entry(int argc, const char* const* argv);
// Windows supplies a UTF-16 environment. The wide CRT startup can use it
// directly; the CRT still provides a narrow environment on first request.
int wmain() { return bolt_program_entry(-1, nullptr); }
// Older SDK linker drivers request mainCRTStartup from this archive.
int main() { return bolt_program_entry(-1, nullptr); }
