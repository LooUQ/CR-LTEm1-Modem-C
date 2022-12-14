/******************************************************************************
 *  \file ltemc-mqtt.c
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
 * MQTT protocol support 
 *****************************************************************************/

#define _DEBUG 2                        // set to non-zero value for PRINTF debugging output, 
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

#include "ltemc-mqtt.h"
#include "ltemc-internal.h"


#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ASCII_CtrlZ_STR "\x1A"
#define ASCII_ESC_STR "\x1B"
#define ASCII_DblQuote_CHAR '"'

// #define MQTT_ACTION_CMD_SZ 81
// #define MQTT_CONNECT_CMD_SZ 300

extern ltemDevice_t g_lqLTEM;

enum
{
    resultCode__parserPending = 0xFFFF
};

/* Local Function Declarations
 ----------------------------------------------------------------------------------------------- */

static void S__updateSubscriptionsTable(mqttCtrl_t *mqttCtrl, bool addSubscription, const char *topic);
static void S__mqttDoWork();
static void S__urlDecode(char *src, int len);

//static cmdParseRslt_t S__mqttOpenStatusParser();
static cmdParseRslt_t S__mqttOpenCompleteParser();
static cmdParseRslt_t S__mqttConnectCompleteParser();
static cmdParseRslt_t S__mqttConnectStatusParser();
static cmdParseRslt_t S__mqttSubscribeCompleteParser();
static cmdParseRslt_t S__mqttPublishCompleteParser();


/* public mqtt functions
 * --------------------------------------------------------------------------------------------- */
#pragma region public functions


/**
 *  \brief Initialize a MQTT protocol control structure.
*/
void mqtt_initControl(mqttCtrl_t *mqttCtrl, uint8_t dataCntxt, uint8_t *recvBuf, uint16_t recvBufSz, mqttRecvFunc_t recvCallback)
{
    ASSERT(mqttCtrl != NULL && recvBuf != NULL, srcfile_ltemc_mqtt_c);
    ASSERT(dataCntxt < dataCntxt__cnt, srcfile_ltemc_mqtt_c);
    ASSERT(((void*)&(mqttCtrl->recvBufCtrl) - (void*)mqttCtrl) == (sizeof(iopStreamCtrl_t) - sizeof(rxDataBufferCtrl_t)), srcfile_ltemc_http_c);

    memset(mqttCtrl, 0, sizeof(mqttCtrl_t));

    mqttCtrl->ctrlMagic = streams__ctrlMagic;
    mqttCtrl->dataCntxt = dataCntxt;
    mqttCtrl->protocol = protocol_mqtt;

    uint16_t bufferSz = IOP_initRxBufferCtrl(&(mqttCtrl->recvBufCtrl), recvBuf, recvBufSz);

    ASSERT_W(recvBufSz == bufferSz, srcfile_ltemc_mqtt_c, "MQTT-RxBufSz not*128B");
    ASSERT(bufferSz > 64, srcfile_ltemc_mqtt_c);

    mqttCtrl->dataRecvCB = recvCallback;
}

/**
 *  \brief Set the remote server connection values.
*/
void mqtt_setConnection(mqttCtrl_t *mqttCtrl, const char *hostUrl, uint16_t hostPort, bool useTls, mqttVersion_t mqttVersion, const char *clientId, const char *username, const char *password)
{
    ASSERT(mqttCtrl != NULL, srcfile_ltemc_mqtt_c);

    strcpy(mqttCtrl->hostUrl, hostUrl);
    mqttCtrl->hostPort = hostPort;
    mqttCtrl->useTls = useTls;
    mqttCtrl->mqttVersion = mqttVersion;

    strncpy(mqttCtrl->clientId, clientId, mqtt__clientIdSz);
    strncpy(mqttCtrl->username, username, mqtt__userNameSz);
    strncpy(mqttCtrl->password, password, mqtt__userPasswordSz);
}


