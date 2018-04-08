/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
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
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */// 函数将ev注册到ev->ev_base上，事件类型由ev->ev_events指明，如果注册成功，ev将被插入

// event.c：event整体框架的代码实现；
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else 
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

// Libevent把所有支持的I/O demultiplex机制存储在
// 一个全局静态数组eventops中，并在初始化时选择使用
// 何种机制
/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_base *current_base = NULL; // 全局变量
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);		/* Signal callback when gotsig is set */
volatile sig_atomic_t event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

// 检测是否支持monotonic时间
static void
detect_monotonic(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	// 如果支持monotonic时间
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1; // 设置相应标志
#endif
}

// base中的tv_cache用来记录时间缓存，主要是为了防止频繁的调用系统函数
// 来获取时间，而获取时间的调用为gettime。
static int
gettime(struct event_base *base, struct timeval *tp)
{
	// 如果缓冲时间的秒非0,则直接返回缓冲时间
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	// 如果支持monotonic时间
	if (use_monotonic) {
		struct timespec	ts;

		// 获取monotonic时间，如果获取失败
		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);
		// 返回获取的monotonic时间
		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		return (0);
	}
#endif

	// 返回获取的时间
	// evutil_gettimeofday 内部直接调用 gettimeofday函数
	return (evutil_gettimeofday(tp, NULL));
}

// 创建一个event_base对象也既是创建了一个新的libevent实例，程序需要通过
// 调用event_init()（内部调用event_base_new函数执行具体操作）函数来创建，
// 该函数同时还对新生成的libevent实例进行了初始化。该函数首先为event_base
// 实例申请空间，然后初始化timer mini-heap，选择并初始化合适的系统I/O 的
// demultiplexer机制，初始化各事件链表；函数还检测了系统的时间设置，为后面
// 的时间管理打下基础。
struct event_base *
event_init(void)
{
	// 创建一个新的libevent实例
	struct event_base *base = event_base_new();

	// 如果创建成功
	if (base != NULL)
		current_base = base; // 设置全局变量

	// 返回
	return (base);
}

// 该函数对新生成的libevent实例进行了初始化。该函数首先为event_base实例申
// 请空间，然后初始化timer mini-heap，选择并初始化合适的系统I/O 的demult
// iplexer机制，初始化各事件链表；函数还检测了系统的时间设置，为后面的时间管
// 理打下基础。
struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;
	
	// 分配内存空间
	if ((base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc", __func__);
	
	event_sigcb = NULL;
	event_gotsig = 0;

	
	detect_monotonic();
	gettime(base, &base->event_tv);
	
	min_heap_ctor(&base->timeheap);
	TAILQ_INIT(&base->eventqueue);
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;

	// 然后libevent根据系统配置和编译选项决定使用哪一种
	// I/O demultiplex机制
	base->evbase = NULL;
	for (i = 0; eventops[i] && !base->evbase; i++) { // I/O 多路复机制存在
		base->evsel = eventops[i]; // 设置I/O多路复用机制
		base->evbase = base->evsel->init(base); // 初始化I/O多路复用机制
	}
	
	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (evutil_getenv("EVENT_SHOW_METHOD")) 
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(base, 1);

	return (base);
}

void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {
		event_del(ev);
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {
		for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
			struct event *next = TAILQ_NEXT(ev, ev_active_next);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {
				event_del(ev);
				++n_deleted;
			}
			ev = next;
		}
	}

	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
			__func__, n_deleted));

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);

	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i]));

	assert(min_heap_empty(&base->timeheap));
	min_heap_dtor(&base->timeheap);

	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]);
	free(base->activequeues);

	assert(TAILQ_EMPTY(&base->eventqueue));

	free(base);
}

/* reinitialized the event base after a fork */
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

#if 0
	/* Right now, reinit always takes effect, since even if the
	   backend doesn't require it, the signal socketpair code does.
	 */
	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)
		return (0);
