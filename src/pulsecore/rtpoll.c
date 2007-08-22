/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/utsname.h>
#include <sys/types.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/macro.h>
#include <pulsecore/llist.h>
#include <pulsecore/rtsig.h>
#include <pulsecore/flist.h>

#include "rtpoll.h"

struct pa_rtpoll {

    struct pollfd *pollfd, *pollfd2;
    unsigned n_pollfd_alloc, n_pollfd_used;

    pa_usec_t interval;

    int scan_for_dead;
    int running, installed, rebuild_needed;

#ifdef HAVE_PPOLL
    int rtsig;
    sigset_t sigset_unblocked;
    struct timespec interval_timespec;
    timer_t timer;
#ifdef __linux__
    int dont_use_ppoll;
#endif    
#endif
    
    PA_LLIST_HEAD(pa_rtpoll_item, items);
};

struct pa_rtpoll_item {
    pa_rtpoll *rtpoll;
    int dead;

    struct pollfd *pollfd;
    unsigned n_pollfd;

    int (*before_cb)(pa_rtpoll_item *i);
    void (*after_cb)(pa_rtpoll_item *i);
    void *userdata;
    
    PA_LLIST_FIELDS(pa_rtpoll_item);
};

PA_STATIC_FLIST_DECLARE(items, 0, pa_xfree);

static void signal_handler_noop(int s) { }

pa_rtpoll *pa_rtpoll_new(void) {
    pa_rtpoll *p;

    p = pa_xnew(pa_rtpoll, 1);

#ifdef HAVE_PPOLL

#ifdef __linux__
    /* ppoll is broken on Linux < 2.6.16 */
    
    p->dont_use_ppoll = 0;

    {
        struct utsname u;
        unsigned major, minor, micro;
    
        pa_assert_se(uname(&u) == 0);

        if (sscanf(u.release, "%u.%u.%u", &major, &minor, &micro) != 3 ||
            (major < 2) ||
            (major == 2 && minor < 6) ||
            (major == 2 && minor == 6 && micro < 16))

            p->dont_use_ppoll = 1;
    }

#endif

    p->rtsig = -1;
    sigemptyset(&p->sigset_unblocked);
    memset(&p->interval_timespec, 0, sizeof(p->interval_timespec));
    p->timer = (timer_t) -1;
        
#endif

    p->n_pollfd_alloc = 32;
    p->pollfd = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->pollfd2 = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->n_pollfd_used = 0;

    p->interval = 0;

    p->running = 0;
    p->installed = 0;
    p->scan_for_dead = 0;
    p->rebuild_needed = 0;
    
    PA_LLIST_HEAD_INIT(pa_rtpoll_item, p->items);

    return p;
}

void pa_rtpoll_install(pa_rtpoll *p) {
    pa_assert(p);
    pa_assert(!p->installed);
    
    p->installed = 1;

#ifdef HAVE_PPOLL
    if (p->dont_use_ppoll)
        return;

    if ((p->rtsig = pa_rtsig_get_for_thread()) < 0) {
        pa_log_warn("Failed to reserve POSIX realtime signal.");
        return;
    }

    pa_log_debug("Acquired POSIX realtime signal SIGRTMIN+%i", p->rtsig - SIGRTMIN);

    {
        sigset_t ss;
        struct sigaction sa;
        
        pa_assert_se(sigemptyset(&ss) == 0);
        pa_assert_se(sigaddset(&ss, p->rtsig) == 0);
        pa_assert_se(pthread_sigmask(SIG_BLOCK, &ss, &p->sigset_unblocked) == 0);
        pa_assert_se(sigdelset(&p->sigset_unblocked, p->rtsig) == 0);

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler_noop;
        pa_assert_se(sigemptyset(&sa.sa_mask) == 0);
        
        pa_assert_se(sigaction(p->rtsig, &sa, NULL) == 0);
        
        /* We never reset the signal handler. Why should we? */
    }
    
#endif
}

static void rtpoll_rebuild(pa_rtpoll *p) {

    struct pollfd *e, *t;
    pa_rtpoll_item *i;
    int ra = 0;
    
    pa_assert(p);

    p->rebuild_needed = 0;

    if (p->n_pollfd_used > p->n_pollfd_alloc) {
        /* Hmm, we have to allocate some more space */
        p->n_pollfd_alloc = p->n_pollfd_used * 2;
        p->pollfd2 = pa_xrealloc(p->pollfd2, p->n_pollfd_alloc * sizeof(struct pollfd));
        ra = 1;
    }

    e = p->pollfd2;

    for (i = p->items; i; i = i->next) {

        if (i->n_pollfd > 0)  {
            size_t l = i->n_pollfd * sizeof(struct pollfd);
            
            if (i->pollfd)
                memcpy(e, i->pollfd, l);
            else
                memset(e, 0, l);

            i->pollfd = e;
        } else
            i->pollfd = NULL;
        
        e += i->n_pollfd;
    }

    pa_assert((unsigned) (e - p->pollfd2) == p->n_pollfd_used);
    t = p->pollfd;
    p->pollfd = p->pollfd2;
    p->pollfd2 = t;
    
    if (ra)
        p->pollfd2 = pa_xrealloc(p->pollfd2, p->n_pollfd_alloc * sizeof(struct pollfd));

}

static void rtpoll_item_destroy(pa_rtpoll_item *i) {
    pa_rtpoll *p;

    pa_assert(i);

    p = i->rtpoll;

    PA_LLIST_REMOVE(pa_rtpoll_item, p->items, i);

    p->n_pollfd_used -= i->n_pollfd;
    
    if (pa_flist_push(PA_STATIC_FLIST_GET(items), i) < 0)
        pa_xfree(i);

    p->rebuild_needed = 1;
}

