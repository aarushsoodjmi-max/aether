#include "service.h"
#include <stdio.h>
#include <string.h>

static int visit_service(AetherService *services, int count, int index,
                         int console_index, int *state,
                         int *order, int *order_count)
{
    if (state[index] == 2)
        return 0;

    if (state[index] == 1) {
        fprintf(stderr, "Aether: dependency cycle at %s\n",
                services[index].name);
        return -1;
    }

    state[index] = 1;

    for (int d = 0; d < services[index].dependency_count; d++) {
        const char *dep = services[index].dependency_storage[d];
        int dep_index = -1;

        for (int j = 0; j < count; j++) {
            if (strcmp(services[j].name, dep) == 0) {
                dep_index = j;
                break;
            }
        }

        if (dep_index < 0) {
            fprintf(stderr, "Aether: %s requires missing service %s\n",
                    services[index].name, dep);
            return -1;
        }

        if (dep_index == console_index && index != console_index) {
            fprintf(stderr,
                    "Aether: service %s cannot require console\n",
                    services[index].name);
            return -1;
        }

        if (visit_service(services, count, dep_index, console_index,
                          state, order, order_count) < 0)
            return -1;
    }

    state[index] = 2;
    order[(*order_count)++] = index;
    return 0;
}

static int order_services(AetherService *services, int count,
                          int *console_index)
{
    if (!services || !console_index ||
        count < 0 || count > MAX_SERVICES ||
        (*console_index < -1 || *console_index >= count)) {
        fprintf(stderr, "Aether: invalid service ordering arguments\n");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(services[i].name, services[j].name) == 0) {
                fprintf(stderr, "Aether: duplicate service name '%s'\n",
                        services[i].name);
                return -1;
            }
        }
    }

    int state[MAX_SERVICES] = {0};
    int order[MAX_SERVICES];
    int order_count = 0;

    /* Start non-console services first; console remains last. */
    for (int i = 0; i < count; i++) {
        if (i == *console_index)
            continue;

        if (visit_service(services, count, i, *console_index,
                          state, order, &order_count) < 0)
            return -1;
    }

    if (*console_index >= 0 &&
        visit_service(services, count, *console_index, *console_index,
                      state, order, &order_count) < 0)
        return -1;

    AetherService sorted[MAX_SERVICES] = {0};

    for (int i = 0; i < count; i++) {
        sorted[i] = services[order[i]];
        sorted[i].name = sorted[i].name_storage;
        sorted[i].program = sorted[i].program_storage;

        if (strcmp(sorted[i].name, "console") == 0)
            *console_index = i;
    }

    memcpy(services, sorted, (size_t)count * sizeof(sorted[0]));
    return 0;
}


int service_order(AetherService *services, int count, int *console_index)
{ return order_services(services, count, console_index); }
