/*
 * Copyright (c) 1999 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999 Apple Computer, Inc.  All Rights Reserved.
 * The contents of this file constitute Original Code as defined in and are 
 * subject to the Apple Public Source License Version 1.1 (the "License").  
 * You may not use this file except in compliance with the License.  Please 
 * obtain a copy of the License at http://www.apple.com/publicsource and 
 * read it before using this file.
 * 
 * This Original Code and all software distributed under the License are 
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER 
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, 
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS 
 * FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the License for 
 * the specific language governing rights and limitations under the 
 * License.
 * 
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
	File:		OSThread.cpp

	Contains:	Thread abstraction implementation

	
	
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef __Win32__
	#if __PTHREADS__
		#include <pthread.h>
		#if USE_THR_YIELD
			#include <thread.h>
		#endif
	#else
		#include <mach/mach.h>
		#include <mach/cthreads.h>
	#endif
	#include <unistd.h>
#endif

#include "OSThread.h"
#include "Exception.h"
#include "MyAssert.h"

//
// OSThread.cp
//
void*	OSThread::sMainThreadData = NULL;

#ifdef __Win32__
DWORD	OSThread::sThreadStorageIndex = 0;
#elif __PTHREADS__
pthread_key_t OSThread::gMainKey = 0;
#ifdef _POSIX_THREAD_PRIORITY_SCHEDULING
pthread_attr_t OSThread::sThreadAttr;
#endif
#endif

void OSThread::Initialize()
{
#ifdef __Win32__
	sThreadStorageIndex = ::TlsAlloc();
	Assert(sThreadStorageIndex >= 0);
#elif __PTHREADS__
	pthread_key_create(&OSThread::gMainKey, NULL);
#ifdef _POSIX_THREAD_PRIORITY_SCHEDULING

	//
	// Added for Solaris...
	
	pthread_attr_init(&sThreadAttr);
	/* Indicate we want system scheduling contention scope. This
	   thread is permanently "bound" to an LWP */
	pthread_attr_setscope(&sThreadAttr, PTHREAD_SCOPE_SYSTEM);
#endif

#endif
}

OSThread::OSThread()
: 	fStopRequested(false),
	fRunning(false),
	fCancelThrown(false),
	fDetached(false),
	fJoined(false),
	fThreadData(NULL)
{
}

OSThread::~OSThread()
{
	this->StopAndWaitForThread();
}

void OSThread::Start()
{
#ifdef __Win32__
	unsigned int theId = 0; // We don't care about the identifier
	fThreadID = (HANDLE)_beginthreadex(	NULL, 	// Inherit security
										0,		// Inherit stack size
										_Entry,	// Entry function
										(void*)this,	// Entry arg
										0,		// Begin executing immediately
										&theId );
	Assert(fThreadID != NULL);
#elif __PTHREADS__
	pthread_attr_t* theAttrP;
#ifdef _POSIX_THREAD_PRIORITY_SCHEDULING
	//theAttrP = &sThreadAttr;
	theAttrP = 0;
#else
	theAttrP = NULL;
#endif
	int err = pthread_create((pthread_t*)&fThreadID, theAttrP, _Entry, (void*)this);
	Assert(err == 0);
#else
	fThreadID = (UInt32)cthread_fork((cthread_fn_t)_Entry, (any_t)this);
#endif
}

void OSThread::StopAndWaitForThread()
{
	if (!fRunning)
		return;
		
	fStopRequested = true;
	if (!fJoined && !fDetached)
		Join();
}

void OSThread::Join()
{
	// What we're trying to do is allow the thread we want to delete to complete
	// running. So we wait for it to stop.
	Assert(!fJoined && !fDetached);
	fJoined = true;
#ifdef __Win32__
	DWORD theErr = ::WaitForSingleObject(fThreadID, INFINITE);
	Assert(theErr == WAIT_OBJECT_0);
#elif __PTHREADS__
	void *retVal;
	pthread_join(pthread_self(), &retVal);
#else
	cthread_join((cthread_t)fThreadID);
#endif
}

