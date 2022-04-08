#include "private.h"
#include <assert.h>
#include <arpa/inet.h>

#if defined(FZ_LINUX)
#include <errno.h>
#include <sys/syscall.h>
#else
#include <xen/xen.h>
#include <xenstore.h>
#endif /* FZ_LINUX */
#include "pv_no_cloning.h"

#define TIME_GET(ts) clock_gettime(CLOCK_REALTIME, (ts))

#define TIME_DIFF(start, stop, result) \
    do { \
        (result)->tv_sec = (stop)->tv_sec - (start)->tv_sec; \
        (result)->tv_nsec = (stop)->tv_nsec - (start)->tv_nsec; \
        if ((result)->tv_nsec < 0) { \
            --(result)->tv_sec; \
            (result)->tv_nsec += 1000000000; \
        } \
    } while (0)

#define TIME_DIFF_MSEC(start, stop, msec) \
    do { \
        struct timespec result; \
        TIME_DIFF(start, stop, &result); \
        *(msec) = result.tv_sec * 1000 + result.tv_nsec / 10000000; \
    } while (0)

/*static pthread_t network_thread;*/

static struct timespec vm_create_start_ts;

extern bool make_fuzz_ready(void);

enum thread_setup_command {
    TSC_NONE,
    TSC_DO_SETUP,
    TSC_DO_EXIT,
};

struct thread_data_setup {
    int started;
    enum thread_setup_command requested_command;
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cond_req;
    pthread_cond_t cond_rsp;
};
struct thread_data_setup thread_setup;

static void *thread_setup_routine(void *arg)
{
    struct thread_data_setup *t = arg;

    t->started = 1;

    while ( 1 )
    {
        pthread_mutex_lock(&t->mtx);

        /* wait command */
        while ( t->requested_command == TSC_NONE )
            pthread_cond_wait(&t->cond_req, &t->mtx);

        if ( t->requested_command == TSC_DO_EXIT )
            break;

        setup = true;
        make_parent_ready();
        setup = false;

        t->requested_command = TSC_NONE;

        /* notify completion */
        pthread_cond_signal(&t->cond_rsp);

        pthread_mutex_unlock(&t->mtx);
    }

    pthread_exit(NULL);
    return NULL;
}

static int thread_setup_init(struct thread_data_setup *t)
{
    int rc;

    t->requested_command = TSC_NONE;

    rc = pthread_mutex_init(&t->mtx, NULL);
    if ( rc )
    {
        fprintf(stderr, "pthread_mutex_init() failed rc=%d errno=%d\n",
                rc, errno);
        goto out;
    }

    rc = pthread_cond_init(&t->cond_req, NULL);
    if ( rc )
    {
        fprintf(stderr, "pthread_cond_init() failed rc=%d errno=%d\n",
                rc, errno);
        goto out;
    }

    rc = pthread_cond_init(&t->cond_rsp, NULL);
    if ( rc )
    {
        fprintf(stderr, "pthread_cond_init() failed rc=%d errno=%d\n",
                rc, errno);
        goto out;
    }

    rc = pthread_create(&t->thread, NULL, &thread_setup_routine, t);
    if ( rc )
    {
        fprintf(stderr, "pthread_create() failed rc=%d errno=%d\n",
                rc, errno);
        goto out;
    }

out:
    return rc;
}

static int thread_setup_fini(struct thread_data_setup *t)
{
    pthread_mutex_lock(&t->mtx);
    t->requested_command = TSC_DO_EXIT;
    pthread_cond_signal(&t->cond_req);
    pthread_mutex_unlock(&t->mtx);

    if ( t->started )
    {
        pthread_join(t->thread, NULL);
        t->started = 0;
    }

    pthread_cond_destroy(&t->cond_req);
    pthread_cond_destroy(&t->cond_rsp);
    pthread_mutex_destroy(&t->mtx);

    return 0;
}

static void thread_setup_request_setup(struct thread_data_setup *t)
{
    pthread_mutex_lock(&t->mtx);

    /* wait previous setup completion */
    t->requested_command = TSC_DO_SETUP;
    pthread_cond_signal(&t->cond_req);

    pthread_mutex_unlock(&t->mtx);
}

static void thread_setup_wait_setup_completion(struct thread_data_setup *t)
{
    pthread_mutex_lock(&t->mtx);

    /* wait setup completion */
    while ( t->requested_command == TSC_DO_SETUP )
        pthread_cond_wait(&t->cond_req, &t->mtx);

    pthread_mutex_unlock(&t->mtx);
}


