/*
 *
 * RTEMS Project (https://www.rtems.org/)
 *
 * Copyright (c) 2021 Vijay Kumar Banerjee <vijay@rtems.org>.
 * All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdarg.h>
/* #include <stdlib.h> */
#include <stdio.h>
#include <errno.h>

#include <rtems.h>
#include <rtems/libio_.h>
#include <rtems/error.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/sysctl.h>

#include <rtems/thread.h>

#include <lwip/tcpip.h>
#include <lwip/api.h>
#include <lwip/netbuf.h>
#include <lwip/netdb.h>
#include <lwip/netifapi.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <sys/errno.h>

#include <lwip/init.h>
#include "lwip/err.h"
#include "lwip/tcp.h"
#include <netif/etharp.h>

#include <string.h>

#include "lwip/tcpip.h"
//#include "arch/eth_lwip.h"
#include "lwip/api.h"
#include "lwip/netbuf.h"
#include "lwip/netdb.h"
#include "lwip/netifapi.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

static const rtems_filesystem_file_handlers_r rtems_lwip_socket_handlers;

static rtems_recursive_mutex rtems_lwip_mutex =
  RTEMS_RECURSIVE_MUTEX_INITIALIZER( "_LWIP" );

void rtems_lwip_semaphore_obtain( void )
{
    rtems_recursive_mutex_lock( &rtems_lwip_mutex );
}

/*
 * Release network mutex
 */
void rtems_lwip_semaphore_release( void )
{
    rtems_recursive_mutex_unlock( &rtems_lwip_mutex );
}

static inline int rtems_lwip_iop_to_lwipfd( rtems_libio_t *iop )
{
  if ( iop == NULL ) {
    errno = EBADF;

    return -1;
  }

/*
   if ((rtems_libio_iop_flags(iop) & LIBIO_FLAGS_OPEN) == 0) {
    errno = EBADF;
    return -1;
   }
 */

  if ( iop->pathinfo.handlers != &rtems_lwip_socket_handlers ) {
    errno = ENOTSOCK;

    return -1;
  }

  return iop->data0;
}

/*
 * Convert an RTEMS file descriptor to a LWIP socket pointer.
 */
int rtems_lwip_sysfd_to_lwipfd( int fd )
{
  if ( (uint32_t) fd >= rtems_libio_number_iops ) {
    errno = EBADF;

    return -1;
  }

  return rtems_lwip_iop_to_lwipfd( rtems_libio_iop( fd ) );
}

/*
 * Create an RTEMS file descriptor for a socket
 */
static int rtems_lwip_make_sysfd_from_lwipfd( int lfwipfd )
{
  rtems_libio_t *iop;
  int            fd;

  iop = rtems_libio_allocate();

  if ( iop == 0 )
    rtems_set_errno_and_return_minus_one( ENFILE );

  fd = rtems_libio_iop_to_descriptor( iop );
  iop->data0 = lfwipfd;
  iop->data1 = NULL;
  iop->pathinfo.handlers = &rtems_lwip_socket_handlers;
  iop->pathinfo.mt_entry = &rtems_filesystem_null_mt_entry;
  rtems_filesystem_location_add_to_mt_entry( &iop->pathinfo );
  rtems_libio_iop_flags_set( iop, LIBIO_FLAGS_READ_WRITE | LIBIO_FLAGS_OPEN );

  return fd;
}

/*
 *********************************************************************
 *                       BSD-style entry points                      *
 *********************************************************************
 */
int socket(
  int domain,
  int type,
  int protocol
)
{
  int fd;
  int lwipfd;

  rtems_lwip_semaphore_obtain();

  lwipfd = lwip_socket( domain, type, 0 );

  if ( lwipfd < 0 ) {
    return -1;
  }

  fd = rtems_lwip_make_sysfd_from_lwipfd( lwipfd );

  if ( fd < 0 ) {
    lwip_close( lwipfd );
  }

  rtems_lwip_semaphore_release();

  return fd;
}

