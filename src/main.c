/*! \mainpage jabberd - Jabber Open Source Server
 *
 * \section intro Introduction
 *
 * The jabberd project aims to provide an open-source server
 * implementation of the Jabber protocols for instant messaging
 * and XML routing. The goal of this project is to provide a
 * scalable, reliable, efficient and extensible server that
 * provides a complete set of features and is up to date with
 * the latest protocol revisions.
 *
 * The project web page: http://jabberd2.org/
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <uv.h>
#include <gc.h>
#include <execinfo.h>
#include <signal.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <lib/util.h>
#include <lib/log.h>
#include <lib/xhash.h>
#include "conf.h"
extern void config_init(int prime);
#include "module.h"

log4c_category_t *log_main = NULL;
log4c_category_t *log_gc = NULL;

static const char *CONF_CORE_MODULES_PATH = "core.modules_path";
const char *modules_path = NULL; /* reachable via extern */

static xht *modules;



static void _save_pidfile(const char *pidfile)
{
    if (pidfile == NULL)
        return;

    FILE *f;
    if ((f = fopen(pidfile, "w+")) == NULL) {
        LOG_ERROR(log_main, "couldn't open %s for writing: %s", pidfile, strerror(errno));
        return;
    }

    pid_t pid = getpid();

    if (fprintf(f, "%d\n", pid) < 0) {
        LOG_ERROR(log_main, "couldn't write to %s: %s", pidfile, strerror(errno));
        fclose(f);
        return;
    }

    fclose(f);

    LOG_INFO(log_main, "process id is %d, written to %s", pid, pidfile);
}

static void _config_modules_path(const char *key, const xconfig_elem_t *elem, const char *value, void *data)
{
    CONFIG_VAL_STRING(key, value, CONF_CORE_MODULES_PATH, modules_path)}
}

static void _garbage_collect(uv_idle_t *handle)
{
    LOG_TRACE(log_gc, "idle garbage collection");
    if (!GC_collect_a_little()) uv_idle_stop(handle);
}

static void _garbage_collect_enable(uv_check_t *handle)
{
    LOG_TRACE(log_gc, "enable idle garbage collection");
    uv_idle_start((uv_idle_t*) handle->data, _garbage_collect);
}

static void _signal_handler(uv_signal_t* handle __attribute__ ((unused)), int signum)
{
    LOG_NOTICE(log_main, "Received signal %d - shutting down", signum);
    uv_stop(uv_default_loop());
}

static void _crash_sigaction(int signum, siginfo_t *info, void *ucontext)
{
    void *array[64];
    int size;
    char path[PATH_MAX];

    fprintf(stderr, "=== SIGNAL %d (%s), ADDR %p\n",
            signum, strsignal(signum), info->si_addr);

    fprintf(stderr, "=== CWD %s\n", getcwd(path, PATH_MAX));

    size = backtrace(array, NELEMS(array));

    /* skip first two stack frames (points here and sigaction) */
    backtrace_symbols_fd(array + 2, size - 3, STDERR_FILENO);

    fprintf(stderr, "=== Please report this crash to " PACKAGE_BUGREPORT "\n");

    /* restore default handler and return to call it */
    signal(signum, SIG_DFL);
}

