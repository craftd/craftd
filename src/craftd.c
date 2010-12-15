/*
 * Copyright (c) 2010 Kevin M. Bowling, <kevin.bowling@kev009.com>, USA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>
 
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <pthread.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/thread.h>

#include "craftd-config.h"
#include "craftd.h"
#include "util.h"
#include "network/network.h"
#include "timeloop.h"
#include "httpd.h"

/**
 * Try and perform cleanup with a atexit call
 */
void
exit_handler(void)
{
  LOG(LOG_INFO, "Exiting.");
  //closelog();
}

void
readcb(struct bufferevent *bev, void *ctx)
{
  struct WQ_entry *workitem;
  struct PL_entry *player = ctx;
  
  /* Allocate and construct new work item */
  workitem = Malloc(sizeof(struct WQ_entry));
  workitem->bev = bev;
  workitem->player = player;
  
  /* Add item to work queue */
  pthread_spin_lock(&WQ_spinlock);
  STAILQ_INSERT_TAIL(&WQ_head, workitem, WQ_entries);
  pthread_spin_unlock(&WQ_spinlock);
  
  /* Dispatch to an open worker */
  //pthread_mutex_lock(&worker_cvmutex);
  pthread_cond_signal(&worker_cv);
  //pthread_mutex_unlock(&worker_cvmutex);

  return; // Good read
}

void
errorcb(struct bufferevent *bev, short error, void *ctx)
{
    int finished = 0;
    
    // Get player context from linked list 
    struct PL_entry *player = ctx;
    
    if (error & BEV_EVENT_EOF)
    {
        /* Connection closed, remove client from tables here */
        if( player->username->valid == 1)
          LOG(LOG_INFO, "Connection closed for: %s", player->username->str);
        else
          LOG(LOG_INFO, "Connection closed ip: %s", player->ip);
        finished = 1;
    }
    else if (error & BEV_EVENT_ERROR)
    {
        /* Some other kind of error, handle it here by checking errno */
        LOG(LOG_ERR, "Some kind of error to be handled in errorcb");
        //EVUTIL_EVENT_ERROR;
        finished = 1;
    }
    else if (error & BEV_EVENT_TIMEOUT)
    {
        /* Handle timeout somehow */
        LOG(LOG_ERR, "A buf event timeout?");
	finished = 1;
    }
    if (finished)
    {
        //TODO: Convert this to a SLIST_FOREACH
        //XXXX Grab a rdlock until player is found, wrlock delete, free
        pthread_rwlock_wrlock(&PL_rwlock);
	SLIST_REMOVE(&PL_head, ctx, PL_entry, PL_entries);
        --PL_count;
	pthread_rwlock_unlock(&PL_rwlock);
	
	/* If the client disconnects, remove any pending WP buffer events */
	struct WQ_entry *workitem, *workitemtmp;
	pthread_spin_lock(&WQ_spinlock);
	STAILQ_FOREACH_SAFE(workitem, &WQ_head, WQ_entries, workitemtmp)
	{
	  if(workitem->bev == bev)
	  {
	    STAILQ_REMOVE(&WQ_head, workitem, WQ_entry, WQ_entries);
	    LOG(LOG_DEBUG, "bev removed from workerpool by errorcb");
	  }
	}
	pthread_spin_unlock(&WQ_spinlock);
	
	if (ctx)
	  free(ctx);
	if (bev)
	  bufferevent_free(bev);
    }
}

