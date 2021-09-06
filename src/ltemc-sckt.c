/******************************************************************************
 *  \file ltemc-sckt.c
 *  \author Greg Terrell
 *  \license MIT License
 *
 *  Copyright (c) 2020,2021 LooUQ Incorporated.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
 * "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 * LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ******************************************************************************
 * TCP/IP sockets protocol support: TCP, UDP, SSL, TLS
 *****************************************************************************/


#define _DEBUG 0                        // set to non-zero value for PRINTF debugging output, 
// debugging output options             // LTEm1c will satisfy PRINTF references with empty definition if not already resolved
#if _DEBUG > 0
    asm(".global _printf_float");       // forces build to link in float support for printf
    #if _DEBUG == 1
    #define SERIAL_DBG 1                // enable serial port output using devl host platform serial, 1=wait for port
    #elif _DEBUG == 2
    #include <jlinkRtt.h>               // output debug PRINTF macros to J-Link RTT channel
    #define PRINTF(c_,f_,__VA_ARGS__...) do { rtt_printf(c_, (f_), ## __VA_ARGS__); } while(0)
    #endif
#else
#define PRINTF(c_, f_, ...)
#endif


#include "ltemc-sckt.h"


#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) < (y)) ? (y) : (x))

#define ASCII_sSENDOK "SEND OK\r\n"

enum
{
    sckt__resultCode_previouslyOpen = 563,
    sckt__defaultOpenTimeoutMS = 60000,

    SCKT__IRD_requestMaxSz = 1500
};

extern ltemDevice_t g_ltem;


// file scope local function declarations
static bool S_requestIrdData(dataContext_t dataPeer, uint16_t reqstSz, bool applyLock);
static resultCode_t S_tcpudpOpenCompleteParser(const char *response, char **endptr);
static resultCode_t S_sslOpenCompleteParser(const char *response, char **endptr);
static resultCode_t S_socketSendCompleteParser(const char *response, char **endptr);
static resultCode_t S_socketStatusParser(const char *response, char **endptr);
void S_scktDoWork();


/* public sockets (IP:TCP/UDP/SSL) functions
 * --------------------------------------------------------------------------------------------- */
#pragma region public functions

/**
 *	\brief Create a socket data control(TCP/UDP/SSL).
 *
 *  \param scktCtrl [in/out] Pointer to socket control structure
 *	\param dataCntxt [in] - Data context (0-5) to host socket.
 *	\param protocol [in] - The IP protocol to use for the connection (TCP/UDP/SSL clients).
 *  \param recvBuf [in] - Pointer to application created receive buffer
 *  \param recvBufSz [in] - Size of allocated receive buffer
 *  \param recvCallback [in] - The callback function in your application to be notified of received data ready.
 * 
 *  \return socket result code similar to http status code, OK = 200
 */
void sckt_initControl(scktCtrl_t *scktCtrl, dataContext_t dataCntxt, protocol_t protocol, uint8_t *recvBuf, uint16_t recvBufSz, scktRecvFunc_t recvCallback)
{
    ASSERT(scktCtrl != NULL && recvBuf != NULL && recvCallback != NULL, srcfile_sckt_c);
    ASSERT(dataCntxt < dataContext_cnt, srcfile_sckt_c);
    ASSERT(protocol < protocol_socket, srcfile_sckt_c);

    memset(scktCtrl, 0, sizeof(scktCtrl_t));

    scktCtrl->ctrlMagic = streams__ctrlMagic;
    scktCtrl->dataCntxt = dataCntxt;
    scktCtrl->protocol = protocol;
    scktCtrl->useTls = protocol == protocol_ssl;

    uint16_t bufferSz = IOP_initRxBufferCtrl(&(scktCtrl->recvBufCtrl), recvBuf, recvBufSz);
    ASSERT_W(recvBufSz == bufferSz, srcfile_sckt_c, "RxBufSz != multiple of 128bytes");
    ASSERT(bufferSz >= 128, srcfile_sckt_c);
    
    scktCtrl->doWorkTimeout = (uint32_t)(scktCtrl->recvBufCtrl._bufferSz / IOP__uartFIFOBufferSz * IOP__uartFIFO_fillMS * 0.8);
    scktCtrl->dataRecvCB = recvCallback;
    scktCtrl->dataPending = false;
    scktCtrl->flushing = false;
    scktCtrl->statsRxCnt = 0;
    scktCtrl->statsTxCnt = 0;
}


