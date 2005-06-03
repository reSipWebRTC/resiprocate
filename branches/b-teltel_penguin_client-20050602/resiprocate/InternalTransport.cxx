#if defined(HAVE_CONFIG_H)
#include "resiprocate/config.hxx"
#endif

#include <iostream>

#if defined(HAVE_SYS_SOCKIO_H)
#include <sys/sockio.h>
#endif

#include "resiprocate/Helper.hxx"
#include "resiprocate/InternalTransport.hxx"
#include "resiprocate/SipMessage.hxx"
#include "resiprocate/TransportMessage.hxx"
#include "resiprocate/os/DnsUtil.hxx"
#include "resiprocate/os/Logger.hxx"
#include "resiprocate/os/Socket.hxx"
#include "resiprocate/os/compat.hxx"
#include "resiprocate/os/WinLeakCheck.hxx"

using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM Subsystem::TRANSPORT


InternalTransport::InternalTransport(Fifo<TransactionMessage>& rxFifo, 
                                     int portNum, 
                                     IpVersion version,
                                     const Data& interfaceObj) :
   Transport(rxFifo, portNum, version, interfaceObj),
   mFd(-1),
   mHasOwnThread(false)
{
}

InternalTransport::~InternalTransport()
{
   if (mFd != -1)
   {
      //DebugLog (<< "Closing " << mFd);
      closeSocket(mFd);
   }
   mFd = -2;
}

bool
InternalTransport::isFinished() const
{
   return !mTxFifo.messageAvailable();
}

Socket
InternalTransport::socket(TransportType type, IpVersion ipVer)
{
   Socket fd;
   switch (type)
   {
      case UDP:
#ifdef USE_IPV6
         fd = ::socket(ipVer == V4 ? PF_INET : PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
#else
         fd = ::socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
         break;
      case TCP:
      case TLS:
#ifdef USE_IPV6
         fd = ::socket(ipVer == V4 ? PF_INET : PF_INET6, SOCK_STREAM, 0);
#else
         fd = ::socket(PF_INET, SOCK_STREAM, 0);
#endif
         break;
      default:
         InfoLog (<< "Try to create an unsupported socket type: " << Tuple::toData(type));
         assert(0);
         throw Transport::Exception("Unsupported transport", __FILE__,__LINE__);
   }
   
   if ( fd == INVALID_SOCKET )
   {
      int e = getErrno();
      InfoLog (<< "Failed to create socket: " << strerror(e));
      throw Transport::Exception("Can't create TcpBaseTransport", __FILE__,__LINE__);
   }

   DebugLog (<< "Creating fd=" << fd << (ipVer == V4 ? " V4/" : " V6/") << (type == UDP ? "UDP" : "TCP"));
   
   return fd;
}

void 
InternalTransport::bind()
{
   DebugLog (<< "Binding to " << DnsUtil::inet_ntop(mTuple));
   
   if ( ::bind( mFd, &mTuple.getMutableSockaddr(), mTuple.length()) == SOCKET_ERROR )
   {
      int e = getErrno();
      if ( e == EADDRINUSE )
      {
         error(e);
         ErrLog (<< mTuple << " already in use ");
         throw Transport::Exception("port already in use", __FILE__,__LINE__);
      }
      else
      {
         error(e);
         ErrLog (<< "Could not bind to " << mTuple);
         throw Transport::Exception("Could not use port", __FILE__,__LINE__);
      }
   }
   
   bool ok = makeSocketNonBlocking(mFd);
   if ( !ok )
   {
      ErrLog (<< "Could not make socket non-blocking " << port());
      throw Transport::Exception("Failed making socket non-blocking", __FILE__,__LINE__);
   }
}

unsigned int 
InternalTransport::getFifoSize() const
{
   return mTxFifo.size();
}

void
InternalTransport::thread()
{
   InfoLog (<< "Starting transport thread for " << mTuple);
#if defined(USE_EPOLL)
   int epollfd = ::open("/dev/epoll", O_RDWR);
   if (epollfd < 0)
   {
      int e = getErrno();
      Transport::error( e );
      ErrLog (<< "Can't find epoll on this system");
      assert(0);
   }
   
   int ret = ::ioctl(epollfd, EP_ALLOC, maxFileDescriptors());
   if (ret != 0)
   {
      int e = getErrno();
      Transport::error( e );
      ErrLog (<< "Failed to define for " << maxFileDescriptors() << " descriptors");
      assert(0);
   }

   char *map = (char *)mmap(NULL, EP_MAP_SIZE(maxFileDescriptors(), 
                                              PROT_READ | PROT_WRITE, MAP_PRIVATE, n
                                              epollfd, 0));
   if (map <= 0)
   {
      ErrLog (<< "Failed to allocate space for epoll in kernel for " << maxFileDescriptors() << " descriptors");
      assert(0);
   }
   
   while (!mShutdown)
   {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLIN | POLLOUT | POLLERR | POLLHUP;
      pfd.revents = 0;
      
      if (write(kdpfd, &pfd, sizeof(pfd)) != sizeof(pfd)) 
      {
         
         /* report error */
      }
      



      FdSet fdset; 
      buildFdSet(fdset);
      int  err = fdset.selectMilliSeconds(100);
      if (err >= 0)
      {
         try
         {
            process(fdset);
         }
         catch (BaseException& e)
         {
            InfoLog (<< "Uncaught exception: " << e);
         }
      }
   }
#else // !USE_EPOLL
   while (!mShutdown)
   {
      FdSet fdset; 
      buildFdSet(fdset);
      int  err = fdset.selectMilliSeconds(100);
      if (err >= 0)
      {
         try
         {
            process(fdset);
         }
         catch (BaseException& e)
         {
            InfoLog (<< "Uncaught exception: " << e);
         }
      }
   }
#endif
   InfoLog (<< "shutdown: ");
}

bool 
InternalTransport::hasDataToSend() const
{
   return mTxFifo.messageAvailable();
}

void 
InternalTransport::transmit(const Tuple& dest, const Data& pdata, const Data& tid)
{
   SendData* data = new SendData(dest, pdata, tid);
   mTxFifo.add(data);
}

/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 */