/**
 *  \brief Open a remote MQTT server for use.
*/
resultCode_t mqtt_open(mqttCtrl_t *mqttCtrl)
{
    // AT+QSSLCFG="sslversion",5,3
    // AT+QMTCFG="ssl",5,1,0
    // AT+QMTCFG="version",5,4
    // AT+QMTOPEN=5,"iothub-dev-pelogical.azure-devices.net",8883

    atcmdResult_t atResult;
    mqttCtrl->state = mqtt_getStatus(mqttCtrl);                 // get MQTT state, state must be not open for config changes

    if (mqttCtrl->state >= mqttState_open)                      // already open or connected
        return resultCode__success;

    // set options prior to open
    if (mqttCtrl->useTls)
    {
        if (atcmd_tryInvoke("AT+QMTCFG=\"ssl\",%d,1,%d", mqttCtrl->dataCntxt, mqttCtrl->dataCntxt))
        {
            if (atcmd_awaitResult() != resultCode__success)
                return resultCode__internalError;
        }
    }
    // AT+QMTCFG="version",0,4
    if (atcmd_tryInvoke("AT+QMTCFG=\"version\",%d,4", mqttCtrl->dataCntxt, mqttCtrl->mqttVersion))
    {
        if (atcmd_awaitResult() != resultCode__success)
            return resultCode__internalError;
    }

    // TYPICAL: AT+QMTOPEN=0,"iothub-dev-pelogical.azure-devices.net",8883
    if (atcmd_tryInvoke("AT+QMTOPEN=%d,\"%s\",%d", mqttCtrl->dataCntxt, mqttCtrl->hostUrl, mqttCtrl->hostPort))
    {
        resultCode_t atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(45), S__mqttOpenCompleteParser);
        if (atResult == resultCode__success && atcmd_getValue() == 0)
        {
            mqttCtrl->state = mqttState_open;
            g_lqLTEM.iop->mqttMap |= 0x01 << mqttCtrl->dataCntxt;
            g_lqLTEM.iop->streamPeers[mqttCtrl->dataCntxt] = mqttCtrl;
            LTEM_registerDoWorker(S__mqttDoWork);                                // register background recv worker
            return resultCode__success;
        }
        else
        {
            switch (atcmd_getValue())
            {
                case -1:
                case 1:
                    return resultCode__badRequest;
                case 2:
                    return resultCode__conflict;
                case 4:
                    return resultCode__notFound;
                default:
                    return resultCode__gtwyTimeout;
            }
        }
    }
}


/**
 *  \brief Connect (authenticate) to a MQTT server.
 *  \param mqttCtrl [in] Pointer to MQTT type stream control to operate on.
 *  \param clientId [in] - The client or device identifier for the connection.
 *  \param username [in] - The user identifier or name for the connection to authenticate.
 *  \param password [in] - The secret string or phrase to authenticate the connection.
 *  \param cleanSession [in] - Directs MQTT to preserve or flush messages received prior to the session start.
 *  \return A resultCode_t value indicating the success or type of failure, OK = 200.
*/
resultCode_t mqtt_connect(mqttCtrl_t *mqttCtrl, bool cleanSession)
{
    resultCode_t atResult;
    mqttCtrl->state = mqtt_getStatus(mqttCtrl);
    if (mqttCtrl->state == mqttState_connected)
        return resultCode__success;

    if (atcmd_tryInvoke("AT+QMTCFG=\"session\",%d,%d", mqttCtrl->dataCntxt, (uint8_t)cleanSession))     // set option to clear session history on connect
    {
        if (atcmd_awaitResult() != resultCode__success)
            return resultCode__internalError;
    }

    /* MQTT connect command can be quite large, using local buffer here rather than bloat global cmd\core buffer */
    char connectCmd[384] = {0};
    snprintf(connectCmd, sizeof(connectCmd), "AT+QMTCONN=%d,\"%s\",\"%s\",\"%s\"", mqttCtrl->dataCntxt, mqttCtrl->clientId, mqttCtrl->username, mqttCtrl->password);

    if (ATCMD_awaitLock(atcmd__defaultTimeout))                             // to use oversized MQTT buffer, we need to use sendCmdData()
    {      
        atcmd_reset(false);
        atcmd_sendCmdData(connectCmd, strlen(connectCmd), "\r");
        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(60), S__mqttConnectCompleteParser);     // in autolock mode, so this will release lock
        if (atResult == resultCode__success)
        {
            switch (atcmd_getValue())
            {
                case 0:
                    return resultCode__success;
                case 1:
                    return resultCode__methodNotAllowed;        // invalid protocol version 
                case 2:               
                case 4:
                    return resultCode__unauthorized;            // bad user ID or password
                case 3:
                    return resultCode__unavailable;             // server unavailable
                case 5:
                    return resultCode__forbidden;               // refused, not authorized
                default:
                    return resultCode__internalError;
            }
        }
        // else if (atResult == resultCode__timeout)                           // assume got a +QMTSTAT back not +QMTCONN
        // {
        //     char *continuePtr = strstr(atcmd_getLastResponse(), "+QMTSTAT: ");
        //     if (continuePtr != NULL)
        //     {
        //         continuePtr += 12;
        //         atResult = atol(continuePtr) + 200;
        //     }
        // }
    }
    return resultCode__badRequest;          // bad parameters assumed
}