/**
 *	\brief Open a data connection (socket) to d data to an established endpoint via protocol used to open socket (TCP/UDP/TCP INCOMING).
 *
 *  \param sckt [in] - Pointer to the control block for the socket.
 *	\param host [in] - The IP address (string) or domain name of the remote host to communicate with.
 *  \param rmtPort [in] - The port number at the remote host.
 *  \param lclPort [in] - The port number on this side of the conversation, set to 0 to auto-assign.
 *  \param cleanSession [in] - If the port is found already open, TRUE: flushes any previous data from the socket session.
 * 
 *  \return Result code similar to http status code, OK = 200
 */
resultCode_t sckt_open(scktCtrl_t *sckt, const char *host, uint16_t rmtPort, uint16_t lclPort, bool cleanSession)
{
    ASSERT(sckt->ctrlMagic == streams__ctrlMagic, srcfile_sckt_c);
    ASSERT(sckt->dataCntxt < dataContext_cnt || ((iop_t*)g_ltem.iop)->streamPeers[sckt->dataCntxt] != NULL, srcfile_sckt_c);
    ASSERT(sckt->protocol < protocol_socket, srcfile_sckt_c);

    ((iop_t*)g_ltem.iop)->scktMap &= 0x01 << sckt->dataCntxt;

    switch (sckt->protocol)
    {
    case protocol_udp:
        atcmd_setOptions(atcmd__setLockModeAuto, sckt__defaultOpenTimeoutMS, S_tcpudpOpenCompleteParser);
        atcmd_tryInvokeAutoLockWithOptions("AT+QIOPEN=%d,%d,\"UDP\",\"%s\",%d,%d", g_ltem.pdpContext, sckt->dataCntxt, host, rmtPort, lclPort);
        break;
  case protocol_tcp:
        atcmd_setOptions(atcmd__setLockModeAuto, sckt__defaultOpenTimeoutMS, S_tcpudpOpenCompleteParser);
        atcmd_tryInvokeAutoLockWithOptions("AT+QIOPEN=%d,%d,\"TCP\",\"%s\",%d,%d", g_ltem.pdpContext, sckt->dataCntxt, host, rmtPort, lclPort);
        break;

    case protocol_ssl:
        atcmd_setOptions(atcmd__setLockModeAuto, sckt__defaultOpenTimeoutMS, S_sslOpenCompleteParser);
        atcmd_tryInvokeAutoLockWithOptions("AT+QSSLOPEN=%d,%d,\"SSL\",\"%s\",%d,%d", g_ltem.pdpContext, sckt->dataCntxt, host, rmtPort, lclPort);
        break;
    }
    // await result of open from inside switch() above
    resultCode_t atResult = atcmd_awaitResult();

    // finish initialization and run background tasks to prime data pipeline
    if (atResult == resultCode__success || atResult == sckt__resultCode_previouslyOpen)
    {
        ((iop_t*)g_ltem.iop)->streamPeers[sckt->dataCntxt] = sckt;
        ((iop_t*)g_ltem.iop)->scktMap |= 0x01 << sckt->dataCntxt;

        LTEM_registerDoWorker(S_scktDoWork);
    }
    
    if (atResult == sckt__resultCode_previouslyOpen)
    {
        atResult = resultCode__previouslyOpened;                                                        // translate 563 BGx response into 200 range
        ((scktCtrl_t *)((iop_t*)g_ltem.iop)->streamPeers[sckt->dataCntxt])->flushing = cleanSession;
        ((scktCtrl_t *)((iop_t*)g_ltem.iop)->streamPeers[sckt->dataCntxt])->dataPending = true;
        PRINTF(dbgColor__white, "Flushing sckt=%d\r", sckt->dataCntxt);
        S_scktDoWork();                                                                                 // run doWork to perform flush
    }
    return atResult;
}



/**
 *	\brief Close an established (open) connection socket.
 *
 *	\param scktCtrl [in] - Pointer to socket control struct governing the sending socket's operation.
 */
