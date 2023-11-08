/** ****************************************************************************
  \file 
  \brief Public API persistent file system support
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


#define SRCFILE "FIL"                       // create SRCFILE (3 char) MACRO for lq-diagnostics ASSERT
//#define ENABLE_DIAGPRINT                    // expand DPRINT into debug output
//#define ENABLE_DIAGPRINT_VERBOSE            // expand DPRINT and DPRINT_V into debug output
#define ENABLE_ASSERT
#include <lqdiag.h>

#include "ltemc-internal.h"
#include "ltemc-files.h"

extern ltemDevice_t g_lqLTEM;


#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

/* Local Static Functions
------------------------------------------------------------------------------------------------------------------------- */
static cmdParseRslt_t S__writeStatusParser();
static resultCode_t S__filesRxHndlr();


/**
 *	@brief Set the data callback function for filedata.
 */
void file_setAppReceiver(fileReceiver_func fileReceiver)
{
    ASSERT(fileReceiver != NULL);                                           // assert user provided receiver function

    g_lqLTEM.fileCtrl->streamType = streamType_file;                        // init singleton fileCtrl
    g_lqLTEM.fileCtrl->dataRxHndlr = S__filesRxHndlr;
    g_lqLTEM.fileCtrl->appRecvDataCB = fileReceiver;
}


/**
 *	@brief get filesystem information.
 */
resultCode_t file_getFSInfo(filesysInfo_t * fsInfo)
{
    resultCode_t rslt;
    char *workPtr;

    // if (!ATCMD_awaitLock(atcmd__defaultTimeout))
    //     return resultCode__conflict;                                    // failed to get lock

    do
    {
        // first get file system info
        if (!atcmd_tryInvoke("AT+QFLDS=\"UFS\""))
        rslt = atcmd_awaitResult();
        if (rslt != resultCode__success)
            break;

        // parse response >>  +QFLDS: <freesize>,<total_size>
        workPtr = atcmd_getResponse() + file__dataOffset_info;    // skip past +QFLDS: 
        fsInfo->freeSz = strtol(workPtr, &workPtr, 10);  
        fsInfo->totalSz = strtol(++workPtr, &workPtr, 10);      // incr past comma, then grab total fileSystem size

        // now get file collection info
        if (!atcmd_tryInvoke("AT+QFLDS"))
            return resultCode__conflict;
        rslt = atcmd_awaitResult();
        if (rslt != resultCode__success)
            break;

        // parse response
        // +QFLDS: <freesize>,<total_size>
        workPtr = atcmd_getResponse() + file__dataOffset_info;    // skip past +QFLDS: 
        fsInfo->filesSz = strtol(workPtr, &workPtr, 10);  
        fsInfo->filesCnt = strtol(++workPtr, &workPtr, 10);     // incr past comma, then grab total fileSystem size
    } while (DO_ONCE);

    return rslt;
}


resultCode_t file_getFilelist(fileListResult_t *fileList, const char* filename)
{

    resultCode_t rslt;
    do
    {
        // if (!ATCMD_awaitLock(atcmd__defaultTimeout))
        // {
        //     rslt = resultCode__conflict;                                    // failed to get lock
        //     break;
        // }

        if (strlen(filename) == 0)
        {
            fileList->namePattern[0] = '*';
            fileList->namePattern[1] = '\0';
            if (!atcmd_tryInvoke("AT+QFLST"))
                return resultCode__conflict;
        }
        else
        {
            strncpy(fileList->namePattern, filename, MIN(strlen(filename), file__filenameSz));
            if (!atcmd_tryInvoke("AT+QFLST=\"%s\"", fileList->namePattern))
                return resultCode__conflict;
        }

        rslt = atcmd_awaitResult();
        if (rslt != resultCode__success)
            break;

        // parse response >>  +QFLST: <filename>,<file_size>
        uint8_t lineNm = 0;
        char *workPtr = atcmd_getResponse();

        for (size_t i = 0; i < file__fileListMaxCnt; i++)
        {
            workPtr += 9;
            uint8_t len = strchr(workPtr, '"') - workPtr;
            strncpy(fileList->files[i].filename, workPtr, len);
            workPtr += len + 2;
            fileList->files[i].fileSz = strtol(workPtr, &workPtr, 10);

            workPtr += 2;
            if (*workPtr != '+')
            {
                fileList->fileCnt = ++i;
                break;
            }
        }
    } while (DO_ONCE);

    return rslt;
}