void
do_accept(evutil_socket_t listener, short event, void *arg)
{
    struct event_base *base = arg;

    struct PL_entry *player;

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int fd = accept(listener, (struct sockaddr*)&ss, &slen);
    if (fd < 0)
    {
        ERR("accept error");
    }
    else if (fd > FD_SETSIZE)
    {
        LOG(LOG_CRIT, "too many clients");
        close(fd);
    }
    else
    {
        struct bufferevent *bev;

        /* Allocate space for a new player */
        player = Malloc(sizeof(struct PL_entry));
        player->fd = fd;
	
	/* Statically initialize the username string for now */
	// TODO: write a string initializer
        player->username = mcstring_create(strlen(""), "");

        /* Get the IPv4 or IPv6 address and store it */
        if (getpeername(fd, (struct sockaddr *)&ss, &slen))
        {
          LOG(LOG_ERR, "Couldn't get peer IP");
          close(fd);
          free(player);
          return;
        }
        void *inaddr;
        if (ss.ss_family == AF_INET)
        {
          inaddr = &((struct sockaddr_in*)&ss)->sin_addr;
        }
        else if (ss.ss_family == AF_INET6)
        {
          inaddr = &((struct sockaddr_in6*)&ss)->sin6_addr;
        }
        else
        {
          LOG(LOG_ERR, "weird address family");
          close(fd);
          free(player);
          return;
        }

        /* Initialize the player's internal rwlock */
        pthread_rwlock_init(&player->rwlock, NULL);
        
        /* Lock for the list ptr update and add them to the Player List */
        pthread_rwlock_wrlock(&PL_rwlock);
        SLIST_INSERT_HEAD(&PL_head, player, PL_entries);
        ++PL_count;
        pthread_rwlock_unlock(&PL_rwlock);

        evutil_inet_ntop(ss.ss_family, inaddr, player->ip, sizeof(player->ip));

        evutil_make_socket_nonblocking(fd);

        bev = bufferevent_socket_new(base, fd, 
				     BEV_OPT_CLOSE_ON_FREE|BEV_OPT_THREADSAFE);
	
	player->bev = bev;
	
        bufferevent_setcb(bev, readcb, NULL, errorcb, player);
        //bufferevent_setwatermark(bev, EV_READ, 0, MAX_BUF);
        bufferevent_enable(bev, EV_READ|EV_WRITE);
    }
}

