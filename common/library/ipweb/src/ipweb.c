/**************************************************************************
*  Copyright (c) 2015 by Michael Fischer (www.emb4fun.de).
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without 
*  modification, are permitted provided that the following conditions 
*  are met:
*  
*  1. Redistributions of source code must retain the above copyright 
*     notice, this list of conditions and the following disclaimer.
*
*  2. Redistributions in binary form must reproduce the above copyright
*     notice, this list of conditions and the following disclaimer in the 
*     documentation and/or other materials provided with the distribution.
*
*  3. Neither the name of the author nor the names of its contributors may 
*     be used to endorse or promote products derived from this software 
*     without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL 
*  THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS 
*  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
*  AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 
*  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF 
*  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
*  SUCH DAMAGE.
*
***************************************************************************
*  History:
*
*  27.06.2015  mifi  First Version.
*  13.03.2016  mifi  Renamed to web.c.
**************************************************************************/
#define __IPWEB_C__

/*=======================================================================*/
/*  Includes                                                             */
/*=======================================================================*/

#include <string.h>
#include <stdint.h>

#include "tal.h"
#include "ipstack.h"
#include "ipweb.h"
#include "web_cgi.h"

#if !defined(IP_WEB_MAX_HTTP_TASKS) 
#define _MAX_WEB_CLIENT_TASKS   4
#else
#define _MAX_WEB_CLIENT_TASKS   IP_WEB_MAX_HTTP_TASKS
#endif

/*=======================================================================*/
/*  Global                                                               */
/*=======================================================================*/

int gRedirectHTTPtoHTTPS = 0;
uint32_t gWebIdleStartTime    = 0;

/*=======================================================================*/
/*  Extern                                                               */
/*=======================================================================*/

extern ISC_LIST(MEDIA_TYPE_ENTRY) mediaTypeList;

/*=======================================================================*/
/*  All Structures and Common Constants                                  */
/*=======================================================================*/

/*
 * Media types
 */
static char mtc_text[] = "text";
static char mtc_image[] = "image";
char mtc_application[] = "application";

/* In umgekehrter Reihenfolge alphabetisch sortiert. */
static MEDIA_TYPE_ENTRY mt_defaults[] = {
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_text,        NULL,         MTFLAG_INITIAL, MediaTypeHandlerText,   "xml"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_text,        "plain",      MTFLAG_INITIAL, MediaTypeHandlerText,   "txt"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_image,       "svg+xml",    MTFLAG_INITIAL, MediaTypeHandlerBinary, "svg"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_text,        "html",       MTFLAG_INITIAL, HttpSsiHandler,         "shtml" },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_image,       NULL,         MTFLAG_INITIAL, MediaTypeHandlerBinary, "png"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_application, NULL,         MTFLAG_INITIAL, MediaTypeHandlerBinary, "pdf"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_application, "javascript", MTFLAG_INITIAL, MediaTypeHandlerText,   "js"    },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_image,       "jpeg",       MTFLAG_INITIAL, MediaTypeHandlerBinary, "jpg"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_image,       "x-icon",     MTFLAG_INITIAL, MediaTypeHandlerBinary, "ico"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_text,        NULL,         MTFLAG_INITIAL, MediaTypeHandlerText,   "html"  },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_text,        "html",       MTFLAG_INITIAL, MediaTypeHandlerText,   "htm"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_image,       NULL,         MTFLAG_INITIAL, MediaTypeHandlerBinary, "gif"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_text,        NULL,         MTFLAG_INITIAL, MediaTypeHandlerText,   "css"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_text,        "cgi",        MTFLAG_INITIAL, MediaTypeHandlerText,   "cgi"   },
    { ISC_LINK_INITIAL(MEDIA_TYPE_ENTRY), mtc_image,       NULL,         MTFLAG_INITIAL, MediaTypeHandlerBinary, "bmp"   },
};

