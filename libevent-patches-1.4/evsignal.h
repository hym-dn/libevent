/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
 */
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

// Libevent中Signal事件的管理是通过结构体
// evsignal_info完成的，结构体位于
// evsignal.h文件中。
struct evsignal_info {
	// 为socket pair的读socket向event_base
	// 注册读事件时使用的event结构体
	struct event ev_signal; // 信号整体作为一个事件加入epoll
	// socket pair对
	int ev_signal_pair[2]; // 套接子对
	// 记录ev_signal事件是否已经注册了
	int ev_signal_added; // 是否已经加入epoll
    // 是否有信号发生的标记；是volatile类型，因为
	// 它会在另外的线程中被修改；
	volatile sig_atomic_t evsignal_caught; // 是否有信号到来
	// 数组，evsigevents[signo]表示注册到信号
	// signo的事件链表
	// evsigevents 数组存放信号注册的事件，当发生该信号的时候，激活
	// 该信号下注册的信号事件。数组下标代表信号。
	struct event_list evsigevents[NSIG]; // 信号链表
	// 具体记录每个信号触发的次数，evsigcaught[
	// signo]是记录信号signo被触发的次数；
	// evsigcaught 记录信号发生的次数，上图中，
	// 信号1发生了2次，而信号4发生了4次。信号事
	// 件在未被激活前，该信号发生了4次，这样，在
	// 进行事件处理时，event3、event4的回调函数
	// 会分别被执行4次。这样，防止信号丢失。
	sig_atomic_t evsigcaught[NSIG];
	// sh_old记录了原来的signal处理函数指针，当信号
	// signo注册的event被清空时，需要重新设置其处理
	// 函数；
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;
#else
	ev_sighandler_t **sh_old;
#endif
    // 表示数字sh_old的大小。而，sh_old 为
	// sigaction类型数组。
	int sh_old_max; // 原信号 数组大小
	/**
	 * evsignal_info的初始化包括，创建socket pair，
	 * 设置ev_signal事件（但并没有注册，而是等到有信号
	 * 注册时才检查并注册），并将所有标记置零，初始化信号
	 * 的注册事件链表指针等。
	 */
};

int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
