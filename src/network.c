/******************************************************************************
 *  \file network.c
 *  \author Greg Terrell
 *  \license MIT License
 *
 *  Copyright (c) 2020 LooUQ Incorporated.
 *****************************************************************************/

//#define _DEBUG
#include "ltem1c.h"

#define PROTOCOLS_CMD_BUFFER_SZ 80
#define MIN(x, y) (((x)<(y)) ? (x):(y))

#pragma region Static Local Function Declarations
static resultCode_t contextStatusCompleteParser(const char *response, char **endptr);
static networkOperator_t getNetworkOperator();
char *grabToken(char *source, int delimiter, char *tokenBuf, uint8_t tokenBufSz);
#pragma endregion


/* public tcpip functions
 * --------------------------------------------------------------------------------------------- */
#pragma region public functions

/**
 *	\brief Initialize the IP network contexts structure.
 */
void ntwk_create()
{
    network_t *networkPtr = calloc(1, sizeof(network_t));
	if (networkPtr == NULL)
	{
        ltem1_notifyApp(ltem1NotifType_memoryAllocFault,  "Could not alloc network struct");
	}

    networkPtr->networkOperator = calloc(1, sizeof(networkOperator_t));
    pdpCntxt_t *context = calloc(BGX_PDPCONTEXT_COUNT, sizeof(pdpCntxt_t));
	if (context == NULL)
	{
        ltem1_notifyApp(ltem1NotifType_memoryAllocFault, "Could not alloc PDP context struct");
        free(networkPtr);
	}

    for (size_t i = 0; i < IOP_SOCKET_COUNT; i++)
    {   
        networkPtr->pdpCntxts[i].ipType = pdpCntxtIpType_IPV4;
    }
    g_ltem1->network = networkPtr;
}



/**
 *   \brief Wait for a network operator name and network mode. Can be cancelled in threaded env via g_ltem1->cancellationRequest.
 * 
 *   \param waitDuration [in] Number of seconds to wait for a network. Supply 0 for no wait.
 * 
 *   \return Struct containing the network operator name (operName) and network mode (ntwkMode).
*/
networkOperator_t ntwk_awaitOperator(uint16_t waitDuration)
{
    networkOperator_t ntwk;
    unsigned long startMillis, endMillis;

    startMillis = lMillis();
    waitDuration = waitDuration * 1000;
    do 
    {
        ntwk = getNetworkOperator();
        if (ntwk.operName != 0)
            break;
        lDelay(1000);
        endMillis = lMillis();
    } while (endMillis - startMillis < waitDuration || g_ltem1->cancellationRequest);
    //       timed out waiting                      || global cancellation
    return ntwk;
}



/**
 *	\brief Get count of APN active data contexts from BGx.
 * 
 *  \return Count of active data contexts (BGx max is 3).
 */
uint8_t ntwk_getActivePdpCntxtCnt()
{
    #define IP_QIACT_SZ 8

    action_tryInvokeAdv("AT+QIACT?", ACTION_TIMEOUTml, contextStatusCompleteParser);
    actionResult_t atResult = action_awaitResult(false);

    for (size_t i = 0; i < BGX_PDPCONTEXT_COUNT; i++)         // empty context table return and if success refill from parsing
    {
        g_ltem1->network->pdpCntxts[i].contextId = 0;
        g_ltem1->network->pdpCntxts[i].ipAddress[0] = 0;
    }

    if (atResult.statusCode != RESULT_CODE_SUCCESS)
    {
        action_close();
        return 0;
    }

    uint8_t apnIndx = 0;
    if (strlen(atResult.response) > IP_QIACT_SZ)
    {
        #define TOKEN_BUF_SZ 16
        char *nextContext;
        char *landmarkAt;
        char *continueAt;
        uint8_t landmarkSz = IP_QIACT_SZ;
        char tokenBuf[TOKEN_BUF_SZ];

        nextContext = strstr(atResult.response, "+QIACT: ");

        // no contexts returned = none active (only active contexts are returned)
        while (nextContext != NULL)                             // now parse each pdp context entry
        {
            landmarkAt = nextContext;
            g_ltem1->network->pdpCntxts[apnIndx].contextId = strtol(landmarkAt + landmarkSz, &continueAt, 10);
            continueAt = strchr(++continueAt, ',');             // skip context_state: always 1
            g_ltem1->network->pdpCntxts[apnIndx].ipType = (int)strtol(continueAt + 1, &continueAt, 10);

            continueAt = grabToken(continueAt + 2, ASCII_cDBLQUOTE, tokenBuf, TOKEN_BUF_SZ);
            if (continueAt != NULL)
            {
                strncpy(g_ltem1->network->pdpCntxts[apnIndx].ipAddress, tokenBuf, TOKEN_BUF_SZ);
            }
            nextContext = strstr(nextContext + landmarkSz, "+QIACT: ");
            apnIndx++;
        }
    }
    action_close();
    return apnIndx;
}


/**
 *	\brief Get PDP Context\APN information
 * 
 *  \param cntxtId [in] - The PDP context (APN) to retreive.
 * 
 *  \return Pointer to PDP context info in active context table, NULL if not active
 */
pdpCntxt_t *ntwk_getPdpCntxt(uint8_t cntxtId)
{
    for (size_t i = 0; i < BGX_PDPCONTEXT_COUNT; i++)
    {
        if(g_ltem1->network->pdpCntxts[i].contextId != 0)
            return &g_ltem1->network->pdpCntxts[i];
    }
    return NULL;
}