#endif

	/* prevent internal delete */
	if (base->sig.ev_signal_added) {
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,
		    EVLIST_INSERTED);
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;
	}

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}

int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}

int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;

	if (base->event_count_active)
		return (-1);

	if (npriorities == base->nactivequeues)
		return (0);

	if (base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);
		}
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)
	    calloc(base->nactivequeues, sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

// 判断当前反应堆中是否存在事件
int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;

	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
		if (ev->ev_events & EV_PERSIST)
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);
		
		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (event_gotsig || base->event_break)
				return;
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

const char *
event_base_get_method(struct event_base *base)
{
	assert(base);
	return (base->evsel->name);
}

static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(const struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}



/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}


// Libevent的事件主循环主要是通过event_base_loop ()函数完成的
int
event_base_loop(struct event_base *base, int flags)
{
	// I\O复用策略
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	
	struct timeval tv;
	struct timeval *tv_p;
	int res, done;
	
	/* clear time cache */
	// 清空时间缓存
	base->tv_cache.tv_sec = 0;

	// 设置事件所属反应堆
	if (base->sig.ev_signal_added)
		evsignal_base = base; // evsignal_base是全局变量，在处理signal时，用于指名signal所属的event_base实例

	// 进入事件循环
	done = 0;
	while (!done) {
		
		/* Terminate the loop if we have been asked to */
		/**
		 * 查看是否需要跳出循环，程序可以调用event_loopexit_cb()设置
		 * event_gotterm标记调用event_base_loopbreak()设置event
		 * _break标记
		 */
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
		}
		if (base->event_break) {
			base->event_break = 0;
			break;
		}
		
		/* You cannot use this interface for multi-threaded apps */
		while (event_gotsig) {
			event_gotsig = 0;
			if (event_sigcb) {
				res = (*event_sigcb)();
				if (res == -1) {
					errno = EINTR;
					return (-1);
				}
			}
		}
				
		// 校正系统时间
	    // 校正系统时间，如果系统使用的是非MONOTONIC时间，用户可能会向后调整了系统时间
		// 在timeout_correct函数里，比较last wait time和当前时间，如果当前时间< 
		// last wait time 表明时间有问题，这是需要更新timer_heap中所有定时事件的超
		// 时时间。
		// 纠正时间系统
		timeout_correct(base, &tv);
		
		// 根据timer heap中事件的最小超时时间，计算系统I/O demultiplexer的最大等待时间
		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) { // 不存在激活事件，并且并未阻塞循环
			timeout_next(base, &tv_p); // 获取当前最小时间堆中，待触发的最小时间
		} else { // 存在激活事件
			/* 
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			 // 依然有未处理的就绪时间，就让I/O demultiplexer立即返回，不必等待
			 // 下面会提到，在libevent中，低优先级的就绪事件可能不能立即被处理
			evutil_timerclear(&tv);
		}

		/* If we have no events, we just exit */
		// 如果当前没有注册事件，就退出
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}
		
		/* update last old time */
		// 获取当前时间，并存储到event_tv中
		gettime(base, &base->event_tv);
		
		/* clear time cache */
		// 清空缓存时间
		base->tv_cache.tv_sec = 0;
		// 调用系统I/O demultiplexer等待就绪I/O events，可能是epoll_wait，或者select等；
		// 在evsel->dispatch()中，会把就绪signal event、I/O event插入到激活链表中
		res = evsel->dispatch(base, evbase, tv_p);
		if (res == -1)
			return (-1);
			
		// 将time cache赋值为当前系统时间
		gettime(base, &base->tv_cache);
		// 检查heap中的timer events，将就绪的timer event从heap上删除，并插入到激活链表中
		timeout_process(base);
        // 调用event_process_active()处理激活链表中的就绪event，调用其回
		// 调函数执行事件处理该函数会寻找最高优先级（priority值越小优先级越高）
		// 的激活事件链表，然后处理链表中的所有就绪事件；因此低优先级的就绪事件
		// 可能得不到及时处理；
		if (base->event_count_active) { // 如果存在激活事件
			event_process_active(base); // 处理激活事件
			if (!base->event_count_active && (flags & EVLOOP_ONCE)) // 不存在激活事件，并且只循环一次
				done = 1; // 设置标志
		} else if (flags & EVLOOP_NONBLOCK) // 循环不阻塞
			done = 1;  // 设置标志
	}
	
	/* clear time cache */
	// 循环结束，清空时间缓存
	base->tv_cache.tv_sec = 0;
	
	event_debug(("%s: asked to terminate loop.", __func__));
	
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if (events == EV_TIMEOUT) {
		if (tv == NULL) {
			evutil_timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);
	if (res == 0)
		res = event_add(&eonce->ev, tv);
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}

