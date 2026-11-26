#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int native_add(int a,int b);
int native_flush_stdout(void);
void native_increment(int* value);
const char* native_greeting(void);
int native_bytes(const unsigned char* values,int size);
#ifdef __cplusplus
}
namespace demo {
struct Vector {float X,Y;Vector(float x,float y):X(x),Y(y){} float LengthSquared() const{return X*X+Y*Y;}};
inline double twice(double x){return x*2;}
}
#endif