/**
 *  \brief Subscribe to a topic on the MQTT server.
 *  \param mqttCtrl [in] Pointer to MQTT type stream control to operate on.
 *  \param topic [in] - The messaging topic to subscribe to.
 *  \param qos [in] - The MQTT QOS level for messages subscribed to.
 *  \return The topic index handle. 0xFF indicates the topic subscription was unsuccessful
 */
resultCode_t mqtt_subscribe(mqttCtrl_t *mqttCtrl, const char *topic, mqttQos_t qos)
{
    ASSERT(strlen(topic) < mqtt__topic_nameSz, srcfile_ltemc_mqtt_c);

    resultCode_t atResult = 400;

    // BGx doesn't provide a test for an existing subscription, but is tolerant of a duplicate create subscription 
    // if sucessful, the topic's subscription will overwrite the IOP peer map without issue as well (same bitmap value)

    // snprintf(actionCmd, MQTT_ACTION_CMD_SZ, "AT+QMTSUB=%d,%d,\"%s\",%d", mqtt->sckt, ++mqtt->msgId, topic, qos);
    if (atcmd_tryInvoke("AT+QMTSUB=%d,%d,\"%s\",%d", mqttCtrl->dataCntxt, ++mqttCtrl->lastMsgId, topic, qos))
    {
        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(30), S__mqttSubscribeCompleteParser);
        if (atResult == resultCode__success)
        {
            S__updateSubscriptionsTable(mqttCtrl, true, topic);
        }
    }
    return atResult;
}


/**
 *  \brief Unsubscribe to a topic on the MQTT server.
 *  \param mqttCtrl [in] Pointer to MQTT type stream control to operate on.
 *  \param topic [in] - The messaging topic to unsubscribe from.
 *  \return A resultCode_t (http status type) value indicating the success or type of failure, OK = 200.
*/
resultCode_t mqtt_unsubscribe(mqttCtrl_t *mqttCtrl, const char *topic)
{
    // snprintf(actionCmd, MQTT_CONNECT_CMD_SZ, "AT+QMTUNS=%d,%d,\"<topic1>\"", mqtt->sckt, ++mqtt->msgId, topic);
    if (atcmd_tryInvoke("AT+QMTUNS=%d,%d,\"%s\"", mqttCtrl->dataCntxt, ++mqttCtrl->lastMsgId, topic))
    {
        if (atcmd_awaitResult() == resultCode__success)
        {
            S__updateSubscriptionsTable(mqttCtrl, false, topic);

            g_lqLTEM.iop->mqttMap &= ~(0x01 << mqttCtrl->dataCntxt);
            g_lqLTEM.iop->streamPeers[mqttCtrl->dataCntxt] = NULL;


            // // unregister worker if this is the last subscription being "unsubscribed"
            // if (((iop_t*)g_ltem.iop)->mqttMap == 0)
            //     LTEM_unregisterDoWorker(); 
            return resultCode__success;
        }
    }
    return resultCode__badRequest;
}