void sckt_close(scktCtrl_t *scktCtrl)
{
    ASSERT(scktCtrl->ctrlMagic != streams__ctrlMagic, 0xFE30);

    char closeCmd[20] = {0};
    uint8_t socketBitMask = 0x01 << scktCtrl->dataCntxt;
    scktCtrl_t *thisSckt = (scktCtrl_t *)((iop_t*)g_ltem.iop)->streamPeers[scktCtrl->dataCntxt];    // for readability

    if (thisSckt == 0)                           // not open
        return;

    if (thisSckt->useTls)
    {
        snprintf(closeCmd, 20, "AT+QSSLCLOSE=%d", scktCtrl->dataCntxt);                                 // BGx syntax different for SSL
    }
    else
    {
        snprintf(closeCmd, 20, "AT+QICLOSE=%d", scktCtrl->dataCntxt);                                   // BGx syntax different for TCP/UDP
    }
    
    if (atcmd_tryInvokeDefaults(closeCmd))
    {
        if (atcmd_awaitResult() == resultCode__success)
        {
            thisSckt = 0;
            ((iop_t*)g_ltem.iop)->scktMap &= ~socketBitMask;
        }
    }
}


/**
 *	\brief Reset open socket connection. This function drains the connection's data pipeline. 
 *
 *	\param scktCtrl [in] - Pointer to socket control struct governing the sending socket's operation.
 *
 *  \return True if flush socket data initiated.
 */
bool sckt_flush(scktCtrl_t *scktCtrl)
{
    ASSERT(scktCtrl->ctrlMagic != streams__ctrlMagic, 0xFE30);

    if ((scktCtrl_t *)((iop_t*)g_ltem.iop)->streamPeers[scktCtrl->dataCntxt] == NULL)       // not open
        return;

    if (s_requestIrdData(scktCtrl->dataCntxt, scktCtrl->recvBufCtrl._bufferSz, true))        // initiate an IRD flow
    {
        scktCtrl->flushing = true;
        return true;
    }
    return false;                                                                       // unable to obtain action lock
}



/**
 *  \brief Close out all TCP/IP sockets on a context.
 *
 *	\param contxtId [in] - The carrier PDP context hosting the sockets to close.
*/
void sckt_closeAll()
{
    for (size_t i = 0; i < dataContext_cnt; i++)
    {
        sckt_close(i);
    }
}


bool sckt_getState(scktCtrl_t *sckt)
{
    ASSERT(sckt->ctrlMagic != streams__ctrlMagic, srcfile_sckt_c);

    atcmd_setOptions(atcmd__setLockModeAuto, atcmd__useDefaultTimeout, S_socketStatusParser);
    if (!atcmd_tryInvokeOptions("AT+QISTATE=1,%d", sckt->dataCntxt))
        return resultCode__conflict;

    return atcmd_awaitResult() == resultCode__success;
}


/**
 *	\brief Send data to an established endpoint via protocol used to open socket (TCP/UDP/TCP INCOMING).
 *
 *	\param scktCtrl [in] - Pointer to socket control struct governing the sending socket's operation.
 *	\param data [in] - A character pointer containing the data to send.
 *  \param dataSz [in] - The size of the buffer (< 1501 bytes).
 */
resultCode_t sckt_send(scktCtrl_t *scktCtrl, const char *data, uint16_t dataSz)
{
    resultCode_t atResult;
    ASSERT(scktCtrl != 0 && (((iop_t*)g_ltem.iop)->scktMap & 0x01 << scktCtrl->dataCntxt) != 0, srcfile_sckt_c);
    ASSERT(dataSz > 0, srcfile_sckt_c);

    // AT+QISEND command initiates send by signaling we plan to send dataSz bytes on a socket,
    // send has subcommand to actual transfer the bytes, so don't automatically close action cmd

    atcmd_setOptions(atcmd__setLockModeManual, atcmd__useDefaultTimeout, atcmd_txDataPromptParser);
    if (atcmd_awaitLock(atcmd__useDefaultTimeout))
    {
        atcmd_invokeReuseLock("AT+QISEND=%d,%d", scktCtrl->dataCntxt, dataSz);      // reusing manual lock
        atResult = atcmd_awaitResult();                                             // waiting for data prompt, leaving action open on return if sucessful

        // await data prompt atResult successful, now send data sub-command to actually transfer data, now automatically close action after data sent
        if (atResult == resultCode__success)
        {
            atcmd_setOptions(atcmd__setLockModeManual, atcmd__useDefaultTimeout, S_socketSendCompleteParser);
            atcmd_sendCmdData(data, dataSz, "");    // parse for BGx send complete
            atResult = atcmd_awaitResult();
            scktCtrl->statsTxCnt++;
        }
    }
    atcmd_close();
    return atResult;                                                            // return sucess -OR- failure from sendRequest\sendRaw action
}


