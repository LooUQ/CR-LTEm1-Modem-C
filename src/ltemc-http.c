/** ****************************************************************************
  \file 
  \brief Public API providing HTTP/HTTPS support
  \author Greg Terrell, LooUQ Incorporated

  \loouq

--------------------------------------------------------------------------------

    This project is released under the GPL-3.0 License.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 
***************************************************************************** */


#define SRCFILE "HTT"                           // create SRCFILE (3 char) MACRO for lq-diagnostics ASSERT
#include "ltemc-internal.h"
#include "ltemc-http.h"

#define _DEBUG 2                        // set to non-zero value for PRINTF debugging output, 
// debugging output options             // LTEm1c will satisfy PRINTF references with empty definition if not already resolved
#if defined(_DEBUG) && _DEBUG > 0
    asm(".global _printf_float");       // forces build to link in float support for printf
    #if _DEBUG == 1
    #define SERIAL_DBG 1                // enable serial port output using devl host platform serial, 1=wait for port
    #elif _DEBUG >= 2
    #include <jlinkRtt.h>               // output debug PRINTF macros to J-Link RTT channel
    #define PRINTF(c_,f_,__VA_ARGS__...) do { rtt_printf(c_, (f_), ## __VA_ARGS__); } while(0)
    #endif
#else
#define PRINTF(c_, f_, ...) ;
#endif

extern ltemDevice_t g_lqLTEM;

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))


/* Local Static Functions
------------------------------------------------------------------------------------------------------------------------- */
// static void S_httpDoWork();
static uint16_t S__parseResponseForHttpStatus(httpCtrl_t *httpCtrl, const char *responseTail);
static uint16_t S__setUrl(const char *url, uint16_t timeoutSec);
static cmdParseRslt_t S__httpGetStatusParser();
static cmdParseRslt_t S__httpPostStatusParser();


/* Public Functions
------------------------------------------------------------------------------------------------------------------------- */

/**
 *	@brief Create a HTTP(s) control structure to manage web communications. 
 */
void http_initControl(httpCtrl_t *httpCtrl, dataCntxt_t dataCntxt, httpRecv_func recvCallback)
{
    ASSERT(httpCtrl != NULL && recvCallback != NULL);
    ASSERT(dataCntxt < dataCntxt__cnt);

    memset(httpCtrl, 0, sizeof(httpCtrl_t));

    g_lqLTEM.streams[dataCntxt].recvDataCB = recvCallback;
    memcpy(g_lqLTEM.streams[dataCntxt].streamType, STREAM_HTTP, sizeof(STREAM_HTTP));

    httpCtrl->dataCntxt = dataCntxt;
    httpCtrl->requestState = httpState_idle;
    httpCtrl->httpStatus = resultCode__unknown;
    httpCtrl->pageCancellation = false;
    httpCtrl->useTls = false;

    // uint16_t bufferSz = IOP_initRxBufferCtrl(&(httpCtrl->recvBufCtrl), recvBuf, recvBufSz);
    // ASSERT_W(recvBufSz == bufferSz, "HTTP-RxBufSz not*128B");
    // ASSERT(bufferSz > 64);

    httpCtrl->cstmHdrs = NULL;
    httpCtrl->cstmHdrsSz = 0;
    httpCtrl->httpStatus = 0xFFFF;

    // LTEM_registerDoWorker(S_httpDoWork);                                // register the HTTP background worker with ltem_doWork()
}

/**
 *	@brief Set host connection characteristics. 
 */
void http_setConnection(httpCtrl_t *httpCtrl, const char *hostUrl, uint16_t hostPort)
{
    ASSERT(strncmp(hostUrl, "HTTP", 4) == 0 || strncmp(hostUrl, "http", 4) == 0);
    ASSERT(hostPort == 0 || hostPort >= 80);

    strncpy(httpCtrl->hostUrl, hostUrl, sizeof(httpCtrl->hostUrl));

    httpCtrl->useTls = ((httpCtrl->hostUrl)[4] == 'S' || (httpCtrl->hostUrl)[4] == 's');
    if (hostPort == 0)
    {
        httpCtrl->hostPort = httpCtrl->useTls ? 443 : 80;                  // if hostPort default, set from URL prefix
    }
}


