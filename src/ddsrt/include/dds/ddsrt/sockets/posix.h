/*
 * Copyright(c) 2006 to 2018 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#ifndef DDSRT_SOCKETS_POSIX_H
#define DDSRT_SOCKETS_POSIX_H

#include <net/if.h>
#include <netinet/in.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef int ddsrt_socket_t;
#define DDSRT_INVALID_SOCKET (-1)
#define PRIdSOCK "d"

#define DDSRT_HAVE_IPV6 1
#define DDSRT_HAVE_DNS  1
#define DDSRT_HAVE_SSM  1

typedef struct iovec ddsrt_iovec_t;
typedef size_t ddsrt_iov_len_t;

#if defined(__linux)
typedef size_t ddsrt_msg_iovlen_t;
#else /* POSIX says int (which macOS, FreeBSD, Solaris do) */
typedef int ddsrt_msg_iovlen_t;
#endif
typedef struct msghdr ddsrt_msghdr_t;

#if defined(__sun) && !defined(_XPG4_2)
# define DDSRT_MSGHDR_FLAGS 0
#else
# define DDSRT_MSGHDR_FLAGS 1
#endif

#if defined(__cplusplus)
}
#endif

#endif /* DDSRT_SOCKETS_POSIX_H */