int main(int argc, char * const _argv[])
{
    /* first things first - setup crash handler */
    struct sigaction sigact;

    sigact.sa_sigaction = _crash_sigaction;
    sigact.sa_flags = SA_RESTART | SA_SIGINFO;

    int sigs[] = {SIGILL, SIGABRT, SIGBUS, SIGFPE, SIGSEGV};
    for (int i = 0; i < NELEMS(sigs); i++) {
        if (sigaction(sigs[i], &sigact, (struct sigaction *)NULL) != 0) {
            fprintf(stderr, "error setting signal handler for %d (%s)\n",
                    sigs[i], strsignal(sigs[i]));
            exit(EXIT_FAILURE);
        }
    }

    /* before logging, as it may create files */
    umask((mode_t) 0027);

    if (log4c_init()) {
        fprintf(stderr, "log4c init failed\n");
        exit(EXIT_FAILURE);
    }

    srand(time(NULL));

    char **argv = uv_setup_args(argc, (char **) _argv);

    log_main = log4c_category_get(PACKAGE_NAME ".daemon");
    LOG_INFO(log_main, "Starting " PACKAGE_STRING " [%s:%d]", argv[0], getpid());

    /* Initialize Garbage Collector */
    GC_INIT();
    GC_enable_incremental();
    log_gc = log4c_category_get(PACKAGE_NAME ".gc");

    /* loaded modules hash */
    modules = xhash_new(59);

    /* initialize configuration subsystem */
    config_init(1021);

    /* subscribe to modules_path config */
    config_register(CONF_CORE_MODULES_PATH, NULL, _config_modules_path);

    /* parse command line */
    int opt;
    char *token;
    module_t *mod = NULL;
    while ((opt = getopt(argc, argv, "o:m:c:f:p:v?h?")) != -1) {
        switch (opt) {
        case 'o':
            token = strsep(&optarg, "=");
            if (token == NULL || optarg == NULL) {
                fprintf(stderr, "%s: invalid option '%c' argument -- '%s=%s'\n", argv[0], opt, token, optarg);
                exit(EXIT_FAILURE);
            }
            config_set(token, optarg);
            break;
        case 'c':
            token = strsep(&optarg, "=");
            if (token == NULL || optarg == NULL) {
                fprintf(stderr, "%s: invalid option '%c' argument -- '%s=%s'\n", argv[0], opt, token, optarg);
                exit(EXIT_FAILURE);
            }
            if (!config_load(token, optarg)) {
                fprintf(stderr, "%s: error loading config file: %s\n", argv[0], optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'f':
            // run a command-line script
            break;
        case 'p':
            _save_pidfile(optarg);
            break;
        case 'v':
            fprintf(stdout,  PACKAGE_STRING "\n"
                    "   Web: " PACKAGE_URL "\n"
                    "  Bugs: " PACKAGE_BUGREPORT "\n");
            exit(EXIT_SUCCESS);
        case 'h':
            fprintf(stdout, PACKAGE_NAME " usage:\n%s [-h] [-v]"
                    " [-o opt=val [...]]"
                    " [-c path=config.xml]"
                    " [-f id=/path/to/script]"
                    " [-p /path/to/pidfile]"
                    " [id=]module:confroot[:confroot[...]] [...]\n",
                    argv[0]);
            exit(EXIT_SUCCESS);
        default:
            fprintf(stderr, "%s: -h for help\n", argv[0]);
            exit(EXIT_FAILURE);

        }
    }

    while (optind < argc) {
        optarg = argv[optind];
        LOG_DEBUG(log_main, "Loading module %s", optarg);
        token = strsep(&optarg, "=");
        if (token == NULL) {
            fprintf(stderr, "%s: invalid module argument -- '%s'\n", argv[0], optarg);
            exit(EXIT_FAILURE);
        }
        if (optarg == NULL) {
            mod = module_load(modules, token);
        } else {
            mod = module_load(modules, optarg);
            if (mod) {
                mod_instance_t *mi = module_new(mod, token);
                if (!mi) {
                    fprintf(stderr, "%s: error instanitating '%s' module instance '%s'\n", argv[0], mi->mod->name, mi->id);
                    exit(EXIT_FAILURE);
                }
            }
        }
        if (!mod) {
            fprintf(stderr, "%s: error loading module '%s'\n", argv[0], token);
            exit(EXIT_FAILURE);
        }

        optind++;
    }

    if (!xhash_count(modules)) {
        fprintf(stderr, "%s: No modules loaded. I will run, but I will not be much of use. :-(\n", argv[0]);
    }

    /* Garbage Collect when nothing else to do */
    uv_idle_t gc_collector;
    uv_idle_init(uv_default_loop(), &gc_collector);
    uv_unref((uv_handle_t*) &gc_collector);

    uv_check_t gc_enabler;
    uv_check_init(uv_default_loop(), &gc_enabler);
    uv_unref((uv_handle_t*) &gc_enabler);
    gc_enabler.data = &gc_collector;
    uv_check_start(&gc_enabler, _garbage_collect_enable);

    /* hook signals */
    uv_signal_t sigint, sigtrm, sighup, sigpipe;
    uv_signal_init(uv_default_loop(), &sigint);
    uv_signal_start(&sigint, _signal_handler, SIGINT);
    uv_signal_init(uv_default_loop(), &sigtrm);
    uv_signal_start(&sigtrm, _signal_handler, SIGTERM);
#ifdef SIGHUP
    uv_signal_init(uv_default_loop(), &sighup);
    uv_signal_start(&sighup, (uv_signal_cb)log4c_reread, SIGHUP);
#endif
#ifdef SIGPIPE
    uv_signal_init(uv_default_loop(), &sigpipe);
    uv_signal_start(&sigint, NULL, SIGPIPE);
#endif

    /* spin the main loop */
    LOG_DEBUG(log_main, "Spinning main loop");
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

    LOG_INFO(log_main, "Exiting " PACKAGE_STRING " [%s:%d]", argv[0], getpid());

    /* shutdown logging system - should be last before exit! */
    if (log4c_fini()) {
        fprintf(stderr, "log4c finish failed\n");
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}