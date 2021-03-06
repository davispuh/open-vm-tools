/*********************************************************
 * Copyright (C) 2013-2015 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * simpleSocket.c --
 *
 *    Simple wrappers for socket.
 *
 */

#include <stdlib.h>
#if defined(linux)
#include <arpa/inet.h>
#endif

#include "simpleSocket.h"
#include "vmci_defs.h"
#include "vmci_sockets.h"
#include "dataMap.h"
#include "err.h"
#include "debug.h"

#define LGPFX "SimpleSock: "


static int
SocketGetLastError(void);

/*
 *-----------------------------------------------------------------------------
 *
 * SocketStartup --
 *
 *      Win32 special socket init.
 *
 * Results:
 *      TRUE on success
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
SocketStartup(void)
{
#if defined(_WIN32)
   int err;
   WSADATA wsaData;

   err = WSAStartup(MAKEWORD(2, 0), &wsaData);
   if (err) {
      Warning(LGPFX "Error in WSAStartup: %d[%s]\n", err,
              Err_Errno2String(err));
      return FALSE;
   }

   if (2 != LOBYTE(wsaData.wVersion) || 0 != HIBYTE(wsaData.wVersion)) {
      Warning(LGPFX "Unsupported Winsock version %d.%d\n",
              LOBYTE(wsaData.wVersion), HIBYTE(wsaData.wVersion));
      return FALSE;
   }
#endif

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SocketCleanup --
 *
 *      Win32 special socket cleanup.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
SocketCleanup(void)
{
#if defined(_WIN32)
   int err = WSACleanup();
   if (err) {
      Warning(LGPFX "Error in WSACleanup: %d[%s]\n", err,
              Err_Errno2String(err));
      return FALSE;
   }
#endif
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Socket_Close --
 *
 *      wrapper for socket close.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
Socket_Close(SOCKET sock)
{
   int res;

#if defined(_WIN32)
   res = closesocket(sock);
#else
   res = close(sock);
#endif

   if (res == SOCKET_ERROR) {
      int err = SocketGetLastError();
      Warning(LGPFX "Error in closing socket %d: %d[%s]\n",
              sock, err, Err_Errno2String(err));
   }

   SocketCleanup();
}


/*
 *-----------------------------------------------------------------------------
 *
 * SocketGetLastError --
 *
 *      Get the last error code.
 *
 * Results:
 *      error code.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int
SocketGetLastError(void)
{
#if defined(_WIN32)
   return WSAGetLastError();
#else
   return errno;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Socket_Recv --
 *
 *      Block until given number of bytes of data is received or error occurs.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
Socket_Recv(SOCKET fd,      // IN
            char *buf,      // OUT
            int len)        // IN
{
   int remaining = len;
   int rv;
   int sysErr;

   while (remaining > 0) {
      rv = recv(fd, buf , remaining, 0);
      if (rv == 0) {
         Warning(LGPFX "Socket %d closed by peer.", fd);
         return FALSE;
      }
      if (rv == SOCKET_ERROR) {
         sysErr = SocketGetLastError();
         if (sysErr == SYSERR_EINTR) {
            continue;
         }
         Warning(LGPFX "Recv error for socket %d: %d[%s]", fd, sysErr,
                 Err_Errno2String(sysErr));
         return FALSE;
      }
      remaining -= rv;
      buf += rv;
   }

   Debug(LGPFX "Recved %d bytes from socket %d\n", len, fd);
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Socket_Send --
 *
 *      Block until the given number of bytes of data is sent or error occurs.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

gboolean
Socket_Send(SOCKET fd,      // IN
            char *buf,      // IN
            int len)        // IN
{
   int left = len;
   int sent = 0;
   int rv;
   int sysErr;

   while (left > 0) {
      rv = send(fd, buf + sent, left, 0);
      if (rv == SOCKET_ERROR) {
         sysErr = SocketGetLastError();
         if (sysErr == SYSERR_EINTR) {
            continue;
         }
         Warning(LGPFX "Send error for socket %d: %d[%s]", fd, sysErr,
                 Err_Errno2String(sysErr));
         return FALSE;
      }
      left -= rv;
      sent += rv;
   }

   Debug(LGPFX "Sent %d bytes from socket %d\n", len, fd);
   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * Socket_ConnectVMCI --
 *
 *      Connect to VMCI port in blocking mode.
 *      If isPriv is true, we will try to bind the local port to a port that
 *      is less than 1024.
 *
 * Results:
 *      returns the raw socket on sucess, otherwise INVALID_SOCKET;
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

SOCKET
Socket_ConnectVMCI(unsigned int cid,                  // IN
                   unsigned int port,                 // IN
                   gboolean isPriv,                   // IN
                   SockConnError *outError)           // OUT
{
   struct sockaddr_vm addr;
   SOCKET fd;
   SockConnError error = SOCKERR_GENERIC;
   int sysErr;
   socklen_t addrLen = sizeof addr;
   int vsockDev = -1;
   int family = VMCISock_GetAFValueFd(&vsockDev);

   if (outError) {
      *outError = SOCKERR_SUCCESS;
   }

   if (!SocketStartup()) {
      goto error;
   }

   if (family == -1) {
      Warning(LGPFX "Couldn't get VMCI socket family info.");
      goto error;
   }

   memset((char *)&addr, 0, sizeof addr);
   addr.svm_family = family;
   addr.svm_cid = cid;
   addr.svm_port = port;

   Debug(LGPFX "creating new socket, connecting to %u:%u\n", cid, port);

   fd = socket(addr.svm_family, SOCK_STREAM, 0);
   if (fd == INVALID_SOCKET) {
      sysErr = SocketGetLastError();
      Warning(LGPFX "failed to create socket, error %d: %s\n",
              sysErr, Err_Errno2String(sysErr));
      goto error;
   }

   if (isPriv) {
      struct sockaddr_vm localAddr;
      gboolean bindOk = FALSE;
      int localPort;

      memset(&localAddr, 0, sizeof localAddr);
      localAddr.svm_family = addr.svm_family;
      localAddr.svm_cid = VMCISock_GetLocalCID();

      /* Try to bind to port 1~1023 for a privileged user. */
      for (localPort = PRIVILEGED_PORT_MAX;
           localPort >= PRIVILEGED_PORT_MIN; localPort--) {

         localAddr.svm_port = localPort;

         if (bind(fd, (struct sockaddr *)&localAddr, sizeof localAddr) != 0) {
            sysErr = SocketGetLastError();
            if (sysErr == SYSERR_EACCESS) {
               Debug(LGPFX "Couldn't bind to privileged port for "
                     "socket %d\n", fd);
               error = SOCKERR_EACCESS;
               Socket_Close(fd);
               goto error;
            }
            if (sysErr == SYSERR_EADDRINUSE) {
               continue;
            }
            Warning(LGPFX "could not bind socket, error %d: %s\n", sysErr,
                    Err_Errno2String(sysErr));
            Socket_Close(fd);
            error = SOCKERR_BIND;
            goto error;
         } else {
            bindOk = TRUE;
            break;
         }
      }

      if (!bindOk) {
         Debug(LGPFX "Failed to bind to privileged port for socket %d, "
               "no port available\n", fd);
         error = SOCKERR_BIND;
         Socket_Close(fd);
         goto error;
      } else {
         Debug(LGPFX "Successfully bound to port %d for socket %d\n",
               localAddr.svm_port, fd);
      }
   }

   if (connect(fd, (struct sockaddr *)&addr, addrLen) != 0) {
      sysErr = SocketGetLastError();
      Debug(LGPFX "socket connect failed, error %d: %s\n",
            sysErr, Err_Errno2String(sysErr));
      Socket_Close(fd);
      error = SOCKERR_CONNECT;
      goto error;
   }

   VMCISock_ReleaseAFValueFd(vsockDev);
   Debug(LGPFX "socket %d connected\n", fd);
   return fd;

