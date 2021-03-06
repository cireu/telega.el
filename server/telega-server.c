#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <td/telegram/td_json_client.h>
#include <td/telegram/td_log.h>

#include "telega-dat.h"
#ifdef WITH_VOIP
extern const char* telega_voip_version(void);
extern int telega_voip_cmd(const char* json);
#endif /* WITH_VOIP */

/*
 * Input/Output Protocol:
 * ~~~~~~~~~~~~~~
 *   <COMMAND> <SPACE> <PLIST-LEN> <NEWLINE>
 *   <PLIST of PLIST-LEN length> <NEWLINE>
 *
 * COMMAND is one of `send', `event' or `error'
 * `event' and `error' is used for output
 *
 * If VOIP support is compiled in (make WITH_VOIP=true) then also
 * `voip' command available
 *
 * For example:
 *   event 105
 *   (:@type "updateAuthorizationState" :authorization_state (:@type "authorizationStateWaitTdlibParameters"))
 *
 *   send 109
 *   (:@type "getTextEntities" :text "@telegram /test_command https://telegram.org telegram.me" :@extra ["5" 7.0])
 *
 */

char* logfile = NULL;
int verbosity = 5;
const char* version = "0.4.0";

/* true when tdlib_loop is running */
volatile bool tdlib_running;

int parse_mode = 0;
#define PARSE_MODE_JSON 1
#define PARSE_MODE_PLIST 2

void
usage(char* prog)
{
        printf("Version %s", version);
#ifdef WITH_VOIP
        printf(", with VOIP tgvoip v%s", telega_voip_version());
#endif /* WITH_VOIP */
        printf("\n");
        printf("usage: %s [-jp] [-l FILE] [-v LVL] [-h]\n", prog);
        printf("\t-l FILE    Log to FILE (default=stderr)\n");
        printf("\t-v LVL     Verbosity level (default=5)\n");
        printf("\t-j         Parse json from stdin and exit\n");
        printf("\t-p         Parse plist from stdin and exit\n");
        exit(0);
}

void
telega_output(const char* otype, const char* str)
{
        if (verbosity > 4) {
                fprintf(stderr, "[telega-server] "
                        "OUTPUT %s: %s\n", otype, str);
        }

        printf("%s %zu\n%s\n", otype, strlen(str), str);
        fflush(stdout);
}

void
telega_output_json(const char* otype, const char* json)
{
        struct telega_dat json_src = TDAT_INIT;
        struct telega_dat plist_dst = TDAT_INIT;

        if (verbosity > 4) {
                fprintf(stderr, "[telega-server] "
                        "OUTPUT %s: %s\n", otype, json);
        }

        tdat_append(&json_src, json, strlen(json));
        tdat_json_value(&json_src, &plist_dst);
        tdat_append1(&plist_dst, "\0");

        assert(tdat_len(&plist_dst) > 0);
        printf("%s %zu\n%s\n", otype, tdat_len(&plist_dst)-1, plist_dst.data);
        fflush(stdout);

        tdat_drop(&json_src);
        tdat_drop(&plist_dst);
}

static void
on_error_cb(const char* errmsg)
{
        telega_output_json("error", errmsg);
}

static void*
tdlib_loop(void* cln)
{
        while (tdlib_running) {
                const char *res = td_json_client_receive(cln, 0.5);
                if (res)
                        telega_output_json("event", res);
        }
        return NULL;
}

/*
 * NOTE: Emacs sends HUP when associated buffer is killed
 * kind of graceful exit
 */
static void
on_sighup(int sig)
{
        close(0);
}