void
run_server(void)
{
    evutil_socket_t listener;
    struct sockaddr_in sin;
    struct event_base* base;
    struct event *listener_event;

    base = event_base_new();
    if (!base)
    {
        LOG(LOG_CRIT, "Could not create MC libevent base!");
        exit(EXIT_FAILURE);
    }

    bzero(&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(Config.game_port);

    if ((listener = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    {
        PERR("cannot create socket");
        return;
    }

    evutil_make_socket_nonblocking(listener);

#ifndef WIN32
    {
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
#endif

    if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) < 0)
    {
        PERR("cannot bind");
        return;
    }

    if (listen(listener, MAX_LISTENBACKLOG) < 0)
    {
        PERR("listen error");
        return;
    }

    listener_event = event_new(base, listener, EV_READ|EV_PERSIST, 
        do_accept, (void*)base);

    event_add(listener_event, NULL);

    event_base_dispatch(base);
}

int
main(int argc, char *argv[])
{
  pthread_t httpd_thread_id;
  pthread_attr_t httpd_thread_attr;
  pthread_t timeloop_thread_id;
  pthread_attr_t timeloop_thread_attr;
  int status = 0;

  atexit(exit_handler);
  //setvbuf(stdout, NULL, _IONBF, 0); // set nonblocking stdout
  openlog(PACKAGE_TARNAME, LOG_PID, LOG_DAEMON);

  /* We initialize with stdout logging until config is loaded and the process
   * daemonizes.  DEBUG logs are disabled by default.
   */
  LOG = &log_console;
  LOG_setlogmask = &log_console_setlogmask;

  /* Print version info */
  craftd_version(argv[0]);

  /* Get command line arguments */
  int opt;
  int dontfork = 0;
  int dontlogmask = 0;
  char *argconfigfile = NULL;
  while((opt = getopt(argc, argv, "c:dhnv")) != -1)
  {
    switch(opt)
    {
      case 'd':
        dontlogmask = 1;
        break;
      case 'v':
        exit(EXIT_SUCCESS); // Version header already printed
      case 'n':
        dontfork = 1;
        break;
      case 'c':
        argconfigfile = optarg; // User specified conf file
        break;
      case 'h':
      default:
        fprintf(stderr, "\nUsage: %s [OPTION]...\n"
            "-c <conf file>\tspecify a conf file location\n"
            "-d\t\tenable verbose debugging messages\n"
            "-h\t\tdisplay this help and exit\n"
            "-n\t\tdon't fork/daemonize (overrides config file)\n"
            "-v\t\toutput version information and exit\n"
            "\nFor complete documentation, visit the wiki.\n\n", argv[0]);
        exit(EXIT_FAILURE);
    }
  }

  /* By default, mask debugging messages */
  if (!dontlogmask)
  {
    log_console_setlogmask(LOG_MASK(LOG_DEBUG));
    setlogmask(LOG_MASK(LOG_DEBUG));
  }

  /* Print startup message */
  LOG(LOG_INFO, "Server starting!  Max FDs: %d", sysconf(_SC_OPEN_MAX));

  /* Initialize the configuration */
  craftd_config_setdefaults();
  craftd_config_parse(argconfigfile);
  
  
  /* Declare the worker pool after reading config values */
  pthread_attr_t WP_thread_attr;
  pthread_t WP_thread_id[Config.workpool_size];
  int WP_id[Config.workpool_size];
  
  /* Player List singly-linked list setup */
  // hsearch w/direct ptr hashtable for name lookup if we need faster direct
  // access (keep two ADTs and entries for each player)
  pthread_rwlock_init(&PL_rwlock, NULL);
  SLIST_INIT(&PL_head);
  PL_count = 0;

  /* Work Queue is a singly-linked tail queue for player work requests */
  pthread_spin_init(&WQ_spinlock, 0);
  STAILQ_INIT(&WQ_head);
  WQ_count = 0;

  if (!dontfork && Config.daemonize == true) // TODO: or argv -d
  {
    LOG(LOG_INFO, "Daemonizing.");

    /* Swap over to syslog */
    LOG = &syslog;
    LOG_setlogmask = &setlogmask;
    
    CRAFTD_daemonize(0, 0); // chdir, and close FDs
  }

#ifdef WIN32
  status = evthread_use_windows_threads();
#else
  status = evthread_use_pthreads();
#endif
  
  evthread_enable_lock_debuging(); // TODO: DEBUG flag

  if(status != 0)
    ERR("Cannot initialize libevent threading");

  /* Start timeloop */
  pthread_attr_init(&timeloop_thread_attr);
  pthread_attr_setdetachstate(&timeloop_thread_attr, PTHREAD_CREATE_DETACHED);
  status = pthread_create(&timeloop_thread_id, &timeloop_thread_attr, 
      run_timeloop, NULL);
  if(status != 0)
    ERR("Cannot start timeloop");
  
  /* Start httpd if it is enabled */
  if (Config.httpd_enabled)
  {
    pthread_attr_init(&httpd_thread_attr);
    pthread_attr_setdetachstate(&httpd_thread_attr, PTHREAD_CREATE_DETACHED);
    status = pthread_create(&httpd_thread_id, &httpd_thread_attr, 
        run_httpd, NULL);
  
    if(status != 0)
      ERR("Cannot start httpd");
  }

  /* Start packet handler pool */
  pthread_attr_init(&WP_thread_attr);
  pthread_attr_setdetachstate(&WP_thread_attr, PTHREAD_CREATE_DETACHED);
  
  pthread_mutex_init(&worker_cvmutex, NULL);
  pthread_mutex_lock(&worker_cvmutex);
  status = pthread_cond_init(&worker_cv, NULL);
  if(status !=0)
    ERR("Worker condition var init failed!");
  
  for (int i = 0; i < Config.workpool_size; ++i)
  {
    WP_id[i] = i;
    status = pthread_create(&WP_thread_id[i], &WP_thread_attr,
        run_worker, (void *) &WP_id[i]);
    if(status != 0)
      ERR("Worker pool startup failed!");
  }

  /* Start inbound game server*/
  run_server();
  
  return 0;
}
