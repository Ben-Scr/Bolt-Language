#define PARAMETERS signed short a, unsigned char b, unsigned short c, int d, int e, int f, int g, int h, \
    unsigned char i, signed short j, int k, unsigned long long l, const unsigned char *p, \
    float m, double n, float o, double q, float r, double s, float t, double u, float v, float w, double z

extern int bolt_probe(void);
extern int bolt_spilled(PARAMETERS);

int probe_arguments(PARAMETERS) {
    if (a != -9 || b != 251 || c != 65522 || d != 17 || e != 18 || f != 19 || g != 20 || h != 21) return 1;
    if (i != 0x21 || j != -124 || k != 0x354769aa || l != 0x11223344556677ull) return 2;
    if (!p || p[0] != 61 || p[1] != 37) return 3;
    if (m != 13.25 || n != 14.25 || o != 15.25 || q != 16.25 || r != 17.25 || s != 18.25 || t != 19.25 || u != 20.25) return 4;
    if (v != 21.25 || w != 22.25 || z != 23.25) return 5;
    return 0;
}

int main(void) {
    int result = bolt_probe();
    if (result) return result;
    return bolt_spilled(11,12,13,14,15,16,17,18,0xff,-127,512,0,0,0,0,0,0,0,0,0,0,0,0,0) == -127 ? 0 : 6;
}