/**
 *  \brief Publish an encoded message to server.
 *  \param mqttCtrl [in] Pointer to MQTT type stream control to operate on.
 *  \param topic [in] - Pointer to the message topic (see your server for topic formatting details).
 *  \param qos [in] - The MQTT QOS to be assigned to sent message.
 *  \param encodedMsg [in] - Pointer to message to be sent.
 *  \param timeoutSeconds [in] - Seconds to wait for publish to complete, highly dependent on network and remote server.
 *  \return A resultCode_t value indicating the success or type of failure (http status type code).
*/
resultCode_t mqtt_publishEncoded(mqttCtrl_t *mqttCtrl, const char *topic, mqttQos_t qos, const char *encodedMsg, uint8_t timeoutSeconds)
{
    ASSERT(strlen(encodedMsg) <= 560, srcfile_ltemc_mqtt_c);                                               // max msg length PUBEX=560, PUB=4096
    ASSERT(strchr(encodedMsg, '"') == NULL, srcfile_ltemc_mqtt_c);

    uint32_t timeoutMS = (timeoutSeconds == 0) ? mqtt__publishTimeout : timeoutSeconds * 1000;
    char msgText[mqtt__messageSz];
    resultCode_t atResult = resultCode__conflict;               // assume lock not obtainable, conflict

    mqttCtrl->lastMsgId++;                                                                                      // keep sequence going regardless of MQTT QOS
    uint16_t msgId = ((uint8_t)qos == 0) ? 0 : mqttCtrl->lastMsgId;                                             // msgId not sent with QOS == 0, otherwise sent

    // AT+QMTPUBEX=<tcpconnectID>,<msgID>,<qos>,<retain>,"<topic>","<msg,len<=(560)>"
    if (atcmd_tryInvoke("AT+QMTPUBEX=%d,%d,%d,0,\"%s\",\"%s\"", mqttCtrl->dataCntxt, msgId, qos, topic, encodedMsg))
    {
        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(timeoutSeconds), NULL);
        if (atResult != resultCode__success)                                        
        {
            PRINTF(dbgColor__dYellow, "MQTT-PUB ERROR: rslt=%d(%d)\r", atResult, atcmd_getValue());
        }
    }
    atcmd_close();
    return atResult;
}


/** mqtt_publish()
 *  \brief Publish a message to server.
 *  \param mqttCtrl [in] Pointer to MQTT type stream control to operate on.
 *  \param topic [in] - Pointer to the message topic (see your server for topic formatting details).
 *  \param qos [in] - The MQTT QOS to be assigned to sent message.
 *  \param message [in] - Pointer to message to be sent.
 *  \param timeoutSeconds [in] - Seconds to wait for publish to complete, highly dependent on network and remote server.
 *  \return A resultCode_t value indicating the success or type of failure (http status type code).
*/
resultCode_t mqtt_publish(mqttCtrl_t *mqttCtrl, const char *topic, mqttQos_t qos, const char *message, uint8_t timeoutSeconds)
{
    ASSERT(strlen(message) <= 4096, srcfile_ltemc_mqtt_c);                                               // max msg length PUBEX=560, PUB=4096
    uint8_t pubstate = 0;
    uint32_t timeoutMS = (timeoutSeconds == 0) ? mqtt__publishTimeout : timeoutSeconds * 1000;
    char msgText[mqtt__messageSz];
    resultCode_t atResult = resultCode__conflict;               // assume lock not obtainable, conflict
    if (ATCMD_awaitLock(timeoutMS))
    {
        mqttCtrl->lastMsgId++;                                                                                      // keep sequence going regardless of MQTT QOS
        uint16_t msgId = ((uint8_t)qos == 0) ? 0 : mqttCtrl->lastMsgId;                                             // msgId not sent with QOS == 0, otherwise sent
        // AT+QMTPUB=<tcpconnectID>,<msgID>,<qos>,<retain>,"<topic>"
        atcmd_invokeReuseLock("AT+QMTPUB=%d,%d,%d,0,\"%s\"", mqttCtrl->dataCntxt, msgId, qos, topic);
        pubstate++;
        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(timeoutSeconds), ATCMD_txDataPromptParser);     // wait for data prompt 
        if (atResult == resultCode__success)                                        
        {
            pubstate++;
            atcmd_sendCmdData(message, strlen(message), ASCII_CtrlZ_STR);                                           // now send data
            atResult = atcmd_awaitResultWithOptions(timeoutMS, S__mqttPublishCompleteParser);
            if (atResult == resultCode__success)
            {
                atcmd_close();
                return resultCode__success;
            }
        }
    }
    atcmd_close();
    atcmd_exitTextMode();                                                                                           // if any problem, make sure BGx is out of "text" mode
    PRINTF(dbgColor__dYellow, "MQTT-PUB ERROR: state=%d, rslt=%d(%d)\r", pubstate, atResult, atcmd_getValue());
    return atResult;
}


