#define _GNU_SOURCE
#include <unistd.h>
/* LD_PRELOAD interposer: getpid() returns a recognisable constant. */
pid_t getpid(void) { return 31337; }
