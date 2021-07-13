// Copyright Contributors to the L3AF Project.
// SPDX-License-Identifier: GPL-2.0

/* Ratelimit incoming TCP connections with sliding window approach */

#include <stdio.h>
#include <linux/bpf.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <getopt.h>
#include <net/if.h>
#include <time.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>

#include "bpf_load.h"
#include "bpf_util.h"
#include "bpf/libbpf.h"

#include "constants.h"
#include "log.h"

static const char *__doc__ =
        "Ratelimit incoming TCP connections using XDP";

static int ifindex;

FILE *info;
static char prev_prog_map[1024];
static const struct option long_options[] = {
    {"help",      no_argument,        NULL, 'h' },
    {"iface",     required_argument,  NULL, 'i' },
    {"rate",      required_argument,  NULL, 'r' },
    {"ports",     optional_argument,  NULL, 'p' },
    {"verbose",   optional_argument,  NULL, 'v' },
    {"direction", optional_argument,  NULL, 'd'},
    {"map-name",  optional_argument,  NULL, 'm' },
    {0,           0,                  NULL,  0  }
};

static void usage(char *argv[])
{
    int i;
    printf("\nDOCUMENTATION:\n%s\n", __doc__);
    printf("\n");
    printf(" Usage: %s (options-see-below)\n", argv[0]);
    printf(" Listing options:\n");
    for (i = 0; long_options[i].name != 0; i++)
    {
        printf(" --%-12s", long_options[i].name);
        if (long_options[i].flag != NULL)
                printf(" flag (internal value:%d)",
                        *long_options[i].flag);
        else
                printf(" short-option: -%c",
                        long_options[i].val);
        printf("\n");
    }
    printf("\n");
}

/* Set log timestamps */
void log_timestamp(char *log_ts) {
    struct timeval tv;
    time_t nowtime;
    struct tm *nowtm;
    char tmbuf[TIMESTAMP_LEN];

    gettimeofday(&tv, NULL);
    nowtime = tv.tv_sec;
    nowtm = localtime(&nowtime);
    strftime(tmbuf, TIMESTAMP_LEN, "%Y-%m-%d %H:%M:%S", nowtm);
    #ifdef DARWIN
    snprintf(log_ts, TIMESTAMP_LEN, "%s.%06d", tmbuf, tv.tv_usec);
    #else
    snprintf(log_ts, TIMESTAMP_LEN, "%s.%06ld", tmbuf, tv.tv_usec);
    #endif
}

int get_length(const char *str)
{
    int len = 0;
    if (*str == '\0')
        return 0;
    while (str[len] != '\0')
       len++;

   return len;
}

/* Set the logging output to the default log file configured */
FILE* set_logfile()
{
    if (info != NULL){
        return info;
    }
    info = fopen(DEFAULT_LOGFILE, "a");
    if (info == NULL) {
        fprintf(stderr, "could not open log file ");
        return NULL;
    }
    fprintf(stderr, "writing errors/warnings/info/debug output to %s \n",
            DEFAULT_LOGFILE);
    return info;
}

// This method to unlink the program
static int xdp_unlink_bpf_chain(const char *map_filename) {
    int ret = 0;
    int key = 0;
    int map_fd = bpf_obj_get(map_filename);
    if (map_fd > 0) {
       ret = bpf_map_delete_elem(map_fd, &key);
       if (ret != 0) {
           log_err("xdp chain remove program failed");
       }
    }
    else {
       log_err("Previous program's map is not found %s", map_filename);
    }

    if (remove(xdp_rl_ingress_next_prog) < 0) {
        log_warn("Failed to remove map file - xdp_rl_ingress_next_prog");
    }

    return ret;
}


/* Unlink xdp kernel program on receiving KILL/INT signals */
static void signal_handler(int signal)
{
    log_info("Received signal %d", signal);

    xdp_unlink_bpf_chain(prev_prog_map);
    int i = 0;
    for(i=0; i<MAP_COUNT;i++) {
       close(map_fd[i]);
    }

    if (info != NULL)
        fclose(info);
    exit(EXIT_SUCCESS);
}

/* Get monotonic clock time in ns */
static __u64 time_get_ns()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Delete stale map entries(LRU) based on the timestamp at which
 * a map element is created. */
static void delete_stale_entries()
{
    log_debug("Deleting stale map entries periodically");

    if (map_fd[1] < 0) {
        log_info("Window map fd not found");
        exit(EXIT_FAILURE);
    }

    __u64 first_key = 0, next_key = 0;
    __u64 curr_time = time_get_ns();
    log_debug("Current time is %llu", curr_time);

    while (!bpf_map_get_next_key(map_fd[1], &first_key, &next_key))
    {
        if (next_key < (curr_time - buffer_time)) {
            log_debug("Deleting stale map entry %llu", next_key);
            if (bpf_map_delete_elem(map_fd[1], &next_key) != 0) {
                log_info("Map element not found");
            }
        }
        first_key = next_key;
    }
}

char * trim_space(char *str) {
    char *end;
    /* skip leading whitespace */
    while (isspace(*str)) {
        str = str + 1;
    }
    /* remove trailing whitespace */
    end = str + get_length(str) - 1;
    while (end > str && isspace(*end)) {
        end = end - 1;
    }
    /* write null character */
    *(end+1) = '\0';
    return str;
}