/**
 *  \brief Disconnect and close a connection to a MQTT server
*/
void mqtt_close(mqttCtrl_t *mqttCtrl)
{
    /*
            mqttCtrl->state = mqttState_open;
            g_lqLTEM.iop->mqttMap |= 0x01 << mqttCtrl->dataCntxt;
            g_lqLTEM.iop->streamPeers[mqttCtrl->dataCntxt] = mqttCtrl;
            LTEM_registerDoWorker(S__mqttDoWork);                                // register background recv worker
    */



    g_lqLTEM.iop->mqttMap &= ~(0x01 & mqttCtrl->dataCntxt);                                     // clear dataCntxt bit in IOP
    g_lqLTEM.iop->streamPeers[mqttCtrl->dataCntxt] = NULL;                                      // remove stream peer reference

    for (size_t i = 0; i < sizeof(mqttCtrl->topicSubs); i++)                                    // clear topic subscriptions table
    {
        mqttCtrl->topicSubs[i].topicName[0] = 0;
    }

    /* not fully documented how Quectel intended to use close/disconnect, LTEmC uses AT+QMTCLOSE which appears to work for both open (NC) and connected states
    */
    if (mqttCtrl->state >= mqttState_open)                                                      // LTEmC uses CLOSE
    {
        if (atcmd_tryInvoke("AT+QMTCLOSE=%d", mqttCtrl->dataCntxt))
            atcmd_awaitResultWithOptions(5000, NULL);
    }
    mqttCtrl->state == mqttState_closed;
    return resultCode__success;
}


/**
 *  \brief Disconnect and close a connection to a MQTT server
 *  \param mqttCtrl [in] Pointer to MQTT type stream control to operate on.
 *  \param resetModem [in] True if modem should be reset prior to reestablishing MQTT connection.
*/
void mqtt_reset(mqttCtrl_t *mqttCtrl, bool resetModem)
{}


