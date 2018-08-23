/*
    Example D-Bus server demonstrating how to handle
    time-consuming CPU-intensive messages with
    multithreading.

    Build with a command like

        gcc $(pkg-config --cflags dbus-1) -o slow_dbus_server \
            slow_dbus_server.c $(pkg-config --libs dbus-1) -pthread

    Copyright 2018 by Lawrence D'Oliveiro <ldo@geek-central.gen.nz>. This
    script is licensed CC0
    <https://creativecommons.org/publicdomain/zero/1.0/>; do with it
    what you will.
*/

#include <stdbool.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dbus/dbus.h>

/*
    Useful stuff
*/

#define die() exit(2)

static long get_milliseconds(void)
  {
    struct timespec now;
    const int sts = clock_gettime(CLOCK_MONOTONIC, &now);
    if (sts != 0)
      {
        perror("getting monotonic clock time");
        die();
      } /*if*/
    return
        (long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
  } /*get_milliseconds*/

static void check_dbus_error
  (
    const DBusError * dberr,
    const char * doing_what
  )
  {
    if (dbus_error_is_set(dberr))
      {
        fprintf(stderr, "libdbus error %s: %s\n", doing_what, dberr->message);
        die();
      } /*if*/
  } /*check_dbus_error*/

/*
    Thread management
*/

static int
    notify_send_pipe,
    notify_receive_pipe;
struct workqueue_entry
  {
    struct workqueue_entry * next;
    DBusMessage * request;
    int valtype;
    unsigned long limit, result;
  };
static struct workqueue_entry
    *finished = NULL,
    *finished_last = NULL;
static pthread_mutex_t
    workqueue_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
    Event-loop handling
*/

static DBusConnection *
    conn;
static bool
    quitting = false;

enum
  { /* should be enough for my simple app */
    MAX_WATCHES = 3,
    MAX_TIMEOUTS = 3,
  };

static DBusWatch *
    watches[MAX_WATCHES];
static DBusTimeout *
    timeouts[MAX_TIMEOUTS];
static int
    nr_watches = 0,
    nr_timeouts = 0;

static dbus_bool_t add_watch
  (
    DBusWatch * watch,
    void * _
  )
  {
    const bool ok = nr_watches < MAX_WATCHES;
    if (ok)
      {
        watches[nr_watches++] = watch;
      }
    else
      {
        fprintf(stderr, "add_watch: limit of %d watches reached.\n", MAX_WATCHES);
      } /*if*/
    return
        ok;
  } /*add_watch*/

static void remove_watch
  (
    DBusWatch * watch,
    void * _
  )
  {
    for (int i = 0;;)
      {
        if (i == nr_watches)
          {
            fprintf(stderr, "remove_watch: watch not found\n");
            break;
          } /*if*/
        if (watches[i] == watch)
          {
            fprintf(stderr, "remove_watch: removing at position %d\n", i);
            --nr_watches;
            for (int j = i; j < nr_watches; ++j)
              {
                watches[j] = watches[j + 1];
              } /*for*/
            break;
          } /*if*/
        ++i;
      } /*for*/
  } /*remove_watch*/

static void toggle_watch
  (
    DBusWatch * watch,
    void * _
  )
  {
    if (dbus_watch_get_enabled(watch))
      {
        add_watch(watch, _);
      }
    else
      {
        remove_watch(watch, _);
      } /*if*/
  } /*toggle_watch*/

static dbus_bool_t add_timeout
  (
    DBusTimeout * timeout,
    void * _
  )
  {
    const bool ok = nr_timeouts < MAX_TIMEOUTS;
    if (ok)
      {
        timeouts[nr_timeouts++] = timeout;
      }
    else
      {
        fprintf(stderr, "add_timeout: limit of %d timeouts reached.\n", MAX_TIMEOUTS);
      } /*if*/
    return
        ok;
  } /*add_timeout*/

static void remove_timeout
  (
    DBusTimeout * timeout,
    void * _
  )
  {
    for (int i = 0;;)
      {
        if (i == nr_timeouts)
          {
            fprintf(stderr, "remove_timeout: timeout not found\n");
            break;
          } /*if*/
        if (timeouts[i] == timeout)
          {
            fprintf(stderr, "remove_timeout: removing at position %d\n", i);
            --nr_timeouts;
            for (int j = i; j < nr_timeouts; ++j)
              {
                timeouts[j] = timeouts[j + 1];
              } /*for*/
            break;
          } /*if*/
        ++i;
      } /*for*/
  } /*remove_timeout*/

static void toggle_timeout
  (
    DBusTimeout * timeout,
    void * _
  )
  {
    if (dbus_timeout_get_enabled(timeout))
      {
        add_timeout(timeout, _);
      }
    else
      {
        remove_timeout(timeout, _);
      } /*if*/
  } /*toggle_timeout*/

static void handle_event(void)
  {
    struct pollfd topoll[MAX_WATCHES + 1];
    int total_timeout = -1; /* to begin with */
    for (int i = 0; i < nr_watches; ++i)
      {
        DBusWatch * const watch = watches[i];
        struct pollfd * const entry = topoll + i;
        entry->fd = dbus_watch_get_unix_fd(watch);
        entry->events = 0; /* to begin with */
        if (dbus_watch_get_enabled(watch))
          {
            const int flags = dbus_watch_get_flags(watch);
            if ((flags & DBUS_WATCH_READABLE) != 0)
              {
                entry->events |= POLLIN | POLLERR;
              } /*if*/
            if ((flags & DBUS_WATCH_WRITABLE) != 0)
              {
                entry->events |= POLLOUT | POLLERR;
              } /*if*/
          } /*if*/
      } /*for*/
      {
        struct pollfd * const entry = topoll + nr_watches;
        entry->fd = notify_receive_pipe;
        entry->events = POLLIN;
      }
    for (int i = 0; i < nr_timeouts; ++i)
      {
        DBusTimeout * const timeout = timeouts[i];
        if (dbus_timeout_get_enabled(timeout))
          {
            const int interval = dbus_timeout_get_interval(timeout);
            if (total_timeout < 0 || total_timeout > interval)
              {
                total_timeout = interval;
              } /*if*/
          } /*if*/
      } /*for*/
    const long timeout_start = get_milliseconds();
    bool got_io;
      {
        const int sts = poll(topoll, nr_watches + 1, total_timeout);
        fprintf(stderr, "poll returned status %d\n", sts);
        if (sts < 0)
          {
            perror("doing poll");
            die();
          } /*if*/
        got_io = sts > 0;
      }
    for (int i = 0; i < nr_watches; ++i)
      {
        struct pollfd * const entry = topoll + i;
        if (entry->revents != 0)
          {
            unsigned int flags =
                    ((entry->revents & POLLIN) != 0 ? DBUS_WATCH_READABLE : 0)
                |
                    ((entry->revents & POLLOUT) != 0 ? DBUS_WATCH_WRITABLE : 0)
                |
                    ((entry->revents & POLLERR) != 0 ? DBUS_WATCH_ERROR : 0);
            const bool ok = dbus_watch_handle(watches[i], flags);
            if (!ok)
              {
                fprintf(stderr, "dbus_watch_handle failure\n");
                die();
              } /*if*/
          } /*if*/
      } /*for*/
      {
        struct pollfd * const entry = topoll + nr_watches;
        if ((entry->revents & POLLIN) != 0)
          {
            char dummy[20];
            read(notify_receive_pipe, dummy, sizeof dummy);
              {
                const int sts = pthread_mutex_lock(&workqueue_mutex);
                if (sts != 0)
                  {
                    perror("locking workqueue mutex to retrieve results");
                  } /*if*/
              }
            for (;;)
              {
                struct workqueue_entry * const entry = finished;
                if (entry == NULL)
                  {
                    finished_last = NULL;
                    break;
                  } /*if*/
                finished = finished->next;
                DBusMessage * const reply = dbus_message_new_method_return(entry->request);
                if (reply == NULL)
                  {
                    fprintf(stderr, "failed to allocate D-Bus reply message\n");
                    die();
                  } /*if*/
                  {
                  /* return same type as was passed */
                    unsigned char bresult;
                    unsigned short wresult;
                    unsigned int iresult;
                    const void * argptr;
                    switch (entry->valtype)
                      {
                    case DBUS_TYPE_BYTE:
                        bresult = entry->result;
                        argptr = &bresult;
                    break;
                    case DBUS_TYPE_UINT16:
                        wresult = entry->result;
                        argptr = &wresult;
                    break;
                    case DBUS_TYPE_UINT32:
                        iresult = entry->result;
                        argptr = &iresult;
                    break;
                    case DBUS_TYPE_UINT64:
                        argptr = &entry->result;
                    break;
                    default:
                        fprintf(stderr, "SHOULDN’T OCCUR: arg valtype = %d\n", entry->valtype);
                        die();
                    break;
                      } /*switch*/
                    const bool ok = dbus_message_append_args
                      (
                        reply,
                        entry->valtype, argptr,
                        DBUS_TYPE_INVALID /* marks end of args */
                      );
                    if (!ok)
                      {
                        fprintf(stderr, "dbus_message_append_args failure\n");
                        die();
                      } /*if*/
                  }
                  {
                    const bool ok = dbus_connection_send(conn, reply, NULL);
                    if (!ok)
                      {
                        fprintf(stderr, "dbus_message_send failure\n");
                        die();
                      } /*if*/
                  }
                dbus_message_unref(reply);
                dbus_message_unref(entry->request);
                free(entry);
              } /*for*/
            pthread_mutex_unlock(&workqueue_mutex);
          } /*if*/
      }
    const long interval = get_milliseconds() - timeout_start;
    for (int i = 0; i < nr_timeouts; ++i)
      {
        DBusTimeout * const timeout = timeouts[i];
        if (dbus_timeout_get_enabled(timeout) && dbus_timeout_get_interval(timeout) > interval)
          {
            const bool ignore = dbus_timeout_handle(timeout);
          } /*if*/
      } /*for*/
    if (got_io)
      {
        for (;;)
          {
            const DBusDispatchStatus sts = dbus_connection_dispatch(conn);
            if (sts == DBUS_DISPATCH_NEED_MEMORY)
              {
                fprintf(stderr, "dbus_connection_dispatch ran out of memory\n");
                die();
              } /*if*/
            if (sts != DBUS_DISPATCH_DATA_REMAINS)
                break;
          } /*for*/
      } /*if*/
  } /*handle_event*/

/*
    Slow Computation
*/

static void * compute_primes
  (
    void * data
  )
  {
    struct workqueue_entry * const context = (struct workqueue_entry *)data;
      {
        const unsigned long limit = context->limit;
        unsigned long result = 0;
        unsigned long step = 1;
        for (unsigned long i = 2;;)
          {
            if (i > limit)
                break;
            bool is_prime;
            for (unsigned long j = 2;;)
              {
                if (i / j < j)
                  {
                    is_prime = true;
                    break;
                  } /*if*/
                if (i % j == 0)
                  {
                    is_prime = false;
                    break;
                  } /*if*/
                ++j;
              } /*for*/
            if (is_prime)
              {
                ++result;
              } /*if*/
            i += step;
            step = 2;
          } /*for*/
        context->result = result;
      }
      {
        const int sts = pthread_mutex_lock(&workqueue_mutex);
        if (sts != 0)
          {
            perror("locking workqueue mutex to return result");
          } /*if*/
      }
    context->next = NULL;
    if (finished == NULL)
      {
        finished = context;
        finished_last = context;
      }
    else
      {
        finished_last->next = context;
        finished_last = context;
      } /*if*/
    pthread_mutex_unlock(&workqueue_mutex);
      { /* wake up mainline */
        unsigned char buf = 0;
        write(notify_send_pipe, &buf, 1);
      }
    return
        NULL;
  } /*compute_primes*/

/*
    Mainline
*/

static const char *
    const my_bus_name = "com.example.slow_server";
static const char *
    const my_interface_name = my_bus_name;

static DBusHandlerResult handle_message
  (
    DBusConnection * conn,
    DBusMessage * message,
    void * _
  )
  {
    bool handled = false; /* initial assumption */
    const char * const path = dbus_message_get_path(message);
    const char * const interface = dbus_message_get_interface(message);
    const char * const member = dbus_message_get_member(message);
    const char * const signature = dbus_message_get_signature(message);
    fprintf(stderr, "message received of type %d, path %s, interface %s, member %s, signature %s\n", dbus_message_get_type(message), path, interface, member, signature); /* debug */
    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL && strcmp(interface, my_interface_name) == 0)
      {
        fprintf(stderr, "matches my interface\n");
        handled = true; /* next assumption */
        if (strcmp(member, "quit") == 0)
          {
            fprintf(stderr, "quit method received\n");
            quitting = true;
          }
        else if (strcmp(member, "count_primes") == 0)
          {
            if (strlen(signature) == 1)
              {
                unsigned long limit;
                bool ok;
                DBusError dberr;
                dbus_error_init(&dberr);
                if (signature[0] == DBUS_TYPE_BYTE)
                  {
                    unsigned char blimit;
                    ok = dbus_message_get_args(message, &dberr, DBUS_TYPE_BYTE, &blimit);
                    limit = blimit;
                  }
                else if (signature[0] == DBUS_TYPE_UINT16)
                  {
                    unsigned short wlimit;
                    ok = dbus_message_get_args(message, &dberr, DBUS_TYPE_UINT16, &wlimit);
                    limit = wlimit;
                  }
                else if (signature[0] == DBUS_TYPE_UINT32)
                  {
                    unsigned int ilimit;
                    ok = dbus_message_get_args(message, &dberr, DBUS_TYPE_UINT32, &ilimit);
                    limit = ilimit;
                  }
                else if (signature[0] == DBUS_TYPE_UINT64)
                  {
                    ok = dbus_message_get_args(message, &dberr, DBUS_TYPE_UINT64, &limit);
                  }
                else
                  {
                    handled = false;
                  } /*if*/
                if (handled)
                  {
                    struct workqueue_entry * context = (struct workqueue_entry *)malloc(sizeof(struct workqueue_entry));
                    if (context == NULL)
                      {
                        fprintf(stderr, "malloc of workqueue entry failed\n");
                        die();
                      } /*if*/
                    context->request = dbus_message_ref(message);
                    context->valtype = signature[0];
                    context->limit = limit;
                    pthread_t child;
                    const int err = pthread_create
                      (
                        /*thread =*/ &child,
                        /*attr =*/ NULL,
                        /*start_routine =*/ compute_primes,
                        /*arg =*/ context
                      );
                    if (err == 0)
                      {
                        fprintf(stderr, "child thread %d created.\n", child);
                      }
                    else
                      {
                        fprintf(stderr, "error %d creating thread: %s\n", err, strerror(err));
                        die();
                      } /*if*/
                  } /*if*/
              }
            else
              {
                handled = false;
              } /*if*/
          }
        else
          {
            handled = false;
          } /*if*/
      } /*if*/
    return
        handled ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  } /*handle_message*/