static int setup_socketpair(int listener, int *socket_vector)
{
  union {
    struct sockaddr addr;
    struct sockaddr_in inaddr;
  } a;
  int reuse = 1;
  socklen_t addrlen = sizeof(a.inaddr);

  memset(&a, 0, sizeof(a));
  a.inaddr.sin_family = AF_INET;
  a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.inaddr.sin_port = 0;

  if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
       (char*) &reuse, (socklen_t) sizeof(reuse)) == -1) {
    return 1;
  }

  if  (bind(listener, &a.addr, sizeof(a.inaddr)) == -1) {
    return 1;
  }

  memset(&a, 0, sizeof(a));
  if  (getsockname(listener, &a.addr, &addrlen) == -1) {
    return 1;
  }

  a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.inaddr.sin_family = AF_INET;

  if (listen(listener, 1) == -1) {
    return 1;
  }

  socket_vector[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_vector[0] == -1) {
    return 1;
  }

  if (connect(socket_vector[0], &a.addr, sizeof(a.inaddr)) == -1) {
    return 1;
  }

  socket_vector[1] = accept(listener, NULL, NULL);
  if (socket_vector[1] == -1) {
    return 1;
  }

  close(listener);
  return 0;
}

/* Fake socketpair() support with a loopback TCP socket */
int
socketpair(int domain, int type, int protocol, int *socket_vector)
{
  int listener;
  int saved_errno;

  if (socket_vector == NULL) {
    errno = EINVAL;
    return -1;
  }
  socket_vector[0] = socket_vector[1] = -1;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == -1)
    return -1;

  if (setup_socketpair(listener, socket_vector) == 0) {
    return 0;
  }

  saved_errno = errno;
  close(listener);
  close(socket_vector[0]);
  close(socket_vector[1]);
  errno = saved_errno;
  socket_vector[0] = socket_vector[1] = -1;
  return -1;
}

int bind(
  int                    s,
  const struct sockaddr *name,
  socklen_t              namelen
)
{
  int                lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_bind( lwipfd, name, namelen );
}

int connect(
  int                    s,
  const struct sockaddr *name,
  socklen_t              namelen
)
{
  int                lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_connect( lwipfd, name, namelen );
}

int listen(
  int s,
  int backlog
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_listen( lwipfd, backlog );
}

int accept(
  int              s,
  struct sockaddr *name,
  socklen_t       *namelen
)
{
  int                ret = -1;
  int                lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  lwipfd = lwip_accept( lwipfd, name, namelen );

  rtems_lwip_semaphore_obtain();
  ret = rtems_lwip_make_sysfd_from_lwipfd( lwipfd );
  rtems_lwip_semaphore_release();

  if ( ret < 0 ) {
    lwip_close( lwipfd );
  }

  return ret;
}

/*
 *  Shutdown routine
 */

int shutdown(
  int s,
  int how
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_shutdown( lwipfd, how );
}

ssize_t recv(
  int    s,
  void  *buf,
  size_t len,
  int    flags
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_recv( lwipfd, buf, len, flags );
}

ssize_t send(
  int         s,
  const void *buf,
  size_t      len,
  int         flags
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_send( lwipfd, buf, len, flags );
}

ssize_t recvfrom(
  int              s,
  void            *buf,
  size_t           len,
  int              flags,
  struct sockaddr *name,
  socklen_t       *namelen
)
{
  int                lwipfd;

  if ( name == NULL )
    return recv( s, buf, len, flags );

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_recvfrom( lwipfd, buf, len, flags, name, namelen );
}

ssize_t sendto(
  int                    s,
  const void            *buf,
  size_t                 len,
  int                    flags,
  const struct sockaddr *name,
  socklen_t              namelen
)
{
  int                lwipfd;

  if ( name == NULL )
    return send( s, buf, len, flags );

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_sendto( lwipfd, buf, len, flags, name, namelen );
}

