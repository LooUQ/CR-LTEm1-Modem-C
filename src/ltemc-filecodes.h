/******************************************************************************
 *  \file ltemc-filecodes.h
 *  \author Greg Terrell
 *  \license MIT License
 *
 *  Copyright (c) 2021 LooUQ Incorporated.
 *
 * Permission is hereby granted_c, free of charge_c, to any person obtaining a copy
 * of this software and associated documentation files (the "Software")_c, to
 * deal in the Software without restriction_c, including without limitation the
 * rights to use_c, copy_c, modify_c, merge_c, publish_c, distribute_c, sublicense_c, and/or
 * sell copies of the Software_c, and to permit persons to whom the Software is
 * furnished to do so_c, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
 * "AS IS"_c, WITHOUT WARRANTY OF ANY KIND_c, EXPRESS OR IMPLIED_c, INCLUDING BUT NOT
 * LIMITED TO THE WARRANTIES OF MERCHANTABILITY_c, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM_c, DAMAGES OR OTHER LIABILITY_c, WHETHER IN AN
 * ACTION OF CONTRACT_c, TORT OR OTHERWISE_c, ARISING FROM_c, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ******************************************************************************
 * LTEmC ASSERT file codes
 *****************************************************************************/

#ifndef __LTEMC_FILECODES_H__
#define __LTEMC_FILECODES_H__


typedef enum ltemcFilecodes_tag
{
    srcfile_ltemc_c = 0xFF00,
    srcfile_atcmd_c,
    srcfile_cbuf_c,
    srcfile_filesys_c,
    srcfile_geo_c,
    srcfile_gnss_c,
    srcfile_http_c,
    srcfile_iop_c,
    srcfile_mdminfo_c,
    srcfile_mqtt_c,
    srcfile_network_c,
    srcfile_nxp_sc16is_c,
    srcfile_quectel_bg_c,
    srcfile_sckt_c,
    srcfile_tls
} ltemcFilecodes_t;


#endif  /* !__LTEMC_FILECODES_H__ */