/**
 *  \brief Query the status of the MQTT server state.
 *  \param mqttCtrl [in] Pointer to MQTT type stream control to operate on.
 *  \param host [in] A char string to match with the currently connected server. Host is not checked if empty string passed.
 *  \return A mqttState_t value indicating the state of the MQTT connection.
*/
mqttState_t mqtt_getStatus(mqttCtrl_t *mqttCtrl)
{
    // See BG96_MQTT_Application_Note: AT+QMTOPEN? and AT+QMTCONN?

    ASSERT(mqttCtrl->ctrlMagic == streams__ctrlMagic, srcfile_ltemc_mqtt_c);        // assert good MQTT control
    ASSERT(mqttCtrl->dataCntxt < dataCntxt__cnt, srcfile_ltemc_mqtt_c);

    /* BGx modules will not respond over serial to AT+QMTOPEN? (command works fine over USB, Quectel denies this)
     * AT+QMTCONN? returns a state == 1 (MQTT is initializing) when MQTT in an open, not connected condition
    */

    resultCode_t atResult;
    if (atcmd_tryInvoke("AT+QMTCONN?"))
    {
        atResult = atcmd_awaitResultWithOptions(PERIOD_FROM_SECONDS(5), S__mqttConnectStatusParser);
        if (atResult == resultCode__success)
        {
            if (atcmd_getPreambleFound())
            {
                switch (atcmd_getValue())
                {
                    case 1:
                        mqttCtrl->state = mqttState_open;
                        break;
                    case 3:
                        mqttCtrl->state = mqttState_connected;
                        break;
                    default:
                        break;
                }
            }
            else
                mqttCtrl->state = mqttState_closed;
        }
    }
    return mqttCtrl->state;
}


uint16_t mqtt_getLastMsgId(mqttCtrl_t *mqttCtrl)
{
    return mqttCtrl->lastMsgId;
}


uint16_t mqtt_getLastBufferReqd(mqttCtrl_t *mqttCtrl)
{
    return mqttCtrl->lastBufferReqd;
}


#pragma endregion



/* private mqtt functions
 * --------------------------------------------------------------------------------------------- */
#pragma region private functions


static void S__updateSubscriptionsTable(mqttCtrl_t *mqttCtrl, bool addSubscription, const char *topic)
{
    uint8_t topicSz = strlen(topic);
    bool wildcard = *(topic + topicSz - 1) == '#';      // test for MQTT multilevel wildcard, store separately for future topic parsing on recv

    char topicEntryName[mqtt__topic_nameSz] = {0};
    memcpy(topicEntryName, topic, wildcard ? topicSz - 1 : topicSz);

    for (size_t i = 0; i < mqtt__topic_subscriptionCnt; i++)
    {
        if (strcmp(mqttCtrl->topicSubs[i].topicName, topicEntryName) == 0)
        {
            if (!addSubscription)
                mqttCtrl->topicSubs[i].topicName[0] = '\0';
            return;
        }
    }

    if (addSubscription)
    {
        for (size_t i = 0; i < sizeof(mqttCtrl->topicSubs); i++)
        {
            if (mqttCtrl->topicSubs[i].topicName[0] == 0)
            {
                strncpy(mqttCtrl->topicSubs[i].topicName, topicEntryName, strlen(topicEntryName)+1);
                if (wildcard)
                    mqttCtrl->topicSubs[i].wildcard = '#';
                return;
            }
        }
    }
    ASSERT(false, srcfile_ltemc_mqtt_c);                                              // if got here subscription failed and appl is likely unstable
}