static int fdset_sysfd_to_lwipfd( int maxfdp1, fd_set *orig_set, fd_set *mapped_set )
{
  int new_max = 0;
  FD_ZERO( mapped_set );

  if ( orig_set == NULL ) {
    return new_max;
  }

  for ( int sysfd = 0; sysfd < maxfdp1; sysfd++ ) {
    int lwipfd;

    if ( FD_ISSET( sysfd, orig_set ) == 0 ) {
      continue;
    }

    lwipfd = rtems_lwip_sysfd_to_lwipfd( sysfd );
    if ( lwipfd < 0 ) {
      return -1;
    }

    if ( lwipfd > (new_max - 1) ) {
        new_max = lwipfd + 1;
    }

    FD_SET( lwipfd, mapped_set );
  }
  return new_max;
}

static void fdset_lwipfd_to_sysfd( int maxfdp1, fd_set *orig_set, fd_set *mapped_set )
{
  fd_set new_orig_set;

  FD_ZERO( &new_orig_set );

  if ( orig_set == NULL ) {
    return;
  }

  for ( int sysfd = 0; sysfd < maxfdp1; sysfd++ ) {
    int lwipfd;

    if ( FD_ISSET( sysfd, orig_set ) == 0 ) {
      continue;
    }

    lwipfd = rtems_lwip_sysfd_to_lwipfd( sysfd );

    if ( FD_ISSET( lwipfd, mapped_set ) == 0 ) {
      continue;
    }

    FD_SET( sysfd, &new_orig_set );
  }

  *orig_set = new_orig_set;
}

int select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset,
                struct timeval *timeout)
{
  int rmaxp1;
  int wmaxp1;
  int emaxp1;
  int newmaxfdp1;
  int ret;
  fd_set newread, newwrite, newexcept;

  /* Save original FD sets,  */

  rtems_lwip_semaphore_obtain();
  rmaxp1 = fdset_sysfd_to_lwipfd( maxfdp1, readset, &newread );
  wmaxp1 = fdset_sysfd_to_lwipfd( maxfdp1, writeset, &newwrite );
  emaxp1 = fdset_sysfd_to_lwipfd( maxfdp1, exceptset, &newexcept );
  rtems_lwip_semaphore_release();

  if ( rmaxp1 < 0 || wmaxp1 < 0 || emaxp1 < 0 ) {
    errno = ENOSYS;
    return -1;
  }

  newmaxfdp1 = rmaxp1;
  if ( wmaxp1 > newmaxfdp1 ) {
    newmaxfdp1 = wmaxp1;
  }
  if ( emaxp1 > newmaxfdp1 ) {
    newmaxfdp1 = emaxp1;
  }

  ret = lwip_select( newmaxfdp1, &newread, &newwrite, &newexcept, timeout );

  rtems_lwip_semaphore_obtain();
  fdset_lwipfd_to_sysfd( maxfdp1, readset, &newread );
  fdset_lwipfd_to_sysfd( maxfdp1, writeset, &newwrite );
  fdset_lwipfd_to_sysfd( maxfdp1, exceptset, &newexcept );
  rtems_lwip_semaphore_release();

  return ret;
}

#if 0

/*
 * All `transmit' operations end up calling this routine.
 */
ssize_t sendmsg(
  int                  s,
  const struct msghdr *mp,
  int                  flags
)
{
  int           ret = -1;
  int           error;
  struct uio    auio;
  struct iovec *iov;
  int           lwipfd;
  struct mbuf  *to;
  struct mbuf  *control = NULL;
  int           i;
  int           len;

  rtems_lwip_semaphore_obtain();

  if ( ( so = rtems_lwip_sysfd_to_lwipfd( s ) ) == NULL ) {
    rtems_lwip_semaphore_release();

    return -1;
  }

  auio.uio_iov = mp->msg_iov;
  auio.uio_iovcnt = mp->msg_iovlen;
  auio.uio_segflg = UIO_USERSPACE;
  auio.uio_rw = UIO_WRITE;
  auio.uio_offset = 0;
  auio.uio_resid = 0;
  iov = mp->msg_iov;

  for ( i = 0; i < mp->msg_iovlen; i++, iov++ ) {
    if ( ( auio.uio_resid += iov->iov_len ) < 0 ) {
      errno = EINVAL;
      rtems_lwip_semaphore_release();

      return -1;
    }
  }

  if ( mp->msg_name ) {
    error = sockargstombuf( &to, mp->msg_name, mp->msg_namelen, MT_SONAME );

    if ( error ) {
      errno = error;
      rtems_lwip_semaphore_release();

      return -1;
    }
  } else {
    to = NULL;
  }

  if ( mp->msg_control ) {
    if ( mp->msg_controllen < sizeof( struct cmsghdr ) ) {
      errno = EINVAL;

      if ( to )
        m_freem( to );

      rtems_lwip_semaphore_release();

      return -1;
    }

    sockargstombuf( &control, mp->msg_control, mp->msg_controllen,
      MT_CONTROL );
  } else {
    control = NULL;
  }

  len = auio.uio_resid;
  error = sosend( so, to, &auio, (struct mbuf *) 0, control, flags );

  if ( error ) {
    if ( auio.uio_resid != len && ( error == EINTR || error == EWOULDBLOCK ) )
      error = 0;
  }

  if ( error )
    errno = error;
  else
    ret = len - auio.uio_resid;

  if ( to )
    m_freem( to );

  rtems_lwip_semaphore_release();

  return ( ret );
}