#define MT_DEFAULTS     (sizeof(mt_defaults) / sizeof(MEDIA_TYPE_ENTRY))

/*
 * Some external swithes are not ready to use if the link
 * is available. Therefore wait a short time.
 */
#define DELAY_AFTER_LINK_MS   2000


#define ROMFS_HTTP_ROOT_PATH  "ROMFS:/htdocs/"


typedef struct _CLIENT_THREAD_PARAM 
{
   HTTP_STREAM        *ctp_stream;
   HTTP_CLIENT_HANDLER ctp_handler;
} CLIENT_THREAD_PARAM;

typedef struct _client_info_
{
   OS_TCB               TCB;
   uint8_t             *Stack;
   uint16_t             StackSize;
   CLIENT_THREAD_PARAM *ctp;
} client_info_t;

/*=======================================================================*/
/*  Definition of all global Data                                        */
/*=======================================================================*/

/*=======================================================================*/
/*  Definition of all local Data                                         */
/*=======================================================================*/

/* 
 * Stack and TCB definition
 */
static OS_STACK (ServerStack, TASK_IP_WEB_SERVER_STK_SIZE); 
static OS_TCB TCBServerTask;

static client_info_t       ClientArray[_MAX_WEB_CLIENT_TASKS];
static uint64_t            ClientStack[_MAX_WEB_CLIENT_TASKS][TASK_IP_WEB_CLIENT_STK_SIZE/8];

static int       nNumThreads = 0;
static int       nWebsInit    = 0;
static int       nWebsRunning = 0;
static uint16_t  wServerPort;

int nNumThreadsMax = 0;

/*=======================================================================*/
/*  Definition of all local Procedures                                   */
/*=======================================================================*/

/*************************************************************************/
/*  InitDefaults                                                         */
/*                                                                       */
/*  Initialize the media types.                                          */
/*                                                                       */
/*  In    : none                                                         */
/*  Out   : none                                                         */
/*  Return: none                                                         */
/*************************************************************************/
static void InitDefaults (void)
{
   int i;

   ISC_LIST_INIT(mediaTypeList);                                     /*lint !e717*/
   for (i = 0; i < (int)MT_DEFAULTS; i++)
   {
      ISC_LIST_APPEND(mediaTypeList, &mt_defaults[i], media_link);   /*lint !e717*/
   }
   
} /* InitDefaults */

/*************************************************************************/
/*  FindFreeClient                                                       */
/*                                                                       */
/*  In    : task parameter                                               */
/*  Out   : none                                                         */
/*  Return: never                                                        */
/*************************************************************************/
static client_info_t *FindFreeClient (void)
{
   client_info_t *Client = NULL;
   
   for (int i=0; i<_MAX_WEB_CLIENT_TASKS; i++)
   {
      if (OS_TASK_STATE_NOT_IN_USE == ClientArray[i].TCB.State)
      {
         Client = &ClientArray[i];
         break;
      }
   }
   
   return(Client);
} /* FindFreeClient */

/*************************************************************************/
/*  WebClient                                                            */
/*                                                                       */
/*  In    : task parameter                                               */
/*  Out   : none                                                         */
/*  Return: never                                                        */
/*************************************************************************/
static void WebClient (void *p)
{
   client_info_t       *Client = (client_info_t*)p;
   CLIENT_THREAD_PARAM *ctp    = (CLIENT_THREAD_PARAM *)Client->ctp;

#if 1
   (*ctp->ctp_handler)(ctp->ctp_stream);
#else   
   uint32_t dStart = OS_TimeGet();
   (*ctp->ctp_handler)(ctp->ctp_stream);
   term_printf("%ld\r\n", OS_TimeGet() - dStart);
#endif   
   
   closesocket(ctp->ctp_stream->strm_csock);
   xfree(ctp->ctp_stream);
   xfree(ctp);

#if 0 // Set to 1 to enable stack info output
{   
   uint32_t             Size;
   uint32_t             Free;

   Free  = OSGetStackInfo(&Client->TCB, &Size);
   TAL_PRINTF("%s: S:%4d  U:%4d  F:%4d\n", Client->TCB.Name, Size, (Size - Free), Free);
   
   if (Free < 128)
   {
      TAL_PRINTF("*** WebClient stack error: S:%4d  U:%4d  F:%4d ***\n", Size, (Size - Free), Free);
   }
}   
#endif

   nNumThreads--;
   
   /* Handle web server idle */   
   if (nNumThreads <= 0)
   {
      /* No more threads available, set time */
      gWebIdleStartTime = OS_TimeGet();
   }      
   
} /* WebClient */