// /**
//  *   \brief Brief inline static function to support doWork() readability
// */
// static inline uint16_t pageDataAvailable(rxDataBufferCtrl_t *buf, uint8_t page)
// {
//     return buf->pages[page].head - buf->pages[page].tail;
// }



/**
 *   \brief Perform background tasks to move socket data through pipeline.
 * 
 *   1) check for existing IRD flow and proccess
 *   2) deliver received data to application
 *   3) if no IRD flow underway, check for other sockets that have dataPending
*/
void S_scktDoWork()
{
    iop_t *iopPtr = (iop_t*)g_ltem.iop;                 // shortcut to the IOP subsystem

    /* PROCESS ACTIVE DATA STREAM FLOW 
     *-------------------------------------------------------------------------------------------*/
    if (iopPtr->rxStreamCtrl != NULL &&                                     // there is an active receive stream peer
        ((baseCtrl_t *)iopPtr->rxStreamCtrl)->protocol < protocol_socket)   // AND it is a socket
    {
        // readability vars
        scktCtrl_t *scktPtr = ((scktCtrl_t *)iopPtr->rxStreamCtrl);
        uint8_t dataCntxt = scktPtr->dataCntxt;
        ASSERT(scktPtr == iopPtr->streamPeers[dataCntxt], srcfile_sckt_c);  // ASSERT dataCntxt cross-links are still valid
        rxDataBufferCtrl_t *bufPtr = &(scktPtr->recvBufCtrl);               // smart-buffer for this operation

        uint16_t dataAvailable = 0;
        do
        {
            /* Check for IRD size unknown and data in IOP page, determine IRD and start segment flow */
            if (scktPtr->irdRemaining == -1 && rxPageDataAvailable(bufPtr, !bufPtr->iopPg) > 9)     // IRD unknown && IOP sufficient for IRD header
            {
                PRINTF(dbgColor__cyan, "scktDoWork-IRDOpen iopPg=%d, [0]=%d, [1]=%d\r", bufPtr->iopPg, rxPageDataAvailable(bufPtr, 0), rxPageDataAvailable(bufPtr, 1));
                                                                                                    // -- 1st data chuck has data header with size of queued data in BGx
                iopPtr->txSendStartTck = 0;                                                         // stop IRD request timeout timer
                char *irdSzAt = bufPtr->pages[!bufPtr->iopPg]._buffer + 9;                          // data prefix from BGx:  len("\r\n+QIRD: ") == 9
                ASSERT(bufPtr->pages[!bufPtr->iopPg]._buffer[7] == ':', srcfile_sckt_c);            // check BGx response if not valid IRD, we are lost
                scktPtr->irdRemaining = strtol(irdSzAt, &bufPtr->pages[!bufPtr->iopPg].tail, 10);   // parse IRD segment sz:  \r\n+QIRD: <dataSz>, tail points to data
                bufPtr->pages[!bufPtr->iopPg].tail += 2;                                            // ignore /r/n between data size and data
            }

            /* Process the data available following the IRD header */
            dataAvailable = rxPageDataAvailable(bufPtr, !bufPtr->iopPg);

            if (scktPtr->irdRemaining > 0 && dataAvailable)                         // IRD segment not complete, send available to appl
            {
                PRINTF(dbgColor__cyan, "scktDoWork-sendApp ird=%d, da=%d\r", scktPtr->irdRemaining, dataAvailable);

                uint16_t applAvailable = MIN(dataAvailable, scktPtr->irdRemaining);     // send what we have, up to irdRemaining
                if (!scktPtr->flushing)
                {
                    /* data ready event, send to application
                     * invoke application socket receiver_func: socket number, data pointer, number of bytes in buffer */
                    scktPtr->dataRecvCB(dataCntxt, bufPtr->pages[!bufPtr->iopPg].tail, applAvailable);
                    scktPtr->irdRemaining -= applAvailable;
                }
                IOP_resetRxDataBufferPage(bufPtr, !bufPtr->iopPg);                  // delivered, clear buffer page
            }

            if (scktPtr->irdRemaining == 0)                                         // IRD segement complete
            {
                PRINTF(dbgColor__cyan, "scktDoWork-CloseCk iopPg=%d, [0]=%d, [1]=%d\r", bufPtr->iopPg, rxPageDataAvailable(bufPtr, 0), rxPageDataAvailable(bufPtr, 1));

                PRINTF(dbgColor__dGreen, "closeIRD sckt=%d\r", dataCntxt);
                iopPtr->rxStreamCtrl = NULL;                                        // take IOP out of data mode
                scktPtr->dataPending = false;
                scktPtr->flushing = false;
                scktPtr->doWorkLastTck = pMillis();                                 // leaving URC\IRD servicing, restart URC cycle
                IOP_resetRxDataBufferPage(bufPtr, !bufPtr->iopPg);                  // IRD response buffer processed, clear buffer page
                atcmd_close();                                                      // close IRD request action and release action lock
                break;
            }

            /* Check for IOP buffer page with data and RX idle, pull data forward to finish the IRD segment */
            if  (rxPageDataAvailable(bufPtr, bufPtr->iopPg) && IOP_detectRxIdle())
            {
                PRINTF(dbgColor__cyan, "Idle sckt=%d, swapToPg=%d\r", dataCntxt, bufPtr->iopPg);
                IOP_swapRxBufferPage(bufPtr);                                       // pull page forward
            }
            dataAvailable = rxPageDataAvailable(bufPtr, !bufPtr->iopPg);

        } while (dataAvailable);

        /* check for timeout on IRD request, so we don't wait forever.
         * irdReqstAt is a global (singleton) in ltemc-sckt, only one request can be threaded through BGx
         *-----------------------------------------------------------------------------------------------------------*/

        if (pElapsed(iopPtr->txSendStartTck, 10000))              // if check for IRD timeout
        // if (pElapsed(iopPtr->txSendStartTck, ATCMD__defaultTimeoutMS))              // if check for IRD timeout
        {
            iopPtr->txSendStartTck = 0;                                             // no longer waiting for IRD response
            atcmd_close();                                                          // release action lock
            ltem_notifyApp(lqNotifType_lqDevice_streamFault, "IRD timeout");
        }
    }

    /* PROCESS OPEN SOCKETS WITHOUT AN ACTIVE DATA STREAM FLOW 
     *------------------------------------------------------------------------------------------------------
     * Now see if any other sockets are open and have dataPending reported by BGx (+QIURC/+QSSLURC event)
     *----------------------------------------------------------------------------------------------------*/
    if (!atcmd_isLockActive() && iopPtr->scktMap)                               // if no ATCMD underway AND there are sockets open 
    {

        iopPtr->scktLstWrk = ++iopPtr->scktLstWrk % dataContext_cnt;            // IRD fairness: if mult sckts. Skip last, get next possible context
        uint8_t nextIrd;
        for (uint8_t i = 0; i < dataContext_cnt; i++)
        {
            nextIrd = (iopPtr->scktLstWrk + i) % dataContext_cnt;               // rotate to look for next open sckt for IRD request
            scktCtrl_t *scktPtr = ((scktCtrl_t *)((iop_t*)g_ltem.iop)->streamPeers[nextIrd]);

            if (iopPtr->scktMap & 0x01 << nextIrd)                              // socket is OPEN
            {
                ASSERT_W(!pElapsed(scktPtr->doWorkLastTck, scktPtr->doWorkTimeout), srcfile_sckt_c, "doWork freq slow:bffr ovrflw risk");
                scktPtr->doWorkLastTck = pMillis();                             // last check for URC\data pending check cycle

                if (scktPtr->dataPending)                                       // socket has data reported (BGx URC)
                {
                    if (S_requestIrdData((dataContext_t)nextIrd, 0, true))      // Request maximum data (IRD) with action lock 
                    {                                                           // this puts IOP in data mode (IRD is hybrid of cmd and data)
                        ((scktCtrl_t*)iopPtr->rxStreamCtrl)->statsRxCnt++;      // tally RX (IRD) segments, full IRD counts as only 1
                        PRINTF(dbgColor__dGreen, "SCKT-openIRD sckt=%d\r", i);
                        break;                              // If the IRD request gets a lock, the IRD process starts for the socket[i]
                                                            // If the request cannot get a lock (maybe a send\transmit cmd is underway) it silently returns
                                                            // 
                                                            // The IRD process is a true BGx action that block other foreground actions until the 
                                                            // pipeline is emptied and no more data is pending. This releases the lock and allows
                                                            // other types of commands to be sent to BGx. 
                    }
                }
            }
        }
        iopPtr->scktLstWrk = nextIrd;
    }
}