int strtoi(const char *str) {
  char *endptr;
  errno = 0;

  long long_var = strtol(str, &endptr, 10);
  //out of range, extra chars at end
  if (errno == ERANGE || *endptr != '\0' || str == endptr) {
     fprintf(stderr, "out of range");
  }

  return (int) long_var;
}

void update_ports(char *ports)
{
    char *ptr,*tmp ;
    uint16_t port = 0;
    uint8_t pval = 1;
    tmp = strdup(ports);
    while((ptr = strsep(&tmp, delim)) != NULL)
    {
        ptr = trim_space(ptr);
        port = (uint16_t)(strtoi(ptr));
        bpf_map_update_elem(map_fd[4], &port, &pval, 0);
    }
    free(tmp);
}

int main(int argc, char **argv)
{
    int longindex = 0, rate = 0, opt;
    int ret = EXIT_SUCCESS;
    char bpf_obj_file[256];
    char ports[2048];
    verbosity = LOG_INFO;
    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    int len = 0;
    snprintf(bpf_obj_file, sizeof(bpf_obj_file), "%s_kern.o", argv[0]);

    memset(&ports, 0, 2048);

    /* Parse commands line args */
    while ((opt = getopt_long(argc, argv, "h", long_options, &longindex)) != -1)
    {
        switch (opt) {
            case 'r':
                rate = strtoi(optarg);
                break;
            case 'i':
                ifindex = if_nametoindex(optarg);
                break;
            case 'v':
                if(optarg) {
                    verbosity = strtoi(optarg);
                }
                break;
            case 'm':
                if(optarg) {
                    len = get_length(optarg);
                    strncpy(prev_prog_map, optarg, len);
                    prev_prog_map[len] = '\0';
                }
                break;
            case 'p':
                if(optarg) {
                    len = get_length(optarg);
                    strncpy(ports, optarg, len);
                    ports[len] = '\0';
                }
                break;
            case 'd':
                /* Not honoured as of now */
                break;
            case 'h':
            default:
                usage(argv);
                return EXIT_FAILURE;
        }
    }
    if (setrlimit(RLIMIT_MEMLOCK, &r)) {
        perror("setrlimit(RLIMIT_MEMLOCK)");
        exit(EXIT_FAILURE);
    }
    set_logfile();

    __u64 ckey = 0, rkey = 0, dkey = 0, pkey = 0;
    __u64 recv_count = 0, drop_count = 0;

    if (load_bpf_file(bpf_obj_file)) {
        log_err("Failed to load bpf program");
        return 1;
    }
    if (!prog_fd[0]) {
        log_err("Failed to get bpf program fd")
        return 1;
    }

    /* Get the previous program's map fd in the chain */
    int prev_prog_map_fd = bpf_obj_get(prev_prog_map);
    if (prev_prog_map_fd < 0) {
        log_err("Failed to fetch previous xdp function in the chain");
        exit(EXIT_FAILURE);
    }
    /* Update current prog fd in the last prog map fd,
     * so it can chain the current one */
    if(bpf_map_update_elem(prev_prog_map_fd, &pkey, &(prog_fd[0]), 0)) {
        log_err("Failed to update prog fd in the chain");
        exit(EXIT_FAILURE);
    }
     /* closing map fd to avoid stale map */
     close(prev_prog_map_fd);

    int next_prog_map_fd = bpf_obj_get(xdp_rl_ingress_next_prog);
    if (next_prog_map_fd < 0) {
        log_info("Failed to fetch next prog map fd, creating one");
        if (bpf_obj_pin(map_fd[5], xdp_rl_ingress_next_prog)) {
            log_info("Failed to pin next prog fd map");
            exit(EXIT_FAILURE);
        }
    }


    /* Map FDs are sequenced same as they are defined in the bpf program ie.,
     * map_fd[0] = rl_config_map, map_fd[1] = rl_window_map
     * map_fd[2] = rl_recv_count_map, map_fd[3] = rl_drop_count_map
     * map_fd[4] = rl_ports_map
     * map_fd[5] =  xdp_rl_ingress_next_prog*/
    if (!map_fd[0]){
        log_err("ERROR: rl_config_map not found");
        return -1;
    }
    ret = bpf_map_update_elem(map_fd[0], &ckey, &rate, 0);
    if (ret) {
        perror("bpf_update_elem");
        return 1;
    }

    if (!map_fd[2]) {
        log_err("ERROR: rl_recv_count_map not found");
        return -1;
    }
    ret = bpf_map_update_elem(map_fd[2], &rkey, &recv_count, 0);
    if (ret) {
        perror("bpf_update_elem");
        return 1;
    }

    if (!map_fd[3]) {
        log_err("ERROR: rl_drop_count_map not found");
        return -1;
    }
    ret = bpf_map_update_elem(map_fd[3], &dkey, &drop_count, 0);
    if (ret) {
            perror("bpf_update_elem");
            return 1;
    }
    if (get_length(ports)) {
        log_info("port list is %s\n", ports);
        update_ports(ports);
    }

    /* Handle signals and exit clean */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    while(1)
    {
        sleep(60);
        /* Keep deleting the stale map entries periodically */
        delete_stale_entries();
        fflush(info);
    }
}