int main
  (
    int argc,
    char ** argv
  )
  {
    DBusError dberr;
    dbus_error_init(&dberr);
    conn = dbus_bus_get(DBUS_BUS_SESSION, &dberr);
    check_dbus_error(&dberr, "getting bus connection");
      {
        const int sts = dbus_bus_request_name
          (
            /*connection =*/ conn,
            /*name =*/ my_bus_name,
            /*flags =*/ DBUS_NAME_FLAG_DO_NOT_QUEUE,
            /*error =*/ &dberr
          );
        check_dbus_error(&dberr, "registering bus name");
        if (sts != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
          {
            fprintf(stderr, "unexpected reply code %d trying to register name\n", sts);
            die();
          } /*if*/
      }
      {
        const bool ok = dbus_connection_set_watch_functions
          (
            /*connection =*/ conn,
            /*add_function =*/ add_watch,
            /*remove_function =*/ remove_watch,
            /*toggled_function =*/ toggle_watch,
            /*data =*/ NULL,
            /*free_data_function =*/ NULL
          );
        if (!ok)
          {
            fprintf(stderr, "dbus_connection_set_watch_functions failure\n");
            die();
          } /*if*/
      }
      {
        const bool ok = dbus_connection_set_timeout_functions
          (
            /*connection =*/ conn,
            /*add_function =*/ add_timeout,
            /*remove_function =*/ remove_timeout,
            /*toggled_function =*/ toggle_timeout,
            /*data =*/ NULL,
            /*free_data_function =*/ NULL
          );
        if (!ok)
          {
            fprintf(stderr, "dbus_connection_set_timeout_functions failure\n");
            die();
          } /*if*/
      }
      {
        const bool ok = dbus_connection_add_filter
          (
            /*connection =*/ conn,
            /*function =*/ handle_message,
            /*user_data =*/ NULL,
            /*free_data_function =*/ NULL
          );
        if (!ok)
          {
            fprintf(stderr, "dbus_connection_add_filter failure\n");
            die();
          } /*if*/
      }
      {
        int pipefd[2];
        const int sts = pipe(pipefd);
        if (sts != 0)
          {
            perror("creating notification pipes");
          } /*if*/
        notify_receive_pipe = pipefd[0];
        notify_send_pipe = pipefd[1];
      }
    do
      {
        handle_event();
      }
    while (!quitting);
    fprintf(stderr, "quitting.\n");
    return
        0;
  } /*main*/