resultCode_t file_open(const char* filename, fileOpenMode_t openMode, uint16_t* fileHandle)
{
    ASSERT(strlen(filename) > 0);                                           // assert user provided a filename
    resultCode_t rslt;
    char *workPtr;

    do
    {
        if (!atcmd_tryInvoke("AT+QFOPEN=\"%s\",%d", filename, openMode))
        {
            return resultCode__conflict;
        }
        
        rslt = atcmd_awaitResult();
        if (rslt != resultCode__success)
        {
            if (rslt == resultCode__cmError)
            {
                uint16_t errDetail = atcmd_getErrorDetailCode();
                if (errDetail == fileErr__detail_fileAlreadyOpen)
                    rslt = fileErr__result_fileAlreadyOpen;
                else if (errDetail == fileErr__detail_fileAlreadyOpen)
                    rslt = fileErr__result_fileAlreadyOpen;
            }
            break;
        }
        // parse response >> +QFOPEN: <filehandle>
        workPtr = atcmd_getResponse() + file__dataOffset_open;
        *fileHandle = strtol(workPtr, NULL, 10);
        break;
    } while (0);

    return rslt;
}


/**
 *	@brief Get a list of open files, including their mode and file handles.
 */
resultCode_t file_getOpenFiles(char *fileInfo, uint16_t fileInfoSz)
{
    if (!atcmd_tryInvoke("AT+QFOPEN?"))
        return resultCode__conflict;

    char* workPtr;
    char* eolPtr;
    memset(fileInfo, 0, fileInfoSz);                            // init for c-str behavior

    if (IS_SUCCESS(atcmd_awaitResult()))
    {
        workPtr = atcmd_getResponse();                          // ptr to response
        while (memcmp(workPtr, "+QFOPEN: ", file__dataOffset_open) == 0)
        {
            workPtr += file__dataOffset_open;
            eolPtr = strchr(workPtr, '\r');
            memcpy(fileInfo, workPtr, eolPtr - workPtr);
            fileInfo += eolPtr - workPtr;
            *fileInfo = '\r';
            fileInfo++;
            workPtr = eolPtr + 2;
        }
        return resultCode__success;
    }
}


/**
 *	@brief Closes the file. 
 */
resultCode_t file_close(uint16_t fileHandle)
{
    if (!atcmd_tryInvoke("AT+QFCLOSE=%d", fileHandle))
        return resultCode__conflict;

    return atcmd_awaitResult();
}


/**
 *	@brief Closes all open files. 
 *  @return ResultCode=200 if successful, otherwise error code (HTTP status type).
 */
resultCode_t file_closeAll()
{
    char openList[file__openFileItemSz * file__openFileMaxCnt];

    resultCode_t _rslt;
    if (IS_NOTSUCCESS_RSLT(file_getOpenFiles(openList, file__openFileItemSz * file__openFileMaxCnt)))
        return _rslt;

    char *workPtr = openList;
    while (*workPtr != 0)
    {
        workPtr = memchr(workPtr, ',', file__openFileItemSz) + 1;
        uint8_t fHandle = strtol(workPtr, &workPtr, 10);
        if (fHandle == 0 || fHandle > file__openFileMaxCnt)
        {
            return resultCode__internalError;
        }
        file_close(fHandle);
        workPtr = memchr(workPtr, '\r', file__openFileItemSz) + 2;
    }
    return resultCode__success;
}