error:
   if (outError) {
      *outError = error;
   }
   VMCISock_ReleaseAFValueFd(vsockDev);

   return INVALID_SOCKET;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Socket_DecodePacket --
 *
 *    Helper function to decode received packet in DataMap encoding format.
 *
 * Result:
 *    None
 *
 * Side-effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
Socket_DecodePacket(const char *recvBuf,       // IN
                    int fullPktLen,            // IN
                    char **payload,            // OUT
                    int32 *payloadLen)         // OUT
{
   ErrorCode res;
   DataMap map;
   char *buf;
   int32 len;

   *payload = NULL;
   *payloadLen = 0;

   /* decoding the packet */
   res = DataMap_Deserialize(recvBuf, fullPktLen, &map);
   if (res != DMERR_SUCCESS) {
      Debug(LGPFX "Error in dataMap decoding, error=%d\n", res);
      return FALSE;
   }

   res = DataMap_GetString(&map, GUESTRPCPKT_FIELD_PAYLOAD, &buf, &len);
   if (res == DMERR_SUCCESS) {
      char *tmpPtr = malloc(len + 1);
      if (tmpPtr == NULL) {
         Debug(LGPFX "Error in allocating memory\n");
         goto error;
      }
      memcpy(tmpPtr, buf, len);
      /* add a trailing 0 for backward compatibility */
      tmpPtr[len] = '\0';

      *payload = tmpPtr;
      *payloadLen = len;
   } else {
      Debug(LGPFX "Error in decoding payload, error=%d\n", res);
      goto error;
   }

   DataMap_Destroy(&map);
   return TRUE;

error:
   DataMap_Destroy(&map);
   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Socket_PackSendData --
 *
 *    Helper function for building send packet and serialize it.
 *
 * Result:
 *    TRUE on sucess, FALSE otherwise.
 *
 * Side-effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static gboolean
Socket_PackSendData(const char *buf,             // IN
                    int len,                     // IN
                    char **serBuf,               // OUT
                    int32 *serBufLen)            // OUT
{
   DataMap map;
   ErrorCode res;
   char *newBuf;
   gboolean mapCreated = FALSE;
   int64 pktType = GUESTRPCPKT_TYPE_DATA;

   res = DataMap_Create(&map);
   if (res != DMERR_SUCCESS) {
      goto error;
   }

   mapCreated = TRUE;
   res = DataMap_SetInt64(&map, GUESTRPCPKT_FIELD_TYPE,
                          pktType, TRUE);
   if (res != DMERR_SUCCESS) {
      goto error;
   }

   newBuf = malloc(len);
   if (newBuf == NULL) {
      Debug(LGPFX "Error in allocating memory.\n");
      goto error;
   }
   memcpy(newBuf, buf, len);
   res = DataMap_SetString(&map, GUESTRPCPKT_FIELD_PAYLOAD, newBuf,
                           len, TRUE);
   if (res != DMERR_SUCCESS) {
      free(newBuf);
      goto error;
   }

   res = DataMap_Serialize(&map, serBuf, serBufLen);
   if (res != DMERR_SUCCESS) {
      goto error;
   }

   DataMap_Destroy(&map);
   return TRUE;

error:
   if (mapCreated) {
      DataMap_Destroy(&map);
   }
   Debug(LGPFX "Error in dataMap encoding\n");
   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Socket_RecvPacket --
 *
 *    Helper function to recv a dataMap packet over the socket.
 *    The caller has to *free* the payload to avoid memory leak.
 *
 * Result:
 *    TRUE on sucess, FALSE otherwise.
 *
 * Side-effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
Socket_RecvPacket(SOCKET sock,               // IN
                  char **payload,            // OUT
                  int *payloadLen)           // OUT
{
   gboolean ok;
   int32 packetLen;
   int packetLenSize = sizeof packetLen;
   int fullPktLen;
   char *recvBuf;
   int recvBufLen;

   ok = Socket_Recv(sock, (char *)&packetLen, packetLenSize);
   if (!ok) {
      Debug(LGPFX "error in recving packet header, err=%d\n",
            SocketGetLastError());
      return FALSE;
   }

   fullPktLen = ntohl(packetLen) + packetLenSize;
   recvBufLen = fullPktLen;
   recvBuf = malloc(recvBufLen);
   if (recvBuf == NULL) {
      Debug(LGPFX "Could not allocate recv buffer.\n");
      return FALSE;
   }

   memcpy(recvBuf, &packetLen, packetLenSize);
   ok = Socket_Recv(sock, recvBuf + packetLenSize,
                     fullPktLen - packetLenSize);
   if (!ok) {
      Debug(LGPFX "error in recving packet, err=%d\n",
            SocketGetLastError());
      free(recvBuf);
      return FALSE;
   }

   ok = Socket_DecodePacket(recvBuf, fullPktLen, payload, payloadLen);
   free(recvBuf);
   return ok;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Socket_SendPacket --
 *
 *    Helper function to send a dataMap packet over the socket.
 *
 * Result:
 *    TRUE on sucess, FALSE otherwise.
 *
 * Side-effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

gboolean
Socket_SendPacket(SOCKET sock,               // IN
                  const char *payload,       // IN
                  int payloadLen)            // IN
{
   gboolean ok;
   char *sendBuf;
   int sendBufLen;

   if (!Socket_PackSendData(payload, payloadLen, &sendBuf, &sendBufLen)) {
      return FALSE;
   }

   ok = Socket_Send(sock, sendBuf, sendBufLen);
   free(sendBuf);

   return ok;
}
