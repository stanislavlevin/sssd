/*
    Authors:
        Pavel Březina <pbrezina@redhat.com>

    Copyright (C) 2015 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <talloc.h>
#include <stdlib.h>
#include <string.h>
#include <popt.h>

#include "config.h"
#include "util/util.h"
#include "confdb/confdb.h"
#include "db/sysdb.h"
#include "tools/common/sss_tools.h"

struct sss_cmdline {
    const char *exec; /* argv[0] */
    const char *command; /* command name */
    int argc; /* rest of arguments */
    const char **argv;
};

static void sss_tool_print_common_opts(void)
{
    fprintf(stderr, _("Common options:\n"));
    fprintf(stderr, "  --debug=INT            %s\n",
                    _("Enable debug at level"));
}

static struct poptOption *sss_tool_common_opts_table(void)
{
    static struct poptOption common_opts[] = {
        {"debug", '\0', POPT_ARG_INT, NULL,
            0, NULL, NULL },
        POPT_TABLEEND
    };

    common_opts[0].descrip = _("The debug level to run with");

    return common_opts;
}

static void sss_tool_common_opts(struct sss_tool_ctx *tool_ctx,
                                 int *argc, const char **argv)
{
    poptContext pc;
    int debug = SSSDBG_DEFAULT;
    int orig_argc = *argc;
    int opt;

    struct poptOption options[] = {
        {"debug", '\0', POPT_ARG_INT | POPT_ARGFLAG_STRIP, &debug,
            0, _("The debug level to run with"), NULL },
        POPT_TABLEEND
    };

    pc = poptGetContext(argv[0], orig_argc, argv, options, 0);
    while ((opt = poptGetNextOpt(pc)) != -1) {
        /* do nothing */
    }

    /* Strip common options from arguments. We will discard_const here,
     * since it is not worth the trouble to convert it back and forth. */
    *argc = poptStrippedArgv(pc, orig_argc, discard_const_p(char *, argv));

    DEBUG_CLI_INIT(debug);

    poptFreeContext(pc);
}

static errno_t sss_tool_confdb_init(TALLOC_CTX *mem_ctx,
                                    struct confdb_ctx **_confdb)
{
    struct confdb_ctx *confdb;
    char *path;
    errno_t ret;

    path = talloc_asprintf(mem_ctx, "%s/%s", DB_PATH, CONFDB_FILE);
    if (path == NULL) {
        return ENOMEM;
    }

    ret = confdb_init(mem_ctx, &confdb, path);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              "Could not initialize connection to the confdb\n");
        talloc_free(path);
        return ret;
    }

    if (_confdb != NULL) {
        *_confdb = confdb;
    }

    return EOK;
}

static errno_t sss_tool_domains_init(TALLOC_CTX *mem_ctx,
                                     struct confdb_ctx *confdb,
                                     struct sss_domain_info **_domains)
{
    struct sss_domain_info *domains;
    struct sss_domain_info *dom;
    errno_t ret;

    ret = confdb_get_domains(confdb, &domains);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to setup domains [%d]: %s\n",
                                   ret, sss_strerror(ret));
        return ret;
    }

    ret = sysdb_init(mem_ctx, domains, false);
    SYSDB_VERSION_ERROR(ret);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              "Could not initialize connection to the sysdb\n");
        return ret;
    }

    for (dom = domains; dom != NULL;
            dom = get_next_domain(dom, SSS_GND_DESCEND)) {
        if (!IS_SUBDOMAIN(dom)) {
            /* Update list of subdomains for this domain */
            ret = sysdb_update_subdomains(dom);
            if (ret != EOK) {
                DEBUG(SSSDBG_MINOR_FAILURE,
                      "Failed to update subdomains for domain %s.\n",
                      dom->name);
            }
        }
    }

    for (dom = domains; dom != NULL;
            dom = get_next_domain(dom, SSS_GND_DESCEND)) {
        ret = sss_names_init(mem_ctx, confdb, dom->name, &dom->names);
        if (ret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE, "sss_names_init() failed\n");
            return ret;
        }
    }

    *_domains = domains;

    return ret;
}