/**
 *	@brief Registers custom headers (char) buffer with HTTP control.
 */
void http_enableCustomHdrs(httpCtrl_t *httpCtrl, char *headerBuf, uint16_t headerBufSz)
{
    ASSERT(strlen(httpCtrl->hostUrl));                                                // host connection initialized
    ASSERT(headerBuf != NULL);                                                        // valid header buffer pointer 
    // Warn on small header buffer, less than likely use for 1 or 2 small headers
    ASSERT_W(headerBufSz > (strlen(httpCtrl->hostUrl) + http__customHdrSmallWarning), "CustomHdr diminutive buffer sz");

    memset(headerBuf, 0, headerBufSz);
    httpCtrl->cstmHdrs = headerBuf;
    httpCtrl->cstmHdrsSz = headerBufSz;
}


/**
 *	@brief Adds common http headers to the custom headers buffer. REQUIRES previous enable of custom headers and buffer registration.
 */
void http_addCommonHdrs(httpCtrl_t *httpCtrl, httpHeaderMap_t headerMap)
{
    ASSERT(httpCtrl->cstmHdrs != NULL);

    bool addedHdrsFit = true;

    if (headerMap & httpHeaderMap_accept > 0 || headerMap == httpHeaderMap_all)
    {
        if (strlen(httpCtrl->cstmHdrs) + 13 < httpCtrl->cstmHdrsSz)                     // 13 = strlen("Accept: */*\r\n")
            strcat(httpCtrl->cstmHdrs, "Accept: */*\r\n");
        else
            addedHdrsFit = false;
    }
    if (headerMap & httpHeaderMap_userAgent > 0 || headerMap == httpHeaderMap_all)
    {
        if (strlen(httpCtrl->cstmHdrs) + 25 < httpCtrl->cstmHdrsSz)
            strcat(httpCtrl->cstmHdrs, "User-Agent: QUECTEL_BGx\r\n");
        else
            addedHdrsFit = false;
    }
    if (headerMap & httpHeaderMap_connection > 0 || headerMap == httpHeaderMap_all)
    {
        if (strlen(httpCtrl->cstmHdrs) + 24 < httpCtrl->cstmHdrsSz)
            strcat(httpCtrl->cstmHdrs, "Connection: Keep-Alive\r\n");
        else
            addedHdrsFit = false;
    }
    if (headerMap & httpHeaderMap_contentType > 0 || headerMap == httpHeaderMap_all)
    {
        if (strlen(httpCtrl->cstmHdrs) + 40 < httpCtrl->cstmHdrsSz)
            strcat(httpCtrl->cstmHdrs, "Content-Type: application/octet-stream\r\n");
        else
            addedHdrsFit = false;
    }

    ASSERT(addedHdrsFit);
}



void http_addCustomHdr(httpCtrl_t *httpCtrl, const char *hdrText)
{
    ASSERT(httpCtrl->cstmHdrs != NULL);
    ASSERT(strlen(httpCtrl->cstmHdrs) + strlen(hdrText) + 2 < httpCtrl->cstmHdrsSz);

    strcat(httpCtrl->cstmHdrs, hdrText);
    strcat(httpCtrl->cstmHdrs, "\r\n");
}


/**
 *	@brief Adds a basic authorization header to the custom headers buffer, requires previous custom headers buffer registration.
 */
void http_addBasicAuthHdr(httpCtrl_t *http, const char *user, const char *pw)
{
    ASSERT(http->cstmHdrs != NULL);

    char toEncode[80];
    char b64str[120];

    strcat(toEncode, user);
    strcat(toEncode, ":");
    strcat(toEncode, pw);
    // endcode to Base64 string
    binToB64(b64str, toEncode, strlen(toEncode));

    ASSERT(strlen(http->cstmHdrs) + 19 + strlen(b64str) < http->cstmHdrsSz);

    strcat(http->cstmHdrs, "Authentication: ");
    strcat(http->cstmHdrs, b64str);
    strcat(http->cstmHdrs, "\r\n");
}


/* ------------------------------------------------------------------------------------------------
 *  Request and Response Section 
 * --------------------------------------------------------------------------------------------- */