/**
 * 要向libevent添加一个事件，需要首先设置event对象，这通过调用libevent提供的函数有：
 * event_set(),ev_resevent_base_set(),event_priority_set()来完成；下面分别进行讲解。
 */

/** 
 * 1.设置事件ev绑定的文件描述符或者信号，对于定时事件，设为-1即可；
 * 2.设置事件类型，比如EV_READ|EV_PERSIST, EV_WRITE, EV_SIGNAL等；
 * 3.设置事件的回调函数以及参数arg；
 * 4.初始化其它字段，比如缺省的event_base和优先级；
 */
// ev - 待设置的事件
// fd - 待绑定的文件描述符号/信号（对于定时事件，设置为-1）
// events - 设置事件类型
// callback - 响应事件的回调函数
// arg - 参数
void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)
{

	/* Take the current base - caller needs to set the real base later */
	// 初始化所属反应堆
	ev->ev_base = current_base;

	// 设置回调函数
	ev->ev_callback = callback; // 设置回调函数
	// 设置参数
	ev->ev_arg = arg; // 参数
	// 设置文件描述符或信号
	ev->ev_fd = fd; // 文件描述符或信号
	// 设置事件
	ev->ev_events = events; // 事件类型
	// ???
	ev->ev_res = 0; // 回调函数返回值
	// 设置当前事件状态
	ev->ev_flags = EVLIST_INIT; // 设置事件状态为“已初始化”
	// 设置事件触发时回调函数调用次数
	ev->ev_ncalls = 0; // 执行时回调函数调用次数
	ev->ev_pncalls = NULL;
	
	// 初始化组最小堆
	min_heap_elem_init(ev);
	
	/* by default, we put new events into the middle priority */
	// 设置事件优先级
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;

}