resultCode_t file_read(uint16_t fileHandle, uint16_t requestSz, uint16_t* readSz)
{
    ASSERT(g_lqLTEM.fileCtrl->appRecvDataCB);                                   // assert that there is a app func registered to receive read data
    ASSERT(bbffr_getCapacity(g_lqLTEM.iop->rxBffr) > (requestSz + 128));        // ensure ample space in buffer for I/O

    atcmd_configDataMode(0, "CONNECT", S__filesRxHndlr, NULL, 0, g_lqLTEM.fileCtrl->appRecvDataCB, false);
    atcmd_ovrrdTimeout(2000);

    if (readSz > 0)
    {
        if (!atcmd_tryInvoke("AT+QFREAD=%d,%d", fileHandle, requestSz))
            return resultCode__conflict;
    }
    else
    {
        if (!atcmd_tryInvoke("AT+QFREAD=%d", fileHandle))
            return resultCode__conflict;
    }

    resultCode_t _rslt;
    if (IS_SUCCESS_RSLT(atcmd_awaitResult()))
    {
        g_lqLTEM.fileCtrl->handle = fileHandle;
        
        const char* token = atcmd_getToken(1);
        *readSz = strtol(token, NULL, 10);
        if (*readSz < requestSz)
            _rslt = resultCode__noContent;                                       // content exhausted    
    }
    else 
        *readSz = 0;

    return _rslt;
}


resultCode_t file_write(uint16_t fileHandle, const char* writeData, uint16_t writeSz, fileWriteResult_t *writeResult)
{
    // if (!ATCMD_awaitLock(atcmd__defaultTimeout))
    //     return resultCode__conflict;                                                            // failed to get lock

    do
    {
        atcmd_configDataMode(0, "CONNECT", atcmd_stdTxDataHndlr, writeData, writeSz, NULL, false);

        if (!atcmd_tryInvoke("AT+QFWRITE=%d,%d", fileHandle, writeSz))
            return resultCode__conflict;

        if (IS_SUCCESS(atcmd_awaitResult()))                                                    // "CONNECT" prompt result
        {
            atcmd_reset(false);                                                                 // clear CONNECT event from atcmd results
            // atcmd_sendCmdData(writeData, writeSz);
        }

        resultCode_t _rslt;
        atcmd_ovrrdParser(S__writeStatusParser);
        if (IS_SUCCESS_RSLT(atcmd_awaitResult()))
        {
            char *workPtr;
            writeResult->writtenSz = strtol(atcmd_getResponse(), &workPtr, 10);
            writeResult->fileSz = strtol(++workPtr, NULL, 10);
        }
        else
            return _rslt;

    } while (DO_ONCE);
    return resultCode__success;
}


/**
 *	@brief Set the position of the file pointer.
 */
resultCode_t file_seek(uint16_t fileHandle, uint32_t offset, fileSeekMode_t seekFrom)
{
    if (!atcmd_tryInvoke("AT+QFSEEK=%d,%d,%d", fileHandle, offset, seekFrom))
        return resultCode__conflict;

    return atcmd_awaitResult();
}


resultCode_t file_getPosition(uint16_t fileHandle, uint32_t* filePtr)
{
    resultCode_t rslt;
    char *workPtr;

    if (!atcmd_tryInvoke("AT+QFPOSITION=%d", fileHandle))
        return resultCode__conflict;

    rslt = atcmd_awaitResult();
    if (rslt != resultCode__success)
        return rslt;

    // parse response
    workPtr = atcmd_getResponse() + file__dataOffset_pos;           // skip past preamble 
    *filePtr = strtol(workPtr, NULL, 10);
    return rslt;
}


/**
 *	@brief Truncate all the data beyond the CURRENT position of the file pointer.
 */
resultCode_t file_truncate(uint16_t fileHandle)
{
    if (!atcmd_tryInvokeAdv("AT+QFTUCAT=%d", fileHandle))
        return resultCode__conflict;

    return atcmd_awaitResult();
}


/**
 *	@brief Delete a file from the file system.
 */
