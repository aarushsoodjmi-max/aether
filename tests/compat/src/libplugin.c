#include <stdio.h>
/* Plugin loaded with dlopen(); its constructor proves the loader ran init code. */
int plugin_ctor_ran;
__attribute__((constructor)) static void ctor(void) { plugin_ctor_ran = 1; }
int plugin_add(int a, int b) { return a + b; }