// 设置event ev将要注册到的event_base；
// libevent有一个全局event_base指针current_base，默认情况下事件
// ev将被注册到current_base上，使用该函数可以指定不同的event_base；
// 如果一个进程中存在多个libevent实例，则必须要调用该函数为event设置
// 不同的event_base；
// base - 输入的event_base
// ev - 输入的事件
int
event_base_set(struct event_base *base, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */
	// 事件尚未被初始化
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	// 存储event_base、设置优先级
	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	// 返回
	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */
// 设置event ev的优先级，没什么可说的，注意的一点就是：当ev正处于就绪状态时，
// 不能设置，返回-1。
// ev - 输入的事件
// pri - 优先级
int
event_priority_set(struct event *ev, int pri)
{
	// 如果事件处于激活状态
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	// 优先级非法
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);
	// 设置优先级
	ev->ev_pri = pri;
	// 返回
	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */
int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	struct timeval	now, res;
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(ev->ev_base, &now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}

/**
 * 前面提到Reactor框架的作用就是提供事件的注册、注销接口；根据系统提供的事件多路分发机
 * 制执行事件循环，当有事件进入“就绪”状态时，调用注册事件的回调函数来处理事件。Libevent
 * 中对应的接口函数主要就是：
 * int  event_add(struct event *ev, const struct timeval *timeout);
 * int  event_del(struct event *ev);
 * int  event_base_loop(struct event_base *base, int loops);
 * void event_active(struct event *event, int res, short events);
 * void event_process_active(struct event_base *base);
 * 本节将按介绍事件注册和删除的代码流程，libevent的事件循环框架将在下一节再具体描述。对于
 * 定时事件，这些函数将调用timer heap管理接口执行插入和删除操作；对于I/O和Signal事件将
 * 调用eventopadd和delete接口函数执行插入和删除操作（eventop会对Signal事件调用Signal
 * 处理接口执行操作）；这些组件将在后面的内容描述。
 */


// 注册事件
// ev：指向要注册的事件；
// tv：超时时间；
// 函数将ev注册到ev->ev_base上，事件类型由ev->ev_events指明，如果注册成功，ev将被插入
// 到已注册链表中；如果tv不是NULL，则会同时注册定时事件，将ev添加到timer堆上；如果其中有
// 一步操作失败，那么函数保证没有事件会被注册，可以讲这相当于一个原子操作。这个函数也体现了
// libevent细节之处的巧妙设计，且仔细看程序代码，部分有省略，注释直接附在代码中。
int
event_add(struct event *ev, const struct timeval *tv)
{

	// 要注册到的event_base
	struct event_base *base = ev->ev_base;
	
	// base使用的系统I/O策略
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	
	int res = 0;
	
	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));
		 
	// 标志中存在其他异常状态
	assert(!(ev->ev_flags & ~EVLIST_ALL));
	
	//
    // 新的timer事件，调用timer heap接口在堆上预留一个位置
    // 注：这样能保证该操作的原子性：
    // 向系统I/O机制注册可能会失败，而当在堆上预留成功后，
    // 定时事件的添加将肯定不会失败；
    // 而预留位置的可能结果是堆扩充，但是内部元素并不会改变
	// 
	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	// 如果时间信息存在，并且事件状态处于非激活状态
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		// 在堆上预留一个位置
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}
	
	// 如果事件ev期望追踪读、写以及信号事件
	// 如果事件ev不在已注册或者激活链表中，则调用evbase注册事件
	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
		res = evsel->add(evbase, ev); // 增加一套I/O多路复用机制
		if (res != -1) // 注册成功，插入event到已注册链表中
			event_queue_insert(base, ev, EVLIST_INSERTED); // 插入已注册链表中
	}

	/* 
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 */
	// 准备添加定时器事件
	if (res != -1 && tv != NULL) {
		
		struct timeval now;

		/* 
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 */
		// EVLIST_TIMEOUT表明event已经在定时器堆中了，删除旧的
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT); // 删除旧的

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		// 如果事件已经是就绪状态
		// 并且当前事件由于超时激活
		if ((ev->ev_flags & EVLIST_ACTIVE)&&
			(ev->ev_res & EV_TIMEOUT)) {
			/* See if we are just active executing this
			 * event in a loop
			 */
			// 将ev_callback调用次数设置为0
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			// 从链表中移除
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}
		
		// 计算时间，并插入到timer小根堆中
		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout);
		
		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		// 将事件插入到timer小根堆中
		event_queue_insert(base, ev, EVLIST_TIMEOUT)
		
	return (res);
}

// 函数将删除事件ev，对于I/O事件，从I/O 的demultiplexer上将事件注销；
// 对于Signal事件，将从Signal事件链表中删除；对于定时事件，将从堆上删除；
// 同样删除事件的操作则不一定是原子的，比如删除时间事件之后，有可能从系统
// I/O机制中注销会失败。
int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	// ev_base为NULL，表明ev没有被注册
	if (ev->ev_base == NULL)
		return (-1);

	// 取得ev注册的event_base和eventop指针
	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	// 将ev_callback调用次数设置为0
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	// 从对应的链表中删除
	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);
	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);
	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		// EVLIST_INSERTED表明是I/O或者Signal事件，
        // 需要调用I/O demultiplexer注销事件
		return (evsel->del(evbase, ev));
	}

	return (0);
}

void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