void pa_rtpoll_free(pa_rtpoll *p) {
    pa_assert(p);

    pa_assert(!p->items);
    pa_xfree(p->pollfd);
    pa_xfree(p->pollfd2);

#ifdef HAVE_PPOLL
    if (p->timer != (timer_t) -1) 
        timer_delete(p->timer);
#endif
    
    pa_xfree(p);
}

int pa_rtpoll_run(pa_rtpoll *p) {
    pa_rtpoll_item *i;
    int r = 0;
    
    pa_assert(p);
    pa_assert(!p->running);
    pa_assert(p->installed);
    
    p->running = 1;

    for (i = p->items; i; i = i->next) {

        if (i->dead)
            continue;
        
        if (!i->before_cb)
            continue;

        if (i->before_cb(i) < 0) {

            /* Hmm, this one doesn't let us enter the poll, so rewind everything */

            for (i = i->prev; i; i = i->prev) {

                if (i->dead)
                    continue;
                
                if (!i->after_cb)
                    continue;

                i->after_cb(i);
            }
            
            goto finish;
        }
    }

    if (p->rebuild_needed)
        rtpoll_rebuild(p);
    
    /* OK, now let's sleep */
#ifdef HAVE_PPOLL

#ifdef __linux__
    if (!p->dont_use_ppoll)
#endif
        r = ppoll(p->pollfd, p->n_pollfd_used, p->interval > 0  ? &p->interval_timespec : NULL, p->rtsig < 0 ? NULL : &p->sigset_unblocked);
#ifdef __linux__
    else
#endif

#else
        r = poll(p->pollfd, p->n_pollfd_used, p->interval > 0 ? p->interval / 1000 : -1);
#endif

    if (r < 0 && (errno == EAGAIN || errno == EINTR))
        r = 0;

    for (i = p->items; i; i = i->next) {

        if (i->dead)
            continue;

        if (!i->after_cb)
            continue;

        i->after_cb(i);
    }

finish:

    p->running = 0;
        
    if (p->scan_for_dead) {
        pa_rtpoll_item *n;

        p->scan_for_dead = 0;
        
        for (i = p->items; i; i = n) {
            n = i->next;

            if (i->dead)
                rtpoll_item_destroy(i);
        }
    }

    return r;
}

void pa_rtpoll_set_itimer(pa_rtpoll *p, pa_usec_t usec) {
    pa_assert(p);

    p->interval = usec;

#ifdef HAVE_PPOLL
    pa_timespec_store(&p->interval_timespec, usec);

#ifdef __linux__
    if (!p->dont_use_ppoll) {
#endif
        
        if (p->timer == (timer_t) -1) {
            struct sigevent se;

            memset(&se, 0, sizeof(se));
            se.sigev_notify = SIGEV_SIGNAL;
            se.sigev_signo = p->rtsig;

            if (timer_create(CLOCK_MONOTONIC, &se, &p->timer) < 0)
                if (timer_create(CLOCK_REALTIME, &se, &p->timer) < 0) {
                    pa_log_warn("Failed to allocate POSIX timer: %s", pa_cstrerror(errno));
                    p->timer = (timer_t) -1;
                }
        }

        if (p->timer != (timer_t) -1) {
            struct itimerspec its;

            memset(&its, 0, sizeof(its));
            pa_timespec_store(&its.it_value, usec);
            pa_timespec_store(&its.it_interval, usec);

            assert(timer_settime(p->timer, 0, &its, NULL) == 0);
        }

#ifdef __linux__
    }
#endif
    
#endif
}

pa_rtpoll_item *pa_rtpoll_item_new(pa_rtpoll *p, unsigned n_fds) {
    pa_rtpoll_item *i;
    
    pa_assert(p);
    pa_assert(n_fds > 0);

    if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        i = pa_xnew(pa_rtpoll_item, 1);

    i->rtpoll = p;
    i->dead = 0;
    i->n_pollfd = n_fds;
    i->pollfd = NULL;

    i->userdata = NULL;
    i->before_cb = NULL;
    i->after_cb = NULL;
    
    PA_LLIST_PREPEND(pa_rtpoll_item, p->items, i);

    p->rebuild_needed = 1;
    p->n_pollfd_used += n_fds;

    return i;
}

void pa_rtpoll_item_free(pa_rtpoll_item *i) {
    pa_assert(i);

    if (i->rtpoll->running) {
        i->dead = 1;
        i->rtpoll->scan_for_dead = 1;
        return;
    }

    rtpoll_item_destroy(i);
}

struct pollfd *pa_rtpoll_item_get_pollfd(pa_rtpoll_item *i, unsigned *n_fds) {
    pa_assert(i);

    if (i->rtpoll->rebuild_needed)
        rtpoll_rebuild(i->rtpoll);
    
    if (n_fds)
        *n_fds = i->n_pollfd;
    
    return i->pollfd;
}

void pa_rtpoll_item_set_before_callback(pa_rtpoll_item *i, int (*before_cb)(pa_rtpoll_item *i)) {
    pa_assert(i);

    i->before_cb = before_cb;
}

void pa_rtpoll_item_set_after_callback(pa_rtpoll_item *i, void (*after_cb)(pa_rtpoll_item *i)) {
    pa_assert(i);

    i->after_cb = after_cb;
}

void pa_rtpoll_item_set_userdata(pa_rtpoll_item *i, void *userdata) {
    pa_assert(i);

    i->userdata = userdata;
}
