/*
 *  SSLv3/TLSv1 shared functions
 *
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */
/*
 *  The SSL 3.0 specification was drafted by Netscape in 1996,
 *  and became an IETF standard in 1999.
 *
 *  http://wp.netscape.com/eng/ssl3/
 *  http://www.ietf.org/rfc/rfc2246.txt
 *  http://www.ietf.org/rfc/rfc4346.txt
 */

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_SSL_TLS_C)

#include <stdint.h>

#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#include <stdlib.h>
#define mbedtls_calloc    calloc
#define mbedtls_free      free
#endif

#include "mbedtls/debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_internal.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/version.h"
#include "mbedtls/platform.h"

#include <string.h>

#if defined(MBEDTLS_X509_CRT_PARSE_C)
#include "mbedtls/oid.h"
#endif

#define PROPER_HS_FRAGMENT 0x75

#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
static int mbedtls_ssl_switch_key( mbedtls_ssl_transform *transform,
                                   const mbedtls_operation_t operation )
{
    unsigned char * key;
    int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
    volatile int flow_ctrl = 0;
#if defined(MBEDTLS_VALIDATE_SSL_KEYS_INTEGRITY)
    uint32_t hash;
#endif
    if( operation == MBEDTLS_ENCRYPT )
    {
        flow_ctrl++;
        key = transform->key_enc;
#if defined(MBEDTLS_VALIDATE_SSL_KEYS_INTEGRITY)
        hash = transform->key_enc_hash;
#endif
    }
    else if ( operation == MBEDTLS_DECRYPT )
    {
        flow_ctrl++;
        key = transform->key_dec;
#if defined(MBEDTLS_VALIDATE_SSL_KEYS_INTEGRITY)
        hash = transform->key_dec_hash;
#endif
    }
    else
    {
        return ( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }
#if defined(MBEDTLS_VALIDATE_SSL_KEYS_INTEGRITY)
    /* Check hash */
    if( hash != mbedtls_hash( key, transform->key_bitlen >> 3 ) )
    {
        return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
    }
    else
    {
        flow_ctrl++;
    }
#else
    flow_ctrl++;
#endif
    if( operation != transform->cipher_ctx.operation )
    {
        if( ( ret = mbedtls_cipher_setkey( &transform->cipher_ctx,
                                           key,
                                           transform->key_bitlen,
                                           operation ) ) != 0 )
        {
            return( ret );
        }
    }
    if( flow_ctrl == 2 )
    {
        return( 0 );
    }
    return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
}
#endif

#if defined(MBEDTLS_USE_TINYCRYPT)
static int uecc_rng_wrapper( uint8_t *dest, unsigned int size )
{
    int ret;
    ret = mbedtls_ssl_conf_rng_func( NULL, dest, size );
    if( ret == 0 )
        return( (int) size );

    return( 0 );
}

int mbedtls_ssl_ecdh_read_peerkey( mbedtls_ssl_context *ssl,
                                   unsigned char **p, unsigned char *end )
{
    size_t const secp256r1_uncompressed_point_length =
        1 /* length */ + 1 /* length */ + 2 * NUM_ECC_BYTES /* data */;

    if( (size_t)( end - *p ) < secp256r1_uncompressed_point_length )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Bad ECDH peer pubkey (too short)" ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    if( (*p)[0] != 2 * NUM_ECC_BYTES + 1 ||
        (*p)[1] != 0x04 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Unexpected ECDH peer pubkey header - expected { %#02x, %#02x }, got { %#02x, %#02x }",
                               2 * NUM_ECC_BYTES + 1,
                               0x04,
                               (unsigned) (*p)[0],
                               (unsigned) (*p)[1] ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    if( mbedtls_platform_memcpy( ssl->handshake->ecdh_peerkey, *p + 2, 2 * NUM_ECC_BYTES ) !=
                                 ssl->handshake->ecdh_peerkey )
    {
        return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
    }

    *p += secp256r1_uncompressed_point_length;
    return( 0 );
}
#endif /* MBEDTLS_USE_TINYCRYPT */

static void ssl_reset_in_out_pointers( mbedtls_ssl_context *ssl );
static uint32_t ssl_get_hs_total_len( mbedtls_ssl_context const *ssl );

/* Length of the "epoch" field in the record header */
static inline size_t ssl_ep_len( const mbedtls_ssl_context *ssl )
{
#if !defined(MBEDTLS_SSL_TRANSPORT__BOTH)
    ((void) ssl);
#endif

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        return( 2 );
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS)
    return( 0 );
#endif
}

static void ssl_send_pending_fatal_alert( mbedtls_ssl_context *ssl )
{
    if( ssl->pending_fatal_alert_msg == MBEDTLS_SSL_ALERT_MSG_NONE )
        return;

    mbedtls_ssl_send_alert_message( ssl,
                                    MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                                    ssl->pending_fatal_alert_msg );
    ssl->pending_fatal_alert_msg = MBEDTLS_SSL_ALERT_MSG_NONE;
}

/*
 * Start a timer.
 * Passing millisecs = 0 cancels a running timer.
 */
static void ssl_set_timer( mbedtls_ssl_context *ssl, uint32_t millisecs )
{
    if( mbedtls_ssl_get_set_timer( ssl ) == NULL )
        return;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "set_timer to %d ms", (int) millisecs ) );
    mbedtls_ssl_get_set_timer( ssl )( ssl->p_timer,
                                           millisecs / 4,
                                           millisecs );
}

/*
 * Return -1 is timer is expired, 0 if it isn't.
 */
static int ssl_check_timer( mbedtls_ssl_context *ssl )
{
    if( mbedtls_ssl_get_get_timer( ssl ) == NULL )
        return( 0 );

    if( mbedtls_ssl_get_get_timer( ssl )( ssl->p_timer ) == 2 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "timer expired" ) );
        return( -1 );
    }

    return( 0 );
}

static void ssl_update_out_pointers( mbedtls_ssl_context *ssl,
                                     mbedtls_ssl_transform *transform );
static void ssl_update_in_pointers( mbedtls_ssl_context *ssl );

#if defined(MBEDTLS_SSL_RECORD_CHECKING)
static int ssl_parse_record_header( mbedtls_ssl_context const *ssl,
                                    unsigned char *buf,
                                    size_t len,
                                    mbedtls_record *rec );

int mbedtls_ssl_check_record( mbedtls_ssl_context const *ssl,
                              unsigned char *buf,
                              size_t buflen )
{
    int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
    mbedtls_record rec;
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "=> mbedtls_ssl_check_record" ) );
    MBEDTLS_SSL_DEBUG_BUF( 3, "record buffer", buf, buflen );

    /* We don't support record checking in TLS because
     * (a) there doesn't seem to be a usecase for it, and
     * (b) In SSLv3 and TLS 1.0, CBC record decryption has state
     *     and we'd need to backup the transform here.
     */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_TLS( ssl->conf->transport ) )
    {
        ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;
        goto exit;
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_TLS */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
   {
       ret = ssl_parse_record_header( ssl, buf, buflen, &rec );
       if( ret != 0 )
       {
           MBEDTLS_SSL_DEBUG_RET( 3, "ssl_parse_record_header", ret );
           goto exit;
       }

       if( ssl->transform_in != NULL )
       {
           ret = mbedtls_ssl_decrypt_buf( ssl, ssl->transform_in, &rec );
           if( ret != 0 )
           {
               MBEDTLS_SSL_DEBUG_RET( 3, "mbedtls_ssl_decrypt_buf", ret );
               goto exit;
           }
       }
   }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

exit:
    /* On success, we have decrypted the buffer in-place, so make
     * sure we don't leak any plaintext data. */
    mbedtls_platform_zeroize( buf, buflen );

    /* For the purpose of this API, treat messages with unexpected CID
     * as well as such from future epochs as unexpected. */
    if( ret == MBEDTLS_ERR_SSL_UNEXPECTED_CID ||
        ret == MBEDTLS_ERR_SSL_EARLY_MESSAGE )
    {
        ret = MBEDTLS_ERR_SSL_UNEXPECTED_RECORD;
    }

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "<= mbedtls_ssl_check_record" ) );
    return( ret );
}
#endif /* MBEDTLS_SSL_RECORD_CHECKING */

#define SSL_DONT_FORCE_FLUSH 0
#define SSL_FORCE_FLUSH      1

#if defined(MBEDTLS_SSL_PROTO_DTLS)

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
/* Top-level Connection ID API */

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID) && \
    !defined(MBEDTLS_SSL_CONF_CID_LEN) &&      \
    !defined(MBEDTLS_SSL_CONF_IGNORE_UNEXPECTED_CID)
int mbedtls_ssl_conf_cid( mbedtls_ssl_config *conf,
                          size_t len,
                          int ignore_other_cid )
{
    if( len > MBEDTLS_SSL_CID_IN_LEN_MAX )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    if( ignore_other_cid != MBEDTLS_SSL_UNEXPECTED_CID_FAIL &&
        ignore_other_cid != MBEDTLS_SSL_UNEXPECTED_CID_IGNORE )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    conf->ignore_unexpected_cid = ignore_other_cid;
    conf->cid_len = len;
    return( 0 );
}
#else  /* MBEDTLS_SSL_DTLS_CONNECTION_ID &&
          !MBEDTLS_SSL_CONF_CID_LEN &&
          !MBEDTLS_SSL_CONF_IGNORE_UNEXPECTED_CID */

#if MBEDTLS_SSL_CONF_CID_LEN > MBEDTLS_SSL_CID_IN_LEN_MAX
#error "Invalid hardcoded value for MBEDTLS_SSL_CONF_CID_LEN"
#endif
#if MBEDTLS_SSL_CONF_IGNORE_UNEXPECTED_CID != MBEDTLS_SSL_UNEXPECTED_CID_IGNORE && \
    MBEDTLS_SSL_CONF_IGNORE_UNEXPECTED_CID != MBEDTLS_SSL_UNEXPECTED_CID_FAIL
#error "Invalid hardcoded value for MBEDTLS_SSL_CONF_IGNORE_UNEXPECTED_CID"
#endif

#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID &&
          !MBEDTLS_SSL_CONF_CID_LEN &&
          !MBEDTLS_SSL_CONF_IGNORE_UNEXPECTED_CID */

int mbedtls_ssl_set_cid( mbedtls_ssl_context *ssl,
                         int enable,
                         unsigned char const *own_cid,
                         size_t own_cid_len )
{
    if( MBEDTLS_SSL_TRANSPORT_IS_TLS( ssl->conf->transport ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ssl->negotiate_cid = enable;
    if( enable == MBEDTLS_SSL_CID_DISABLED )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Disable use of CID extension." ) );
        return( 0 );
    }
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "Enable use of CID extension." ) );
    MBEDTLS_SSL_DEBUG_BUF( 3, "Own CID", own_cid, own_cid_len );

    if( own_cid_len != mbedtls_ssl_conf_get_cid_len( ssl->conf ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "CID length %u does not match CID length %u in config",
                                    (unsigned) own_cid_len,
                                    (unsigned) mbedtls_ssl_conf_get_cid_len( ssl->conf ) ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    /* Not using more secure mbedtls_platform_memcpy as cid is public */
    memcpy( ssl->own_cid, own_cid, own_cid_len );
    /* Truncation is not an issue here because
     * MBEDTLS_SSL_CID_IN_LEN_MAX at most 255. */
    ssl->own_cid_len = (uint8_t) own_cid_len;

    return( 0 );
}

int mbedtls_ssl_get_peer_cid( mbedtls_ssl_context *ssl,
                     int *enabled,
                     unsigned char peer_cid[ MBEDTLS_SSL_CID_OUT_LEN_MAX ],
                     size_t *peer_cid_len )
{
    *enabled = MBEDTLS_SSL_CID_DISABLED;

    if( MBEDTLS_SSL_TRANSPORT_IS_TLS( ssl->conf->transport ) ||
        ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    /* We report MBEDTLS_SSL_CID_DISABLED in case the CID extensions
     * were used, but client and server requested the empty CID.
     * This is indistinguishable from not using the CID extension
     * in the first place. */
    if( ssl->transform_in->in_cid_len  == 0 &&
        ssl->transform_in->out_cid_len == 0 )
    {
        return( 0 );
    }

    if( peer_cid_len != NULL )
    {
        *peer_cid_len = ssl->transform_in->out_cid_len;
        if( peer_cid != NULL )
        {
            /* Not using more secure mbedtls_platform_memcpy as cid is public */
            memcpy( peer_cid, ssl->transform_in->out_cid,
                    ssl->transform_in->out_cid_len );
        }
    }

    *enabled = MBEDTLS_SSL_CID_ENABLED;

    return( 0 );
}
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

/* Forward declarations for functions related to message buffering. */
static void ssl_buffering_free( mbedtls_ssl_context *ssl );
static void ssl_buffering_free_slot( mbedtls_ssl_context *ssl,
                                     uint8_t slot );
static void ssl_free_buffered_record( mbedtls_ssl_context *ssl );
static int ssl_load_buffered_message( mbedtls_ssl_context *ssl );
static int ssl_load_buffered_record( mbedtls_ssl_context *ssl );
static int ssl_buffer_message( mbedtls_ssl_context *ssl );
static int ssl_buffer_future_record( mbedtls_ssl_context *ssl,
                                     mbedtls_record const *rec );
static int ssl_next_record_is_in_datagram( mbedtls_ssl_context *ssl );

static size_t ssl_get_current_mtu( const mbedtls_ssl_context *ssl );
static size_t ssl_get_maximum_datagram_size( mbedtls_ssl_context const *ssl )
{
    size_t mtu = ssl_get_current_mtu( ssl );
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    size_t out_buf_len = ssl->out_buf_len;
#else
    size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;
#endif

    if( mtu != 0 && mtu < out_buf_len )
        return( mtu );

    return( out_buf_len );
}

static int ssl_get_remaining_space_in_datagram( mbedtls_ssl_context const *ssl )
{
    size_t const bytes_written = ssl->out_left;
    size_t const mtu           = ssl_get_maximum_datagram_size( ssl );

    /* Double-check that the write-index hasn't gone
     * past what we can transmit in a single datagram. */
    if( bytes_written > mtu )
    {
        /* Should never happen... */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    return( (int) ( mtu - bytes_written ) );
}

static int ssl_get_remaining_payload_in_datagram( mbedtls_ssl_context const *ssl )
{
    int ret;
    size_t remaining, expansion;
    size_t max_len = MBEDTLS_SSL_OUT_CONTENT_LEN;

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
    const size_t mfl = mbedtls_ssl_get_output_max_frag_len( ssl );

    if( max_len > mfl )
        max_len = mfl;

    /* By the standard (RFC 6066 Sect. 4), the MFL extension
     * only limits the maximum record payload size, so in theory
     * we would be allowed to pack multiple records of payload size
     * MFL into a single datagram. However, this would mean that there's
     * no way to explicitly communicate MTU restrictions to the peer.
     *
     * The following reduction of max_len makes sure that we never
     * write datagrams larger than MFL + Record Expansion Overhead.
     */
    if( max_len <= ssl->out_left )
        return( 0 );

    max_len -= ssl->out_left;
#endif

    ret = ssl_get_remaining_space_in_datagram( ssl );
    if( ret < 0 )
        return( ret );
    remaining = (size_t) ret;

    ret = mbedtls_ssl_get_record_expansion( ssl );
    if( ret < 0 )
        return( ret );
    expansion = (size_t) ret;

    if( remaining <= expansion )
        return( 0 );

    remaining -= expansion;
    if( remaining >= max_len )
        remaining = max_len;

    return( (int) remaining );
}

/*
 * Double the retransmit timeout value, within the allowed range,
 * returning -1 if the maximum value has already been reached.
 */
static int ssl_double_retransmit_timeout( mbedtls_ssl_context *ssl )
{
    uint32_t new_timeout;

    if( ssl->handshake->retransmit_timeout >=
        mbedtls_ssl_conf_get_hs_timeout_max( ssl->conf ) )
    {
        return( -1 );
    }

    /* Implement the final paragraph of RFC 6347 section 4.1.1.1
     * in the following way: after the initial transmission and a first
     * retransmission, back off to a temporary estimated MTU of 508 bytes.
     * This value is guaranteed to be deliverable (if not guaranteed to be
     * delivered) of any compliant IPv4 (and IPv6) network, and should work
     * on most non-IP stacks too. */
    if( ssl->handshake->retransmit_timeout !=
        mbedtls_ssl_conf_get_hs_timeout_min( ssl->conf ) )
    {
        ssl->handshake->mtu = 508;
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "mtu autoreduction to %d bytes", ssl->handshake->mtu ) );
    }

    new_timeout = 2 * ssl->handshake->retransmit_timeout;

    /* Avoid arithmetic overflow and range overflow */
    if( new_timeout < ssl->handshake->retransmit_timeout ||
        new_timeout > mbedtls_ssl_conf_get_hs_timeout_max( ssl->conf ) )
    {
        new_timeout = mbedtls_ssl_conf_get_hs_timeout_max( ssl->conf );
    }

    ssl->handshake->retransmit_timeout = new_timeout;
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "update timeout value to %d millisecs",
                        ssl->handshake->retransmit_timeout ) );

    return( 0 );
}

static void ssl_reset_retransmit_timeout( mbedtls_ssl_context *ssl )
{
    ssl->handshake->retransmit_timeout = mbedtls_ssl_conf_get_hs_timeout_min( ssl->conf );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "update timeout value to %d millisecs",
                        ssl->handshake->retransmit_timeout ) );
}
#endif /* MBEDTLS_SSL_PROTO_DTLS */

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
/*
 * Convert max_fragment_length codes to length.
 * RFC 6066 says:
 *    enum{
 *        2^9(1), 2^10(2), 2^11(3), 2^12(4), (255)
 *    } MaxFragmentLength;
 * and we add 0 -> extension unused
 */
static unsigned int ssl_mfl_code_to_length( int mfl )
{
    switch( mfl )
    {
    case MBEDTLS_SSL_MAX_FRAG_LEN_NONE:
        return( MBEDTLS_TLS_EXT_ADV_CONTENT_LEN );
    case MBEDTLS_SSL_MAX_FRAG_LEN_512:
        return 512;
    case MBEDTLS_SSL_MAX_FRAG_LEN_1024:
        return 1024;
    case MBEDTLS_SSL_MAX_FRAG_LEN_2048:
        return 2048;
    case MBEDTLS_SSL_MAX_FRAG_LEN_4096:
        return 4096;
    default:
        return( MBEDTLS_TLS_EXT_ADV_CONTENT_LEN );
    }
}
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

int mbedtls_ssl_session_copy( mbedtls_ssl_session *dst,
                              const mbedtls_ssl_session *src )
{
    mbedtls_ssl_session_free( dst );
    mbedtls_platform_memcpy( dst, src, sizeof( mbedtls_ssl_session ) );

#if defined(MBEDTLS_X509_CRT_PARSE_C)

#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    if( src->peer_cert != NULL )
    {
        int ret;

        dst->peer_cert = mbedtls_calloc( 1, sizeof(mbedtls_x509_crt) );
        if( dst->peer_cert == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        mbedtls_x509_crt_init( dst->peer_cert );

        if( ( ret = mbedtls_x509_crt_parse_der( dst->peer_cert, src->peer_cert->raw.p,
                                        src->peer_cert->raw.len ) ) != 0 )
        {
            mbedtls_free( dst->peer_cert );
            dst->peer_cert = NULL;
            return( ret );
        }
    }
#elif defined(MBEDTLS_SSL_RENEGOTIATION)
    if( src->peer_cert_digest != NULL )
    {
        dst->peer_cert_digest =
            mbedtls_calloc( 1, src->peer_cert_digest_len );
        if( dst->peer_cert_digest == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        mbedtls_platform_memcpy( dst->peer_cert_digest, src->peer_cert_digest,
                src->peer_cert_digest_len );
        dst->peer_cert_digest_type = src->peer_cert_digest_type;
        dst->peer_cert_digest_len = src->peer_cert_digest_len;
    }
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE && MBEDTLS_SSL_RENEGOTIATION */

#endif /* MBEDTLS_X509_CRT_PARSE_C */

#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
    if( src->ticket != NULL )
    {
        dst->ticket = mbedtls_calloc( 1, src->ticket_len );
        if( dst->ticket == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        mbedtls_platform_memcpy( dst->ticket, src->ticket, src->ticket_len );
    }
#endif /* MBEDTLS_SSL_SESSION_TICKETS && MBEDTLS_SSL_CLI_C */

    return( 0 );
}

#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
static int resize_buffer( unsigned char **buffer, size_t len_new, size_t *len_old )
{
    unsigned char* resized_buffer = mbedtls_calloc( 1, len_new );
    if( resized_buffer == NULL )
        return -1;

    /* We want to copy len_new bytes when downsizing the buffer, and
     * len_old bytes when upsizing, so we choose the smaller of two sizes,
     * to fit one buffer into another. Size checks, ensuring that no data is
     * lost, are done outside of this function. */
    memcpy( resized_buffer, *buffer,
            ( len_new < *len_old ) ? len_new : *len_old );
    mbedtls_platform_zeroize( *buffer, *len_old );
    mbedtls_free( *buffer );

    *buffer = resized_buffer;
    *len_old = len_new;

    return 0;
}

#define BUFFER_UPSIZING 0
#define BUFFER_DOWNSIZING 1
static void handle_buffer_resizing( mbedtls_ssl_context *ssl, int downsizing,
                                    uint32_t in_buf_new_len,
                                    uint32_t out_buf_new_len )
{
    int modified = 0;
    size_t written_in = 0, len_offset_in = 0;
    size_t written_out = 0, iv_offset_out = 0, len_offset_out = 0;
    if( ssl->in_buf != NULL )
    {
        written_in = ssl->in_msg - ssl->in_buf;
        len_offset_in = ssl->in_len - ssl->in_buf;
        if( ( downsizing && ssl->in_buf_len > in_buf_new_len && ssl->in_left < in_buf_new_len ) ||
            ( !downsizing && ssl->in_buf_len < in_buf_new_len ) )
        {
            if( resize_buffer( &ssl->in_buf, in_buf_new_len, &ssl->in_buf_len ) != 0 )
            {
                /* No need to return an error here; The buffer will remain as
                 * is with no negative impact on the flow. */
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "input buffer resizing failed - out of memory" ) );
            }
            else
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "Reallocating in_buf to %d", in_buf_new_len ) );
                modified = 1;
            }
        }
    }

    if( ssl->out_buf != NULL )
    {
        written_out = ssl->out_msg - ssl->out_buf;
        iv_offset_out = ssl->out_iv - ssl->out_buf;
        len_offset_out = ssl->out_len - ssl->out_buf;
        if( ( downsizing && ssl->out_buf_len > out_buf_new_len && ssl->out_left < out_buf_new_len ) ||
            ( !downsizing && ssl->out_buf_len < out_buf_new_len ) )
        {
            if( resize_buffer( &ssl->out_buf, out_buf_new_len, &ssl->out_buf_len ) != 0 )
            {
                /* No need to return an error here; The buffer will remain as
                 * is with no negative impact on the flow. */
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "output buffer resizing failed - out of memory" ) );
            }
            else
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "Reallocating out_buf to %d", out_buf_new_len ) );
                modified = 1;
            }
        }
    }
    if( modified )
    {
        /* Update pointers here to avoid doing it twice. */
        ssl_reset_in_out_pointers( ssl );
        /* Fields below might not be properly updated with record
         * splitting or with CID, so they are manually updated here. */
        ssl->out_msg = ssl->out_buf + written_out;
        ssl->out_len = ssl->out_buf + len_offset_out;
        ssl->out_iv = ssl->out_buf + iv_offset_out;

        ssl->in_msg = ssl->in_buf + written_in;
        ssl->in_len = ssl->in_buf + len_offset_in;
    }
}
#endif /* MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH */

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
int (*mbedtls_ssl_hw_record_init)( mbedtls_ssl_context *ssl,
                     const unsigned char *key_enc, const unsigned char *key_dec,
                     size_t keylen,
                     const unsigned char *iv_enc,  const unsigned char *iv_dec,
                     size_t ivlen,
                     const unsigned char *mac_enc, const unsigned char *mac_dec,
                     size_t maclen ) = NULL;
int (*mbedtls_ssl_hw_record_activate)( mbedtls_ssl_context *ssl, int direction) = NULL;
int (*mbedtls_ssl_hw_record_reset)( mbedtls_ssl_context *ssl ) = NULL;
int (*mbedtls_ssl_hw_record_write)( mbedtls_ssl_context *ssl ) = NULL;
int (*mbedtls_ssl_hw_record_read)( mbedtls_ssl_context *ssl ) = NULL;
int (*mbedtls_ssl_hw_record_finish)( mbedtls_ssl_context *ssl ) = NULL;
#endif /* MBEDTLS_SSL_HW_RECORD_ACCEL */

/*
 * Key material generation
 */
#if defined(MBEDTLS_SSL_PROTO_SSL3)
MBEDTLS_NO_INLINE static int ssl3_prf( const unsigned char *secret, size_t slen,
                     const char *label,
                     const unsigned char *random, size_t rlen,
                     unsigned char *dstbuf, size_t dlen )
{
    int ret = 0;
    size_t i;
    mbedtls_md5_context md5;
    mbedtls_sha1_context sha1;
    unsigned char padding[16];
    unsigned char sha1sum[20];
    ((void)label);

    mbedtls_md5_init(  &md5  );
    mbedtls_sha1_init( &sha1 );

    /*
     *  SSLv3:
     *    block =
     *      MD5( secret + SHA1( 'A'    + secret + random ) ) +
     *      MD5( secret + SHA1( 'BB'   + secret + random ) ) +
     *      MD5( secret + SHA1( 'CCC'  + secret + random ) ) +
     *      ...
     */
    for( i = 0; i < dlen / 16; i++ )
    {
        mbedtls_platform_memset( padding, (unsigned char) ('A' + i), 1 + i );

        if( ( ret = mbedtls_sha1_starts_ret( &sha1 ) ) != 0 )
            goto exit;
        if( ( ret = mbedtls_sha1_update_ret( &sha1, padding, 1 + i ) ) != 0 )
            goto exit;
        if( ( ret = mbedtls_sha1_update_ret( &sha1, secret, slen ) ) != 0 )
            goto exit;
        if( ( ret = mbedtls_sha1_update_ret( &sha1, random, rlen ) ) != 0 )
            goto exit;
        if( ( ret = mbedtls_sha1_finish_ret( &sha1, sha1sum ) ) != 0 )
            goto exit;

        if( ( ret = mbedtls_md5_starts_ret( &md5 ) ) != 0 )
            goto exit;
        if( ( ret = mbedtls_md5_update_ret( &md5, secret, slen ) ) != 0 )
            goto exit;
        if( ( ret = mbedtls_md5_update_ret( &md5, sha1sum, 20 ) ) != 0 )
            goto exit;
        if( ( ret = mbedtls_md5_finish_ret( &md5, dstbuf + i * 16 ) ) != 0 )
            goto exit;
    }

exit:
    mbedtls_md5_free(  &md5  );
    mbedtls_sha1_free( &sha1 );

    mbedtls_platform_zeroize( padding, sizeof( padding ) );
    mbedtls_platform_zeroize( sha1sum, sizeof( sha1sum ) );

    return( ret );
}
#endif /* MBEDTLS_SSL_PROTO_SSL3 */

#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
MBEDTLS_NO_INLINE static int tls1_prf( const unsigned char *secret, size_t slen,
                     const char *label,
                     const unsigned char *random, size_t rlen,
                     unsigned char *dstbuf, size_t dlen )
{
    size_t nb, hs;
    size_t i, j, k;
    const unsigned char *S1, *S2;
    unsigned char tmp[128];
    unsigned char h_i[20];
    mbedtls_md_handle_t md_info;
    mbedtls_md_context_t md_ctx;
    int ret;

    mbedtls_md_init( &md_ctx );

    if( sizeof( tmp ) < 20 + strlen( label ) + rlen )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    hs = ( slen + 1 ) / 2;
    S1 = secret;
    S2 = secret + slen - hs;

    nb = strlen( label );
    mbedtls_platform_memcpy( tmp + 20, label, nb );
    mbedtls_platform_memcpy( tmp + 20 + nb, random, rlen );
    nb += rlen;

    /*
     * First compute P_md5(secret,label+random)[0..dlen]
     */
    if( ( md_info = mbedtls_md_info_from_type( MBEDTLS_MD_MD5 ) ) ==
        MBEDTLS_MD_INVALID_HANDLE )
    {
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    if( ( ret = mbedtls_md_setup( &md_ctx, md_info, 1 ) ) != 0 )
        return( ret );

    mbedtls_md_hmac_starts( &md_ctx, S1, hs );
    mbedtls_md_hmac_update( &md_ctx, tmp + 20, nb );
    mbedtls_md_hmac_finish( &md_ctx, 4 + tmp );

    for( i = 0; i < dlen; i += 16 )
    {
        mbedtls_md_hmac_reset ( &md_ctx );
        mbedtls_md_hmac_update( &md_ctx, 4 + tmp, 16 + nb );
        mbedtls_md_hmac_finish( &md_ctx, h_i );

        mbedtls_md_hmac_reset ( &md_ctx );
        mbedtls_md_hmac_update( &md_ctx, 4 + tmp, 16 );
        mbedtls_md_hmac_finish( &md_ctx, 4 + tmp );

        k = ( i + 16 > dlen ) ? dlen % 16 : 16;

        for( j = 0; j < k; j++ )
            dstbuf[i + j]  = h_i[j];
    }

    mbedtls_md_free( &md_ctx );

    /*
     * XOR out with P_sha1(secret,label+random)[0..dlen]
     */
    if( ( md_info = mbedtls_md_info_from_type( MBEDTLS_MD_SHA1 ) ) ==
        MBEDTLS_MD_INVALID_HANDLE )
    {
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    if( ( ret = mbedtls_md_setup( &md_ctx, md_info, 1 ) ) != 0 )
        return( ret );

    mbedtls_md_hmac_starts( &md_ctx, S2, hs );
    mbedtls_md_hmac_update( &md_ctx, tmp + 20, nb );
    mbedtls_md_hmac_finish( &md_ctx, tmp );

    for( i = 0; i < dlen; i += 20 )
    {
        mbedtls_md_hmac_reset ( &md_ctx );
        mbedtls_md_hmac_update( &md_ctx, tmp, 20 + nb );
        mbedtls_md_hmac_finish( &md_ctx, h_i );

        mbedtls_md_hmac_reset ( &md_ctx );
        mbedtls_md_hmac_update( &md_ctx, tmp, 20 );
        mbedtls_md_hmac_finish( &md_ctx, tmp );

        k = ( i + 20 > dlen ) ? dlen % 20 : 20;

        for( j = 0; j < k; j++ )
            dstbuf[i + j] = (unsigned char)( dstbuf[i + j] ^ h_i[j] );
    }

    mbedtls_md_free( &md_ctx );

    mbedtls_platform_zeroize( tmp, sizeof( tmp ) );
    mbedtls_platform_zeroize( h_i, sizeof( h_i ) );

    return( 0 );
}
#endif /* MBEDTLS_SSL_PROTO_TLS1) || MBEDTLS_SSL_PROTO_TLS1_1 */

#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if !( defined(MBEDTLS_SHA256_C) && defined(MBEDTLS_SHA512_C) )
MBEDTLS_ALWAYS_INLINE static inline
#else
static
#endif
int tls_prf_generic( mbedtls_md_type_t md_type,
                            const unsigned char *secret, size_t slen,
                            const char *label,
                            const unsigned char *random, size_t rlen,
                            unsigned char *dstbuf, size_t dlen )
{
    size_t nb;
    size_t i, j, k, md_len;
    unsigned char tmp[128] = {0};
    unsigned char h_i[MBEDTLS_MD_MAX_SIZE] = {0};
    mbedtls_md_handle_t md_info;
    mbedtls_md_context_t md_ctx;
    int ret;

    mbedtls_md_init( &md_ctx );

    if( ( md_info = mbedtls_md_info_from_type( md_type ) ) ==
        MBEDTLS_MD_INVALID_HANDLE )
    {
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    md_len = mbedtls_md_get_size( md_info );

    if( sizeof( tmp ) < md_len + strlen( label ) + rlen )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    nb = strlen( label );
    (void)mbedtls_platform_memcpy( tmp + md_len, label, nb );
    (void)mbedtls_platform_memcpy( tmp + md_len + nb, random, rlen );
    nb += rlen;

    /*
     * Compute P_<hash>(secret, label + random)[0..dlen]
     */
    if ( ( ret = mbedtls_md_setup( &md_ctx, md_info, 1 ) ) != 0 )
        return( ret );

    if ( ( ret = mbedtls_md_hmac_starts( &md_ctx, secret, slen ) ) != 0 )
        return( ret );
    if ( ( ret = mbedtls_md_hmac_update( &md_ctx, tmp + md_len, nb ) ) != 0 )
        return( ret );
    if ( ( ret = mbedtls_md_hmac_finish( &md_ctx, tmp ) ) != 0 )
        return( ret );

    for( i = 0; i < dlen; i += md_len )
    {
        if ( ( ret = mbedtls_md_hmac_reset ( &md_ctx ) ) != 0 )
            return( ret );
        if ( ( ret = mbedtls_md_hmac_update( &md_ctx, tmp, md_len + nb ) ) != 0 )
            return( ret );
        if ( ( ret = mbedtls_md_hmac_finish( &md_ctx, h_i ) ) != 0 )
            return( ret );

        if ( ( ret = mbedtls_md_hmac_reset ( &md_ctx ) ) != 0 )
            return( ret );
        if ( ( ret = mbedtls_md_hmac_update( &md_ctx, tmp, md_len ) ) != 0 )
            return( ret );
        if ( ( ret = mbedtls_md_hmac_finish( &md_ctx, tmp ) ) != 0 )
            return( ret );

        k = ( i + md_len > dlen ) ? dlen % md_len : md_len;

        for( j = 0; j < k; j++ )
            dstbuf[i + j]  = h_i[j];
    }

    mbedtls_md_free( &md_ctx );

    (void)mbedtls_platform_zeroize( tmp, sizeof( tmp ) );
    (void)mbedtls_platform_zeroize( h_i, sizeof( h_i ) );

    return( 0 );
}

#if defined(MBEDTLS_SHA256_C)
MBEDTLS_NO_INLINE static int tls_prf_sha256(
                           const unsigned char *secret, size_t slen,
                           const char *label,
                           const unsigned char *random, size_t rlen,
                           unsigned char *dstbuf, size_t dlen )
{
    return( tls_prf_generic( MBEDTLS_MD_SHA256, secret, slen,
                             label, random, rlen, dstbuf, dlen ) );
}
#endif /* MBEDTLS_SHA256_C */

#if defined(MBEDTLS_SHA512_C)
MBEDTLS_NO_INLINE static int tls_prf_sha384(
                           const unsigned char *secret, size_t slen,
                           const char *label,
                           const unsigned char *random, size_t rlen,
                           unsigned char *dstbuf, size_t dlen )
{
    return( tls_prf_generic( MBEDTLS_MD_SHA384, secret, slen,
                             label, random, rlen, dstbuf, dlen ) );
}
#endif /* MBEDTLS_SHA512_C */
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */

/*
 * Call the appropriate PRF function
 */
MBEDTLS_ALWAYS_INLINE static inline int ssl_prf( int minor_ver,
                    mbedtls_md_type_t hash,
                    const unsigned char *secret, size_t slen,
                    const char *label,
                    const unsigned char *random, size_t rlen,
                    unsigned char *dstbuf, size_t dlen )
{
#if !defined(MBEDTLS_SSL_PROTO_TLS1_2) || !defined(MBEDTLS_SHA512_C)
    (void) hash;
#endif

#if defined(MBEDTLS_SSL_PROTO_SSL3)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_0 )
        return( ssl3_prf( secret, slen, label, random, rlen, dstbuf, dlen ) );
    else
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
    if( mbedtls_ssl_ver_lt( minor_ver, MBEDTLS_SSL_MINOR_VERSION_3 ) )
        return( tls1_prf( secret, slen, label, random, rlen, dstbuf, dlen ) );
    else
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA512_C)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_3 &&
        hash == MBEDTLS_MD_SHA384 )
    {
        return( tls_prf_sha384( secret, slen, label, random, rlen,
                                dstbuf, dlen ) );
    }
    else
#endif
#if defined(MBEDTLS_SHA256_C)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_3 )
    {
        return( tls_prf_sha256( secret, slen, label, random, rlen,
                                dstbuf, dlen ) );
    }
#endif
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */

    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
}

#if defined(MBEDTLS_SSL_PROTO_SSL3)
MBEDTLS_NO_INLINE static void ssl_calc_finished_ssl(
                mbedtls_ssl_context *ssl, unsigned char *buf, int from )
{
    const char *sender;
    mbedtls_md5_context  md5;
    mbedtls_sha1_context sha1;

    unsigned char padbuf[48];
    unsigned char md5sum[16];
    unsigned char sha1sum[20];

    mbedtls_ssl_session *session = ssl->session_negotiate;
    if( !session )
        session = ssl->session;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc  finished ssl" ) );

    mbedtls_md5_init( &md5 );
    mbedtls_sha1_init( &sha1 );

    mbedtls_md5_clone( &md5, &ssl->handshake->fin_md5 );
    mbedtls_sha1_clone( &sha1, &ssl->handshake->fin_sha1 );

    /*
     * SSLv3:
     *   hash =
     *      MD5( master + pad2 +
     *          MD5( handshake + sender + master + pad1 ) )
     *   + SHA1( master + pad2 +
     *         SHA1( handshake + sender + master + pad1 ) )
     */

#if !defined(MBEDTLS_MD5_ALT)
    MBEDTLS_SSL_DEBUG_BUF( 4, "finished  md5 state", (unsigned char *)
                    md5.state, sizeof(  md5.state ) );
#endif

#if !defined(MBEDTLS_SHA1_ALT)
    MBEDTLS_SSL_DEBUG_BUF( 4, "finished sha1 state", (unsigned char *)
                   sha1.state, sizeof( sha1.state ) );
#endif

    sender = ( from == MBEDTLS_SSL_IS_CLIENT ) ? "CLNT"
                                       : "SRVR";

    mbedtls_platform_memset( padbuf, 0x36, 48 );

    mbedtls_md5_update_ret( &md5, (const unsigned char *) sender, 4 );
    mbedtls_md5_update_ret( &md5, session->master, 48 );
    mbedtls_md5_update_ret( &md5, padbuf, 48 );
    mbedtls_md5_finish_ret( &md5, md5sum );

    mbedtls_sha1_update_ret( &sha1, (const unsigned char *) sender, 4 );
    mbedtls_sha1_update_ret( &sha1, session->master, 48 );
    mbedtls_sha1_update_ret( &sha1, padbuf, 40 );
    mbedtls_sha1_finish_ret( &sha1, sha1sum );

    mbedtls_platform_memset( padbuf, 0x5C, 48 );

    mbedtls_md5_starts_ret( &md5 );
    mbedtls_md5_update_ret( &md5, session->master, 48 );
    mbedtls_md5_update_ret( &md5, padbuf, 48 );
    mbedtls_md5_update_ret( &md5, md5sum, 16 );
    mbedtls_md5_finish_ret( &md5, buf );

    mbedtls_sha1_starts_ret( &sha1 );
    mbedtls_sha1_update_ret( &sha1, session->master, 48 );
    mbedtls_sha1_update_ret( &sha1, padbuf , 40 );
    mbedtls_sha1_update_ret( &sha1, sha1sum, 20 );
    mbedtls_sha1_finish_ret( &sha1, buf + 16 );

    MBEDTLS_SSL_DEBUG_BUF( 3, "calc finished result", buf, 36 );

    mbedtls_md5_free(  &md5  );
    mbedtls_sha1_free( &sha1 );

    mbedtls_platform_zeroize(  padbuf, sizeof(  padbuf ) );
    mbedtls_platform_zeroize(  md5sum, sizeof(  md5sum ) );
    mbedtls_platform_zeroize( sha1sum, sizeof( sha1sum ) );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc  finished" ) );
}
#endif /* MBEDTLS_SSL_PROTO_SSL3 */

#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
MBEDTLS_NO_INLINE static void ssl_calc_finished_tls(
                mbedtls_ssl_context *ssl, unsigned char *buf, int from )
{
    int len = 12;
    const char *sender;
    mbedtls_md5_context  md5;
    mbedtls_sha1_context sha1;
    unsigned char padbuf[36];

    mbedtls_ssl_session *session = ssl->session_negotiate;
    if( !session )
        session = ssl->session;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc  finished tls" ) );

    mbedtls_md5_init( &md5 );
    mbedtls_sha1_init( &sha1 );

    mbedtls_md5_clone( &md5, &ssl->handshake->fin_md5 );
    mbedtls_sha1_clone( &sha1, &ssl->handshake->fin_sha1 );

    /*
     * TLSv1:
     *   hash = PRF( master, finished_label,
     *               MD5( handshake ) + SHA1( handshake ) )[0..11]
     */

#if !defined(MBEDTLS_MD5_ALT)
    MBEDTLS_SSL_DEBUG_BUF( 4, "finished  md5 state", (unsigned char *)
                    md5.state, sizeof(  md5.state ) );
#endif

#if !defined(MBEDTLS_SHA1_ALT)
    MBEDTLS_SSL_DEBUG_BUF( 4, "finished sha1 state", (unsigned char *)
                   sha1.state, sizeof( sha1.state ) );
#endif

    sender = ( from == MBEDTLS_SSL_IS_CLIENT )
             ? "client finished"
             : "server finished";

    mbedtls_md5_finish_ret(  &md5, padbuf );
    mbedtls_sha1_finish_ret( &sha1, padbuf + 16 );

    ssl_prf( mbedtls_ssl_get_minor_ver( ssl ),
             mbedtls_ssl_suite_get_mac(
                 mbedtls_ssl_ciphersuite_from_id(
                     mbedtls_ssl_session_get_ciphersuite( session ) ) ),
             session->master, 48, sender,
             padbuf, 36, buf, len );

    MBEDTLS_SSL_DEBUG_BUF( 3, "calc finished result", buf, len );

    mbedtls_md5_free(  &md5  );
    mbedtls_sha1_free( &sha1 );

    mbedtls_platform_zeroize(  padbuf, sizeof(  padbuf ) );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc  finished" ) );
}
#endif /* MBEDTLS_SSL_PROTO_TLS1 || MBEDTLS_SSL_PROTO_TLS1_1 */

#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA256_C)
MBEDTLS_NO_INLINE static void ssl_calc_finished_tls_sha256(
                mbedtls_ssl_context *ssl, unsigned char *buf, int from )
{
    int len = 12;
    const char *sender;
    mbedtls_sha256_context sha256;
    unsigned char padbuf[32];

    mbedtls_ssl_session *session = ssl->session_negotiate;
    if( !session )
        session = ssl->session;

    mbedtls_sha256_init( &sha256 );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc  finished tls sha256" ) );

    mbedtls_sha256_clone( &sha256, &ssl->handshake->fin_sha256 );

    /*
     * TLSv1.2:
     *   hash = PRF( master, finished_label,
     *               Hash( handshake ) )[0.11]
     */

#if !defined(MBEDTLS_SHA256_ALT)
    MBEDTLS_SSL_DEBUG_BUF( 4, "finished sha2 state", (unsigned char *)
                   sha256.state, sizeof( sha256.state ) );
#endif

    sender = ( from == MBEDTLS_SSL_IS_CLIENT )
             ? "client finished"
             : "server finished";

    mbedtls_sha256_finish_ret( &sha256, padbuf );

    ssl_prf( mbedtls_ssl_get_minor_ver( ssl ),
             mbedtls_ssl_suite_get_mac(
                 mbedtls_ssl_ciphersuite_from_id(
                     mbedtls_ssl_session_get_ciphersuite( session ) ) ),
             session->master, 48, sender,
             padbuf, 32, buf, len );

    MBEDTLS_SSL_DEBUG_BUF( 3, "calc finished result", buf, len );

    mbedtls_sha256_free( &sha256 );

    mbedtls_platform_zeroize(  padbuf, sizeof(  padbuf ) );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc  finished" ) );
}
#endif /* MBEDTLS_SHA256_C */

#if defined(MBEDTLS_SHA512_C)
MBEDTLS_NO_INLINE static void ssl_calc_finished_tls_sha384(
                mbedtls_ssl_context *ssl, unsigned char *buf, int from )
{
    int len = 12;
    const char *sender;
    mbedtls_sha512_context sha512;
    unsigned char padbuf[48];

    mbedtls_ssl_session *session = ssl->session_negotiate;
    if( !session )
        session = ssl->session;

    mbedtls_sha512_init( &sha512 );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc  finished tls sha384" ) );

    mbedtls_sha512_clone( &sha512, &ssl->handshake->fin_sha512 );

    /*
     * TLSv1.2:
     *   hash = PRF( master, finished_label,
     *               Hash( handshake ) )[0.11]
     */

#if !defined(MBEDTLS_SHA512_ALT)
    MBEDTLS_SSL_DEBUG_BUF( 4, "finished sha512 state", (unsigned char *)
                   sha512.state, sizeof( sha512.state ) );
#endif

    sender = ( from == MBEDTLS_SSL_IS_CLIENT )
             ? "client finished"
             : "server finished";

    mbedtls_sha512_finish_ret( &sha512, padbuf );

    ssl_prf( mbedtls_ssl_get_minor_ver( ssl ),
             mbedtls_ssl_suite_get_mac(
                 mbedtls_ssl_ciphersuite_from_id(
                     mbedtls_ssl_session_get_ciphersuite( session ) ) ),
             session->master, 48, sender,
             padbuf, 48, buf, len );

    MBEDTLS_SSL_DEBUG_BUF( 3, "calc finished result", buf, len );

    mbedtls_sha512_free( &sha512 );

    mbedtls_platform_zeroize(  padbuf, sizeof( padbuf ) );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc  finished" ) );
}
#endif /* MBEDTLS_SHA512_C */
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */

MBEDTLS_ALWAYS_INLINE static inline int ssl_calc_finished(
                                     int minor_ver,
                                     mbedtls_md_type_t hash,
                                     mbedtls_ssl_context *ssl,
                                     unsigned char *buf,
                                     int from )
{
#if !defined(MBEDTLS_SSL_PROTO_TLS1_2) || !defined(MBEDTLS_SHA512_C)
    (void) hash;
#endif

#if defined(MBEDTLS_SSL_PROTO_SSL3)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_0 )
        ssl_calc_finished_ssl( ssl, buf, from );
    else
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
    if( mbedtls_ssl_ver_lt( minor_ver, MBEDTLS_SSL_MINOR_VERSION_3 ) )
        ssl_calc_finished_tls( ssl, buf, from );
    else
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA512_C)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_3 &&
        hash == MBEDTLS_MD_SHA384 )
    {
        ssl_calc_finished_tls_sha384( ssl, buf, from );
    }
    else
#endif
#if defined(MBEDTLS_SHA256_C)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_3 )
        ssl_calc_finished_tls_sha256( ssl, buf, from );
    else
#endif
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    return( 0 );
}

/*
 * Populate a transform structure with session keys and all the other
 * necessary information.
 *
 * Parameters:
 * - [in/out]: transform: structure to populate
 *      [in] must be just initialised with mbedtls_ssl_transform_init()
 *      [out] fully populated, ready for use by mbedtls_ssl_{en,de}crypt_buf()
 * - [in] ciphersuite
 * - [in] master
 * - [in] encrypt_then_mac
 * - [in] trunc_hmac
 * - [in] compression
 * - [in] tls_prf: pointer to PRF to use for key derivation
 * - [in] randbytes: buffer holding ServerHello.random + ClientHello.random
 * - [in] minor_ver: SSL/TLS minor version
 * - [in] endpoint: client or server
 * - [in] ssl: optionally used for:
 *        - MBEDTLS_SSL_HW_RECORD_ACCEL: whole context
 *        - MBEDTLS_SSL_EXPORT_KEYS: ssl->conf->{f,p}_export_keys
 *        - MBEDTLS_DEBUG_C: ssl->conf->{f,p}_dbg
 */
/* Force compilers to inline this function if it's used only
 * from one place, because at least ARMC5 doesn't do that
 * automatically. */
#if !defined(MBEDTLS_SSL_CONTEXT_SERIALIZATION)
MBEDTLS_ALWAYS_INLINE static inline
#else
static
#endif /* MBEDTLS_SSL_CONTEXT_SERIALIZATION */
int ssl_populate_transform( mbedtls_ssl_transform *transform,
                                   int ciphersuite,
                                   const unsigned char master[48],
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
                                   int encrypt_then_mac,
#endif
#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
                                   int trunc_hmac,
#endif
#endif /* MBEDTLS_SSL_SOME_MODES_USE_MAC */
#if defined(MBEDTLS_ZLIB_SUPPORT)
                                   int compression,
#endif
                                   const unsigned char randbytes[64],
                                   int minor_ver,
                                   unsigned endpoint,
#if !defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
                                   const
#endif
                                   mbedtls_ssl_context *ssl )
{
    int ret = 0;
    unsigned char keyblk[256];
    unsigned char *key1;
    unsigned char *key2;
    unsigned char *mac_enc;
    unsigned char *mac_dec;
    size_t mac_key_len;
    size_t iv_copy_len;
    unsigned keylen;
    mbedtls_ssl_ciphersuite_handle_t ciphersuite_info;
    const mbedtls_cipher_info_t *cipher_info;
    mbedtls_md_handle_t md_info;

#if !defined(MBEDTLS_SSL_HW_RECORD_ACCEL) && \
    !defined(MBEDTLS_SSL_EXPORT_KEYS) && \
    !defined(MBEDTLS_DEBUG_C) && \
    !defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    ssl = NULL; /* make sure we don't use it except for those cases */
    (void) ssl;
#endif

    /*
     * Some data just needs copying into the structure
     */
#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC) && \
    defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
    transform->encrypt_then_mac = encrypt_then_mac;
#endif

#if !defined(MBEDTLS_SSL_CONF_FIXED_MINOR_VER)
    transform->minor_ver = minor_ver;
#else
    ((void) minor_ver);
#endif /* !MBEDTLS_SSL_CONF_FIXED_MINOR_VER */

#if defined(MBEDTLS_SSL_CONTEXT_SERIALIZATION)
    mbedtls_platform_memcpy( transform->randbytes, randbytes, sizeof( transform->randbytes ) );
#endif

    /*
     * Get various info structures
     */
    ciphersuite_info = mbedtls_ssl_ciphersuite_from_id( ciphersuite );
    if( ciphersuite_info == MBEDTLS_SSL_CIPHERSUITE_INVALID_HANDLE )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "ciphersuite info for %d not found",
                                    ciphersuite ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    cipher_info = mbedtls_cipher_info_from_type(
        mbedtls_ssl_suite_get_cipher( ciphersuite_info ) );
    if( cipher_info == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "cipher info for %d not found",
                       mbedtls_ssl_suite_get_cipher( ciphersuite_info ) ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    md_info = mbedtls_md_info_from_type(
        mbedtls_ssl_suite_get_mac( ciphersuite_info ) );
    if( md_info == MBEDTLS_MD_INVALID_HANDLE )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "mbedtls_md info for %d not found",
                       mbedtls_ssl_suite_get_mac( ciphersuite_info ) ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    /* Copy own and peer's CID if the use of the CID
     * extension has been negotiated. */
    if( ssl->handshake->cid_in_use == MBEDTLS_SSL_CID_ENABLED )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Copy CIDs into SSL transform" ) );

        transform->in_cid_len = ssl->own_cid_len;
        /* Not using more secure mbedtls_platform_memcpy as cid is public */
        memcpy( transform->in_cid, ssl->own_cid, ssl->own_cid_len );
        MBEDTLS_SSL_DEBUG_BUF( 3, "Incoming CID", transform->in_cid,
                               transform->in_cid_len );

        transform->out_cid_len = ssl->handshake->peer_cid_len;
        /* Not using more secure mbedtls_platform_memcpy as cid is public */
        memcpy( transform->out_cid, ssl->handshake->peer_cid,
                ssl->handshake->peer_cid_len );
        MBEDTLS_SSL_DEBUG_BUF( 3, "Outgoing CID", transform->out_cid,
                               transform->out_cid_len );
    }
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

    /*
     * Compute key block using the PRF
     */
    ret = ssl_prf( minor_ver,
                   mbedtls_ssl_suite_get_mac( ciphersuite_info ),
                   master, 48, "key expansion", randbytes, 64,
                   keyblk, 256 );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "prf", ret );
        return( ret );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ciphersuite = %s",
                           mbedtls_ssl_get_ciphersuite_name( ciphersuite ) ) );
    MBEDTLS_SSL_DEBUG_BUF( 3, "master secret", master, 48 );
    MBEDTLS_SSL_DEBUG_BUF( 4, "random bytes", randbytes, 64 );
    MBEDTLS_SSL_DEBUG_BUF( 4, "key block", keyblk, 256 );

    /*
     * Determine the appropriate key, IV and MAC length.
     */

    keylen = cipher_info->key_bitlen / 8;

#if defined(MBEDTLS_GCM_C) ||                           \
    defined(MBEDTLS_CCM_C) ||                           \
    defined(MBEDTLS_CHACHAPOLY_C)
    if( cipher_info->mode == MBEDTLS_MODE_GCM ||
        cipher_info->mode == MBEDTLS_MODE_CCM ||
        cipher_info->mode == MBEDTLS_MODE_CHACHAPOLY )
    {
        transform->maclen = 0;
        mac_key_len = 0;
        transform->taglen = mbedtls_ssl_suite_get_flags( ciphersuite_info ) &
            MBEDTLS_CIPHERSUITE_SHORT_TAG ? 8 : 16;

        /* All modes haves 96-bit IVs;
         * GCM and CCM has 4 implicit and 8 explicit bytes
         * ChachaPoly has all 12 bytes implicit
         */
        transform->ivlen = 12;
        if( cipher_info->mode == MBEDTLS_MODE_CHACHAPOLY )
            transform->fixed_ivlen = 12;
        else
            transform->fixed_ivlen = 4;
    }
    else
#endif /* MBEDTLS_GCM_C || MBEDTLS_CCM_C || MBEDTLS_CHACHAPOLY_C */
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
    if( cipher_info->mode == MBEDTLS_MODE_STREAM ||
        cipher_info->mode == MBEDTLS_MODE_CBC )
    {
        /* Initialize HMAC contexts */
        if( ( ret = mbedtls_md_setup( &transform->md_ctx_enc, md_info, 1 ) ) != 0 ||
            ( ret = mbedtls_md_setup( &transform->md_ctx_dec, md_info, 1 ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md_setup", ret );
            return( ret );
        }

        /* Get MAC length */
        mac_key_len = mbedtls_md_get_size( md_info );
        transform->maclen = mac_key_len;

#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
        /*
         * If HMAC is to be truncated, we shall keep the leftmost bytes,
         * (rfc 6066 page 13 or rfc 2104 section 4),
         * so we only need to adjust the length here.
         */
        if( trunc_hmac == MBEDTLS_SSL_TRUNC_HMAC_ENABLED )
        {
            transform->maclen = MBEDTLS_SSL_TRUNCATED_HMAC_LEN;

#if defined(MBEDTLS_SSL_TRUNCATED_HMAC_COMPAT)
            /* Fall back to old, non-compliant version of the truncated
             * HMAC implementation which also truncates the key
             * (Mbed TLS versions from 1.3 to 2.6.0) */
            mac_key_len = transform->maclen;
#endif
        }
#endif /* MBEDTLS_SSL_TRUNCATED_HMAC */

        /* IV length */
        transform->ivlen = cipher_info->iv_size;
    }
    else
#endif /* MBEDTLS_SSL_SOME_MODES_USE_MAC */
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "keylen: %u, ivlen: %u, maclen: %u",
                                (unsigned) keylen,
                                (unsigned) transform->ivlen,
                                (unsigned) transform->maclen ) );

    /*
     * Finally setup the cipher contexts, IVs and MAC secrets.
     */
#if defined(MBEDTLS_SSL_CLI_C)
    if( endpoint == MBEDTLS_SSL_IS_CLIENT )
    {
        key1 = keyblk + mac_key_len * 2;
        key2 = keyblk + mac_key_len * 2 + keylen;

        mac_enc = keyblk;
        mac_dec = keyblk + mac_key_len;

        /*
         * This is not used in TLS v1.1.
         */
        iv_copy_len = ( transform->fixed_ivlen ) ?
                            transform->fixed_ivlen : transform->ivlen;
        mbedtls_platform_memcpy( transform->iv_enc, key2 + keylen,  iv_copy_len );
        mbedtls_platform_memcpy( transform->iv_dec, key2 + keylen + iv_copy_len,
                iv_copy_len );
    }
    else
#endif /* MBEDTLS_SSL_CLI_C */
#if defined(MBEDTLS_SSL_SRV_C)
    if( endpoint == MBEDTLS_SSL_IS_SERVER )
    {
        key1 = keyblk + mac_key_len * 2 + keylen;
        key2 = keyblk + mac_key_len * 2;

        mac_enc = keyblk + mac_key_len;
        mac_dec = keyblk;

        /*
         * This is not used in TLS v1.1.
         */
        iv_copy_len = ( transform->fixed_ivlen ) ?
                            transform->fixed_ivlen : transform->ivlen;
        mbedtls_platform_memcpy( transform->iv_dec, key1 + keylen,  iv_copy_len );
        mbedtls_platform_memcpy( transform->iv_enc, key1 + keylen + iv_copy_len,
                iv_copy_len );
    }
    else
#endif /* MBEDTLS_SSL_SRV_C */
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
#if defined(MBEDTLS_SSL_PROTO_SSL3)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_0 )
    {
        if( mac_key_len > sizeof( transform->mac_enc ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        mbedtls_platform_memcpy( transform->mac_enc, mac_enc, mac_key_len );
        mbedtls_platform_memcpy( transform->mac_dec, mac_dec, mac_key_len );
    }
    else
#endif /* MBEDTLS_SSL_PROTO_SSL3 */
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_2)
    if( mbedtls_ssl_ver_geq( minor_ver, MBEDTLS_SSL_MINOR_VERSION_1 ) )
    {
        /* For HMAC-based ciphersuites, initialize the HMAC transforms.
           For AEAD-based ciphersuites, there is nothing to do here. */
        if( mac_key_len != 0 )
        {
            mbedtls_md_hmac_starts( &transform->md_ctx_enc, mac_enc, mac_key_len );
            mbedtls_md_hmac_starts( &transform->md_ctx_dec, mac_dec, mac_key_len );
        }
    }
    else
#endif
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }
#endif /* MBEDTLS_SSL_SOME_MODES_USE_MAC */

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_init != NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "going for mbedtls_ssl_hw_record_init()" ) );

        if( ( ret = mbedtls_ssl_hw_record_init( ssl, key1, key2, keylen,
                                        transform->iv_enc, transform->iv_dec,
                                        iv_copy_len,
                                        mac_enc, mac_dec,
                                        mac_key_len ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_hw_record_init", ret );
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        }
    }
#else
    ((void) mac_dec);
    ((void) mac_enc);
#endif /* MBEDTLS_SSL_HW_RECORD_ACCEL */

#if defined(MBEDTLS_SSL_EXPORT_KEYS)
    if( ssl->conf->f_export_keys != NULL )
    {
        ssl->conf->f_export_keys( ssl->conf->p_export_keys,
                                  master, keyblk,
                                  mac_key_len, keylen,
                                  iv_copy_len );
    }
#endif
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
    if( ( ret = mbedtls_cipher_setup( &transform->cipher_ctx,
                                 cipher_info ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_setup", ret );
        return( ret );
    }
    transform->key_enc = mbedtls_calloc( 1, cipher_info->key_bitlen >> 3 );
    transform->key_dec = mbedtls_calloc( 1, cipher_info->key_bitlen >> 3 );

    if( transform->key_enc == NULL || transform->key_dec == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Failed to allocate cipher keys" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }
    memcpy( transform->key_enc, key1, cipher_info->key_bitlen >> 3 );
    memcpy( transform->key_dec, key2, cipher_info->key_bitlen >> 3 );

    transform->key_bitlen = cipher_info->key_bitlen;
#if defined(MBEDTLS_VALIDATE_SSL_KEYS_INTEGRITY)
    transform->key_enc_hash = mbedtls_hash( transform->key_enc, transform->key_bitlen >> 3 );
    transform->key_dec_hash = mbedtls_hash( transform->key_dec, transform->key_bitlen >> 3 );
#endif

#else
    if( ( ret = mbedtls_cipher_setup( &transform->cipher_ctx_enc,
                                 cipher_info ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_setup", ret );
        return( ret );
    }

    if( ( ret = mbedtls_cipher_setup( &transform->cipher_ctx_dec,
                                 cipher_info ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_setup", ret );
        return( ret );
    }

    if( ( ret = mbedtls_cipher_setkey( &transform->cipher_ctx_enc, key1,
                               cipher_info->key_bitlen,
                               MBEDTLS_ENCRYPT ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_setkey", ret );
        return( ret );
    }

    if( ( ret = mbedtls_cipher_setkey( &transform->cipher_ctx_dec, key2,
                               cipher_info->key_bitlen,
                               MBEDTLS_DECRYPT ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_setkey", ret );
        return( ret );
    }
#endif
#if defined(MBEDTLS_CIPHER_MODE_CBC)
    if( cipher_info->mode == MBEDTLS_MODE_CBC )
    {
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        if( ( ret = mbedtls_cipher_set_padding_mode( &transform->cipher_ctx,
                                             MBEDTLS_PADDING_NONE ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_set_padding_mode", ret );
            return( ret );
        }
#else
        if( ( ret = mbedtls_cipher_set_padding_mode( &transform->cipher_ctx_enc,
                                             MBEDTLS_PADDING_NONE ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_set_padding_mode", ret );
            return( ret );
        }

        if( ( ret = mbedtls_cipher_set_padding_mode( &transform->cipher_ctx_dec,
                                             MBEDTLS_PADDING_NONE ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_set_padding_mode", ret );
            return( ret );
        }
#endif
    }
#endif /* MBEDTLS_CIPHER_MODE_CBC */

    mbedtls_platform_zeroize( keyblk, sizeof( keyblk ) );

    /* Initialize Zlib contexts */
#if defined(MBEDTLS_ZLIB_SUPPORT)
    if( compression == MBEDTLS_SSL_COMPRESS_DEFLATE )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Initializing zlib states" ) );

        mbedtls_platform_memset( &transform->ctx_deflate, 0, sizeof( transform->ctx_deflate ) );
        mbedtls_platform_memset( &transform->ctx_inflate, 0, sizeof( transform->ctx_inflate ) );

        if( deflateInit( &transform->ctx_deflate,
                         Z_DEFAULT_COMPRESSION )   != Z_OK ||
            inflateInit( &transform->ctx_inflate ) != Z_OK )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Failed to initialize compression" ) );
            return( MBEDTLS_ERR_SSL_COMPRESSION_FAILED );
        }
    }
#endif /* MBEDTLS_ZLIB_SUPPORT */

    return( 0 );
}

#if defined(MBEDTLS_SSL_PROTO_SSL3)
static inline void ssl_calc_verify_ssl( const mbedtls_ssl_context *ssl,
                                        unsigned char hash[36],
                                        size_t *hlen )
{
    mbedtls_md5_context md5;
    mbedtls_sha1_context sha1;
    unsigned char pad_1[48];
    unsigned char pad_2[48];

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc verify ssl" ) );

    mbedtls_md5_init( &md5 );
    mbedtls_sha1_init( &sha1 );

    mbedtls_md5_clone( &md5, &ssl->handshake->fin_md5 );
    mbedtls_sha1_clone( &sha1, &ssl->handshake->fin_sha1 );

    mbedtls_platform_memset( pad_1, 0x36, 48 );
    mbedtls_platform_memset( pad_2, 0x5C, 48 );

    mbedtls_md5_update_ret( &md5, ssl->session_negotiate->master, 48 );
    mbedtls_md5_update_ret( &md5, pad_1, 48 );
    mbedtls_md5_finish_ret( &md5, hash );

    mbedtls_md5_starts_ret( &md5 );
    mbedtls_md5_update_ret( &md5, ssl->session_negotiate->master, 48 );
    mbedtls_md5_update_ret( &md5, pad_2, 48 );
    mbedtls_md5_update_ret( &md5, hash,  16 );
    mbedtls_md5_finish_ret( &md5, hash );

    mbedtls_sha1_update_ret( &sha1, ssl->session_negotiate->master, 48 );
    mbedtls_sha1_update_ret( &sha1, pad_1, 40 );
    mbedtls_sha1_finish_ret( &sha1, hash + 16 );

    mbedtls_sha1_starts_ret( &sha1 );
    mbedtls_sha1_update_ret( &sha1, ssl->session_negotiate->master, 48 );
    mbedtls_sha1_update_ret( &sha1, pad_2, 40 );
    mbedtls_sha1_update_ret( &sha1, hash + 16, 20 );
    mbedtls_sha1_finish_ret( &sha1, hash + 16 );

    *hlen = 36;

    MBEDTLS_SSL_DEBUG_BUF( 3, "calculated verify result", hash, *hlen );
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc verify" ) );

    mbedtls_md5_free(  &md5  );
    mbedtls_sha1_free( &sha1 );

    return;
}
#endif /* MBEDTLS_SSL_PROTO_SSL3 */

#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
static inline void ssl_calc_verify_tls( const mbedtls_ssl_context *ssl,
                                        unsigned char hash[36],
                                        size_t *hlen )
{
    mbedtls_md5_context md5;
    mbedtls_sha1_context sha1;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc verify tls" ) );

    mbedtls_md5_init( &md5 );
    mbedtls_sha1_init( &sha1 );

    mbedtls_md5_clone( &md5, &ssl->handshake->fin_md5 );
    mbedtls_sha1_clone( &sha1, &ssl->handshake->fin_sha1 );

     mbedtls_md5_finish_ret( &md5,  hash );
    mbedtls_sha1_finish_ret( &sha1, hash + 16 );

    *hlen = 36;

    MBEDTLS_SSL_DEBUG_BUF( 3, "calculated verify result", hash, *hlen );
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc verify" ) );

    mbedtls_md5_free(  &md5  );
    mbedtls_sha1_free( &sha1 );

    return;
}
#endif /* MBEDTLS_SSL_PROTO_TLS1 || MBEDTLS_SSL_PROTO_TLS1_1 */

#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA256_C)
static inline void ssl_calc_verify_tls_sha256( const mbedtls_ssl_context *ssl,
                                               unsigned char hash[32],
                                               size_t *hlen )
{
    mbedtls_sha256_context sha256;

    mbedtls_sha256_init( &sha256 );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc verify sha256" ) );

    mbedtls_sha256_clone( &sha256, &ssl->handshake->fin_sha256 );
    mbedtls_sha256_finish_ret( &sha256, hash );

    *hlen = 32;

    MBEDTLS_SSL_DEBUG_BUF( 3, "calculated verify result", hash, *hlen );
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc verify" ) );

    mbedtls_sha256_free( &sha256 );

    return;
}
#endif /* MBEDTLS_SHA256_C */

#if defined(MBEDTLS_SHA512_C)
static inline void ssl_calc_verify_tls_sha384( const mbedtls_ssl_context *ssl,
                                               unsigned char hash[48],
                                               size_t *hlen )
{
    mbedtls_sha512_context sha512;

    mbedtls_sha512_init( &sha512 );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> calc verify sha384" ) );

    mbedtls_sha512_clone( &sha512, &ssl->handshake->fin_sha512 );
    mbedtls_sha512_finish_ret( &sha512, hash );

    *hlen = 48;

    MBEDTLS_SSL_DEBUG_BUF( 3, "calculated verify result", hash, *hlen );
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= calc verify" ) );

    mbedtls_sha512_free( &sha512 );

    return;
}
#endif /* MBEDTLS_SHA512_C */
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */

int mbedtls_ssl_calc_verify( int minor_ver,
                             mbedtls_md_type_t hash,
                             mbedtls_ssl_context const *ssl,
                             unsigned char *dst,
                             size_t *hlen )
{
#if !defined(MBEDTLS_SSL_PROTO_TLS1_2) || !defined(MBEDTLS_SHA512_C)
    (void) hash;
#endif

#if defined(MBEDTLS_SSL_PROTO_SSL3)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_0 )
        ssl_calc_verify_ssl( ssl, dst, hlen );
    else
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
    if( mbedtls_ssl_ver_lt( minor_ver, MBEDTLS_SSL_MINOR_VERSION_3 ) )
        ssl_calc_verify_tls( ssl, dst, hlen );
    else
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA512_C)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_3 &&
        hash == MBEDTLS_MD_SHA384 )
    {
        ssl_calc_verify_tls_sha384( ssl, dst, hlen );
    }
    else
#endif
#if defined(MBEDTLS_SHA256_C)
    if( minor_ver == MBEDTLS_SSL_MINOR_VERSION_3 )
    {
        ssl_calc_verify_tls_sha256( ssl, dst, hlen );
    }
    else
#endif
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */
    {
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    return( 0 );
}

/*
 * Compute master secret if needed
 *
 * Parameters:
 * [in/out] handshake
 *          [in] resume, premaster, extended_ms, calc_verify, tls_prf
 *          [out] premaster (cleared)
 * [out] master
 * [in] ssl: optionally used for debugging and calc_verify
 */
static int ssl_compute_master( mbedtls_ssl_handshake_params *handshake,
                               unsigned char *master,
                               const mbedtls_ssl_context *ssl )
{
    int ret;

/* #if !defined(MBEDTLS_DEBUG_C) && !defined(MBEDTLS_SSL_EXTENDED_MASTER_SECRET) */
/*     ssl = NULL; /\* make sure we don't use it except for debug and EMS *\/ */
/*     (void) ssl; */
/* #endif */

    mbedtls_ssl_ciphersuite_handle_t const ciphersuite =
        mbedtls_ssl_handshake_get_ciphersuite( handshake );

#if !defined(MBEDTLS_SSL_NO_SESSION_RESUMPTION)
    if( handshake->resume == MBEDTLS_SSL_FI_FLAG_SET )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "no premaster (session resumed)" ) );
        return( 0 );
    }
#endif /* !MBEDTLS_SSL_NO_SESSION_RESUMPTION */

    MBEDTLS_SSL_DEBUG_BUF( 3, "premaster secret", handshake->premaster,
                                                  handshake->pmslen );

#if defined(MBEDTLS_SSL_EXTENDED_MASTER_SECRET)
    if( mbedtls_ssl_hs_get_extended_ms( handshake )
          == MBEDTLS_SSL_EXTENDED_MS_ENABLED )
    {
        unsigned char session_hash[48];
        size_t hash_len;

        mbedtls_ssl_calc_verify(
            mbedtls_ssl_get_minor_ver( ssl ),
            mbedtls_ssl_suite_get_mac( ciphersuite ),
            ssl, session_hash, &hash_len );

        MBEDTLS_SSL_DEBUG_BUF( 3, "session hash for extended master secret",
                                  session_hash, hash_len );

        ret = ssl_prf( mbedtls_ssl_get_minor_ver( ssl ),
                       mbedtls_ssl_suite_get_mac( ciphersuite ),
                       handshake->premaster, handshake->pmslen,
                       "extended master secret",
                       session_hash, hash_len,
                       master, 48 );
    }
    else
#endif
    {
        ret = ssl_prf( mbedtls_ssl_get_minor_ver( ssl ),
                       mbedtls_ssl_suite_get_mac( ciphersuite ),
                       handshake->premaster, handshake->pmslen,
                       "master secret",
                       handshake->randbytes, 64,
                       master, 48 );
    }
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "prf", ret );
        return( ret );
    }

    if( handshake->premaster == mbedtls_platform_zeroize(
                    handshake->premaster, sizeof(handshake->premaster) ) )
    {
        return( 0 );
    }

    return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
}

int mbedtls_ssl_derive_keys( mbedtls_ssl_context *ssl )
{
    volatile int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> derive keys" ) );
    ssl->handshake->key_derivation_done = MBEDTLS_SSL_FI_FLAG_UNSET;
    /* Compute master secret if needed */
    ret = ssl_compute_master( ssl->handshake,
                              ssl->session_negotiate->master,
                              ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_compute_master", ret );
        return( ret );
    }

    /* Swap the client and server random values:
     * - MS derivation wanted client+server (RFC 5246 8.1)
     * - key derivation wants server+client (RFC 5246 6.3) */
    {
        unsigned char tmp[64];
        mbedtls_platform_memcpy( tmp, ssl->handshake->randbytes, 64 );
        mbedtls_platform_memcpy( ssl->handshake->randbytes, tmp + 32, 32 );
        mbedtls_platform_memcpy( ssl->handshake->randbytes + 32, tmp, 32 );
        mbedtls_platform_zeroize( tmp, sizeof( tmp ) );
    }

    /* Populate transform structure */
    ret = ssl_populate_transform( ssl->transform_negotiate,
                  mbedtls_ssl_session_get_ciphersuite( ssl->session_negotiate ),
                  ssl->session_negotiate->master,
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
                  ssl->session_negotiate->encrypt_then_mac,
#endif
#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
                  ssl->session_negotiate->trunc_hmac,
#endif
#endif /* MBEDTLS_SSL_SOME_MODES_USE_MAC */
#if defined(MBEDTLS_ZLIB_SUPPORT)
                  ssl->session_negotiate->compression,
#endif
                  ssl->handshake->randbytes,
                  mbedtls_ssl_get_minor_ver( ssl ),
                  mbedtls_ssl_conf_get_endpoint( ssl->conf ),
                  ssl );
    if( ret == 0 )
    {
        mbedtls_platform_random_delay();
        if( ret == 0 )
        {
            ssl->handshake->key_derivation_done = MBEDTLS_SSL_FI_FLAG_SET;
        }
        else
        {
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        }
    }
    else
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_populate_transform", ret );
        return( ret );
    }

    /* We no longer need Server/ClientHello.random values */
    mbedtls_platform_zeroize( ssl->handshake->randbytes,
                      sizeof( ssl->handshake->randbytes ) );

    /* Allocate compression buffer */
#if defined(MBEDTLS_ZLIB_SUPPORT)
    if( ssl->session_negotiate->compression == MBEDTLS_SSL_COMPRESS_DEFLATE &&
        ssl->compress_buf == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Allocating compression buffer" ) );
        ssl->compress_buf = mbedtls_calloc( 1, MBEDTLS_SSL_COMPRESS_BUFFER_LEN );
        if( ssl->compress_buf == NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc(%d bytes) failed",
                                        MBEDTLS_SSL_COMPRESS_BUFFER_LEN ) );
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
        }
    }
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= derive keys" ) );

    return( 0 );
}

int mbedtls_ssl_build_pms( mbedtls_ssl_context *ssl )
{
    volatile int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;

    mbedtls_ssl_ciphersuite_handle_t ciphersuite_info =
        mbedtls_ssl_handshake_get_ciphersuite( ssl->handshake );
#if defined(MBEDTLS_USE_TINYCRYPT)
    if( mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDHE_RSA                       ||
        mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA                     ||
        mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDH_RSA                        ||
        mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA )
    {
        ret = uECC_shared_secret( ssl->handshake->ecdh_peerkey,
                                  ssl->handshake->ecdh_privkey,
                                  ssl->handshake->premaster );
        if( ret == UECC_FAULT_DETECTED )
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        if( ret != UECC_SUCCESS )
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        ssl->handshake->premaster_generated = MBEDTLS_SSL_FI_FLAG_SET;

        ssl->handshake->pmslen = NUM_ECC_BYTES;
    }
    else
#endif
#if defined(MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED)
    if( mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_DHE_RSA )
    {
        ret = mbedtls_dhm_calc_secret( &ssl->handshake->dhm_ctx,
                                      ssl->handshake->premaster,
                                      MBEDTLS_PREMASTER_SIZE,
                                     &ssl->handshake->pmslen,
                                      mbedtls_ssl_conf_get_frng( ssl->conf ),
                                      mbedtls_ssl_conf_get_prng( ssl->conf ) );
        if( ret == 0 )
        {
            mbedtls_platform_random_delay();
            if( ret == 0 )
            {
                ssl->handshake->premaster_generated = MBEDTLS_SSL_FI_FLAG_SET;
            }
            else
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_dhm_calc_secret",
                                       MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
                return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
            }
        }
        else
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_dhm_calc_secret", ret );
            return( ret );
        }

        MBEDTLS_SSL_DEBUG_MPI( 3, "DHM: K ", &ssl->handshake->dhm_ctx.K  );
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED */
#if defined(MBEDTLS_ECDH_C) &&                                          \
    ( defined(MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED)   ||              \
      defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED) ||              \
      defined(MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED)    ||              \
      defined(MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED) )
    if( mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDHE_RSA                       ||
        mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA                     ||
        mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDH_RSA                        ||
        mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA )
    {
        ret = mbedtls_ecdh_calc_secret( &ssl->handshake->ecdh_ctx,
                                      &ssl->handshake->pmslen,
                                       ssl->handshake->premaster,
                                       MBEDTLS_MPI_MAX_SIZE,
                                       mbedtls_ssl_conf_get_frng( ssl->conf ),
                                       mbedtls_ssl_conf_get_prng( ssl->conf ) );
        if( ret == 0 )
        {
            mbedtls_platform_random_delay();
            if( ret == 0 )
            {
                ssl->handshake->premaster_generated = MBEDTLS_SSL_FI_FLAG_SET;
            }
            else
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ecdh_calc_secret", ret );
                return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
            }
        }
        else
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ecdh_calc_secret", ret );
            return( ret );
        }

        MBEDTLS_SSL_DEBUG_MPI( 3, "ECDH: z", &ssl->handshake->ecdh_ctx.z );
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED ||
          MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED ||
          MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED ||
          MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED */
#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
    if( mbedtls_ssl_ciphersuite_uses_psk( ciphersuite_info ) )
    {
        ret = mbedtls_ssl_psk_derive_premaster( ssl,
            mbedtls_ssl_suite_get_key_exchange( ciphersuite_info ) );
        if( ret == 0 )
        {
            mbedtls_platform_random_delay();
            if( ret == 0 )
            {
                ssl->handshake->premaster_generated = MBEDTLS_SSL_FI_FLAG_SET;
            }
            else
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_psk_derive_premaster", ret );
                return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
            }
        }
        else
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_psk_derive_premaster", ret );
            return( ret );
        }
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED */
#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
    if( mbedtls_ssl_suite_get_key_exchange( ciphersuite_info ) ==
        MBEDTLS_KEY_EXCHANGE_ECJPAKE )
    {
        ret = mbedtls_ecjpake_derive_secret( &ssl->handshake->ecjpake_ctx,
                ssl->handshake->premaster, 32, &ssl->handshake->pmslen,
                mbedtls_ssl_conf_get_frng( ssl->conf ),
                mbedtls_ssl_conf_get_prng( ssl->conf ) );
        if( ret == 0 )
        {
            mbedtls_platform_random_delay();
            if( ret == 0 )
            {
                ssl->handshake->premaster_generated = MBEDTLS_SSL_FI_FLAG_SET;
            }
            else
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ecjpake_derive_secret", ret );
                return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
            }
        }
        else
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ecjpake_derive_secret", ret );
            return( ret );
        }
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED */
#if defined(MBEDTLS_KEY_EXCHANGE_RSA_ENABLED)
    if( mbedtls_ssl_suite_get_key_exchange( ciphersuite_info )
        == MBEDTLS_KEY_EXCHANGE_RSA )
    {
        ((void) ret);
        /* The premaster secret has already been set by
         * ssl_rsa_generate_partial_pms(). Only the
         * PMS length needs to be set. */
        ssl->handshake->pmslen = 48;
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED */
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    return( 0 );
}

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
int mbedtls_ssl_psk_derive_premaster( mbedtls_ssl_context *ssl, mbedtls_key_exchange_type_t key_ex )
{
    unsigned char *p = ssl->handshake->premaster;
    unsigned char *end = p + sizeof( ssl->handshake->premaster );
    const unsigned char *psk = ssl->conf->psk;
    size_t psk_len = ssl->conf->psk_len;

    /* If the psk callback was called, use its result */
    if( ssl->handshake->psk != NULL )
    {
        psk = ssl->handshake->psk;
        psk_len = ssl->handshake->psk_len;
    }

    /*
     * PMS = struct {
     *     opaque other_secret<0..2^16-1>;
     *     opaque psk<0..2^16-1>;
     * };
     * with "other_secret" depending on the particular key exchange
     */
#if defined(MBEDTLS_KEY_EXCHANGE_PSK_ENABLED)
    if( key_ex == MBEDTLS_KEY_EXCHANGE_PSK )
    {
        if( end - p < 2 )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        p = mbedtls_platform_put_uint16_be( p, psk_len );

        if( end < p || (size_t)( end - p ) < psk_len )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        mbedtls_platform_memset( p, 0, psk_len );
        p += psk_len;
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_PSK_ENABLED */
#if defined(MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED)
    if( key_ex == MBEDTLS_KEY_EXCHANGE_RSA_PSK )
    {
        /*
         * other_secret already set by the ClientKeyExchange message,
         * and is 48 bytes long
         */
        if( end - p < 2 )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        *p++ = 0;
        *p++ = 48;
        p += 48;
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED */
#if defined(MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED)
    if( key_ex == MBEDTLS_KEY_EXCHANGE_DHE_PSK )
    {
        int ret;
        size_t len;

        /* Write length only when we know the actual value */
        if( ( ret = mbedtls_dhm_calc_secret( &ssl->handshake->dhm_ctx,
                                      p + 2, end - ( p + 2 ), &len,
                                      mbedtls_ssl_conf_get_frng( ssl->conf ),
                                      mbedtls_ssl_conf_get_prng( ssl->conf ) ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_dhm_calc_secret", ret );
            return( ret );
        }
        p = mbedtls_platform_put_uint16_be( p, len );
        p += len;

        MBEDTLS_SSL_DEBUG_MPI( 3, "DHM: K ", &ssl->handshake->dhm_ctx.K  );
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED */
#if defined(MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED)
    if( key_ex == MBEDTLS_KEY_EXCHANGE_ECDHE_PSK )
    {
        int ret;
        size_t zlen;

#if defined(MBEDTLS_USE_TINYCRYPT)
        ret = uECC_shared_secret( ssl->handshake->ecdh_peerkey,
                                  ssl->handshake->ecdh_privkey,
                                  p + 2 );
        if( ret == UECC_FAULT_DETECTED )
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        if( ret != UECC_SUCCESS )
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );

        zlen = NUM_ECC_BYTES;
#else /* MBEDTLS_USE_TINYCRYPT */
        if( ( ret = mbedtls_ecdh_calc_secret( &ssl->handshake->ecdh_ctx, &zlen,
                                       p + 2, end - ( p + 2 ),
                                       mbedtls_ssl_conf_get_frng( ssl->conf ),
                                       mbedtls_ssl_conf_get_prng( ssl->conf ) ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ecdh_calc_secret", ret );
            return( ret );
        }

        MBEDTLS_SSL_DEBUG_ECDH( 3, &ssl->handshake->ecdh_ctx,
                                MBEDTLS_DEBUG_ECDH_Z );
#endif /* MBEDTLS_USE_TINYCRYPT */

        p = mbedtls_platform_put_uint16_be( p, zlen );
        p += zlen;
    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED */
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    /* opaque psk<0..2^16-1>; */
    if( end - p < 2 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    p = mbedtls_platform_put_uint16_be( p, psk_len );

    if( end < p || (size_t)( end - p ) < psk_len )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    if( mbedtls_platform_memcpy( p, psk, psk_len ) != p )
    {
        return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
    }
    p += psk_len;

    ssl->handshake->pmslen = p - ssl->handshake->premaster;

    ssl->handshake->premaster_generated = MBEDTLS_SSL_FI_FLAG_SET;

    return( 0 );
}
#endif /* MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED */

#if defined(MBEDTLS_SSL_PROTO_SSL3)
/*
 * SSLv3.0 MAC functions
 */
#define SSL_MAC_MAX_BYTES   20  /* MD-5 or SHA-1 */
static void ssl_mac( mbedtls_md_context_t *md_ctx,
                     const unsigned char *secret,
                     const unsigned char *buf, size_t len,
                     const unsigned char *ctr, int type,
                     unsigned char out[SSL_MAC_MAX_BYTES] )
{
    unsigned char header[11];
    unsigned char padding[48];
    int padlen;
    int md_size = mbedtls_md_get_size( md_ctx->md_info );
    int md_type = mbedtls_md_get_type( md_ctx->md_info );

    /* Only MD5 and SHA-1 supported */
    if( md_type == MBEDTLS_MD_MD5 )
        padlen = 48;
    else
        padlen = 40;

    mbedtls_platform_memcpy( header, ctr, 8 );
    header[8] = (unsigned char) type;
    (void)mbedtls_platform_put_uint16_be( &header[9], len );

    mbedtls_platform_memset( padding, 0x36, padlen );
    mbedtls_md_starts( md_ctx );
    mbedtls_md_update( md_ctx, secret,  md_size );
    mbedtls_md_update( md_ctx, padding, padlen  );
    mbedtls_md_update( md_ctx, header,  11      );
    mbedtls_md_update( md_ctx, buf,     len     );
    mbedtls_md_finish( md_ctx, out              );

    mbedtls_platform_memset( padding, 0x5C, padlen );
    mbedtls_md_starts( md_ctx );
    mbedtls_md_update( md_ctx, secret,    md_size );
    mbedtls_md_update( md_ctx, padding,   padlen  );
    mbedtls_md_update( md_ctx, out,       md_size );
    mbedtls_md_finish( md_ctx, out                );
}
#endif /* MBEDTLS_SSL_PROTO_SSL3 */

/* The function below is only used in the Lucky 13 counter-measure in
 * mbedtls_ssl_decrypt_buf(). These are the defines that guard the call site. */
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC) && \
    ( defined(MBEDTLS_SSL_PROTO_TLS1) || \
      defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
      defined(MBEDTLS_SSL_PROTO_TLS1_2) )
/* This function makes sure every byte in the memory region is accessed
 * (in ascending addresses order) */
static void ssl_read_memory( unsigned char *p, size_t len )
{
    unsigned char acc = 0;
    volatile unsigned char force;

    for( ; len != 0; p++, len-- )
        acc ^= *p;

    force = acc;
    (void) force;
}
#endif /* SSL_SOME_MODES_USE_MAC && ( TLS1 || TLS1_1 || TLS1_2 ) */

/*
 * Encryption/decryption functions
 */

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
/* This functions transforms a DTLS plaintext fragment and a record content
 * type into an instance of the DTLSInnerPlaintext structure:
 *
 *        struct {
 *            opaque content[DTLSPlaintext.length];
 *            ContentType real_type;
 *            uint8 zeros[length_of_padding];
 *        } DTLSInnerPlaintext;
 *
 *  Input:
 *  - `content`: The beginning of the buffer holding the
 *               plaintext to be wrapped.
 *  - `*content_size`: The length of the plaintext in Bytes.
 *  - `max_len`: The number of Bytes available starting from
 *               `content`. This must be `>= *content_size`.
 *  - `rec_type`: The desired record content type.
 *
 *  Output:
 *  - `content`: The beginning of the resulting DTLSInnerPlaintext structure.
 *  - `*content_size`: The length of the resulting DTLSInnerPlaintext structure.
 *
 *  Returns:
 *  - `0` on success.
 *  - A negative error code if `max_len` didn't offer enough space
 *    for the expansion.
 */
static int ssl_cid_build_inner_plaintext( unsigned char *content,
                                          size_t *content_size,
                                          size_t remaining,
                                          uint8_t rec_type )
{
    size_t len = *content_size;
    size_t pad = ( MBEDTLS_SSL_CID_PADDING_GRANULARITY -
                   ( len + 1 ) % MBEDTLS_SSL_CID_PADDING_GRANULARITY ) %
        MBEDTLS_SSL_CID_PADDING_GRANULARITY;

    /* Write real content type */
    if( remaining == 0 )
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    content[ len ] = rec_type;
    len++;
    remaining--;

    if( remaining < pad )
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    mbedtls_platform_memset( content + len, 0, pad );
    len += pad;
    remaining -= pad;

    *content_size = len;
    return( 0 );
}

/* This function parses a DTLSInnerPlaintext structure.
 * See ssl_cid_build_inner_plaintext() for details. */
static int ssl_cid_parse_inner_plaintext( unsigned char const *content,
                                          size_t *content_size,
                                          uint8_t *rec_type )
{
    size_t remaining = *content_size;

    /* Determine length of padding by skipping zeroes from the back. */
    do
    {
        if( remaining == 0 )
            return( -1 );
        remaining--;
    } while( content[ remaining ] == 0 );

    *content_size = remaining;
    *rec_type = content[ remaining ];

    return( 0 );
}
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

/* `add_data` must have size 13 Bytes if the CID extension is disabled,
 * and 13 + 1 + CID-length Bytes if the CID extension is enabled. */
static void ssl_extract_add_data_from_record( unsigned char* add_data,
                                              size_t *add_data_len,
                                              mbedtls_record *rec )
{
    /* Quoting RFC 5246 (TLS 1.2):
     *
     *    additional_data = seq_num + TLSCompressed.type +
     *                      TLSCompressed.version + TLSCompressed.length;
     *
     * For the CID extension, this is extended as follows
     * (quoting draft-ietf-tls-dtls-connection-id-05,
     *  https://tools.ietf.org/html/draft-ietf-tls-dtls-connection-id-05):
     *
     *       additional_data = seq_num + DTLSPlaintext.type +
     *                         DTLSPlaintext.version +
     *                         cid +
     *                         cid_length +
     *                         length_of_DTLSInnerPlaintext;
     */

    mbedtls_platform_memcpy( add_data, rec->ctr, sizeof( rec->ctr ) );
    add_data[8] = rec->type;
    mbedtls_platform_memcpy( add_data + 9, rec->ver, sizeof( rec->ver ) );

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    if( rec->cid_len != 0 )
    {
        mbedtls_platform_memcpy( add_data + 11, rec->cid, rec->cid_len );
        add_data[11 + rec->cid_len + 0] = rec->cid_len;
        (void)mbedtls_platform_put_uint16_be( &add_data[11 + rec->cid_len + 1],
                                              rec->data_len );
        *add_data_len = 13 + 1 + rec->cid_len;
    }
    else
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
    {
        (void)mbedtls_platform_put_uint16_be( &add_data[11], rec->data_len );
        *add_data_len = 13;
    }
}

int mbedtls_ssl_encrypt_buf( mbedtls_ssl_context *ssl,
                             mbedtls_ssl_transform *transform,
                             mbedtls_record *rec,
                             int (*f_rng)(void *, unsigned char *, size_t),
                             void *p_rng )
{
    mbedtls_cipher_mode_t mode;
    int auth_done = 0;
    unsigned char * data;
    unsigned char add_data[13 + 1 + MBEDTLS_SSL_CID_OUT_LEN_MAX ];
    size_t add_data_len;
    size_t post_avail;
    int encryption_status = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;

    /* The SSL context is only used for debugging purposes! */
#if !defined(MBEDTLS_DEBUG_C)
    ssl = NULL; /* make sure we don't use it except for debug */
    ((void) ssl);
#endif

    /* The PRNG is used for dynamic IV generation that's used
     * for CBC transformations in TLS 1.1 and TLS 1.2. */
#if !( defined(MBEDTLS_CIPHER_MODE_CBC) &&                              \
       ( defined(MBEDTLS_AES_C)  ||                                     \
         defined(MBEDTLS_ARIA_C) ||                                     \
         defined(MBEDTLS_CAMELLIA_C) ) &&                               \
       ( defined(MBEDTLS_SSL_PROTO_TLS1_1) || defined(MBEDTLS_SSL_PROTO_TLS1_2) ) )
    ((void) f_rng);
    ((void) p_rng);
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> encrypt buf" ) );

    if( transform == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "no transform provided to encrypt_buf" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }
    if( rec == NULL
        || rec->buf == NULL
        || rec->buf_len < rec->data_offset
        || rec->buf_len - rec->data_offset < rec->data_len
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
        || rec->cid_len != 0
#endif
        )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad record structure provided to encrypt_buf" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    data = rec->buf + rec->data_offset;
    post_avail = rec->buf_len - ( rec->data_len + rec->data_offset );
    MBEDTLS_SSL_DEBUG_BUF( 4, "before encrypt: output payload",
                           data, rec->data_len );
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
    mode = mbedtls_cipher_get_cipher_mode( &transform->cipher_ctx );
#else
    mode = mbedtls_cipher_get_cipher_mode( &transform->cipher_ctx_enc );
#endif
    if( rec->data_len > MBEDTLS_SSL_OUT_CONTENT_LEN )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Record content %u too large, maximum %d",
                                    (unsigned) rec->data_len,
                                    MBEDTLS_SSL_OUT_CONTENT_LEN ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    /*
     * Add CID information
     */
    rec->cid_len = transform->out_cid_len;
    /* Not using more secure mbedtls_platform_memcpy as cid is public */
    memcpy( rec->cid, transform->out_cid, transform->out_cid_len );
    MBEDTLS_SSL_DEBUG_BUF( 3, "CID", rec->cid, rec->cid_len );
    if( rec->cid_len != 0 )
    {
        int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
        /*
         * Wrap plaintext into DTLSInnerPlaintext structure.
         * See ssl_cid_build_inner_plaintext() for more information.
         *
         * Note that this changes `rec->data_len`, and hence
         * `post_avail` needs to be recalculated afterwards.
         */
        if( ( ret = ssl_cid_build_inner_plaintext( data,
                                                   &rec->data_len,
                                                   post_avail,
                                                   rec->type ) ) != 0 )
        {
            return( ret );
        }

        rec->type = MBEDTLS_SSL_MSG_CID;
    }
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

    post_avail = rec->buf_len - ( rec->data_len + rec->data_offset );

    /*
     * Add MAC before if needed
     */
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
    if( mode == MBEDTLS_MODE_STREAM ||
        ( mode == MBEDTLS_MODE_CBC
#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
          && transform->encrypt_then_mac == MBEDTLS_SSL_ETM_DISABLED
#endif
        ) )
    {
        if( post_avail < transform->maclen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Buffer provided for encrypted record not large enough" ) );
            return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
        }

#if defined(MBEDTLS_SSL_PROTO_SSL3)
        if( mbedtls_ssl_transform_get_minor_ver( transform ) ==
            MBEDTLS_SSL_MINOR_VERSION_0 )
        {
            unsigned char mac[SSL_MAC_MAX_BYTES];
            ssl_mac( &transform->md_ctx_enc, transform->mac_enc,
                     data, rec->data_len, rec->ctr, rec->type, mac );
            mbedtls_platform_memcpy( data + rec->data_len, mac, transform->maclen );
        }
        else
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
        defined(MBEDTLS_SSL_PROTO_TLS1_2)
        if( mbedtls_ssl_ver_geq(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_1 ) )
        {
            unsigned char mac[MBEDTLS_SSL_MAC_ADD];

            ssl_extract_add_data_from_record( add_data, &add_data_len, rec );

            mbedtls_md_hmac_update( &transform->md_ctx_enc, add_data,
                                    add_data_len );
            mbedtls_md_hmac_update( &transform->md_ctx_enc,
                                    data, rec->data_len );
            mbedtls_md_hmac_finish( &transform->md_ctx_enc, mac );
            mbedtls_md_hmac_reset( &transform->md_ctx_enc );

            mbedtls_platform_memcpy( data + rec->data_len, mac, transform->maclen );
        }
        else
#endif
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        MBEDTLS_SSL_DEBUG_BUF( 4, "computed mac", data + rec->data_len,
                               transform->maclen );

        rec->data_len += transform->maclen;
        post_avail -= transform->maclen;
        auth_done++;
    }
#endif /* MBEDTLS_SSL_SOME_MODES_USE_MAC */

    /*
     * Encrypt
     */
#if defined(MBEDTLS_ARC4_C) || defined(MBEDTLS_CIPHER_NULL_CIPHER)
    if( mode == MBEDTLS_MODE_STREAM )
    {
        int ret;
        size_t olen;
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "before encrypt: msglen = %d, "
                                    "including %d bytes of padding",
                                    rec->data_len, 0 ) );
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        if( ( ret = mbedtls_ssl_switch_key( transform, MBEDTLS_ENCRYPT ) )
                != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_switch_key", ret );
            return( ret );
        }

        if( ( ret = encryption_status = mbedtls_cipher_crypt( &transform->cipher_ctx,
                                   transform->iv_enc, transform->ivlen,
                                   data, rec->data_len,
                                   data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#else
        if( ( ret = encryption_status = mbedtls_cipher_crypt( &transform->cipher_ctx_enc,
                                   transform->iv_enc, transform->ivlen,
                                   data, rec->data_len,
                                   data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#endif
        if( rec->data_len != olen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }
    }
    else
#endif /* MBEDTLS_ARC4_C || MBEDTLS_CIPHER_NULL_CIPHER */

#if defined(MBEDTLS_GCM_C) || \
    defined(MBEDTLS_CCM_C) || \
    defined(MBEDTLS_CHACHAPOLY_C)
    if( mode == MBEDTLS_MODE_GCM ||
        mode == MBEDTLS_MODE_CCM ||
        mode == MBEDTLS_MODE_CHACHAPOLY )
    {
        int ret;
        unsigned char iv[12];
        size_t explicit_iv_len = transform->ivlen - transform->fixed_ivlen;

        /* Check that there's space for both the authentication tag
         * and the explicit IV before and after the record content. */
        if( post_avail < transform->taglen ||
            rec->data_offset < explicit_iv_len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Buffer provided for encrypted record not large enough" ) );
            return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
        }

        /*
         * Generate IV
         */
        if( transform->ivlen == 12 && transform->fixed_ivlen == 4 )
        {
            /* GCM and CCM: fixed || explicit (=seqnum) */
            mbedtls_platform_memcpy( iv, transform->iv_enc, transform->fixed_ivlen );
            mbedtls_platform_memcpy( iv + transform->fixed_ivlen, rec->ctr,
                    explicit_iv_len );
            /* Prefix record content with explicit IV. */
            mbedtls_platform_memcpy( data - explicit_iv_len, rec->ctr, explicit_iv_len );
        }
        else if( transform->ivlen == 12 && transform->fixed_ivlen == 12 )
        {
            /* ChachaPoly: fixed XOR sequence number */
            uint_fast8_t i;

            mbedtls_platform_memcpy( iv, transform->iv_enc, transform->fixed_ivlen );

            for( i = 0; i < 8; i++ )
                iv[i+4] ^= rec->ctr[i];
        }
        else
        {
            /* Reminder if we ever add an AEAD mode with a different size */
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        ssl_extract_add_data_from_record( add_data, &add_data_len, rec );

        MBEDTLS_SSL_DEBUG_BUF( 4, "IV used (internal)",
                                  iv, transform->ivlen );
        MBEDTLS_SSL_DEBUG_BUF( 4, "IV used (transmitted)",
                                  data - explicit_iv_len, explicit_iv_len );
        MBEDTLS_SSL_DEBUG_BUF( 4, "additional data used for AEAD",
                               add_data, add_data_len );
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "before encrypt: msglen = %d, "
                                    "including 0 bytes of padding",
                                    rec->data_len ) );

        /*
         * Encrypt and authenticate
         */
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        if( ( ret = mbedtls_ssl_switch_key( transform, MBEDTLS_ENCRYPT ) )
                != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_switch_key", ret );
            return( ret );
        }

        if( ( ret = encryption_status = mbedtls_cipher_auth_encrypt( &transform->cipher_ctx,
                   iv, transform->ivlen,
                   add_data, add_data_len,       /* add data     */
                   data, rec->data_len,          /* source       */
                   data, &rec->data_len,         /* destination  */
                   data + rec->data_len, transform->taglen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_auth_encrypt", ret );
            return( ret );
        }
#else
        if( ( ret = encryption_status = mbedtls_cipher_auth_encrypt( &transform->cipher_ctx_enc,
                   iv, transform->ivlen,
                   add_data, add_data_len,       /* add data     */
                   data, rec->data_len,          /* source       */
                   data, &rec->data_len,         /* destination  */
                   data + rec->data_len, transform->taglen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_auth_encrypt", ret );
            return( ret );
        }
#endif

        MBEDTLS_SSL_DEBUG_BUF( 4, "after encrypt: tag",
                               data + rec->data_len, transform->taglen );

        rec->data_len    += transform->taglen + explicit_iv_len;
        rec->data_offset -= explicit_iv_len;
        post_avail -= transform->taglen;
        auth_done++;
    }
    else
#endif /* MBEDTLS_GCM_C || MBEDTLS_CCM_C */
#if defined(MBEDTLS_CIPHER_MODE_CBC) &&                                    \
    ( defined(MBEDTLS_AES_C) || defined(MBEDTLS_CAMELLIA_C) || defined(MBEDTLS_ARIA_C) )
    if( mode == MBEDTLS_MODE_CBC )
    {
        int ret;
        size_t padlen, i;
        size_t olen;

        /* Currently we're always using minimal padding
         * (up to 255 bytes would be allowed). */
        padlen = transform->ivlen - ( rec->data_len + 1 ) % transform->ivlen;
        if( padlen == transform->ivlen )
            padlen = 0;

        /* Check there's enough space in the buffer for the padding. */
        if( post_avail < padlen + 1 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Buffer provided for encrypted record not large enough" ) );
            return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
        }

        for( i = 0; i <= padlen; i++ )
            data[rec->data_len + i] = (unsigned char) padlen;

        rec->data_len += padlen + 1;
        post_avail -= padlen + 1;

#if defined(MBEDTLS_SSL_PROTO_TLS1_1) || defined(MBEDTLS_SSL_PROTO_TLS1_2)
        /*
         * Prepend per-record IV for block cipher in TLS v1.1 and up as per
         * Method 1 (6.2.3.2. in RFC4346 and RFC5246)
         */
        if( mbedtls_ssl_ver_geq(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_2 ) )
        {
            if( f_rng == NULL )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "No PRNG provided to encrypt_record routine" ) );
                return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }

            if( rec->data_offset < transform->ivlen )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "Buffer provided for encrypted record not large enough" ) );
                return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
            }

            /*
             * Generate IV
             */
            ret = f_rng( p_rng, transform->iv_enc, transform->ivlen );
            if( ret != 0 )
                return( ret );

            mbedtls_platform_memcpy( data - transform->ivlen, transform->iv_enc,
                    transform->ivlen );

        }
#endif /* MBEDTLS_SSL_PROTO_TLS1_1 || MBEDTLS_SSL_PROTO_TLS1_2 */

        MBEDTLS_SSL_DEBUG_MSG( 3, ( "before encrypt: msglen = %d, "
                            "including %d bytes of IV and %d bytes of padding",
                            rec->data_len, transform->ivlen,
                            padlen + 1 ) );
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        if( ( ret = mbedtls_ssl_switch_key( transform, MBEDTLS_ENCRYPT ) )
             != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_switch_key", ret );
            return( ret );
        }

        if( ( ret = encryption_status = mbedtls_cipher_crypt( &transform->cipher_ctx,
                                   transform->iv_enc,
                                   transform->ivlen,
                                   data, rec->data_len,
                                   data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#else
        if( ( ret = encryption_status = mbedtls_cipher_crypt( &transform->cipher_ctx_enc,
                                   transform->iv_enc,
                                   transform->ivlen,
                                   data, rec->data_len,
                                   data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#endif

        if( rec->data_len != olen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1)
        if( mbedtls_ssl_ver_lt(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_2 ) )
        {
            /*
             * Save IV in SSL3 and TLS1
             */
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
            mbedtls_platform_memcpy( transform->iv_enc, transform->cipher_ctx.iv,
                    transform->ivlen );
#else
            mbedtls_platform_memcpy( transform->iv_enc, transform->cipher_ctx_enc.iv,
                    transform->ivlen );
#endif
        }
        else
#endif
        {
            data             -= transform->ivlen;
            rec->data_offset -= transform->ivlen;
            rec->data_len    += transform->ivlen;
        }

#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
        if( auth_done == 0 )
        {
            unsigned char mac[MBEDTLS_SSL_MAC_ADD];

            /*
             * MAC(MAC_write_key, seq_num +
             *     TLSCipherText.type +
             *     TLSCipherText.version +
             *     length_of( (IV +) ENC(...) ) +
             *     IV + // except for TLS 1.0
             *     ENC(content + padding + padding_length));
             */

            if( post_avail < transform->maclen)
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "Buffer provided for encrypted record not large enough" ) );
                return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
            }

            ssl_extract_add_data_from_record( add_data, &add_data_len, rec );

            MBEDTLS_SSL_DEBUG_MSG( 3, ( "using encrypt then mac" ) );
            MBEDTLS_SSL_DEBUG_BUF( 4, "MAC'd meta-data", add_data,
                                   add_data_len );

            mbedtls_md_hmac_update( &transform->md_ctx_enc, add_data,
                                    add_data_len );
            mbedtls_md_hmac_update( &transform->md_ctx_enc,
                                    data, rec->data_len );
            mbedtls_md_hmac_finish( &transform->md_ctx_enc, mac );
            mbedtls_md_hmac_reset( &transform->md_ctx_enc );

            mbedtls_platform_memcpy( data + rec->data_len, mac, transform->maclen );

            rec->data_len += transform->maclen;
            post_avail -= transform->maclen;
            auth_done++;
        }
#endif /* MBEDTLS_SSL_ENCRYPT_THEN_MAC */
    }
    else
#endif /* MBEDTLS_CIPHER_MODE_CBC &&
          ( MBEDTLS_AES_C || MBEDTLS_CAMELLIA_C || MBEDTLS_ARIA_C ) */
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    /* Make extra sure authentication was performed, exactly once */
    if( auth_done != 1 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= encrypt buf" ) );

    if( encryption_status == 0 )
    {
        return( 0 );
    }
    return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
}

int mbedtls_ssl_decrypt_buf( mbedtls_ssl_context const *ssl,
                             mbedtls_ssl_transform *transform,
                             mbedtls_record *rec )
{
    size_t olen;
    mbedtls_cipher_mode_t mode;
    int ret, auth_done = 0;
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
    size_t padlen = 0, correct = 1;
#endif
    unsigned char* data;
    unsigned char add_data[13 + 1 + MBEDTLS_SSL_CID_IN_LEN_MAX ];
    size_t add_data_len;

#if !defined(MBEDTLS_DEBUG_C)
    ssl = NULL; /* make sure we don't use it except for debug */
    ((void) ssl);
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> decrypt buf" ) );
    if( rec == NULL                     ||
        rec->buf == NULL                ||
        rec->buf_len < rec->data_offset ||
        rec->buf_len - rec->data_offset < rec->data_len )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad record structure provided to decrypt_buf" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    data = rec->buf + rec->data_offset;
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
    mode = mbedtls_cipher_get_cipher_mode( &transform->cipher_ctx );
#else
    mode = mbedtls_cipher_get_cipher_mode( &transform->cipher_ctx_dec );
#endif
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    /*
     * Match record's CID with incoming CID.
     */
    if( rec->cid_len != transform->in_cid_len ||
        memcmp( rec->cid, transform->in_cid, rec->cid_len ) != 0 ) // use regular memcmp as CID is public
    {
        return( MBEDTLS_ERR_SSL_UNEXPECTED_CID );
    }
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

#if defined(MBEDTLS_ARC4_C) || defined(MBEDTLS_CIPHER_NULL_CIPHER)
    if( mode == MBEDTLS_MODE_STREAM )
    {
        padlen = 0;
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        if( ( ret = mbedtls_ssl_switch_key( transform, MBEDTLS_DECRYPT ) )
                != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_switch_key", ret );
            return( ret );
        }
        if( ( ret = mbedtls_cipher_crypt( &transform->cipher_ctx,
                                   transform->iv_dec,
                                   transform->ivlen,
                                   data, rec->data_len,
                                   data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#else
        if( ( ret = mbedtls_cipher_crypt( &transform->cipher_ctx_dec,
                                   transform->iv_dec,
                                   transform->ivlen,
                                   data, rec->data_len,
                                   data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#endif
        if( rec->data_len != olen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }
    }
    else
#endif /* MBEDTLS_ARC4_C || MBEDTLS_CIPHER_NULL_CIPHER */
#if defined(MBEDTLS_GCM_C) || \
    defined(MBEDTLS_CCM_C) || \
    defined(MBEDTLS_CHACHAPOLY_C)
    if( mode == MBEDTLS_MODE_GCM ||
        mode == MBEDTLS_MODE_CCM ||
        mode == MBEDTLS_MODE_CHACHAPOLY )
    {
        unsigned char iv[12];
        size_t explicit_iv_len = transform->ivlen - transform->fixed_ivlen;

        /*
         * Prepare IV from explicit and implicit data.
         */

        /* Check that there's enough space for the explicit IV
         * (at the beginning of the record) and the MAC (at the
         * end of the record). */
        if( rec->data_len < explicit_iv_len + transform->taglen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "msglen (%d) < explicit_iv_len (%d) "
                                        "+ taglen (%d)", rec->data_len,
                                        explicit_iv_len, transform->taglen ) );
            return( MBEDTLS_ERR_SSL_INVALID_MAC );
        }

#if defined(MBEDTLS_GCM_C) || defined(MBEDTLS_CCM_C)
        if( transform->ivlen == 12 && transform->fixed_ivlen == 4 )
        {
            /* GCM and CCM: fixed || explicit */

            /* Fixed */
            mbedtls_platform_memcpy( iv, transform->iv_dec, transform->fixed_ivlen );
            /* Explicit */
            mbedtls_platform_memcpy( iv + transform->fixed_ivlen, data, 8 );
        }
        else
#endif /* MBEDTLS_GCM_C || MBEDTLS_CCM_C */
#if defined(MBEDTLS_CHACHAPOLY_C)
        if( transform->ivlen == 12 && transform->fixed_ivlen == 12 )
        {
            /* ChachaPoly: fixed XOR sequence number */
            uint_fast8_t i;

            mbedtls_platform_memcpy( iv, transform->iv_dec, transform->fixed_ivlen );

            for( i = 0; i < 8; i++ )
                iv[i+4] ^= rec->ctr[i];
        }
        else
#endif /* MBEDTLS_CHACHAPOLY_C */
        {
            /* Reminder if we ever add an AEAD mode with a different size */
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        /* Group changes to data, data_len, and add_data, because
         * add_data depends on data_len. */
        data += explicit_iv_len;
        rec->data_offset += explicit_iv_len;
        rec->data_len -= explicit_iv_len + transform->taglen;

        ssl_extract_add_data_from_record( add_data, &add_data_len, rec );
        MBEDTLS_SSL_DEBUG_BUF( 4, "additional data used for AEAD",
                               add_data, add_data_len );

        /* Because of the check above, we know that there are
         * explicit_iv_len Bytes preceeding data, and taglen
         * bytes following data + data_len. This justifies
         * the debug message and the invocation of
         * mbedtls_cipher_auth_decrypt() below. */

        MBEDTLS_SSL_DEBUG_BUF( 4, "IV used", iv, transform->ivlen );
        MBEDTLS_SSL_DEBUG_BUF( 4, "TAG used", data + rec->data_len,
                               transform->taglen );

        /*
         * Decrypt and authenticate
         */
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        if( ( ret = mbedtls_ssl_switch_key( transform, MBEDTLS_DECRYPT ) )
                != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_switch_key", ret );
            return( ret );
        }
        if( ( ret = mbedtls_cipher_auth_decrypt( &transform->cipher_ctx,
                  iv, transform->ivlen,
                  add_data, add_data_len,
                  data, rec->data_len,
                  data, &olen,
                  data + rec->data_len,
                  transform->taglen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_auth_decrypt", ret );

            if( ret == MBEDTLS_ERR_CIPHER_AUTH_FAILED )
                return( MBEDTLS_ERR_SSL_INVALID_MAC );

            return( ret );
        }
#else
        if( ( ret = mbedtls_cipher_auth_decrypt( &transform->cipher_ctx_dec,
                  iv, transform->ivlen,
                  add_data, add_data_len,
                  data, rec->data_len,
                  data, &olen,
                  data + rec->data_len,
                  transform->taglen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_auth_decrypt", ret );

            if( ret == MBEDTLS_ERR_CIPHER_AUTH_FAILED )
                return( MBEDTLS_ERR_SSL_INVALID_MAC );

            return( ret );
        }
#endif

        auth_done++;

        /* Double-check that AEAD decryption doesn't change content length. */
        if( olen != rec->data_len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }
    }
    else
#endif /* MBEDTLS_GCM_C || MBEDTLS_CCM_C */
#if defined(MBEDTLS_CIPHER_MODE_CBC) &&                                    \
    ( defined(MBEDTLS_AES_C) || defined(MBEDTLS_CAMELLIA_C) || defined(MBEDTLS_ARIA_C) )
    if( mode == MBEDTLS_MODE_CBC )
    {
        size_t minlen = 0;

        /*
         * Check immediate ciphertext sanity
         */
#if defined(MBEDTLS_SSL_PROTO_TLS1_1) || defined(MBEDTLS_SSL_PROTO_TLS1_2)
        if( mbedtls_ssl_ver_geq(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_2 ) )
        {
            /* The ciphertext is prefixed with the CBC IV. */
            minlen += transform->ivlen;
        }
#endif

        /* Size considerations:
         *
         * - The CBC cipher text must not be empty and hence
         *   at least of size transform->ivlen.
         *
         * Together with the potential IV-prefix, this explains
         * the first of the two checks below.
         *
         * - The record must contain a MAC, either in plain or
         *   encrypted, depending on whether Encrypt-then-MAC
         *   is used or not.
         *   - If it is, the message contains the IV-prefix,
         *     the CBC ciphertext, and the MAC.
         *   - If it is not, the padded plaintext, and hence
         *     the CBC ciphertext, has at least length maclen + 1
         *     because there is at least the padding length byte.
         *
         * As the CBC ciphertext is not empty, both cases give the
         * lower bound minlen + maclen + 1 on the record size, which
         * we test for in the second check below.
         */
        if( rec->data_len < minlen + transform->ivlen ||
            rec->data_len < minlen + transform->maclen + 1 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "msglen (%d) < max( ivlen(%d), maclen (%d) "
                                "+ 1 ) ( + expl IV )", rec->data_len,
                                transform->ivlen,
                                transform->maclen ) );
            return( MBEDTLS_ERR_SSL_INVALID_MAC );
        }

        /*
         * Authenticate before decrypt if enabled
         */
#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
        if( transform->encrypt_then_mac == MBEDTLS_SSL_ETM_ENABLED )
        {
            unsigned char mac_expect[MBEDTLS_SSL_MAC_ADD];

            MBEDTLS_SSL_DEBUG_MSG( 3, ( "using encrypt then mac" ) );

            /* Update data_len in tandem with add_data.
             *
             * The subtraction is safe because of the previous check
             * data_len >= minlen + maclen + 1.
             *
             * Afterwards, we know that data + data_len is followed by at
             * least maclen Bytes, which justifies the call to
             * mbedtls_platform_memequal() below.
             *
             * Further, we still know that data_len > minlen */
            rec->data_len -= transform->maclen;
            ssl_extract_add_data_from_record( add_data, &add_data_len, rec );

            /* Calculate expected MAC. */
            MBEDTLS_SSL_DEBUG_BUF( 4, "MAC'd meta-data", add_data,
                                   add_data_len );
            mbedtls_md_hmac_update( &transform->md_ctx_dec, add_data,
                                    add_data_len );
            mbedtls_md_hmac_update( &transform->md_ctx_dec,
                                    data, rec->data_len );
            mbedtls_md_hmac_finish( &transform->md_ctx_dec, mac_expect );
            mbedtls_md_hmac_reset( &transform->md_ctx_dec );

            MBEDTLS_SSL_DEBUG_BUF( 4, "message  mac", data + rec->data_len,
                                   transform->maclen );
            MBEDTLS_SSL_DEBUG_BUF( 4, "expected mac", mac_expect,
                                   transform->maclen );

            /* Compare expected MAC with MAC at the end of the record. */
            if( mbedtls_platform_memequal( data + rec->data_len, mac_expect,
                                           transform->maclen ) != 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "message mac does not match" ) );
                return( MBEDTLS_ERR_SSL_INVALID_MAC );
            }
            auth_done++;
        }
#endif /* MBEDTLS_SSL_ENCRYPT_THEN_MAC */

        /*
         * Check length sanity
         */

        /* We know from above that data_len > minlen >= 0,
         * so the following check in particular implies that
         * data_len >= minlen + ivlen ( = minlen or 2 * minlen ). */
        if( rec->data_len % transform->ivlen != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "msglen (%d) %% ivlen (%d) != 0",
                                        rec->data_len, transform->ivlen ) );
            return( MBEDTLS_ERR_SSL_INVALID_MAC );
        }

#if defined(MBEDTLS_SSL_PROTO_TLS1_1) || defined(MBEDTLS_SSL_PROTO_TLS1_2)
        /*
         * Initialize for prepended IV for block cipher in TLS v1.1 and up
         */
        if( mbedtls_ssl_ver_geq(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_2 ) )
        {
            /* Safe because data_len >= minlen + ivlen = 2 * ivlen. */
            mbedtls_platform_memcpy( transform->iv_dec, data, transform->ivlen );

            data += transform->ivlen;
            rec->data_offset += transform->ivlen;
            rec->data_len -= transform->ivlen;
        }
#endif /* MBEDTLS_SSL_PROTO_TLS1_1 || MBEDTLS_SSL_PROTO_TLS1_2 */

        /* We still have data_len % ivlen == 0 and data_len >= ivlen here. */
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        if( ( ret = mbedtls_ssl_switch_key( transform, MBEDTLS_DECRYPT ) )
                != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_switch_key", ret );
            return( ret );
        }

        if( ( ret = mbedtls_cipher_crypt( &transform->cipher_ctx,
                                   transform->iv_dec, transform->ivlen,
                                   data, rec->data_len, data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#else
        if( ( ret = mbedtls_cipher_crypt( &transform->cipher_ctx_dec,
                                   transform->iv_dec, transform->ivlen,
                                   data, rec->data_len, data, &olen ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_cipher_crypt", ret );
            return( ret );
        }
#endif
        /* Double-check that length hasn't changed during decryption. */
        if( rec->data_len != olen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1)
        if( mbedtls_ssl_ver_lt(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_2 ) )
        {
            /*
             * Save IV in SSL3 and TLS1, where CBC decryption of consecutive
             * records is equivalent to CBC decryption of the concatenation
             * of the records; in other words, IVs are maintained across
             * record decryptions.
             */
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
            mbedtls_platform_memcpy( transform->iv_dec, transform->cipher_ctx.iv,
                    transform->ivlen );
#else
            mbedtls_platform_memcpy( transform->iv_dec, transform->cipher_ctx_dec.iv,
                    transform->ivlen );
#endif
        }
#endif

        /* Safe since data_len >= minlen + maclen + 1, so after having
         * subtracted at most minlen and maclen up to this point,
         * data_len > 0 (because of data_len % ivlen == 0, it's actually
         * >= ivlen ). */
        padlen = data[rec->data_len - 1];

        if( auth_done == 1 )
        {
            correct *= ( rec->data_len >= padlen + 1 );
            padlen  *= ( rec->data_len >= padlen + 1 );
        }
        else
        {
#if defined(MBEDTLS_SSL_DEBUG_ALL)
            if( rec->data_len < transform->maclen + padlen + 1 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "msglen (%d) < maclen (%d) + padlen (%d)",
                                            rec->data_len,
                                            transform->maclen,
                                            padlen + 1 ) );
            }
#endif

            correct *= ( rec->data_len >= transform->maclen + padlen + 1 );
            padlen  *= ( rec->data_len >= transform->maclen + padlen + 1 );
        }

        padlen++;

        /* Regardless of the validity of the padding,
         * we have data_len >= padlen here. */

#if defined(MBEDTLS_SSL_PROTO_SSL3)
        if( mbedtls_ssl_transform_get_minor_ver( transform ) ==
            MBEDTLS_SSL_MINOR_VERSION_0 )
        {
            if( padlen > transform->ivlen )
            {
#if defined(MBEDTLS_SSL_DEBUG_ALL)
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad padding length: is %d, "
                                            "should be no more than %d",
                                            padlen, transform->ivlen ) );
#endif
                correct = 0;
            }
        }
        else
#endif /* MBEDTLS_SSL_PROTO_SSL3 */
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_2)
        if( mbedtls_ssl_ver_gt(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_0 ) )
        {
            /* The padding check involves a series of up to 256
             * consecutive memory reads at the end of the record
             * plaintext buffer. In order to hide the length and
             * validity of the padding, always perform exactly
             * `min(256,plaintext_len)` reads (but take into account
             * only the last `padlen` bytes for the padding check). */
            size_t pad_count = 0;
            size_t real_count = 0;
            volatile unsigned char* const check = data;

            /* Index of first padding byte; it has been ensured above
             * that the subtraction is safe. */
            size_t const padding_idx = rec->data_len - padlen;
            size_t const num_checks = rec->data_len <= 256 ? rec->data_len : 256;
            size_t const start_idx = rec->data_len - num_checks;
            size_t idx;

            for( idx = start_idx; idx < rec->data_len; idx++ )
            {
                real_count |= ( idx >= padding_idx );
                pad_count += real_count * ( check[idx] == padlen - 1 );
            }
            correct &= ( pad_count == padlen );

#if defined(MBEDTLS_SSL_DEBUG_ALL)
            if( padlen > 0 && correct == 0 )
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad padding byte detected" ) );
#endif
            padlen &= correct * 0x1FF;
        }
        else
#endif /* MBEDTLS_SSL_PROTO_TLS1 || MBEDTLS_SSL_PROTO_TLS1_1 || \
          MBEDTLS_SSL_PROTO_TLS1_2 */
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        /* If the padding was found to be invalid, padlen == 0
         * and the subtraction is safe. If the padding was found valid,
         * padlen hasn't been changed and the previous assertion
         * data_len >= padlen still holds. */
        rec->data_len -= padlen;
    }
    else
#endif /* MBEDTLS_CIPHER_MODE_CBC &&
          ( MBEDTLS_AES_C || MBEDTLS_CAMELLIA_C || MBEDTLS_ARIA_C ) */
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

#if defined(MBEDTLS_SSL_DEBUG_ALL)
    MBEDTLS_SSL_DEBUG_BUF( 4, "raw buffer after decryption",
                           data, rec->data_len );
#endif

    /*
     * Authenticate if not done yet.
     * Compute the MAC regardless of the padding result (RFC4346, CBCTIME).
     */
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
    if( auth_done == 0 )
    {
        unsigned char mac_expect[MBEDTLS_SSL_MAC_ADD];

        /* If the initial value of padlen was such that
         * data_len < maclen + padlen + 1, then padlen
         * got reset to 1, and the initial check
         * data_len >= minlen + maclen + 1
         * guarantees that at this point we still
         * have at least data_len >= maclen.
         *
         * If the initial value of padlen was such that
         * data_len >= maclen + padlen + 1, then we have
         * subtracted either padlen + 1 (if the padding was correct)
         * or 0 (if the padding was incorrect) since then,
         * hence data_len >= maclen in any case.
         */
        rec->data_len -= transform->maclen;
        ssl_extract_add_data_from_record( add_data, &add_data_len, rec );

#if defined(MBEDTLS_SSL_PROTO_SSL3)
        if( mbedtls_ssl_transform_get_minor_ver( transform ) ==
            MBEDTLS_SSL_MINOR_VERSION_0 )
        {
            ssl_mac( &transform->md_ctx_dec,
                     transform->mac_dec,
                     data, rec->data_len,
                     rec->ctr, rec->type,
                     mac_expect );
        }
        else
#endif /* MBEDTLS_SSL_PROTO_SSL3 */
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
        defined(MBEDTLS_SSL_PROTO_TLS1_2)
        if( mbedtls_ssl_ver_gt(
                mbedtls_ssl_transform_get_minor_ver( transform ),
                MBEDTLS_SSL_MINOR_VERSION_0 ) )
        {
            /*
             * Process MAC and always update for padlen afterwards to make
             * total time independent of padlen.
             *
             * Known timing attacks:
             *  - Lucky Thirteen (http://www.isg.rhul.ac.uk/tls/TLStiming.pdf)
             *
             * To compensate for different timings for the MAC calculation
             * depending on how much padding was removed (which is determined
             * by padlen), process extra_run more blocks through the hash
             * function.
             *
             * The formula in the paper is
             *   extra_run = ceil( (L1-55) / 64 ) - ceil( (L2-55) / 64 )
             * where L1 is the size of the header plus the decrypted message
             * plus CBC padding and L2 is the size of the header plus the
             * decrypted message. This is for an underlying hash function
             * with 64-byte blocks.
             * We use ( (Lx+8) / 64 ) to handle 'negative Lx' values
             * correctly. We round down instead of up, so -56 is the correct
             * value for our calculations instead of -55.
             *
             * Repeat the formula rather than defining a block_size variable.
             * This avoids requiring division by a variable at runtime
             * (which would be marginally less efficient and would require
             * linking an extra division function in some builds).
             */
            size_t j, extra_run = 0;
            unsigned char tmp[MBEDTLS_MD_MAX_BLOCK_SIZE];

            /*
             * The next two sizes are the minimum and maximum values of
             * in_msglen over all padlen values.
             *
             * They're independent of padlen, since we previously did
             * data_len -= padlen.
             *
             * Note that max_len + maclen is never more than the buffer
             * length, as we previously did in_msglen -= maclen too.
             */
            const size_t max_len = rec->data_len + padlen;
            const size_t min_len = ( max_len > 256 ) ? max_len - 256 : 0;

            memset( tmp, 0, sizeof( tmp ) );

            switch( mbedtls_md_get_type(
                        mbedtls_md_get_handle( &transform->md_ctx_dec ) ) )
            {
#if defined(MBEDTLS_MD5_C) || defined(MBEDTLS_SHA1_C) || \
    defined(MBEDTLS_SHA256_C)
                case MBEDTLS_MD_MD5:
                case MBEDTLS_MD_SHA1:
                case MBEDTLS_MD_SHA256:
                    /* 8 bytes of message size, 64-byte compression blocks */
                    extra_run =
                        ( add_data_len + rec->data_len + padlen + 8 ) / 64 -
                        ( add_data_len + rec->data_len          + 8 ) / 64;
                    break;
#endif
#if defined(MBEDTLS_SHA512_C)
                case MBEDTLS_MD_SHA384:
                    /* 16 bytes of message size, 128-byte compression blocks */
                    extra_run =
                        ( add_data_len + rec->data_len + padlen + 16 ) / 128 -
                        ( add_data_len + rec->data_len          + 16 ) / 128;
                    break;
#endif
                default:
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
                    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }

            extra_run &= correct * 0xFF;

            mbedtls_md_hmac_update( &transform->md_ctx_dec, add_data,
                                    add_data_len );
            mbedtls_md_hmac_update( &transform->md_ctx_dec, data,
                                    rec->data_len );
            /* Make sure we access everything even when padlen > 0. This
             * makes the synchronisation requirements for just-in-time
             * Prime+Probe attacks much tighter and hopefully impractical. */
            ssl_read_memory( data + rec->data_len, padlen );
            mbedtls_md_hmac_finish( &transform->md_ctx_dec, mac_expect );

            /* Call mbedtls_md_process at least once due to cache attacks
             * that observe whether md_process() was called of not */
            for( j = 0; j < extra_run + 1; j++ )
                mbedtls_md_process( &transform->md_ctx_dec, tmp );

            mbedtls_md_hmac_reset( &transform->md_ctx_dec );

            /* Make sure we access all the memory that could contain the MAC,
             * before we check it in the next code block. This makes the
             * synchronisation requirements for just-in-time Prime+Probe
             * attacks much tighter and hopefully impractical. */
            ssl_read_memory( data + min_len,
                             max_len - min_len + transform->maclen );
        }
        else
#endif /* MBEDTLS_SSL_PROTO_TLS1 || MBEDTLS_SSL_PROTO_TLS1_1 || \
              MBEDTLS_SSL_PROTO_TLS1_2 */
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

#if defined(MBEDTLS_SSL_DEBUG_ALL)
        MBEDTLS_SSL_DEBUG_BUF( 4, "expected mac", mac_expect, transform->maclen );
        MBEDTLS_SSL_DEBUG_BUF( 4, "message  mac", data + rec->data_len, transform->maclen );
#endif

        if( mbedtls_platform_memequal( data + rec->data_len, mac_expect,
                                       transform->maclen ) != 0 )
        {
#if defined(MBEDTLS_SSL_DEBUG_ALL)
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "message mac does not match" ) );
#endif
            correct = 0;
        }
        auth_done++;
    }

    /*
     * Finally check the correct flag
     */
    if( correct == 0 )
        return( MBEDTLS_ERR_SSL_INVALID_MAC );
#endif /* MBEDTLS_SSL_SOME_MODES_USE_MAC */

    /* Make extra sure authentication was performed, exactly once */
    if( auth_done != 1 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    if( rec->cid_len != 0 )
    {
        ret = ssl_cid_parse_inner_plaintext( data, &rec->data_len,
                                             &rec->type );
        if( ret != 0 )
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
    }
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= decrypt buf" ) );

    return( 0 );
}

#undef MAC_NONE
#undef MAC_PLAINTEXT
#undef MAC_CIPHERTEXT

#if defined(MBEDTLS_ZLIB_SUPPORT)
/*
 * Compression/decompression functions
 */
static int ssl_compress_buf( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned char *msg_post = ssl->out_msg;
    ptrdiff_t bytes_written = ssl->out_msg - ssl->out_buf;
    size_t len_pre = ssl->out_msglen;
    unsigned char *msg_pre = ssl->compress_buf;
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    size_t out_buf_len = ssl->out_buf_len;
#else
    size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> compress buf" ) );

    if( len_pre == 0 )
        return( 0 );

    mbedtls_platform_memcpy( msg_pre, ssl->out_msg, len_pre );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "before compression: msglen = %d, ",
                   ssl->out_msglen ) );

    MBEDTLS_SSL_DEBUG_BUF( 4, "before compression: output payload",
                   ssl->out_msg, ssl->out_msglen );

    ssl->transform_out->ctx_deflate.next_in = msg_pre;
    ssl->transform_out->ctx_deflate.avail_in = len_pre;
    ssl->transform_out->ctx_deflate.next_out = msg_post;
    ssl->transform_out->ctx_deflate.avail_out = out_buf_len - bytes_written;

    ret = deflate( &ssl->transform_out->ctx_deflate, Z_SYNC_FLUSH );
    if( ret != Z_OK )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "failed to perform compression (%d)", ret ) );
        return( MBEDTLS_ERR_SSL_COMPRESSION_FAILED );
    }

    ssl->out_msglen = out_buf_len -
                      ssl->transform_out->ctx_deflate.avail_out - bytes_written;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "after compression: msglen = %d, ",
                   ssl->out_msglen ) );

    MBEDTLS_SSL_DEBUG_BUF( 4, "after compression: output payload",
                   ssl->out_msg, ssl->out_msglen );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= compress buf" ) );

    return( 0 );
}

static int ssl_decompress_buf( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned char *msg_post = ssl->in_msg;
    ptrdiff_t header_bytes = ssl->in_msg - ssl->in_buf;
    size_t len_pre = ssl->in_msglen;
    unsigned char *msg_pre = ssl->compress_buf;
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    size_t in_buf_len = ssl->in_buf_len;
#else
    size_t in_buf_len = MBEDTLS_SSL_IN_BUFFER_LEN;
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> decompress buf" ) );

    if( len_pre == 0 )
        return( 0 );

    mbedtls_platform_memcpy( msg_pre, ssl->in_msg, len_pre );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "before decompression: msglen = %d, ",
                   ssl->in_msglen ) );

    MBEDTLS_SSL_DEBUG_BUF( 4, "before decompression: input payload",
                   ssl->in_msg, ssl->in_msglen );

    ssl->transform_in->ctx_inflate.next_in = msg_pre;
    ssl->transform_in->ctx_inflate.avail_in = len_pre;
    ssl->transform_in->ctx_inflate.next_out = msg_post;
    ssl->transform_in->ctx_inflate.avail_out = in_buf_len -
                                               header_bytes;

    ret = inflate( &ssl->transform_in->ctx_inflate, Z_SYNC_FLUSH );
    if( ret != Z_OK )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "failed to perform decompression (%d)", ret ) );
        return( MBEDTLS_ERR_SSL_COMPRESSION_FAILED );
    }

    ssl->in_msglen = in_buf_len -
                     ssl->transform_in->ctx_inflate.avail_out - header_bytes;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "after decompression: msglen = %d, ",
                   ssl->in_msglen ) );

    MBEDTLS_SSL_DEBUG_BUF( 4, "after decompression: input payload",
                   ssl->in_msg, ssl->in_msglen );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= decompress buf" ) );

    return( 0 );
}
#endif /* MBEDTLS_ZLIB_SUPPORT */

#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_RENEGOTIATION)
static int ssl_write_hello_request( mbedtls_ssl_context *ssl );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
static int ssl_resend_hello_request( mbedtls_ssl_context *ssl )
{
    /* If renegotiation is not enforced, retransmit until we would reach max
     * timeout if we were using the usual handshake doubling scheme */
    if( ssl->conf->renego_max_records < 0 )
    {
        uint32_t ratio =
            mbedtls_ssl_conf_get_hs_timeout_max( ssl->conf ) /
            mbedtls_ssl_conf_get_hs_timeout_min( ssl->conf ) + 1;
        int_fast8_t doublings = 1;

        while( ratio != 0 )
        {
            ++doublings;
            ratio >>= 1;
        }

        if( ++ssl->renego_records_seen > doublings )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "no longer retransmitting hello request" ) );
            return( 0 );
        }
    }

    return( ssl_write_hello_request( ssl ) );
}
#endif
#endif /* MBEDTLS_SSL_SRV_C && MBEDTLS_SSL_RENEGOTIATION */

/*
 * Fill the input message buffer by appending data to it.
 * The amount of data already fetched is in ssl->in_left.
 *
 * If we return 0, is it guaranteed that (at least) nb_want bytes are
 * available (from this read and/or a previous one). Otherwise, an error code
 * is returned (possibly EOF or WANT_READ).
 *
 * With stream transport (TLS) on success ssl->in_left == nb_want, but
 * with datagram transport (DTLS) on success ssl->in_left >= nb_want,
 * since we always read a whole datagram at once.
 *
 * For DTLS, it is up to the caller to set ssl->next_record_offset when
 * they're done reading a record.
 */
int mbedtls_ssl_fetch_input( mbedtls_ssl_context *ssl, size_t nb_want )
{
    int ret;
    size_t len;
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    size_t in_buf_len = ssl->in_buf_len;
#else
    size_t in_buf_len = MBEDTLS_SSL_IN_BUFFER_LEN;
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> fetch input" ) );

    if( mbedtls_ssl_get_recv( ssl ) == NULL &&
        mbedtls_ssl_get_recv_timeout( ssl ) == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Bad usage of mbedtls_ssl_set_bio() "
                            "or mbedtls_ssl_set_bio()" ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    if( nb_want > in_buf_len - (size_t)( ssl->in_hdr - ssl->in_buf ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "requesting more data than fits" ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        uint32_t timeout;

        /* Just to be sure */
        if( mbedtls_ssl_get_set_timer( ssl ) == NULL ||
            mbedtls_ssl_get_get_timer( ssl ) == NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "You must use "
                        "mbedtls_ssl_set_timer_cb() for DTLS" ) );
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
        }

        /*
         * The point is, we need to always read a full datagram at once, so we
         * sometimes read more then requested, and handle the additional data.
         * It could be the rest of the current record (while fetching the
         * header) and/or some other records in the same datagram.
         */

        /*
         * Move to the next record in the already read datagram if applicable
         */
        if( ssl->next_record_offset != 0 )
        {
            if( ssl->in_left < ssl->next_record_offset )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
                return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }

            ssl->in_left -= ssl->next_record_offset;

            if( ssl->in_left != 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "next record in same datagram, offset: %d",
                                    ssl->next_record_offset ) );
                memmove( ssl->in_hdr,
                         ssl->in_hdr + ssl->next_record_offset,
                         ssl->in_left );
            }

            ssl->next_record_offset = 0;
        }

        MBEDTLS_SSL_DEBUG_MSG( 2, ( "in_left: %d, nb_want: %d",
                       ssl->in_left, nb_want ) );

        /*
         * Done if we already have enough data.
         */
        if( nb_want <= ssl->in_left)
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= fetch input" ) );
            return( 0 );
        }

        /*
         * A record can't be split across datagrams. If we need to read but
         * are not at the beginning of a new record, the caller did something
         * wrong.
         */
        if( ssl->in_left != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        /*
         * Don't even try to read if time's out already.
         * This avoids by-passing the timer when repeatedly receiving messages
         * that will end up being dropped.
         */
        if( ssl_check_timer( ssl ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "timer has expired" ) );
            ret = MBEDTLS_ERR_SSL_TIMEOUT;
        }
        else
        {
            len = in_buf_len - ( ssl->in_hdr - ssl->in_buf );

            if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
                timeout = ssl->handshake->retransmit_timeout;
            else
                timeout = mbedtls_ssl_conf_get_read_timeout( ssl->conf );

            MBEDTLS_SSL_DEBUG_MSG( 3, ( "f_recv_timeout: %u ms", timeout ) );

            if( mbedtls_ssl_get_recv_timeout( ssl ) != NULL )
            {
                ret = mbedtls_ssl_get_recv_timeout( ssl )
                    ( ssl->p_bio, ssl->in_hdr, len, timeout );
            }
            else
            {
                ret = mbedtls_ssl_get_recv( ssl )
                    ( ssl->p_bio, ssl->in_hdr, len );
            }

            MBEDTLS_SSL_DEBUG_RET( 2, "ssl->f_recv(_timeout)", ret );

            if( ret == 0 )
                return( MBEDTLS_ERR_SSL_CONN_EOF );
        }

        if( ret == MBEDTLS_ERR_SSL_TIMEOUT )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "timeout" ) );
            ssl_set_timer( ssl, 0 );

            if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
            {
                if( ssl_double_retransmit_timeout( ssl ) != 0 )
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "handshake timeout" ) );
                    return( MBEDTLS_ERR_SSL_TIMEOUT );
                }

                if( ( ret = mbedtls_ssl_resend( ssl ) ) != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_resend", ret );
                    return( ret );
                }

                return( MBEDTLS_ERR_SSL_WANT_READ );
            }
#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_RENEGOTIATION)
            else if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
                       MBEDTLS_SSL_IS_SERVER &&
                     ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_PENDING )
            {
                if( ( ret = ssl_resend_hello_request( ssl ) ) != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "ssl_resend_hello_request", ret );
                    return( ret );
                }

                return( MBEDTLS_ERR_SSL_WANT_READ );
            }
#endif /* MBEDTLS_SSL_SRV_C && MBEDTLS_SSL_RENEGOTIATION */
        }

        if( ret < 0 )
            return( ret );

        ssl->in_left = ret;
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "in_left: %d, nb_want: %d",
                       ssl->in_left, nb_want ) );

        while( ssl->in_left < nb_want )
        {
            len = nb_want - ssl->in_left;

            if( ssl_check_timer( ssl ) != 0 )
                ret = MBEDTLS_ERR_SSL_TIMEOUT;
            else
            {
                if( mbedtls_ssl_get_recv_timeout( ssl ) != NULL )
                {
                    ret = mbedtls_ssl_get_recv_timeout( ssl )( ssl->p_bio,
                       ssl->in_hdr + ssl->in_left, len,
                       mbedtls_ssl_conf_get_read_timeout( ssl->conf ) );
                }
                else
                {
                    ret = mbedtls_ssl_get_recv( ssl )( ssl->p_bio,
                                       ssl->in_hdr + ssl->in_left, len );
                }
            }

            MBEDTLS_SSL_DEBUG_MSG( 2, ( "in_left: %d, nb_want: %d",
                                        ssl->in_left, nb_want ) );
            MBEDTLS_SSL_DEBUG_RET( 2, "ssl->f_recv(_timeout)", ret );

            if( ret == 0 )
                return( MBEDTLS_ERR_SSL_CONN_EOF );

            if( ret < 0 )
                return( ret );

            if ( (size_t)ret > len || ( INT_MAX > SIZE_MAX && ret > SIZE_MAX ) )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1,
                    ( "f_recv returned %d bytes but only %lu were requested",
                    ret, (unsigned long)len ) );
                return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }

            ssl->in_left += ret;
        }
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= fetch input" ) );

    return( 0 );
}

/*
 * Flush any data not yet written
 */
int mbedtls_ssl_flush_output( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned char *buf;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> flush output" ) );

    if( mbedtls_ssl_get_send( ssl ) == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Bad usage of mbedtls_ssl_set_bio() "
                            "or mbedtls_ssl_set_bio()" ) );
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    /* Avoid incrementing counter if data is flushed */
    if( ssl->out_left == 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= flush output" ) );
        return( 0 );
    }

    while( ssl->out_left > 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "message length: %d, out_left: %d",
                       mbedtls_ssl_out_hdr_len( ssl ) + ssl->out_msglen, ssl->out_left ) );

        buf = ssl->out_hdr - ssl->out_left;
        ret = mbedtls_ssl_get_send( ssl )( ssl->p_bio, buf, ssl->out_left );

        MBEDTLS_SSL_DEBUG_RET( 2, "ssl->f_send", ret );

        if( ret <= 0 )
            return( ret );

        if( (size_t)ret > ssl->out_left || ( INT_MAX > SIZE_MAX && ret > SIZE_MAX ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1,
                ( "f_send returned %d bytes but only %lu bytes were sent",
                ret, (unsigned long)ssl->out_left ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        ssl->out_left -= ret;
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        ssl->out_hdr = ssl->out_buf;
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        ssl->out_hdr = ssl->out_buf + 8;
    }
#endif
    ssl_update_out_pointers( ssl, ssl->transform_out );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= flush output" ) );

    return( 0 );
}

/*
 * Functions to handle the DTLS retransmission state machine
 */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
static int ssl_swap_epochs( mbedtls_ssl_context *ssl );

static int mbedtls_ssl_flight_transmit_msg( mbedtls_ssl_context *ssl, mbedtls_ssl_flight_item *msg )
{
    size_t max_frag_len;
    int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
    int const is_retransmitting =
            ( ssl->handshake->retransmit_state == MBEDTLS_SSL_RETRANS_SENDING );
    int const is_finished =
        ( msg->type == MBEDTLS_SSL_MSG_HANDSHAKE &&
          msg->p[0] == MBEDTLS_SSL_HS_FINISHED );

    uint8_t const force_flush = ssl->disable_datagram_packing == 1 ?
        SSL_FORCE_FLUSH : SSL_DONT_FORCE_FLUSH;

    /* Swap epochs before sending Finished: we can't do it after
     * sending ChangeCipherSpec, in case write returns WANT_READ.
     * Must be done before copying, may change out_msg pointer */
    if( is_retransmitting && is_finished && ssl->handshake->cur_msg_p == ( msg->p + 12 ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "swap epochs to send finished message" ) );
        if( ( ret = ssl_swap_epochs( ssl ) ) != 0 )
            return( ret );
    }

    ret = ssl_get_remaining_payload_in_datagram( ssl );
    if( ret < 0 )
        return( ret );
    max_frag_len = (size_t) ret;

    /* CCS is copied as is, while HS messages may need fragmentation */
    if( msg->type == MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC )
    {
        if( max_frag_len == 0 )
        {
            if( ( ret = mbedtls_ssl_flush_output( ssl ) ) != 0 )
                return( ret );

            return( 0 );
        }

        mbedtls_platform_memcpy( ssl->out_msg, msg->p, msg->len );
        ssl->out_msglen  = msg->len;
        ssl->out_msgtype = msg->type;

        /* Update position inside current message */
        ssl->handshake->cur_msg_p += msg->len;
    }
    else
    {
        const unsigned char * const p = ssl->handshake->cur_msg_p;
        const size_t hs_len = msg->len - 12;
        const size_t frag_off = p - ( msg->p + 12 );
        const size_t rem_len = hs_len - frag_off;
        size_t cur_hs_frag_len, max_hs_frag_len;

        if( ( max_frag_len < 12 ) || ( max_frag_len == 12 && hs_len != 0 ) )
        {
            if( is_finished && is_retransmitting )
            {
                if( ( ret = ssl_swap_epochs( ssl ) ) != 0 )
                    return( ret );
            }

            if( ( ret = mbedtls_ssl_flush_output( ssl ) ) != 0 )
                return( ret );

            return( 0 );
        }
        max_hs_frag_len = max_frag_len - 12;

        cur_hs_frag_len = rem_len > max_hs_frag_len ?
            max_hs_frag_len : rem_len;

        if( frag_off == 0 && cur_hs_frag_len != hs_len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "fragmenting handshake message (%u > %u)",
                                        (unsigned) cur_hs_frag_len,
                                        (unsigned) max_hs_frag_len ) );
        }

        /* Messages are stored with handshake headers as if not fragmented,
         * copy beginning of headers then fill fragmentation fields.
         * Handshake headers: type(1) len(3) seq(2) f_off(3) f_len(3) */
        mbedtls_platform_memcpy( ssl->out_msg, msg->p, 6 );

        (void)mbedtls_platform_put_uint24_be( &ssl->out_msg[6], frag_off );
        (void)mbedtls_platform_put_uint24_be( &ssl->out_msg[9],
                                              cur_hs_frag_len );

        MBEDTLS_SSL_DEBUG_BUF( 3, "handshake header", ssl->out_msg, 12 );

        /* Copy the handshake message content and set records fields */
        mbedtls_platform_memcpy( ssl->out_msg + 12, p, cur_hs_frag_len );
        ssl->out_msglen = cur_hs_frag_len + 12;
        ssl->out_msgtype = msg->type;

        /* Update position inside current message */
        ssl->handshake->cur_msg_p += cur_hs_frag_len;
    }

    /* If done with the current message move to the next one if any */
    if( ssl->handshake->cur_msg_p >= msg->p + msg->len )
    {
        if( msg->next != NULL )
        {
            ssl->handshake->cur_msg = msg->next;
            ssl->handshake->cur_msg_p = msg->next->p + 12;
        }
        else
        {
            ssl->handshake->cur_msg = NULL;
            ssl->handshake->cur_msg_p = NULL;
        }
    }

    /* Actually send the message out */
    if( ( ret = mbedtls_ssl_write_record( ssl, force_flush ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_write_record", ret );
        return( ret );
    }
    return( ret );
}

/*
 * Append current handshake message to current outgoing flight
 */
static int ssl_flight_append( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_flight_item *msg;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> ssl_flight_append" ) );
    MBEDTLS_SSL_DEBUG_BUF( 4, "message appended to flight",
                           ssl->out_msg, ssl->out_msglen );

    /* Allocate space for current message */
    if( ( msg = mbedtls_calloc( 1, sizeof(  mbedtls_ssl_flight_item ) ) ) == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc %d bytes failed",
                            sizeof( mbedtls_ssl_flight_item ) ) );
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    if( ( msg->p = mbedtls_calloc( 1, ssl->out_msglen ) ) == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc %d bytes failed", ssl->out_msglen ) );
        mbedtls_free( msg );
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    /* Copy current handshake message with headers */
    mbedtls_platform_memcpy( msg->p, ssl->out_msg, ssl->out_msglen );
    msg->len = ssl->out_msglen;
    msg->type = ssl->out_msgtype;
    msg->next = NULL;

    /* Append to the current flight */
    if( ssl->handshake->flight == NULL )
        ssl->handshake->flight = msg;
    else
    {
        mbedtls_ssl_flight_item *cur = ssl->handshake->flight;
        while( cur->next != NULL )
            cur = cur->next;
        cur->next = msg;
    }

#if defined(MBEDTLS_SSL_IMMEDIATE_TRANSMISSION)
    ssl->handshake->cur_msg = msg;
    ssl->handshake->cur_msg_p = msg->p + 12;
    {
        int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
        while( ssl->handshake->cur_msg != NULL )
        {
            if( ( ret = mbedtls_ssl_flight_transmit_msg( ssl, ssl->handshake->cur_msg ) ) != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_flight_transmit_msg", ret );
                return( ret );
            }

            if( ( ret = mbedtls_ssl_flush_output( ssl ) ) != 0 )
                return( ret );
        }
    }
#endif
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= ssl_flight_append" ) );
    return( 0 );
}

/*
 * Free the current flight of handshake messages
 */
static void ssl_flight_free( mbedtls_ssl_flight_item *flight )
{
    mbedtls_ssl_flight_item *cur = flight;
    mbedtls_ssl_flight_item *next;

    while( cur != NULL )
    {
        next = cur->next;

        mbedtls_free( cur->p );
        mbedtls_free( cur );

        cur = next;
    }
}

#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
static void ssl_dtls_replay_reset( mbedtls_ssl_context *ssl );
#endif

/*
 * Swap transform_out and out_ctr with the alternative ones
 */
static int ssl_swap_epochs( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_transform *tmp_transform;
    unsigned char tmp_out_ctr[8];
#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    int ret;
#endif /* MBEDTLS_SSL_HW_RECORD_ACCEL */

    if( ssl->transform_out == ssl->handshake->alt_transform_out )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "skip swap epochs" ) );
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "swap epochs" ) );

    /* Swap transforms */
    tmp_transform                     = ssl->transform_out;
    ssl->transform_out                = ssl->handshake->alt_transform_out;
    ssl->handshake->alt_transform_out = tmp_transform;

    /* Swap epoch + sequence_number */
    mbedtls_platform_memcpy( tmp_out_ctr,                 ssl->cur_out_ctr,            8 );
    mbedtls_platform_memcpy( ssl->cur_out_ctr,            ssl->handshake->alt_out_ctr, 8 );
    mbedtls_platform_memcpy( ssl->handshake->alt_out_ctr, tmp_out_ctr,                 8 );

    /* Adjust to the newly activated transform */
    ssl_update_out_pointers( ssl, ssl->transform_out );

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_activate != NULL )
    {
        if( ( ret = mbedtls_ssl_hw_record_activate( ssl, MBEDTLS_SSL_CHANNEL_OUTBOUND ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_hw_record_activate", ret );
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        }
    }
#endif /* MBEDTLS_SSL_HW_RECORD_ACCEL */

    return( 0 );
}

/*
 * Retransmit the current flight of messages.
 */
int mbedtls_ssl_resend( mbedtls_ssl_context *ssl )
{
    int ret = 0;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> mbedtls_ssl_resend" ) );

    ret = mbedtls_ssl_flight_transmit( ssl );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= mbedtls_ssl_resend" ) );

    return( ret );
}

#if defined(MBEDTLS_SSL_IMMEDIATE_TRANSMISSION)
void mbedtls_ssl_immediate_flight_done( mbedtls_ssl_context *ssl )
{
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> mbedtls_ssl_immediate_flight_done" ) );

    /* Update state and set timer */
    if( ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER )
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_FINISHED;
    else
    {
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_WAITING;
        ssl_set_timer( ssl, ssl->handshake->retransmit_timeout );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= mbedtls_ssl_immediate_flight_done" ) );
}
#endif

/*
 * Transmit or retransmit the current flight of messages.
 *
 * Need to remember the current message in case flush_output returns
 * WANT_WRITE, causing us to exit this function and come back later.
 * This function must be called until state is no longer SENDING.
 */
int mbedtls_ssl_flight_transmit( mbedtls_ssl_context *ssl )
{
    int ret;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> mbedtls_ssl_flight_transmit" ) );

    if( ssl->handshake->retransmit_state != MBEDTLS_SSL_RETRANS_SENDING )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "initialise flight transmission" ) );

        ssl->handshake->cur_msg = ssl->handshake->flight;
        ssl->handshake->cur_msg_p = ssl->handshake->flight->p + 12;
        if( ( ret = ssl_swap_epochs( ssl ) ) != 0 )
            return( ret );

        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_SENDING;
    }

    while( ssl->handshake->cur_msg != NULL )
    {
        if( ( ret = mbedtls_ssl_flight_transmit_msg( ssl, ssl->handshake->cur_msg ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_flight_transmit_msg", ret );
            return( ret );
        }
    }

    if( ( ret = mbedtls_ssl_flush_output( ssl ) ) != 0 )
        return( ret );

    /* Update state and set timer */
    if( ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER )
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_FINISHED;
    else
    {
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_WAITING;
        ssl_set_timer( ssl, ssl->handshake->retransmit_timeout );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= mbedtls_ssl_flight_transmit" ) );

    return( ret );
}

/*
 * To be called when the last message of an incoming flight is received.
 */
void mbedtls_ssl_recv_flight_completed( mbedtls_ssl_context *ssl )
{
    /* We won't need to resend that one any more */
    ssl_flight_free( ssl->handshake->flight );
    ssl->handshake->flight = NULL;
    ssl->handshake->cur_msg = NULL;

    /* The next incoming flight will start with this msg_seq */
    ssl->handshake->in_flight_start_seq = ssl->handshake->in_msg_seq;

    /* We don't want to remember CCS's across flight boundaries. */
    ssl->handshake->buffering.seen_ccs = 0;

    /* Clear future message buffering structure. */
    ssl_buffering_free( ssl );

    /* Cancel timer */
    ssl_set_timer( ssl, 0 );

    if( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE &&
        ssl->in_msg[0] == MBEDTLS_SSL_HS_FINISHED )
    {
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_FINISHED;
    }
    else
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_PREPARING;
}

/*
 * To be called when the last message of an outgoing flight is send.
 */
void mbedtls_ssl_send_flight_completed( mbedtls_ssl_context *ssl )
{
    ssl_reset_retransmit_timeout( ssl );
    ssl_set_timer( ssl, ssl->handshake->retransmit_timeout );

    if( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE &&
        ssl->in_msg[0] == MBEDTLS_SSL_HS_FINISHED )
    {
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_FINISHED;
    }
    else
        ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_WAITING;
}
#endif /* MBEDTLS_SSL_PROTO_DTLS */

/*
 * Handshake layer functions
 */

/*
 * Write (DTLS: or queue) current handshake (including CCS) message.
 *
 *  - fill in handshake headers
 *  - update handshake checksum
 *  - DTLS: save message for resending
 *  - then pass to the record layer
 *
 * DTLS: except for HelloRequest, messages are only queued, and will only be
 * actually sent when calling flight_transmit() or resend().
 *
 * Inputs:
 *  - ssl->out_msglen: 4 + actual handshake message len
 *      (4 is the size of handshake headers for TLS)
 *  - ssl->out_msg[0]: the handshake type (ClientHello, ServerHello, etc)
 *  - ssl->out_msg + 4: the handshake message body
 *
 * Outputs, ie state before passing to flight_append() or write_record():
 *   - ssl->out_msglen: the length of the record contents
 *      (including handshake headers but excluding record headers)
 *   - ssl->out_msg: the record contents (handshake headers + content)
 */
int mbedtls_ssl_write_handshake_msg( mbedtls_ssl_context *ssl )
{
    int ret;
    const size_t hs_len = ssl->out_msglen - 4;
    const unsigned char hs_type = ssl->out_msg[0];

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write handshake message" ) );

    /*
     * Sanity checks
     */
    if( ssl->out_msgtype != MBEDTLS_SSL_MSG_HANDSHAKE          &&
        ssl->out_msgtype != MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC )
    {
        /* In SSLv3, the client might send a NoCertificate alert. */
#if defined(MBEDTLS_SSL_PROTO_SSL3) && defined(MBEDTLS_SSL_CLI_C)
        if( ! ( mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_0 &&
                ssl->out_msgtype    == MBEDTLS_SSL_MSG_ALERT       &&
                mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
                  MBEDTLS_SSL_IS_CLIENT ) )
#endif /* MBEDTLS_SSL_PROTO_SSL3 && MBEDTLS_SSL_SRV_C */
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }
    }

    /* Whenever we send anything different from a
     * HelloRequest we should be in a handshake - double check. */
    if( ! ( ssl->out_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE &&
            hs_type          == MBEDTLS_SSL_HS_HELLO_REQUEST ) &&
        ssl->handshake == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        ssl->handshake != NULL &&
        ssl->handshake->retransmit_state == MBEDTLS_SSL_RETRANS_SENDING )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }
#endif

    /* Double-check that we did not exceed the bounds
     * of the outgoing record buffer.
     * This should never fail as the various message
     * writing functions must obey the bounds of the
     * outgoing record buffer, but better be safe.
     *
     * Note: We deliberately do not check for the MTU or MFL here.
     */
    if( ssl->out_msglen > MBEDTLS_SSL_OUT_CONTENT_LEN )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Record too large: "
                                    "size %u, maximum %u",
                                    (unsigned) ssl->out_msglen,
                                    (unsigned) MBEDTLS_SSL_OUT_CONTENT_LEN ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    /*
     * Fill handshake headers
     */
    if( ssl->out_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE )
    {
        (void)mbedtls_platform_put_uint24_be( &ssl->out_msg[1], hs_len );

        /*
         * DTLS has additional fields in the Handshake layer,
         * between the length field and the actual payload:
         *      uint16 message_seq;
         *      uint24 fragment_offset;
         *      uint24 fragment_length;
         */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
        if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        {
            /* Make room for the additional DTLS fields */
            if( MBEDTLS_SSL_OUT_CONTENT_LEN - ssl->out_msglen < 8 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "DTLS handshake message too large: "
                              "size %u, maximum %u",
                               (unsigned) ( hs_len ),
                               (unsigned) ( MBEDTLS_SSL_OUT_CONTENT_LEN - 12 ) ) );
                return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
            }

            memmove( ssl->out_msg + 12, ssl->out_msg + 4, hs_len );
            ssl->out_msglen += 8;

            /* Write message_seq and update it, except for HelloRequest */
            if( hs_type != MBEDTLS_SSL_HS_HELLO_REQUEST )
            {
                (void)mbedtls_platform_put_uint16_be( &ssl->out_msg[4],
                    ssl->handshake->out_msg_seq );
                ++( ssl->handshake->out_msg_seq );
            }
            else
            {
                ssl->out_msg[4] = 0;
                ssl->out_msg[5] = 0;
            }

            /* Handshake hashes are computed without fragmentation,
             * so set frag_offset = 0 and frag_len = hs_len for now */
            if( mbedtls_platform_memset( ssl->out_msg + 6, 0x00, 3 ) !=
                                         ssl->out_msg + 6 )
            {
                return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
            }
            if( mbedtls_platform_memcpy( ssl->out_msg + 9, ssl->out_msg + 1, 3 ) !=
                                         ssl->out_msg + 9 )
            {
                return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
            }
        }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

        /* Update running hashes of handshake messages seen */
        if( hs_type != MBEDTLS_SSL_HS_HELLO_REQUEST )
            mbedtls_ssl_update_checksum( ssl, ssl->out_msg, ssl->out_msglen );
    }

    /* Either send now, or just save to be sent (and resent) later */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        ! ( ssl->out_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE &&
            hs_type          == MBEDTLS_SSL_HS_HELLO_REQUEST ) )
    {
        if( ( ret = ssl_flight_append( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_flight_append", ret );
            return( ret );
        }
    }
    else
#endif
    {
        if( ( ret = mbedtls_ssl_write_record( ssl, SSL_FORCE_FLUSH ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_write_record", ret );
            return( ret );
        }
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write handshake message" ) );

    return( 0 );
}

/*
 * Record layer functions
 */

/*
 * Write current record.
 *
 * Uses:
 *  - ssl->out_msgtype: type of the message (AppData, Handshake, Alert, CCS)
 *  - ssl->out_msglen: length of the record content (excl headers)
 *  - ssl->out_msg: record content
 */
int mbedtls_ssl_write_record( mbedtls_ssl_context *ssl, uint8_t force_flush )
{
    int ret, done = 0;
    size_t len = ssl->out_msglen;
    uint8_t flush = force_flush;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write record" ) );

#if defined(MBEDTLS_ZLIB_SUPPORT)
    if( ssl->transform_out != NULL &&
        ssl->session_out->compression == MBEDTLS_SSL_COMPRESS_DEFLATE )
    {
        if( ( ret = ssl_compress_buf( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_compress_buf", ret );
            return( ret );
        }

        len = ssl->out_msglen;
    }
#endif /*MBEDTLS_ZLIB_SUPPORT */

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_write != NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "going for mbedtls_ssl_hw_record_write()" ) );

        ret = mbedtls_ssl_hw_record_write( ssl );
        if( ret != 0 && ret != MBEDTLS_ERR_SSL_HW_ACCEL_FALLTHROUGH )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_hw_record_write", ret );
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        }

        if( ret == 0 )
            done = 1;
    }
#endif /* MBEDTLS_SSL_HW_RECORD_ACCEL */
    if( !done )
    {
        unsigned i;
        size_t protected_record_size;
        volatile int encrypted_fi = 0;
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
        size_t out_buf_len = ssl->out_buf_len;
#else
        size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;
#endif

        /* Skip writing the record content type to after the encryption,
         * as it may change when using the CID extension. */

        mbedtls_ssl_write_version( mbedtls_ssl_get_major_ver( ssl ),
                                   mbedtls_ssl_get_minor_ver( ssl ),
                                   mbedtls_ssl_conf_get_transport( ssl->conf ),
                                   ssl->out_hdr + 1 );

        mbedtls_platform_memcpy( ssl->out_ctr, ssl->cur_out_ctr, 8 );
        (void)mbedtls_platform_put_uint16_be( ssl->out_len, len );

        if( ssl->transform_out != NULL )
        {
            mbedtls_record rec;

            rec.buf         = ssl->out_iv;
            rec.buf_len     = out_buf_len - ( ssl->out_iv - ssl->out_buf );
            rec.data_len    = ssl->out_msglen;
            rec.data_offset = ssl->out_msg - rec.buf;

            mbedtls_platform_memcpy( &rec.ctr[0], ssl->out_ctr, 8 );
            mbedtls_ssl_write_version( mbedtls_ssl_get_major_ver( ssl ),
                                       mbedtls_ssl_get_minor_ver( ssl ),
                                       mbedtls_ssl_conf_get_transport( ssl->conf ),
                                       rec.ver );
            rec.type = ssl->out_msgtype;

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
            /* The CID is set by mbedtls_ssl_encrypt_buf(). */
            rec.cid_len = 0;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

            if( ( ret = mbedtls_ssl_encrypt_buf( ssl, ssl->transform_out, &rec,
                                         mbedtls_ssl_conf_get_frng( ssl->conf ),
                                         mbedtls_ssl_conf_get_prng( ssl->conf ) ) ) != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "ssl_encrypt_buf", ret );
                return( ret );
            }

            if( rec.data_offset != 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
                return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }

            /* Update the record content type and CID. */
            ssl->out_msgtype = rec.type;
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID )
            /* Not using more secure mbedtls_platform_memcpy as cid is public */
            memcpy( ssl->out_cid, rec.cid, rec.cid_len );
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
            ssl->out_msglen = len = rec.data_len;
            (void)mbedtls_platform_put_uint16_be( ssl->out_len, rec.data_len );
            encrypted_fi = 1;
        }

        /* Double check to ensure the encryption has been done */
        if( ssl->transform_out != NULL && encrypted_fi == 0 )
        {
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        }

        protected_record_size = len + mbedtls_ssl_out_hdr_len( ssl );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
        /* In case of DTLS, double-check that we don't exceed
         * the remaining space in the datagram. */
        if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        {
            ret = ssl_get_remaining_space_in_datagram( ssl );
            if( ret < 0 )
                return( ret );

            if( protected_record_size > (size_t) ret )
            {
                /* Should never happen */
                return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }
        }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

        /* Now write the potentially updated record content type. */
        ssl->out_hdr[0] = (unsigned char) ssl->out_msgtype;

        MBEDTLS_SSL_DEBUG_MSG( 3, ( "output record: msgtype = %d, "
                                    "version = [%d:%d], msglen = %d",
                                    ssl->out_hdr[0], ssl->out_hdr[1],
                                    ssl->out_hdr[2], len ) );

        MBEDTLS_SSL_DEBUG_BUF( 4, "output record sent to network",
                               ssl->out_hdr, protected_record_size );

        ssl->out_left += protected_record_size;
        ssl->out_hdr  += protected_record_size;
        ssl_update_out_pointers( ssl, ssl->transform_out );

        for( i = 8; i > ssl_ep_len( ssl ); i-- )
            if( ++ssl->cur_out_ctr[i - 1] != 0 )
                break;

        /* The loop goes to its end iff the counter is wrapping */
        if( i == ssl_ep_len( ssl ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "outgoing message counter would wrap" ) );
            return( MBEDTLS_ERR_SSL_COUNTER_WRAPPING );
        }
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        flush == SSL_DONT_FORCE_FLUSH )
    {
        size_t remaining;
        ret = ssl_get_remaining_payload_in_datagram( ssl );
        if( ret < 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_get_remaining_payload_in_datagram",
                                   ret );
            return( ret );
        }

        remaining = (size_t) ret;
        if( remaining == 0 )
        {
            flush = SSL_FORCE_FLUSH;
        }
        else
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "Still %u bytes available in current datagram", (unsigned) remaining ) );
        }
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    if( ( flush == SSL_FORCE_FLUSH ) &&
        ( ret = mbedtls_ssl_flush_output( ssl ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_flush_output", ret );
        return( ret );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write record" ) );

    return( 0 );
}

#if defined(MBEDTLS_SSL_PROTO_DTLS)

static int ssl_hs_is_proper_fragment( mbedtls_ssl_context *ssl )
{
    if( ssl->in_msglen < ssl->in_hslen ||
        mbedtls_platform_memequal( ssl->in_msg + 6, "\0\0\0",        3 ) != 0 ||
        mbedtls_platform_memequal( ssl->in_msg + 9, ssl->in_msg + 1, 3 ) != 0 )
    {
        return( PROPER_HS_FRAGMENT );
    }
    return( 0 );
}

static uint32_t ssl_get_hs_frag_len( mbedtls_ssl_context const *ssl )
{
    return( (uint32_t)mbedtls_platform_get_uint24_be( &ssl->in_msg[9] ) );
}

static uint32_t ssl_get_hs_frag_off( mbedtls_ssl_context const *ssl )
{
    return( (uint32_t)mbedtls_platform_get_uint24_be( &ssl->in_msg[6] ) );
}

static int ssl_check_hs_header( mbedtls_ssl_context const *ssl )
{
    uint32_t msg_len, frag_off, frag_len;

    msg_len  = ssl_get_hs_total_len( ssl );
    frag_off = ssl_get_hs_frag_off( ssl );
    frag_len = ssl_get_hs_frag_len( ssl );

    if( frag_off > msg_len )
        return( -1 );

    if( frag_len > msg_len - frag_off )
        return( -1 );

    if( frag_len + 12 > ssl->in_msglen )
        return( -1 );

    return( 0 );
}

/*
 * Mark bits in bitmask (used for DTLS HS reassembly)
 */
static void ssl_bitmask_set( unsigned char *mask, size_t offset, size_t len )
{
    unsigned int start_bits, end_bits;

    start_bits = 8 - ( offset % 8 );
    if( start_bits != 8 )
    {
        size_t first_byte_idx = offset / 8;

        /* Special case */
        if( len <= start_bits )
        {
            for( ; len != 0; len-- )
                mask[first_byte_idx] |= 1 << ( start_bits - len );

            /* Avoid potential issues with offset or len becoming invalid */
            return;
        }

        offset += start_bits; /* Now offset % 8 == 0 */
        len -= start_bits;

        for( ; start_bits != 0; start_bits-- )
            mask[first_byte_idx] |= 1 << ( start_bits - 1 );
    }

    end_bits = len % 8;
    if( end_bits != 0 )
    {
        size_t last_byte_idx = ( offset + len ) / 8;

        len -= end_bits; /* Now len % 8 == 0 */

        for( ; end_bits != 0; end_bits-- )
            mask[last_byte_idx] |= 1 << ( 8 - end_bits );
    }

    memset( mask + offset / 8, 0xFF, len / 8 );
}

#define BITMASK_CHECK_FAILED 0x75555555
/*
 * Check that bitmask is full
 */
static int ssl_bitmask_check( unsigned char *mask, size_t len )
{
    size_t i;

    for( i = 0; i < len / 8; i++ )
        if( mask[i] != 0xFF )
            return( BITMASK_CHECK_FAILED );

    for( i = 0; i < len % 8; i++ )
        if( ( mask[len / 8] & ( 1 << ( 7 - i ) ) ) == 0 )
            return( BITMASK_CHECK_FAILED );

    return( 0 );
}

/* msg_len does not include the handshake header */
static size_t ssl_get_reassembly_buffer_size( size_t msg_len,
                                              unsigned add_bitmap )
{
    size_t alloc_len;

    alloc_len  = 12;                                 /* Handshake header */
    alloc_len += msg_len;                            /* Content buffer   */

    if( add_bitmap )
        alloc_len += msg_len / 8 + ( msg_len % 8 != 0 ); /* Bitmap       */

    return( alloc_len );
}

#endif /* MBEDTLS_SSL_PROTO_DTLS */

static uint32_t ssl_get_hs_total_len( mbedtls_ssl_context const *ssl )
{
    return( (uint32_t)mbedtls_platform_get_uint24_be( &ssl->in_msg[1] ) );
}

int mbedtls_ssl_prepare_handshake_record( mbedtls_ssl_context *ssl )
{
    if( ssl->in_msglen < mbedtls_ssl_hs_hdr_len( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "handshake message too short: %d",
                            ssl->in_msglen ) );
        return( MBEDTLS_ERR_SSL_INVALID_RECORD );
    }

    ssl->in_hslen = mbedtls_ssl_hs_hdr_len( ssl ) + ssl_get_hs_total_len( ssl );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "handshake message: msglen ="
                        " %d, type = %d, hslen = %d",
                        ssl->in_msglen, ssl->in_msg[0], ssl->in_hslen ) );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        int ret;
        unsigned int recv_msg_seq = (unsigned int)
            mbedtls_platform_get_uint16_be( &ssl->in_msg[4] );

        if( ssl_check_hs_header( ssl ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid handshake header" ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }

        if( ssl->handshake != NULL &&
            ( ( ssl->state   != MBEDTLS_SSL_HANDSHAKE_OVER &&
                recv_msg_seq != ssl->handshake->in_msg_seq ) ||
              ( ssl->state  == MBEDTLS_SSL_HANDSHAKE_OVER &&
                ssl->in_msg[0] != MBEDTLS_SSL_HS_CLIENT_HELLO ) ) )
        {
            if( recv_msg_seq > ssl->handshake->in_msg_seq )
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "received future handshake message of sequence number %u (next %u)",
                                            recv_msg_seq,
                                            ssl->handshake->in_msg_seq ) );
                return( MBEDTLS_ERR_SSL_EARLY_MESSAGE );
            }

            /* Retransmit only on last message from previous flight, to avoid
             * too many retransmissions.
             * Besides, No sane server ever retransmits HelloVerifyRequest */
            if( recv_msg_seq == ssl->handshake->in_flight_start_seq - 1 &&
                ssl->in_msg[0] != MBEDTLS_SSL_HS_HELLO_VERIFY_REQUEST )
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "received message from last flight, "
                                    "message_seq = %d, start_of_flight = %d",
                                    recv_msg_seq,
                                    ssl->handshake->in_flight_start_seq ) );

                if( ( ret = mbedtls_ssl_resend( ssl ) ) != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_resend", ret );
                    return( ret );
                }
            }
            else
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "dropping out-of-sequence message: "
                                    "message_seq = %d, expected = %d",
                                    recv_msg_seq,
                                    ssl->handshake->in_msg_seq ) );
            }

            return( MBEDTLS_ERR_SSL_CONTINUE_PROCESSING );
        }
        /* Wait until message completion to increment in_msg_seq */

        /* Message reassembly is handled alongside buffering of future
         * messages; the commonality is that both handshake fragments and
         * future messages cannot be forwarded immediately to the
         * handshake logic layer. */
        if( ssl_hs_is_proper_fragment( ssl ) == PROPER_HS_FRAGMENT )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "found fragmented DTLS handshake message" ) );
            return( MBEDTLS_ERR_SSL_EARLY_MESSAGE );
        }
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        /* With TLS we don't handle fragmentation (for now) */
        if( ssl->in_msglen < ssl->in_hslen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "TLS handshake fragmentation not supported" ) );
            return( MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE );
        }
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */

    return( 0 );
}

void mbedtls_ssl_update_handshake_status( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;

    if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER && hs != NULL )
        mbedtls_ssl_update_checksum( ssl, ssl->in_msg, ssl->in_hslen );

    /* Handshake message is complete, increment counter */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        ssl->handshake != NULL )
    {
        unsigned offset;
        mbedtls_ssl_hs_buffer *hs_buf;

        /* Increment handshake sequence number */
        hs->in_msg_seq++;

        /*
         * Clear up handshake buffering and reassembly structure.
         */

        /* Free first entry */
        ssl_buffering_free_slot( ssl, 0 );

        /* Shift all other entries */
        for( offset = 0, hs_buf = &hs->buffering.hs[0];
             offset + 1 < MBEDTLS_SSL_MAX_BUFFERED_HS;
             offset++, hs_buf++ )
        {
            *hs_buf = *(hs_buf + 1);
        }

        /* Create a fresh last entry */
        mbedtls_platform_memset( hs_buf, 0, sizeof( mbedtls_ssl_hs_buffer ) );
    }
#endif
}

/*
 * DTLS anti-replay: RFC 6347 4.1.2.6
 *
 * in_window is a field of bits numbered from 0 (lsb) to 63 (msb).
 * Bit n is set iff record number in_window_top - n has been seen.
 *
 * Usually, in_window_top is the last record number seen and the lsb of
 * in_window is set. The only exception is the initial state (record number 0
 * not seen yet).
 */
#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
static void ssl_dtls_replay_reset( mbedtls_ssl_context *ssl )
{
    ssl->in_window_top = 0;
    ssl->in_window = 0;
}

static inline uint64_t ssl_load_six_bytes( unsigned char *buf )
{
    return( ( (uint64_t) buf[0] << 40 ) |
            ( (uint64_t) buf[1] << 32 ) |
            ( (uint64_t) buf[2] << 24 ) |
            ( (uint64_t) buf[3] << 16 ) |
            ( (uint64_t) buf[4] <<  8 ) |
            ( (uint64_t) buf[5]       ) );
}

static int mbedtls_ssl_dtls_record_replay_check( mbedtls_ssl_context *ssl, uint8_t *record_in_ctr )
{
    int ret;
    unsigned char *original_in_ctr;

    // save original in_ctr
    original_in_ctr = ssl->in_ctr;

    // use counter from record
    ssl->in_ctr = record_in_ctr;

    ret = mbedtls_ssl_dtls_replay_check( (mbedtls_ssl_context const *) ssl );

    // restore the counter
    ssl->in_ctr = original_in_ctr;

    return ret;
}

/*
 * Return 0 if sequence number is acceptable, -1 otherwise
 */
int mbedtls_ssl_dtls_replay_check( mbedtls_ssl_context const *ssl )
{
    uint64_t rec_seqnum = ssl_load_six_bytes( ssl->in_ctr + 2 );
    uint64_t bit;

    if( mbedtls_ssl_conf_get_anti_replay( ssl->conf ) ==
        MBEDTLS_SSL_ANTI_REPLAY_DISABLED )
    {
        return( 0 );
    }

    if( rec_seqnum > ssl->in_window_top )
        return( 0 );

    bit = ssl->in_window_top - rec_seqnum;

    if( bit >= 64 )
        return( -1 );

    if( ( ssl->in_window & ( (uint64_t) 1 << bit ) ) != 0 )
        return( -1 );

    return( 0 );
}

/*
 * Update replay window on new validated record
 */
void mbedtls_ssl_dtls_replay_update( mbedtls_ssl_context *ssl )
{
    uint64_t rec_seqnum = ssl_load_six_bytes( ssl->in_ctr + 2 );

    if( mbedtls_ssl_conf_get_anti_replay( ssl->conf ) ==
        MBEDTLS_SSL_ANTI_REPLAY_DISABLED )
    {
        return;
    }

    if( rec_seqnum > ssl->in_window_top )
    {
        /* Update window_top and the contents of the window */
        uint64_t shift = rec_seqnum - ssl->in_window_top;

        if( shift >= 64 )
            ssl->in_window = 1;
        else
        {
            ssl->in_window <<= shift;
            ssl->in_window |= 1;
        }

        ssl->in_window_top = rec_seqnum;
    }
    else
    {
        /* Mark that number as seen in the current window */
        uint64_t bit = ssl->in_window_top - rec_seqnum;

        if( bit < 64 ) /* Always true, but be extra sure */
            ssl->in_window |= (uint64_t) 1 << bit;
    }
}
#endif /* MBEDTLS_SSL_DTLS_ANTI_REPLAY */

#if defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE) && defined(MBEDTLS_SSL_SRV_C)
/* Forward declaration */
static int ssl_session_reset_int( mbedtls_ssl_context *ssl, int partial );

/*
 * Without any SSL context, check if a datagram looks like a ClientHello with
 * a valid cookie, and if it doesn't, generate a HelloVerifyRequest message.
 * Both input and output include full DTLS headers.
 *
 * - if cookie is valid, return 0
 * - if ClientHello looks superficially valid but cookie is not,
 *   fill obuf and set olen, then
 *   return MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED
 * - otherwise return a specific error code
 */
static int ssl_check_dtls_clihlo_cookie(
                           mbedtls_ssl_cookie_write_t *f_cookie_write,
                           mbedtls_ssl_cookie_check_t *f_cookie_check,
                           void *p_cookie,
                           const unsigned char *cli_id, size_t cli_id_len,
                           const unsigned char *in, size_t in_len,
                           unsigned char *obuf, size_t buf_len, size_t *olen )
{
    size_t sid_len, cookie_len;
    unsigned char *p;

    /*
     * Structure of ClientHello with record and handshake headers,
     * and expected values. We don't need to check a lot, more checks will be
     * done when actually parsing the ClientHello - skipping those checks
     * avoids code duplication and does not make cookie forging any easier.
     *
     *  0-0  ContentType type;                  copied, must be handshake
     *  1-2  ProtocolVersion version;           copied
     *  3-4  uint16 epoch;                      copied, must be 0
     *  5-10 uint48 sequence_number;            copied
     * 11-12 uint16 length;                     (ignored)
     *
     * 13-13 HandshakeType msg_type;            (ignored)
     * 14-16 uint24 length;                     (ignored)
     * 17-18 uint16 message_seq;                copied
     * 19-21 uint24 fragment_offset;            copied, must be 0
     * 22-24 uint24 fragment_length;            (ignored)
     *
     * 25-26 ProtocolVersion client_version;    (ignored)
     * 27-58 Random random;                     (ignored)
     * 59-xx SessionID session_id;              1 byte len + sid_len content
     * 60+   opaque cookie<0..2^8-1>;           1 byte len + content
     *       ...
     *
     * Minimum length is 61 bytes.
     */
    if( in_len < 61 ||
        in[0] != MBEDTLS_SSL_MSG_HANDSHAKE ||
        in[3] != 0 || in[4] != 0 ||
        in[19] != 0 || in[20] != 0 || in[21] != 0 )
    {
        return( MBEDTLS_ERR_SSL_BAD_HS_CLIENT_HELLO );
    }

    sid_len = in[59];
    if( sid_len > in_len - 61 )
        return( MBEDTLS_ERR_SSL_BAD_HS_CLIENT_HELLO );

    cookie_len = in[60 + sid_len];
    if( cookie_len > in_len - 60 )
        return( MBEDTLS_ERR_SSL_BAD_HS_CLIENT_HELLO );

    if( f_cookie_check( p_cookie, in + sid_len + 61, cookie_len,
                        cli_id, cli_id_len ) == 0 )
    {
        /* Valid cookie */
        return( 0 );
    }

    /*
     * If we get here, we've got an invalid cookie, let's prepare HVR.
     *
     *  0-0  ContentType type;                  copied
     *  1-2  ProtocolVersion version;           copied
     *  3-4  uint16 epoch;                      copied
     *  5-10 uint48 sequence_number;            copied
     * 11-12 uint16 length;                     olen - 13
     *
     * 13-13 HandshakeType msg_type;            hello_verify_request
     * 14-16 uint24 length;                     olen - 25
     * 17-18 uint16 message_seq;                copied
     * 19-21 uint24 fragment_offset;            copied
     * 22-24 uint24 fragment_length;            olen - 25
     *
     * 25-26 ProtocolVersion server_version;    0xfe 0xff
     * 27-27 opaque cookie<0..2^8-1>;           cookie_len = olen - 27, cookie
     *
     * Minimum length is 28.
     */
    if( buf_len < 28 )
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );

    /* Copy most fields and adapt others */
    mbedtls_platform_memcpy( obuf, in, 25 );
    obuf[13] = MBEDTLS_SSL_HS_HELLO_VERIFY_REQUEST;
    obuf[25] = 0xfe;
    obuf[26] = 0xff;

    /* Generate and write actual cookie */
    p = obuf + 28;
    if( f_cookie_write( p_cookie,
                        &p, obuf + buf_len, cli_id, cli_id_len ) != 0 )
    {
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    *olen = p - obuf;

    /* Go back and fill length fields */
    obuf[27] = (unsigned char)( *olen - 28 );

    (void)mbedtls_platform_put_uint24_be( &obuf[14], ( *olen - 25 ) );
    obuf[22] = obuf[14];
    obuf[23] = obuf[15];
    obuf[24] = obuf[16];

    (void)mbedtls_platform_put_uint16_be( &obuf[11], ( *olen - 13 ) );

    return( MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED );
}

/*
 * Handle possible client reconnect with the same UDP quadruplet
 * (RFC 6347 Section 4.2.8).
 *
 * Called by ssl_parse_record_header() in case we receive an epoch 0 record
 * that looks like a ClientHello.
 *
 * - if the input looks like a ClientHello without cookies,
 *   send back HelloVerifyRequest, then return 0
 * - if the input looks like a ClientHello with a valid cookie,
 *   reset the session of the current context, and
 *   return MBEDTLS_ERR_SSL_CLIENT_RECONNECT
 * - if anything goes wrong, return a specific error code
 *
 * This function is called (through ssl_check_client_reconnect()) when an
 * unexpected record is found in ssl_get_next_record(), which will discard the
 * record if we return 0, and bubble up the return value otherwise (this
 * includes the case of MBEDTLS_ERR_SSL_CLIENT_RECONNECT and of unexpected
 * errors, and is the right thing to do in both cases).
 */
static int ssl_handle_possible_reconnect( mbedtls_ssl_context *ssl )
{
    int ret;
    size_t len;

    if( ssl->conf->f_cookie_write == NULL ||
        ssl->conf->f_cookie_check == NULL )
    {
        /* If we can't use cookies to verify reachability of the peer,
         * drop the record. */
         MBEDTLS_SSL_DEBUG_MSG( 1, ( "no cookie callbacks, "
                                     "can't check reconnect validity" ) );
        return( 0 );
    }

    ret = ssl_check_dtls_clihlo_cookie(
            ssl->conf->f_cookie_write,
            ssl->conf->f_cookie_check,
            ssl->conf->p_cookie,
            ssl->cli_id, ssl->cli_id_len,
            ssl->in_buf, ssl->in_left,
            ssl->out_buf, MBEDTLS_SSL_OUT_CONTENT_LEN, &len );

    MBEDTLS_SSL_DEBUG_RET( 2, "ssl_check_dtls_clihlo_cookie", ret );

    if( ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED )
    {
        int send_ret;
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "sending HelloVerifyRequest" ) );
        MBEDTLS_SSL_DEBUG_BUF( 4, "output record sent to network",
                                  ssl->out_buf, len );
        /* Don't check write errors as we can't do anything here.
         * If the error is permanent we'll catch it later,
         * if it's not, then hopefully it'll work next time. */
        send_ret = mbedtls_ssl_get_send( ssl )( ssl->p_bio, ssl->out_buf, len );
        MBEDTLS_SSL_DEBUG_RET( 2, "mbedtls_ssl_get_send", send_ret );
        (void) send_ret;
        return( 0 );
    }

    if( ret == 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "cookie is valid, resetting context" ) );
        if( ( ret = ssl_session_reset_int( ssl, 1 ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "reset", ret );
            return( ret );
        }

        return( MBEDTLS_ERR_SSL_CLIENT_RECONNECT );
    }

    return( ret );
}
#endif /* MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE && MBEDTLS_SSL_SRV_C */

static int ssl_check_record_type( uint8_t record_type )
{
    if( record_type != MBEDTLS_SSL_MSG_HANDSHAKE &&
        record_type != MBEDTLS_SSL_MSG_ALERT &&
        record_type != MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC &&
        record_type != MBEDTLS_SSL_MSG_APPLICATION_DATA )
    {
        return( MBEDTLS_ERR_SSL_INVALID_RECORD );
    }

    return( 0 );
}

/*
 * ContentType type;
 * ProtocolVersion version;
 * uint16 epoch;            // DTLS only
 * uint48 sequence_number;  // DTLS only
 * uint16 length;
 *
 * Return 0 if header looks sane (and, for DTLS, the record is expected)
 * MBEDTLS_ERR_SSL_INVALID_RECORD if the header looks bad,
 * MBEDTLS_ERR_SSL_UNEXPECTED_RECORD (DTLS only) if sane but unexpected.
 *
 * With DTLS, mbedtls_ssl_read_record() will:
 * 1. proceed with the record if this function returns 0
 * 2. drop only the current record if this function returns UNEXPECTED_RECORD
 * 3. return CLIENT_RECONNECT if this function return that value
 * 4. drop the whole datagram if this function returns anything else.
 * Point 2 is needed when the peer is resending, and we have already received
 * the first record from a datagram but are still waiting for the others.
 */
static int ssl_parse_record_header( mbedtls_ssl_context const *ssl,
                                    unsigned char *buf,
                                    size_t len,
                                    mbedtls_record *rec )
{
    int major_ver, minor_ver;

    size_t const rec_hdr_type_offset    = 0;
    size_t const rec_hdr_type_len       = 1;

    size_t const rec_hdr_version_offset = rec_hdr_type_offset +
                                          rec_hdr_type_len;
    size_t const rec_hdr_version_len    = 2;

    size_t const rec_hdr_ctr_len        = 8;
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    uint32_t     rec_epoch;
    size_t const rec_hdr_ctr_offset     = rec_hdr_version_offset +
                                          rec_hdr_version_len;

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    size_t const rec_hdr_cid_offset     = rec_hdr_ctr_offset +
                                          rec_hdr_ctr_len;
    size_t       rec_hdr_cid_len        = 0;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    size_t       rec_hdr_len_offset; /* To be determined */
    size_t const rec_hdr_len_len    = 2;

    /*
     * Check minimum lengths for record header.
     */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        rec_hdr_len_offset = rec_hdr_ctr_offset + rec_hdr_ctr_len;
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        rec_hdr_len_offset = rec_hdr_version_offset + rec_hdr_version_len;
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    if( len < rec_hdr_len_offset + rec_hdr_len_len )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "datagram of length %u too small to hold DTLS record header of length %u",
                 (unsigned) len,
                 (unsigned)( rec_hdr_len_len + rec_hdr_len_len ) ) );
        return( MBEDTLS_ERR_SSL_INVALID_RECORD );
    }

    /*
     * Parse and validate record content type
     */

    rec->type = buf[ rec_hdr_type_offset ];

    /* Check record content type */
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    rec->cid_len = 0;

    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        mbedtls_ssl_conf_get_cid_len( ssl->conf ) != 0        &&
        rec->type == MBEDTLS_SSL_MSG_CID )
    {
        /* Shift pointers to account for record header including CID
         * struct {
         *   ContentType special_type = tls12_cid;
         *   ProtocolVersion version;
         *   uint16 epoch;
         *   uint48 sequence_number;
         *   opaque cid[cid_length]; // Additional field compared to
         *                           // default DTLS record format
         *   uint16 length;
         *   opaque enc_content[DTLSCiphertext.length];
         * } DTLSCiphertext;
         */

        /* So far, we only support static CID lengths
         * fixed in the configuration. */
        rec_hdr_cid_len = mbedtls_ssl_conf_get_cid_len( ssl->conf );
        rec_hdr_len_offset += rec_hdr_cid_len;

        if( len < rec_hdr_len_offset + rec_hdr_len_len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "datagram of length %u too small to hold DTLS record header including CID, length %u",
                (unsigned) len,
                (unsigned)( rec_hdr_len_offset + rec_hdr_len_len ) ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }

        /* configured CID len is guaranteed at most 255, see
         * MBEDTLS_SSL_CID_OUT_LEN_MAX in check_config.h */
        rec->cid_len = (uint8_t) rec_hdr_cid_len;
        /* Not using more secure mbedtls_platform_memcpy as cid is public */
        memcpy( rec->cid, buf + rec_hdr_cid_offset, rec_hdr_cid_len );
    }
    else
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
    {
        if( ssl_check_record_type( rec->type ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "unknown record type %u",
                                        (unsigned) rec->type ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }
    }

    /*
     * Parse and validate record version
     */

    rec->ver[0] = buf[ rec_hdr_version_offset + 0 ];
    rec->ver[1] = buf[ rec_hdr_version_offset + 1 ];
    mbedtls_ssl_read_version( &major_ver, &minor_ver,
                              mbedtls_ssl_conf_get_transport( ssl->conf ),
                              &rec->ver[0] );

    if( major_ver != mbedtls_ssl_get_major_ver( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "major version mismatch" ) );
        return( MBEDTLS_ERR_SSL_INVALID_RECORD );
    }

    if( mbedtls_ssl_ver_gt( minor_ver,
                            mbedtls_ssl_conf_get_max_minor_ver( ssl->conf ) ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "minor version mismatch" ) );
        return( MBEDTLS_ERR_SSL_INVALID_RECORD );
    }

    /*
     * Parse/Copy record sequence number.
     */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        /* Copy explicit record sequence number from input buffer. */
        /* Not using more secure mbedtls_platform_memcpy as sequence number is public */
        memcpy( &rec->ctr[0], buf + rec_hdr_ctr_offset,
                rec_hdr_ctr_len );
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        /* Copy implicit record sequence number from SSL context structure. */
        /* Not using more secure mbedtls_platform_memcpy as sequence number is public */
        memcpy( &rec->ctr[0], ssl->in_ctr, rec_hdr_ctr_len );
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */

    /*
     * Parse record length.
     */

    rec->data_offset = rec_hdr_len_offset + rec_hdr_len_len;
    rec->data_len = mbedtls_platform_get_uint16_be(  &buf[rec_hdr_len_offset] );
    MBEDTLS_SSL_DEBUG_BUF( 4, "input record header", buf, rec->data_offset );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "input record: msgtype = %d, "
                                "version = [%d:%d], msglen = %d",
                                rec->type,
                                major_ver, minor_ver, rec->data_len ) );

    rec->buf     = buf;
    rec->buf_len = rec->data_offset + rec->data_len;

    if( rec->data_len == 0 )
        return( MBEDTLS_ERR_SSL_INVALID_RECORD );

    /*
     * DTLS-related tests.
     * Check epoch before checking length constraint because
     * the latter varies with the epoch. E.g., if a ChangeCipherSpec
     * message gets duplicated before the corresponding Finished message,
     * the second ChangeCipherSpec should be discarded because it belongs
     * to an old epoch, but not because its length is shorter than
     * the minimum record length for packets using the new record transform.
     * Note that these two kinds of failures are handled differently,
     * as an unexpected record is silently skipped but an invalid
     * record leads to the entire datagram being dropped.
     */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        rec_epoch = (uint32_t)mbedtls_platform_get_uint16_be( rec->ctr );

        /* Check that the datagram is large enough to contain a record
         * of the advertised length. */
        if( len < rec->data_offset + rec->data_len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Datagram of length %u too small to contain record of advertised length %u.",
                             (unsigned) len,
                             (unsigned)( rec->data_offset + rec->data_len ) ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }

        /* Records from other, non-matching epochs are silently discarded.
         * (The case of same-port Client reconnects must be considered in
         *  the caller). */
        if( rec_epoch != ssl->in_epoch )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "record from another epoch: "
                                        "expected %d, received %d",
                                        ssl->in_epoch, rec_epoch ) );

            /* Records from the next epoch are considered for buffering
             * (concretely: early Finished messages). */
            if( rec_epoch == (unsigned) ssl->in_epoch + 1 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "Consider record for buffering" ) );
                return( MBEDTLS_ERR_SSL_EARLY_MESSAGE );
            }

            return( MBEDTLS_ERR_SSL_UNEXPECTED_RECORD );
        }
#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
        /* For records from the correct epoch, check whether their
         * sequence number has been seen before. */
        else if( mbedtls_ssl_dtls_record_replay_check( (mbedtls_ssl_context *) ssl,
            &rec->ctr[0] ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "replayed record" ) );
            return( MBEDTLS_ERR_SSL_UNEXPECTED_RECORD );
        }
#endif
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    return( 0 );
}


#if defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE) && defined(MBEDTLS_SSL_SRV_C)
static int ssl_check_client_reconnect( mbedtls_ssl_context *ssl )
{
    unsigned int rec_epoch = (unsigned int)
        mbedtls_platform_get_uint16_be( &ssl->in_ctr[0] );

    /*
     * Check for an epoch 0 ClientHello. We can't use in_msg here to
     * access the first byte of record content (handshake type), as we
     * have an active transform (possibly iv_len != 0), so use the
     * fact that the record header len is 13 instead.
     */
    if( rec_epoch == 0 &&
        mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
        MBEDTLS_SSL_IS_SERVER &&
        ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER &&
        ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE &&
        ssl->in_left > 13 &&
        ssl->in_buf[13] == MBEDTLS_SSL_HS_CLIENT_HELLO )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "possible client reconnect "
                                    "from the same port" ) );
        return( ssl_handle_possible_reconnect( ssl ) );
    }

    return( 0 );
}
#endif /* MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE && MBEDTLS_SSL_SRV_C */

/*
 * If applicable, decrypt record content
 */
static int ssl_prepare_record_content( mbedtls_ssl_context *ssl,
                                       mbedtls_record *rec )
{
    int ret, done = 0;

    MBEDTLS_SSL_DEBUG_BUF( 4, "input record from network",
                           rec->buf, rec->buf_len );

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_read != NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "going for mbedtls_ssl_hw_record_read()" ) );

        ret = mbedtls_ssl_hw_record_read( ssl );
        if( ret != 0 && ret != MBEDTLS_ERR_SSL_HW_ACCEL_FALLTHROUGH )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_hw_record_read", ret );
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        }

        if( ret == 0 )
            done = 1;
    }
#endif /* MBEDTLS_SSL_HW_RECORD_ACCEL */
    if( !done && ssl->transform_in != NULL )
    {
        unsigned char const old_msg_type = rec->type;

        if( ( ret = mbedtls_ssl_decrypt_buf( ssl, ssl->transform_in,
                                             rec ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_decrypt_buf", ret );

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
            if( ret == MBEDTLS_ERR_SSL_UNEXPECTED_CID &&
                mbedtls_ssl_conf_get_ignore_unexpected_cid( ssl->conf )
                  == MBEDTLS_SSL_UNEXPECTED_CID_IGNORE )
            {
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "ignoring unexpected CID" ) );
                ret = MBEDTLS_ERR_SSL_CONTINUE_PROCESSING;
            }
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

            return( ret );
        }

        if( old_msg_type != rec->type )
        {
            MBEDTLS_SSL_DEBUG_MSG( 4, ( "record type after decrypt (before %d): %d",
                                        old_msg_type, rec->type ) );
        }

        MBEDTLS_SSL_DEBUG_BUF( 4, "input payload after decrypt",
                               rec->buf + rec->data_offset, rec->data_len );

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
        /* We have already checked the record content type
         * in ssl_parse_record_header(), failing or silently
         * dropping the record in the case of an unknown type.
         *
         * Since with the use of CIDs, the record content type
         * might change during decryption, re-check the record
         * content type, but treat a failure as fatal this time. */
        if( ssl_check_record_type( rec->type ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "unknown record type" ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

        if( rec->data_len == 0 )
        {
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
            if( mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_3
                && rec->type != MBEDTLS_SSL_MSG_APPLICATION_DATA )
            {
                /* TLS v1.2 explicitly disallows zero-length messages which are not application data */
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid zero-length message type: %d", ssl->in_msgtype ) );
                return( MBEDTLS_ERR_SSL_INVALID_RECORD );
            }
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */

            ssl->nb_zero++;

            /*
             * Three or more empty messages may be a DoS attack
             * (excessive CPU consumption).
             */
            if( ssl->nb_zero > 3 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "received four consecutive empty "
                                            "messages, possible DoS attack" ) );
                /* Treat the records as if they were not properly authenticated,
                 * thereby failing the connection if we see more than allowed
                 * by the configured bad MAC threshold. */
                return( MBEDTLS_ERR_SSL_INVALID_MAC );
            }
        }
        else
            ssl->nb_zero = 0;

        /* Only needed for TLS, as with DTLS in_ctr is read from the header */
#if defined(MBEDTLS_SSL_PROTO_TLS)
        if( MBEDTLS_SSL_TRANSPORT_IS_TLS( ssl->conf->transport ) )
        {
            unsigned i;
            for( i = 8; i > 0; i-- )
                if( ++ssl->in_ctr[i - 1] != 0 )
                    break;

            /* The loop goes to its end only if the counter is wrapping around */
            if( i == 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "incoming message counter would wrap" ) );
                return( MBEDTLS_ERR_SSL_COUNTER_WRAPPING );
            }
        }
#endif /* MBEDTLS_SSL_PROTO_TLS */

    }

#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        mbedtls_ssl_dtls_replay_update( ssl );
    }
#endif

    /* Check actual (decrypted) record content length against
     * configured maximum. */
    if( ssl->in_msglen > MBEDTLS_SSL_IN_CONTENT_LEN )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad message length" ) );
        return( MBEDTLS_ERR_SSL_INVALID_RECORD );
    }

    return( 0 );
}

static void ssl_handshake_wrapup_free_hs_transform( mbedtls_ssl_context *ssl );

/*
 * Read a record.
 *
 * Silently ignore non-fatal alert (and for DTLS, invalid records as well,
 * RFC 6347 4.1.2.7) and continue reading until a valid record is found.
 *
 */

/* Helper functions for mbedtls_ssl_read_record(). */
static int ssl_consume_current_message( mbedtls_ssl_context *ssl );
static int ssl_get_next_record( mbedtls_ssl_context *ssl );
static int ssl_record_is_in_progress( mbedtls_ssl_context *ssl );

int mbedtls_ssl_read_record( mbedtls_ssl_context *ssl,
                             unsigned update_hs_digest )
{
    int ret;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> read record" ) );

    if( ssl->keep_current_message == 0 )
    {
        do {

            ret = ssl_consume_current_message( ssl );
            if( ret != 0 )
                return( ret );

            if( ssl_record_is_in_progress( ssl ) == 0 )
            {
#if defined(MBEDTLS_SSL_PROTO_DTLS)
                int have_buffered = 0;

                /* We only check for buffered messages if the
                 * current datagram is fully consumed. */
                if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
                    ssl_next_record_is_in_datagram( ssl ) == 0 )
                {
                    if( ssl_load_buffered_message( ssl ) == 0 )
                        have_buffered = 1;
                }

                if( have_buffered == 0 )
#endif /* MBEDTLS_SSL_PROTO_DTLS */
                {
                    ret = ssl_get_next_record( ssl );
                    if( ret == MBEDTLS_ERR_SSL_CONTINUE_PROCESSING )
                        continue;

                    if( ret != 0 )
                    {
                        MBEDTLS_SSL_DEBUG_RET( 1, ( "ssl_get_next_record" ), ret );
                        ssl_send_pending_fatal_alert( ssl );
                        return( ret );
                    }
                }
            }

            ret = mbedtls_ssl_handle_message_type( ssl );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
            if( ret == MBEDTLS_ERR_SSL_EARLY_MESSAGE )
            {
                /* Buffer future message */
                ret = ssl_buffer_message( ssl );
                if( ret != 0 )
                    return( ret );

                ret = MBEDTLS_ERR_SSL_CONTINUE_PROCESSING;
            }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

        } while( MBEDTLS_ERR_SSL_NON_FATAL           == ret  ||
                 MBEDTLS_ERR_SSL_CONTINUE_PROCESSING == ret );

        if( 0 != ret )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, ( "mbedtls_ssl_handle_message_type" ), ret );
            return( ret );
        }

        if( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE &&
            update_hs_digest == 1 )
        {
            mbedtls_ssl_update_handshake_status( ssl );
        }
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "reuse previously read message" ) );
        ssl->keep_current_message = 0;
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= read record" ) );

    return( 0 );
}

#if defined(MBEDTLS_SSL_PROTO_DTLS)
static int ssl_next_record_is_in_datagram( mbedtls_ssl_context *ssl )
{
    if( ssl->in_left > ssl->next_record_offset )
        return( 1 );

    return( 0 );
}

static int ssl_load_buffered_message( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;
    mbedtls_ssl_hs_buffer * hs_buf;
    int ret = 0;

    if( hs == NULL )
        return( -1 );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> ssl_load_buffered_messsage" ) );

    if( ssl->state == MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC ||
        ssl->state == MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC )
    {
        /* Check if we have seen a ChangeCipherSpec before.
         * If yes, synthesize a CCS record. */
        if( !hs->buffering.seen_ccs )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "CCS not seen in the current flight" ) );
            ret = -1;
            goto exit;
        }

        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Injecting buffered CCS message" ) );
        ssl->in_msgtype = MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC;
        ssl->in_msglen = 1;
        ssl->in_msg[0] = 1;

        /* As long as they are equal, the exact value doesn't matter. */
        ssl->in_left            = 0;
        ssl->next_record_offset = 0;

        hs->buffering.seen_ccs = 0;
        goto exit;
    }

#if defined(MBEDTLS_DEBUG_C)
    /* Debug only */
    {
        unsigned offset;
        for( offset = 1; offset < MBEDTLS_SSL_MAX_BUFFERED_HS; offset++ )
        {
            hs_buf = &hs->buffering.hs[offset];
            if( hs_buf->is_valid == 1 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 2, ( "Future message with sequence number %u %s buffered.",
                            hs->in_msg_seq + offset,
                            hs_buf->is_complete ? "fully" : "partially" ) );
            }
        }
    }
#endif /* MBEDTLS_DEBUG_C */

    /* Check if we have buffered and/or fully reassembled the
     * next handshake message. */
    hs_buf = &hs->buffering.hs[0];
    if( ( hs_buf->is_valid == 1 ) && ( hs_buf->is_complete == 1 ) )
    {
        /* Synthesize a record containing the buffered HS message. */
        size_t msg_len = mbedtls_platform_get_uint24_be( &hs_buf->data[1] );

        /* Double-check that we haven't accidentally buffered
         * a message that doesn't fit into the input buffer. */
        if( msg_len + 12 > MBEDTLS_SSL_IN_CONTENT_LEN )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Next handshake message has been buffered - load" ) );
        MBEDTLS_SSL_DEBUG_BUF( 3, "Buffered handshake message (incl. header)",
                               hs_buf->data, msg_len + 12 );

        ssl->in_msgtype = MBEDTLS_SSL_MSG_HANDSHAKE;
        ssl->in_hslen   = msg_len + 12;
        ssl->in_msglen  = msg_len + 12;
        mbedtls_platform_memcpy( ssl->in_msg, hs_buf->data, ssl->in_hslen );

        ret = 0;
        goto exit;
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Next handshake message %u not or only partially bufffered",
                                    hs->in_msg_seq ) );
    }

    ret = -1;

exit:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= ssl_load_buffered_message" ) );
    return( ret );
}

static int ssl_buffer_make_space( mbedtls_ssl_context *ssl,
                                  size_t desired )
{
    int offset;
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "Attempt to free buffered messages to have %u bytes available",
                                (unsigned) desired ) );

    /* Get rid of future records epoch first, if such exist. */
    ssl_free_buffered_record( ssl );

    /* Check if we have enough space available now. */
    if( desired <= ( MBEDTLS_SSL_DTLS_MAX_BUFFERING -
                     hs->buffering.total_bytes_buffered ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Enough space available after freeing future epoch record" ) );
        return( 0 );
    }

    /* We don't have enough space to buffer the next expected handshake
     * message. Remove buffers used for future messages to gain space,
     * starting with the most distant one. */
    for( offset = MBEDTLS_SSL_MAX_BUFFERED_HS - 1;
         offset >= 0; offset-- )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Free buffering slot %d to make space for reassembly of next handshake message",
                                    offset ) );

        ssl_buffering_free_slot( ssl, (uint8_t) offset );

        /* Check if we have enough space available now. */
        if( desired <= ( MBEDTLS_SSL_DTLS_MAX_BUFFERING -
                         hs->buffering.total_bytes_buffered ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "Enough space available after freeing buffered HS messages" ) );
            return( 0 );
        }
    }

    return( -1 );
}

static int ssl_buffer_message( mbedtls_ssl_context *ssl )
{
    int ret = 0;
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;

    if( hs == NULL )
        return( 0 );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> ssl_buffer_message" ) );

    switch( ssl->in_msgtype )
    {
        case MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "Remember CCS message" ) );

            hs->buffering.seen_ccs = 1;
            break;

        case MBEDTLS_SSL_MSG_HANDSHAKE:
        {
            unsigned recv_msg_seq_offset;
            unsigned recv_msg_seq = (unsigned)
                mbedtls_platform_get_uint16_be( &ssl->in_msg[4] );

            mbedtls_ssl_hs_buffer *hs_buf;
            size_t msg_len = ssl->in_hslen - 12;

            /* We should never receive an old handshake
             * message - double-check nonetheless. */
            if( recv_msg_seq < ssl->handshake->in_msg_seq )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
                return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }

            recv_msg_seq_offset = recv_msg_seq - ssl->handshake->in_msg_seq;
            if( recv_msg_seq_offset >= MBEDTLS_SSL_MAX_BUFFERED_HS )
            {
                /* Silently ignore -- message too far in the future */
                MBEDTLS_SSL_DEBUG_MSG( 2,
                 ( "Ignore future HS message with sequence number %u, "
                   "buffering window %u - %u",
                   recv_msg_seq, ssl->handshake->in_msg_seq,
                   ssl->handshake->in_msg_seq + MBEDTLS_SSL_MAX_BUFFERED_HS - 1 ) );

                goto exit;
            }

            MBEDTLS_SSL_DEBUG_MSG( 2, ( "Buffering HS message with sequence number %u, offset %u ",
                                        recv_msg_seq, recv_msg_seq_offset ) );

            hs_buf = &hs->buffering.hs[ recv_msg_seq_offset ];

            /* Check if the buffering for this seq nr has already commenced. */
            if( !hs_buf->is_valid )
            {
                size_t reassembly_buf_sz;

                hs_buf->is_fragmented =
                    ( ssl_hs_is_proper_fragment( ssl ) == PROPER_HS_FRAGMENT );

                /* We copy the message back into the input buffer
                 * after reassembly, so check that it's not too large.
                 * This is an implementation-specific limitation
                 * and not one from the standard, hence it is not
                 * checked in ssl_check_hs_header(). */
                if( msg_len + 12 > MBEDTLS_SSL_IN_CONTENT_LEN )
                {
                    /* Ignore message */
                    goto exit;
                }

                /* Check if we have enough space to buffer the message. */
                if( hs->buffering.total_bytes_buffered >
                    MBEDTLS_SSL_DTLS_MAX_BUFFERING )
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
                    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
                }

                reassembly_buf_sz = ssl_get_reassembly_buffer_size( msg_len,
                                                       hs_buf->is_fragmented );

                if( reassembly_buf_sz > ( MBEDTLS_SSL_DTLS_MAX_BUFFERING -
                                          hs->buffering.total_bytes_buffered ) )
                {
                    if( recv_msg_seq_offset > 0 )
                    {
                        /* If we can't buffer a future message because
                         * of space limitations -- ignore. */
                        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Buffering of future message of size %u would exceed the compile-time limit %u (already %u bytes buffered) -- ignore\n",
                             (unsigned) msg_len, MBEDTLS_SSL_DTLS_MAX_BUFFERING,
                             (unsigned) hs->buffering.total_bytes_buffered ) );
                        goto exit;
                    }
                    else
                    {
                        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Buffering of future message of size %u would exceed the compile-time limit %u (already %u bytes buffered) -- attempt to make space by freeing buffered future messages\n",
                             (unsigned) msg_len, MBEDTLS_SSL_DTLS_MAX_BUFFERING,
                             (unsigned) hs->buffering.total_bytes_buffered ) );
                    }

                    if( ssl_buffer_make_space( ssl, reassembly_buf_sz ) != 0 )
                    {
                        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Reassembly of next message of size %u (%u with bitmap) would exceed the compile-time limit %u (already %u bytes buffered) -- fail\n",
                             (unsigned) msg_len,
                             (unsigned) reassembly_buf_sz,
                             MBEDTLS_SSL_DTLS_MAX_BUFFERING,
                             (unsigned) hs->buffering.total_bytes_buffered ) );
                        ret = MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
                        goto exit;
                    }
                }

                MBEDTLS_SSL_DEBUG_MSG( 2, ( "initialize reassembly, total length = %d",
                                            msg_len ) );

                hs_buf->data = mbedtls_calloc( 1, reassembly_buf_sz );
                if( hs_buf->data == NULL )
                {
                    ret = MBEDTLS_ERR_SSL_ALLOC_FAILED;
                    goto exit;
                }
                hs_buf->data_len = reassembly_buf_sz;

                /* Prepare final header: copy msg_type, length and message_seq,
                 * then add standardised fragment_offset and fragment_length */
                if( mbedtls_platform_memcpy( hs_buf->data, ssl->in_msg, 6 ) !=
                                             hs_buf->data )
                {
                    ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
                    goto exit;
                }
                if( mbedtls_platform_memset( hs_buf->data + 6, 0, 3 ) !=
                                             hs_buf->data + 6 )
                {
                    ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
                    goto exit;
                }
                if( mbedtls_platform_memcpy( hs_buf->data + 9, hs_buf->data + 1, 3 ) !=
                                             hs_buf->data + 9 )
                {
                    ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
                    goto exit;
                }

                hs_buf->is_valid = 1;

                hs->buffering.total_bytes_buffered += reassembly_buf_sz;
            }
            else
            {
                /* Make sure msg_type and length are consistent */
                if( memcmp( hs_buf->data, ssl->in_msg, 4 ) != 0 ) // use regular memcmp as msg type is public
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Fragment header mismatch - ignore" ) );
                    /* Ignore */
                    goto exit;
                }
            }

            if( !hs_buf->is_complete )
            {
                size_t frag_len, frag_off;
                unsigned char * const msg = hs_buf->data + 12;

                /*
                 * Check and copy current fragment
                 */

                /* Validation of header fields already done in
                 * mbedtls_ssl_prepare_handshake_record(). */
                frag_off = ssl_get_hs_frag_off( ssl );
                frag_len = ssl_get_hs_frag_len( ssl );

                MBEDTLS_SSL_DEBUG_MSG( 2, ( "adding fragment, offset = %d, length = %d",
                                            frag_off, frag_len ) );
                mbedtls_platform_memcpy( msg + frag_off, ssl->in_msg + 12, frag_len );

                if( hs_buf->is_fragmented )
                {
                    unsigned char * const bitmask = msg + msg_len;
                    ssl_bitmask_set( bitmask, frag_off, frag_len );
                    hs_buf->is_complete = ( ssl_bitmask_check( bitmask,
                                                               msg_len ) == 0 );
                }
                else
                {
                    hs_buf->is_complete = 1;
                }

                MBEDTLS_SSL_DEBUG_MSG( 2, ( "message %scomplete",
                                   hs_buf->is_complete ? "" : "not yet " ) );
            }

            break;
        }

        default:
            /* We don't buffer other types of messages. */
            break;
    }

exit:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= ssl_buffer_message" ) );
    return( ret );
}
#endif /* MBEDTLS_SSL_PROTO_DTLS */

static int ssl_consume_current_message( mbedtls_ssl_context *ssl )
{
    /*
     * Consume last content-layer message and potentially
     * update in_msglen which keeps track of the contents'
     * consumption state.
     *
     * (1) Handshake messages:
     *     Remove last handshake message, move content
     *     and adapt in_msglen.
     *
     * (2) Alert messages:
     *     Consume whole record content, in_msglen = 0.
     *
     * (3) Change cipher spec:
     *     Consume whole record content, in_msglen = 0.
     *
     * (4) Application data:
     *     Don't do anything - the record layer provides
     *     the application data as a stream transport
     *     and consumes through mbedtls_ssl_read only.
     *
     */

    /* Case (1): Handshake messages */
    if( ssl->in_hslen != 0 )
    {
        /* Hard assertion to be sure that no application data
         * is in flight, as corrupting ssl->in_msglen during
         * ssl->in_offt != NULL is fatal. */
        if( ssl->in_offt != NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        /*
         * Get next Handshake message in the current record
         */

        /* Notes:
         * (1) in_hslen is not necessarily the size of the
         *     current handshake content: If DTLS handshake
         *     fragmentation is used, that's the fragment
         *     size instead. Using the total handshake message
         *     size here is faulty and should be changed at
         *     some point.
         * (2) While it doesn't seem to cause problems, one
         *     has to be very careful not to assume that in_hslen
         *     is always <= in_msglen in a sensible communication.
         *     Again, it's wrong for DTLS handshake fragmentation.
         *     The following check is therefore mandatory, and
         *     should not be treated as a silently corrected assertion.
         *     Additionally, ssl->in_hslen might be arbitrarily out of
         *     bounds after handling a DTLS message with an unexpected
         *     sequence number, see mbedtls_ssl_prepare_handshake_record.
         */
        if( ssl->in_hslen < ssl->in_msglen )
        {
            ssl->in_msglen -= ssl->in_hslen;
            memmove( ssl->in_msg, ssl->in_msg + ssl->in_hslen,
                     ssl->in_msglen );

            MBEDTLS_SSL_DEBUG_BUF( 4, "remaining content in record",
                                   ssl->in_msg, ssl->in_msglen );
        }
        else
        {
            ssl->in_msglen = 0;
        }

        ssl->in_hslen   = 0;
    }
    /* Case (4): Application data */
    else if( ssl->in_offt != NULL )
    {
        return( 0 );
    }
    /* Everything else (CCS & Alerts) */
    else
    {
        ssl->in_msglen = 0;
    }

    return( 0 );
}

static int ssl_record_is_in_progress( mbedtls_ssl_context *ssl )
{
    if( ssl->in_msglen > 0 )
        return( 1 );

    return( 0 );
}

#if defined(MBEDTLS_SSL_PROTO_DTLS)

static void ssl_free_buffered_record( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;
    if( hs == NULL )
        return;

    if( hs->buffering.future_record.data != NULL )
    {
        hs->buffering.total_bytes_buffered -=
            hs->buffering.future_record.len;

        mbedtls_free( hs->buffering.future_record.data );
        hs->buffering.future_record.data = NULL;
    }
}

static int ssl_load_buffered_record( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;
    unsigned char * rec;
    size_t rec_len;
    unsigned rec_epoch;
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    size_t in_buf_len = ssl->in_buf_len;
#else
    size_t in_buf_len = MBEDTLS_SSL_IN_BUFFER_LEN;
#endif

    if( MBEDTLS_SSL_TRANSPORT_IS_TLS( ssl->conf->transport ) )
        return( 0 );

    if( hs == NULL )
        return( 0 );

    rec       = hs->buffering.future_record.data;
    rec_len   = hs->buffering.future_record.len;
    rec_epoch = hs->buffering.future_record.epoch;

    if( rec == NULL )
        return( 0 );

    /* Only consider loading future records if the
     * input buffer is empty. */
    if( ssl_next_record_is_in_datagram( ssl ) == 1 )
        return( 0 );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> ssl_load_buffered_record" ) );

    if( rec_epoch != ssl->in_epoch )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Buffered record not from current epoch." ) );
        goto exit;
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "Found buffered record from current epoch - load" ) );

    /* Double-check that the record is not too large */
    if( rec_len > in_buf_len - (size_t)( ssl->in_hdr - ssl->in_buf ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    mbedtls_platform_memcpy( ssl->in_hdr, rec, rec_len );
    ssl->in_left = rec_len;
    ssl->next_record_offset = 0;

    ssl_free_buffered_record( ssl );

exit:
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= ssl_load_buffered_record" ) );
    return( 0 );
}

static int ssl_buffer_future_record( mbedtls_ssl_context *ssl,
                                     mbedtls_record const *rec )
{
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;

    /* Don't buffer future records outside handshakes. */
    if( hs == NULL )
        return( 0 );

    /* Only buffer handshake records (we are only interested
     * in Finished messages). */
    if( rec->type != MBEDTLS_SSL_MSG_HANDSHAKE )
        return( 0 );

    /* Don't buffer more than one future epoch record. */
    if( hs->buffering.future_record.data != NULL )
        return( 0 );

    /* Don't buffer record if there's not enough buffering space remaining. */
    if( rec->buf_len > ( MBEDTLS_SSL_DTLS_MAX_BUFFERING -
                         hs->buffering.total_bytes_buffered ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Buffering of future epoch record of size %u would exceed the compile-time limit %u (already %u bytes buffered) -- ignore\n",
                        (unsigned) rec->buf_len, MBEDTLS_SSL_DTLS_MAX_BUFFERING,
                        (unsigned) hs->buffering.total_bytes_buffered ) );
        return( 0 );
    }

    /* Buffer record */
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "Buffer record from epoch %u",
                                ssl->in_epoch + 1 ) );
    MBEDTLS_SSL_DEBUG_BUF( 3, "Buffered record", rec->buf, rec->buf_len );

    /* ssl_parse_record_header() only considers records
     * of the next epoch as candidates for buffering. */
    hs->buffering.future_record.epoch = ssl->in_epoch + 1;
    hs->buffering.future_record.len   = rec->buf_len;

    hs->buffering.future_record.data =
        mbedtls_calloc( 1, hs->buffering.future_record.len );
    if( hs->buffering.future_record.data == NULL )
    {
        /* If we run out of RAM trying to buffer a
         * record from the next epoch, just ignore. */
        return( 0 );
    }

    mbedtls_platform_memcpy( hs->buffering.future_record.data, rec->buf, rec->buf_len );

    hs->buffering.total_bytes_buffered += rec->buf_len;
    return( 0 );
}

#endif /* MBEDTLS_SSL_PROTO_DTLS */

static int ssl_get_next_record( mbedtls_ssl_context *ssl )
{
    int ret;
    mbedtls_record rec;

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    /* We might have buffered a future record; if so,
     * and if the epoch matches now, load it.
     * On success, this call will set ssl->in_left to
     * the length of the buffered record, so that
     * the calls to ssl_fetch_input() below will
     * essentially be no-ops. */
    ret = ssl_load_buffered_record( ssl );
    if( ret != 0 )
        return( ret );
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    /* Ensure that we have enough space available for the default form
     * of TLS / DTLS record headers (5 Bytes for TLS, 13 Bytes for DTLS,
     * with no space for CIDs counted in). */
    ret = mbedtls_ssl_fetch_input( ssl, mbedtls_ssl_in_hdr_len( ssl ) );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_fetch_input", ret );
        return( ret );
    }

    ret = ssl_parse_record_header( ssl, ssl->in_hdr, ssl->in_left, &rec );
    if( ret != 0 )
    {
#if defined(MBEDTLS_SSL_PROTO_DTLS)
        if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        {
            if( ret == MBEDTLS_ERR_SSL_EARLY_MESSAGE )
            {
                ret = ssl_buffer_future_record( ssl, &rec );
                if( ret != 0 )
                    return( ret );

                /* Fall through to handling of unexpected records */
                ret = MBEDTLS_ERR_SSL_UNEXPECTED_RECORD;
            }

            if( ret == MBEDTLS_ERR_SSL_UNEXPECTED_RECORD )
            {
#if defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE) && defined(MBEDTLS_SSL_SRV_C)
                /* Reset in pointers to default state for TLS/DTLS records,
                 * assuming no CID and no offset between record content and
                 * record plaintext. */
                ssl_update_in_pointers( ssl );

                /* Setup internal message pointers from record structure. */
                ssl->in_msgtype = rec.type;
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
                ssl->in_len = ssl->in_cid + rec.cid_len;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
                ssl->in_msg = ssl->in_len + 2;
                ssl->in_msglen = rec.data_len;

                ret = ssl_check_client_reconnect( ssl );
                MBEDTLS_SSL_DEBUG_RET( 2, "ssl_check_client_reconnect", ret );
                if( ret != 0 )
                    return( ret );
#endif

                /* Skip unexpected record (but not whole datagram) */
                ssl->next_record_offset = rec.buf_len;

                MBEDTLS_SSL_DEBUG_MSG( 1, ( "discarding unexpected record "
                                            "(header)" ) );
            }
            else
            {
                /* Skip invalid record and the rest of the datagram */
                ssl->next_record_offset = 0;
                ssl->in_left = 0;

                MBEDTLS_SSL_DEBUG_MSG( 1, ( "discarding invalid record "
                                            "(header)" ) );
            }

            /* Get next record */
            return( MBEDTLS_ERR_SSL_CONTINUE_PROCESSING );
        }
        else
#endif
        {
            return( ret );
        }
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        /* Remember offset of next record within datagram. */
        ssl->next_record_offset = rec.buf_len;
        if( ssl->next_record_offset < ssl->in_left )
        {
            MBEDTLS_SSL_DEBUG_MSG( 3, ( "more than one record within datagram" ) );
        }
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        /*
         * Fetch record contents from underlying transport.
         */
        ret = mbedtls_ssl_fetch_input( ssl, rec.buf_len );
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_fetch_input", ret );
            return( ret );
        }

        ssl->in_left = 0;
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */

    /*
     * Decrypt record contents.
     */

    if( ( ret = ssl_prepare_record_content( ssl, &rec ) ) != 0 )
    {
#if defined(MBEDTLS_SSL_PROTO_DTLS)
        if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        {
            /* Silently discard invalid records */
            if( ret == MBEDTLS_ERR_SSL_INVALID_MAC )
            {
                /* Except when waiting for Finished as a bad mac here
                 * probably means something went wrong in the handshake
                 * (eg wrong psk used, mitm downgrade attempt, etc.) */
                if( ssl->state == MBEDTLS_SSL_CLIENT_FINISHED ||
                    ssl->state == MBEDTLS_SSL_SERVER_FINISHED )
                {
#if defined(MBEDTLS_SSL_ALL_ALERT_MESSAGES)
                    if( ret == MBEDTLS_ERR_SSL_INVALID_MAC )
                    {
                        mbedtls_ssl_pend_fatal_alert( ssl,
                                MBEDTLS_SSL_ALERT_MSG_BAD_RECORD_MAC );
                    }
#endif
                    return( ret );
                }

#if defined(MBEDTLS_SSL_DTLS_BADMAC_LIMIT)
                if( mbedtls_ssl_conf_get_badmac_limit( ssl->conf ) != 0 &&
                    ++ssl->badmac_seen >= mbedtls_ssl_conf_get_badmac_limit( ssl->conf ) )
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "too many records with bad MAC" ) );
                    return( MBEDTLS_ERR_SSL_INVALID_MAC );
                }
#endif

                /* As above, invalid records cause
                 * dismissal of the whole datagram. */

                ssl->next_record_offset = 0;
                ssl->in_left = 0;

                MBEDTLS_SSL_DEBUG_MSG( 1, ( "discarding invalid record (mac)" ) );
                return( MBEDTLS_ERR_SSL_CONTINUE_PROCESSING );
            }

            return( ret );
        }
        MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
        {
            /* Error out (and send alert) on invalid records */
#if defined(MBEDTLS_SSL_ALL_ALERT_MESSAGES)
            if( ret == MBEDTLS_ERR_SSL_INVALID_MAC )
            {
                mbedtls_ssl_pend_fatal_alert( ssl,
                        MBEDTLS_SSL_ALERT_MSG_BAD_RECORD_MAC );
            }
#endif
            return( ret );
        }
#endif /* MBEDTLS_SSL_PROTO_TLS */
    }


    /* Reset in pointers to default state for TLS/DTLS records,
     * assuming no CID and no offset between record content and
     * record plaintext. */
    ssl_update_in_pointers( ssl );
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    ssl->in_len = ssl->in_cid + rec.cid_len;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
    ssl->in_msg = ssl->in_len + 2;

    /* The record content type may change during decryption,
     * so re-read it. */
    ssl->in_msgtype = rec.type;
    /* Also update the input buffer, because unfortunately
     * the server-side ssl_parse_client_hello() reparses the
     * record header when receiving a ClientHello initiating
     * a renegotiation. */
    ssl->in_hdr[0] = rec.type;
    ssl->in_msg    = rec.buf + rec.data_offset;
    ssl->in_msglen = rec.data_len;
    (void)mbedtls_platform_put_uint16_be( ssl->in_len, rec.data_len );

#if defined(MBEDTLS_ZLIB_SUPPORT)
    if( ssl->transform_in != NULL &&
        ssl->session_in->compression == MBEDTLS_SSL_COMPRESS_DEFLATE )
    {
        if( ( ret = ssl_decompress_buf( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_decompress_buf", ret );
            return( ret );
        }

        /* Check actual (decompress) record content length against
         * configured maximum. */
        if( ssl->in_msglen > MBEDTLS_SSL_IN_CONTENT_LEN )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad message length" ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }
    }
#endif /* MBEDTLS_ZLIB_SUPPORT */

    return( 0 );
}

int mbedtls_ssl_handle_message_type( mbedtls_ssl_context *ssl )
{
    int ret;

    /*
     * Handle particular types of records
     */
    if( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE )
    {
        if( ( ret = mbedtls_ssl_prepare_handshake_record( ssl ) ) != 0 )
        {
            return( ret );
        }
    }

    if( ssl->in_msgtype == MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC )
    {
        if( ssl->in_msglen != 1 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid CCS message, len: %d",
                           ssl->in_msglen ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }

        if( ssl->in_msg[0] != 1 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid CCS message, content: %02x",
                                        ssl->in_msg[0] ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
        if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
            ssl->state != MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC    &&
            ssl->state != MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC )
        {
            if( ssl->handshake == NULL )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "dropping ChangeCipherSpec outside handshake" ) );
                return( MBEDTLS_ERR_SSL_UNEXPECTED_RECORD );
            }

            MBEDTLS_SSL_DEBUG_MSG( 1, ( "received out-of-order ChangeCipherSpec - remember" ) );
            return( MBEDTLS_ERR_SSL_EARLY_MESSAGE );
        }
#endif
    }

    if( ssl->in_msgtype == MBEDTLS_SSL_MSG_ALERT )
    {
        if( ssl->in_msglen != 2 )
        {
            /* Note: Standard allows for more than one 2 byte alert
               to be packed in a single message, but Mbed TLS doesn't
               currently support this. */
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid alert message, len: %d",
                           ssl->in_msglen ) );
            return( MBEDTLS_ERR_SSL_INVALID_RECORD );
        }

        MBEDTLS_SSL_DEBUG_MSG( 2, ( "got an alert message, type: [%d:%d]",
                       ssl->in_msg[0], ssl->in_msg[1] ) );

        /*
         * Ignore non-fatal alerts, except close_notify and no_renegotiation
         */
        if( ssl->in_msg[0] == MBEDTLS_SSL_ALERT_LEVEL_FATAL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "is a fatal alert message (msg %d)",
                           ssl->in_msg[1] ) );
            return( MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE );
        }

        if( ssl->in_msg[0] == MBEDTLS_SSL_ALERT_LEVEL_WARNING &&
            ssl->in_msg[1] == MBEDTLS_SSL_ALERT_MSG_CLOSE_NOTIFY )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "is a close notify message" ) );
            return( MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY );
        }

#if defined(MBEDTLS_SSL_RENEGOTIATION_ENABLED)
        if( ssl->in_msg[0] == MBEDTLS_SSL_ALERT_LEVEL_WARNING &&
            ssl->in_msg[1] == MBEDTLS_SSL_ALERT_MSG_NO_RENEGOTIATION )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "is a SSLv3 no renegotiation alert" ) );
            /* Will be handled when trying to parse ServerHello */
            return( 0 );
        }
#endif

#if defined(MBEDTLS_SSL_PROTO_SSL3) && defined(MBEDTLS_SSL_SRV_C)
        if( mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_0 &&
            mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
              MBEDTLS_SSL_IS_SERVER &&
            ssl->in_msg[0] == MBEDTLS_SSL_ALERT_LEVEL_WARNING &&
            ssl->in_msg[1] == MBEDTLS_SSL_ALERT_MSG_NO_CERT )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "is a SSLv3 no_cert" ) );
            /* Will be handled in mbedtls_ssl_parse_certificate() */
            return( 0 );
        }
#endif /* MBEDTLS_SSL_PROTO_SSL3 && MBEDTLS_SSL_SRV_C */

        /* Silently ignore: fetch new message */
        return MBEDTLS_ERR_SSL_NON_FATAL;
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        /* Drop unexpected ApplicationData records,
         * except at the beginning of renegotiations */
        if( ssl->in_msgtype == MBEDTLS_SSL_MSG_APPLICATION_DATA &&
            ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER
#if defined(MBEDTLS_SSL_RENEGOTIATION)
            && ! ( ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_IN_PROGRESS &&
                   ssl->state == MBEDTLS_SSL_SERVER_HELLO )
#endif
            )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "dropping unexpected ApplicationData" ) );
            return( MBEDTLS_ERR_SSL_NON_FATAL );
        }

        if( ssl->handshake != NULL &&
            ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER  )
        {
            ssl_handshake_wrapup_free_hs_transform( ssl );
        }
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    return( 0 );
}

int mbedtls_ssl_send_fatal_handshake_failure( mbedtls_ssl_context *ssl )
{
    int ret;

    if( ( ret = mbedtls_ssl_send_alert_message( ssl,
                    MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                    MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE ) ) != 0 )
    {
        return( ret );
    }

    return( 0 );
}

int mbedtls_ssl_send_alert_message( mbedtls_ssl_context *ssl,
                                    unsigned char level,
                                    unsigned char message )
{
    int ret;

    if( ssl == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> send alert message" ) );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "send alert level=%u message=%u", level, message ));

    ssl->out_msgtype = MBEDTLS_SSL_MSG_ALERT;
    ssl->out_msglen = 2;
    ssl->out_msg[0] = level;
    ssl->out_msg[1] = message;

    if( ( ret = mbedtls_ssl_write_record( ssl, SSL_FORCE_FLUSH ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_write_record", ret );
        return( ret );
    }
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= send alert message" ) );

    return( 0 );
}

#if defined(MBEDTLS_X509_CRT_PARSE_C)
static void ssl_clear_peer_cert( mbedtls_ssl_session *session )
{
#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    if( session->peer_cert != NULL )
    {
        mbedtls_x509_crt_free( session->peer_cert );
        mbedtls_free( session->peer_cert );
        session->peer_cert = NULL;
    }
#elif defined(MBEDTLS_SSL_RENEGOTIATION)
    if( session->peer_cert_digest != NULL )
    {
        /* Zeroization is not necessary. */
        mbedtls_free( session->peer_cert_digest );
        session->peer_cert_digest      = NULL;
        session->peer_cert_digest_type = MBEDTLS_MD_NONE;
        session->peer_cert_digest_len  = 0;
    }
#else
    ((void) session);
#endif /* !MBEDTLS_SSL_KEEP_PEER_CERTIFICATE && MBEDTLS_SSL_RENEGOTIATION */
}
#endif /* MBEDTLS_X509_CRT_PARSE_C */

/*
 * Handshake functions
 */
#if !defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
/* No certificate support -> dummy functions */
int mbedtls_ssl_write_certificate( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_ciphersuite_handle_t ciphersuite_info =
        mbedtls_ssl_handshake_get_ciphersuite( ssl->handshake );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write certificate" ) );

    if( !mbedtls_ssl_ciphersuite_uses_srv_cert( ciphersuite_info ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip write certificate" ) );
        if( ssl->state == MBEDTLS_SSL_CLIENT_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_CLIENT_KEY_EXCHANGE;
        }
        else if( ssl->state == MBEDTLS_SSL_SERVER_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_SERVER_KEY_EXCHANGE;
        }
        else
        {
            ssl->state = MBEDTLS_SSL_INVALID;
        }
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
}

int mbedtls_ssl_parse_certificate( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_ciphersuite_handle_t ciphersuite_info =
        mbedtls_ssl_handshake_get_ciphersuite( ssl->handshake );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse certificate" ) );

    if( !mbedtls_ssl_ciphersuite_uses_srv_cert( ciphersuite_info ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip parse certificate" ) );
        if( ssl->state == MBEDTLS_SSL_CLIENT_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_CLIENT_KEY_EXCHANGE;
        }
        else if( ssl->state == MBEDTLS_SSL_SERVER_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_SERVER_KEY_EXCHANGE;
        }
        else
        {
            ssl->state = MBEDTLS_SSL_INVALID;
        }
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
}

#else /* MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED */
/* Some certificate support -> implement write and parse */

int mbedtls_ssl_write_certificate( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;
    size_t i, n;
    const mbedtls_x509_crt *crt;
    mbedtls_ssl_ciphersuite_handle_t ciphersuite_info =
        mbedtls_ssl_handshake_get_ciphersuite( ssl->handshake );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write certificate" ) );

    if( !mbedtls_ssl_ciphersuite_uses_srv_cert( ciphersuite_info ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip write certificate" ) );
        if( ssl->state == MBEDTLS_SSL_CLIENT_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_CLIENT_KEY_EXCHANGE;
        }
        else if( ssl->state == MBEDTLS_SSL_SERVER_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_SERVER_KEY_EXCHANGE;
        }
        else
        {
            ssl->state = MBEDTLS_SSL_INVALID;
        }
        return( 0 );
    }

#if defined(MBEDTLS_SSL_CLI_C)
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
          MBEDTLS_SSL_IS_CLIENT )
    {
        if( ssl->client_auth == 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip write certificate" ) );
            if( ssl->state == MBEDTLS_SSL_CLIENT_CERTIFICATE )
            {
                ssl->state = MBEDTLS_SSL_CLIENT_KEY_EXCHANGE;
            }
            else if( ssl->state == MBEDTLS_SSL_SERVER_CERTIFICATE )
            {
                ssl->state = MBEDTLS_SSL_SERVER_KEY_EXCHANGE;
            }
            else
            {
                ssl->state = MBEDTLS_SSL_INVALID;
            }
            return( 0 );
        }

#if defined(MBEDTLS_SSL_PROTO_SSL3)
        /*
         * If using SSLv3 and got no cert, send an Alert message
         * (otherwise an empty Certificate message will be sent).
         */
        if( mbedtls_ssl_own_cert( ssl )  == NULL &&
            mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_0 )
        {
            ssl->out_msglen  = 2;
            ssl->out_msgtype = MBEDTLS_SSL_MSG_ALERT;
            ssl->out_msg[0]  = MBEDTLS_SSL_ALERT_LEVEL_WARNING;
            ssl->out_msg[1]  = MBEDTLS_SSL_ALERT_MSG_NO_CERT;

            MBEDTLS_SSL_DEBUG_MSG( 2, ( "got no certificate to send" ) );
            goto write_msg;
        }
#endif /* MBEDTLS_SSL_PROTO_SSL3 */
    }
#endif /* MBEDTLS_SSL_CLI_C */
#if defined(MBEDTLS_SSL_SRV_C)
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_SERVER )
    {
        if( mbedtls_ssl_own_cert( ssl ) == NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "got no certificate to send" ) );
            return( MBEDTLS_ERR_SSL_CERTIFICATE_REQUIRED );
        }
    }
#endif

    MBEDTLS_SSL_DEBUG_CRT( 3, "own certificate", mbedtls_ssl_own_cert( ssl ) );

    /*
     *     0  .  0    handshake type
     *     1  .  3    handshake length
     *     4  .  6    length of all certs
     *     7  .  9    length of cert. 1
     *    10  . n-1   peer certificate
     *     n  . n+2   length of cert. 2
     *    n+3 . ...   upper level cert, etc.
     */
    i = 7;
    crt = mbedtls_ssl_own_cert( ssl );

    while( crt != NULL )
    {
        n = crt->raw.len;
        if( n > MBEDTLS_SSL_OUT_CONTENT_LEN - 3 - i )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "certificate too large, %d > %d",
                           i + 3 + n, MBEDTLS_SSL_OUT_CONTENT_LEN ) );
            return( MBEDTLS_ERR_SSL_CERTIFICATE_TOO_LARGE );
        }

        (void)mbedtls_platform_put_uint24_be( &ssl->out_msg[i], n );

        i += 3; mbedtls_platform_memcpy( ssl->out_msg + i, crt->raw.p, n );
        i += n; crt = crt->next;
    }

    (void)mbedtls_platform_put_uint24_be( &ssl->out_msg[4], ( i - 7 ) );

    ssl->out_msglen  = i;
    ssl->out_msgtype = MBEDTLS_SSL_MSG_HANDSHAKE;
    ssl->out_msg[0]  = MBEDTLS_SSL_HS_CERTIFICATE;

#if defined(MBEDTLS_SSL_PROTO_SSL3) && defined(MBEDTLS_SSL_CLI_C)
write_msg:
#endif

    if( ssl->state == MBEDTLS_SSL_CLIENT_CERTIFICATE )
    {
        ssl->state = MBEDTLS_SSL_CLIENT_KEY_EXCHANGE;
    }
    else if( ssl->state == MBEDTLS_SSL_SERVER_CERTIFICATE )
    {
        ssl->state = MBEDTLS_SSL_SERVER_KEY_EXCHANGE;
    }
    else
    {
        ssl->state = MBEDTLS_SSL_INVALID;
    }

    if( ( ret = mbedtls_ssl_write_handshake_msg( ssl ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_write_handshake_msg", ret );
        return( ret );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write certificate" ) );

    return( ret );
}

#if defined(MBEDTLS_SSL_RENEGOTIATION) && defined(MBEDTLS_SSL_CLI_C)

#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
#define PEER_CRT_CHANGED 0x75555555
static int ssl_check_peer_crt_unchanged( mbedtls_ssl_context *ssl,
                                         unsigned char *crt_buf,
                                         size_t crt_buf_len )
{
    mbedtls_x509_crt const * const peer_crt = ssl->session->peer_cert;

    if( peer_crt == NULL )
        return( PEER_CRT_CHANGED );

    if( peer_crt->raw.len != crt_buf_len )
        return( PEER_CRT_CHANGED );

    return( mbedtls_platform_memequal( peer_crt->raw.p, crt_buf, crt_buf_len ) );
}
#elif defined(MBEDTLS_SSL_RENEGOTIATION)
#define PEER_CRT_CHANGED 0x75555555
static int ssl_check_peer_crt_unchanged( mbedtls_ssl_context *ssl,
                                         unsigned char *crt_buf,
                                         size_t crt_buf_len )
{
    int ret;
    unsigned char const * const peer_cert_digest =
        ssl->session->peer_cert_digest;
    mbedtls_md_type_t const peer_cert_digest_type =
        ssl->session->peer_cert_digest_type;
    mbedtls_md_handle_t digest_info =
        mbedtls_md_info_from_type( peer_cert_digest_type );
    unsigned char tmp_digest[MBEDTLS_SSL_PEER_CERT_DIGEST_MAX_LEN];
    size_t digest_len;

    if( peer_cert_digest == NULL ||
        digest_info == MBEDTLS_MD_INVALID_HANDLE )
    {
        return( PEER_CRT_CHANGED );
    }

    digest_len = mbedtls_md_get_size( digest_info );
    if( digest_len > MBEDTLS_SSL_PEER_CERT_DIGEST_MAX_LEN )
        return( PEER_CRT_CHANGED );

    ret = mbedtls_md( digest_info, crt_buf, crt_buf_len, tmp_digest );
    if( ret != 0 )
        return( PEER_CRT_CHANGED );

    return( mbedtls_platform_memequal( tmp_digest, peer_cert_digest, digest_len ) );
}
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE && MBEDTLS_SSL_RENEGOTIATION */
#endif /* MBEDTLS_SSL_RENEGOTIATION && MBEDTLS_SSL_CLI_C */

/*
 * Once the certificate message is read, parse it into a cert chain and
 * perform basic checks, but leave actual verification to the caller
 */
static int ssl_parse_certificate_chain( mbedtls_ssl_context *ssl,
                                        mbedtls_x509_crt *chain )
{
    volatile int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
#if defined(MBEDTLS_SSL_RENEGOTIATION) && defined(MBEDTLS_SSL_CLI_C)
    int crt_cnt=0;
#endif
    size_t i, n;
    uint8_t alert;

    if( ssl->in_msgtype != MBEDTLS_SSL_MSG_HANDSHAKE )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate message" ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                            MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE );
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
    }

    if( ssl->in_msg[0] != MBEDTLS_SSL_HS_CERTIFICATE ||
        ssl->in_hslen < mbedtls_ssl_hs_hdr_len( ssl ) + 3 + 3 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate message" ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE );
    }

    i = mbedtls_ssl_hs_hdr_len( ssl );

    /*
     * Same message structure as in mbedtls_ssl_write_certificate()
     */
    n = mbedtls_platform_get_uint16_be( &ssl->in_msg[i + 1] );

    if( ssl->in_msg[i] != 0 ||
        ssl->in_hslen != n + 3 + mbedtls_ssl_hs_hdr_len( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate message" ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE );
    }

    /* Make &ssl->in_msg[i] point to the beginning of the CRT chain. */
    i += 3;

    /* Iterate through and parse the CRTs in the provided chain. */
    while( i < ssl->in_hslen )
    {
        /* Check that there's room for the next CRT's length fields. */
        if ( i + 3 > ssl->in_hslen ) {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate message" ) );
            mbedtls_ssl_pend_fatal_alert( ssl,
                                          MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR );
            return( MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE );
        }
        /* In theory, the CRT can be up to 2**24 Bytes, but we don't support
         * anything beyond 2**16 ~ 64K. */
        if( ssl->in_msg[i] != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate message" ) );
            mbedtls_ssl_pend_fatal_alert( ssl,
                                          MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR );
            return( MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE );
        }

        /* Read length of the next CRT in the chain. */
        n = mbedtls_platform_get_uint16_be( &ssl->in_msg[i + 1] );
        i += 3;

        if( n < 128 || i + n > ssl->in_hslen )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate message" ) );
            mbedtls_ssl_pend_fatal_alert( ssl,
                                          MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR );
            return( MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE );
        }

        /* Check if we're handling the first CRT in the chain. */
#if defined(MBEDTLS_SSL_RENEGOTIATION) && defined(MBEDTLS_SSL_CLI_C)
        if( crt_cnt++ == 0 &&
            mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
              MBEDTLS_SSL_IS_CLIENT &&
            ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_IN_PROGRESS )
        {
            /* During client-side renegotiation, check that the server's
             * end-CRTs hasn't changed compared to the initial handshake,
             * mitigating the triple handshake attack. On success, reuse
             * the original end-CRT instead of parsing it again. */
            MBEDTLS_SSL_DEBUG_MSG( 3, ( "Check that peer CRT hasn't changed during renegotiation" ) );
            if( ssl_check_peer_crt_unchanged( ssl,
                                              &ssl->in_msg[i],
                                              n ) != 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "new server cert during renegotiation" ) );
                mbedtls_ssl_pend_fatal_alert( ssl,
                                       MBEDTLS_SSL_ALERT_MSG_ACCESS_DENIED );
                return( MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE );
            }

            /* Now we can safely free the original chain. */
            ssl_clear_peer_cert( ssl->session );
        }
#endif /* MBEDTLS_SSL_RENEGOTIATION && MBEDTLS_SSL_CLI_C */

        /* Parse the next certificate in the chain. */
#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
        ret = mbedtls_x509_crt_parse_der( chain, ssl->in_msg + i, n );
#else
        /* If we don't need to store the CRT chain permanently, parse
         * it in-place from the input buffer instead of making a copy. */
        ret = mbedtls_x509_crt_parse_der_nocopy( chain, ssl->in_msg + i, n );
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */
        switch( ret )
        {
            case 0: /* ok */
                mbedtls_platform_random_delay();
                if( ret != 0 )
                {
                    alert = MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR;
                    ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
                    goto crt_parse_der_failed;
                }
                break;
            case MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG + MBEDTLS_ERR_OID_NOT_FOUND:
                /* Ignore certificate with an unknown algorithm: maybe a
                 * prior certificate was already trusted. */
                mbedtls_platform_random_delay();
                if( ret != MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG + MBEDTLS_ERR_OID_NOT_FOUND )
                {
                    alert = MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR;
                    ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
                    goto crt_parse_der_failed;
                }
                break;

            case MBEDTLS_ERR_X509_ALLOC_FAILED:
                alert = MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR;
                goto crt_parse_der_failed;

            case MBEDTLS_ERR_X509_UNKNOWN_VERSION:
                alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT;
                goto crt_parse_der_failed;

            default:
                alert = MBEDTLS_SSL_ALERT_MSG_BAD_CERT;
            crt_parse_der_failed:
                mbedtls_ssl_pend_fatal_alert( ssl,  alert );
                MBEDTLS_SSL_DEBUG_RET( 1, " mbedtls_x509_crt_parse_der", ret );
                return( ret );
        }

        i += n;
    }

    MBEDTLS_SSL_DEBUG_CRT( 3, "peer certificate", chain );
    return( 0 );
}

#if defined(MBEDTLS_SSL_SRV_C)
static int ssl_srv_check_client_no_crt_notification( mbedtls_ssl_context *ssl )
{
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_CLIENT )
        return( -1 );

#if defined(MBEDTLS_SSL_PROTO_SSL3)
    /*
     * Check if the client sent an empty certificate
     */
    if( mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_0 )
    {
        if( ssl->in_msglen  == 2                        &&
            ssl->in_msgtype == MBEDTLS_SSL_MSG_ALERT            &&
            ssl->in_msg[0]  == MBEDTLS_SSL_ALERT_LEVEL_WARNING  &&
            ssl->in_msg[1]  == MBEDTLS_SSL_ALERT_MSG_NO_CERT )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "SSLv3 client has no certificate" ) );
            return( 0 );
        }

        return( -1 );
    }
#endif /* MBEDTLS_SSL_PROTO_SSL3 */

#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_2)
    if( ssl->in_hslen   == 3 + mbedtls_ssl_hs_hdr_len( ssl ) &&
        ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE    &&
        ssl->in_msg[0]  == MBEDTLS_SSL_HS_CERTIFICATE   &&
        memcmp( ssl->in_msg + mbedtls_ssl_hs_hdr_len( ssl ), "\0\0\0", 3 ) == 0 ) // use regular memcmp as comparing public data
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "TLSv1 client has no certificate" ) );
        return( 0 );
    }

#endif /* MBEDTLS_SSL_PROTO_TLS1 || MBEDTLS_SSL_PROTO_TLS1_1 || \
          MBEDTLS_SSL_PROTO_TLS1_2 */

    return( -1 );
}
#endif /* MBEDTLS_SSL_SRV_C */

/* Check if a certificate message is expected.
 * Return either
 * - SSL_CERTIFICATE_EXPECTED, or
 * - SSL_CERTIFICATE_SKIP
 * indicating whether a Certificate message is expected or not.
 */
#define SSL_CERTIFICATE_EXPECTED 0
#define SSL_CERTIFICATE_SKIP     0xff
static int ssl_parse_certificate_coordinate( mbedtls_ssl_context *ssl,
                                             int authmode )
{
    mbedtls_ssl_ciphersuite_handle_t ciphersuite_info =
        mbedtls_ssl_handshake_get_ciphersuite( ssl->handshake );

    if( !mbedtls_ssl_ciphersuite_uses_srv_cert( ciphersuite_info ) )
        return( SSL_CERTIFICATE_SKIP );

#if defined(MBEDTLS_SSL_SRV_C)
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_SERVER )
    {
        if( mbedtls_ssl_suite_get_key_exchange( ciphersuite_info ) ==
            MBEDTLS_KEY_EXCHANGE_RSA_PSK )
        {
            return( SSL_CERTIFICATE_SKIP );
        }

        if( authmode == MBEDTLS_SSL_VERIFY_NONE )
        {
            ssl->session_negotiate->verify_result =
                MBEDTLS_X509_BADCERT_SKIP_VERIFY;
            return( SSL_CERTIFICATE_SKIP );
        }
    }
#else
    ((void) authmode);
#endif /* MBEDTLS_SSL_SRV_C */

    return( SSL_CERTIFICATE_EXPECTED );
}

static int ssl_parse_certificate_verify( mbedtls_ssl_context *ssl,
                                         volatile int authmode,
                                         mbedtls_x509_crt *chain,
                                         void *rs_ctx )
{
    volatile int verify_ret = MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE;
    volatile int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
    volatile int flow_counter = 0;
    mbedtls_ssl_ciphersuite_handle_t ciphersuite_info =
        mbedtls_ssl_handshake_get_ciphersuite( ssl->handshake );
    mbedtls_x509_crt *ca_chain;
    mbedtls_x509_crl *ca_crl;

    if( authmode == MBEDTLS_SSL_VERIFY_NONE )
    {
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse certificate verify" ) );
#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
    if( ssl->handshake->sni_ca_chain != NULL )
    {
        ca_chain = ssl->handshake->sni_ca_chain;
        ca_crl   = ssl->handshake->sni_ca_crl;
    }
    else
#endif
    {
        ca_chain = ssl->conf->ca_chain;
        ca_crl   = ssl->conf->ca_crl;
    }

    /*
     * Main check: verify certificate
     */
    verify_ret = mbedtls_x509_crt_verify_restartable(
        chain,
        ca_chain, ca_crl,
        ssl->conf->cert_profile,
#if defined(MBEDTLS_X509_CRT_PARSE_C) && !defined(MBEDTLS_X509_REMOVE_HOSTNAME_VERIFICATION)
        ssl->hostname,
#endif /* MBEDTLS_X509_CRT_PARSE_C && !MBEDTLS_X509_REMOVE_HOSTNAME_VERIFICATION */
        &ssl->session_negotiate->verify_result,
#if !defined(MBEDTLS_X509_REMOVE_VERIFY_CALLBACK)
        ssl->conf->f_vrfy, ssl->conf->p_vrfy,
#endif /* MBEDTLS_X509_REMOVE_VERIFY_CALLBACK */
        rs_ctx );

    if( verify_ret == 0 )
    {
        mbedtls_platform_random_delay();
        if( verify_ret == 0 )
        {
            flow_counter++;
        }
        else
        {
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        }
    }
    if( verify_ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "x509_verify_cert", verify_ret );
        flow_counter++;
    }

#if defined(MBEDTLS_SSL__ECP_RESTARTABLE)
    if( verify_ret == MBEDTLS_ERR_ECP_IN_PROGRESS )
        return( MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS );
#endif

    /*
     * Secondary checks: always done, but change 'ret' only if it was 0
     */

#if defined(MBEDTLS_ECP_C) || defined(MBEDTLS_USE_TINYCRYPT)
    {
#if defined(MBEDTLS_USE_TINYCRYPT)
        ret = mbedtls_ssl_check_curve_uecc( ssl, MBEDTLS_UECC_DP_SECP256R1 );
#else /* MBEDTLS_USE_TINYCRYPT */
        mbedtls_pk_context *pk;
        ret = mbedtls_x509_crt_pk_acquire( chain, &pk );
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_x509_crt_pk_acquire", ret );
            return( ret );
        }

        /* If certificate uses an EC key, make sure the curve is OK */
        if( mbedtls_pk_can_do( pk, MBEDTLS_PK_ECKEY ) )
        {
            ret = mbedtls_ssl_check_curve( ssl, mbedtls_pk_ec( *pk )->grp.id );
        }

        mbedtls_x509_crt_pk_release( chain );
#endif /* MBEDTLS_USE_TINYCRYPT */

        if( ret != 0 )
        {
            ssl->session_negotiate->verify_result |= MBEDTLS_X509_BADCERT_BAD_KEY;

            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate (EC key curve)" ) );
            if( verify_ret == 0 )
                verify_ret = MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE;
            flow_counter++;
        }
        if( ret == 0 )
        {
            flow_counter++;
        }
    }
#endif /* MBEDTLS_ECP_C || MEDTLS_USE_TINYCRYPT */

    ret = mbedtls_ssl_check_cert_usage( chain,
                                      ciphersuite_info,
                                      ( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
                                        MBEDTLS_SSL_IS_CLIENT ),
                                      &ssl->session_negotiate->verify_result );
    if( ret == 0 )
    {
        flow_counter++;
    }
    else
    {
        flow_counter++;
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad certificate (usage extensions)" ) );
        if( verify_ret == 0 )
            verify_ret = MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE;
    }

    /* mbedtls_x509_crt_verify_with_profile is supposed to report a
     * verification failure through MBEDTLS_ERR_X509_CERT_VERIFY_FAILED,
     * with details encoded in the verification flags. All other kinds
     * of error codes, including those from the user provided f_vrfy
     * functions, are treated as fatal and lead to a failure of
     * ssl_parse_certificate even if verification was optional. */
    if( authmode == MBEDTLS_SSL_VERIFY_OPTIONAL &&
        ( verify_ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ||
          verify_ret == MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE ) )
    {
        mbedtls_platform_random_delay();
        if( authmode == MBEDTLS_SSL_VERIFY_OPTIONAL &&
            ( verify_ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ||
              verify_ret == MBEDTLS_ERR_SSL_BAD_HS_CERTIFICATE ) )
        {
            verify_ret = 0;
            flow_counter++;
        }
        else
        {
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        }
    } else {
        flow_counter++;
    }

    if( ca_chain == NULL && authmode == MBEDTLS_SSL_VERIFY_REQUIRED )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "got no CA chain" ) );
        verify_ret = MBEDTLS_ERR_SSL_CA_CHAIN_REQUIRED;
        flow_counter++;
    }
    else
    {
        flow_counter++;
    }

    if( verify_ret != 0 )
    {
        uint8_t alert;

        /* The certificate may have been rejected for several reasons.
           Pick one and send the corresponding alert. Which alert to send
           may be a subject of debate in some cases. */
        if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_OTHER )
            alert = MBEDTLS_SSL_ALERT_MSG_ACCESS_DENIED;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_CN_MISMATCH )
            alert = MBEDTLS_SSL_ALERT_MSG_BAD_CERT;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_KEY_USAGE )
            alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_EXT_KEY_USAGE )
            alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_NS_CERT_TYPE )
            alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_BAD_PK )
            alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_BAD_KEY )
            alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_CERT;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_EXPIRED )
            alert = MBEDTLS_SSL_ALERT_MSG_CERT_EXPIRED;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_REVOKED )
            alert = MBEDTLS_SSL_ALERT_MSG_CERT_REVOKED;
        else if( ssl->session_negotiate->verify_result & MBEDTLS_X509_BADCERT_NOT_TRUSTED )
            alert = MBEDTLS_SSL_ALERT_MSG_UNKNOWN_CA;
        else
            alert = MBEDTLS_SSL_ALERT_MSG_CERT_UNKNOWN;
        mbedtls_ssl_pend_fatal_alert( ssl, alert );
    }

    if( verify_ret == 0 &&
#if defined(MBEDTLS_ECP_C) || defined(MBEDTLS_USE_TINYCRYPT)
        flow_counter == 5 )
#else
        flow_counter == 4 )
#endif
    {
        mbedtls_platform_random_delay();
        if( verify_ret == 0 &&
#if defined(MBEDTLS_ECP_C) || defined(MBEDTLS_USE_TINYCRYPT)
            flow_counter == 5 )
#else
            flow_counter == 4 )
#endif
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> PEER AUTHENTICATED" ) );
            ssl->handshake->peer_authenticated = MBEDTLS_SSL_FI_FLAG_SET;
        }
        else
        {
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        }
    } else {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> PEER NOT AUTHENTICATED, %d", flow_counter));
    }

#if defined(MBEDTLS_DEBUG_C)
    if( ssl->session_negotiate->verify_result != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "! Certificate verification flags %x",
                                    ssl->session_negotiate->verify_result ) );
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Certificate verification flags clear" ) );
    }
#endif /* MBEDTLS_DEBUG_C */

    return( verify_ret );
}


#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED) && defined(MBEDTLS_SSL_DELAYED_SERVER_CERT_VERIFICATION)
/* mbedtls_ssl_parse_delayed_certificate_verify() defines a wrapper around ssl_parse_certificate_verify
 * to call it in ssl_cli.c rather than purely internal to ssl_tls.c.
 */
int mbedtls_ssl_parse_delayed_certificate_verify( mbedtls_ssl_context *ssl,
                                                  int authmode,
                                                  mbedtls_x509_crt *chain,
                                                  void *rs_ctx )
{

    return( ssl_parse_certificate_verify( ssl,
                                          authmode,
                                          chain,
                                          rs_ctx ) );

}
#endif /* MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED && MBEDTLS_SSL_DELAYED_SERVER_CERT_VERIFICATION */


#if !defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)

#if defined(MBEDTLS_SSL_RENEGOTIATION)
static int ssl_remember_peer_crt_digest( mbedtls_ssl_context *ssl,
                                         unsigned char *start, size_t len )
{
    int ret;
    /* Remember digest of the peer's end-CRT. */
    ssl->session_negotiate->peer_cert_digest =
        mbedtls_calloc( 1, MBEDTLS_SSL_PEER_CERT_DIGEST_DFL_LEN );
    if( ssl->session_negotiate->peer_cert_digest == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc(%d bytes) failed",
                                    sizeof( MBEDTLS_SSL_PEER_CERT_DIGEST_DFL_LEN ) ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR );

        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    ret = mbedtls_md( mbedtls_md_info_from_type(
                          MBEDTLS_SSL_PEER_CERT_DIGEST_DFL_TYPE ),
                      start, len,
                      ssl->session_negotiate->peer_cert_digest );

    ssl->session_negotiate->peer_cert_digest_type =
        MBEDTLS_SSL_PEER_CERT_DIGEST_DFL_TYPE;
    ssl->session_negotiate->peer_cert_digest_len =
        MBEDTLS_SSL_PEER_CERT_DIGEST_DFL_LEN;

    return( ret );
}
#endif /* MBEDTLS_SSL_RENEGOTIATION */

static int ssl_remember_peer_pubkey( mbedtls_ssl_context *ssl,
                                     unsigned char *start, size_t len )
{
    unsigned char *end = start + len;
    int ret;

    /* Make a copy of the peer's raw public key. */
    mbedtls_pk_init( &ssl->handshake->peer_pubkey );
    ret = mbedtls_pk_parse_subpubkey( &start, end,
                                      &ssl->handshake->peer_pubkey );
    if( ret != 0 )
    {
        /* We should have parsed the public key before. */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    ssl->handshake->got_peer_pubkey = 1;
    return( 0 );
}
#endif /* !MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */

int mbedtls_ssl_parse_certificate( mbedtls_ssl_context *ssl )
{
    volatile int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
    volatile int ret_verify = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
    volatile int check_cert_initiated = 0;
    volatile int crt_expected = SSL_CERTIFICATE_EXPECTED;
#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
    const int authmode = ssl->handshake->sni_authmode != MBEDTLS_SSL_VERIFY_UNSET
                       ? ssl->handshake->sni_authmode
                       : mbedtls_ssl_conf_get_authmode( ssl->conf );
#else
    const int authmode = mbedtls_ssl_conf_get_authmode( ssl->conf );
#endif
    void *rs_ctx = NULL;
    mbedtls_x509_crt *chain = NULL;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse certificate" ) );

    crt_expected = ssl_parse_certificate_coordinate( ssl, authmode );
    if( crt_expected == SSL_CERTIFICATE_SKIP )
    {
        mbedtls_platform_random_delay();
        crt_expected = ssl_parse_certificate_coordinate( ssl, authmode );
        if( crt_expected == SSL_CERTIFICATE_SKIP )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip parse certificate" ) );
            ret = 0;
            goto exit;
        }
    }

#if defined(MBEDTLS_SSL__ECP_RESTARTABLE)
    if( ssl->handshake->ecrs_enabled &&
        ssl->handshake->ecrs_state == ssl_ecrs_crt_verify )
    {
        chain = ssl->handshake->ecrs_peer_cert;
        ssl->handshake->ecrs_peer_cert = NULL;
        goto crt_verify;
    }
#endif

    if( ( ret = mbedtls_ssl_read_record( ssl, 1 ) ) != 0 )
    {
        /* mbedtls_ssl_read_record may have sent an alert already. We
           let it decide whether to alert. */
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        goto exit;
    }

#if defined(MBEDTLS_SSL_SRV_C)
    if( ssl_srv_check_client_no_crt_notification( ssl ) == 0 )
    {
        ssl->session_negotiate->verify_result = MBEDTLS_X509_BADCERT_MISSING;

        if( authmode == MBEDTLS_SSL_VERIFY_OPTIONAL )
            ret = 0;
        else
            ret = MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE;

        goto exit;
    }
#endif /* MBEDTLS_SSL_SRV_C */

    /* Clear existing peer CRT structure in case we tried to
     * reuse a session but it failed, and allocate a new one. */
    ssl_clear_peer_cert( ssl->session_negotiate );

    chain = mbedtls_calloc( 1, sizeof( mbedtls_x509_crt ) );
    if( chain == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc(%d bytes) failed",
                                    sizeof( mbedtls_x509_crt ) ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR );

        ret = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        goto exit;
    }
    mbedtls_x509_crt_init( chain );

    ret = ssl_parse_certificate_chain( ssl, chain );
    if( ret != 0 )
        goto exit;

#if defined(MBEDTLS_SSL__ECP_RESTARTABLE)
    if( ssl->handshake->ecrs_enabled)
        ssl->handshake->ecrs_state = ssl_ecrs_crt_verify;

crt_verify:
    check_cert_initiated = 1;
    if( ssl->handshake->ecrs_enabled)
        rs_ctx = &ssl->handshake->ecrs_ctx;
#endif

#if defined(MBEDTLS_SSL_DELAYED_SERVER_CERT_VERIFICATION)
    if ( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_CLIENT )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "delay server certificate verification" ) );
        check_cert_initiated = 0;
        ret = 0;
    }
    else
#endif /* MBEDTLS_SSL_DELAYED_SERVER_CERT_VERIFICATION */
    {
        ret_verify = ssl_parse_certificate_verify( ssl, authmode,
                                                   chain, rs_ctx );
        ret = ret_verify;
        if( ret_verify != 0 )
        {
            goto exit;
        }
    }

#if !defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    {
        size_t pk_len;
        unsigned char *pk_start;

        /* We parse the CRT chain without copying, so
         * these pointers point into the input buffer,
         * and are hence still valid after freeing the
         * CRT chain. */

#if defined(MBEDTLS_SSL_RENEGOTIATION)
        unsigned char *crt_start;
        size_t crt_len;

        crt_start = chain->raw.p;
        crt_len   = chain->raw.len;
#endif /* MBEDTLS_SSL_RENEGOTIATION */

        pk_start = chain->cache->pk_raw.p;
        pk_len   = chain->cache->pk_raw.len;

        /* Free the CRT structures before computing
         * digest and copying the peer's public key. */
        mbedtls_x509_crt_free( chain );
        mbedtls_free( chain );
        chain = NULL;

#if defined(MBEDTLS_SSL_RENEGOTIATION)
        ret = ssl_remember_peer_crt_digest( ssl, crt_start, crt_len );
        if( ret != 0 )
            goto exit;
#endif /* MBEDTLS_SSL_RENEGOTIATION */

        ret = ssl_remember_peer_pubkey( ssl, pk_start, pk_len );
        if( ret != 0 )
            goto exit;
    }
#else /* !MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */
    /* Pass ownership to session structure. */
    ssl->session_negotiate->peer_cert = chain;
    chain = NULL;
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse certificate" ) );

exit:

    if( check_cert_initiated && ( ret == 0 ) )
    {
        ret = ret_verify;
    }
    if( ret == 0 )
    {
        if( ssl->state == MBEDTLS_SSL_CLIENT_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_CLIENT_KEY_EXCHANGE;
        }
        else if( ssl->state == MBEDTLS_SSL_SERVER_CERTIFICATE )
        {
            ssl->state = MBEDTLS_SSL_SERVER_KEY_EXCHANGE;
        }
        else
        {
            ssl->state = MBEDTLS_SSL_INVALID;
        }
    }

#if defined(MBEDTLS_SSL__ECP_RESTARTABLE)
    if( ret == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS )
    {
        ssl->handshake->ecrs_peer_cert = chain;
        chain = NULL;
    }
#endif

    if( chain != NULL )
    {
        mbedtls_x509_crt_free( chain );
        mbedtls_free( chain );
    }

    return( ret );
}
#endif /* MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED */

int mbedtls_ssl_write_change_cipher_spec( mbedtls_ssl_context *ssl )
{
    int ret;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write change cipher spec" ) );

    ssl->out_msgtype = MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC;
    ssl->out_msglen  = 1;
    ssl->out_msg[0]  = 1;

    if( ssl->state == MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC )
    {
        ssl->state = MBEDTLS_SSL_CLIENT_FINISHED;
    }
    else if( ssl->state == MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC )
    {
        ssl->state = MBEDTLS_SSL_SERVER_FINISHED;
    }
    else
    {
        ssl->state = MBEDTLS_SSL_INVALID;
    }

    if( ( ret = mbedtls_ssl_write_handshake_msg( ssl ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_write_handshake_msg", ret );
        return( ret );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write change cipher spec" ) );

    return( 0 );
}

int mbedtls_ssl_parse_change_cipher_spec( mbedtls_ssl_context *ssl )
{
    int ret;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse change cipher spec" ) );

    if( ( ret = mbedtls_ssl_read_record( ssl, 1 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        return( ret );
    }

    if( ssl->in_msgtype != MBEDTLS_SSL_MSG_CHANGE_CIPHER_SPEC )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad change cipher spec message" ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                             MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE );
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
    }

    /* CCS records are only accepted if they have length 1 and content '1',
     * so we don't need to check this here. */

    /*
     * Switch to our negotiated transform and session parameters for inbound
     * data.
     */
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "switching to new transform spec for inbound data" ) );
    ssl->transform_in = ssl->transform_negotiate;
    ssl->session_in = ssl->session_negotiate;

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
        ssl_dtls_replay_reset( ssl );
#endif

        /* Increment epoch */
        if( ++ssl->in_epoch == 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "DTLS epoch would wrap" ) );
            /* This is highly unlikely to happen for legitimate reasons, so
               treat it as an attack and don't send an alert. */
            return( MBEDTLS_ERR_SSL_COUNTER_WRAPPING );
        }
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        mbedtls_platform_memset( ssl->in_ctr, 0, 8 );
    }
#endif

    ssl_update_in_pointers( ssl );

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_activate != NULL )
    {
        if( ( ret = mbedtls_ssl_hw_record_activate( ssl, MBEDTLS_SSL_CHANNEL_INBOUND ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_hw_record_activate", ret );
            mbedtls_ssl_pend_fatal_alert( ssl,
                                  MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR );
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        }
    }
#endif

    if( ssl->state == MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC )
    {
        ssl->state = MBEDTLS_SSL_CLIENT_FINISHED;
    }
    else if( ssl->state == MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC )
    {
        ssl->state = MBEDTLS_SSL_SERVER_FINISHED;
    }
    else
    {
        ssl->state = MBEDTLS_SSL_INVALID;
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse change cipher spec" ) );

    return( 0 );
}

void mbedtls_ssl_reset_checksum( mbedtls_ssl_context *ssl )
{
#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_1)
    mbedtls_md5_starts_ret( &ssl->handshake->fin_md5  );
    mbedtls_sha1_starts_ret( &ssl->handshake->fin_sha1 );
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA256_C)
    mbedtls_sha256_starts_ret( &ssl->handshake->fin_sha256, 0 );
#endif
#if defined(MBEDTLS_SHA512_C)
    mbedtls_sha512_starts_ret( &ssl->handshake->fin_sha512, 1 );
#endif
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */
}

static void ssl_handshake_wrapup_free_hs_transform( mbedtls_ssl_context *ssl )
{
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "=> handshake wrapup: final free" ) );

    /*
     * Free our handshake params
     */
    mbedtls_ssl_handshake_free( ssl );
    mbedtls_free( ssl->handshake );
    ssl->handshake = NULL;

    /*
     * Free the previous transform and swith in the current one
     */
    if( ssl->transform )
    {
        mbedtls_ssl_transform_free( ssl->transform );
        mbedtls_free( ssl->transform );
    }
    ssl->transform = ssl->transform_negotiate;
    ssl->transform_negotiate = NULL;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "<= handshake wrapup: final free" ) );
}

int mbedtls_ssl_handshake_wrapup( mbedtls_ssl_context *ssl )
{
    volatile int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;

#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
    volatile const int authmode = ssl->handshake->sni_authmode != MBEDTLS_SSL_VERIFY_UNSET
                       ? ssl->handshake->sni_authmode
                       : mbedtls_ssl_conf_get_authmode( ssl->conf );
#else
    volatile const int authmode = mbedtls_ssl_conf_get_authmode( ssl->conf );
#endif
#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
    volatile int crt_expected = SSL_CERTIFICATE_EXPECTED;
    crt_expected = ssl_parse_certificate_coordinate( ssl, authmode );
#endif
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "=> handshake wrapup" ) );

#if defined(MBEDTLS_SSL_RENEGOTIATION)
    if( ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_IN_PROGRESS )
    {
        ssl->renego_status =  MBEDTLS_SSL_RENEGOTIATION_DONE;
        ssl->renego_records_seen = 0;
    }
#endif

    /*
     * Free the previous session and switch in the current one
     */
    if( ssl->session )
    {
#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
        /* RFC 7366 3.1: keep the EtM state */
        ssl->session_negotiate->encrypt_then_mac =
                  ssl->session->encrypt_then_mac;
#endif

        mbedtls_ssl_session_free( ssl->session );
        mbedtls_free( ssl->session );
    }
    ssl->session = ssl->session_negotiate;
    ssl->session_negotiate = NULL;

#if defined(MBEDTLS_SSL_SRV_C) && !defined(MBEDTLS_SSL_NO_SESSION_CACHE)
    /*
     * Add cache entry
     */
    if( ssl->conf->f_set_cache != NULL &&
        ssl->session->id_len != 0 &&
        ssl->handshake->resume == MBEDTLS_SSL_FI_FLAG_UNSET )
    {
        if( ssl->conf->f_set_cache( ssl->conf->p_cache, ssl->session ) != 0 )
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "cache did not store session" ) );
    }
#endif /* MBEDTLS_SSL_SRV_C && !MBEDTLS_SSL_NO_SESSION_CACHE */

    if( authmode == MBEDTLS_SSL_VERIFY_NONE ||
        authmode == MBEDTLS_SSL_VERIFY_OPTIONAL ||
#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
        crt_expected == SSL_CERTIFICATE_SKIP )
#else
        1 )
#endif
    {
        mbedtls_platform_random_delay();
        if( authmode == MBEDTLS_SSL_VERIFY_NONE ||
            authmode == MBEDTLS_SSL_VERIFY_OPTIONAL ||
#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
            crt_expected == SSL_CERTIFICATE_SKIP )
#else
            1 )
#endif
        {
            ssl->handshake->peer_authenticated = MBEDTLS_SSL_FI_FLAG_SET;
        }
        else
        {
            ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
            goto cleanup;
        }
    }

#if !defined(MBEDTLS_SSL_NO_SESSION_RESUMPTION)
    if( ssl->handshake->resume == MBEDTLS_SSL_FI_FLAG_SET )
    {
        mbedtls_platform_random_delay();
        if( ssl->handshake->resume == MBEDTLS_SSL_FI_FLAG_SET )
        {
            /* When doing session resume, no premaster or peer authentication */
            ssl->handshake->peer_authenticated = MBEDTLS_SSL_FI_FLAG_SET;
            ssl->handshake->premaster_generated = MBEDTLS_SSL_FI_FLAG_SET;
        }
        else
        {
            ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
            goto cleanup;
        }
    }
#endif

    if( ssl->handshake->peer_authenticated == MBEDTLS_SSL_FI_FLAG_SET )
    {
        mbedtls_platform_random_delay();
        if( ssl->handshake->peer_authenticated == MBEDTLS_SSL_FI_FLAG_SET )
        {
            ret = 0;
        }
        else
        {
            ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
            goto cleanup;
        }
    }
    else
    {
        ret = MBEDTLS_ERR_SSL_PEER_VERIFY_FAILED;
        goto cleanup;
    }

    if( ssl->handshake->hello_random_set == MBEDTLS_SSL_FI_FLAG_SET &&
        ssl->handshake->key_derivation_done == MBEDTLS_SSL_FI_FLAG_SET &&
        ssl->handshake->premaster_generated == MBEDTLS_SSL_FI_FLAG_SET )
    {
        mbedtls_platform_random_delay();
        if( ssl->handshake->hello_random_set == MBEDTLS_SSL_FI_FLAG_SET &&
            ssl->handshake->key_derivation_done == MBEDTLS_SSL_FI_FLAG_SET &&
            ssl->handshake->premaster_generated == MBEDTLS_SSL_FI_FLAG_SET )
        {
            ret = 0;
        }
        else
        {
            ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
            goto cleanup;
        }
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "hello random %d", ssl->handshake->hello_random_set ) );
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "key_derivation_done %d", ssl->handshake->key_derivation_done ) );
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "premaster_generated %d", ssl->handshake->premaster_generated ) );
        ret = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }

cleanup:
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        ssl->handshake->flight != NULL )
    {
        /* Cancel handshake timer */
        ssl_set_timer( ssl, 0 );

        /* Keep last flight around in case we need to resend it:
         * we need the handshake and transform structures for that */
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "skip freeing handshake and transform" ) );
    }
    else
#endif
        ssl_handshake_wrapup_free_hs_transform( ssl );

    ssl->state = MBEDTLS_SSL_HANDSHAKE_OVER;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "<= handshake wrapup" ) );
    return ret;
}

int mbedtls_ssl_write_finished( mbedtls_ssl_context *ssl )
{
    int ret, hash_len;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write finished" ) );

    ssl_update_out_pointers( ssl, ssl->transform_negotiate );

    ssl_calc_finished( mbedtls_ssl_get_minor_ver( ssl ),
                       mbedtls_ssl_suite_get_mac(
                           mbedtls_ssl_ciphersuite_from_id(
                               mbedtls_ssl_session_get_ciphersuite(
                                   ssl->session_negotiate ) ) ),
                       ssl, ssl->out_msg + 4,
                       mbedtls_ssl_conf_get_endpoint( ssl->conf ) );

    /*
     * RFC 5246 7.4.9 (Page 63) says 12 is the default length and ciphersuites
     * may define some other value. Currently (early 2016), no defined
     * ciphersuite does this (and this is unlikely to change as activity has
     * moved to TLS 1.3 now) so we can keep the hardcoded 12 here.
     */
    hash_len = ( mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_0 ) ? 36 : 12;

#if defined(MBEDTLS_SSL_RENEGOTIATION)
    ssl->verify_data_len = hash_len;
    mbedtls_platform_memcpy( ssl->own_verify_data, ssl->out_msg + 4, hash_len );
#endif

    ssl->out_msglen  = 4 + hash_len;
    ssl->out_msgtype = MBEDTLS_SSL_MSG_HANDSHAKE;
    ssl->out_msg[0]  = MBEDTLS_SSL_HS_FINISHED;

#if !defined(MBEDTLS_SSL_NO_SESSION_RESUMPTION)
    /*
     * In case of session resuming, invert the client and server
     * ChangeCipherSpec messages order.
     */
    if( ssl->handshake->resume == MBEDTLS_SSL_FI_FLAG_SET )
    {
#if defined(MBEDTLS_SSL_CLI_C)
        if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
              MBEDTLS_SSL_IS_CLIENT )
        {
            ssl->state = MBEDTLS_SSL_HANDSHAKE_WRAPUP;
        }
#endif
#if defined(MBEDTLS_SSL_SRV_C)
        if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
              MBEDTLS_SSL_IS_SERVER )
        {
            ssl->state = MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC;
        }
#endif
    }
    else
#endif /* !MBEDTLS_SSL_NO_SESSION_RESUMPTION */
    {
        if( ssl->state == MBEDTLS_SSL_CLIENT_FINISHED )
        {
            ssl->state = MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC;
        }
        else if( ssl->state == MBEDTLS_SSL_SERVER_FINISHED )
        {
            ssl->state = MBEDTLS_SSL_FLUSH_BUFFERS;
        }
        else
        {
            ssl->state = MBEDTLS_SSL_INVALID;
        }
    }

    /*
     * Switch to our negotiated transform and session parameters for outbound
     * data.
     */
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "switching to new transform spec for outbound data" ) );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        uint_fast8_t i;

        /* Remember current epoch settings for resending */
        ssl->handshake->alt_transform_out = ssl->transform_out;
        mbedtls_platform_memcpy( ssl->handshake->alt_out_ctr, ssl->cur_out_ctr, 8 );

        /* Set sequence_number to zero */
        memset( ssl->cur_out_ctr + 2, 0, 6 );

        /* Increment epoch */
        for( i = 2; i > 0; i-- )
            if( ++ssl->cur_out_ctr[i - 1] != 0 )
                break;

        /* The loop goes to its end iff the counter is wrapping */
        if( i == 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "DTLS epoch would wrap" ) );
            return( MBEDTLS_ERR_SSL_COUNTER_WRAPPING );
        }
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        mbedtls_platform_memset( ssl->cur_out_ctr, 0, 8 );
    }
#endif

    ssl->transform_out = ssl->transform_negotiate;
    ssl->session_out = ssl->session_negotiate;

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_activate != NULL )
    {
        if( ( ret = mbedtls_ssl_hw_record_activate( ssl, MBEDTLS_SSL_CHANNEL_OUTBOUND ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_hw_record_activate", ret );
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        }
    }
#endif

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        mbedtls_ssl_send_flight_completed( ssl );
#endif

    if( ( ret = mbedtls_ssl_write_handshake_msg( ssl ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_write_handshake_msg", ret );
        return( ret );
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
#if defined(MBEDTLS_SSL_IMMEDIATE_TRANSMISSION)
        mbedtls_ssl_immediate_flight_done( ssl );
#else
        if( ( ret = mbedtls_ssl_flight_transmit( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_flight_transmit", ret );
            return( ret );
        }
#endif
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write finished" ) );

    return( 0 );
}

#if defined(MBEDTLS_SSL_PROTO_SSL3)
#define SSL_MAX_HASH_LEN 36
#else
#define SSL_MAX_HASH_LEN 12
#endif

int mbedtls_ssl_parse_finished( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned int hash_len;
    unsigned char buf[SSL_MAX_HASH_LEN];

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse finished" ) );

    ssl_calc_finished( mbedtls_ssl_get_minor_ver( ssl ),
                       mbedtls_ssl_suite_get_mac(
                           mbedtls_ssl_ciphersuite_from_id(
                               mbedtls_ssl_session_get_ciphersuite(
                                   ssl->session_negotiate ) ) ),
                       ssl, buf,
                       mbedtls_ssl_conf_get_endpoint( ssl->conf ) ^ 1 );

    if( ( ret = mbedtls_ssl_read_record( ssl, 1 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        return( ret );
    }

    if( ssl->in_msgtype != MBEDTLS_SSL_MSG_HANDSHAKE )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad finished message" ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                               MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE );
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
    }

    /* There is currently no ciphersuite using another length with TLS 1.2 */
#if defined(MBEDTLS_SSL_PROTO_SSL3)
    if( mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_0 )
        hash_len = 36;
    else
#endif
        hash_len = 12;

    if( ssl->in_msg[0] != MBEDTLS_SSL_HS_FINISHED ||
        ssl->in_hslen  != mbedtls_ssl_hs_hdr_len( ssl ) + hash_len )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad finished message" ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_BAD_HS_FINISHED );
    }

    if( mbedtls_platform_memequal( ssl->in_msg + mbedtls_ssl_hs_hdr_len( ssl ),
                                   buf, hash_len ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad finished message" ) );
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_BAD_HS_FINISHED );
    }

#if defined(MBEDTLS_SSL_RENEGOTIATION)
    ssl->verify_data_len = hash_len;
    mbedtls_platform_memcpy( ssl->peer_verify_data, buf, hash_len );
#endif

#if !defined(MBEDTLS_SSL_NO_SESSION_RESUMPTION)
    if( ssl->handshake->resume == MBEDTLS_SSL_FI_FLAG_SET )
    {
#if defined(MBEDTLS_SSL_CLI_C)
        if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_CLIENT )
            ssl->state = MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC;
#endif
#if defined(MBEDTLS_SSL_SRV_C)
        if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_SERVER )
            ssl->state = MBEDTLS_SSL_HANDSHAKE_WRAPUP;
#endif
    }
    else
#endif /* !MBEDTLS_SSL_NO_SESSION_RESUMPTION */
    {
        if( ssl->state == MBEDTLS_SSL_CLIENT_FINISHED )
        {
            ssl->state = MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC;
        }
        else if( ssl->state == MBEDTLS_SSL_SERVER_FINISHED )
        {
            ssl->state = MBEDTLS_SSL_FLUSH_BUFFERS;
        }
        else
        {
            ssl->state = MBEDTLS_SSL_INVALID;
        }
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        mbedtls_ssl_recv_flight_completed( ssl );
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse finished" ) );

    return( 0 );
}

static void ssl_handshake_params_init( mbedtls_ssl_handshake_params *handshake )
{
    memset( handshake, 0, sizeof( mbedtls_ssl_handshake_params ) );

#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_1)
     mbedtls_md5_init(   &handshake->fin_md5  );
    mbedtls_sha1_init(   &handshake->fin_sha1 );
     mbedtls_md5_starts_ret( &handshake->fin_md5  );
    mbedtls_sha1_starts_ret( &handshake->fin_sha1 );
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA256_C)
    mbedtls_sha256_init(   &handshake->fin_sha256    );
    mbedtls_sha256_starts_ret( &handshake->fin_sha256, 0 );
#endif
#if defined(MBEDTLS_SHA512_C)
    mbedtls_sha512_init(   &handshake->fin_sha512    );
    mbedtls_sha512_starts_ret( &handshake->fin_sha512, 1 );
#endif
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */

#if defined(MBEDTLS_SSL_PROTO_TLS1_2) && \
    defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
    mbedtls_ssl_sig_hash_set_init( &handshake->hash_algs );
#endif

#if defined(MBEDTLS_DHM_C)
    mbedtls_dhm_init( &handshake->dhm_ctx );
#endif
#if defined(MBEDTLS_ECDH_C)
    mbedtls_ecdh_init( &handshake->ecdh_ctx );
#endif
#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
    mbedtls_ecjpake_init( &handshake->ecjpake_ctx );
#if defined(MBEDTLS_SSL_CLI_C)
    handshake->ecjpake_cache = NULL;
    handshake->ecjpake_cache_len = 0;
#endif
#endif

#if defined(MBEDTLS_SSL__ECP_RESTARTABLE)
    mbedtls_x509_crt_restart_init( &handshake->ecrs_ctx );
#endif

#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
    handshake->sni_authmode = MBEDTLS_SSL_VERIFY_UNSET;
#endif

#if defined(MBEDTLS_X509_CRT_PARSE_C) && \
    !defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    mbedtls_pk_init( &handshake->peer_pubkey );
#endif
}

void mbedtls_ssl_transform_init( mbedtls_ssl_transform *transform )
{
    memset( transform, 0, sizeof(mbedtls_ssl_transform) );
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
    mbedtls_cipher_init( &transform->cipher_ctx );
#else
    mbedtls_cipher_init( &transform->cipher_ctx_enc );
    mbedtls_cipher_init( &transform->cipher_ctx_dec );
#endif

#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
    mbedtls_md_init( &transform->md_ctx_enc );
    mbedtls_md_init( &transform->md_ctx_dec );
#endif
}

void mbedtls_ssl_session_init( mbedtls_ssl_session *session )
{
    memset( session, 0, sizeof(mbedtls_ssl_session) );
}

static int ssl_handshake_init( mbedtls_ssl_context *ssl )
{
    /* Clear old handshake information if present */
    if( ssl->transform_negotiate )
        mbedtls_ssl_transform_free( ssl->transform_negotiate );
    if( ssl->session_negotiate )
        mbedtls_ssl_session_free( ssl->session_negotiate );
    if( ssl->handshake )
        mbedtls_ssl_handshake_free( ssl );

    /*
     * Either the pointers are now NULL or cleared properly and can be freed.
     * Now allocate missing structures.
     */
    if( ssl->transform_negotiate == NULL )
    {
        ssl->transform_negotiate = mbedtls_calloc( 1, sizeof(mbedtls_ssl_transform) );
    }

    if( ssl->session_negotiate == NULL )
    {
        ssl->session_negotiate = mbedtls_calloc( 1, sizeof(mbedtls_ssl_session) );
    }

    if( ssl->handshake == NULL )
    {
        ssl->handshake = mbedtls_calloc( 1, sizeof(mbedtls_ssl_handshake_params) );
    }

#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    /* If the buffers are too small - reallocate */
    handle_buffer_resizing( ssl, BUFFER_UPSIZING, MBEDTLS_SSL_IN_BUFFER_LEN,
                                                  MBEDTLS_SSL_OUT_BUFFER_LEN );
#endif

    /* All pointers should exist and can be directly freed without issue */
    if( ssl->handshake == NULL ||
        ssl->transform_negotiate == NULL ||
        ssl->session_negotiate == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc() of ssl sub-contexts failed" ) );

        mbedtls_free( ssl->handshake );
        mbedtls_free( ssl->transform_negotiate );
        mbedtls_free( ssl->session_negotiate );

        ssl->handshake = NULL;
        ssl->transform_negotiate = NULL;
        ssl->session_negotiate = NULL;

        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    /* Initialize structures */
    mbedtls_ssl_session_init( ssl->session_negotiate );
    mbedtls_ssl_transform_init( ssl->transform_negotiate );
    ssl_handshake_params_init( ssl->handshake );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        ssl->handshake->alt_transform_out = ssl->transform_out;

        if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_CLIENT )
            ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_PREPARING;
        else
            ssl->handshake->retransmit_state = MBEDTLS_SSL_RETRANS_WAITING;
    }
#endif

    return( 0 );
}

#if defined(MBEDTLS_SSL_DTLS_HELLO_VERIFY) && defined(MBEDTLS_SSL_SRV_C)
/* Dummy cookie callbacks for defaults */
static int ssl_cookie_write_dummy( void *ctx,
                      unsigned char **p, unsigned char *end,
                      const unsigned char *cli_id, size_t cli_id_len )
{
    ((void) ctx);
    ((void) p);
    ((void) end);
    ((void) cli_id);
    ((void) cli_id_len);

    return( MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE );
}

static int ssl_cookie_check_dummy( void *ctx,
                      const unsigned char *cookie, size_t cookie_len,
                      const unsigned char *cli_id, size_t cli_id_len )
{
    ((void) ctx);
    ((void) cookie);
    ((void) cookie_len);
    ((void) cli_id);
    ((void) cli_id_len);

    return( MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE );
}
#endif /* MBEDTLS_SSL_DTLS_HELLO_VERIFY && MBEDTLS_SSL_SRV_C */

/* Once ssl->out_hdr as the address of the beginning of the
 * next outgoing record is set, deduce the other pointers.
 *
 * Note: For TLS, we save the implicit record sequence number
 *       (entering MAC computation) in the 8 bytes before ssl->out_hdr,
 *       and the caller has to make sure there's space for this.
 */

static void ssl_update_out_pointers( mbedtls_ssl_context *ssl,
                                     mbedtls_ssl_transform *transform )
{
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        ssl->out_ctr = ssl->out_hdr +  3;
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
        ssl->out_cid = ssl->out_ctr +  8;
        ssl->out_len = ssl->out_cid;
        if( transform != NULL )
            ssl->out_len += transform->out_cid_len;
#else /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
        ssl->out_len = ssl->out_ctr + 8;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
        ssl->out_iv  = ssl->out_len + 2;
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        ssl->out_ctr = ssl->out_hdr - 8;
        ssl->out_len = ssl->out_hdr + 3;
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
        ssl->out_cid = ssl->out_len;
#endif
        ssl->out_iv  = ssl->out_hdr + 5;
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */

    /* Adjust out_msg to make space for explicit IV, if used. */
    if( transform != NULL &&
        mbedtls_ssl_ver_geq(
            mbedtls_ssl_get_minor_ver( ssl ),
            MBEDTLS_SSL_MINOR_VERSION_2 ) )
    {
        ssl->out_msg = ssl->out_iv + transform->ivlen - transform->fixed_ivlen;
    }
    else
        ssl->out_msg = ssl->out_iv;
}

/* Once ssl->in_hdr as the address of the beginning of the
 * next incoming record is set, deduce the other pointers.
 *
 * Note: For TLS, we save the implicit record sequence number
 *       (entering MAC computation) in the 8 bytes before ssl->in_hdr,
 *       and the caller has to make sure there's space for this.
 */

static void ssl_update_in_pointers( mbedtls_ssl_context *ssl )
{
    /* This function sets the pointers to match the case
     * of unprotected TLS/DTLS records, with ssl->in_msg
     * pointing to the beginning of the record content.
     *
     * When decrypting a protected record, ssl->in_msg
     * will be shifted to point to the beginning of the
     * record plaintext.
     */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        /* This sets the header pointers to match records
         * without CID. When we receive a record containing
         * a CID, the fields are shifted accordingly in
         * ssl_parse_record_header(). */
        ssl->in_ctr = ssl->in_hdr +  3;
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
        ssl->in_cid = ssl->in_ctr +  8;
        ssl->in_len = ssl->in_cid; /* Default: no CID */
#else /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
        ssl->in_len = ssl->in_ctr + 8;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */
        ssl->in_msg  = ssl->in_len + 2;
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        ssl->in_ctr = ssl->in_hdr - 8;
        ssl->in_len = ssl->in_hdr + 3;
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
        ssl->in_cid = ssl->in_len;
#endif
        ssl->in_msg  = ssl->in_hdr + 5;
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */
}

/*
 * Initialize an SSL context
 */
void mbedtls_ssl_init( mbedtls_ssl_context *ssl )
{
    memset( ssl, 0, sizeof( mbedtls_ssl_context ) );
}

/*
 * Setup an SSL context
 */

static void ssl_reset_in_out_pointers( mbedtls_ssl_context *ssl )
{
    /* Set the incoming and outgoing record pointers. */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        ssl->out_hdr = ssl->out_buf;
        ssl->in_hdr  = ssl->in_buf;
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        ssl->out_hdr = ssl->out_buf + 8;
        ssl->in_hdr  = ssl->in_buf  + 8;
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */

    /* Derive other internal pointers. */
    ssl_update_out_pointers( ssl, NULL /* no transform enabled */ );
    ssl_update_in_pointers ( ssl );
}

int mbedtls_ssl_setup( mbedtls_ssl_context *ssl,
                       const mbedtls_ssl_config *conf )
{
    int ret;
    size_t in_buf_len = MBEDTLS_SSL_IN_BUFFER_LEN;
    size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;

    ssl->conf = conf;

#if defined(MBEDTLS_USE_TINYCRYPT)
    uECC_set_rng( &uecc_rng_wrapper );
#endif

    /*
     * Prepare base structures
     */

    /* Set to NULL in case of an error condition */
    ssl->out_buf = NULL;

#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    ssl->in_buf_len = in_buf_len;
#endif
    ssl->in_buf = mbedtls_calloc( 1, in_buf_len );
    if( ssl->in_buf == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc(%d bytes) failed", in_buf_len) );
        ret = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        goto error;
    }

#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    ssl->out_buf_len = out_buf_len;
#endif

    ssl->out_buf = mbedtls_calloc( 1, out_buf_len  );
    if( ssl->out_buf == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc(%d bytes) failed", out_buf_len) );
        ret = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        goto error;
    }

    ssl_reset_in_out_pointers( ssl );

    if( ( ret = ssl_handshake_init( ssl ) ) != 0 )
        goto error;

    ssl->pending_fatal_alert_msg = MBEDTLS_SSL_ALERT_MSG_NONE;

    return( 0 );

error:
    mbedtls_free( ssl->in_buf );
    mbedtls_free( ssl->out_buf );

    ssl->conf = NULL;

#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    ssl->in_buf_len = 0;
    ssl->out_buf_len = 0;
#endif

    ssl->in_buf = NULL;
    ssl->out_buf = NULL;

    ssl->in_hdr = NULL;
    ssl->in_ctr = NULL;
    ssl->in_len = NULL;
    ssl->in_msg = NULL;

    ssl->out_hdr = NULL;
    ssl->out_ctr = NULL;
    ssl->out_len = NULL;
    ssl->out_iv = NULL;
    ssl->out_msg = NULL;

    return( ret );
}

/*
 * Reset an initialized and used SSL context for re-use while retaining
 * all application-set variables, function pointers and data.
 *
 * If partial is non-zero, keep data in the input buffer and client ID.
 * (Use when a DTLS client reconnects from the same port.)
 */
static int ssl_session_reset_int( mbedtls_ssl_context *ssl, int partial )
{
    int ret;
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    size_t in_buf_len = ssl->in_buf_len;
    size_t out_buf_len = ssl->out_buf_len;
#else
    size_t in_buf_len = MBEDTLS_SSL_IN_BUFFER_LEN;
    size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;
#endif

#if !defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE) ||     \
    !defined(MBEDTLS_SSL_SRV_C)
    ((void) partial);
#endif

    ssl->state = MBEDTLS_SSL_HELLO_REQUEST;

    /* Cancel any possibly running timer */
    ssl_set_timer( ssl, 0 );

#if defined(MBEDTLS_SSL_RENEGOTIATION)
    ssl->renego_status = MBEDTLS_SSL_INITIAL_HANDSHAKE;
    ssl->renego_records_seen = 0;

    ssl->verify_data_len = 0;
    mbedtls_platform_memset( ssl->own_verify_data, 0, MBEDTLS_SSL_VERIFY_DATA_MAX_LEN );
    mbedtls_platform_memset( ssl->peer_verify_data, 0, MBEDTLS_SSL_VERIFY_DATA_MAX_LEN );
#endif
    ssl->secure_renegotiation = MBEDTLS_SSL_LEGACY_RENEGOTIATION;

    ssl->in_offt = NULL;
    ssl_reset_in_out_pointers( ssl );

    ssl->in_msgtype = 0;
    ssl->in_msglen = 0;
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    ssl->next_record_offset = 0;
    ssl->in_epoch = 0;
#endif
#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
    ssl_dtls_replay_reset( ssl );
#endif

    ssl->in_hslen = 0;
    ssl->nb_zero = 0;

    ssl->keep_current_message = 0;

    ssl->out_msgtype = 0;
    ssl->out_msglen = 0;
    ssl->out_left = 0;
#if defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
    if( ssl->split_done != MBEDTLS_SSL_CBC_RECORD_SPLITTING_DISABLED )
        ssl->split_done = 0;
#endif

    memset( ssl->cur_out_ctr, 0, sizeof( ssl->cur_out_ctr ) );

    ssl->transform_in = NULL;
    ssl->transform_out = NULL;

    ssl->session_in = NULL;
    ssl->session_out = NULL;

    mbedtls_platform_memset( ssl->out_buf, 0, out_buf_len );

#if defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE) && defined(MBEDTLS_SSL_SRV_C)
    if( partial == 0 )
#endif /* MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE && MBEDTLS_SSL_SRV_C */
    {
        ssl->in_left = 0;
        mbedtls_platform_memset( ssl->in_buf, 0, in_buf_len );
    }

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_reset != NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "going for mbedtls_ssl_hw_record_reset()" ) );
        if( ( ret = mbedtls_ssl_hw_record_reset( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_hw_record_reset", ret );
            return( MBEDTLS_ERR_SSL_HW_ACCEL_FAILED );
        }
    }
#endif

    if( ssl->transform )
    {
        mbedtls_ssl_transform_free( ssl->transform );
        mbedtls_free( ssl->transform );
        ssl->transform = NULL;
    }

    if( ssl->session )
    {
        mbedtls_ssl_session_free( ssl->session );
        mbedtls_free( ssl->session );
        ssl->session = NULL;
    }

#if defined(MBEDTLS_SSL_ALPN)
    ssl->alpn_chosen = NULL;
#endif

#if defined(MBEDTLS_SSL_DTLS_HELLO_VERIFY) && defined(MBEDTLS_SSL_SRV_C)
#if defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE)
    if( partial == 0 )
#endif
    {
        mbedtls_free( ssl->cli_id );
        ssl->cli_id = NULL;
        ssl->cli_id_len = 0;
    }
#endif

    if( ( ret = ssl_handshake_init( ssl ) ) != 0 )
        return( ret );

    return( 0 );
}

/*
 * Reset an initialized and used SSL context for re-use while retaining
 * all application-set variables, function pointers and data.
 */
int mbedtls_ssl_session_reset( mbedtls_ssl_context *ssl )
{
    return( ssl_session_reset_int( ssl, 0 ) );
}

/*
 * SSL set accessors
 */
#if !defined(MBEDTLS_SSL_CONF_ENDPOINT)
void mbedtls_ssl_conf_endpoint( mbedtls_ssl_config *conf, int endpoint )
{
    conf->endpoint   = endpoint;
}
#endif /* MBEDTLS_SSL_CONF_ENDPOINT */

#if !defined(MBEDTLS_SSL_CONF_TRANSPORT)
void mbedtls_ssl_conf_transport( mbedtls_ssl_config *conf, int transport )
{
    conf->transport = transport;
}
#endif /* MBEDTLS_SSL_CONF_TRANSPORT */

#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY) && \
    !defined(MBEDTLS_SSL_CONF_ANTI_REPLAY)
void mbedtls_ssl_conf_dtls_anti_replay( mbedtls_ssl_config *conf, char mode )
{
    conf->anti_replay   = mode;
}
#endif /* MBEDTLS_SSL_DTLS_ANTI_REPLAY && !MBEDTLS_SSL_CONF_ANTI_REPLAY */

#if defined(MBEDTLS_SSL_DTLS_BADMAC_LIMIT) && \
    !defined(MBEDTLS_SSL_CONF_BADMAC_LIMIT)
void mbedtls_ssl_conf_dtls_badmac_limit( mbedtls_ssl_config *conf,
                                         unsigned limit )
{
    conf->badmac_limit = limit;
}
#endif /* MBEDTLS_SSL_DTLS_BADMAC_LIMIT && !MBEDTLS_SSL_CONF_BADMAC_LIMIT */

#if defined(MBEDTLS_SSL_PROTO_DTLS)

void mbedtls_ssl_set_datagram_packing( mbedtls_ssl_context *ssl,
                                       unsigned allow_packing )
{
    ssl->disable_datagram_packing = !allow_packing;
}

#if !( defined(MBEDTLS_SSL_CONF_HS_TIMEOUT_MAX) &&      \
       defined(MBEDTLS_SSL_CONF_HS_TIMEOUT_MIN) )
void mbedtls_ssl_conf_handshake_timeout( mbedtls_ssl_config *conf,
                                         uint32_t min, uint32_t max )
{
    conf->hs_timeout_min = min;
    conf->hs_timeout_max = max;
}
#else /* !( MBEDTLS_SSL_CONF_HS_TIMEOUT_MIN &&
            MBEDTLS_SSL_CONF_HS_TIMEOUT_MAX ) */
void mbedtls_ssl_conf_handshake_timeout( mbedtls_ssl_config *conf,
                                         uint32_t min, uint32_t max )
{
    ((void) conf);
    ((void) min);
    ((void) max);
}
#endif /* MBEDTLS_SSL_CONF_HS_TIMEOUT_MIN &&
          MBEDTLS_SSL_CONF_HS_TIMEOUT_MAX */

#endif /* MBEDTLS_SSL_PROTO_DTLS */

void mbedtls_ssl_conf_authmode( mbedtls_ssl_config *conf, int authmode )
{
#if !defined(MBEDTLS_SSL_CONF_AUTHMODE)
    conf->authmode = authmode;
#else
    ((void) conf);
    ((void) authmode);
#endif /* MBEDTLS_SSL_CONF_AUTHMODE */
}

#if defined(MBEDTLS_X509_CRT_PARSE_C) && \
    !defined(MBEDTLS_X509_REMOVE_VERIFY_CALLBACK)
void mbedtls_ssl_conf_verify( mbedtls_ssl_config *conf,
                     int (*f_vrfy)(void *, mbedtls_x509_crt *, int, uint32_t *),
                     void *p_vrfy )
{
    conf->f_vrfy      = f_vrfy;
    conf->p_vrfy      = p_vrfy;
}
#endif /* MBEDTLS_X509_CRT_PARSE_C && !MBEDTLS_X509_REMOVE_VERIFY_CALLBACK */

#if !defined(MBEDTLS_SSL_CONF_RNG)
void mbedtls_ssl_conf_rng( mbedtls_ssl_config *conf,
                  int (*f_rng)(void *, unsigned char *, size_t),
                  void *p_rng )
{
    conf->f_rng      = f_rng;
    conf->p_rng      = p_rng;
}
#endif

#if defined(MBEDTLS_DEBUG_C)
void mbedtls_ssl_conf_dbg( mbedtls_ssl_config *conf,
                  void (*f_dbg)(void *, int, const char *, int, const char *),
                  void  *p_dbg )
{
    conf->f_dbg      = f_dbg;
    conf->p_dbg      = p_dbg;
}
#endif /* MBEDTLS_DEBUG_C */

#if !defined(MBEDTLS_SSL_CONF_RECV) && \
    !defined(MBEDTLS_SSL_CONF_SEND) && \
    !defined(MBEDTLS_SSL_CONF_RECV_TIMEOUT)
void mbedtls_ssl_set_bio( mbedtls_ssl_context *ssl,
        void *p_bio,
        mbedtls_ssl_send_t *f_send,
        mbedtls_ssl_recv_t *f_recv,
        mbedtls_ssl_recv_timeout_t *f_recv_timeout )
{
    ssl->p_bio = p_bio;
    ssl->f_send = f_send;
    ssl->f_recv = f_recv;
    ssl->f_recv_timeout = f_recv_timeout;
}
#else
void mbedtls_ssl_set_bio_ctx( mbedtls_ssl_context *ssl,
                              void *p_bio )
{
    ssl->p_bio = p_bio;
}
#endif

#if defined(MBEDTLS_SSL_PROTO_DTLS)
void mbedtls_ssl_set_mtu( mbedtls_ssl_context *ssl, uint16_t mtu )
{
    ssl->mtu = mtu;
}
#endif

#if !defined(MBEDTLS_SSL_CONF_READ_TIMEOUT)
void mbedtls_ssl_conf_read_timeout( mbedtls_ssl_config *conf, uint32_t timeout )
{
    conf->read_timeout   = timeout;
}
#endif /* MBEDTLS_SSL_CONF_READ_TIMEOUT */

#if !defined(MBEDTLS_SSL_CONF_SET_TIMER) && \
    !defined(MBEDTLS_SSL_CONF_GET_TIMER)
void mbedtls_ssl_set_timer_cb( mbedtls_ssl_context *ssl,
                               void *p_timer,
                               mbedtls_ssl_set_timer_t *f_set_timer,
                               mbedtls_ssl_get_timer_t *f_get_timer )
{
    ssl->p_timer        = p_timer;
    ssl->f_set_timer    = f_set_timer;
    ssl->f_get_timer    = f_get_timer;
    /* Make sure we start with no timer running */
    ssl_set_timer( ssl, 0 );
}
#else
void mbedtls_ssl_set_timer_cb_ctx( mbedtls_ssl_context *ssl,
                               void *p_timer )
{
    ssl->p_timer        = p_timer;
    /* Make sure we start with no timer running */
    ssl_set_timer( ssl, 0 );
}
#endif

#if defined(MBEDTLS_SSL_SRV_C) && !defined(MBEDTLS_SSL_NO_SESSION_CACHE)
void mbedtls_ssl_conf_session_cache( mbedtls_ssl_config *conf,
        void *p_cache,
        int (*f_get_cache)(void *, mbedtls_ssl_session *),
        int (*f_set_cache)(void *, const mbedtls_ssl_session *) )
{
    conf->p_cache = p_cache;
    conf->f_get_cache = f_get_cache;
    conf->f_set_cache = f_set_cache;
}
#endif /* MBEDTLS_SSL_SRV_C && !MBEDTLS_SSL_NO_SESSION_CACHE */

#if defined(MBEDTLS_SSL_CLI_C) && !defined(MBEDTLS_SSL_NO_SESSION_RESUMPTION)
int mbedtls_ssl_set_session( mbedtls_ssl_context *ssl, const mbedtls_ssl_session *session )
{
    int ret;

    if( ssl == NULL ||
        session == NULL ||
        ssl->session_negotiate == NULL ||
        mbedtls_ssl_conf_get_endpoint( ssl->conf ) != MBEDTLS_SSL_IS_CLIENT )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    if( ( ret = mbedtls_ssl_session_copy( ssl->session_negotiate,
                                          session ) ) != 0 )
        return( ret );

    ssl->handshake->resume = MBEDTLS_SSL_FI_FLAG_SET;

    return( 0 );
}
#endif /* MBEDTLS_SSL_CLI_C && !MBEDTLS_SSL_NO_SESSION_RESUMPTION */

#if !defined(MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE)
void mbedtls_ssl_conf_ciphersuites( mbedtls_ssl_config *conf,
                                   const int *ciphersuites )
{
    conf->ciphersuite_list[0] = ciphersuites;
    conf->ciphersuite_list[1] = ciphersuites;
    conf->ciphersuite_list[2] = ciphersuites;
    conf->ciphersuite_list[3] = ciphersuites;
}

void mbedtls_ssl_conf_ciphersuites_for_version( mbedtls_ssl_config *conf,
                                       const int *ciphersuites,
                                       int major, int minor )
{
    if( major != MBEDTLS_SSL_MAJOR_VERSION_3 )
        return;

    if( mbedtls_ssl_ver_lt( minor, MBEDTLS_SSL_MINOR_VERSION_0 ) ||
        mbedtls_ssl_ver_gt( minor, MBEDTLS_SSL_MINOR_VERSION_3 ) )
    {
        return;
    }

    conf->ciphersuite_list[mbedtls_ssl_minor_ver_index( minor )] =
        ciphersuites;
}
#endif /* MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE */

#if defined(MBEDTLS_X509_CRT_PARSE_C)
void mbedtls_ssl_conf_cert_profile( mbedtls_ssl_config *conf,
                                    const mbedtls_x509_crt_profile *profile )
{
    conf->cert_profile = profile;
}

/* Append a new keycert entry to a (possibly empty) list */
static int ssl_append_key_cert( mbedtls_ssl_key_cert **head,
                                mbedtls_x509_crt *cert,
                                mbedtls_pk_context *key )
{
    mbedtls_ssl_key_cert *new_cert;

    new_cert = mbedtls_calloc( 1, sizeof( mbedtls_ssl_key_cert ) );
    if( new_cert == NULL )
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

    new_cert->cert = cert;
    new_cert->key  = key;
    new_cert->next = NULL;

    /* Update head is the list was null, else add to the end */
    if( *head == NULL )
    {
        *head = new_cert;
    }
    else
    {
        mbedtls_ssl_key_cert *cur = *head;
        while( cur->next != NULL )
            cur = cur->next;
        cur->next = new_cert;
    }

    return( 0 );
}

int mbedtls_ssl_conf_own_cert( mbedtls_ssl_config *conf,
                              mbedtls_x509_crt *own_cert,
                              mbedtls_pk_context *pk_key )
{
    return( ssl_append_key_cert( &conf->key_cert, own_cert, pk_key ) );
}

void mbedtls_ssl_conf_ca_chain( mbedtls_ssl_config *conf,
                               mbedtls_x509_crt *ca_chain,
                               mbedtls_x509_crl *ca_crl )
{
    conf->ca_chain   = ca_chain;
    conf->ca_crl     = ca_crl;
}
#endif /* MBEDTLS_X509_CRT_PARSE_C */

#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
int mbedtls_ssl_set_hs_own_cert( mbedtls_ssl_context *ssl,
                                 mbedtls_x509_crt *own_cert,
                                 mbedtls_pk_context *pk_key )
{
    return( ssl_append_key_cert( &ssl->handshake->sni_key_cert,
                                 own_cert, pk_key ) );
}

void mbedtls_ssl_set_hs_ca_chain( mbedtls_ssl_context *ssl,
                                  mbedtls_x509_crt *ca_chain,
                                  mbedtls_x509_crl *ca_crl )
{
    ssl->handshake->sni_ca_chain   = ca_chain;
    ssl->handshake->sni_ca_crl     = ca_crl;
}

void mbedtls_ssl_set_hs_authmode( mbedtls_ssl_context *ssl,
                                  int authmode )
{
    ssl->handshake->sni_authmode = authmode;
}
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */

#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
/*
 * Set EC J-PAKE password for current handshake
 */
int mbedtls_ssl_set_hs_ecjpake_password( mbedtls_ssl_context *ssl,
                                         const unsigned char *pw,
                                         size_t pw_len )
{
    mbedtls_ecjpake_role role;

    if( ssl->handshake == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_SERVER )
        role = MBEDTLS_ECJPAKE_SERVER;
    else
        role = MBEDTLS_ECJPAKE_CLIENT;

    return( mbedtls_ecjpake_setup( &ssl->handshake->ecjpake_ctx,
                                   role,
                                   MBEDTLS_MD_SHA256,
                                   MBEDTLS_ECP_DP_SECP256R1,
                                   pw, pw_len ) );
}
#endif /* MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED */

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
int mbedtls_ssl_conf_psk( mbedtls_ssl_config *conf,
                const unsigned char *psk, size_t psk_len,
                const unsigned char *psk_identity, size_t psk_identity_len )
{
    if( psk == NULL || psk_identity == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    if( psk_len > MBEDTLS_PSK_MAX_LEN )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    /* Identity len will be encoded on two bytes */
    if( ( psk_identity_len >> 16 ) != 0 ||
        psk_identity_len > MBEDTLS_SSL_OUT_CONTENT_LEN )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    if( conf->psk != NULL )
    {
        mbedtls_platform_zeroize( conf->psk, conf->psk_len );

        mbedtls_free( conf->psk );
        conf->psk = NULL;
        conf->psk_len = 0;
    }
    if( conf->psk_identity != NULL )
    {
        mbedtls_free( conf->psk_identity );
        conf->psk_identity = NULL;
        conf->psk_identity_len = 0;
    }

    if( ( conf->psk = mbedtls_calloc( 1, psk_len ) ) == NULL ||
        ( conf->psk_identity = mbedtls_calloc( 1, psk_identity_len ) ) == NULL )
    {
        mbedtls_free( conf->psk );
        mbedtls_free( conf->psk_identity );
        conf->psk = NULL;
        conf->psk_identity = NULL;
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    conf->psk_len = psk_len;
    conf->psk_identity_len = psk_identity_len;

    mbedtls_platform_memcpy( conf->psk, psk, conf->psk_len );
    mbedtls_platform_memcpy( conf->psk_identity, psk_identity, conf->psk_identity_len );

    return( 0 );
}

int mbedtls_ssl_set_hs_psk( mbedtls_ssl_context *ssl,
                            const unsigned char *psk, size_t psk_len )
{
    if( psk == NULL || ssl->handshake == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    if( psk_len > MBEDTLS_PSK_MAX_LEN )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    if( ssl->handshake->psk != NULL )
    {
        mbedtls_platform_zeroize( ssl->handshake->psk,
                                  ssl->handshake->psk_len );
        mbedtls_free( ssl->handshake->psk );
        ssl->handshake->psk_len = 0;
    }

    if( ( ssl->handshake->psk = mbedtls_calloc( 1, psk_len ) ) == NULL )
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

    ssl->handshake->psk_len = psk_len;
    mbedtls_platform_memcpy( ssl->handshake->psk, psk, ssl->handshake->psk_len );

    return( 0 );
}

void mbedtls_ssl_conf_psk_cb( mbedtls_ssl_config *conf,
                     int (*f_psk)(void *, mbedtls_ssl_context *, const unsigned char *,
                     size_t),
                     void *p_psk )
{
    conf->f_psk = f_psk;
    conf->p_psk = p_psk;
}
#endif /* MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED */

#if defined(MBEDTLS_DHM_C) && defined(MBEDTLS_SSL_SRV_C)

#if !defined(MBEDTLS_DEPRECATED_REMOVED)
int mbedtls_ssl_conf_dh_param( mbedtls_ssl_config *conf, const char *dhm_P, const char *dhm_G )
{
    int ret;

    if( ( ret = mbedtls_mpi_read_string( &conf->dhm_P, 16, dhm_P ) ) != 0 ||
        ( ret = mbedtls_mpi_read_string( &conf->dhm_G, 16, dhm_G ) ) != 0 )
    {
        mbedtls_mpi_free( &conf->dhm_P );
        mbedtls_mpi_free( &conf->dhm_G );
        return( ret );
    }

    return( 0 );
}
#endif /* MBEDTLS_DEPRECATED_REMOVED */

int mbedtls_ssl_conf_dh_param_bin( mbedtls_ssl_config *conf,
                                   const unsigned char *dhm_P, size_t P_len,
                                   const unsigned char *dhm_G, size_t G_len )
{
    int ret;

    if( ( ret = mbedtls_mpi_read_binary( &conf->dhm_P, dhm_P, P_len ) ) != 0 ||
        ( ret = mbedtls_mpi_read_binary( &conf->dhm_G, dhm_G, G_len ) ) != 0 )
    {
        mbedtls_mpi_free( &conf->dhm_P );
        mbedtls_mpi_free( &conf->dhm_G );
        return( ret );
    }

    return( 0 );
}

int mbedtls_ssl_conf_dh_param_ctx( mbedtls_ssl_config *conf, mbedtls_dhm_context *dhm_ctx )
{
    int ret;

    if( ( ret = mbedtls_mpi_copy( &conf->dhm_P, &dhm_ctx->P ) ) != 0 ||
        ( ret = mbedtls_mpi_copy( &conf->dhm_G, &dhm_ctx->G ) ) != 0 )
    {
        mbedtls_mpi_free( &conf->dhm_P );
        mbedtls_mpi_free( &conf->dhm_G );
        return( ret );
    }

    return( 0 );
}
#endif /* MBEDTLS_DHM_C && MBEDTLS_SSL_SRV_C */

#if defined(MBEDTLS_DHM_C) && defined(MBEDTLS_SSL_CLI_C)
/*
 * Set the minimum length for Diffie-Hellman parameters
 */
void mbedtls_ssl_conf_dhm_min_bitlen( mbedtls_ssl_config *conf,
                                      unsigned int bitlen )
{
    conf->dhm_min_bitlen = bitlen;
}
#endif /* MBEDTLS_DHM_C && MBEDTLS_SSL_CLI_C */

#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
/*
 * Set allowed/preferred hashes for handshake signatures
 */
void mbedtls_ssl_conf_sig_hashes( mbedtls_ssl_config *conf,
                                  const int *hashes )
{
#if !defined(MBEDTLS_SSL_CONF_SINGLE_SIG_HASH)
    conf->sig_hashes = hashes;
#else
    ((void) conf);
    ((void) hashes);
#endif /* MBEDTLS_SSL_CONF_SINGLE_SIG_HASH */
}
#endif /* MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED */

#if defined(MBEDTLS_ECP_C)
#if !defined(MBEDTLS_SSL_CONF_SINGLE_EC)
/*
 * Set the allowed elliptic curves
 */
void mbedtls_ssl_conf_curves( mbedtls_ssl_config *conf,
                             const mbedtls_ecp_group_id *curve_list )
{
    conf->curve_list = curve_list;
}
#endif /* MBEDTLS_SSL_CONF_SINGLE_EC */
#endif /* MBEDTLS_ECP_C */

#if defined(MBEDTLS_X509_CRT_PARSE_C) && !defined(MBEDTLS_X509_REMOVE_HOSTNAME_VERIFICATION)
int mbedtls_ssl_set_hostname( mbedtls_ssl_context *ssl, const char *hostname )
{
    /* Initialize to suppress unnecessary compiler warning */
    size_t hostname_len = 0;

    /* Check if new hostname is valid before
     * making any change to current one */
    if( hostname != NULL )
    {
        hostname_len = strlen( hostname );

        if( hostname_len > MBEDTLS_SSL_MAX_HOST_NAME_LEN )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    /* Now it's clear that we will overwrite the old hostname,
     * so we can free it safely */

    if( ssl->hostname != NULL )
    {
        mbedtls_platform_zeroize( ssl->hostname, strlen( ssl->hostname ) );
        mbedtls_free( ssl->hostname );
    }

    /* Passing NULL as hostname shall clear the old one */

    if( hostname == NULL )
    {
        ssl->hostname = NULL;
    }
    else
    {
        ssl->hostname = mbedtls_calloc( 1, hostname_len + 1 );
        if( ssl->hostname == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        /* Not using more secure mbedtls_platform_memcpy as hostname is public in initial handshake */
        memcpy( ssl->hostname, hostname, hostname_len );

        ssl->hostname[hostname_len] = '\0';
    }

    return( 0 );
}
#endif /* MBEDTLS_X509_CRT_PARSE_C && !MBEDTLS_X509_REMOVE_HOSTNAME_VERIFICATION */

#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
void mbedtls_ssl_conf_sni( mbedtls_ssl_config *conf,
                  int (*f_sni)(void *, mbedtls_ssl_context *,
                                const unsigned char *, size_t),
                  void *p_sni )
{
    conf->f_sni = f_sni;
    conf->p_sni = p_sni;
}
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */

#if defined(MBEDTLS_SSL_ALPN)
int mbedtls_ssl_conf_alpn_protocols( mbedtls_ssl_config *conf, const char **protos )
{
    size_t cur_len, tot_len;
    const char **p;

    /*
     * RFC 7301 3.1: "Empty strings MUST NOT be included and byte strings
     * MUST NOT be truncated."
     * We check lengths now rather than later.
     */
    tot_len = 0;
    for( p = protos; *p != NULL; p++ )
    {
        cur_len = strlen( *p );
        tot_len += cur_len;

        if( cur_len == 0 || cur_len > 255 || tot_len > 65535 )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    conf->alpn_list = protos;

    return( 0 );
}

const char *mbedtls_ssl_get_alpn_protocol( const mbedtls_ssl_context *ssl )
{
    return( ssl->alpn_chosen );
}
#endif /* MBEDTLS_SSL_ALPN */

#if !defined(MBEDTLS_SSL_CONF_MAX_MINOR_VER) || \
    !defined(MBEDTLS_SSL_CONF_MAX_MAJOR_VER)
void mbedtls_ssl_conf_max_version( mbedtls_ssl_config *conf,
                                   int major, int minor )
{
    conf->max_major_ver = major;
    conf->max_minor_ver = minor;
}
#endif /* MBEDTLS_SSL_CONF_MAX_MINOR_VER ||
          MBEDTLS_SSL_CONF_MAX_MAJOR_VER */

#if !defined(MBEDTLS_SSL_CONF_MIN_MINOR_VER) || \
    !defined(MBEDTLS_SSL_CONF_MIN_MAJOR_VER)
void mbedtls_ssl_conf_min_version( mbedtls_ssl_config *conf,
                                   int major, int minor )
{
    conf->min_major_ver = major;
    conf->min_minor_ver = minor;
}
#endif /* MBEDTLS_SSL_CONF_MIN_MINOR_VER ||
          MBEDTLS_SSL_CONF_MIN_MAJOR_VER */

#if defined(MBEDTLS_SSL_FALLBACK_SCSV) && defined(MBEDTLS_SSL_CLI_C)
void mbedtls_ssl_conf_fallback( mbedtls_ssl_config *conf, char fallback )
{
    conf->fallback = fallback;
}
#endif

#if defined(MBEDTLS_SSL_SRV_C) && !defined(MBEDTLS_SSL_CONF_CERT_REQ_CA_LIST)
void mbedtls_ssl_conf_cert_req_ca_list( mbedtls_ssl_config *conf,
                                          char cert_req_ca_list )
{
    conf->cert_req_ca_list = cert_req_ca_list;
}
#endif

#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
void mbedtls_ssl_conf_encrypt_then_mac( mbedtls_ssl_config *conf, char etm )
{
    conf->encrypt_then_mac = etm;
}
#endif

#if defined(MBEDTLS_SSL_EXTENDED_MASTER_SECRET)
#if !defined(MBEDTLS_SSL_CONF_EXTENDED_MASTER_SECRET)
void mbedtls_ssl_conf_extended_master_secret( mbedtls_ssl_config *conf, char ems )
{
    conf->extended_ms = ems;
}
#endif /* !MBEDTLS_SSL_CONF_EXTENDED_MASTER_SECRET */
#if !defined(MBEDTLS_SSL_CONF_ENFORCE_EXTENDED_MASTER_SECRET)
void mbedtls_ssl_conf_extended_master_secret_enforce( mbedtls_ssl_config *conf,
                                                        char ems_enf )
{
    conf->enforce_extended_master_secret = ems_enf;
}
#endif /* !MBEDTLS_SSL_CONF_ENFORCE_EXTENDED_MASTER_SECRET */
#endif /* !MBEDTLS_SSL_EXTENDED_MASTER_SECRET */

#if defined(MBEDTLS_ARC4_C)
void mbedtls_ssl_conf_arc4_support( mbedtls_ssl_config *conf, char arc4 )
{
    conf->arc4_disabled = arc4;
}
#endif

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
int mbedtls_ssl_conf_max_frag_len( mbedtls_ssl_config *conf, unsigned char mfl_code )
{
    if( mfl_code >= MBEDTLS_SSL_MAX_FRAG_LEN_INVALID ||
        ssl_mfl_code_to_length( mfl_code ) > MBEDTLS_TLS_EXT_ADV_CONTENT_LEN )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    conf->mfl_code = mfl_code;

    return( 0 );
}
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
void mbedtls_ssl_conf_truncated_hmac( mbedtls_ssl_config *conf, int truncate )
{
    conf->trunc_hmac = truncate;
}
#endif /* MBEDTLS_SSL_TRUNCATED_HMAC */

#if defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
void mbedtls_ssl_conf_cbc_record_splitting( mbedtls_ssl_config *conf, char split )
{
    conf->cbc_record_splitting = split;
}
#endif

#if !defined(MBEDTLS_SSL_CONF_ALLOW_LEGACY_RENEGOTIATION)
void mbedtls_ssl_conf_legacy_renegotiation( mbedtls_ssl_config *conf, int allow_legacy )
{
    conf->allow_legacy_renegotiation = allow_legacy;
}
#endif /* !MBEDTLS_SSL_CONF_ALLOW_LEGACY_RENEGOTIATION */

#if defined(MBEDTLS_SSL_RENEGOTIATION)
void mbedtls_ssl_conf_renegotiation( mbedtls_ssl_config *conf, int renegotiation )
{
    conf->disable_renegotiation = renegotiation;
}

void mbedtls_ssl_conf_renegotiation_enforced( mbedtls_ssl_config *conf, int max_records )
{
    conf->renego_max_records = max_records;
}

void mbedtls_ssl_conf_renegotiation_period( mbedtls_ssl_config *conf,
                                   const unsigned char period[8] )
{
    mbedtls_platform_memcpy( conf->renego_period, period, 8 );
}
#endif /* MBEDTLS_SSL_RENEGOTIATION */

#if defined(MBEDTLS_SSL_SESSION_TICKETS)
#if defined(MBEDTLS_SSL_CLI_C)
void mbedtls_ssl_conf_session_tickets( mbedtls_ssl_config *conf, int use_tickets )
{
    conf->session_tickets = use_tickets;
}
#endif

#if defined(MBEDTLS_SSL_SRV_C)
void mbedtls_ssl_conf_session_tickets_cb( mbedtls_ssl_config *conf,
        mbedtls_ssl_ticket_write_t *f_ticket_write,
        mbedtls_ssl_ticket_parse_t *f_ticket_parse,
        void *p_ticket )
{
    conf->f_ticket_write = f_ticket_write;
    conf->f_ticket_parse = f_ticket_parse;
    conf->p_ticket       = p_ticket;
}
#endif
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

#if defined(MBEDTLS_SSL_EXPORT_KEYS)
void mbedtls_ssl_conf_export_keys_cb( mbedtls_ssl_config *conf,
        mbedtls_ssl_export_keys_t *f_export_keys,
        void *p_export_keys )
{
    conf->f_export_keys = f_export_keys;
    conf->p_export_keys = p_export_keys;
}
#endif

#if defined(MBEDTLS_SSL_ASYNC_PRIVATE)
void mbedtls_ssl_conf_async_private_cb(
    mbedtls_ssl_config *conf,
    mbedtls_ssl_async_sign_t *f_async_sign,
    mbedtls_ssl_async_decrypt_t *f_async_decrypt,
    mbedtls_ssl_async_resume_t *f_async_resume,
    mbedtls_ssl_async_cancel_t *f_async_cancel,
    void *async_config_data )
{
    conf->f_async_sign_start = f_async_sign;
    conf->f_async_decrypt_start = f_async_decrypt;
    conf->f_async_resume = f_async_resume;
    conf->f_async_cancel = f_async_cancel;
    conf->p_async_config_data = async_config_data;
}

void *mbedtls_ssl_conf_get_async_config_data( const mbedtls_ssl_config *conf )
{
    return( conf->p_async_config_data );
}

void *mbedtls_ssl_get_async_operation_data( const mbedtls_ssl_context *ssl )
{
    if( ssl->handshake == NULL )
        return( NULL );
    else
        return( ssl->handshake->user_async_ctx );
}

void mbedtls_ssl_set_async_operation_data( mbedtls_ssl_context *ssl,
                                 void *ctx )
{
    if( ssl->handshake != NULL )
        ssl->handshake->user_async_ctx = ctx;
}
#endif /* MBEDTLS_SSL_ASYNC_PRIVATE */

/*
 * SSL get accessors
 */
size_t mbedtls_ssl_get_bytes_avail( const mbedtls_ssl_context *ssl )
{
    return( ssl->in_offt == NULL ? 0 : ssl->in_msglen );
}

int mbedtls_ssl_check_pending( const mbedtls_ssl_context *ssl )
{
    /*
     * Case A: We're currently holding back
     * a message for further processing.
     */

    if( ssl->keep_current_message == 1 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "ssl_check_pending: record held back for processing" ) );
        return( 1 );
    }

    /*
     * Case B: Further records are pending in the current datagram.
     */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        ssl->in_left > ssl->next_record_offset )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "ssl_check_pending: more records within current datagram" ) );
        return( 1 );
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    /*
     * Case C: A handshake message is being processed.
     */

    if( ssl->in_hslen > 0 && ssl->in_hslen < ssl->in_msglen )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "ssl_check_pending: more handshake messages within current record" ) );
        return( 1 );
    }

    /*
     * Case D: An application data message is being processed
     */
    if( ssl->in_offt != NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "ssl_check_pending: application data record is being processed" ) );
        return( 1 );
    }

    /*
     * In all other cases, the rest of the message can be dropped.
     * As in ssl_get_next_record, this needs to be adapted if
     * we implement support for multiple alerts in single records.
     */

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ssl_check_pending: nothing pending" ) );
    return( 0 );
}

uint32_t mbedtls_ssl_get_verify_result( const mbedtls_ssl_context *ssl )
{
    if( ssl->session != NULL )
        return( ssl->session->verify_result );

    if( ssl->session_negotiate != NULL )
        return( ssl->session_negotiate->verify_result );

    return( 0xFFFFFFFF );
}

const char *mbedtls_ssl_get_ciphersuite( const mbedtls_ssl_context *ssl )
{
    int suite;

    if( ssl == NULL || ssl->session == NULL )
        return( NULL );

    suite = mbedtls_ssl_session_get_ciphersuite( ssl->session );
    return( mbedtls_ssl_get_ciphersuite_name( suite ) );
}

const char *mbedtls_ssl_get_version( const mbedtls_ssl_context *ssl )
{
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        switch( mbedtls_ssl_get_minor_ver( ssl ) )
        {
            case MBEDTLS_SSL_MINOR_VERSION_2:
                return( "DTLSv1.0" );

            case MBEDTLS_SSL_MINOR_VERSION_3:
                return( "DTLSv1.2" );

            default:
                return( "unknown (DTLS)" );
        }
    }
    MBEDTLS_SSL_TRANSPORT_ELSE
#endif /* MBEDTLS_SSL_PROTO_DTLS */
#if defined(MBEDTLS_SSL_PROTO_TLS)
    {
        switch( mbedtls_ssl_get_minor_ver( ssl ) )
        {
            case MBEDTLS_SSL_MINOR_VERSION_0:
                return( "SSLv3.0" );

            case MBEDTLS_SSL_MINOR_VERSION_1:
                return( "TLSv1.0" );

            case MBEDTLS_SSL_MINOR_VERSION_2:
                return( "TLSv1.1" );

            case MBEDTLS_SSL_MINOR_VERSION_3:
                return( "TLSv1.2" );

            default:
                return( "unknown" );
        }
    }
#endif /* MBEDTLS_SSL_PROTO_TLS */
}

int mbedtls_ssl_get_record_expansion( const mbedtls_ssl_context *ssl )
{
    size_t transform_expansion = 0;
    const mbedtls_ssl_transform *transform = ssl->transform_out;

    size_t out_hdr_len = mbedtls_ssl_out_hdr_len( ssl );

    if( transform == NULL )
        return( (int) out_hdr_len );

#if defined(MBEDTLS_ZLIB_SUPPORT)
    if( ssl->session_out->compression != MBEDTLS_SSL_COMPRESS_NULL )
        return( MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE );
#endif
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
    switch( mbedtls_cipher_get_cipher_mode( &transform->cipher_ctx ) )
#else
    switch( mbedtls_cipher_get_cipher_mode( &transform->cipher_ctx_enc ) )
#endif
    {
#if defined(MBEDTLS_GCM_C)        || \
    defined(MBEDTLS_CCM_C)        || \
    defined(MBEDTLS_CHACHAPOLY_C)
#if defined(MBEDTLS_GCM_C)
        case MBEDTLS_MODE_GCM:
#endif
#if defined(MBEDTLS_CCM_C)
        case MBEDTLS_MODE_CCM:
#endif
#if defined(MBEDTLS_CHACHAPOLY_C)
        case MBEDTLS_MODE_CHACHAPOLY:
#endif
            transform_expansion =
                transform->ivlen - transform->fixed_ivlen + transform->taglen;
            break;

#endif /* MBEDTLS_GCM_C        || MBEDTLS_CCM_C ||
          MBEDTLS_CHACHAPOLY_C */

#if defined(MBEDTLS_CIPHER_MODE_STREAM)
        case MBEDTLS_MODE_STREAM:
            transform_expansion = transform->maclen;
            break;
#endif /* MBEDTLS_CIPHER_MODE_STREAM */

#if defined(MBEDTLS_CIPHER_MODE_CBC)
        case MBEDTLS_MODE_CBC:
        {
            size_t block_size;
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
            block_size = mbedtls_cipher_get_block_size(
                &transform->cipher_ctx );
#else
            block_size = mbedtls_cipher_get_block_size(
                &transform->cipher_ctx_enc );
#endif
            /* Expansion due to the addition of the MAC. */
            transform_expansion += transform->maclen;

            /* Expansion due to the addition of CBC padding;
             * Theoretically up to 256 bytes, but we never use
             * more than the block size of the underlying cipher. */
            transform_expansion += block_size;

            /* For TLS 1.1 or higher, an explicit IV is added
             * after the record header. */
#if defined(MBEDTLS_SSL_PROTO_TLS1_1) || defined(MBEDTLS_SSL_PROTO_TLS1_2)
            if( mbedtls_ssl_ver_geq(
                    mbedtls_ssl_get_minor_ver( ssl ),
                    MBEDTLS_SSL_MINOR_VERSION_2 ) )
            {
                transform_expansion += block_size;
            }
#endif /* MBEDTLS_SSL_PROTO_TLS1_1 || MBEDTLS_SSL_PROTO_TLS1_2 */

            break;
        }
#endif /* MBEDTLS_CIPHER_MODE_CBC */

        default:
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    if( transform->out_cid_len != 0 )
        transform_expansion += MBEDTLS_SSL_MAX_CID_EXPANSION;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

    return( (int)( out_hdr_len + transform_expansion ) );
}

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
size_t mbedtls_ssl_get_input_max_frag_len( const mbedtls_ssl_context *ssl )
{
    size_t max_len = MBEDTLS_SSL_MAX_CONTENT_LEN;
    size_t read_mfl;

    /* Use the configured MFL for the client if we're past SERVER_HELLO_DONE */
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_CLIENT &&
        ssl->state >= MBEDTLS_SSL_SERVER_HELLO_DONE )
    {
        return ssl_mfl_code_to_length( ssl->conf->mfl_code );
    }

    /* Check if a smaller max length was negotiated */
    if( ssl->session_out != NULL )
    {
        read_mfl = ssl_mfl_code_to_length( ssl->session_out->mfl_code );
        if( read_mfl < max_len )
        {
            max_len = read_mfl;
        }
    }

    // During a handshake, use the value being negotiated
    if( ssl->session_negotiate != NULL )
    {
        read_mfl = ssl_mfl_code_to_length( ssl->session_negotiate->mfl_code );
        if( read_mfl < max_len )
        {
            max_len = read_mfl;
        }
    }

    return( max_len );
}

size_t mbedtls_ssl_get_output_max_frag_len( const mbedtls_ssl_context *ssl )
{
    size_t max_len;

    /*
     * Assume mfl_code is correct since it was checked when set
     */
    max_len = ssl_mfl_code_to_length( ssl->conf->mfl_code );

    /* Check if a smaller max length was negotiated */
    if( ssl->session_out != NULL &&
        ssl_mfl_code_to_length( ssl->session_out->mfl_code ) < max_len )
    {
        max_len = ssl_mfl_code_to_length( ssl->session_out->mfl_code );
    }

    /* During a handshake, use the value being negotiated */
    if( ssl->session_negotiate != NULL &&
        ssl_mfl_code_to_length( ssl->session_negotiate->mfl_code ) < max_len )
    {
        max_len = ssl_mfl_code_to_length( ssl->session_negotiate->mfl_code );
    }

    return( max_len );
}

#if !defined(MBEDTLS_DEPRECATED_REMOVED)
size_t mbedtls_ssl_get_max_frag_len( const mbedtls_ssl_context *ssl )
{
    return mbedtls_ssl_get_output_max_frag_len( ssl );
}
#endif /* !MBEDTLS_DEPRECATED_REMOVED */
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
static size_t ssl_get_current_mtu( const mbedtls_ssl_context *ssl )
{
    /* Return unlimited mtu for client hello messages to avoid fragmentation. */
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_CLIENT &&
        ( ssl->state == MBEDTLS_SSL_CLIENT_HELLO ||
          ssl->state == MBEDTLS_SSL_SERVER_HELLO ) )
        return( 0 );

    if( ssl->handshake == NULL || ssl->handshake->mtu == 0 )
        return( ssl->mtu );

    if( ssl->mtu == 0 )
        return( ssl->handshake->mtu );

    return( ssl->mtu < ssl->handshake->mtu ?
            ssl->mtu : ssl->handshake->mtu );
}
#endif /* MBEDTLS_SSL_PROTO_DTLS */

int mbedtls_ssl_get_max_out_record_payload( const mbedtls_ssl_context *ssl )
{
    size_t max_len = MBEDTLS_SSL_OUT_CONTENT_LEN;

#if !defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH) && \
    !defined(MBEDTLS_SSL_PROTO_DTLS)
    (void) ssl;
#endif

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
    const size_t mfl = mbedtls_ssl_get_output_max_frag_len( ssl );

    if( max_len > mfl )
        max_len = mfl;
#endif

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( ssl_get_current_mtu( ssl ) != 0 )
    {
        const size_t mtu = ssl_get_current_mtu( ssl );
        const int ret = mbedtls_ssl_get_record_expansion( ssl );
        const size_t overhead = (size_t) ret;

        if( ret < 0 )
            return( ret );

        if( mtu <= overhead )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "MTU too low for record expansion" ) );
            return( MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE );
        }

        if( max_len > mtu - overhead )
            max_len = mtu - overhead;
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

#if !defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH) &&        \
    !defined(MBEDTLS_SSL_PROTO_DTLS)
    ((void) ssl);
#endif

    return( (int) max_len );
}

#if defined(MBEDTLS_X509_CRT_PARSE_C)
const mbedtls_x509_crt *mbedtls_ssl_get_peer_cert( const mbedtls_ssl_context *ssl )
{
    if( ssl == NULL || ssl->session == NULL )
        return( NULL );

#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    return( ssl->session->peer_cert );
#else
    return( NULL );
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */
}
#endif /* MBEDTLS_X509_CRT_PARSE_C */

#if defined(MBEDTLS_SSL_CLI_C)
int mbedtls_ssl_get_session( const mbedtls_ssl_context *ssl,
                             mbedtls_ssl_session *dst )
{
    if( ssl == NULL ||
        dst == NULL ||
        ssl->session == NULL ||
        mbedtls_ssl_conf_get_endpoint( ssl->conf ) != MBEDTLS_SSL_IS_CLIENT )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    return( mbedtls_ssl_session_copy( dst, ssl->session ) );
}
#endif /* MBEDTLS_SSL_CLI_C */

const mbedtls_ssl_session *mbedtls_ssl_get_session_pointer( const mbedtls_ssl_context *ssl )
{
    if( ssl == NULL )
        return( NULL );

    return( ssl->session );
}

/*
 * Define ticket header determining Mbed TLS version
 * and structure of the ticket.
 */

/*
 * Define bitflag determining compile-time settings influencing
 * structure of serialized SSL sessions.
 */

#if defined(MBEDTLS_HAVE_TIME)
#define SSL_SERIALIZED_SESSION_CONFIG_TIME 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_TIME 0
#endif /* MBEDTLS_HAVE_TIME */

#if defined(MBEDTLS_X509_CRT_PARSE_C)
#define SSL_SERIALIZED_SESSION_CONFIG_CRT 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_CRT 0
#endif /* MBEDTLS_X509_CRT_PARSE_C */

#if defined(MBEDTLS_SSL_CLI_C) && defined(MBEDTLS_SSL_SESSION_TICKETS)
#define SSL_SERIALIZED_SESSION_CONFIG_CLIENT_TICKET 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_CLIENT_TICKET 0
#endif /* MBEDTLS_SSL_CLI_C && MBEDTLS_SSL_SESSION_TICKETS */

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
#define SSL_SERIALIZED_SESSION_CONFIG_MFL 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_MFL 0
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
#define SSL_SERIALIZED_SESSION_CONFIG_TRUNC_HMAC 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_TRUNC_HMAC 0
#endif /* MBEDTLS_SSL_TRUNCATED_HMAC */

#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
#define SSL_SERIALIZED_SESSION_CONFIG_ETM 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_ETM 0
#endif /* MBEDTLS_SSL_ENCRYPT_THEN_MAC */

#if defined(MBEDTLS_SSL_SESSION_TICKETS)
#define SSL_SERIALIZED_SESSION_CONFIG_TICKET 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_TICKET 0
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
#define SSL_SERIALIZED_SESSION_CONFIG_KEEP_CRT 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_KEEP_CRT 0
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */

#if defined(MBEDTLS_ZLIB_SUPPORT)
#define SSL_SERIALIZED_SESSION_CONFIG_COMPRESSION 1
#else
#define SSL_SERIALIZED_SESSION_CONFIG_COMPRESSION 0
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */

#define SSL_SERIALIZED_SESSION_CONFIG_TIME_BIT          0
#define SSL_SERIALIZED_SESSION_CONFIG_CRT_BIT           1
#define SSL_SERIALIZED_SESSION_CONFIG_CLIENT_TICKET_BIT 2
#define SSL_SERIALIZED_SESSION_CONFIG_MFL_BIT           3
#define SSL_SERIALIZED_SESSION_CONFIG_TRUNC_HMAC_BIT    4
#define SSL_SERIALIZED_SESSION_CONFIG_ETM_BIT           5
#define SSL_SERIALIZED_SESSION_CONFIG_TICKET_BIT        6
#define SSL_SERIALIZED_SESSION_CONFIG_KEEP_CRT_BIT      7
#define SSL_SERIALIZED_SESSION_CONFIG_COMPRESSION_BIT   8

#define SSL_SERIALIZED_SESSION_CONFIG_BITFLAG                           \
    ( (uint16_t) (                                                      \
        ( SSL_SERIALIZED_SESSION_CONFIG_TIME          << SSL_SERIALIZED_SESSION_CONFIG_TIME_BIT          ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_CRT           << SSL_SERIALIZED_SESSION_CONFIG_CRT_BIT           ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_CLIENT_TICKET << SSL_SERIALIZED_SESSION_CONFIG_CLIENT_TICKET_BIT ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_MFL           << SSL_SERIALIZED_SESSION_CONFIG_MFL_BIT           ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_TRUNC_HMAC    << SSL_SERIALIZED_SESSION_CONFIG_TRUNC_HMAC_BIT    ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_ETM           << SSL_SERIALIZED_SESSION_CONFIG_ETM_BIT           ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_TICKET        << SSL_SERIALIZED_SESSION_CONFIG_TICKET_BIT        ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_COMPRESSION   << SSL_SERIALIZED_SESSION_CONFIG_COMPRESSION_BIT   ) | \
        ( SSL_SERIALIZED_SESSION_CONFIG_KEEP_CRT      << SSL_SERIALIZED_SESSION_CONFIG_KEEP_CRT_BIT      ) ) )

static unsigned char ssl_serialized_session_header[] = {
    MBEDTLS_VERSION_MAJOR,
    MBEDTLS_VERSION_MINOR,
    MBEDTLS_VERSION_PATCH,
    ( SSL_SERIALIZED_SESSION_CONFIG_BITFLAG >> 8 ) & 0xFF,
    ( SSL_SERIALIZED_SESSION_CONFIG_BITFLAG >> 0 ) & 0xFF,
};

/*
 * Serialize a session in the following format:
 * (in the presentation language of TLS, RFC 8446 section 3)
 *
 *  opaque mbedtls_version[3];   // major, minor, patch
 *  opaque session_format[2];    // version-specific 16-bit field determining
 *                               // the format of the remaining
 *                               // serialized data.
 *
 *  Note: When updating the format, remember to keep
 *        these version+format bytes.
 *
 *                               // In this version, `session_format` determines
 *                               // the setting of those compile-time
 *                               // configuration options which influence
 *                               // the structure of mbedtls_ssl_session.
 *  uint64 start_time;
 *  uint8 ciphersuite[2];        // defined by the standard
 *  uint8 compression;           // 0 or 1
 *  uint8 session_id_len;        // at most 32
 *  opaque session_id[32];
 *  opaque master[48];           // fixed length in the standard
 *  uint32 verify_result;
 *  select (MBEDTLS_SSL_KEEP_PEER_CERTIFICATE) {
 *      case enabled:  opaque peer_cert<0..2^24-1>; // length 0 means no cert
 *      case disabled: uint8_t peer_cert_digest_type;
 *                     opaque peer_cert_digest<0..2^8-1>;
 *  }
 *  opaque ticket<0..2^24-1>;    // length 0 means no ticket
 *  uint32 ticket_lifetime;
 *  uint8 mfl_code;              // up to 255 according to standard
 *  uint8 trunc_hmac;            // 0 or 1
 *  uint8 encrypt_then_mac;      // 0 or 1
 *
 * The order is the same as in the definition of the structure, except
 * verify_result is put before peer_cert so that all mandatory fields come
 * together in one block.
 */
static int ssl_session_save( const mbedtls_ssl_session *session,
                             unsigned char omit_header,
                             unsigned char *buf,
                             size_t buf_len,
                             size_t *olen )
{
    unsigned char *p = buf;
    size_t used = 0;
#if defined(MBEDTLS_HAVE_TIME)
    uint64_t start;
#endif
#if defined(MBEDTLS_X509_CRT_PARSE_C)
#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    size_t cert_len;
#endif
#endif

    if( !omit_header )
    {
        /*
         * Add version identifier
         */

        used += sizeof( ssl_serialized_session_header );

        if( used <= buf_len )
        {
            mbedtls_platform_memcpy( p, ssl_serialized_session_header,
                    sizeof( ssl_serialized_session_header ) );
            p += sizeof( ssl_serialized_session_header );
        }
    }

    /*
     * Time
     */
#if defined(MBEDTLS_HAVE_TIME)
    used += 8;

    if( used <= buf_len )
    {
        start = (uint64_t) session->start;

        *p++ = (unsigned char)( ( start >> 56 ) & 0xFF );
        *p++ = (unsigned char)( ( start >> 48 ) & 0xFF );
        *p++ = (unsigned char)( ( start >> 40 ) & 0xFF );
        *p++ = (unsigned char)( ( start >> 32 ) & 0xFF );
        *p++ = (unsigned char)( ( start >> 24 ) & 0xFF );
        *p++ = (unsigned char)( ( start >> 16 ) & 0xFF );
        *p++ = (unsigned char)( ( start >>  8 ) & 0xFF );
        *p++ = (unsigned char)( ( start       ) & 0xFF );
    }
#endif /* MBEDTLS_HAVE_TIME */

    /*
     * Basic mandatory fields
     */
    {
        size_t const ciphersuite_len = 2;
#if defined(MBEDTLS_ZLIB_SUPPORT)
        size_t const compression_len = 1;
#else
        size_t const compression_len = 0;
#endif
        size_t const id_len_len = 1;
        size_t const id_len = 32;
        size_t const master_len = 48;
        size_t const verif_result_len = 4;

        size_t const basic_len =
            ciphersuite_len +
            compression_len +
            id_len_len      +
            id_len          +
            master_len      +
            verif_result_len;

        used += basic_len;
    }

    if( used <= buf_len )
    {
        const int ciphersuite =
            mbedtls_ssl_session_get_ciphersuite( session );
        p = mbedtls_platform_put_uint16_be( p, ciphersuite );

#if defined(MBEDTLS_ZLIB_SUPPORT)
        *p++ = (unsigned char)(
            mbedtls_ssl_session_get_compression( session ) );
#endif

        *p++ = (unsigned char)( session->id_len & 0xFF );
        /* Not using more secure mbedtls_platform_memcpy as session id is public */
        memcpy( p, session->id, 32 );
        p += 32;

        mbedtls_platform_memcpy( p, session->master, 48 );
        p += 48;
        p = mbedtls_platform_put_uint32_be( p, session->verify_result );
    }

    /*
     * Peer's end-entity certificate
     */
#if defined(MBEDTLS_X509_CRT_PARSE_C)
#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    if( session->peer_cert == NULL )
        cert_len = 0;
    else
        cert_len = session->peer_cert->raw.len;

    used += 3 + cert_len;

    if( used <= buf_len )
    {
        p = mbedtls_platform_put_uint24_be( p, cert_len );

        if( session->peer_cert != NULL )
        {
            mbedtls_platform_memcpy( p, session->peer_cert->raw.p, cert_len );
            p += cert_len;
        }
    }

#elif defined(MBEDTLS_SSL_RENEGOTIATION)
    /* Digest of peer certificate */
    if( session->peer_cert_digest != NULL )
    {
        used += 1 /* type */ + 1 /* length */ + session->peer_cert_digest_len;
        if( used <= buf_len )
        {
            *p++ = (unsigned char) session->peer_cert_digest_type;
            *p++ = (unsigned char) session->peer_cert_digest_len;
            mbedtls_platform_memcpy( p, session->peer_cert_digest,
                    session->peer_cert_digest_len );
            p += session->peer_cert_digest_len;
        }
    }
    else
    {
        used += 2;
        if( used <= buf_len )
        {
            *p++ = (unsigned char) MBEDTLS_MD_NONE;
            *p++ = 0;
        }
    }
#endif /* !MBEDTLS_SSL_KEEP_PEER_CERTIFICATE && MBEDTLS_SSL_RENEGOTIATION */
#endif /* MBEDTLS_X509_CRT_PARSE_C */

    /*
     * Session ticket if any, plus associated data
     */
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
    used += 3 + session->ticket_len + 4; /* len + ticket + lifetime */

    if( used <= buf_len )
    {
        p = mbedtls_platform_put_uint24_be( p, session->ticket_len );

        if( session->ticket != NULL )
        {
            mbedtls_platform_memcpy( p, session->ticket, session->ticket_len );
            p += session->ticket_len;
        }

        p = mbedtls_platform_put_uint32_be( p, session->ticket_lifetime );
    }
#endif /* MBEDTLS_SSL_SESSION_TICKETS && MBEDTLS_SSL_CLI_C */

    /*
     * Misc extension-related info
     */
#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
    used += 1;

    if( used <= buf_len )
        *p++ = session->mfl_code;
#endif

#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
    used += 1;

    if( used <= buf_len )
        *p++ = (unsigned char)( ( session->trunc_hmac ) & 0xFF );
#endif

#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
    used += 1;

    if( used <= buf_len )
        *p++ = (unsigned char)( ( session->encrypt_then_mac ) & 0xFF );
#endif

    /* Done */
    *olen = used;

    if( used > buf_len )
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );

    return( 0 );
}

/*
 * Public wrapper for ssl_session_save()
 */
int mbedtls_ssl_session_save( const mbedtls_ssl_session *session,
                              unsigned char *buf,
                              size_t buf_len,
                              size_t *olen )
{
    return( ssl_session_save( session, 0, buf, buf_len, olen ) );
}

/*
 * Deserialize session, see mbedtls_ssl_session_save() for format.
 *
 * This internal version is wrapped by a public function that cleans up in
 * case of error, and has an extra option omit_header.
 */
static int ssl_session_load( mbedtls_ssl_session *session,
                             unsigned char omit_header,
                             const unsigned char *buf,
                             size_t len )
{
    const unsigned char *p = buf;
    const unsigned char * const end = buf + len;
    int ciphersuite;
#if defined(MBEDTLS_HAVE_TIME)
    uint64_t start;
#endif
#if defined(MBEDTLS_X509_CRT_PARSE_C)
#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    size_t cert_len;
#endif
#endif

    if( !omit_header )
    {
        /*
         * Check version identifier
         */

        if( (size_t)( end - p ) < sizeof( ssl_serialized_session_header ) )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        // use regular memcmp as session header is public data
        if( memcmp( p, ssl_serialized_session_header,
                    sizeof( ssl_serialized_session_header ) ) != 0 )
        {
            return( MBEDTLS_ERR_SSL_VERSION_MISMATCH );
        }
        p += sizeof( ssl_serialized_session_header );
    }

    /*
     * Time
     */
#if defined(MBEDTLS_HAVE_TIME)
    if( 8 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    start = ( (uint64_t) p[0] << 56 ) |
            ( (uint64_t) p[1] << 48 ) |
            ( (uint64_t) p[2] << 40 ) |
            ( (uint64_t) p[3] << 32 ) |
            ( (uint64_t) p[4] << 24 ) |
            ( (uint64_t) p[5] << 16 ) |
            ( (uint64_t) p[6] <<  8 ) |
            ( (uint64_t) p[7]       );
    p += 8;

    session->start = (time_t) start;
#endif /* MBEDTLS_HAVE_TIME */

    /*
     * Basic mandatory fields
     */
    {
        size_t const ciphersuite_len = 2;
#if defined(MBEDTLS_ZLIB_SUPPORT)
        size_t const compression_len = 1;
#else
        size_t const compression_len = 0;
#endif
        size_t const id_len_len = 1;
        size_t const id_len = 32;
        size_t const master_len = 48;
        size_t const verif_result_len = 4;

        size_t const basic_len =
            ciphersuite_len +
            compression_len +
            id_len_len      +
            id_len          +
            master_len      +
            verif_result_len;

        if( basic_len > (size_t)( end - p ) )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    ciphersuite = (int)mbedtls_platform_get_uint16_be( p );
    p += 2;

#if !defined(MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE)
    session->ciphersuite = ciphersuite;
#else
    if( ciphersuite !=
        MBEDTLS_SSL_SUITE_ID( MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE ) )
    {
        return( MBEDTLS_ERR_SSL_VERSION_MISMATCH );
    }
#endif

#if defined(MBEDTLS_ZLIB_SUPPORT)
    session->compression = *p++;
#endif

    session->id_len = *p++;
    /* Not using more secure mbedtls_platform_memcpy as session id is public */
    memcpy( session->id, p, 32 );
    p += 32;

    mbedtls_platform_memcpy( session->master, p, 48 );
    p += 48;

    session->verify_result = (uint32_t)mbedtls_platform_get_uint32_be( p );
    p += 4;

    /* Immediately clear invalid pointer values that have been read, in case
     * we exit early before we replaced them with valid ones. */
#if defined(MBEDTLS_X509_CRT_PARSE_C)
#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    session->peer_cert = NULL;
#elif defined(MBEDTLS_SSL_RENEGOTIATION)
    session->peer_cert_digest = NULL;
#endif /* !MBEDTLS_SSL_KEEP_PEER_CERTIFICATE && MBEDTLS_SSL_RENEGOTIATION */
#endif
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
    session->ticket = NULL;
#endif

    /*
     * Peer certificate
     */
#if defined(MBEDTLS_X509_CRT_PARSE_C)
#if defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    if( 3 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    cert_len = mbedtls_platform_get_uint24_be( &p[0] );

    p += 3;

    if( cert_len == 0 )
    {
        session->peer_cert = NULL;
    }
    else
    {
        int ret;

        if( cert_len > (size_t)( end - p ) )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        session->peer_cert = mbedtls_calloc( 1, sizeof( mbedtls_x509_crt ) );

        if( session->peer_cert == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        mbedtls_x509_crt_init( session->peer_cert );

        if( ( ret = mbedtls_x509_crt_parse_der( session->peer_cert,
                                                p, cert_len ) ) != 0 )
        {
            mbedtls_x509_crt_free( session->peer_cert );
            mbedtls_free( session->peer_cert );
            session->peer_cert = NULL;
            return( ret );
        }

        p += cert_len;
    }
#elif defined(MBEDTLS_SSL_RENEGOTIATION)
    /* Deserialize CRT digest from the end of the ticket. */
    if( 2 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    session->peer_cert_digest_type = (mbedtls_md_type_t) *p++;
    session->peer_cert_digest_len  = (size_t) *p++;

    if( session->peer_cert_digest_len != 0 )
    {
        mbedtls_md_handle_t md_info =
            mbedtls_md_info_from_type( session->peer_cert_digest_type );
        if( md_info == MBEDTLS_MD_INVALID_HANDLE )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
        if( session->peer_cert_digest_len != mbedtls_md_get_size( md_info ) )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        if( session->peer_cert_digest_len > (size_t)( end - p ) )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        session->peer_cert_digest =
            mbedtls_calloc( 1, session->peer_cert_digest_len );
        if( session->peer_cert_digest == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        mbedtls_platform_memcpy( session->peer_cert_digest, p,
                session->peer_cert_digest_len );
        p += session->peer_cert_digest_len;
    }
#endif /* MBEDTLS_SSL_KEEP_PEER_CERTIFICATE && MBEDTLS_SSL_RENEGOTIATION */
#endif /* MBEDTLS_X509_CRT_PARSE_C */

    /*
     * Session ticket and associated data
     */
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
    if( 3 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    session->ticket_len = mbedtls_platform_get_uint24_be( &p[0] );
    p += 3;

    if( session->ticket_len != 0 )
    {
        if( session->ticket_len > (size_t)( end - p ) )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        session->ticket = mbedtls_calloc( 1, session->ticket_len );
        if( session->ticket == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        mbedtls_platform_memcpy( session->ticket, p, session->ticket_len );
        p += session->ticket_len;
    }

    if( 4 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    session->ticket_lifetime = (uint32_t)mbedtls_platform_get_uint32_be( p );
    p += 4;
#endif /* MBEDTLS_SSL_SESSION_TICKETS && MBEDTLS_SSL_CLI_C */

    /*
     * Misc extension-related info
     */
#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
    if( 1 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    session->mfl_code = *p++;
#endif

#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
    if( 1 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    session->trunc_hmac = *p++;
#endif

#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
    if( 1 > (size_t)( end - p ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    session->encrypt_then_mac = *p++;
#endif

    /* Done, should have consumed entire buffer */
    if( p != end )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    return( 0 );
}

/*
 * Deserialize session: public wrapper for error cleaning
 */
int mbedtls_ssl_session_load( mbedtls_ssl_session *session,
                              const unsigned char *buf,
                              size_t len )
{
    int ret = ssl_session_load( session, 0, buf, len );

    if( ret != 0 )
        mbedtls_ssl_session_free( session );

    return( ret );
}

/*
 * Perform a single step of the SSL handshake
 */
int mbedtls_ssl_handshake_step( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;

    if( ssl == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

#if defined(MBEDTLS_SSL_CLI_C)
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_CLIENT )
        ret = mbedtls_ssl_handshake_client_step( ssl );
#endif
#if defined(MBEDTLS_SSL_SRV_C)
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_SERVER )
        ret = mbedtls_ssl_handshake_server_step( ssl );
#endif

    ssl_send_pending_fatal_alert( ssl );
    return( ret );
}

/*
 * Perform the SSL handshake
 */
int mbedtls_ssl_handshake( mbedtls_ssl_context *ssl )
{
    int ret = 0;

    if( ssl == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> handshake" ) );

    while( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
    {
        ret = mbedtls_ssl_handshake_step( ssl );

        if( ret != 0 )
            break;
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= handshake" ) );

    return( ret );
}

#if defined(MBEDTLS_SSL_RENEGOTIATION)
#if defined(MBEDTLS_SSL_SRV_C)
/*
 * Write HelloRequest to request renegotiation on server
 */
static int ssl_write_hello_request( mbedtls_ssl_context *ssl )
{
    int ret;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write hello request" ) );

    ssl->out_msglen  = 4;
    ssl->out_msgtype = MBEDTLS_SSL_MSG_HANDSHAKE;
    ssl->out_msg[0]  = MBEDTLS_SSL_HS_HELLO_REQUEST;

    if( ( ret = mbedtls_ssl_write_handshake_msg( ssl ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_write_handshake_msg", ret );
        return( ret );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write hello request" ) );

    return( 0 );
}
#endif /* MBEDTLS_SSL_SRV_C */

/*
 * Actually renegotiate current connection, triggered by either:
 * - any side: calling mbedtls_ssl_renegotiate(),
 * - client: receiving a HelloRequest during mbedtls_ssl_read(),
 * - server: receiving any handshake message on server during mbedtls_ssl_read() after
 *   the initial handshake is completed.
 * If the handshake doesn't complete due to waiting for I/O, it will continue
 * during the next calls to mbedtls_ssl_renegotiate() or mbedtls_ssl_read() respectively.
 */
static int ssl_start_renegotiation( mbedtls_ssl_context *ssl )
{
    int ret;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> renegotiate" ) );

    if( ( ret = ssl_handshake_init( ssl ) ) != 0 )
        return( ret );

    /* RFC 6347 4.2.2: "[...] the HelloRequest will have message_seq = 0 and
     * the ServerHello will have message_seq = 1" */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
        ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_PENDING )
    {
        if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
              MBEDTLS_SSL_IS_SERVER )
        {
            ssl->handshake->out_msg_seq = 1;
        }
        else
        {
            ssl->handshake->in_msg_seq = 1;
        }
    }
#endif

    ssl->state = MBEDTLS_SSL_HELLO_REQUEST;
    ssl->renego_status = MBEDTLS_SSL_RENEGOTIATION_IN_PROGRESS;

    if( ( ret = mbedtls_ssl_handshake( ssl ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_handshake", ret );
        return( ret );
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= renegotiate" ) );

    return( 0 );
}

/*
 * Renegotiate current connection on client,
 * or request renegotiation on server
 */
int mbedtls_ssl_renegotiate( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;

    if( ssl == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

#if defined(MBEDTLS_SSL_SRV_C)
    /* On server, just send the request */
    if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) == MBEDTLS_SSL_IS_SERVER )
    {
        if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        ssl->renego_status = MBEDTLS_SSL_RENEGOTIATION_PENDING;

        /* Did we already try/start sending HelloRequest? */
        if( ssl->out_left != 0 )
            return( mbedtls_ssl_flush_output( ssl ) );

        return( ssl_write_hello_request( ssl ) );
    }
#endif /* MBEDTLS_SSL_SRV_C */

#if defined(MBEDTLS_SSL_CLI_C)
    /*
     * On client, either start the renegotiation process or,
     * if already in progress, continue the handshake
     */
    if( ssl->renego_status != MBEDTLS_SSL_RENEGOTIATION_IN_PROGRESS )
    {
        if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        if( ( ret = ssl_start_renegotiation( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_start_renegotiation", ret );
            return( ret );
        }
    }
    else
    {
        if( ( ret = mbedtls_ssl_handshake( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_handshake", ret );
            return( ret );
        }
    }
#endif /* MBEDTLS_SSL_CLI_C */

    return( ret );
}

/*
 * Check record counters and renegotiate if they're above the limit.
 */
static int ssl_check_ctr_renegotiate( mbedtls_ssl_context *ssl )
{
    size_t ep_len = ssl_ep_len( ssl );
    int in_ctr_cmp;
    int out_ctr_cmp;

    if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER ||
        ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_PENDING ||
        ! mbedtls_ssl_conf_is_renegotiation_enabled( ssl->conf ) )
    {
        return( 0 );
    }

    // use regular memcmp as counters are public data
    in_ctr_cmp = memcmp( ssl->in_ctr + ep_len,
                        ssl->conf->renego_period + ep_len, 8 - ep_len );
    out_ctr_cmp = memcmp( ssl->cur_out_ctr + ep_len,
                          ssl->conf->renego_period + ep_len, 8 - ep_len );

    if( in_ctr_cmp <= 0 && out_ctr_cmp <= 0 )
    {
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "record counter limit reached: renegotiate" ) );
    return( mbedtls_ssl_renegotiate( ssl ) );
}
#endif /* MBEDTLS_SSL_RENEGOTIATION */

/*
 * Receive application data decrypted from the SSL layer
 */
int mbedtls_ssl_read( mbedtls_ssl_context *ssl, unsigned char *buf, size_t len )
{
    int ret;
    size_t n;
    volatile unsigned char *buf_dup = buf;
    volatile size_t len_dup = len;

    if( ssl == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> read" ) );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
    {
        if( ( ret = mbedtls_ssl_flush_output( ssl ) ) != 0 )
            return( ret );

        if( ssl->handshake != NULL &&
            ssl->handshake->retransmit_state == MBEDTLS_SSL_RETRANS_SENDING )
        {
            if( ( ret = mbedtls_ssl_flight_transmit( ssl ) ) != 0 )
                return( ret );
        }
    }
#endif

    /*
     * Check if renegotiation is necessary and/or handshake is
     * in process. If yes, perform/continue, and fall through
     * if an unexpected packet is received while the client
     * is waiting for the ServerHello.
     *
     * (There is no equivalent to the last condition on
     *  the server-side as it is not treated as within
     *  a handshake while waiting for the ClientHello
     *  after a renegotiation request.)
     */

#if defined(MBEDTLS_SSL_RENEGOTIATION)
    ret = ssl_check_ctr_renegotiate( ssl );
    if( ret != MBEDTLS_ERR_SSL_WAITING_SERVER_HELLO_RENEGO &&
        ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_check_ctr_renegotiate", ret );
        return( ret );
    }
#endif

    if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
    {
        ret = mbedtls_ssl_handshake( ssl );
        if( ret != MBEDTLS_ERR_SSL_WAITING_SERVER_HELLO_RENEGO &&
            ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_handshake", ret );
            return( ret );
        }
    }

    /* Loop as long as no application data record is available */
    while( ssl->in_offt == NULL )
    {
        /* Start timer if not already running */
        if( mbedtls_ssl_get_get_timer( ssl ) != NULL &&
            mbedtls_ssl_get_get_timer( ssl )( ssl->p_timer ) == -1 )
        {
            ssl_set_timer( ssl,
                           mbedtls_ssl_conf_get_read_timeout( ssl->conf ) );
        }

        if( ( ret = mbedtls_ssl_read_record( ssl, 1 ) ) != 0 )
        {
            if( ret == MBEDTLS_ERR_SSL_CONN_EOF )
                return( 0 );

            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
            return( ret );
        }

        if( ssl->in_msglen  == 0 &&
            ssl->in_msgtype == MBEDTLS_SSL_MSG_APPLICATION_DATA )
        {
            /*
             * OpenSSL sends empty messages to randomize the IV
             */
            if( ( ret = mbedtls_ssl_read_record( ssl, 1 ) ) != 0 )
            {
                if( ret == MBEDTLS_ERR_SSL_CONN_EOF )
                    return( 0 );

                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
                return( ret );
            }
        }

        if( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "received handshake message" ) );

            /*
             * - For client-side, expect SERVER_HELLO_REQUEST.
             * - For server-side, expect CLIENT_HELLO.
             * - Fail (TLS) or silently drop record (DTLS) in other cases.
             */

#if defined(MBEDTLS_SSL_CLI_C)
            if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
                  MBEDTLS_SSL_IS_CLIENT &&
                ( ssl->in_msg[0] != MBEDTLS_SSL_HS_HELLO_REQUEST ||
                  ssl->in_hslen  != mbedtls_ssl_hs_hdr_len( ssl ) ) )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "handshake received (not HelloRequest)" ) );

                /* With DTLS, drop the packet (probably from last handshake) */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
                if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
                {
                    continue;
                }
                MBEDTLS_SSL_TRANSPORT_ELSE
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS)
                {
                    return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
                }
#endif
            }
#endif /* MBEDTLS_SSL_CLI_C */

#if defined(MBEDTLS_SSL_SRV_C)
            if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
                  MBEDTLS_SSL_IS_SERVER &&
                ssl->in_msg[0] != MBEDTLS_SSL_HS_CLIENT_HELLO )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "handshake received (not ClientHello)" ) );

                /* With DTLS, drop the packet (probably from last handshake) */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
                if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
                {
                    continue;
                }
                MBEDTLS_SSL_TRANSPORT_ELSE
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS)
                {
                    return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
                }
#endif
            }
#endif /* MBEDTLS_SSL_SRV_C */

#if defined(MBEDTLS_SSL_RENEGOTIATION)
            /* Determine whether renegotiation attempt should be accepted */
            if( ! ( ssl->conf->disable_renegotiation == MBEDTLS_SSL_RENEGOTIATION_DISABLED ||
                    ( ssl->secure_renegotiation == MBEDTLS_SSL_LEGACY_RENEGOTIATION &&
                      mbedtls_ssl_conf_get_allow_legacy_renegotiation( ssl->conf ) ==
                                                   MBEDTLS_SSL_LEGACY_NO_RENEGOTIATION ) ) )
            {
                /*
                 * Accept renegotiation request
                 */

                /* DTLS clients need to know renego is server-initiated */
#if defined(MBEDTLS_SSL_PROTO_DTLS)
                if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) &&
                    mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
                      MBEDTLS_SSL_IS_CLIENT )
                {
                    ssl->renego_status = MBEDTLS_SSL_RENEGOTIATION_PENDING;
                }
#endif
                ret = ssl_start_renegotiation( ssl );
                if( ret != MBEDTLS_ERR_SSL_WAITING_SERVER_HELLO_RENEGO &&
                    ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "ssl_start_renegotiation", ret );
                    return( ret );
                }
            }
            else
#endif /* MBEDTLS_SSL_RENEGOTIATION */
            {
                /*
                 * Refuse renegotiation
                 */

                MBEDTLS_SSL_DEBUG_MSG( 3, ( "refusing renegotiation, sending alert" ) );

#if defined(MBEDTLS_SSL_PROTO_SSL3)
                if( mbedtls_ssl_get_minor_ver( ssl ) == MBEDTLS_SSL_MINOR_VERSION_0 )
                {
                    /* SSLv3 does not have a "no_renegotiation" warning, so
                       we send a fatal alert and abort the connection. */
                    mbedtls_ssl_send_alert_message( ssl, MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                                                    MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE );
                    return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
                }
                else
#endif /* MBEDTLS_SSL_PROTO_SSL3 */
#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_2)
                if( mbedtls_ssl_ver_geq(
                        mbedtls_ssl_get_minor_ver( ssl ),
                        MBEDTLS_SSL_MINOR_VERSION_1 ) )
                {
                    ret = mbedtls_ssl_send_alert_message( ssl,
                                             MBEDTLS_SSL_ALERT_LEVEL_WARNING,
                                             MBEDTLS_SSL_ALERT_MSG_NO_RENEGOTIATION );
                    if( ret != 0 )
                        return( ret );
                }
                else
#endif /* MBEDTLS_SSL_PROTO_TLS1 || MBEDTLS_SSL_PROTO_TLS1_1 ||
          MBEDTLS_SSL_PROTO_TLS1_2 */
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
                    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
                }
            }

            /* At this point, we don't know whether the renegotiation has been
             * completed or not. The cases to consider are the following:
             * 1) The renegotiation is complete. In this case, no new record
             *    has been read yet.
             * 2) The renegotiation is incomplete because the client received
             *    an application data record while awaiting the ServerHello.
             * 3) The renegotiation is incomplete because the client received
             *    a non-handshake, non-application data message while awaiting
             *    the ServerHello.
             * In each of these case, looping will be the proper action:
             * - For 1), the next iteration will read a new record and check
             *   if it's application data.
             * - For 2), the loop condition isn't satisfied as application data
             *   is present, hence continue is the same as break
             * - For 3), the loop condition is satisfied and read_record
             *   will re-deliver the message that was held back by the client
             *   when expecting the ServerHello.
             */
            continue;
        }
#if defined(MBEDTLS_SSL_RENEGOTIATION)
        else if( ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_PENDING )
        {
            if( ssl->conf->renego_max_records >= 0 )
            {
                if( ++ssl->renego_records_seen > ssl->conf->renego_max_records )
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "renegotiation requested, "
                                        "but not honored by client" ) );
                    return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
                }
            }
        }
#endif /* MBEDTLS_SSL_RENEGOTIATION */

        /* Fatal and closure alerts handled by mbedtls_ssl_read_record() */
        if( ssl->in_msgtype == MBEDTLS_SSL_MSG_ALERT )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "ignoring non-fatal non-closure alert" ) );
            return( MBEDTLS_ERR_SSL_WANT_READ );
        }

        if( ssl->in_msgtype != MBEDTLS_SSL_MSG_APPLICATION_DATA )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad application data message" ) );
            return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
        }

        ssl->in_offt = ssl->in_msg;

        /* We're going to return something now, cancel timer,
         * except if handshake (renegotiation) is in progress */
        if( ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER )
            ssl_set_timer( ssl, 0 );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
        /* If we requested renego but received AppData, resend HelloRequest.
         * Do it now, after setting in_offt, to avoid taking this branch
         * again if ssl_write_hello_request() returns WANT_WRITE */
#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_RENEGOTIATION)
        if( mbedtls_ssl_conf_get_endpoint( ssl->conf ) ==
              MBEDTLS_SSL_IS_SERVER &&
            ssl->renego_status == MBEDTLS_SSL_RENEGOTIATION_PENDING )
        {
            if( ( ret = ssl_resend_hello_request( ssl ) ) != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "ssl_resend_hello_request", ret );
                return( ret );
            }
        }
#endif /* MBEDTLS_SSL_SRV_C && MBEDTLS_SSL_RENEGOTIATION */
#endif /* MBEDTLS_SSL_PROTO_DTLS */
    }

    n = ( len < ssl->in_msglen )
        ? len : ssl->in_msglen;

    if( mbedtls_platform_memcpy( buf, ssl->in_offt, n ) !=
                                 buf )
    {
        return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
    }
    ssl->in_msglen -= n;

    // clear incoming data after it's copied to buffer
    if( mbedtls_platform_memset( ssl->in_offt, 0, n ) !=
                                 ssl->in_offt )
    {
        return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
    }

    if( ssl->in_msglen == 0 )
    {
        /* all bytes consumed */
        ssl->in_offt = NULL;
        ssl->keep_current_message = 0;
    }
    else
    {
        /* more data available */
        ssl->in_offt += n;
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= read" ) );

    /* Secure against buffer substitution */
    if( buf_dup == buf && len_dup == len )
    {
        return( (int) n );
    }
    return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
}

/*
 * Send application data to be encrypted by the SSL layer, taking care of max
 * fragment length and buffer size.
 *
 * According to RFC 5246 Section 6.2.1:
 *
 *      Zero-length fragments of Application data MAY be sent as they are
 *      potentially useful as a traffic analysis countermeasure.
 *
 * Therefore, it is possible that the input message length is 0 and the
 * corresponding return code is 0 on success.
 */
static int ssl_write_real( mbedtls_ssl_context *ssl,
                           const unsigned char *buf, size_t len )
{
    volatile int ret = mbedtls_ssl_get_max_out_record_payload( ssl );
    const size_t max_len = (size_t) ret;

    if( ret < 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_get_max_out_record_payload", ret );
        return( ret );
    }

    if( len > max_len )
    {
#if defined(MBEDTLS_SSL_PROTO_DTLS)
        if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( ssl->conf->transport ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "fragment larger than the (negotiated) "
                                "maximum fragment length: %d > %d",
                                len, max_len ) );
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
        }
        MBEDTLS_SSL_TRANSPORT_ELSE
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS)
        {
            len = max_len;
        }
#endif
    }

    if( ssl->out_left != 0 )
    {
        /*
         * The user has previously tried to send the data and
         * MBEDTLS_ERR_SSL_WANT_WRITE or the message was only partially
         * written. In this case, we expect the high-level write function
         * (e.g. mbedtls_ssl_write()) to be called with the same parameters
         */
        if( ( ret = mbedtls_ssl_flush_output( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_flush_output", ret );
            return( ret );
        }
    }
    else
    {
        /*
         * The user is trying to send a message the first time, so we need to
         * copy the data into the internal buffers and setup the data structure
         * to keep track of partial writes
         */
        ssl->out_msglen = len;
        ssl->out_msgtype = MBEDTLS_SSL_MSG_APPLICATION_DATA;
        if( mbedtls_platform_memcpy( ssl->out_msg, buf, len ) != ssl->out_msg )
        {
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        }

#if defined(MBEDTLS_FI_COUNTERMEASURES) && !defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
        /*
         * Buffer pointer and size duplication cannot be supported with MBEDTLS_SSL_CBC_RECORD_SPLITTING.
         * After splitting pointers and data size will not be the same as initaly provides by user.
         */
        /* Secure against buffer substitution */
        if( buf == ssl->out_msg_dup &&
            ssl->out_msglen == ssl->out_msglen_dup &&
            ssl->out_msg_dup[0] == ssl->out_msg[0] )
        {/*write record only if data was copied from correct user pointer */
#endif
            if( ( ret = mbedtls_ssl_write_record( ssl, SSL_FORCE_FLUSH ) ) != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_write_record", ret );
                return( ret );
            }

#if defined(MBEDTLS_FI_COUNTERMEASURES) && !defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
        }
        else
        {
            return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
        }
#endif
    }
    if ( ret == 0 )
    {
        return( (int) len );
    }
    else
    {
        return( MBEDTLS_ERR_PLATFORM_FAULT_DETECTED );
    }

}

/*
 * Write application data, doing 1/n-1 splitting if necessary.
 *
 * With non-blocking I/O, ssl_write_real() may return WANT_WRITE,
 * then the caller will call us again with the same arguments, so
 * remember whether we already did the split or not.
 */
#if defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
static int ssl_write_split( mbedtls_ssl_context *ssl,
                            const unsigned char *buf, size_t len )
{
    int ret;

    if( ssl->conf->cbc_record_splitting ==
            MBEDTLS_SSL_CBC_RECORD_SPLITTING_DISABLED ||
        len <= 1 ||
        mbedtls_ssl_ver_gt(
            mbedtls_ssl_get_minor_ver( ssl ),
            MBEDTLS_SSL_MINOR_VERSION_1 ) ||
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
        mbedtls_cipher_get_cipher_mode( &ssl->transform_out->cipher_ctx )
                                != MBEDTLS_MODE_CBC )
#else
        mbedtls_cipher_get_cipher_mode( &ssl->transform_out->cipher_ctx_enc )
                                != MBEDTLS_MODE_CBC )
#endif
    {
        return( ssl_write_real( ssl, buf, len ) );
    }

    if( ssl->split_done == 0 )
    {
        if( ( ret = ssl_write_real( ssl, buf, 1 ) ) <= 0 )
            return( ret );
        ssl->split_done = 1;
    }

    if( ( ret = ssl_write_real( ssl, buf + 1, len - 1 ) ) <= 0 )
        return( ret );
    ssl->split_done = 0;

    return( ret + 1 );
}
#endif /* MBEDTLS_SSL_CBC_RECORD_SPLITTING */

/*
 * Write application data (public-facing wrapper)
 */
int mbedtls_ssl_write( mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len )
{
    int ret = MBEDTLS_ERR_PLATFORM_FAULT_DETECTED;
#if defined(MBEDTLS_FI_COUNTERMEASURES) && !defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
    /*
     * Buffer pointer and size duplication cannot be supported with MBEDTLS_SSL_CBC_RECORD_SPLITTING.
     * After splitting pointers and data size will not be the same as initaly provides by user.
     */
    volatile const unsigned char *buf_dup = buf;
    volatile size_t len_dup = len;
#endif
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write" ) );

    if( ssl == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

#if defined(MBEDTLS_SSL_RENEGOTIATION)
    if( ( ret = ssl_check_ctr_renegotiate( ssl ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_check_ctr_renegotiate", ret );
        return( ret );
    }
#endif

    if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
    {
        if( ( ret = mbedtls_ssl_handshake( ssl ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_handshake", ret );
            return( ret );
        }
    }

#if defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
    ret = ssl_write_split( ssl, buf, len );
#else
#if defined(MBEDTLS_FI_COUNTERMEASURES)
    /* Add const user pointers to context. We will be able to check its validity before copy to context */
    ssl->out_msg_dup = (unsigned char*)buf_dup;
    ssl->out_msglen_dup = len_dup;
#endif //MBEDTLS_FI_COUNTERMEASURES
    ret = ssl_write_real( ssl, buf, len );
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write" ) );


    return( ret );

}

/*
 * Notify the peer that the connection is being closed
 */
int mbedtls_ssl_close_notify( mbedtls_ssl_context *ssl )
{
    int ret;

    if( ssl == NULL || ssl->conf == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write close notify" ) );

    if( ssl->out_left != 0 )
        return( mbedtls_ssl_flush_output( ssl ) );

    if( ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER )
    {
        if( ( ret = mbedtls_ssl_send_alert_message( ssl,
                        MBEDTLS_SSL_ALERT_LEVEL_WARNING,
                        MBEDTLS_SSL_ALERT_MSG_CLOSE_NOTIFY ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_send_alert_message", ret );
            return( ret );
        }
    }

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write close notify" ) );

    return( 0 );
}

void mbedtls_ssl_transform_free( mbedtls_ssl_transform *transform )
{
    if( transform == NULL )
        return;

#if defined(MBEDTLS_ZLIB_SUPPORT)
    deflateEnd( &transform->ctx_deflate );
    inflateEnd( &transform->ctx_inflate );
#endif
#if defined(MBEDTLS_SSL_TRANSFORM_OPTIMIZE_CIPHERS)
    mbedtls_cipher_free( &transform->cipher_ctx );
    if( transform->key_dec != NULL )
        mbedtls_free( transform->key_dec );
    if( transform->key_enc != NULL )
        mbedtls_free( transform->key_enc );
#else
    mbedtls_cipher_free( &transform->cipher_ctx_enc );
    mbedtls_cipher_free( &transform->cipher_ctx_dec );
#endif
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
    mbedtls_md_free( &transform->md_ctx_enc );
    mbedtls_md_free( &transform->md_ctx_dec );
#endif

    mbedtls_platform_zeroize( transform, sizeof( mbedtls_ssl_transform ) );
}

#if defined(MBEDTLS_X509_CRT_PARSE_C)
static void ssl_key_cert_free( mbedtls_ssl_key_cert *key_cert )
{
    mbedtls_ssl_key_cert *cur = key_cert, *next;

    while( cur != NULL )
    {
        next = cur->next;
        mbedtls_free( cur );
        cur = next;
    }
}
#endif /* MBEDTLS_X509_CRT_PARSE_C */

#if defined(MBEDTLS_SSL_PROTO_DTLS)

static void ssl_buffering_free( mbedtls_ssl_context *ssl )
{
    unsigned offset;
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;

    if( hs == NULL )
        return;

    ssl_free_buffered_record( ssl );

    for( offset = 0; offset < MBEDTLS_SSL_MAX_BUFFERED_HS; offset++ )
        ssl_buffering_free_slot( ssl, offset );
}

static void ssl_buffering_free_slot( mbedtls_ssl_context *ssl,
                                     uint8_t slot )
{
    mbedtls_ssl_handshake_params * const hs = ssl->handshake;
    mbedtls_ssl_hs_buffer * const hs_buf = &hs->buffering.hs[slot];

    if( slot >= MBEDTLS_SSL_MAX_BUFFERED_HS )
        return;

    if( hs_buf->is_valid == 1 )
    {
        hs->buffering.total_bytes_buffered -= hs_buf->data_len;
        mbedtls_platform_zeroize( hs_buf->data, hs_buf->data_len );
        mbedtls_free( hs_buf->data );
        mbedtls_platform_memset( hs_buf, 0, sizeof( mbedtls_ssl_hs_buffer ) );
    }
}

#endif /* MBEDTLS_SSL_PROTO_DTLS */

void mbedtls_ssl_handshake_free( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    if( handshake == NULL )
        return;

#if defined(MBEDTLS_SSL_ASYNC_PRIVATE)
    if( ssl->conf->f_async_cancel != NULL && handshake->async_in_progress != 0 )
    {
        ssl->conf->f_async_cancel( ssl );
        handshake->async_in_progress = 0;
    }
#endif /* MBEDTLS_SSL_ASYNC_PRIVATE */

#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_1)
    mbedtls_md5_free(    &handshake->fin_md5  );
    mbedtls_sha1_free(   &handshake->fin_sha1 );
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA256_C)
    mbedtls_sha256_free(   &handshake->fin_sha256    );
#endif
#if defined(MBEDTLS_SHA512_C)
    mbedtls_sha512_free(   &handshake->fin_sha512    );
#endif
#endif /* MBEDTLS_SSL_PROTO_TLS1_2 */

#if defined(MBEDTLS_SSL_FREE_SERVER_CERTIFICATE) && \
    defined(MBEDTLS_X509_CRT_PARSE_C) && \
    defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    if( ssl->session_negotiate )
    {
        ssl_clear_peer_cert( ssl->session_negotiate );
    }
    if( ssl->session )
    {
        ssl_clear_peer_cert( ssl->session );
    }
#endif /* MBEDTLS_SSL_FREE_SERVER_CERTIFICATE */

#if defined(MBEDTLS_DHM_C)
    mbedtls_dhm_free( &handshake->dhm_ctx );
#endif
#if defined(MBEDTLS_ECDH_C)
    mbedtls_ecdh_free( &handshake->ecdh_ctx );
#endif
#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
    mbedtls_ecjpake_free( &handshake->ecjpake_ctx );
#if defined(MBEDTLS_SSL_CLI_C)
    mbedtls_free( handshake->ecjpake_cache );
    handshake->ecjpake_cache = NULL;
    handshake->ecjpake_cache_len = 0;
#endif
#endif

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
    if( handshake->psk != NULL )
    {
        mbedtls_platform_zeroize( handshake->psk, handshake->psk_len );
        mbedtls_free( handshake->psk );
    }
#endif

#if defined(MBEDTLS_X509_CRT_PARSE_C) && \
    defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
    /*
     * Free only the linked list wrapper, not the keys themselves
     * since the belong to the SNI callback
     */
    if( handshake->sni_key_cert != NULL )
    {
        mbedtls_ssl_key_cert *cur = handshake->sni_key_cert, *next;

        while( cur != NULL )
        {
            next = cur->next;
            mbedtls_free( cur );
            cur = next;
        }
    }
#endif /* MBEDTLS_X509_CRT_PARSE_C && MBEDTLS_SSL_SERVER_NAME_INDICATION */

#if defined(MBEDTLS_SSL__ECP_RESTARTABLE)
    mbedtls_x509_crt_restart_free( &handshake->ecrs_ctx );
    if( handshake->ecrs_peer_cert != NULL )
    {
        mbedtls_x509_crt_free( handshake->ecrs_peer_cert );
        mbedtls_free( handshake->ecrs_peer_cert );
    }
#endif

#if defined(MBEDTLS_X509_CRT_PARSE_C) &&        \
    !defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
    mbedtls_pk_free( &handshake->peer_pubkey );
#endif /* MBEDTLS_X509_CRT_PARSE_C && !MBEDTLS_SSL_KEEP_PEER_CERTIFICATE */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    mbedtls_free( handshake->verify_cookie );
    ssl_flight_free( handshake->flight );
    ssl_buffering_free( ssl );
#endif

    mbedtls_platform_zeroize( handshake,
                              sizeof( mbedtls_ssl_handshake_params ) );

#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
    /* If the buffers are too big - reallocate. Because of the way Mbed TLS
     * processes datagrams and the fact that a datagram is allowed to have
     * several records in it, it is possible that the I/O buffers are not
     * empty at this stage */
    handle_buffer_resizing( ssl, BUFFER_DOWNSIZING,
                            mbedtls_ssl_get_input_buflen( ssl ),
                            mbedtls_ssl_get_output_buflen( ssl ) );
#endif
}

void mbedtls_ssl_session_free( mbedtls_ssl_session *session )
{
    if( session == NULL )
        return;

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    ssl_clear_peer_cert( session );
#endif

#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
    mbedtls_free( session->ticket );
#endif

    mbedtls_platform_zeroize( session, sizeof( mbedtls_ssl_session ) );
}

#if defined(MBEDTLS_SSL_CONTEXT_SERIALIZATION)

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_CONNECTION_ID 1u
#else
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_CONNECTION_ID 0u
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

#if defined(MBEDTLS_SSL_DTLS_BADMAC_LIMIT)
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_BADMAC_LIMIT 1u
#else
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_BADMAC_LIMIT 0u
#endif /* MBEDTLS_SSL_DTLS_BADMAC_LIMIT */

#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_ANTI_REPLAY 1u
#else
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_ANTI_REPLAY 0u
#endif /* MBEDTLS_SSL_DTLS_ANTI_REPLAY */

#if defined(MBEDTLS_SSL_ALPN)
#define SSL_SERIALIZED_CONTEXT_CONFIG_ALPN 1u
#else
#define SSL_SERIALIZED_CONTEXT_CONFIG_ALPN 0u
#endif /* MBEDTLS_SSL_ALPN */

#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_CONNECTION_ID_BIT    0
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_BADMAC_LIMIT_BIT     1
#define SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_ANTI_REPLAY_BIT      2
#define SSL_SERIALIZED_CONTEXT_CONFIG_ALPN_BIT                  3

#define SSL_SERIALIZED_CONTEXT_CONFIG_BITFLAG   \
    ( (uint32_t) (                              \
        ( SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_CONNECTION_ID     << SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_CONNECTION_ID_BIT     ) | \
        ( SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_BADMAC_LIMIT      << SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_BADMAC_LIMIT_BIT      ) | \
        ( SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_ANTI_REPLAY       << SSL_SERIALIZED_CONTEXT_CONFIG_DTLS_ANTI_REPLAY_BIT       ) | \
        ( SSL_SERIALIZED_CONTEXT_CONFIG_ALPN                   << SSL_SERIALIZED_CONTEXT_CONFIG_ALPN_BIT                   ) | \
        0u ) )

static unsigned char ssl_serialized_context_header[] = {
    MBEDTLS_VERSION_MAJOR,
    MBEDTLS_VERSION_MINOR,
    MBEDTLS_VERSION_PATCH,
    ( SSL_SERIALIZED_SESSION_CONFIG_BITFLAG >> 8 ) & 0xFF,
    ( SSL_SERIALIZED_SESSION_CONFIG_BITFLAG >> 0 ) & 0xFF,
    ( SSL_SERIALIZED_CONTEXT_CONFIG_BITFLAG >> 16 ) & 0xFF,
    ( SSL_SERIALIZED_CONTEXT_CONFIG_BITFLAG >>  8 ) & 0xFF,
    ( SSL_SERIALIZED_CONTEXT_CONFIG_BITFLAG >>  0 ) & 0xFF,
};

/*
 * Serialize a full SSL context
 *
 * The format of the serialized data is:
 * (in the presentation language of TLS, RFC 8446 section 3)
 *
 *  // header
 *  opaque mbedtls_version[3];   // major, minor, patch
 *  opaque context_format[5];    // version-specific field determining
 *                               // the format of the remaining
 *                               // serialized data.
 *  Note: When updating the format, remember to keep these
 *        version+format bytes. (We may make their size part of the API.)
 *
 *  // session sub-structure
 *  opaque session<1..2^32-1>;  // see mbedtls_ssl_session_save()
 *  // transform sub-structure
 *  uint8 random[64];           // ServerHello.random+ClientHello.random
 *  uint8 in_cid<0..2^8-1>      // Connection ID: expected incoming value
 *  uint8 out_cid<0..2^8-1>     // Connection ID: outgoing value to use
 *  // fields from ssl_context
 *  uint32 badmac_seen;         // DTLS: number of records with failing MAC
 *  uint64 in_window_top;       // DTLS: last validated record seq_num
 *  uint64 in_window;           // DTLS: bitmask for replay protection
 *  uint8 disable_datagram_packing; // DTLS: only one record per datagram
 *  uint64 cur_out_ctr;         // Record layer: outgoing sequence number
 *  uint16 mtu;                 // DTLS: path mtu (max outgoing fragment size)
 *  uint8 alpn_chosen<0..2^8-1> // ALPN: negotiated application protocol
 *
 * Note that many fields of the ssl_context or sub-structures are not
 * serialized, as they fall in one of the following categories:
 *
 *  1. forced value (eg in_left must be 0)
 *  2. pointer to dynamically-allocated memory (eg session, transform)
 *  3. value can be re-derived from other data (eg session keys from MS)
 *  4. value was temporary (eg content of input buffer)
 *  5. value will be provided by the user again (eg I/O callbacks and context)
 */
int mbedtls_ssl_context_save( mbedtls_ssl_context *ssl,
                              unsigned char *buf,
                              size_t buf_len,
                              size_t *olen )
{
    unsigned char *p = buf;
    size_t used = 0;
    size_t session_len;
    int ret = 0;

    /*
     * Enforce usage restrictions, see "return BAD_INPUT_DATA" in
     * this function's documentation.
     *
     * These are due to assumptions/limitations in the implementation. Some of
     * them are likely to stay (no handshake in progress) some might go away
     * (only DTLS) but are currently used to simplify the implementation.
     */
    /* The initial handshake must be over */
    if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    if( ssl->handshake != NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    /* Double-check that sub-structures are indeed ready */
    if( ssl->transform == NULL || ssl->session == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    /* There must be no pending incoming or outgoing data */
    if( mbedtls_ssl_check_pending( ssl ) != 0 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    if( ssl->out_left != 0 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    /* Protocol must be DLTS, not TLS */
    if( MBEDTLS_SSL_TRANSPORT_IS_TLS( ssl->conf->transport ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    /* Version must be 1.2 */
    if( mbedtls_ssl_get_major_ver( ssl ) != MBEDTLS_SSL_MAJOR_VERSION_3 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    if( mbedtls_ssl_get_minor_ver( ssl ) != MBEDTLS_SSL_MINOR_VERSION_3 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    /* We must be using an AEAD ciphersuite */
    if( mbedtls_ssl_transform_uses_aead( ssl->transform ) != 1 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    /* Renegotiation must not be enabled */
    if( mbedtls_ssl_conf_is_renegotiation_enabled( ssl->conf ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    /*
     * Version and format identifier
     */
    used += sizeof( ssl_serialized_context_header );

    if( used <= buf_len )
    {
        mbedtls_platform_memcpy( p, ssl_serialized_context_header,
                sizeof( ssl_serialized_context_header ) );
        p += sizeof( ssl_serialized_context_header );
    }

    /*
     * Session (length + data)
     */
    ret = ssl_session_save( ssl->session, 1, NULL, 0, &session_len );
    if( ret != MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL )
        return( ret );

    used += 4 + session_len;
    if( used <= buf_len )
    {
        p = mbedtls_platform_put_uint32_be( p, session_len );

        ret = ssl_session_save( ssl->session, 1,
                                p, session_len, &session_len );
        if( ret != 0 )
            return( ret );

        p += session_len;
    }

    /*
     * Transform
     */
    used += sizeof( ssl->transform->randbytes );
    if( used <= buf_len )
    {
        mbedtls_platform_memcpy( p, ssl->transform->randbytes,
           sizeof( ssl->transform->randbytes ) );
        p += sizeof( ssl->transform->randbytes );
    }

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    used += 2 + ssl->transform->in_cid_len + ssl->transform->out_cid_len;
    if( used <= buf_len )
    {
        *p++ = ssl->transform->in_cid_len;
        /* Not using more secure mbedtls_platform_memcpy as cid is public */
        memcpy( p, ssl->transform->in_cid, ssl->transform->in_cid_len );
        p += ssl->transform->in_cid_len;

        *p++ = ssl->transform->out_cid_len;
        /* Not using more secure mbedtls_platform_memcpy as cid is public */
        memcpy( p, ssl->transform->out_cid, ssl->transform->out_cid_len );
        p += ssl->transform->out_cid_len;
    }
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

    /*
     * Saved fields from top-level ssl_context structure
     */
#if defined(MBEDTLS_SSL_DTLS_BADMAC_LIMIT)
    used += 4;
    if( used <= buf_len )
    {
        p = mbedtls_platform_put_uint32_be( p, ssl->badmac_seen );
    }
#endif /* MBEDTLS_SSL_DTLS_BADMAC_LIMIT */

#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
    used += 16;
    if( used <= buf_len )
    {
        *p++ = (unsigned char)( ( ssl->in_window_top >> 56 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window_top >> 48 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window_top >> 40 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window_top >> 32 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window_top >> 24 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window_top >> 16 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window_top >>  8 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window_top       ) & 0xFF );

        *p++ = (unsigned char)( ( ssl->in_window >> 56 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window >> 48 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window >> 40 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window >> 32 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window >> 24 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window >> 16 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window >>  8 ) & 0xFF );
        *p++ = (unsigned char)( ( ssl->in_window       ) & 0xFF );
    }
#endif /* MBEDTLS_SSL_DTLS_ANTI_REPLAY */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    used += 1;
    if( used <= buf_len )
    {
        *p++ = ssl->disable_datagram_packing;
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    used += 8;
    if( used <= buf_len )
    {
        mbedtls_platform_memcpy( p, ssl->cur_out_ctr, 8 );
        p += 8;
    }

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    used += 2;
    if( used <= buf_len )
    {
        p = mbedtls_platform_put_uint16_be( p, ssl->mtu );
    }
#endif /* MBEDTLS_SSL_PROTO_DTLS */

#if defined(MBEDTLS_SSL_ALPN)
    {
        const uint8_t alpn_len = ssl->alpn_chosen
                               ? (uint8_t) strlen( ssl->alpn_chosen )
                               : 0;

        used += 1 + alpn_len;
        if( used <= buf_len )
        {
            *p++ = alpn_len;

            if( ssl->alpn_chosen != NULL )
            {
                mbedtls_platform_memcpy( p, ssl->alpn_chosen, alpn_len );
                p += alpn_len;
            }
        }
    }
#endif /* MBEDTLS_SSL_ALPN */

    /*
     * Done
     */
    *olen = used;

    if( used > buf_len )
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );

    MBEDTLS_SSL_DEBUG_BUF( 4, "saved context", buf, used );

    return( ssl_session_reset_int( ssl, 0 ) );
}

/*
 * Deserialize context, see mbedtls_ssl_context_save() for format.
 *
 * This internal version is wrapped by a public function that cleans up in
 * case of error.
 */
static int ssl_context_load( mbedtls_ssl_context *ssl,
                             const unsigned char *buf,
                             size_t len )
{
    const unsigned char *p = buf;
    const unsigned char * const end = buf + len;
    size_t session_len;
    int ret;

    /*
     * The context should have been freshly setup or reset.
     * Give the user an error in case of obvious misuse.
     * (Checking session is useful because it won't be NULL if we're
     * renegotiating, or if the user mistakenly loaded a session first.)
     */
    if( ssl->state != MBEDTLS_SSL_HELLO_REQUEST ||
        ssl->session != NULL )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    /*
     * We can't check that the config matches the initial one, but we can at
     * least check it matches the requirements for serializing.
     */
    if( MBEDTLS_SSL_TRANSPORT_IS_TLS( ssl->conf->transport ) ||
        mbedtls_ssl_ver_lt(
            mbedtls_ssl_conf_get_max_major_ver( ssl->conf ),
            MBEDTLS_SSL_MAJOR_VERSION_3 ) ||
        mbedtls_ssl_ver_gt(
            mbedtls_ssl_conf_get_min_major_ver( ssl->conf ),
            MBEDTLS_SSL_MAJOR_VERSION_3 ) ||
        mbedtls_ssl_ver_lt(
            mbedtls_ssl_conf_get_max_minor_ver( ssl->conf ),
            MBEDTLS_SSL_MINOR_VERSION_3 ) ||
        mbedtls_ssl_ver_gt(
            mbedtls_ssl_conf_get_min_minor_ver( ssl->conf ),
            MBEDTLS_SSL_MINOR_VERSION_3 ) ||
        mbedtls_ssl_conf_is_renegotiation_enabled( ssl->conf ) )
    {
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    MBEDTLS_SSL_DEBUG_BUF( 4, "context to load", buf, len );

    /*
     * Check version identifier
     */
    if( (size_t)( end - p ) < sizeof( ssl_serialized_context_header ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    // use regular memcmp as header is not that critical
    if( memcmp( p, ssl_serialized_context_header,
                sizeof( ssl_serialized_context_header ) ) != 0 )
    {
        return( MBEDTLS_ERR_SSL_VERSION_MISMATCH );
    }
    p += sizeof( ssl_serialized_context_header );

    /*
     * Session
     */
    if( (size_t)( end - p ) < 4 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    session_len = mbedtls_platform_get_uint32_be( p );
    p += 4;

    /* This has been allocated by ssl_handshake_init(), called by
     * by either ssl_session_reset_int() or mbedtls_ssl_setup(). */
    ssl->session = ssl->session_negotiate;
    ssl->session_in = ssl->session;
    ssl->session_out = ssl->session;
    ssl->session_negotiate = NULL;

    if( (size_t)( end - p ) < session_len )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ret = ssl_session_load( ssl->session, 1, p, session_len );
    if( ret != 0 )
    {
        mbedtls_ssl_session_free( ssl->session );
        return( ret );
    }

    p += session_len;

    /*
     * Transform
     */

    /* This has been allocated by ssl_handshake_init(), called by
     * by either ssl_session_reset_int() or mbedtls_ssl_setup(). */
    ssl->transform = ssl->transform_negotiate;
    ssl->transform_in = ssl->transform;
    ssl->transform_out = ssl->transform;
    ssl->transform_negotiate = NULL;

    /* Read random bytes and populate structure */
    if( (size_t)( end - p ) < sizeof( ssl->transform->randbytes ) )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ret = ssl_populate_transform( ssl->transform,
                  mbedtls_ssl_session_get_ciphersuite( ssl->session ),
                  ssl->session->master,
#if defined(MBEDTLS_SSL_SOME_MODES_USE_MAC)
#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
                  ssl->session->encrypt_then_mac,
#endif
#if defined(MBEDTLS_SSL_TRUNCATED_HMAC)
                  ssl->session->trunc_hmac,
#endif
#endif /* MBEDTLS_SSL_SOME_MODES_USE_MAC */
#if defined(MBEDTLS_ZLIB_SUPPORT)
                  ssl->session->compression,
#endif
                  p, /* currently pointing to randbytes */
                  MBEDTLS_SSL_MINOR_VERSION_3, /* (D)TLS 1.2 is forced */
                  mbedtls_ssl_conf_get_endpoint( ssl->conf ),
                  ssl );
    if( ret != 0 )
        return( ret );

    p += sizeof( ssl->transform->randbytes );

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    /* Read connection IDs and store them */
    if( (size_t)( end - p ) < 1 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ssl->transform->in_cid_len = *p++;

    if( (size_t)( end - p ) < ssl->transform->in_cid_len + 1u )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    /* Not using more secure mbedtls_platform_memcpy as cid is public */
    memcpy( ssl->transform->in_cid, p, ssl->transform->in_cid_len );
    p += ssl->transform->in_cid_len;

    ssl->transform->out_cid_len = *p++;

    if( (size_t)( end - p ) < ssl->transform->out_cid_len )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    /* Not using more secure mbedtls_platform_memcpy as cid is public */
    memcpy( ssl->transform->out_cid, p, ssl->transform->out_cid_len );
    p += ssl->transform->out_cid_len;
#endif /* MBEDTLS_SSL_DTLS_CONNECTION_ID */

    /*
     * Saved fields from top-level ssl_context structure
     */
#if defined(MBEDTLS_SSL_DTLS_BADMAC_LIMIT)
    if( (size_t)( end - p ) < 4 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ssl->badmac_seen = (unsigned)mbedtls_platform_get_uint32_be( p );
    p += 4;
#endif /* MBEDTLS_SSL_DTLS_BADMAC_LIMIT */

#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY)
    if( (size_t)( end - p ) < 16 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ssl->in_window_top = ( (uint64_t) p[0] << 56 ) |
                         ( (uint64_t) p[1] << 48 ) |
                         ( (uint64_t) p[2] << 40 ) |
                         ( (uint64_t) p[3] << 32 ) |
                         ( (uint64_t) p[4] << 24 ) |
                         ( (uint64_t) p[5] << 16 ) |
                         ( (uint64_t) p[6] <<  8 ) |
                         ( (uint64_t) p[7]       );
    p += 8;

    ssl->in_window = ( (uint64_t) p[0] << 56 ) |
                     ( (uint64_t) p[1] << 48 ) |
                     ( (uint64_t) p[2] << 40 ) |
                     ( (uint64_t) p[3] << 32 ) |
                     ( (uint64_t) p[4] << 24 ) |
                     ( (uint64_t) p[5] << 16 ) |
                     ( (uint64_t) p[6] <<  8 ) |
                     ( (uint64_t) p[7]       );
    p += 8;
#endif /* MBEDTLS_SSL_DTLS_ANTI_REPLAY */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( (size_t)( end - p ) < 1 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ssl->disable_datagram_packing = *p++;
#endif /* MBEDTLS_SSL_PROTO_DTLS */

    if( (size_t)( end - p ) < 8 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    mbedtls_platform_memcpy( ssl->cur_out_ctr, p, 8 );
    p += 8;

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    if( (size_t)( end - p ) < 2 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    ssl->mtu = (uint16_t)mbedtls_platform_get_uint16_be( p );
    p += 2;
#endif /* MBEDTLS_SSL_PROTO_DTLS */

#if defined(MBEDTLS_SSL_ALPN)
    {
        uint8_t alpn_len;
        const char **cur;

        if( (size_t)( end - p ) < 1 )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        alpn_len = *p++;

        if( alpn_len != 0 && ssl->conf->alpn_list != NULL )
        {
            /* alpn_chosen should point to an item in the configured list */
            for( cur = ssl->conf->alpn_list; *cur != NULL; cur++ )
            {
                if( strlen( *cur ) == alpn_len &&
                    mbedtls_platform_memequal( p, cur, alpn_len ) == 0 )
                {
                    ssl->alpn_chosen = *cur;
                    break;
                }
            }
        }

        /* can only happen on conf mismatch */
        if( alpn_len != 0 && ssl->alpn_chosen == NULL )
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

        p += alpn_len;
    }
#endif /* MBEDTLS_SSL_ALPN */

    /*
     * Forced fields from top-level ssl_context structure
     *
     * Most of them already set to the correct value by mbedtls_ssl_init() and
     * mbedtls_ssl_reset(), so we only need to set the remaining ones.
     */
    ssl->state = MBEDTLS_SSL_HANDSHAKE_OVER;

#if !defined(MBEDTLS_SSL_CONF_FIXED_MAJOR_VER)
    ssl->major_ver = MBEDTLS_SSL_MAJOR_VERSION_3;
#endif /* !MBEDTLS_SSL_CONF_FIXED_MAJOR_VER */
#if !defined(MBEDTLS_SSL_CONF_FIXED_MINOR_VER)
    ssl->minor_ver = MBEDTLS_SSL_MINOR_VERSION_3;
#endif /* !MBEDTLS_SSL_CONF_FIXED_MINOR_VER */

    /* Adjust pointers for header fields of outgoing records to
     * the given transform, accounting for explicit IV and CID. */
    ssl_update_out_pointers( ssl, ssl->transform );

#if defined(MBEDTLS_SSL_PROTO_DTLS)
    ssl->in_epoch = 1;
#endif

    /* mbedtls_ssl_reset() leaves the handshake sub-structure allocated,
     * which we don't want - otherwise we'd end up freeing the wrong transform
     * by calling ssl_handshake_wrapup_free_hs_transform() inappropriately. */
    if( ssl->handshake != NULL )
    {
        mbedtls_ssl_handshake_free( ssl );
        mbedtls_free( ssl->handshake );
        ssl->handshake = NULL;
    }

    /*
     * Done - should have consumed entire buffer
     */
    if( p != end )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    return( 0 );
}

/*
 * Deserialize context: public wrapper for error cleaning
 */
int mbedtls_ssl_context_load( mbedtls_ssl_context *context,
                              const unsigned char *buf,
                              size_t len )
{
    int ret = ssl_context_load( context, buf, len );

    if( ret != 0 )
        mbedtls_ssl_free( context );

    return( ret );
}
#endif /* MBEDTLS_SSL_CONTEXT_SERIALIZATION */

/*
 * Free an SSL context
 */
void mbedtls_ssl_free( mbedtls_ssl_context *ssl )
{
    if( ssl == NULL )
        return;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> free" ) );

    if( ssl->out_buf != NULL )
    {
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
        size_t out_buf_len = ssl->out_buf_len;
#else
        size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;
#endif
        mbedtls_platform_zeroize( ssl->out_buf, out_buf_len );
        mbedtls_free( ssl->out_buf );
        ssl->out_buf = NULL;
    }

    if( ssl->in_buf != NULL )
    {
#if defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH)
        size_t in_buf_len = ssl->in_buf_len;
#else
        size_t in_buf_len = MBEDTLS_SSL_IN_BUFFER_LEN;
#endif
        mbedtls_platform_zeroize( ssl->in_buf, in_buf_len );
        mbedtls_free( ssl->in_buf );
        ssl->in_buf = NULL;
    }

#if defined(MBEDTLS_ZLIB_SUPPORT)
    if( ssl->compress_buf != NULL )
    {
        mbedtls_platform_zeroize( ssl->compress_buf, MBEDTLS_SSL_COMPRESS_BUFFER_LEN );
        mbedtls_free( ssl->compress_buf );
    }
#endif

    if( ssl->transform )
    {
        mbedtls_ssl_transform_free( ssl->transform );
        mbedtls_free( ssl->transform );
    }

    if( ssl->handshake )
    {
        mbedtls_ssl_handshake_free( ssl );
        mbedtls_ssl_transform_free( ssl->transform_negotiate );
        mbedtls_ssl_session_free( ssl->session_negotiate );

        mbedtls_free( ssl->handshake );
        mbedtls_free( ssl->transform_negotiate );
        mbedtls_free( ssl->session_negotiate );
    }

    if( ssl->session )
    {
        mbedtls_ssl_session_free( ssl->session );
        mbedtls_free( ssl->session );
    }

#if defined(MBEDTLS_X509_CRT_PARSE_C) && !defined(MBEDTLS_X509_REMOVE_HOSTNAME_VERIFICATION)
    if( ssl->hostname != NULL )
    {
        mbedtls_platform_zeroize( ssl->hostname, strlen( ssl->hostname ) );
        mbedtls_free( ssl->hostname );
    }
#endif

#if defined(MBEDTLS_SSL_HW_RECORD_ACCEL)
    if( mbedtls_ssl_hw_record_finish != NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "going for mbedtls_ssl_hw_record_finish()" ) );
        mbedtls_ssl_hw_record_finish( ssl );
    }
#endif

#if defined(MBEDTLS_SSL_DTLS_HELLO_VERIFY) && defined(MBEDTLS_SSL_SRV_C)
    mbedtls_free( ssl->cli_id );
#endif

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= free" ) );

    /* Actually clear after last debug message */
    mbedtls_platform_zeroize( ssl, sizeof( mbedtls_ssl_context ) );
}

/*
 * Initialze mbedtls_ssl_config
 */
void mbedtls_ssl_config_init( mbedtls_ssl_config *conf )
{
    memset( conf, 0, sizeof( mbedtls_ssl_config ) );

#if !defined(MBEDTLS_SSL_PROTO_TLS)
#if !defined(MBEDTLS_SSL_CONF_TRANSPORT)
    conf->transport = MBEDTLS_SSL_TRANSPORT_DATAGRAM;
#endif
#endif
}

#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
#if !defined(MBEDTLS_SSL_CONF_SINGLE_SIG_HASH)
static int ssl_preset_default_hashes[] = {
#if defined(MBEDTLS_SHA512_C)
    MBEDTLS_MD_SHA512,
    MBEDTLS_MD_SHA384,
#endif
#if defined(MBEDTLS_SHA256_C)
    MBEDTLS_MD_SHA256,
    MBEDTLS_MD_SHA224,
#endif
#if defined(MBEDTLS_SHA1_C) && defined(MBEDTLS_TLS_DEFAULT_ALLOW_SHA1_IN_KEY_EXCHANGE)
    MBEDTLS_MD_SHA1,
#endif
    MBEDTLS_MD_NONE
};
#endif
#endif

#if !defined(MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE)
static int ssl_preset_suiteb_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    0
};
#endif /* !MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE */

#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
#if !defined(MBEDTLS_SSL_CONF_SINGLE_SIG_HASH)
static int ssl_preset_suiteb_hashes[] = {
    MBEDTLS_MD_SHA256,
    MBEDTLS_MD_SHA384,
    MBEDTLS_MD_NONE
};
#endif
#endif

#if defined(MBEDTLS_ECP_C) && !defined(MBEDTLS_SSL_CONF_SINGLE_EC)
static mbedtls_ecp_group_id ssl_preset_suiteb_curves[] = {
#if defined(MBEDTLS_ECP_DP_SECP256R1_ENABLED)
    MBEDTLS_ECP_DP_SECP256R1,
#endif
#if defined(MBEDTLS_ECP_DP_SECP384R1_ENABLED)
    MBEDTLS_ECP_DP_SECP384R1,
#endif
    MBEDTLS_ECP_DP_NONE
};
#endif

/*
 * Load default in mbedtls_ssl_config
 */
int mbedtls_ssl_config_defaults( mbedtls_ssl_config *conf,
                                 int endpoint, int transport, int preset )
{
#if defined(MBEDTLS_DHM_C) && defined(MBEDTLS_SSL_SRV_C)
    int ret;
#endif

    /* Use the functions here so that they are covered in tests,
     * but otherwise access member directly for efficiency */
#if !defined(MBEDTLS_SSL_CONF_ENDPOINT)
    mbedtls_ssl_conf_endpoint( conf, endpoint );
#endif
#if !defined(MBEDTLS_SSL_CONF_TRANSPORT)
    mbedtls_ssl_conf_transport( conf, transport );
#else
    ((void) transport);
#endif

    /*
     * Things that are common to all presets
     */
#if defined(MBEDTLS_SSL_CLI_C)
    if( endpoint == MBEDTLS_SSL_IS_CLIENT )
    {
#if !defined(MBEDTLS_SSL_CONF_AUTHMODE)
        conf->authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
#endif /* !MBEDTLS_SSL_CONF_AUTHMODE */
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
        conf->session_tickets = MBEDTLS_SSL_SESSION_TICKETS_ENABLED;
#endif
    }
#endif

#if defined(MBEDTLS_ARC4_C)
    conf->arc4_disabled = MBEDTLS_SSL_ARC4_DISABLED;
#endif

#if defined(MBEDTLS_SSL_ENCRYPT_THEN_MAC)
    conf->encrypt_then_mac = MBEDTLS_SSL_ETM_ENABLED;
#endif

#if defined(MBEDTLS_SSL_EXTENDED_MASTER_SECRET)
#if !defined(MBEDTLS_SSL_CONF_EXTENDED_MASTER_SECRET)
    conf->extended_ms = MBEDTLS_SSL_EXTENDED_MS_ENABLED;
#endif /* !MBEDTLS_SSL_CONF_EXTENDED_MASTER_SECRET */
#if !defined(MBEDTLS_SSL_CONF_ENFORCE_EXTENDED_MASTER_SECRET)
    conf->enforce_extended_master_secret =
        MBEDTLS_SSL_EXTENDED_MS_ENFORCE_DISABLED;
#endif /* !MBEDTLS_SSL_CONF_ENFORCE_EXTENDED_MASTER_SECRET */
#endif

#if defined(MBEDTLS_SSL_CBC_RECORD_SPLITTING)
    conf->cbc_record_splitting = MBEDTLS_SSL_CBC_RECORD_SPLITTING_ENABLED;
#endif

#if defined(MBEDTLS_SSL_DTLS_HELLO_VERIFY) && defined(MBEDTLS_SSL_SRV_C)
    conf->f_cookie_write = ssl_cookie_write_dummy;
    conf->f_cookie_check = ssl_cookie_check_dummy;
#endif

#if defined(MBEDTLS_SSL_DTLS_ANTI_REPLAY) && \
    !defined(MBEDTLS_SSL_CONF_ANTI_REPLAY)
    conf->anti_replay = MBEDTLS_SSL_ANTI_REPLAY_ENABLED;
#endif

#if defined(MBEDTLS_SSL_SRV_C)
#if !defined(MBEDTLS_SSL_CONF_CERT_REQ_CA_LIST)
    conf->cert_req_ca_list = MBEDTLS_SSL_CERT_REQ_CA_LIST_ENABLED;
#endif /* !MBEDTLS_SSL_CONF_CERT_REQ_CA_LIST */
#endif /* MBEDTLS_SSL_SRV_C */

#if defined(MBEDTLS_SSL_PROTO_DTLS)
#if !defined(MBEDTLS_SSL_CONF_HS_TIMEOUT_MIN)
    conf->hs_timeout_min = MBEDTLS_SSL_DTLS_TIMEOUT_DFL_MIN;
#endif /* !MBEDTLS_SSL_CONF_HS_TIMEOUT_MIN */
#if !defined(MBEDTLS_SSL_CONF_HS_TIMEOUT_MAX)
    conf->hs_timeout_max = MBEDTLS_SSL_DTLS_TIMEOUT_DFL_MAX;
#endif /* !MBEDTLS_SSL_CONF_HS_TIMEOUT_MAX */
#endif /* MBEDTLS_SSL_PROTO_DTLS */

#if defined(MBEDTLS_SSL_RENEGOTIATION)
    conf->renego_max_records = MBEDTLS_SSL_RENEGO_MAX_RECORDS_DEFAULT;
    mbedtls_platform_memset( conf->renego_period,     0x00, 2 );
    mbedtls_platform_memset( conf->renego_period + 2, 0xFF, 6 );
#endif

#if defined(MBEDTLS_DHM_C) && defined(MBEDTLS_SSL_SRV_C)
            if( endpoint == MBEDTLS_SSL_IS_SERVER )
            {
                const unsigned char dhm_p[] =
                    MBEDTLS_DHM_RFC3526_MODP_2048_P_BIN;
                const unsigned char dhm_g[] =
                    MBEDTLS_DHM_RFC3526_MODP_2048_G_BIN;

                if ( ( ret = mbedtls_ssl_conf_dh_param_bin( conf,
                                               dhm_p, sizeof( dhm_p ),
                                               dhm_g, sizeof( dhm_g ) ) ) != 0 )
                {
                    return( ret );
                }
            }
#endif

    /*
     * Preset-specific defaults
     */
    switch( preset )
    {
        /*
         * NSA Suite B
         */
        case MBEDTLS_SSL_PRESET_SUITEB:
#if !defined(MBEDTLS_SSL_CONF_MIN_MAJOR_VER)
            conf->min_major_ver = MBEDTLS_SSL_MAJOR_VERSION_3;
#endif /* !MBEDTLS_SSL_CONF_MIN_MAJOR_VER */
#if !defined(MBEDTLS_SSL_CONF_MIN_MINOR_VER)
            conf->min_minor_ver = MBEDTLS_SSL_MINOR_VERSION_3; /* TLS 1.2 */
#endif /* !MBEDTLS_SSL_CONF_MIN_MINOR_VER */
#if !defined(MBEDTLS_SSL_CONF_MAX_MAJOR_VER)
            conf->max_major_ver = MBEDTLS_SSL_MAX_MAJOR_VERSION;
#endif /* !MBEDTLS_SSL_CONF_MAX_MAJOR_VER */
#if !defined(MBEDTLS_SSL_CONF_MAX_MINOR_VER)
            conf->max_minor_ver = MBEDTLS_SSL_MAX_MINOR_VERSION;
#endif /* !MBEDTLS_SSL_CONF_MAX_MINOR_VER */

#if !defined(MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE)
            conf->ciphersuite_list[0] =
            conf->ciphersuite_list[1] =
            conf->ciphersuite_list[2] =
            conf->ciphersuite_list[3] =
                ssl_preset_suiteb_ciphersuites;
#endif /* MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE */

#if defined(MBEDTLS_X509_CRT_PARSE_C)
            conf->cert_profile = &mbedtls_x509_crt_profile_suiteb;
#endif

#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
#if !defined(MBEDTLS_SSL_CONF_SINGLE_SIG_HASH)
            conf->sig_hashes = ssl_preset_suiteb_hashes;
#endif
#endif

#if defined(MBEDTLS_ECP_C)
#if !defined(MBEDTLS_SSL_CONF_SINGLE_EC)
            conf->curve_list = ssl_preset_suiteb_curves;
#endif
#endif
            break;

        /*
         * Default
         */
        default:
#if !defined(MBEDTLS_SSL_CONF_MIN_MAJOR_VER)
            conf->min_major_ver = ( MBEDTLS_SSL_MIN_MAJOR_VERSION >
                                    MBEDTLS_SSL_MIN_VALID_MAJOR_VERSION ) ?
                                    MBEDTLS_SSL_MIN_MAJOR_VERSION :
                                    MBEDTLS_SSL_MIN_VALID_MAJOR_VERSION;
#endif /* !MBEDTLS_SSL_CONF_MIN_MAJOR_VER */
#if !defined(MBEDTLS_SSL_CONF_MIN_MINOR_VER)
            conf->min_minor_ver = ( MBEDTLS_SSL_MIN_MINOR_VERSION >
                                    MBEDTLS_SSL_MIN_VALID_MINOR_VERSION ) ?
                                    MBEDTLS_SSL_MIN_MINOR_VERSION :
                                    MBEDTLS_SSL_MIN_VALID_MINOR_VERSION;
#if defined(MBEDTLS_SSL_PROTO_DTLS)
            if( MBEDTLS_SSL_TRANSPORT_IS_DTLS( transport ) )
                conf->min_minor_ver = MBEDTLS_SSL_MINOR_VERSION_2;
#endif
#endif /* !MBEDTLS_SSL_CONF_MIN_MINOR_VER */
#if !defined(MBEDTLS_SSL_CONF_MAX_MAJOR_VER)
            conf->max_major_ver = MBEDTLS_SSL_MAX_MAJOR_VERSION;
#endif /* !MBEDTLS_SSL_CONF_MAX_MAJOR_VER */
#if !defined(MBEDTLS_SSL_CONF_MAX_MINOR_VER)
            conf->max_minor_ver = MBEDTLS_SSL_MAX_MINOR_VERSION;
#endif /* !MBEDTLS_SSL_CONF_MAX_MINOR_VER */

#if !defined(MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE)
            conf->ciphersuite_list[0] =
            conf->ciphersuite_list[1] =
            conf->ciphersuite_list[2] =
            conf->ciphersuite_list[3] =
                mbedtls_ssl_list_ciphersuites();
#endif /* MBEDTLS_SSL_CONF_SINGLE_CIPHERSUITE */

#if defined(MBEDTLS_X509_CRT_PARSE_C)
            conf->cert_profile = &mbedtls_x509_crt_profile_default;
#endif

#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
#if !defined(MBEDTLS_SSL_CONF_SINGLE_SIG_HASH)
            conf->sig_hashes = ssl_preset_default_hashes;
#endif
#endif

#if defined(MBEDTLS_ECP_C)
#if !defined(MBEDTLS_SSL_CONF_SINGLE_EC)
            conf->curve_list = mbedtls_ecp_grp_id_list();
#endif
#endif

#if defined(MBEDTLS_DHM_C) && defined(MBEDTLS_SSL_CLI_C)
            conf->dhm_min_bitlen = 1024;
#endif
    }

    return( 0 );
}

/*
 * Free mbedtls_ssl_config
 */
void mbedtls_ssl_config_free( mbedtls_ssl_config *conf )
{
#if defined(MBEDTLS_DHM_C)
    mbedtls_mpi_free( &conf->dhm_P );
    mbedtls_mpi_free( &conf->dhm_G );
#endif

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
    if( conf->psk != NULL )
    {
        mbedtls_platform_zeroize( conf->psk, conf->psk_len );
        mbedtls_free( conf->psk );
        conf->psk = NULL;
        conf->psk_len = 0;
    }

    if( conf->psk_identity != NULL )
    {
        mbedtls_platform_zeroize( conf->psk_identity, conf->psk_identity_len );
        mbedtls_free( conf->psk_identity );
        conf->psk_identity = NULL;
        conf->psk_identity_len = 0;
    }
#endif

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    ssl_key_cert_free( conf->key_cert );
#endif

    mbedtls_platform_zeroize( conf, sizeof( mbedtls_ssl_config ) );
}

#if defined(MBEDTLS_PK_C) && \
    ( defined(MBEDTLS_RSA_C) || defined(MBEDTLS_ECDSA_C) ) || \
    ( defined(MBEDTLS_USE_TINYCRYPT) )
/*
 * Convert between MBEDTLS_PK_XXX and SSL_SIG_XXX
 */
unsigned char mbedtls_ssl_sig_from_pk( mbedtls_pk_context *pk )
{
#if defined(MBEDTLS_RSA_C)
    if( mbedtls_pk_can_do( pk, MBEDTLS_PK_RSA ) )
        return( MBEDTLS_SSL_SIG_RSA );
#endif
#if defined(MBEDTLS_ECDSA_C) || defined(MBEDTLS_USE_TINYCRYPT)
    if( mbedtls_pk_can_do( pk, MBEDTLS_PK_ECDSA ) )
        return( MBEDTLS_SSL_SIG_ECDSA );
#endif
    return( MBEDTLS_SSL_SIG_ANON );
}

unsigned char mbedtls_ssl_sig_from_pk_alg( mbedtls_pk_type_t type )
{
    switch( type ) {
        case MBEDTLS_PK_RSA:
            return( MBEDTLS_SSL_SIG_RSA );
        case MBEDTLS_PK_ECDSA:
        case MBEDTLS_PK_ECKEY:
            return( MBEDTLS_SSL_SIG_ECDSA );
        default:
            return( MBEDTLS_SSL_SIG_ANON );
    }
}

mbedtls_pk_type_t mbedtls_ssl_pk_alg_from_sig( unsigned char sig )
{
    switch( sig )
    {
#if defined(MBEDTLS_RSA_C)
        case MBEDTLS_SSL_SIG_RSA:
            return( MBEDTLS_PK_RSA );
#endif
#if defined(MBEDTLS_ECDSA_C) || defined(MBEDTLS_USE_TINYCRYPT)
        case MBEDTLS_SSL_SIG_ECDSA:
            return( MBEDTLS_PK_ECDSA );
#endif
        default:
            return( MBEDTLS_PK_NONE );
    }
}
#endif /* MBEDTLS_PK_C && ( MBEDTLS_RSA_C || MBEDTLS_ECDSA_C ) */

#if defined(MBEDTLS_SSL_PROTO_TLS1_2) && \
    defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)

/* Find an entry in a signature-hash set matching a given hash algorithm. */
mbedtls_md_type_t mbedtls_ssl_sig_hash_set_find( mbedtls_ssl_sig_hash_set_t *set,
                                                 mbedtls_pk_type_t sig_alg )
{
    switch( sig_alg )
    {
        case MBEDTLS_PK_RSA:
            return( set->rsa );
        case MBEDTLS_PK_ECDSA:
            return( set->ecdsa );
        default:
            return( MBEDTLS_MD_NONE );
    }
}

/* Add a signature-hash-pair to a signature-hash set */
void mbedtls_ssl_sig_hash_set_add( mbedtls_ssl_sig_hash_set_t *set,
                                   mbedtls_pk_type_t sig_alg,
                                   mbedtls_md_type_t md_alg )
{
    switch( sig_alg )
    {
        case MBEDTLS_PK_RSA:
            if( set->rsa == MBEDTLS_MD_NONE )
                set->rsa = md_alg;
            break;

        case MBEDTLS_PK_ECDSA:
            if( set->ecdsa == MBEDTLS_MD_NONE )
                set->ecdsa = md_alg;
            break;

        default:
            break;
    }
}

/* Allow exactly one hash algorithm for each signature. */
void mbedtls_ssl_sig_hash_set_const_hash( mbedtls_ssl_sig_hash_set_t *set,
                                          mbedtls_md_type_t md_alg )
{
    set->rsa   = md_alg;
    set->ecdsa = md_alg;
}

#endif /* MBEDTLS_SSL_PROTO_TLS1_2) &&
          MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED */

/*
 * Convert from MBEDTLS_SSL_HASH_XXX to MBEDTLS_MD_XXX
 */
mbedtls_md_type_t mbedtls_ssl_md_alg_from_hash( unsigned char hash )
{
    switch( hash )
    {
#if defined(MBEDTLS_MD5_C)
        case MBEDTLS_SSL_HASH_MD5:
            return( MBEDTLS_MD_MD5 );
#endif
#if defined(MBEDTLS_SHA1_C)
        case MBEDTLS_SSL_HASH_SHA1:
            return( MBEDTLS_MD_SHA1 );
#endif
#if defined(MBEDTLS_SHA256_C)
        case MBEDTLS_SSL_HASH_SHA224:
            return( MBEDTLS_MD_SHA224 );
        case MBEDTLS_SSL_HASH_SHA256:
            return( MBEDTLS_MD_SHA256 );
#endif
#if defined(MBEDTLS_SHA512_C)
        case MBEDTLS_SSL_HASH_SHA384:
            return( MBEDTLS_MD_SHA384 );
        case MBEDTLS_SSL_HASH_SHA512:
            return( MBEDTLS_MD_SHA512 );
#endif
        default:
            return( MBEDTLS_MD_NONE );
    }
}

/*
 * Convert from MBEDTLS_MD_XXX to MBEDTLS_SSL_HASH_XXX
 */
unsigned char mbedtls_ssl_hash_from_md_alg( int md )
{
    switch( md )
    {
#if defined(MBEDTLS_MD5_C)
        case MBEDTLS_MD_MD5:
            return( MBEDTLS_SSL_HASH_MD5 );
#endif
#if defined(MBEDTLS_SHA1_C)
        case MBEDTLS_MD_SHA1:
            return( MBEDTLS_SSL_HASH_SHA1 );
#endif
#if defined(MBEDTLS_SHA256_C)
        case MBEDTLS_MD_SHA224:
            return( MBEDTLS_SSL_HASH_SHA224 );
        case MBEDTLS_MD_SHA256:
            return( MBEDTLS_SSL_HASH_SHA256 );
#endif
#if defined(MBEDTLS_SHA512_C)
        case MBEDTLS_MD_SHA384:
            return( MBEDTLS_SSL_HASH_SHA384 );
        case MBEDTLS_MD_SHA512:
            return( MBEDTLS_SSL_HASH_SHA512 );
#endif
        default:
            return( MBEDTLS_SSL_HASH_NONE );
    }
}

#if defined(MBEDTLS_USE_TINYCRYPT)
/*
 * Check if a curve proposed by the peer is in our list.
 * Return 0 if we're willing to use it, -1 otherwise.
 */
int mbedtls_ssl_check_curve_uecc( const mbedtls_ssl_context *ssl,
                                  mbedtls_uecc_group_id grp_id )
{
    MBEDTLS_SSL_BEGIN_FOR_EACH_SUPPORTED_UECC_GRP_ID( own_ec_id )
    if( own_ec_id == grp_id )
        return( 0 );
    MBEDTLS_SSL_END_FOR_EACH_SUPPORTED_UECC_GRP_ID

    return( -1 );
}
#endif /* MBEDTLS_USE_TINYCRYPT */

#if defined(MBEDTLS_ECP_C)
/*
 * Check if a curve proposed by the peer is in our list.
 * Return 0 if we're willing to use it, -1 otherwise.
 */
int mbedtls_ssl_check_curve( const mbedtls_ssl_context *ssl,
                             mbedtls_ecp_group_id grp_id )
{
    MBEDTLS_SSL_BEGIN_FOR_EACH_SUPPORTED_EC_GRP_ID( own_ec_id )
    if( own_ec_id == grp_id )
        return( 0 );
    MBEDTLS_SSL_END_FOR_EACH_SUPPORTED_EC_GRP_ID

    return( -1 );
}
#endif /* MBEDTLS_ECP_C */

#if defined(MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED)
/*
 * Check if a hash proposed by the peer is in our list.
 * Return 0 if we're willing to use it, -1 otherwise.
 */
int mbedtls_ssl_check_sig_hash( const mbedtls_ssl_context *ssl,
                                mbedtls_md_type_t md )
{
    MBEDTLS_SSL_BEGIN_FOR_EACH_SIG_HASH( md_alg )
    if( md_alg == md )
        return( 0 );
    MBEDTLS_SSL_END_FOR_EACH_SIG_HASH

    return( -1 );
}
#endif /* MBEDTLS_KEY_EXCHANGE__WITH_CERT__ENABLED */

#if defined(MBEDTLS_X509_CRT_PARSE_C)
int mbedtls_ssl_check_cert_usage( const mbedtls_x509_crt *cert,
                          mbedtls_ssl_ciphersuite_handle_t ciphersuite,
                          int cert_endpoint,
                          uint32_t *flags )
{
    int ret = 0;
#if defined(MBEDTLS_X509_CHECK_KEY_USAGE)
    int usage = 0;
#endif
#if defined(MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE)
    const char *ext_oid;
    size_t ext_len;
#endif

#if !defined(MBEDTLS_X509_CHECK_KEY_USAGE) &&          \
    !defined(MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE)
    ((void) cert);
    ((void) cert_endpoint);
    ((void) flags);
#endif

#if defined(MBEDTLS_X509_CHECK_KEY_USAGE)
    if( cert_endpoint == MBEDTLS_SSL_IS_SERVER )
    {
        /* Server part of the key exchange */
        switch( mbedtls_ssl_suite_get_key_exchange( ciphersuite ) )
        {
            case MBEDTLS_KEY_EXCHANGE_RSA:
            case MBEDTLS_KEY_EXCHANGE_RSA_PSK:
                usage = MBEDTLS_X509_KU_KEY_ENCIPHERMENT;
                break;

            case MBEDTLS_KEY_EXCHANGE_DHE_RSA:
            case MBEDTLS_KEY_EXCHANGE_ECDHE_RSA:
            case MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA:
                usage = MBEDTLS_X509_KU_DIGITAL_SIGNATURE;
                break;

            case MBEDTLS_KEY_EXCHANGE_ECDH_RSA:
            case MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA:
                usage = MBEDTLS_X509_KU_KEY_AGREEMENT;
                break;

            /* Don't use default: we want warnings when adding new values */
            case MBEDTLS_KEY_EXCHANGE_NONE:
            case MBEDTLS_KEY_EXCHANGE_PSK:
            case MBEDTLS_KEY_EXCHANGE_DHE_PSK:
            case MBEDTLS_KEY_EXCHANGE_ECDHE_PSK:
            case MBEDTLS_KEY_EXCHANGE_ECJPAKE:
                usage = 0;
        }
    }
    else
    {
        /* Client auth: we only implement rsa_sign and mbedtls_ecdsa_sign for now */
        usage = MBEDTLS_X509_KU_DIGITAL_SIGNATURE;
    }

    if( mbedtls_x509_crt_check_key_usage( cert, usage ) != 0 )
    {
        *flags |= MBEDTLS_X509_BADCERT_KEY_USAGE;
        ret = -1;
    }
#else
    ((void) ciphersuite);
#endif /* MBEDTLS_X509_CHECK_KEY_USAGE */

#if defined(MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE)
    if( cert_endpoint == MBEDTLS_SSL_IS_SERVER )
    {
        ext_oid = MBEDTLS_OID_SERVER_AUTH;
        ext_len = MBEDTLS_OID_SIZE( MBEDTLS_OID_SERVER_AUTH );
    }
    else
    {
        ext_oid = MBEDTLS_OID_CLIENT_AUTH;
        ext_len = MBEDTLS_OID_SIZE( MBEDTLS_OID_CLIENT_AUTH );
    }

    if( mbedtls_x509_crt_check_extended_key_usage( cert, ext_oid, ext_len ) != 0 )
    {
        *flags |= MBEDTLS_X509_BADCERT_EXT_KEY_USAGE;
        ret = -1;
    }
#endif /* MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE */

    return( ret );
}
#endif /* MBEDTLS_X509_CRT_PARSE_C */

#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_1)
int mbedtls_ssl_get_key_exchange_md_ssl_tls( mbedtls_ssl_context *ssl,
                                        unsigned char *output,
                                        unsigned char *data, size_t data_len )
{
    int ret = 0;
    mbedtls_md5_context mbedtls_md5;
    mbedtls_sha1_context mbedtls_sha1;

    mbedtls_md5_init( &mbedtls_md5 );
    mbedtls_sha1_init( &mbedtls_sha1 );

    /*
     * digitally-signed struct {
     *     opaque md5_hash[16];
     *     opaque sha_hash[20];
     * };
     *
     * md5_hash
     *     MD5(ClientHello.random + ServerHello.random
     *                            + ServerParams);
     * sha_hash
     *     SHA(ClientHello.random + ServerHello.random
     *                            + ServerParams);
     */
    if( ( ret = mbedtls_md5_starts_ret( &mbedtls_md5 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md5_starts_ret", ret );
        goto exit;
    }
    if( ( ret = mbedtls_md5_update_ret( &mbedtls_md5,
                                        ssl->handshake->randbytes, 64 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md5_update_ret", ret );
        goto exit;
    }
    if( ( ret = mbedtls_md5_update_ret( &mbedtls_md5, data, data_len ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md5_update_ret", ret );
        goto exit;
    }
    if( ( ret = mbedtls_md5_finish_ret( &mbedtls_md5, output ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md5_finish_ret", ret );
        goto exit;
    }

    if( ( ret = mbedtls_sha1_starts_ret( &mbedtls_sha1 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_sha1_starts_ret", ret );
        goto exit;
    }
    if( ( ret = mbedtls_sha1_update_ret( &mbedtls_sha1,
                                         ssl->handshake->randbytes, 64 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_sha1_update_ret", ret );
        goto exit;
    }
    if( ( ret = mbedtls_sha1_update_ret( &mbedtls_sha1, data,
                                         data_len ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_sha1_update_ret", ret );
        goto exit;
    }
    if( ( ret = mbedtls_sha1_finish_ret( &mbedtls_sha1,
                                         output + 16 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_sha1_finish_ret", ret );
        goto exit;
    }

exit:
    mbedtls_md5_free( &mbedtls_md5 );
    mbedtls_sha1_free( &mbedtls_sha1 );

    if( ret != 0 )
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR );

    return( ret );

}
#endif /* MBEDTLS_SSL_PROTO_SSL3 || MBEDTLS_SSL_PROTO_TLS1 || \
          MBEDTLS_SSL_PROTO_TLS1_1 */

#if defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || \
    defined(MBEDTLS_SSL_PROTO_TLS1_2)
int mbedtls_ssl_get_key_exchange_md_tls1_2( mbedtls_ssl_context *ssl,
                                            unsigned char *hash, size_t *hashlen,
                                            unsigned char *data, size_t data_len,
                                            mbedtls_md_type_t md_alg )
{
    int ret = 0;
    mbedtls_md_context_t ctx;

    mbedtls_md_handle_t md_info = mbedtls_md_info_from_type( md_alg );
    *hashlen = mbedtls_md_get_size( md_info );

    mbedtls_md_init( &ctx );

    /*
     * digitally-signed struct {
     *     opaque client_random[32];
     *     opaque server_random[32];
     *     ServerDHParams params;
     * };
     */
    if( ( ret = mbedtls_md_setup( &ctx, md_info, 0 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md_setup", ret );
        goto exit;
    }
    if( ( ret = mbedtls_md_starts( &ctx ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md_starts", ret );
        goto exit;
    }
    if( ( ret = mbedtls_md_update( &ctx, ssl->handshake->randbytes, 64 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md_update", ret );
        goto exit;
    }
    if( ( ret = mbedtls_md_update( &ctx, data, data_len ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md_update", ret );
        goto exit;
    }
    if( ( ret = mbedtls_md_finish( &ctx, hash ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_md_finish", ret );
        goto exit;
    }

exit:
    mbedtls_md_free( &ctx );

    if( ret != 0 )
        mbedtls_ssl_pend_fatal_alert( ssl,
                                      MBEDTLS_SSL_ALERT_MSG_INTERNAL_ERROR );

    return( ret );
}
#endif /* MBEDTLS_SSL_PROTO_TLS1 || MBEDTLS_SSL_PROTO_TLS1_1 || \
          MBEDTLS_SSL_PROTO_TLS1_2 */

#endif /* MBEDTLS_SSL_TLS_C */