// 获取最小时间堆中，等待的最小时间
static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	// 如果最小堆为空
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}

	// 获取当前时间
	if (gettime(base, &now) == -1)
		return (-1);

	//如果当前时间大于定时的时间，说明已过时，tv清零返回  
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);
		return (0);
	}

	//定时时间减去当前时间获得tv要等待的时间
	evutil_timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */
//
// 时间校正
// CLOCK_REALTIME:系统实时时间,随系统实时时间改变而改变,即从UTC1970-1-
//                10:0:0开始计时,中间时刻如果系统时间被用户改成其他,则对
//                应的时间相应改变
// CLOCK_MONOTONIC:从系统启动这一刻起开始计时,不受系统时间被用户改变的影响
// CLOCK_PROCESS_CPUTIME_ID:本进程到当前代码系统CPU花费的时间
// CLOCK_THREAD_CPUTIME_ID:本线程到当前代码系统CPU花费的时间
// 如果系统支持monotonic,就不需要校正，系统不支持monotonic,而用户可能会修
// 改系统时间,将时间向前调，这时，就需要校正，由函数timeout_correct来完成
static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	// 如果当前系统使用monotonic，则直接返回
	if (use_monotonic)
		return;
	
	/* Check if time is running backwards */
	// 获取当前时间
	gettime(base, tv);

	//时间不够向前调，将base的event_tv设置为当前时间，返回 
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
		    __func__));

	//计算时间差  
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	// 获取指向时间堆的数组
	pev = base->timeheap.p;
	// 获取时间堆保存的元素个数
	size = base->timeheap.n;
	// 将时间堆中的时间减去上面计算出的时间差  
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	// 保存时间
	base->event_tv = *tv;
}

void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))
		return;

	gettime(base, &now);

	while ((ev = min_heap_top(&base->timeheap))) {
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		event_active(ev, EV_TIMEOUT, 1);
	}
}

// 函数将删除事件ev，对于I/O事件，从I/O 的demultiplexer上将事件注销；
// 对于Signal事件，将从Signal事件链表中删除；对于定时事件，将从堆上删除；
// 同样删除事件的操作则不一定是原子的，比如删除时间事件之后，有可能从系统
// I/O机制中注销会失败。
void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	// 如果当前事件状态不存在
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue); // 输出错误

	// 如果当前事件不是内部事件
	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--; // 事件计数递减

	// 取消事件标记
	ev->ev_flags &= ~queue;

	// 检测事件类型
	switch (queue) {
	// 已注册事件
	case EVLIST_INSERTED:
		// 从事件队列中删除
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		// 跳出
		break;
	// 激活事件
	case EVLIST_ACTIVE:
		// 激活事件递减
		base->event_count_active--;
		// 删除激活事件
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		// 跳出
		break;
	// 超时事件
	case EVLIST_TIMEOUT:
		// 从最小堆中删除
		min_heap_erase(&base->timeheap, ev);
		// 跳出
		break;
	// 默认
	default:
		// 错误输出
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

// 负责将事件插入到对应的链表中
void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	// 如果事件已经在相应的链表中
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		// 如果是激活链表则直接返回
		if (queue & EVLIST_ACTIVE)
			return;
        // 输出警报
		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	// 如果事件并非是内部使用的
	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++; // 增加事件计数
		
	// 记录queue标记
	ev->ev_flags |= queue;

	switch (queue) {
	// 已注册事件，加入到已注册事件链表
	case EVLIST_INSERTED: 
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	// 激活事件，加入激活链表
	case EVLIST_ACTIVE:
		// 自增激活事件计数
		base->event_count_active++;
		// 插入到链表尾端
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	// 超时事件，加入到最小堆
	case EVLIST_TIMEOUT: { // 定时事件，加入堆
		// 插入到最小堆
		min_heap_push(&base->timeheap, ev);
		break;
	}
	// 其他
	default:
		// 输出
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */
const char *
event_get_version(void)
{
	return (VERSION);
}

/* 
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}