/*************************************************************************/
/*  WebServer                                                            */
/*                                                                       */
/*  In    : task parameter                                               */
/*  Out   : none                                                         */
/*  Return: never                                                        */
/*************************************************************************/
static void WebServer (void *p)
{
   SOCKET               sock;
   SOCKET               csock;
   struct sockaddr_in   addr;
   struct sockaddr_in   caddr;
   socklen_t            len;
   CLIENT_THREAD_PARAM *ctp;
   HTTP_STREAM         *ctp_stream;
   unsigned int         optval;
   client_info_t       *Client;
   
   (void)p;

   
   /* Setup client data */
   memset(ClientArray, 0x00, sizeof(ClientArray));
   for(int i=0; i<_MAX_WEB_CLIENT_TASKS; i++)
   {
      ClientArray[i].Stack     = (uint8_t*)&ClientStack[i][0];
      ClientArray[i].StackSize = TASK_IP_WEB_CLIENT_STK_SIZE;
   }
   

   /* Wait that the IP interface is ready for use */
   while(!IP_IF_IsReady(IFACE_ANY))
   {
      OS_TimeDly(500);
   }
   
   /* Wait some time for the external switch */
   OS_TimeDly(DELAY_AFTER_LINK_MS);
   
   /* Create server socket */
   sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (sock != INVALID_SOCKET)
   {
      memset((void*)&addr, 0, sizeof(addr));
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
      addr.sin_port        = htons(wServerPort);
      addr.sin_family      = AF_INET;

      /* Assign a name (port) to an unnamed socket */
      if (bind(sock, (const struct sockaddr*)&addr, sizeof(addr)) == 0) /*lint !e740*/
      {
         if (listen(sock, _MAX_WEB_CLIENT_TASKS) == 0)
         {
            for (;;) 
            {
               len = sizeof(caddr);
               if ((csock = accept(sock, (struct sockaddr*)&caddr, &len)) == INVALID_SOCKET) /*lint !e740*/
               {
                  continue;   /* Error */
               }

               Client = FindFreeClient();
               if (Client != NULL)
               {
                  /* Create the Http client task */
                                    
                  //optval = 1000;
                  //setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, (char *)&optval, sizeof(optval));

#if (HTTP_KEEP_ALIVE_REQ >= 1)
                  optval = 1;
                  setsockopt(csock, SOL_SOCKET, SO_KEEPALIVE, (char *)&optval, sizeof(optval));
#endif                  

                  ctp        = xmalloc(XM_ID_WEB, sizeof(CLIENT_THREAD_PARAM));
                  ctp_stream = xcalloc(XM_ID_WEB, 1, sizeof(HTTP_STREAM));
                  
                  if ((ctp != NULL) && (ctp_stream != NULL))
                  {
                     if (0 == gRedirectHTTPtoHTTPS)
                     {
                        ctp->ctp_handler = HttpdClientHandler;
                     }
                     else
                     {
                        ctp->ctp_handler = TlsRedirHandler;
                     }   
                     ctp->ctp_stream = ctp_stream;
                     ctp->ctp_stream->strm_ssock = sock;
                     ctp->ctp_stream->strm_csock = csock;
                     memcpy(&ctp->ctp_stream->strm_saddr, &addr, sizeof(ctp->ctp_stream->strm_saddr));
                     memcpy(&ctp->ctp_stream->strm_caddr, &caddr, sizeof(ctp->ctp_stream->strm_caddr));

                     nNumThreads++;
                     Client->ctp = ctp;
                     gWebIdleStartTime = 0;

                     if (nNumThreads > nNumThreadsMax)
                     {
                        nNumThreadsMax = nNumThreads;
                     }

                     OS_TaskCreate(&Client->TCB, WebClient, (void*)Client, (TASK_IP_WEB_SERVER_PRIORITY + 1),
                                   Client->Stack, Client->StackSize, 
                                   "WebClient");
                               
                     while (nNumThreads >= _MAX_WEB_CLIENT_TASKS)
                     {
                        OS_TimeDly(10);
                     }
                  }
                  else
                  {
                     /* Memory error */
                     if (ctp != NULL)        xfree(ctp);
                     if (ctp_stream != NULL) xfree(ctp_stream);
                     
                     closesocket(csock);
                  }                     
               }
               else
               {
                  closesocket(csock);
               } /* end if (Client != NULL) */
            }
         } /* end if "listen" */
      } /* end if "bind" */
      
      closesocket(sock);
   }
   
} /* WebServer */