#endif

/*
 * All `receive' operations end up calling this routine.
 */
ssize_t recvmsg(
  int            s,
  struct msghdr *mp,
  int            flags
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_recvmsg( lwipfd, mp, flags );
}

int setsockopt(
  int         s,
  int         level,
  int         name,
  const void *val,
  socklen_t   len
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_setsockopt( lwipfd, level, name, val, len );
}

int getsockopt(
  int        s,
  int        level,
  int        name,
  void      *aval,
  socklen_t *avalsize
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_getsockopt( lwipfd, level, name, aval, avalsize );
}

#if 0

static int getpeersockname(
  int              s,
  struct sockaddr *name,
  socklen_t       *namelen,
  int              pflag
)
{
  int          lwipfd;
  struct mbuf *m;
  int          len = *namelen;
  int          error;

  rtems_lwip_semaphore_obtain();

  if ( ( so = rtems_lwip_sysfd_to_lwipfd( s ) ) == NULL ) {
    rtems_lwip_semaphore_release();

    return -1;
  }

  m = m_getclr( M_WAIT, MT_SONAME );

  if ( m == NULL ) {
    errno = ENOBUFS;
    rtems_lwip_semaphore_release();

    return -1;
  }

  if ( pflag )
    error = ( *so->so_proto->pr_usrreqs->pru_peeraddr )( so, m );
  else
    error = ( *so->so_proto->pr_usrreqs->pru_sockaddr )( so, m );

  if ( error ) {
    m_freem( m );
    errno = error;
    rtems_lwip_semaphore_release();

    return -1;
  }

  if ( len > m->m_len ) {
    len = m->m_len;
    *namelen = len;
  }

  memcpy( name, mtod( m, caddr_t ), len );
  m_freem( m );
  rtems_lwip_semaphore_release();

  return 0;
}

#endif

int getpeername(
  int              s,
  struct sockaddr *name,
  socklen_t       *namelen
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_getpeername(lwipfd, name, namelen);
}

int getsockname(
  int              s,
  struct sockaddr *name,
  socklen_t       *namelen
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_sysfd_to_lwipfd( s );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_getsockname( lwipfd, name, namelen );
}

#if 0

int sysctl(
  const int  *name,
  u_int       namelen,
  void       *oldp,
  size_t     *oldlenp,
  const void *newp,
  size_t      newlen
)
{
  int    error;
  size_t j;

  rtems_lwip_semaphore_obtain();
  error =
    userland_sysctl( 0, name, namelen, oldp, oldlenp, 1, newp, newlen, &j );
  rtems_lwip_semaphore_release();

  if ( oldlenp )
    *oldlenp = j;

  if ( error ) {
    errno = error;

    return -1;
  }

  return 0;
}

#endif

/*
 ************************************************************************
 *                      RTEMS I/O HANDLER ROUTINES                      *
 ************************************************************************
 */
static int rtems_lwip_close( rtems_libio_t *iop )
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_iop_to_lwipfd( iop );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_close( lwipfd );
}