/**
 *  \brief Performs background tasks to advance MQTT pipeline dataflows.
*/
static void S__mqttDoWork()
{
    if (g_lqLTEM.iop->rxStreamCtrl != NULL && g_lqLTEM.iop->rxStreamCtrl->protocol == protocol_mqtt)
    {
        /* parse received MQTT message into topic, wildcard (topic vars) and message
         * EXAMPLE: +QMTRECV: 0,0, "topic/wildcard","This is the payload related to topic" */

        char *topicPtr;
        char *topicVarPtr;
        char *messagePtr;
        uint16_t messageSz = 0;

        // readability variables
        mqttCtrl_t *mqttPtr = ((mqttCtrl_t*)g_lqLTEM.iop->rxStreamCtrl);
        rxDataBufferCtrl_t *rxBufPtr = &mqttPtr->recvBufCtrl;

        /* Check for message complete in IOP page, if not record progress for timeout detection and exit 
         * BGx is sloppy on MQTT end-of-message, look for dblQuote + line end ("+CR+LF) */
        char *trailerPtr = lq_strnstr(rxBufPtr->pages[rxBufPtr->iopPg].head - 8, "\"\r\n", 8);
        if (trailerPtr == NULL)
        {
            uint32_t idleTime = IOP_getRxIdleDuration();
            if (idleTime > IOP__rxDefaultTimeout)
            {
                ltem_notifyApp(appEvent_proto_recvFault, "MQTT message recv timeout");
                IOP_resetRxDataBufferPage(rxBufPtr, rxBufPtr->iopPg);                            // clear partial page recv content
                g_lqLTEM.iop->rxStreamCtrl = NULL;                                               // exit data mode
            }
            return;
        }

        // iopPg has trailer text, swap in new page and process (parse) the current iopPg
        mqttPtr->lastBufferReqd = IOP_rxPageDataAvailable(rxBufPtr, rxBufPtr->iopPg);
        IOP_swapRxBufferPage(rxBufPtr);
        uint8_t thisPage = !rxBufPtr->iopPg;
        
        // uint16_t newLen = lq_strUrlDecode(dBufPtr->pages[thisPage]._buffer, mqttPtr->lastBufferReqd);   // un-escape the topic and message recv'd
        // dBufPtr->pages[thisPage].head += newLen - mqttPtr->lastBufferReqd;                      // shorten to UrlDecode adjusted
        // trailerPtr = lq_strnstr(dBufPtr->pages[thisPage].head - 8, "\"\r\n\r\n", 8);            // re-establish trailer pointer

        // ASSERT test for buffer overflow, good buffer has recv header and trailer in same page
        char *continuePtr = lq_strnstr(rxBufPtr->pages[thisPage]._buffer, "+QMTRECV: ", 12);     // allow for leading /r/n
        ASSERT(continuePtr != NULL, srcfile_ltemc_mqtt_c);

        uint16_t msgId = atol(continuePtr + 12);
        topicPtr = memchr(rxBufPtr->pages[thisPage].tail, '"', mqtt__topic_offset);
        if (topicPtr == NULL)                                                                   // malformed
        {
            IOP_resetRxDataBufferPage(rxBufPtr, thisPage);                                      // done with this page, clear it
            g_lqLTEM.iop->rxStreamCtrl = NULL;                                                        // exit data mode
            return;
        }                                                             
        rxBufPtr->pages[thisPage].tail = ++topicPtr;                                            // point past opening dblQuote
        rxBufPtr->pages[thisPage].tail = memchr(rxBufPtr->pages[thisPage].tail, '"', IOP_rxPageDataAvailable(rxBufPtr,thisPage));
        if (rxBufPtr->pages[thisPage].tail == NULL)                                             // malformed, overflowed somehow
        {
            IOP_resetRxDataBufferPage(rxBufPtr, thisPage);                                      // done with this page, clear it
            g_lqLTEM.iop->rxStreamCtrl = NULL;                                                        // exit data mode
            return;
        }

        messagePtr = rxBufPtr->pages[thisPage].tail += 3;                                       // set the message start
        messageSz = trailerPtr - messagePtr;
        *trailerPtr = '\0';                                                                     // null term the message (remove BGx trailing "\r\n)

        /* find topic in subscriptions array & invoke application receiver */
        for (size_t i = 0; i < mqtt__topic_subscriptionCnt; i++)
        {
            uint16_t topicSz = strlen(mqttPtr->topicSubs[i].topicName);
            if (topicSz > 0 && strncmp(mqttPtr->topicSubs[i].topicName, topicPtr, topicSz) == 0)
            {
                if (topicPtr + topicSz + 3 < messagePtr)                                        // test for topic wildcard
                {
                    topicVarPtr = topicPtr + topicSz;                                           // props or other variable data at end of topic (null-terminated)                    
                    *(topicVarPtr - 1) = '\0';                                                  // null-term topic : overwrite \ separating topic and wildcard (topic vars)
                }
                else
                {
                    topicVarPtr = NULL;                                                         // no topic wildcard\variables
                }
                *(messagePtr - 3) = '\0';                                                       // null-term topicVars
                mqttPtr->dataRecvCB(mqttPtr->dataCntxt, msgId, topicPtr, topicVarPtr, messagePtr, messageSz);
                IOP_resetRxDataBufferPage(rxBufPtr, thisPage);                                  // done with this page, clear it
                g_lqLTEM.iop->rxStreamCtrl = NULL;                                              // exit data mode
                return;
            }
        }
        // finally:
        //     IOP_resetRxDataBufferPage(rxBufPtr, thisPage);                                      // done with this page, clear it
        //     iopPtr->rxStreamCtrl = NULL;                                                        // exit data mode
    }
}