/**
 *	@brief Perform HTTP GET operation. Results are internally buffered on the LTEm, see http_read().
 *  ===============================================================================================
 */
resultCode_t http_get(httpCtrl_t *httpCtrl, const char* relativeUrl, bool returnResponseHdrs, uint8_t timeoutSec)
{
    httpCtrl->requestState = httpState_idle;
    httpCtrl->httpStatus = resultCode__unknown;
    strcpy(httpCtrl->requestType, "GET");

    timeoutSec = (timeoutSec == 0) ? http__defaultTimeoutBGxSec : timeoutSec;
    // uint32_t timeoutMS = timeoutSec * 1000;
    resultCode_t atResult;

    if (ATCMD_awaitLock(PERIOD_FROM_SECONDS(timeoutSec)))
    {
        if (returnResponseHdrs)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"responseheader\",%d",  (int)(httpCtrl->returnResponseHdrs));
            atResult = atcmd_awaitResultWithOptions(atcmd__defaultTimeout, NULL);
            if (atResult != resultCode__success)
            {
                atcmd_close();
                return atResult;
            }
        }

        if (httpCtrl->useTls)
        {
            // AT+QHTTPCFG="sslctxid",<httpCtrl->sckt>
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"sslctxid\",%d",  (int)httpCtrl->dataCntxt);
            atResult = atcmd_awaitResult();
            if (atResult != resultCode__success)
            {
                atcmd_close();
                return atResult;
            }
        }

        /* SET URL FOR REQUEST
        * set BGx HTTP URL: AT+QHTTPURL=<urlLength>,timeoutSec  (BGx default timeout is 60, if not specified)
        * wait for CONNECT prompt, then output <URL>, /r/n/r/nOK
        * 
        * NOTE: there is only 1 URL in the BGx at a time
        *---------------------------------------------------------------------------------------------------------------*/
        // if (strlen(relativeUrl) > 0 && relativeUrl[0] != '/')
        //     return resultCode__badRequest;

        char url[240];                                                              // need to get length of URL prior to cmd
        strcpy(url, httpCtrl->hostUrl);
        if (strlen(relativeUrl) > 0 && relativeUrl[0] != '/')
        {
            strcat(url, "/");
        }
        strcat(url, relativeUrl);
        PRINTF(dbgColor__dMagenta, "URL(%d)=%s \r", strlen(url), url);

        atResult = S__setUrl(url, timeoutSec);
        if (atResult != resultCode__success)
        {
            PRINTF(dbgColor__warn, "Failed set URL (%d)\r", atResult);
            atcmd_close();
            return atResult;
        }

        /* INVOKE HTTP GET METHOD
        * BGx responds with OK immediately upon acceptance of cmd, then later (up to timeout) with "+QHTTPGET: " string
        * After "OK" we switch IOP to data mode and return. S_httpDoWork() handles the parsing of the page response and
        * if successful, the issue of the AT+QHTTPREAD command to start the page data stream
        * 
        * This allows other application tasks to be performed while waiting for page. No LTEm commands can be invoked
        * but non-LTEm tasks like reading sensors can continue.
        *---------------------------------------------------------------------------------------------------------------*/

        /* If custom headers, need to both set flag here and include in request stream below
         */
        atcmd_invokeReuseLock("AT+QHTTPCFG=\"requestheader\",%d", httpCtrl->cstmHdrs ? 1 : 0);

        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(timeoutSec), NULL);
        if (atResult != resultCode__success)
        {
            atcmd_close();
            return atResult;
        }

        char httpRequestCmd[http__getRequestLength];
        if (httpCtrl->cstmHdrs)
        {
            char *hostName = strchr(httpCtrl->hostUrl, ':');
            hostName = hostName ? hostName + 3 : httpCtrl->hostUrl;

            char cstmRequest[240];
            snprintf(cstmRequest, sizeof(cstmRequest), "%s %s HTTP/1.1\r\nHost: %s\r\n%s\r\n", httpCtrl->requestType, relativeUrl, hostName, httpCtrl->cstmHdrs);
            PRINTF(dbgColor__dMagenta, "CustomRqst:\r%s\r", cstmRequest);

            atcmd_invokeReuseLock("AT+QHTTPGET=%d,%d", timeoutSec, strlen(cstmRequest));
            atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(5), ATCMD_connectPromptParser);
            if (atResult == resultCode__success)                                                            // "CONNECT" prompt result
            {
                atcmd_reset(false);                                                                         // clear CONNECT event from atcmd results
                atcmd_sendCmdData(cstmRequest, strlen(cstmRequest));
            }
        }
        else
        {
            atcmd_invokeReuseLock("AT+QHTTPGET=%d",  timeoutSec);
            atcmd_sendCmdData("AT+QHTTPGET=30\r", 15);
        }

        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(timeoutSec), S__httpGetStatusParser);                                                                     // wait for "+QHTTPGET trailer (request completed)
        if (atResult == resultCode__success && atcmd_getValue() == 0)
        {
            httpCtrl->httpStatus = S__parseResponseForHttpStatus(httpCtrl, atcmd_getResponse());
            if (httpCtrl->httpStatus >= resultCode__success && httpCtrl->httpStatus <= resultCode__successMax)
            {
                httpCtrl->requestState = httpState_requestComplete;                                         // update httpState, got GET/POST response
                PRINTF(dbgColor__magenta, "GetRqst dCntxt:%d, status=%d\r", httpCtrl->dataCntxt, httpCtrl->httpStatus);
            }
        }
        else
        {
            httpCtrl->requestState = httpState_idle;
            httpCtrl->httpStatus = atcmd_getValue();
            PRINTF(dbgColor__warn, "Closed failed GET request, status=%d %s\r", httpCtrl->httpStatus, atcmd_getErrorDetail());
        }
        atcmd_close();
        return httpCtrl->httpStatus;
    }
    return resultCode__timeout;
}   /* http_get() */



