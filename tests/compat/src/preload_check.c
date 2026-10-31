#include "t.h"
#include <unistd.h>
int main(void) { CHECK(getpid() == 31337); DONE("preload_check"); }
