#include "../src/core/service.h"
#include <stdio.h>
#include <string.h>

int service_order(AetherService *, int, int *);

static void setup(AetherService *s, const char *name)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->name_storage, sizeof(s->name_storage), "%s", name);
    snprintf(s->program_storage, sizeof(s->program_storage), "/bin/%s", name);
    s->name = s->name_storage;
    s->program = s->program_storage;
}

int main(void)
{
    AetherService edge[16] = {0};
    int edge_console = -1;

    if (service_order(edge, 0, &edge_console) < 0) {
        fprintf(stderr, "FAIL: zero services\n");
        return 1;
    }
    puts("PASS: zero services");

    if (service_order(edge, 17, &edge_console) >= 0) {
        fprintf(stderr, "FAIL: oversized service count\n");
        return 1;
    }
    puts("PASS: oversized count rejected");

    setup(&edge[0], "app");
    edge_console = -1;
    if (service_order(edge, 1, &edge_console) < 0) {
        fprintf(stderr, "FAIL: missing console handling\n");
        return 1;
    }
    if (strcmp(edge[0].name, "app") != 0) {
        fprintf(stderr, "FAIL: service lost without console\n");
        return 1;
    }
    puts("PASS: missing console handled");

    AetherService s[16] = {0};
    int console = -1;

    /* Dependency must start before its dependent service. */
    setup(&s[0], "app");
    setup(&s[1], "base");
    setup(&s[2], "console");

    strcpy(s[0].dependency_storage[0], "base");
    s[0].dependency_count = 1;

    if (service_order(s, 3, &console) < 0 ||
        strcmp(s[0].name, "base") != 0 ||
        strcmp(s[1].name, "app") != 0 ||
        console != 2 ||
        strcmp(s[console].name, "console") != 0) {
        fprintf(stderr, "FAIL: dependency ordering\n");
        return 1;
    }
    puts("PASS: dependency ordering");

    /* Missing dependency must fail. */
    memset(s, 0, sizeof(s));
    setup(&s[0], "app");
    setup(&s[1], "console");
    strcpy(s[0].dependency_storage[0], "missing");
    s[0].dependency_count = 1;
    console = -1;

    if (service_order(s, 2, &console) >= 0) {
        fprintf(stderr, "FAIL: missing dependency accepted\n");
        return 1;
    }
    puts("PASS: missing dependency rejected");

    /* Dependency cycle must fail. */
    memset(s, 0, sizeof(s));
    setup(&s[0], "alpha");
    setup(&s[1], "beta");
    setup(&s[2], "console");

    strcpy(s[0].dependency_storage[0], "beta");
    s[0].dependency_count = 1;
    strcpy(s[1].dependency_storage[0], "alpha");
    s[1].dependency_count = 1;
    console = -1;

    if (service_order(s, 3, &console) >= 0) {
        fprintf(stderr, "FAIL: dependency cycle accepted\n");
        return 1;
    }
    puts("PASS: dependency cycle rejected");

    /* Duplicate service names must fail. */
    memset(s, 0, sizeof(s));
    setup(&s[0], "dup");
    setup(&s[1], "dup");
    console = -1;
    if (service_order(s, 2, &console) >= 0) {
        fprintf(stderr, "FAIL: duplicate names accepted\n");
        return 1;
    }
    puts("PASS: duplicate names rejected");

    /* Nothing may depend on the console. */
    memset(s, 0, sizeof(s));
    setup(&s[0], "app");
    setup(&s[1], "console");
    strcpy(s[0].dependency_storage[0], "console");
    s[0].dependency_count = 1;
    console = 1;
    if (service_order(s, 2, &console) >= 0) {
        fprintf(stderr, "FAIL: dependency on console accepted\n");
        return 1;
    }
    puts("PASS: dependency on console rejected");

    /* Transitive chain c -> b -> a, listed in reverse; console stays last. */
    memset(s, 0, sizeof(s));
    setup(&s[0], "console");
    setup(&s[1], "c");
    setup(&s[2], "b");
    setup(&s[3], "a");
    strcpy(s[1].dependency_storage[0], "b");
    s[1].dependency_count = 1;
    strcpy(s[2].dependency_storage[0], "a");
    s[2].dependency_count = 1;
    console = 0;
    if (service_order(s, 4, &console) < 0 ||
        strcmp(s[0].name, "a") != 0 || strcmp(s[1].name, "b") != 0 ||
        strcmp(s[2].name, "c") != 0 || console != 3 ||
        strcmp(s[3].name, "console") != 0) {
        fprintf(stderr, "FAIL: transitive ordering\n");
        return 1;
    }
    puts("PASS: transitive ordering, console last");

    /* Invalid console index must be rejected. */
    console = 7;
    if (service_order(s, 4, &console) >= 0) {
        fprintf(stderr, "FAIL: bad console index accepted\n");
        return 1;
    }
    puts("PASS: bad console index rejected");

    return 0;
}
