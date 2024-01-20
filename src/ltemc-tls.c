/** ***************************************************************************
  @file ltem-tls.c
  @brief Modem protocol security (SSL/TLS) communication functions/services.

  @author Greg Terrell, LooUQ Incorporated

  \loouq
-------------------------------------------------------------------------------

LooUQ-LTEmC // Software driver for the LooUQ LTEm series cellular modems.
Copyright (C) 2017-2023 LooUQ Incorporated

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
Also add information on how to contact you by electronic and paper mail.

**************************************************************************** */


#include <lq-embed.h>
#define lqLOG_LEVEL lqLOGLEVEL_OFF
//#define DISABLE_ASSERTS                                   // ASSERT/ASSERT_W enabled by default, can be disabled 
#define LQ_SRCFILE "TLS"                                    // create SRCFILE (3 char) MACRO for lq-diagnostics ASSERT

#include "ltemc-internal.h"
#include "ltemc-tls.h"


bool tls_configure(uint8_t dataCntxt, tlsVersion_t version, tlsCipher_t cipherSuite, tlsCertExpiration_t certExpirationCheck, tlsSecurityLevel_t securityLevel)
{
    if (atcmd_tryInvoke("AT+QSSLCFG=\"sslversion\",%d,%d", dataCntxt, version))                   // set SSL/TLS version
    {
        if (atcmd_awaitResult() != resultCode__success)                                         // return on failure, continue on success
            return false;
    }


    if (atcmd_tryInvoke("AT+QSSLCFG=\"ciphersuite\",%d,0X%X", dataCntxt, cipherSuite))            // set cipher suite
    {
        if (atcmd_awaitResult() != resultCode__success)                                         // return on failure, continue on success
            return false;
    }

    if (atcmd_tryInvoke("AT+QSSLCFG=\"ignorelocaltime\",%d,%d", dataCntxt, certExpirationCheck))  // set certificate expiration check
    {
        if (atcmd_awaitResult() != resultCode__success)                                         // return on failure, continue on success
            return false;
    }

    if (atcmd_tryInvoke("AT+QSSLCFG=\"seclevel\",%d,%d", dataCntxt, securityLevel))               // set security level, aka what is checked
    {
        if (atcmd_awaitResult() != resultCode__success)                                         // return on failure, continue on success
            return false;
    }

    return true;
}


/** 
 *  @brief Configure a TLS/SSL control block with current settings
 */
void tls_initControl(tlsCtrl_t* tlsCtrl, tlsVersion_t version, tlsCipher_t cipherSuite, tlsCertExpiration_t certExpirationCheck, tlsSecurityLevel_t securityLevel, bool sniEnabled)
{
    memset(tlsCtrl, 0, sizeof(tlsCtrl_t));
   
    tlsCtrl->version = version;
    tlsCtrl->cipherSuite = cipherSuite;
    tlsCtrl->certExpirationCheck = certExpirationCheck;
    tlsCtrl->securityLevel = securityLevel;
    tlsCtrl->sniEnabled = sniEnabled;
}


tlsOptions_t tlsGetOptions(uint8_t dataCntxt)
{
    tlsOptions_t result = {0};

    if (atcmd_tryInvoke("AT+QSSLCFG=\"sslversion\",%d", (uint8_t)dataCntxt))    // get SSL\TLS version
    {   
        if (atcmd_awaitResult() == resultCode__success)
        {
            DPRINT(PRNT_DEFAULT, "%s", atcmd_getLastResponse());
            // strncpy(result.version, atResult.response);
        }
        atcmd_close();
    }
    return result;
}


resultCode_t tls_enableSni(dataCntxt_t dataCntxt, bool enableSNI)
{
    resultCode_t rslt = resultCode__internalError;

    if (atcmd_tryInvoke("AT+QSSLCFG=\"sni\",%d,%d", dataCntxt, enableSNI))              // set SNI for TLS
    {   
        if (atcmd_awaitResult() == resultCode__success)
        {
            DPRINT(PRNT_DEFAULT, "%s", atcmd_getResponse());
            // strncpy(result.version, atResult.response);
        }
        atcmd_close();
    }
    return rslt;
}


/** 
 *  @brief Apply settings from a TLS/SSL control to a data context.
 */
bool tls_applySettings(dataCntxt_t dataCntxt, tlsCtrl_t* tlsCtrl)
{
    if (atcmd_tryInvoke("AT+QSSLCFG=\"sslversion\",%d,%d", dataCntxt, tlsCtrl->version))                    // set SSL/TLS version
    {
        if (atcmd_awaitResult() != resultCode__success)                                                     // return on failure, continue on success
            return false;
    }
    if (atcmd_tryInvoke("AT+QSSLCFG=\"ciphersuite\",%d,0X%X", dataCntxt, tlsCtrl->cipherSuite))             // set cipher suite
    {
        if (atcmd_awaitResult() != resultCode__success)                                                     // return on failure, continue on success
            return false;
    }
    if (atcmd_tryInvoke("AT+QSSLCFG=\"ignorelocaltime\",%d,%d", dataCntxt, tlsCtrl->certExpirationCheck))   // set certificate expiration check
    {
        if (atcmd_awaitResult() != resultCode__success)                                                     // return on failure, continue on success
            return false;
    }
    if (atcmd_tryInvoke("AT+QSSLCFG=\"seclevel\",%d,%d", dataCntxt, tlsCtrl->securityLevel))                // set security level, aka what is checked
    {
        if (atcmd_awaitResult() != resultCode__success)                                                     // return on failure, continue on success
            return false;
    }

    if (atcmd_tryInvoke("AT+QSSLCFG=\"sni\",%d,%d", dataCntxt, tlsCtrl->sniEnabled))                         // set security level, aka what is checked
    {
        if (atcmd_awaitResult() != resultCode__success)                                                     // return on failure, continue on success
            return false;
    }


    return true;

}