/*=======================================================================*/
/*  All code exported                                                    */
/*=======================================================================*/

/*************************************************************************/
/*  IPWebsInit                                                           */
/*                                                                       */
/*  Initialize the web server.                                           */
/*                                                                       */
/*  In    : none                                                         */
/*  Out   : none                                                         */
/*  Return: 0 = OK / -1 Error                                            */
/*************************************************************************/
int IPWebsInit (void)
{
   int rc = -1;
   
   if (0 == nWebsInit)
   {
      nWebsInit = 1;
      
      /* 
       * Initialize the stream interface. 
       */
      StreamInit();
      StreamInitSsl(NULL, NULL);
   
      /* 
       * Register media type defaults. These are configurable
       * in include/cfg/http.h or the Nut/OS Configurator.
       */
      InitDefaults();
      HttpRegisterMediaType("html", "text", "html", HttpSsiHandler);
      HttpRegisterMediaType("htm",  "text", "html", HttpSsiHandler);
      HttpRegisterMediaType("cgi", NULL, NULL, HttpCgiFunctionHandler);

      web_CGIInit();
      web_SSIInit();
   
      rc = 0;                   
   }
      
   return(rc);
} /* IPWebsInit */

/*************************************************************************/
/*  IPWebsStart                                                          */
/*                                                                       */
/*  Start the web server.                                                */
/*                                                                       */
/*  In    : wPort                                                        */
/*  Out   : none                                                         */
/*  Return: 0 = OK / -1 Error                                            */
/*************************************************************************/
int IPWebsStart (uint16_t wPort)
{
   int rc = -1;
   
   if ((1 == nWebsInit) && (0 == nWebsRunning))
   {
      nWebsRunning = 1;
      wServerPort  = wPort;
      
      web_CGIStart();
      web_SSIStart();

      /* Create the HTTP server tasks */
      OS_TaskCreate(&TCBServerTask, WebServer, NULL, TASK_IP_WEB_SERVER_PRIORITY,
                    ServerStack, sizeof(ServerStack), "WebServer");
                   
      rc = 0;                   
   }
      
   return(rc);
} /* IPWebsStart */    

/*************************************************************************/
/*  IPWebsIsRunnung                                                      */
/*                                                                       */
/*  Return the state if the web server is running.                       */
/*                                                                       */
/*  In    : none                                                         */
/*  Out   : none                                                         */
/*  Return: 1 = running / 0 = not running.                               */
/*************************************************************************/
int IPWebsIsRunnung (void)
{
   return(nWebsRunning);
} /* IPWebsIsRunnung */

/*** EOF ***/