static void
stdin_loop(void* cln)
{
        struct telega_dat plist_src = TDAT_INIT;
        struct telega_dat json_dst = TDAT_INIT;
        char cmdline[33];

        signal(SIGHUP, on_sighup);
        while (fgets(cmdline, 33, stdin)) {
                cmdline[32] = '\0';

                char cmd[33];
                size_t cmdsz;
                if (2 != sscanf(cmdline, "%s %zu\n", cmd, &cmdsz)) {
                        fprintf(stderr, "[telega-server] "
                                "Unexpected cmdline format: %s\n", cmdline);
                        continue;
                }

                tdat_ensure(&plist_src, cmdsz);

                /* read including newline */
                size_t rc = fread(plist_src.data, 1, cmdsz + 1, stdin);
                if (rc != cmdsz + 1) {
                        /* EOF or error */
                        fprintf(stderr, "[telega-server] "
                                "fread() error: %d\n", ferror(stdin));
                        break;
                }
                plist_src.end = cmdsz + 1;

                tdat_plist_value(&plist_src, &json_dst);
                tdat_append1(&json_dst, "\0");
                if (verbosity > 4)
                        fprintf(stderr, "[telega-server] "
                                "INPUT: %s\n", json_dst.data);

                if (!strcmp(cmd, "send"))
                        td_json_client_send(cln, json_dst.data);
#ifdef WITH_VOIP
                else if (!strcmp(cmd, "voip"))
                        telega_voip_cmd(json_dst.data);
#endif /* WITH_VOIP */
                else {
                        char error[128];
                        snprintf(error, 128, "\"Unknown cmd `%s'\"", cmd);
                        telega_output("error", error);

                        fprintf(stderr, "[telega-server] "
                                "Unknown command: %s\n", cmd);
                }

                tdat_reset(&plist_src);
                tdat_reset(&json_dst);
        }

        tdat_drop(&plist_src);
        tdat_drop(&json_dst);
}

static void
parse_stdin(void)
{
        struct telega_dat src = TDAT_INIT;

#define RDSIZE 1024
        tdat_ensure(&src, RDSIZE);

        ssize_t rlen;
        while ((rlen = read(0, &src.data[src.end], RDSIZE)) > 0) {
                src.end += rlen;
                tdat_ensure(&src, RDSIZE);
        }
#undef RDSIZE
        tdat_append1(&src, "\0");

        struct telega_dat dst = TDAT_INIT;
        if (parse_mode == PARSE_MODE_JSON)
                tdat_json_value(&src, &dst);
        else
                tdat_plist_value(&src, &dst);
        tdat_append1(&dst, "\0");

        printf("%s\n", dst.data);

        tdat_drop(&src);
        tdat_drop(&dst);
}

void
telega_set_verbosity(int verbosity)
{
        char req[1024];
        snprintf(req, 1024, "{\"@type\":\"setLogVerbosityLevel\","
                 "\"new_verbosity_level\":%d}", verbosity);
        td_json_client_execute(NULL, req);
}

void
telega_set_logfile(char* logfile)
{
        char req[1024];
        snprintf(req, 1024, "{\"@type\":\"setLogStream\","
                 "\"log_stream\":{\"@type\":\"logStreamFile\","
                 "\"path\":\"%s\", \"max_file_size\":2097152}}", logfile);
        td_json_client_execute(NULL, req);
}

int
main(int ac, char** av)
{
        int ch;
        while ((ch = getopt(ac, av, "jpl:v:h")) != -1) {
                switch (ch) {
                case 'v':
                        verbosity = atoi(optarg);
                        telega_set_verbosity(verbosity);
                        break;
                case 'l':
                        logfile = optarg;
                        telega_set_logfile(logfile);
                        break;
                case 'j':
                        parse_mode = PARSE_MODE_JSON;
                        break;
                case 'p':
                        parse_mode = PARSE_MODE_PLIST;
                        break;
                case 'h':
                case '?':
                default:
                        usage(av[0]);
                }
        }

        if (parse_mode) {
                parse_stdin();
                return 0;
                /* NOT REACHED */
        }

        td_set_log_fatal_error_callback(on_error_cb);

        void *client = td_json_client_create();

        pthread_t td_thread;
        tdlib_running = true;
        int rc = pthread_create(&td_thread, NULL, tdlib_loop, client);
        assert(rc == 0);

        stdin_loop(client);

        /* Gracefully stop the tdlib_loop */
        tdlib_running = false;
        rc = pthread_join(td_thread, NULL);
        assert(rc == 0);

        td_json_client_destroy(client);

        return 0;
}