/**
 *	@brief Performs a HTTP POST page web request.
 *  ===============================================================================================
 */
resultCode_t http_post(httpCtrl_t *httpCtrl, const char *relativeUrl, bool returnResponseHdrs, const char *postData, uint16_t postDataSz, uint8_t timeoutSec)
{
    httpCtrl->requestState = httpState_idle;
    httpCtrl->httpStatus = resultCode__unknown;
    strcpy(httpCtrl->requestType, "POST");

    timeoutSec = (timeoutSec == 0) ? http__defaultTimeoutBGxSec : timeoutSec;
    // uint32_t timeoutMS = timeoutSec * 1000;
    resultCode_t atResult;

    if (ATCMD_awaitLock(PERIOD_FROM_SECONDS(timeoutSec)))
    {
        if (returnResponseHdrs)
        {
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"responseheader\",%d",  (int)(httpCtrl->returnResponseHdrs));

            atResult = atcmd_awaitResultWithOptions(atcmd__defaultTimeout, NULL);
            if (atResult != resultCode__success)
            {
                atcmd_close();
                return atResult;
            }
        }

        if (httpCtrl->useTls)
        {
            // AT+QHTTPCFG="sslctxid",<httpCtrl->sckt>
            atcmd_invokeReuseLock("AT+QHTTPCFG=\"sslctxid\",%d",  (int)httpCtrl->dataCntxt);
            atResult = atcmd_awaitResult();
            if (atResult != resultCode__success)
            {
                atcmd_close();
                return atResult;
            }
        }

        /* SET URL FOR REQUEST
        * set BGx HTTP URL: AT+QHTTPURL=<urlLength>,timeoutSec  (BGx default timeout is 60, if not specified)
        * wait for CONNECT prompt, then output <URL>, /r/n/r/nOK
        * 
        * NOTE: there is only 1 URL in the BGx at a time
        *---------------------------------------------------------------------------------------------------------------*/
        char url[240];                                                              // need to get length of URL prior to cmd
        strcpy(url, &httpCtrl->hostUrl);
        if (strlen(relativeUrl) > 0 && relativeUrl[0] != '/')
            strcat(url, "/");
        strcat(url, relativeUrl);
        PRINTF(dbgColor__dMagenta, "URL(%d)=%s \r", strlen(url), url);

        atResult = S__setUrl(url, timeoutSec);
        if (atResult != resultCode__success)
        {
            PRINTF(dbgColor__warn, "Failed set URL (%d)\r", atResult);
            atcmd_close();
            return atResult;
        }

        /* INVOKE HTTP ** POST ** METHOD
        * BGx responds with OK immediately upon acceptance of cmd, then later (up to timeout) with "+QHTTPPOST: " string
        * After "OK" we switch IOP to data mode and return. S_httpDoWork() handles the parsing of the page response and
        * if successful, the issue of the AT+QHTTPREAD command to start the page data stream
        * 
        * This allows other application tasks to be performed while waiting for page. No LTEm commands can be invoked
        * but non-LTEm tasks like reading sensors can continue.
        *---------------------------------------------------------------------------------------------------------------*/
        atcmd_reset(false);                                                                             // reset atCmd control struct WITHOUT clearing lock

        char httpRequestCmd[http__postRequestLength];
        uint16_t httpRequestLength = postDataSz;
        httpRequestLength += (httpCtrl->cstmHdrsSz > 0) ? (httpCtrl->cstmHdrsSz + 2) : 0;               // add the custom headers + 2 for EOL after hdrs

        // snprintf(httpRequestCmd, sizeof(httpRequestCmd), "AT+QHTTPPOST=%d,15,%d", httpRequestLength, timeoutSec);    // AT+QHTTPPOST=<data_length>,<input_time>,<rsptime>
        // atcmd_sendCmdData(httpRequestCmd, strlen(httpRequestCmd), "\r\n");               
        atcmd_invokeReuseLock("AT+QHTTPPOST=%d,30,%d", httpRequestLength, timeoutSec);

        if (atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(timeoutSec), ATCMD_connectPromptParser) == resultCode__success)    // wait for "CONNECT", the status result can be 5,10 seconds or longer
        {
            atcmd_reset(false);                                                                         // clear CONNECT event from atcmd results
            atcmd_sendCmdData(postData, postDataSz);
            atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(timeoutSec), S__httpPostStatusParser);
            if (atResult == resultCode__success && atcmd_getValue() == 0)                               // wait for "+QHTTPPOST trailer
            {
                httpCtrl->httpStatus = S__parseResponseForHttpStatus(httpCtrl, atcmd_getResponse());
                if (httpCtrl->httpStatus >= resultCode__success && httpCtrl->httpStatus <= resultCode__successMax)
                {
                    httpCtrl->requestState = httpState_requestComplete;                                 // update httpState, got GET/POST response
                    PRINTF(dbgColor__magenta, "PostRqst dCntxt:%d, status=%d\r", httpCtrl->dataCntxt, httpCtrl->httpStatus);
                }
            }
            else
            {
                httpCtrl->requestState = httpState_idle;
                httpCtrl->httpStatus = atResult;
                PRINTF(dbgColor__warn, "Closed failed POST request, status=%d (%s)\r", httpCtrl->httpStatus, atcmd_getErrorDetail());
            }
        }
        else
            httpCtrl->httpStatus = resultCode__internalError;
        atcmd_close();
        return httpCtrl->httpStatus;
    }   // awaitLock()

    return resultCode__timeout;
}   /* http_post() */