/**
 *	\brief Activate PDP Context\APN.
 * 
 *  \param cntxtId [in] - The APN to operate on. Typically 0 or 1
 */
void ntwk_activatePdpContext(uint8_t cntxtId)
{
    char atCmd[PROTOCOLS_CMD_BUFFER_SZ] = {0};

    snprintf(atCmd, PROTOCOLS_CMD_BUFFER_SZ, "AT+QIACT=%d\r", cntxtId);
    if (action_tryInvokeAdv(atCmd, ACTION_TIMEOUTml, contextStatusCompleteParser))
    {
        resultCode_t atResult = action_awaitResult(true).statusCode;
        if ( atResult == RESULT_CODE_SUCCESS)
            ntwk_getActivePdpCntxtCnt();
    }
}



/**
 *	\brief Deactivate APN.
 * 
 *  \param cntxtId [in] - The APN number to operate on.
 */
void ntwk_deactivatePdpContext(uint8_t cntxtId)
{
    char atCmd[PROTOCOLS_CMD_BUFFER_SZ] = {0};
    snprintf(atCmd, PROTOCOLS_CMD_BUFFER_SZ, "AT+QIDEACT=%d\r", cntxtId);

    if (action_tryInvokeAdv(atCmd, ACTION_TIMEOUTml, contextStatusCompleteParser))
    {
        resultCode_t atResult = action_awaitResult(true).statusCode;
        if ( atResult == RESULT_CODE_SUCCESS)
            ntwk_getActivePdpContexts();
    }
}


/**
 *	\brief Reset (deactivate\activate) all network APNs.
 *
 *  \NOTE: activate and deactivate have side effects, they internally call getActiveContexts prior to return
 */
void ntwk_resetPdpContexts()
{
    uint8_t activeIds[BGX_PDPCONTEXT_COUNT] = {0};

    for (size_t i = 0; i < BGX_PDPCONTEXT_COUNT; i++)                         // preserve initial active APN list
    {
        if(g_ltem1->network->pdpCntxts[i].contextId != 0)
            activeIds[i] = g_ltem1->network->pdpCntxts[i].contextId;
    }
    for (size_t i = 0; i < BGX_PDPCONTEXT_COUNT; i++)                         // now, cycle the active contexts
    {
        if(activeIds[i] != 0)
        {
            ntwk_deactivatePdpContext(activeIds[i]);
            ntwk_activatePdpContext(activeIds[i]);
        }
    }
}

#pragma endregion


/* private functions
 * --------------------------------------------------------------------------------------------- */
#pragma region private functions


/**
 *   \brief Tests for the completion of a network APN context activate action.
 * 
 *   \return standard action result integer (http result).
*/
static resultCode_t contextStatusCompleteParser(const char *response, char **endptr)
{
    return action_defaultResultParser(response, "+QIACT: ", false, 2, ASCII_sOK, endptr);
}



/**
 *   \brief Get the network operator name and network mode.
 * 
 *   \return Struct containing the network operator name (operName) and network mode (ntwkMode).
*/
static networkOperator_t getNetworkOperator()
{
    if (*g_ltem1->network->networkOperator->operName != 0)
        return *g_ltem1->network->networkOperator;

    if (action_tryInvoke("AT+COPS?"))
    {
        actionResult_t atResult = action_awaitResult(false);
        if (atResult.statusCode == RESULT_CODE_SUCCESS)
        {
            char *continueAt;
            uint8_t ntwkMode;
            continueAt = strchr(atResult.response, ASCII_cDBLQUOTE);
            if (continueAt != NULL)
            {
                continueAt = grabToken(continueAt + 1, ASCII_cDBLQUOTE, g_ltem1->network->networkOperator->operName, NTWKOPERATOR_OPERNAME_SZ);
                ntwkMode = (uint8_t)strtol(continueAt + 1, &continueAt, 10);
                if (ntwkMode == 8)
                    strcpy(g_ltem1->network->networkOperator->ntwkMode, "CAT-M1");
                else
                    strcpy(g_ltem1->network->networkOperator->ntwkMode, "CAT-NB1");
            }
        }
        else
        {
            g_ltem1->network->networkOperator->operName[0] = 0;
            g_ltem1->network->networkOperator->ntwkMode[0] = 0;
        }
    }
    action_close();
    return *g_ltem1->network->networkOperator;
}

/**
 *  \brief Scans a C-String (char array) for the next delimeted token and null terminates it.
 * 
 *  \param source [in] - Original char array to scan.
 *  \param delimeter [in] - Character separating tokens (passed as integer value).
 *  \param token [out] - Pointer to where token should be copied to.
 *  \param tokenSz [in] - Size of buffer to receive token.
 * 
 *  \return Pointer to the location in the source string immediately following the token.
*/
char *grabToken(char *source, int delimiter, char *tokenBuf, uint8_t tokenBufSz) 
{
    char *delimAt;
    if (source == NULL)
        return false;

    delimAt = strchr(source, delimiter);
    uint8_t tokenSz = delimAt - source;
    if (tokenSz == 0)
        return NULL;

    memset(tokenBuf, 0, tokenSz);
    strncpy(tokenBuf, source, MIN(tokenSz, tokenBufSz-1));
    return delimAt + 1;
}

#pragma endregion
