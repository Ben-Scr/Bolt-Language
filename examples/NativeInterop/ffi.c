#include "ffi.h"
#include <stdio.h>
int native_flush_stdout(void){return fflush(stdout);}
int native_add(int a,int b){return a+b;}
void native_increment(int* value){++*value;}
const char* native_greeting(void){return "Hello from C";}
int native_bytes(const unsigned char* values,int size){int sum=0;for(int i=0;i<size;++i)sum+=values[i];return sum;}
