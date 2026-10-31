#include "t.h"
#include <unistd.h>
int main(void) { CHECK(getpid() > 0); DONE("static_glibc"); }