void OSThread::ThreadYield()
{
	// on platforms who's threading is not pre-emptive yield 
	// to another thread
#if THREADING_IS_COOPERATIVE
	#if __PTHREADS__
		#if USE_THR_YIELD
			thr_yield();
		#else
			sched_yield();
		#endif
	#endif
#endif
}

void OSThread::Sleep(UInt32 inMsec)
{
#ifdef __Win32__
	::Sleep(inMsec);
#else
	::usleep(inMsec * 1000);
#endif
}


void OSThread::Detach()
{
	Assert(!fDetached && !fJoined);
	fDetached = true;
}

void OSThread::CheckForStopRequest()
{
	if (fStopRequested && !fCancelThrown)
		ThrowStopRequest();
}


void OSThread::ThrowStopRequest()
{
	if (fStopRequested && !fCancelThrown) {
		fCancelThrown = true;
		Throw_(Cancel_E);
	}
}

void OSThread::CallEntry(OSThread* thread) 	// static method
{
	thread->fRunning = true;

	try
	{
		thread->Entry();
	} 
	catch(...)
	{
	}

	thread->fRunning = false;
	
	if (thread->fDetached) {
		Assert(!thread->fJoined);
#ifdef __Win32__
		// FIX: What to do here?
#elif __PTHREADS__
		pthread_detach((pthread_t)thread->fThreadID);
#else
		cthread_detach((cthread_t)thread->fThreadID);
#endif
		delete thread;
	}
}

#ifdef __Win32__
unsigned int WINAPI OSThread::_Entry(LPVOID inThread)
#else
void* OSThread::_Entry(void *inThread)  //static
#endif
{
	OSThread* theThread = (OSThread*)inThread;
#ifdef __Win32__
	BOOL theErr = ::TlsSetValue(sThreadStorageIndex, theThread);
	Assert(theErr == TRUE);
#elif __PTHREADS__
	theThread->fThreadID = (UInt32)pthread_self();
	pthread_setspecific(OSThread::gMainKey, theThread);
#else
	theThread->fThreadID = (UInt32)cthread_self();
	cthread_set_data(cthread_self(), (any_t)theThread);
#endif
	OSThread::CallEntry(theThread);
	return NULL;
}

OSThread*	OSThread::GetCurrent()
{
#ifdef __Win32__
	return (OSThread *)::TlsGetValue(sThreadStorageIndex);
#elif __PTHREADS__
	return (OSThread *)pthread_getspecific(OSThread::gMainKey);
#else
	return (OSThread*)cthread_data(cthread_self());
#endif
}

#ifdef __Win32__
int	OSThread::GetErrno()
{
	int winErr = ::GetLastError();

	
	// Convert to a POSIX errorcode. The *major* assumption is that
	// the meaning of these codes is 1-1 and each Winsock, etc, etc
	// function is equivalent in errors to the POSIX standard. This is 
	// a big assumption, but the server only checks for a small subset of
	// the real errors, on only a small number of functions, so this is probably ok.
	switch (winErr)
	{

		case ERROR_FILE_NOT_FOUND: return ENOENT;

		case ERROR_PATH_NOT_FOUND: return ENOENT;		




		case WSAEINTR:		return EINTR;
		case WSAENETRESET:	return EPIPE;
		case WSAENOTCONN:	return ENOTCONN;
		case WSAEWOULDBLOCK:return EAGAIN;
		case WSAECONNRESET:	return EPIPE;
		case WSAEADDRINUSE:	return EADDRINUSE;
		case WSAEMFILE:		return EMFILE;
		case WSAEINPROGRESS:return EINPROGRESS;
		case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
		case WSAECONNABORTED: return EPIPE;
		case 0:				return 0;
		
		default: 			return ENOTCONN;
	}
}
#endif