/* void* wait_network(__attribute__((unused)) void *arg)
{

   static int port = 8888;
   char client_buf[BUFSIZ / 8];
   int server_fd, client_fd;
   struct sockaddr_in server_addr, client_addr;
   socklen_t client_len = sizeof(client_addr);

   server_fd = socket(PF_INET, SOCK_STREAM, 0);
   memset(&server_addr, 0, sizeof(server_addr));

   server_addr.sin_family = AF_INET;
   server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   server_addr.sin_port = htons(port);

   int opt_val = 1;
   setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

   if (bind(server_fd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
       fprintf(stderr, "Could not bind socket\n");
   }

   if (listen(server_fd, 2) < 0 ) {
       fprintf(stderr, "Could not listen on socket\n");
   }
   fprintf(stderr, "Server is listening on %d\n", port);

   fd_set fds;
   FD_ZERO(&fds);
   FD_SET(server_fd, &fds);
   int max_fd = server_fd;

  while (1) {
     fd_set read_fds = fds;
     int rc = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
     if (rc == -1) {
          fprintf(stderr, "select failed\n");
     }

     for (int i = 0; i <= max_fd; ++i) {
          if (FD_ISSET(i, &read_fds)) {
            fprintf(stderr, "fd = %d\n", i);
            if (i == server_fd) {
               client_fd = accept(server_fd, (struct sockaddr*) &client_addr, &client_len);
               FD_SET(client_fd, &fds);
               if (client_fd > max_fd) {
                  max_fd = client_fd;
               }
            } else {
          struct timespec end_time;
          int bytes = recv(i, &end_time, sizeof(end_time), 0);
          if (bytes <= 0) {
              close(i);
              FD_CLR(i, &fds);
              continue;
          }
          double time_diff = TIME_DIFF(vm_create_time, end_time);
          fprintf(stderr, "time network %lfms\n", time_diff * 1000);
            }
    }
     }
   }

   pthread_exit(NULL);
} */

#if DO_READ_BOOT_TIMESTAMP
static int read_timestamp_xs(struct timespec *ts)
{
    char *path = NULL, *value = NULL;
    unsigned int value_len;
    int rc;

    /* seconds */
    rc = asprintf(&path, "/local/domain/%d/data/seconds", domid);
    if ( !path )
    {
        fprintf(stderr, "Error allocating path rc=%d errno=%d\n", rc, errno);
        rc = -errno;
        goto out;
    }

    value = xs_read(xsh, XBT_NULL, path, &value_len);
    if ( !value )
    {
        fprintf(stderr, "Error calling xs_read() errno=%d\n", errno);
        rc = -errno;
        goto out;
    }

    sscanf(value, "%lu", &ts->tv_sec);

    free(path);
    path = NULL;
    free(value);
    value = NULL;

    /* nanoseconds */
    rc = asprintf(&path, "/local/domain/%d/data/nseconds", domid);
    if ( !path )
    {
        fprintf(stderr, "Error allocating path rc=%d errno=%d\n", rc, errno);
        rc = -errno;
        goto out;
    }

    value = xs_read(xsh, XBT_NULL, path, &value_len);
    if ( !value )
    {
        fprintf(stderr, "Error calling xs_read() errno=%d\n", errno);
        rc = -errno;
        goto out;
    }

    sscanf(value, "%lu", &ts->tv_nsec);
out:
    if ( path )
        free(path);
    if ( value )
        free(value);
    return rc;
}
#endif

static int wait_ready_xs(void)
{
    struct timespec start, stop;
    char *dom_path = NULL, *watched_path = NULL, *token = "ready";
    int rc = 0, waiting;

    /* register watch */

    dom_path = xs_get_domain_path(xsh, domid);
    if ( !dom_path )
    {
        fprintf(stderr, "Error calling xs_get_domain_path() errno=%d\n", errno);
        rc = -1;
        goto out;
    }

    rc = asprintf(&watched_path, "%s/data/trigger-harness", dom_path);
    if ( !watched_path )
    {
        fprintf(stderr, "Error allocating watched_path rc=%d errno=%d\n",
                rc, errno);
        rc = -1;
        goto out;
    }
    fprintf(stderr, "Watching %s\n", watched_path);

    rc = xs_watch(xsh, watched_path, token);
    if ( rc == false )
    {
        fprintf(stderr, "Error calling xs_watch() rc=%d errno=%d\n",
                rc, errno);
        rc = -1;
        goto out;
    }

    /* wait watch */

    // TODO: clock_get_time -> MACROS pentru masurat timp Expected: 30 ms
    waiting = 1;
    while ( waiting )
    {
        int fd = xs_fileno(xsh);
        fd_set set;
        long time_diff_msec;

        FD_ZERO(&set);
        FD_SET(fd, &set);

        TIME_GET(&start);
        rc = select(fd + 1, &set, NULL, NULL, NULL);
        TIME_GET(&stop);
        TIME_DIFF_MSEC(&start, &stop, &time_diff_msec);
        fprintf(stderr, "select time %ldms\n", time_diff_msec);

        if ( rc > 0 && FD_ISSET(fd, &set) )
        {
            char **vec = NULL, *value = NULL;
            unsigned int num, value_len;

            vec = xs_read_watch(xsh, &num);
            if ( !vec )
            {
                fprintf(stderr, "Error calling xs_read_watch() errno=%d\n",
                        errno);
                waiting = 0;
                rc = -1;
                goto out_loop;
            }

            value = xs_read(xsh, XBT_NULL, vec[XS_WATCH_PATH], &value_len);
            if ( value )
            {
                if ( !strcmp(value, "ready") )
                {
#if DO_READ_BOOT_TIMESTAMP
                    struct timespec vm_ready_ts;

                    rc = read_timestamp_xs(&vm_ready_ts);

                    TIME_DIFF_MSEC(&vm_create_start_ts, &vm_ready_ts, &time_diff_msec);
                    fprintf(stderr, "time xenstore %ldms\n", time_diff_msec);
#endif
                    waiting = 0;
                }
            }

            rc = 0;
out_loop:
            if ( value )
                free(value);
            if ( vec )
                free(vec);
        }
    }

    assert(xs_unwatch(xsh, watched_path, token) == true);
out:
    if ( watched_path )
        free(watched_path);
    if ( dom_path )
        free(dom_path);
   return rc;
}