/**
 *	@brief Retrieves page results from a previous GET or POST.
 *  ===============================================================================================
 */
bool http_readPage(httpCtrl_t *httpCtrl, char *pageBffr, uint16_t pageBffrSz, uint16_t *httpResult)
{
    resultCode_t rslt;
    uint32_t timeoutMS = httpCtrl->defaultTimeoutS * 1000;

    int pages = 0;
    int bytes = 0;

    /* PROCESS ACTIVE DATA STREAM FLOW 
     *-------------------------------------------------------------------------------------------*/
    if (httpCtrl->requestState != httpState_requestComplete)
        return resultCode__preConditionFailed;                                  // readPage() only valid after a completed GET\POST

    // readability var
    cBuffer_t* rxBffr = g_lqLTEM.iop->rxBffr;

    //uint16_t atResult;
    //char grabBffr[20];
    //uint16_t trlrIndx;

    #define CONNECT_SZ 9
    #define CHECKFOR_CONNECT_SZ 15
    #define LOOKBACK_SZ 30
    #define TRAILER_SZ 20


    /* 1st time: Issue HTTP Read command, wait for CONNECT data header, grab any data available and give to appl
     * 2nd+ times: grab any data available and give to appl
     *
     * ISR will be firing and filling buffers, this block marshalls them to application
     * Done when trailer recv'd, progress times out, or cancel is issued
     *  ---------------------------------------------------------------------------------------- */
    if (httpCtrl->requestState == httpState_requestComplete)
    {
        if (atcmd_tryInvoke("AT+QHTTPREAD=%d", httpCtrl->defaultTimeoutS))
        {

            /* Waiting for CONNECT signal, data follows immediately
                ----------------------------------------------------------------------------------- */
            if (httpCtrl->requestState == httpState_requestComplete && 
                cbffr_getOccupied(rxBffr) > CONNECT_SZ)
            {
                if (cbffr_find(rxBffr, "CONNECT\r\n", 0, 0, true) == CBFFR_NOFIND)
                {
                    httpCtrl->httpStatus = resultCode__internalError;
                    return false;
                }
                httpCtrl->requestState = httpState_readingData;
                PRINTF(dbgColor__dMagenta, "httpRead() >>reading\r");
                cbffr_skipTail(rxBffr, CONNECT_SZ);                                         // skip ahead to data past "CONNECT\r\n"
            }
        }
    }

    // HTTP Read trailer: "OK\r\n\r\n+QHTTPREAD: ###\r\n"        Error codes 0, 701-730;  length=21 or 23
    #define READ_TRAILER_SZ 23

    if (httpCtrl->requestState == httpState_readingData)
    {
        do
        {
            /* Check for end-of-page (EOP), stop sending app data at "+QHTTPREAD: <err>" 
                * <err> == 0: no error
            *-------------------------------------------------------------------------------*/
            uint16_t trailerOffset = cbffr_find(rxBffr, "OK\r\n\r\n+QHTTPREAD: ", 0, 0, false);

            if (trailerOffset == CBFFR_NOFIND)                                                      // in the data section of response
            {
                cbffr_popLeave(rxBffr, pageBffr, pageBffrSz, READ_TRAILER_SZ);                      // grab up to a possible trailer
                httpResult = 0;
                return true;                                                                        // more data available, host appl to make additional calls
            }
            if (trailerOffset > pageBffrSz)                                                         // more data in front of trailer than requested buffer size
            {
                cbffr_pop(rxBffr, pageBffr, pageBffrSz);                                            // return data and get trailer on subsequent call
                httpResult = 0;
                return true;
            }

            // trailer found, grab remaining data and parse trailer for http result
            if (cbffr_getOccupied(rxBffr) >= READ_TRAILER_SZ)                                       // complete trailer present, ready to parse
            {
                uint16_t popSz = MIN(pageBffrSz, trailerOffset);
                cbffr_pop(rxBffr, pageBffr, pageBffrSz);


                char parseBffr[READ_TRAILER_SZ - 2];
                cbffr_pop(rxBffr, parseBffr, READ_TRAILER_SZ - 2);                                  // may end in "7xx" or "0\r\n"
                uint32_t httpRslt = strtol(parseBffr + 18, NULL, 10);
                *httpResult = httpRslt == 0 ? resultCode__success : (uint16_t)httpRslt;


                // NEED TO POP DATA TOO
            }



            // if (trlrIndx == CBFFR_NOFIND)                                                    // have not found trailer yet, keep push data to application
            //     dataRemains = cbffr_getBlockRef(rxBffr, &appData, &appDataLen);                 // get pointer to non-wrapped data block
            // else
            // {
            //     cbffr_grab(rxBffr, trlrIndx + TRAILER_SZ, grabBffr, sizeof(grabBffr));       // found trailer need to parse it for http result
            //     httpCtrl->bgxError = strtol(grabBffr, NULL, 10);
            //     httpCtrl->requestState = httpState_closing;
            //     httpCtrl->httpStatus = resultCode__success;
            // }

            // /* Forward data to app
            // *-------------------------------------------------------------------------------*/
            // if (appDataLen > 0)
            // {
            //     if (!httpCtrl->pageCancellation)
            //     {
            //         uint32_t start = pMillis();
            //         ((httpRecv_func)g_lqLTEM.streams[httpCtrl->dataCntxt].recvDataCB)(httpCtrl->dataCntxt, httpCtrl->httpStatus, appData, appDataLen);
            //         uint32_t duration = pMillis() - start;
            //         ASSERT_W(duration <= 5, "HTTP pageCB slow-ovrflw risk")   // BGx out 115200 baud, NXP buffer 60 chars = 5.2mS
            //     }
            //     cbffr_discardBlock(rxBffr);                                                     // delivered, signal block consumed
            // }
            
            if (httpCtrl->requestState == httpState_closing)                                    // if page trailer detected
            {
                PRINTF(dbgColor__magenta, "ReadRqst dCntxt:%d, status=%d\r", httpCtrl->dataCntxt, httpCtrl->httpStatus);
                httpCtrl->requestState = httpState_idle;
                break;
            }

            /* catch REQUEST timeout here, so we don't wait forever.
            *-----------------------------------------------------------------------------------------------------------*/
            if (pElapsed(g_lqLTEM.atcmd->invokedAt, timeoutMS))                                         // if check for request timeout
            {
                httpCtrl->requestState = httpState_closing;
                httpCtrl->httpStatus = resultCode__timeout;
                break;
            }

        } while(true);

    }   // if httpState_readingData

    if (httpCtrl->pageCancellation)
        httpCtrl->httpStatus = resultCode__cancelled;

    atcmd_close();                                                                                  // close the GET\POST request and release atcmd lock
    // g_lqLTEM.iop->rxStreamCtrl = NULL;                                                              // done, take IOP out of stream data mode
    return httpCtrl->httpStatus;
}