#pragma endregion


#pragma region private local static functions
/*-----------------------------------------------------------------------------------------------*/

/**
 *  \brief [private] Invoke IRD command to request BGx for socket (read) data
*/
static bool S_requestIrdData(dataContext_t dataCntx, uint16_t reqstSz, bool applyLock)
{
    ASSERT(dataCntx < dataContext_cnt, srcfile_sckt_c);                                     // ASSERT data context is valid context
    ASSERT(((iop_t*)g_ltem.iop)->scktMap & 0x01 << dataCntx, srcfile_sckt_c);               // ASSERT socket is open 

    char irdCmd[24] = {0};
    uint16_t requestedSz = (!reqstSz) ? SCKT__IRD_requestMaxSz : MAX(SCKT__IRD_requestMaxSz, reqstSz);

    if (((scktCtrl_t *)((iop_t*)g_ltem.iop)->streamPeers[dataCntx])->protocol == protocol_ssl)
        snprintf(irdCmd, 24, "AT+QSSLRECV=%d,%d", dataCntx, requestedSz);
    else
        snprintf(irdCmd, 24, "AT+QIRD=%d,%d", dataCntx, requestedSz);
    // PRINTF(dbgColor__white, "rqstIrd lck=%d, cmd=%s\r", applyLock, irdCmd);

    if (applyLock && !atcmd_awaitLock(atcmd__useDefaultTimeout))
        return false;

    ((iop_t*)g_ltem.iop)->rxStreamCtrl = ((iop_t*)g_ltem.iop)->streamPeers[dataCntx];       // set IOP in data mode
    ((iop_t*)g_ltem.iop)->txSendStartTck = pMillis();
    ((scktCtrl_t*)((iop_t*)g_ltem.iop)->rxStreamCtrl)->irdRemaining = -1;                   // signal IRD pending with unknown pending

    IOP_sendTx(irdCmd, strlen(irdCmd), false);
    IOP_sendTx("\r", 1, true);
    return true;
}