#pragma endregion



/* MQTT ATCMD Parsers
 * --------------------------------------------------------------------------------------------- */
#pragma region MQTT ATCMD Parsers
// /**
//  *	\brief [private] MQTT open status response parser.
//  *
//  *  \param response [in] Character data recv'd from BGx to parse for task complete
//  *  \param endptr [out] Char pointer to the char following parsed text
//  * 
//  *  \return HTTP style result code, 0 = not complete
//  */
// static cmdParseRslt_t S__mqttOpenStatusParser() 
// {
//     cmdParseRslt_t parserRslt = atcmd_stdResponseParser("+QMTOPEN: ", false, ",", 0, 3, "OK\r\n", 0);

//     // cmdParseRslt_t parserRslt = atcmd_stdResponseParser("+QMTOPEN: ", true, NULL, 1, 1, NULL, 0);
//     // resultCode_t parserResult = atcmd_serviceResponseParserTerm(response, "+QMTOPEN: ", 0, "\r\n", endptr);

//     // if (parserResult > resultCode__successMax && strstr(response, "OK\r\n"))                        // if no QMTOPEN and OK: not connected
//     //     return resultCode__notFound;

//     return parserRslt;
// }


/**
 *	\brief [private] MQTT open connection response parser.
 *  \return LTEmC parse result
 */
static cmdParseRslt_t S__mqttOpenCompleteParser() 
{
    cmdParseRslt_t parserRslt = atcmd_stdResponseParser("+QMTOPEN: ", true, ",", 0, 2, "", 0);
    return parserRslt;
}


/**
 *	\brief [private] MQTT connect to server response parser.
 *  \return LTEmC parse result
 */
static cmdParseRslt_t S__mqttConnectCompleteParser() 
{
    // return ATCMD_testResponseTrace();
    return atcmd_stdResponseParser("+QMTCONN: ", true, ",", 0, 3, "", 0);
}


/**
 *	\brief [private] MQTT connect to server response parser.
 *  \return LTEmC parse result
 */
static cmdParseRslt_t S__mqttConnectStatusParser() 
{
    // BGx +QMTCONN Returns status: 1 = connecting, 3 = connected. Service parser returns 200 + status.
    // A simple "OK" response indicates no connection
    return atcmd_stdResponseParser("+QMTCONN: ", false, ",", 0, 2, "OK\r\n", 0);
}


/**
 *	\brief [private] MQTT subscribe to topic response parser.
 *  \return LTEmC parse result
 */
static cmdParseRslt_t S__mqttSubscribeCompleteParser() 
{
    return atcmd_stdResponseParser("+QMTSUB: ", true, ",", 0, 2, "", 0);
}


/**
 *	\brief [private] MQTT publish message to topic response parser.
 *  \return LTEmC parse result
 */
static cmdParseRslt_t S__mqttPublishCompleteParser() 
{
    return atcmd_stdResponseParser("+QMTPUB: ", true, ",", 0, 2, "", 0);
}

/**
 *	\brief [private] MQTT publish message to topic response parser.
 *  \return LTEmC parse result
 */
static cmdParseRslt_t S__mqttCloseCompleteParser() 
{
    return atcmd_stdResponseParser("OK\r\n\r\n+QMTCLOSE: ", true, ",", 0, 2, "", 0);
}


#pragma endregion