#pragma endregion



#pragma region Static Functions
/*-----------------------------------------------------------------------------------------------*/

static uint16_t S__parseResponseForHttpStatus(httpCtrl_t *httpCtrl, const char *response)
{
    char *continueAt = strchr(response, ',');                               // skip ',' and parse http status
    if (continueAt)
    {
        httpCtrl->httpStatus = strtol(++continueAt, &continueAt, 10);
        httpCtrl->pageSize = strtol(++continueAt, &continueAt, 10);         // skip next ',' and parse content length

        httpCtrl->pageRemaining = 0;
        if (httpCtrl->pageSize > 0)
            httpCtrl->pageRemaining = httpCtrl->pageSize;                   // read() will decrement this
    }
    else
        httpCtrl->httpStatus = resultCode__preConditionFailed;
    return httpCtrl->httpStatus;
}


static uint16_t S__setUrl(const char *url, uint16_t timeoutSec)
{
    uint16_t atResult;
    uint8_t urlState = 0;
    atcmd_invokeReuseLock("AT+QHTTPURL=%d,%d", strlen(url), timeoutSec);
    atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(5), ATCMD_connectPromptParser);
    if (atResult == resultCode__success)                                            // waiting for URL prompt "CONNECT"
    {
        urlState++;
        atcmd_reset(false);
        atcmd_sendCmdData(url, strlen(url));                                        // wait for BGx OK (send complete) 
        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(5), NULL);
        if (atResult == resultCode__success)
            urlState++;
    }
    else
        atcmd_restoreOptionDefaults(); 

    return (urlState == 2) ? resultCode__success : atResult;
}

void http_cancelPage(httpCtrl_t *httpCtrl)
{}

#pragma Static Response Parsers

/* Note for parsers below:
 * httprspcode is only reported if err is 0, have to search for finale (\r\n) after a preamble (parser speak)
*/

static cmdParseRslt_t S__httpGetStatusParser() 
{
    // +QHTTPGET: <err>[,<httprspcode>[,<content_length>]]
    return atcmd_stdResponseParser("+QHTTPGET: ", true, ",", 0, 1, "\r\n", 0);
}

static cmdParseRslt_t S__httpPostStatusParser() 
{
    // +QHTTPPOST: <err>[,<httprspcode>[,<content_length>]] 
    return atcmd_stdResponseParser("+QHTTPPOST: ", true, ",", 0, 1, "\r\n", 0);
}

#pragma endregion