struct sss_tool_ctx *sss_tool_init(TALLOC_CTX *mem_ctx,
                                   int *argc, const char **argv)
{
    struct sss_tool_ctx *tool_ctx;
    errno_t ret;

    tool_ctx = talloc_zero(mem_ctx, struct sss_tool_ctx);
    if (tool_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_zero() failed\n");
        return NULL;
    }

    sss_tool_common_opts(tool_ctx, argc, argv);

    /* Connect to confdb. */
    ret = sss_tool_confdb_init(tool_ctx, &tool_ctx->confdb);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to open confdb [%d]: %s\n",
                                   ret, sss_strerror(ret));
        goto done;
    }

    /* Setup domains. */
    ret = sss_tool_domains_init(tool_ctx, tool_ctx->confdb, &tool_ctx->domains);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to setup domains [%d]: %s\n",
                                   ret, sss_strerror(ret));
        goto done;
    }

    ret = confdb_get_string(tool_ctx->confdb, tool_ctx,
                            CONFDB_MONITOR_CONF_ENTRY,
                            CONFDB_MONITOR_DEFAULT_DOMAIN,
                            NULL, &tool_ctx->default_domain);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "Cannot get the default domain [%d]: %s\n",
                                 ret, strerror(ret));
        goto done;
    }

    ret = EOK;

done:
    if (ret != EOK) {
        talloc_zfree(tool_ctx);
    }

    return tool_ctx;
}

int sss_tool_usage(const char *tool_name,
                   struct sss_route_cmd *commands)
{
    int i;

    fprintf(stderr, _("Usage:\n%s COMMAND COMMAND-ARGS\n\n"), tool_name);
    fprintf(stderr, _("Available commands:\n"));

    for (i = 0; commands[i].command != NULL; i++) {
        fprintf(stderr, "* %s\n", commands[i].command);
    }

    fprintf(stderr, _("\n"));
    sss_tool_print_common_opts();

    return EXIT_FAILURE;
}

int sss_tool_route(int argc, const char **argv,
                   struct sss_tool_ctx *tool_ctx,
                   struct sss_route_cmd *commands,
                   void *pvt)
{
    struct sss_cmdline cmdline;
    const char *cmd;
    int i;

    if (commands == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Bug: commands can't be NULL!\n");
        return EXIT_FAILURE;
    }

    if (argc < 2) {
        return sss_tool_usage(argv[0], commands);
    }

    cmd = argv[1];
    for (i = 0; commands[i].command != NULL; i++) {
        if (strcmp(commands[i].command, cmd) == 0) {
            cmdline.exec = argv[0];
            cmdline.command = argv[1];
            cmdline.argc = argc - 2;
            cmdline.argv = argv + 2;

            return commands[i].fn(&cmdline, tool_ctx, pvt);
        }
    }

    return sss_tool_usage(argv[0], commands);
}

static struct poptOption *nonnull_popt_table(struct poptOption *options)
{
    static struct poptOption empty[] = {
        POPT_TABLEEND
    };

    if (options == NULL) {
        return empty;
    }

    return options;
}