resultCode_t file_delete(const char* filename)
{
    if (!atcmd_tryInvoke("AT+QFDEL=\"%s\"", filename))
        return resultCode__conflict;

    return atcmd_awaitResult();
}


void file_getTsFilename(char* tsFilename, uint8_t fnSize, const char* suffix)
{
    char* timestamp[30] = {0};
    char* srcPtr = timestamp;
    char* destPtr = tsFilename;

    ASSERT(fnSize >= strlen(suffix) + 13);                                  // ensure buffer can hold result

    memset(tsFilename, 0, fnSize);
    strcpy(tsFilename, ltem_getUtcDateTime('c'));

    if (strlen(suffix) > 0)                                                 // add suffix, if provided by caller
        strcat(destPtr,suffix);
}


/* Possible future API methods
*/
// fileUploadResult_t file_upload(const char* filename) {}
// fileDownloadResult_t file_download(const char* filename) {}


#pragma Static Helpers and Response Parsers
/*
 * --------------------------------------------------------------------------------------------- */


static cmdParseRslt_t S__writeStatusParser() 
{
    // +QFWRITE: <written_length>,<total_length>
    return atcmd_stdResponseParser("+QFWRITE: ", true, ",", 0, 1, "\r\n", 0);
}


/**
 * @brief File stream RX data handler, marshalls incoming data from RX buffer to app (application).
 * 
 * @return number of bytes read 
 */
static resultCode_t S__filesRxHndlr()
{
    char wrkBffr[32] = {0};
    
    uint8_t popCnt = bbffr_find(g_lqLTEM.iop->rxBffr, "\r", 0, 0, false);
    if (BBFFR_ISNOTFOUND(popCnt))
    {
        return resultCode__notFound;
    }
    
    bbffr_pop(g_lqLTEM.iop->rxBffr, wrkBffr, popCnt + 2);                                                               // pop CONNECT phrase w/ EOL to parse data length
    uint16_t readSz = strtol(wrkBffr + 8, NULL, 10);
    uint16_t availableSz = readSz;
    g_lqLTEM.atcmd->retValue = 0;
    DPRINT_V(PRNT_CYAN, "S__filesRxHndlr() fHandle=%d available=%d\r", g_lqLTEM.fileCtrl->handle, availableSz);

    uint32_t readTimeout = pMillis();
    uint16_t occupiedCnt;
    do
    {
        occupiedCnt = bbffr_getOccupied(g_lqLTEM.iop->rxBffr);
        if (pMillis() - readTimeout > g_lqLTEM.atcmd->timeout)
        {
            g_lqLTEM.atcmd->retValue = 0;
            DPRINT(PRNT_WARN, "S__filesRxHndlr bffr timeout: %d rcvd\r\n", occupiedCnt);
            return resultCode__timeout;                                                                             // return no receive
        }
    } while (occupiedCnt < readSz + file__readTrailerSz);
    
    do                                                                                                              // depending on buffer wrap may take 2 ops
    {
        char* streamPtr;
        uint16_t blockSz = bbffr_popBlock(g_lqLTEM.iop->rxBffr, &streamPtr, readSz);                                // get address from rxBffr
        DPRINT_V(PRNT_CYAN, "S__filesRxHndlr() ptr=%p, bSz=%d, rSz=%d\r", streamPtr, blockSz, readSz);
        ((fileReceiver_func)(*g_lqLTEM.fileCtrl->appRecvDataCB))(g_lqLTEM.fileCtrl->handle, streamPtr, blockSz);    // forward to application
        bbffr_popBlockFinalize(g_lqLTEM.iop->rxBffr, true);                                                         // commit POP
        readSz -= blockSz;
    } while (readSz > 0);

    if (bbffr_getOccupied(g_lqLTEM.iop->rxBffr) >= file__readTrailerSz)                                             // cleanup, remove trailer
    {
        bbffr_skipTail(g_lqLTEM.iop->rxBffr, file__readTrailerSz);
    }
    g_lqLTEM.atcmd->retValue = availableSz;
    return resultCode__success;
}


#pragma endregion