/**
 *	\brief [private] TCP/UDP wrapper for open connection parser.
 */
static resultCode_t S_tcpudpOpenCompleteParser(const char *response, char **endptr) 
{
    return atcmd_serviceResponseParser(response, "+QIOPEN: ", 1, endptr);
}


/**
 *	\brief [private] SSL wrapper for open connection parser.
 */
static resultCode_t S_sslOpenCompleteParser(const char *response, char **endptr) 
{
    return atcmd_serviceResponseParser(response, "+QSSLOPEN: ", 1, endptr);
}


/**
 *	\brief [private] SSL wrapper for open connection parser.
 */
static resultCode_t S_socketSendCompleteParser(const char *response, char **endptr)
{
    return atcmd_defaultResultParser(response, "", false, 0, ASCII_sSENDOK, endptr);
}

/**
 *	\brief [static] Socket status parser
 *
 *  Wraps generic atcm
 *
 *  \param response [in] Character data recv'd from BGx to parse for task complete
 *  \param endptr [out] Char pointer to the char following parsed text
 * 
 *  \return HTTP style result code, 0 = not complete
 */
static resultCode_t S_socketStatusParser(const char *response, char **endptr) 
{
    // BGx +QMTCONN Read returns Status = 3 for connected, service parser returns 203
    return atcmd_serviceResponseParser(response, "+QISTATE: ", 5, endptr) == 202 ? resultCode__success : resultCode__unavailable;
}


#pragma endregion