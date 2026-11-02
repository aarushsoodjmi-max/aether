/*
 * aether-config: inspect and validate Aether Core configuration.
 *
 *   aether-config check [DIR]        report problems; exit 1 if there are any
 *   aether-config show  [DIR]        print the effective configuration
 *   aether-config get NAME [DIR]     print one value, e.g. system.hostname
 *   aether-config init  [DIR]        create missing files with the defaults;
 *                                    existing files are never touched
 *
 * DIR defaults to /etc/aether. This tool never modifies an existing file.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"

static int usage(FILE *out, int rc)
{
    fprintf(out,
            "Usage: aether-config check [DIR]\n"
            "       aether-config show [DIR]\n"
            "       aether-config get NAME [DIR]    (NAME like system.hostname)\n"
            "       aether-config init [DIR]        (creates missing files only)\n"
            "       aether-config help\n");
    return rc;
}

static const char *state_text(int state)
{
    switch (state) {
    case AETHER_FILE_LOADED:   return "loaded";
    case AETHER_FILE_MISSING:  return "missing, defaults in use";
    case AETHER_FILE_UNUSABLE: return "unusable, defaults in use";
    default:                   return "not loaded";
    }
}

static int cmd_check(const char *dir)
{
    AetherConfig cfg;
    AetherConfigReport rep;
    int issues = aether_config_load(dir, &cfg, &rep);

    for (int s = 0; s < AETHER_SECTION_COUNT; s++)
        printf("%s: %s\n", aether_config_section_file(s),
               state_text(rep.file_state[s]));
    aether_config_print_issues(stderr, &rep, "");
    if (issues > 0) {
        printf("check: %d problem(s) found in %s\n", issues, dir);
        return 1;
    }
    printf("check: configuration in %s is valid\n", dir);
    return 0;
}

static int cmd_show(const char *dir)
{
    AetherConfig cfg;
    AetherConfigReport rep;
    char buf[2048];

    aether_config_load(dir, &cfg, &rep);
    aether_config_print_issues(stderr, &rep, "aether-config: ");
    if (aether_config_format(&cfg, buf, sizeof(buf)) < 0) {
        fprintf(stderr, "aether-config: internal error: output too large\n");
        return 1;
    }
    fputs(buf, stdout);
    return 0;
}

static int cmd_get(const char *name, const char *dir)
{
    AetherConfig cfg;
    AetherConfigReport rep;
    char value[AETHER_LOGFILE_MAX + 1];

    aether_config_load(dir, &cfg, &rep);
    aether_config_print_issues(stderr, &rep, "aether-config: ");
    if (aether_config_get(&cfg, name, value, sizeof(value)) != 0) {
        fprintf(stderr, "aether-config: unknown setting '%s'\n", name);
        return 1;
    }
    puts(value);
    return 0;
}

static int cmd_init(const char *dir)
{
    int result[AETHER_SECTION_COUNT];
    int rc = aether_config_install_defaults(dir, result);

    for (int s = 0; s < AETHER_SECTION_COUNT; s++) {
        const char *file = aether_config_section_file(s);

        if (result[s] == AETHER_INSTALL_CREATED)
            printf("created %s/%s\n", dir, file);
        else if (result[s] == AETHER_INSTALL_KEPT)
            printf("kept existing %s/%s\n", dir, file);
        else
            fprintf(stderr, "aether-config: cannot create %s/%s\n", dir, file);
    }
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *cmd = argc >= 2 ? argv[1] : NULL;

    if (!cmd)
        return usage(stderr, 2);
    if (strcmp(cmd, "help") == 0)
        return usage(stdout, 0);
    if (strcmp(cmd, "check") == 0 && argc <= 3)
        return cmd_check(argc == 3 ? argv[2] : AETHER_CONFIG_DIR);
    if (strcmp(cmd, "show") == 0 && argc <= 3)
        return cmd_show(argc == 3 ? argv[2] : AETHER_CONFIG_DIR);
    if (strcmp(cmd, "get") == 0 && (argc == 3 || argc == 4))
        return cmd_get(argv[2], argc == 4 ? argv[3] : AETHER_CONFIG_DIR);
    if (strcmp(cmd, "init") == 0 && argc <= 3)
        return cmd_init(argc == 3 ? argv[2] : AETHER_CONFIG_DIR);
    return usage(stderr, 2);
}
