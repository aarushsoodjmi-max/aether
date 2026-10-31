#define _GNU_SOURCE
#include "t.h"
#include <unistd.h>
extern char **environ;
int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {          /* re-exec'd copy */
        CHECK(argc == 4); CHECK(strcmp(argv[2], "two words") == 0);
        CHECK(argv[3][0] == '\0');                              /* empty arg preserved */
        CHECK(getenv("AETHER_CHILD") && strcmp(getenv("AETHER_CHILD"), "yes") == 0);
        CHECK(getenv("AETHER_PARENT") == NULL);                 /* parent-only var not inherited */
        DONE("args_env");
    }
    CHECK(getenv("AETHER_T") && strcmp(getenv("AETHER_T"), "val=with=equals") == 0);
    CHECK(setenv("AETHER_PARENT", "1", 1) == 0);
    CHECK(strcmp(getenv("AETHER_PARENT"), "1") == 0);
    CHECK(unsetenv("AETHER_T") == 0 && getenv("AETHER_T") == NULL);
    char self[4096]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1); CHECK(n > 0); self[n] = 0;
    char *nargv[] = { argv[0], "child", "two words", "", NULL };
    char *nenv[] = { "AETHER_CHILD=yes", NULL };
    execve(self, nargv, nenv);
    CHECK(!"execve returned");
}