static int domain_exists(uint32_t domid)
{
    xc_dominfo_t dominfo;
    int rc;

    rc = xc_domain_getinfo(xc, domid, 1, &dominfo);
    if ( rc < 0 )
    {
        if ( errno != ESRCH )
            fprintf(stderr, "Error calling xc_domain_getinfo() rc=%d errno=%d\n",
                    rc, errno);
        rc = 0;
        goto out;
    }

    if ( rc != 1 )
    {
        fprintf(stderr, "xc_domain_getinfo(): too many results rc=%d\n", rc);
        rc = 0;
        goto out;
    }

    if ( dominfo.domid != domid )
    {
        fprintf(stderr, "xc_domain_getinfo(): Invalid domid=%u vs %u\n",
                dominfo.domid, domid);
        rc = 0;
        goto out;
    }

    rc = 1;

out:
    return rc;
}

int no_cloning_reset(void)
{
    char cmd[1000];
    struct timespec start, stop;
    long time_elapsed_msec;
    int rc = 0;

    if ( !domain_exists(domid) )
    {
        fprintf(stderr, "Domain domid=%u does not exist\n", domid);
        rc = -1;
        goto out;
    }

    close_trace(vmi);
    vmi_destroy(vmi);

    /* destroy domain and wait for completion */
#if 0
    sprintf(cmd, "xl destroy %d\n", domid);
    rc = system(cmd);
    if ( rc )
    {
        fprintf(stderr, "Error executing command: %s, rc=%d\n", cmd, rc);
        goto out;
    }
#else
    rc = xc_domain_destroy(xc, domid);
    if ( rc )
    {
        fprintf(stderr, "Error calling xc_domain_destroy() rc=%d\n", rc);
        goto out;
    }
#endif

    TIME_GET(&start);
    while ( domain_exists(domid) )
    {
        TIME_GET(&stop);
        TIME_DIFF_MSEC(&start, &stop, &time_elapsed_msec);
        if ( time_elapsed_msec >= 3000 )
        {
            fprintf(stderr, "Could not destroy domid=%u\n", domid);
            rc = -1;
            goto out;
        }
    }

    domid = domid + 1;

    /* create domain */
    sprintf(cmd, "xl create -q -e %s\n", xl_config_path);

    TIME_GET(&vm_create_start_ts);
    rc = system(cmd);
    if ( rc )
    {
        fprintf(stderr, "Error executing command: %s, rc=%d\n", cmd, rc);
        goto out;
    }

    /* wait domain to get ready */
    TIME_GET(&start);
    rc = wait_ready_xs();
    TIME_GET(&stop);
    TIME_DIFF_MSEC(&start, &stop, &time_elapsed_msec);
    if ( rc )
    {
        fprintf(stderr, "Error calling wait_ready_xs() rc=%d\n", rc);
        goto out;
    }

    /* do setup */
    thread_setup_request_setup(&thread_setup);

    /* trigger guest harnessing */
    sprintf(cmd,
        "xenstore-write /local/domain/%d/data/trigger-harness done", domid);
    rc = system(cmd);
    if ( rc )
    {
        fprintf(stderr, "Error executing command: %s, rc=%d\n", cmd, rc);
        goto out;
    }

    thread_setup_wait_setup_completion(&thread_setup);

    fuzzdomid = domid;
    sinkdomid = domid;

    xc_domain_fuzzing_enable(xc, domid);

    make_parent_ready();
    make_sink_ready();
    make_fuzz_ready();

out:
    return rc;
}

int no_cloning_init(void)
{
    int rc;

    xsh = xs_open(0);
    if ( !xsh )
    {
        fprintf(stderr, "Error calling xs_open() errno=%d\n", errno);
        rc = -1;
        goto out;
    }

    rc = thread_setup_init(&thread_setup);
    if (rc)
        goto out;

    /*pthread_create(&network_thread, NULL, &wait_network, NULL);*/

out:
    return rc;
}

int no_cloning_fini(void)
{
    int rc;

    rc = thread_setup_fini(&thread_setup);
    if (rc)
        goto out;

    /*pthread_join(network_thread, NULL);*/

    if ( xsh )
        xs_close(xsh);

    free(xl_config_path);
    xl_config_path = NULL;

out:
    return rc;
}