int sss_tool_popt_ex(struct sss_cmdline *cmdline,
                     struct poptOption *options,
                     enum sss_tool_opt require_option,
                     sss_popt_fn popt_fn,
                     void *popt_fn_pvt,
                     const char *fopt_name,
                     const char *fopt_help,
                     const char **_fopt)
{
    struct poptOption opts_table[] = {
        {NULL, '\0', POPT_ARG_INCLUDE_TABLE, nonnull_popt_table(options), \
         0, _("Command options:"), NULL },
        {NULL, '\0', POPT_ARG_INCLUDE_TABLE, sss_tool_common_opts_table(), \
         0, _("Common options:"), NULL },
        POPT_AUTOHELP
        POPT_TABLEEND
    };
    char *help;
    poptContext pc;
    int ret;

    /* Create help option string. We always need to append command name since
     * we use POPT_CONTEXT_KEEP_FIRST. */
    if (fopt_name == NULL) {
        help = talloc_asprintf(NULL, "%s %s %s", cmdline->exec,
                               cmdline->command, _("[OPTIONS...]"));
    } else {
        help = talloc_asprintf(NULL, "%s %s %s %s", cmdline->exec,
                               cmdline->command, fopt_name, _("[OPTIONS...]"));
    }
    if (help == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "talloc_asprintf() failed\n");
        return EXIT_FAILURE;
    }

    /* Create popt context. This function is supposed to be called on
     * command argv which does not contain executable (argv[0]), therefore
     * we need to use KEEP_FIRST that ensures argv[0] is also processed. */
    pc = poptGetContext(cmdline->exec, cmdline->argc, cmdline->argv,
                        opts_table, POPT_CONTEXT_KEEP_FIRST);

    poptSetOtherOptionHelp(pc, help);

    /* Parse options. Invoke custom function if provided. If no parsing
     * function is provided, print error on unknown option. */
    while ((ret = poptGetNextOpt(pc)) != -1) {
        if (popt_fn != NULL) {
            ret = popt_fn(pc, ret, popt_fn_pvt);
            if (ret != EOK) {
                ret = EXIT_FAILURE;
                goto done;
            }
        } else {
            fprintf(stderr, _("Invalid option %s: %s\n\n"),
                    poptBadOption(pc, 0), poptStrerror(ret));
            poptPrintHelp(pc, stderr, 0);
            ret = EXIT_FAILURE;
            goto done;
        }
    }

    /* Parse free option which is always required if requested. */
    if (_fopt != NULL) {
        *_fopt = poptGetArg(pc);
        if (*_fopt == NULL) {
            fprintf(stderr, _("Missing option: %s\n\n"), fopt_help);
            poptPrintHelp(pc, stderr, 0);
            ret = EXIT_FAILURE;
            goto done;
        }

        /* No more arguments expected. If something follows it is an error. */
        if (poptGetArg(pc)) {
            fprintf(stderr, _("Only one free argument is expected!\n\n"));
            poptPrintHelp(pc, stderr, 0);
            ret = EXIT_FAILURE;
            goto done;
        }
    }

    /* If at least one option is required and not provided, print error. */
    if (require_option == SSS_TOOL_OPT_REQUIRED
            && ((_fopt != NULL && cmdline->argc < 2) || cmdline->argc < 1)) {
        fprintf(stderr, _("At least one option is required!\n\n"));
        poptPrintHelp(pc, stderr, 0);
        ret = EXIT_FAILURE;
        goto done;
    }

    ret = EXIT_SUCCESS;

done:
    poptFreeContext(pc);
    talloc_free(help);
    return ret;
}

int sss_tool_popt(struct sss_cmdline *cmdline,
                  struct poptOption *options,
                  enum sss_tool_opt require_option,
                  sss_popt_fn popt_fn,
                  void *popt_fn_pvt)
{
    return sss_tool_popt_ex(cmdline, options, require_option,
                            popt_fn, popt_fn_pvt, NULL, NULL, NULL);
}

int sss_tool_main(int argc, const char **argv,
                  struct sss_route_cmd *commands,
                  void *pvt)
{
    struct sss_tool_ctx *tool_ctx;
    uid_t uid;
    int ret;

    uid = getuid();
    if (uid != 0) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Running under %d, must be root\n", uid);
        ERROR("%1$s must be run as root\n", argv[0]);
        return EXIT_FAILURE;
    }

    tool_ctx = sss_tool_init(NULL, &argc, argv);
    if (tool_ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to create tool context\n");
        return EXIT_FAILURE;
    }

    ret = sss_tool_route(argc, argv, tool_ctx, commands, pvt);
    talloc_free(tool_ctx);

    return ret;
}

int sss_tool_parse_name(TALLOC_CTX *mem_ctx,
                        struct sss_tool_ctx *tool_ctx,
                        const char *input,
                        const char **_username,
                        struct sss_domain_info **_domain)
{
    char *username = NULL;
    char *domname = NULL;
    struct sss_domain_info *domain;
    int ret;

    ret = sss_parse_name_for_domains(mem_ctx, tool_ctx->domains,
                                     tool_ctx->default_domain, input,
                                     &domname, &username);
    if (ret == EAGAIN) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to find domain. The domain name may "
              "be a subdomain that was not yet found.\n");
        goto done;
    } else if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to parse name [%d]: %s\n",
              ret, sss_strerror(ret));
        goto done;
    }

    domain = find_domain_by_name(tool_ctx->domains, domname, true);

    *_username = username;
    *_domain = domain;

    ret = EOK;

done:
    if (ret != EOK) {
        talloc_zfree(username);
        talloc_zfree(domname);
    }

    return ret;
}