static ssize_t rtems_lwip_read(
  rtems_libio_t *iop,
  void          *buffer,
  size_t         count
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_iop_to_lwipfd( iop );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_read( lwipfd, buffer, count );
}

static ssize_t rtems_lwip_write(
  rtems_libio_t *iop,
  const void    *buffer,
  size_t         count
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_iop_to_lwipfd( iop );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  return lwip_write( lwipfd, buffer, count );
}

int so_ioctl(
  rtems_libio_t *iop,
  int            lwipfd,
  uint32_t       command,
  void          *buffer
)
{
#if 0

  switch ( command ) {
    case FIONBIO:

      if ( *(int *) buffer ) {
        iop->flags |= O_NONBLOCK;
        so->so_state |= SS_NBIO;
      } else {
        iop->flags &= ~O_NONBLOCK;
        so->so_state &= ~SS_NBIO;
      }

      return 0;

    case FIONREAD:
      *(int *) buffer = so->so_rcv.sb_cc;

      return 0;
  }

  if ( IOCGROUP( command ) == 'i' )
    return ifioctl( so, command, buffer, NULL );

  if ( IOCGROUP( command ) == 'r' )
    return rtioctl( command, buffer, NULL );

  return ( *so->so_proto->pr_usrreqs->pru_control )( so, command, buffer, 0 );
#endif

  return -1;
}

static int rtems_lwip_ioctl(
  rtems_libio_t  *iop,
  ioctl_command_t command,
  void           *buffer
)
{
#if 0
  int lwipfd;
  int error;

  rtems_lwip_semaphore_obtain();

  if ( ( so = iop->data1 ) == NULL ) {
    errno = EBADF;
    rtems_lwip_semaphore_release();

    return -1;
  }

  error = so_ioctl( iop, so, command, buffer );
  rtems_lwip_semaphore_release();

  if ( error ) {
    errno = error;

    return -1;
  }

  return 0;
#endif

  return -1;
}

static int rtems_lwip_fcntl(
  rtems_libio_t *iop,
  int            cmd
)
{
  int lwipfd;

  rtems_lwip_semaphore_obtain();
  lwipfd = rtems_lwip_iop_to_lwipfd( iop );
  rtems_lwip_semaphore_release();

  if ( lwipfd < 0 ) {
    return -1;
  }

  /*
   * Returning non-zero here for F_GETFL results in the call failing instead of
   * passing back the value retrieved in the libio layer.
   */
  if (cmd == F_GETFL) {
    return 0;
  }

  return lwip_fcntl( lwipfd, cmd, O_NONBLOCK );
}

static int rtems_lwip_fstat(
  const rtems_filesystem_location_info_t *loc,
  struct stat                            *sp
)
{
  sp->st_mode = S_IFSOCK;

  return 0;
}

static const rtems_filesystem_file_handlers_r rtems_lwip_socket_handlers = {
  .open_h = rtems_filesystem_default_open,
  .close_h = rtems_lwip_close,
  .read_h = rtems_lwip_read,
  .write_h = rtems_lwip_write,
  .ioctl_h = rtems_lwip_ioctl,
  .lseek_h = rtems_filesystem_default_lseek,
  .fstat_h = rtems_lwip_fstat,
  .ftruncate_h = rtems_filesystem_default_ftruncate,
  .fsync_h = rtems_filesystem_default_fsync_or_fdatasync,
  .fdatasync_h = rtems_filesystem_default_fsync_or_fdatasync,
  .fcntl_h = rtems_lwip_fcntl,
  .kqfilter_h = rtems_filesystem_default_kqfilter,
  .mmap_h = rtems_filesystem_default_mmap,
  .poll_h = rtems_filesystem_default_poll,
  .readv_h = rtems_filesystem_default_readv,
  .writev_h = rtems_filesystem_default_writev
};

const char *
inet_ntop(int af, const void *src, char *dst, socklen_t size){
	    return lwip_inet_ntop(af, src, dst, size);
}

int inet_pton(int af, const char *src, void *dst)
{
  return lwip_inet_pton(af, src, dst);
}
