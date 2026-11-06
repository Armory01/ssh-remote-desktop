// Test-only native SSH_ASKPASS adapter. No credential is compiled or stored.
#include <windows.h>
#include <cstdio>
int main() {
    char value[4096]{};DWORD n=GetEnvironmentVariableA("RA_TEST_SSH_PASSWORD",value,sizeof(value));
    if(!n || n>=sizeof(value))return 1;
    fwrite(value,1,n,stdout);fputc('\n',stdout);SecureZeroMemory(value,sizeof(value));return 0;
}
