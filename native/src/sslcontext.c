/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** SSL Context wrapper
 *
 * @author Mladen Turk
 * @version $Id$
 */

#include "tcn.h"

#include "apr_file_io.h"
#include "apr_thread_mutex.h"
#include "apr_poll.h"

#ifdef HAVE_OPENSSL
#include "ssl_private.h"

#define KEY_NONE      0
#define KEY_PRIMARY   1
#define KEY_SECONDARY 2
#define KEY_SINGLE    3

struct SSL_ticket_key {
    unsigned char type;
    unsigned char padding[15];
    unsigned char name[16];
    unsigned char aes_key[16];
    unsigned char hmac_key[16];
};

struct OCSP_staple {
    int len;
    unsigned char *data;
};

int ssl_session_ticket_keys_index = -1;

static apr_status_t ssl_context_cleanup(void *data)
{
    tcn_ssl_ctxt_t *c = (tcn_ssl_ctxt_t *)data;
    if (c) {
        int i;
        if (c->crl)
            X509_STORE_free(c->crl);
        c->crl = NULL;
        if (c->ctx)
            SSL_CTX_free(c->ctx);
        c->ctx = NULL;
        for (i = 0; i < SSL_AIDX_MAX; i++) {
            if (c->certs[i]) {
                X509_free(c->certs[i]);
                c->certs[i] = NULL;
            }
            if (c->keys[i]) {
                EVP_PKEY_free(c->keys[i]);
                c->keys[i] = NULL;
            }
        }
        if (c->bio_is) {
            SSL_BIO_close(c->bio_is);
            c->bio_is = NULL;
        }
        if (c->bio_os) {
            SSL_BIO_close(c->bio_os);
            c->bio_os = NULL;
        }
    }
    return APR_SUCCESS;
}

/* Initialize server context */
TCN_IMPLEMENT_CALL(jlong, SSLContext, make)(TCN_STDARGS, jlong pool,
                                            jint protocol, jint mode)
{
    apr_pool_t *p = J2P(pool, apr_pool_t *);
    tcn_ssl_ctxt_t *c = NULL;
    SSL_CTX *ctx = NULL;
    UNREFERENCED(o);

    if (protocol == SSL_PROTOCOL_TLSV1_2) {
#ifdef SSL_OP_NO_TLSv1_2
        if (mode == SSL_MODE_CLIENT)
            ctx = SSL_CTX_new(TLSv1_2_client_method());
        else if (mode == SSL_MODE_SERVER)
            ctx = SSL_CTX_new(TLSv1_2_server_method());
        else
            ctx = SSL_CTX_new(TLSv1_2_method());
#endif
    } else if (protocol == SSL_PROTOCOL_TLSV1_1) {
#ifdef SSL_OP_NO_TLSv1_1
        if (mode == SSL_MODE_CLIENT)
            ctx = SSL_CTX_new(TLSv1_1_client_method());
        else if (mode == SSL_MODE_SERVER)
            ctx = SSL_CTX_new(TLSv1_1_server_method());
        else
            ctx = SSL_CTX_new(TLSv1_1_method());
#endif
    } else if (protocol == SSL_PROTOCOL_TLSV1) {
        if (mode == SSL_MODE_CLIENT)
            ctx = SSL_CTX_new(TLSv1_client_method());
        else if (mode == SSL_MODE_SERVER)
            ctx = SSL_CTX_new(TLSv1_server_method());
        else
            ctx = SSL_CTX_new(TLSv1_method());
    } else if (protocol == SSL_PROTOCOL_SSLV3) {
        if (mode == SSL_MODE_CLIENT)
            ctx = SSL_CTX_new(SSLv3_client_method());
        else if (mode == SSL_MODE_SERVER)
            ctx = SSL_CTX_new(SSLv3_server_method());
        else
            ctx = SSL_CTX_new(SSLv3_method());
#ifndef OPENSSL_NO_SSL2
    } else if (protocol == SSL_PROTOCOL_SSLV2) {
        if (mode == SSL_MODE_CLIENT)
            ctx = SSL_CTX_new(SSLv2_client_method());
        else if (mode == SSL_MODE_SERVER)
            ctx = SSL_CTX_new(SSLv2_server_method());
        else
            ctx = SSL_CTX_new(SSLv2_method());
#endif
#ifndef SSL_OP_NO_TLSv1_2
    } else if (protocol & SSL_PROTOCOL_TLSV1_2) {
        /* requested but not supported */
#endif
#ifndef SSL_OP_NO_TLSv1_1
    } else if (protocol & SSL_PROTOCOL_TLSV1_1) {
        /* requested but not supported */
#endif
    } else {
        if (mode == SSL_MODE_CLIENT)
            ctx = SSL_CTX_new(SSLv23_client_method());
        else if (mode == SSL_MODE_SERVER)
            ctx = SSL_CTX_new(SSLv23_server_method());
        else
            ctx = SSL_CTX_new(SSLv23_method());
    }

    if (!ctx) {
        char err[256];
        ERR_error_string(ERR_get_error(), err);
        tcn_Throw(e, "Invalid Server SSL Protocol (%s)", err);
        goto init_failed;
    }
    if ((c = apr_pcalloc(p, sizeof(tcn_ssl_ctxt_t))) == NULL) {
        tcn_ThrowAPRException(e, apr_get_os_error());
        goto init_failed;
    }

    c->protocol = protocol;
    c->mode     = mode;
    c->ctx      = ctx;
    c->pool     = p;
    c->bio_os   = BIO_new(BIO_s_file());
    if (c->bio_os != NULL)
        BIO_set_fp(c->bio_os, stderr, BIO_NOCLOSE | BIO_FP_TEXT);
    SSL_CTX_set_options(c->ctx, SSL_OP_ALL);
    if (!(protocol & SSL_PROTOCOL_SSLV2))
        SSL_CTX_set_options(c->ctx, SSL_OP_NO_SSLv2);
    if (!(protocol & SSL_PROTOCOL_SSLV3))
        SSL_CTX_set_options(c->ctx, SSL_OP_NO_SSLv3);
    if (!(protocol & SSL_PROTOCOL_TLSV1))
        SSL_CTX_set_options(c->ctx, SSL_OP_NO_TLSv1);
#ifdef SSL_OP_NO_TLSv1_1
    if (!(protocol & SSL_PROTOCOL_TLSV1_1))
        SSL_CTX_set_options(c->ctx, SSL_OP_NO_TLSv1_1);
#endif
#ifdef SSL_OP_NO_TLSv1_2
    if (!(protocol & SSL_PROTOCOL_TLSV1_2))
        SSL_CTX_set_options(c->ctx, SSL_OP_NO_TLSv1_2);
#endif
    /*
     * Configure additional context ingredients
     */
    SSL_CTX_set_options(c->ctx, SSL_OP_SINGLE_DH_USE);
#ifdef HAVE_ECC
    SSL_CTX_set_options(c->ctx, SSL_OP_SINGLE_ECDH_USE);
#endif

#ifdef SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION
    /*
     * Disallow a session from being resumed during a renegotiation,
     * so that an acceptable cipher suite can be negotiated.
     */
    SSL_CTX_set_options(c->ctx, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
#endif
    /* Default session context id and cache size */
    SSL_CTX_sess_set_cache_size(c->ctx, SSL_DEFAULT_CACHE_SIZE);
    EVP_Digest((const unsigned char *)SSL_DEFAULT_VHOST_NAME,
               (unsigned long)((sizeof SSL_DEFAULT_VHOST_NAME) - 1),
               &(c->context_id[0]), NULL, EVP_sha1(), NULL);
    if (mode) {
#ifdef HAVE_ECC
        /* Set default (nistp256) elliptic curve for ephemeral ECDH keys */
        EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        SSL_CTX_set_tmp_ecdh(c->ctx, ecdh);
        EC_KEY_free(ecdh);
#endif
        SSL_CTX_set_tmp_rsa_callback(c->ctx, SSL_callback_tmp_RSA);
        SSL_CTX_set_tmp_dh_callback(c->ctx,  SSL_callback_tmp_DH);
    }
    /* Set default Certificate verification level
     * and depth for the Client Authentication
     */
    c->verify_depth  = 1;
    c->verify_mode   = SSL_CVERIFY_UNSET;
    c->shutdown_type = SSL_SHUTDOWN_TYPE_UNSET;

    /* Set default password callback */
    SSL_CTX_set_default_passwd_cb(c->ctx, (pem_password_cb *)SSL_password_callback);
    SSL_CTX_set_default_passwd_cb_userdata(c->ctx, (void *)(&tcn_password_callback));
    SSL_CTX_set_info_callback(c->ctx, SSL_callback_handshake);
    /*
     * Let us cleanup the ssl context when the pool is destroyed
     */
    apr_pool_cleanup_register(p, (const void *)c,
                              ssl_context_cleanup,
                              apr_pool_cleanup_null);

    return P2J(c);
init_failed:
    return 0;
}

TCN_IMPLEMENT_CALL(jint, SSLContext, free)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    UNREFERENCED_STDARGS;
    TCN_ASSERT(ctx != 0);
    /* Run and destroy the cleanup callback */
    return apr_pool_cleanup_run(c->pool, c, ssl_context_cleanup);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setContextId)(TCN_STDARGS, jlong ctx,
                                                   jstring id)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    TCN_ALLOC_CSTRING(id);

    TCN_ASSERT(ctx != 0);
    UNREFERENCED(o);
    if (J2S(id)) {
        EVP_Digest((const unsigned char *)J2S(id),
                   (unsigned long)strlen(J2S(id)),
                   &(c->context_id[0]), NULL, EVP_sha1(), NULL);
    }
    TCN_FREE_CSTRING(id);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setBIO)(TCN_STDARGS, jlong ctx,
                                             jlong bio, jint dir)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    BIO *bio_handle   = J2P(bio, BIO *);

    UNREFERENCED_STDARGS;
    TCN_ASSERT(ctx != 0);
    if (dir == 0) {
        if (c->bio_os && c->bio_os != bio_handle)
            SSL_BIO_close(c->bio_os);
        c->bio_os = bio_handle;
    }
    else if (dir == 1) {
        if (c->bio_is && c->bio_is != bio_handle)
            SSL_BIO_close(c->bio_is);
        c->bio_is = bio_handle;
    }
    else
        return;
    SSL_BIO_doref(bio_handle);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setOptions)(TCN_STDARGS, jlong ctx,
                                                 jint opt)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);

    UNREFERENCED_STDARGS;
    TCN_ASSERT(ctx != 0);
#ifndef SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION
    /* Clear the flag if not supported */
    if (opt & 0x00040000)
        opt &= ~0x00040000;
#endif
    SSL_CTX_set_options(c->ctx, opt);
}

TCN_IMPLEMENT_CALL(void, SSLContext, clearOptions)(TCN_STDARGS, jlong ctx,
                                                   jint opt)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);

    UNREFERENCED_STDARGS;
    TCN_ASSERT(ctx != 0);
    SSL_CTX_clear_options(c->ctx, opt);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setQuietShutdown)(TCN_STDARGS, jlong ctx,
                                                       jboolean mode)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);

    UNREFERENCED_STDARGS;
    TCN_ASSERT(ctx != 0);
    SSL_CTX_set_quiet_shutdown(c->ctx, mode ? 1 : 0);
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCipherSuite)(TCN_STDARGS, jlong ctx,
                                                         jstring ciphers)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    TCN_ALLOC_CSTRING(ciphers);
    jboolean rv = JNI_TRUE;

    UNREFERENCED(o);
    TCN_ASSERT(ctx != 0);
    if (!J2S(ciphers))
        return JNI_FALSE;

    if (!SSL_CTX_set_cipher_list(c->ctx, J2S(ciphers))) {
        char err[256];
        ERR_error_string(ERR_get_error(), err);
        tcn_Throw(e, "Unable to configure permitted SSL ciphers (%s)", err);
        rv = JNI_FALSE;
    }
    TCN_FREE_CSTRING(ciphers);
    return rv;
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCARevocation)(TCN_STDARGS, jlong ctx,
                                                          jstring file,
                                                          jstring path)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    TCN_ALLOC_CSTRING(file);
    TCN_ALLOC_CSTRING(path);
    jboolean rv = JNI_FALSE;
    X509_LOOKUP *lookup;
    char err[256];

    UNREFERENCED(o);
    TCN_ASSERT(ctx != 0);
    if (J2S(file) == NULL && J2S(path) == NULL)
        return JNI_FALSE;

    if (!c->crl) {
        if ((c->crl = X509_STORE_new()) == NULL)
            goto cleanup;
    }
    if (J2S(file)) {
        lookup = X509_STORE_add_lookup(c->crl, X509_LOOKUP_file());
        if (lookup == NULL) {
            ERR_error_string(ERR_get_error(), err);
            X509_STORE_free(c->crl);
            c->crl = NULL;
            tcn_Throw(e, "Lookup failed for file %s (%s)", J2S(file), err);
            goto cleanup;
        }
        X509_LOOKUP_load_file(lookup, J2S(file), X509_FILETYPE_PEM);
    }
    if (J2S(path)) {
        lookup = X509_STORE_add_lookup(c->crl, X509_LOOKUP_hash_dir());
        if (lookup == NULL) {
            ERR_error_string(ERR_get_error(), err);
            X509_STORE_free(c->crl);
            c->crl = NULL;
            tcn_Throw(e, "Lookup failed for path %s (%s)", J2S(file), err);
            goto cleanup;
        }
        X509_LOOKUP_add_dir(lookup, J2S(path), X509_FILETYPE_PEM);
    }
    rv = JNI_TRUE;
cleanup:
    TCN_FREE_CSTRING(file);
    TCN_FREE_CSTRING(path);
    return rv;
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCertificateChainFile)(TCN_STDARGS, jlong ctx,
                                                                  jstring file,
                                                                  jboolean skipfirst)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jboolean rv = JNI_FALSE;
    TCN_ALLOC_CSTRING(file);

    UNREFERENCED(o);
    TCN_ASSERT(ctx != 0);
    if (!J2S(file))
        return JNI_FALSE;
    if (SSL_CTX_use_certificate_chain(c->ctx, J2S(file), skipfirst) > 0)
        rv = JNI_TRUE;
    TCN_FREE_CSTRING(file);
    return rv;
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCACertificate)(TCN_STDARGS,
                                                           jlong ctx,
                                                           jstring file,
                                                           jstring path)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jboolean rv = JNI_TRUE;
    TCN_ALLOC_CSTRING(file);
    TCN_ALLOC_CSTRING(path);

    UNREFERENCED(o);
    TCN_ASSERT(ctx != 0);
    if (file == NULL && path == NULL)
        return JNI_FALSE;

   /*
     * Configure Client Authentication details
     */
    if (!SSL_CTX_load_verify_locations(c->ctx,
                                       J2S(file), J2S(path))) {
        char err[256];
        ERR_error_string(ERR_get_error(), err);
        tcn_Throw(e, "Unable to configure locations "
                  "for client authentication (%s)", err);
        rv = JNI_FALSE;
        goto cleanup;
    }
    c->store = SSL_CTX_get_cert_store(c->ctx);
    if (c->mode) {
        STACK_OF(X509_NAME) *ca_certs;
        c->ca_certs++;
        ca_certs = SSL_CTX_get_client_CA_list(c->ctx);
        if (ca_certs == NULL) {
            SSL_load_client_CA_file(J2S(file));
            if (ca_certs != NULL)
                SSL_CTX_set_client_CA_list(c->ctx, ca_certs);
        }
        else {
            if (!SSL_add_file_cert_subjects_to_stack(ca_certs, J2S(file)))
                ca_certs = NULL;
        }
        if (ca_certs == NULL && c->verify_mode == SSL_CVERIFY_REQUIRE) {
            /*
             * Give a warning when no CAs were configured but client authentication
             * should take place. This cannot work.
            */
            BIO_printf(c->bio_os,
                        "[WARN] Oops, you want to request client "
                        "authentication, but no CAs are known for "
                        "verification!?");
        }
    }
cleanup:
    TCN_FREE_CSTRING(file);
    TCN_FREE_CSTRING(path);
    return rv;
}

TCN_IMPLEMENT_CALL(void, SSLContext, setShutdownType)(TCN_STDARGS, jlong ctx,
                                                      jint type)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);

    UNREFERENCED_STDARGS;
    TCN_ASSERT(ctx != 0);
    c->shutdown_type = type;
}

TCN_IMPLEMENT_CALL(void, SSLContext, setVerify)(TCN_STDARGS, jlong ctx,
                                                jint level, jint depth)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    int verify = SSL_VERIFY_NONE;

    UNREFERENCED(o);
    TCN_ASSERT(ctx != 0);
    c->verify_mode = level;

    if (c->verify_mode == SSL_CVERIFY_UNSET)
        c->verify_mode = SSL_CVERIFY_NONE;
    if (depth > 0)
        c->verify_depth = depth;
    /*
     *  Configure callbacks for SSL context
     */
    if (c->verify_mode == SSL_CVERIFY_REQUIRE)
        verify |= SSL_VERIFY_PEER_STRICT;
    if ((c->verify_mode == SSL_CVERIFY_OPTIONAL) ||
        (c->verify_mode == SSL_CVERIFY_OPTIONAL_NO_CA))
        verify |= SSL_VERIFY_PEER;
    if (!c->store) {
        if (SSL_CTX_set_default_verify_paths(c->ctx)) {
            c->store = SSL_CTX_get_cert_store(c->ctx);
            X509_STORE_set_flags(c->store, 0);
        }
        else {
            /* XXX: See if this is fatal */
        }
    }

    SSL_CTX_set_verify(c->ctx, verify, SSL_callback_SSL_verify);
}

static EVP_PKEY *load_pem_key(tcn_ssl_ctxt_t *c, const char *file)
{
    BIO *bio = NULL;
    EVP_PKEY *key = NULL;
    tcn_pass_cb_t *cb_data = c->cb_data;
    int i;

    if ((bio = BIO_new(BIO_s_file())) == NULL) {
        return NULL;
    }
    if (BIO_read_filename(bio, file) <= 0) {
        BIO_free(bio);
        return NULL;
    }
    if (!cb_data)
        cb_data = &tcn_password_callback;
    for (i = 0; i < 3; i++) {
        key = PEM_read_bio_PrivateKey(bio, NULL,
                    (pem_password_cb *)SSL_password_callback,
                    (void *)cb_data);
        if (key)
            break;
        cb_data->password[0] = '\0';
        BIO_ctrl(bio, BIO_CTRL_RESET, 0, NULL);
    }
    BIO_free(bio);
    return key;
}

static X509 *load_pem_cert(tcn_ssl_ctxt_t *c, const char *file)
{
    BIO *bio = NULL;
    X509 *cert = NULL;
    tcn_pass_cb_t *cb_data = c->cb_data;

    if ((bio = BIO_new(BIO_s_file())) == NULL) {
        return NULL;
    }
    if (BIO_read_filename(bio, file) <= 0) {
        BIO_free(bio);
        return NULL;
    }
    if (!cb_data)
        cb_data = &tcn_password_callback;
    cert = PEM_read_bio_X509_AUX(bio, NULL,
                (pem_password_cb *)SSL_password_callback,
                (void *)cb_data);
    if (cert == NULL &&
       (ERR_GET_REASON(ERR_peek_last_error()) == PEM_R_NO_START_LINE)) {
        ERR_clear_error();
        BIO_ctrl(bio, BIO_CTRL_RESET, 0, NULL);
        cert = d2i_X509_bio(bio, NULL);
    }
    BIO_free(bio);
    return cert;
}

static int ssl_load_pkcs12(tcn_ssl_ctxt_t *c, const char *file,
                           EVP_PKEY **pkey, X509 **cert, STACK_OF(X509) **ca)
{
    const char *pass;
    char        buff[PEM_BUFSIZE];
    int         len, rc = 0;
    PKCS12     *p12;
    BIO        *in;
    tcn_pass_cb_t *cb_data = c->cb_data;

    if ((in = BIO_new(BIO_s_file())) == 0)
        return 0;
    if (BIO_read_filename(in, file) <= 0) {
        BIO_free(in);
        return 0;
    }
    p12 = d2i_PKCS12_bio(in, 0);
    if (p12 == 0) {
        /* Error loading PKCS12 file */
        goto cleanup;
    }
    /* See if an empty password will do */
    if (PKCS12_verify_mac(p12, "", 0) || PKCS12_verify_mac(p12, 0, 0)) {
        pass = "";
    }
    else {
        if (!cb_data)
            cb_data = &tcn_password_callback;
        len = SSL_password_callback(buff, PEM_BUFSIZE, 0, cb_data);
        if (len < 0) {
            /* Passpharse callback error */
            goto cleanup;
        }
        if (!PKCS12_verify_mac(p12, buff, len)) {
            /* Mac verify error (wrong password?) in PKCS12 file */
            goto cleanup;
        }
        pass = buff;
    }
    rc = PKCS12_parse(p12, pass, pkey, cert, ca);
cleanup:
    if (p12 != 0)
        PKCS12_free(p12);
    BIO_free(in);
    return rc;
}

TCN_IMPLEMENT_CALL(void, SSLContext, setRandom)(TCN_STDARGS, jlong ctx,
                                                jstring file)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    TCN_ALLOC_CSTRING(file);

    TCN_ASSERT(ctx != 0);
    UNREFERENCED(o);
    if (J2S(file))
        c->rand_file = apr_pstrdup(c->pool, J2S(file));
    TCN_FREE_CSTRING(file);
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCertificate)(TCN_STDARGS, jlong ctx,
                                                         jstring cert, jstring key,
                                                         jstring password, jint idx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jboolean rv = JNI_TRUE;
    TCN_ALLOC_CSTRING(cert);
    TCN_ALLOC_CSTRING(key);
    TCN_ALLOC_CSTRING(password);
    const char *key_file, *cert_file;
    const char *p;
    char err[256];

    UNREFERENCED(o);
    TCN_ASSERT(ctx != 0);

    if (idx < 0 || idx >= SSL_AIDX_MAX) {
        /* TODO: Throw something */
        rv = JNI_FALSE;
        goto cleanup;
    }
    if (J2S(password)) {
        if (!c->cb_data)
            c->cb_data = &tcn_password_callback;
        strncpy(c->cb_data->password, J2S(password), SSL_MAX_PASSWORD_LEN);
        c->cb_data->password[SSL_MAX_PASSWORD_LEN-1] = '\0';
    }
    key_file  = J2S(key);
    cert_file = J2S(cert);
    if (!key_file)
        key_file = cert_file;
    if (!key_file || !cert_file) {
        tcn_Throw(e, "No Certificate file specified or invalid file format");
        rv = JNI_FALSE;
        goto cleanup;
    }
    if ((p = strrchr(cert_file, '.')) != NULL && strcmp(p, ".pkcs12") == 0) {
        if (!ssl_load_pkcs12(c, cert_file, &c->keys[idx], &c->certs[idx], 0)) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "Unable to load certificate %s (%s)",
                      cert_file, err);
            rv = JNI_FALSE;
            goto cleanup;
        }
    }
    else {
        if ((c->keys[idx] = load_pem_key(c, key_file)) == NULL) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "Unable to load certificate key %s (%s)",
                      key_file, err);
            rv = JNI_FALSE;
            goto cleanup;
        }
        if ((c->certs[idx] = load_pem_cert(c, cert_file)) == NULL) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "Unable to load certificate %s (%s)",
                      cert_file, err);
            rv = JNI_FALSE;
            goto cleanup;
        }
    }
    if (SSL_CTX_use_certificate(c->ctx, c->certs[idx]) <= 0) {
        ERR_error_string(ERR_get_error(), err);
        tcn_Throw(e, "Error setting certificate (%s)", err);
        rv = JNI_FALSE;
        goto cleanup;
    }
    if (SSL_CTX_use_PrivateKey(c->ctx, c->keys[idx]) <= 0) {
        ERR_error_string(ERR_get_error(), err);
        tcn_Throw(e, "Error setting private key (%s)", err);
        rv = JNI_FALSE;
        goto cleanup;
    }
    if (SSL_CTX_check_private_key(c->ctx) <= 0) {
        ERR_error_string(ERR_get_error(), err);
        tcn_Throw(e, "Private key does not match the certificate public key (%s)",
                  err);
        rv = JNI_FALSE;
        goto cleanup;
    }
cleanup:
    TCN_FREE_CSTRING(cert);
    TCN_FREE_CSTRING(key);
    TCN_FREE_CSTRING(password);
    return rv;
}

static int ticket_key_callback(SSL* ssl, unsigned char key_name[16], unsigned char* iv,
                               EVP_CIPHER_CTX* evp_ctx, HMAC_CTX* hmac_ctx, int new_session) {

    struct SSL_ticket_key* key = SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ssl_session_ticket_keys_index);
    struct SSL_ticket_key* secondary_key = NULL;

    if (key == NULL) {
        return -1;
    } else if (key->type == KEY_PRIMARY) {
        secondary_key = key + 1;
    } else if (key->type == KEY_SECONDARY) {
        secondary_key = key++;
    }

    if (new_session) {
        RAND_pseudo_bytes(iv, 16);
        EVP_EncryptInit_ex(evp_ctx, EVP_aes_128_cbc(), NULL, key->aes_key, iv);
        HMAC_Init_ex(hmac_ctx, key->hmac_key, 16, EVP_sha256(), NULL);
        memcpy(key_name, key->name, 16);
        return 1;
    } else {
        if (memcmp(key_name, key->name, 16) == 0) {
            HMAC_Init_ex(hmac_ctx, key->hmac_key, 16, EVP_sha256(), NULL);
            EVP_DecryptInit_ex(evp_ctx, EVP_aes_128_cbc(), NULL, key->aes_key, iv);
            return 1;
        } else if (secondary_key != NULL && memcmp(key_name, secondary_key->name, 16) == 0) {
            HMAC_Init_ex(hmac_ctx, secondary_key->hmac_key, 16, EVP_sha256(), NULL);
            EVP_DecryptInit_ex(evp_ctx, EVP_aes_128_cbc(), NULL, secondary_key->aes_key, iv);
            return 2;
        }
        return 0;
    }
}

void ticket_key_free(void *parent, void *ptr, CRYPTO_EX_DATA *ad, int idx, long argl, void *argp) {
    free(ptr);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setSessionTicketKey)(TCN_STDARGS, jlong ctx,
                                                              jbyteArray key)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);

    if (ssl_session_ticket_keys_index == -1) {
        ssl_session_ticket_keys_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, ticket_key_free);
    }

    if (ssl_session_ticket_keys_index == -1) {
        tcn_Throw(e, "SSL_CTX_get_ex_new_index() failed");
    }

    struct SSL_ticket_key* app_data_key = SSL_CTX_get_ex_data(c->ctx, ssl_session_ticket_keys_index);

    if (app_data_key == NULL) {
        app_data_key = malloc(2 * sizeof(struct SSL_ticket_key));
        app_data_key->type = KEY_NONE;
        if (SSL_CTX_set_ex_data(c->ctx, ssl_session_ticket_keys_index, app_data_key) == 0) {
            tcn_Throw(e, "SSL_CTX_set_ex_data() failed");
        }
    }

   if (key == NULL) {
        SSL_CTX_set_tlsext_ticket_key_cb(c->ctx, NULL);
        app_data_key->type = KEY_NONE;
    } else if ((*e)->GetArrayLength(e, key) != 48) {
        tcn_Throw(e, "TLS ticket key must be 48 bytes long");
    } else {
        struct SSL_ticket_key* new_key;
        unsigned char new_type;

        switch (app_data_key->type) {
            case KEY_NONE:
                new_key = app_data_key;
                new_type = KEY_SINGLE;
                break;
            case KEY_SECONDARY:
                new_key = app_data_key;
                new_type = KEY_PRIMARY;
                break;
            default:
                new_key = app_data_key + 1;
                new_type = KEY_SECONDARY;
        }

        (*e)->GetByteArrayRegion(e, key, 0, 48, (jbyte*)&new_key->name);
        app_data_key->type = new_type;

        SSL_CTX_set_tlsext_ticket_key_cb(c->ctx, ticket_key_callback);
    }
}

TCN_IMPLEMENT_CALL(void, SSLContext, setSessionCacheTimeout)(TCN_STDARGS, jlong ctx,
                                                             jlong timeout)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    SSL_CTX_set_timeout(c->ctx, timeout);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setDHParameters)(TCN_STDARGS, jlong ctx,
                                                             jstring file)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    TCN_ALLOC_CSTRING(file);

    TCN_ASSERT(ctx != 0);
    UNREFERENCED(o);

    BIO *bio = NULL;
    DH *dh = NULL;
    char err[256];

    if (J2S(file)) {
        if ((bio = BIO_new(BIO_s_file())) == NULL) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "BIO_new() failed: %s");
            goto cleanup;
        }

        if (BIO_read_filename(bio, J2S(file)) <= 0) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "Error reading file %s: %s", J2S(file), err);
            goto cleanup;
        }

        dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);

        if (dh == NULL) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "PEM_read_bio_DHparams() failed: %s", err);
            goto cleanup;
        }

        if (SSL_CTX_set_tmp_dh(c->ctx, dh) != 1) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "Error setting DHParams: %s", err);
            goto cleanup;
        }
    }

cleanup:
    TCN_FREE_CSTRING(file);
    if (bio != NULL)
        BIO_free(bio);
    if (dh != NULL)
        DH_free(dh);
}

static int ocsp_stapling_cb(SSL *ssl, void *data) {
    struct OCSP_staple *staple = (struct OCSP_staple *) data;

    // have to make a copy of the OCSP response
    // because openssl will make free this var on the context termination
    unsigned char *response_copy = (unsigned char *) malloc(staple->len);

    if (response_copy == NULL)
         return SSL_TLSEXT_ERR_ALERT_FATAL;

    memcpy(response_copy, staple->data, staple->len);

    SSL_set_tlsext_status_ocsp_resp(ssl, response_copy, staple->len);

    return SSL_TLSEXT_ERR_OK;
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setOCSPStaplingFile)(TCN_STDARGS, jlong ctx,
                                                             jstring file)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    TCN_ALLOC_CSTRING(file);

    TCN_ASSERT(ctx != 0);
    UNREFERENCED(o);

    BIO *bio = NULL;
    OCSP_RESPONSE *response = NULL;
    int len;
    unsigned char *buf = NULL;
    struct OCSP_staple *staple = NULL;
    char err[256];

    if (J2S(file)) {

        if ((bio = BIO_new(BIO_s_file())) == NULL) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "BIO_new() failed: %s", err);
            goto cleanup;
        }

        if (BIO_read_filename(bio, J2S(file)) <= 0) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "Error reading file %s: %s",
                    J2S(file), err);
            goto cleanup;
        }

        response = d2i_OCSP_RESPONSE_bio(bio, NULL);
        if (response == NULL) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "Error parsing OCSP response file %s: %s",
                    J2S(file), err);
            goto cleanup;
        }

        len = i2d_OCSP_RESPONSE(response, NULL);
        if (len <= 0) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "i2d_OCSP_RESPONSE() failed: %s", err);
            goto cleanup;
        }

        buf = (char *) malloc(len);
        if (buf == NULL) {
            tcn_Throw(e, "buf malloc() failed");
            goto cleanup;
        }

        len = i2d_OCSP_RESPONSE(response, &buf);
        if (len <= 0) {
            ERR_error_string(ERR_get_error(), err);
            tcn_Throw(e, "i2d_OCSP_RESPONSE() filed: %s", err);
            free(buf);
            goto cleanup;
        }

        staple = (struct OCSP_staple *) malloc(sizeof(struct OCSP_staple));
        if (staple == NULL) {
            tcn_Throw(e, "staple malloc() failed");
            free(buf);
            goto cleanup;
        }

        staple->data = buf - len;
        staple->len = len;

        SSL_CTX_set_tlsext_status_cb(c->ctx, ocsp_stapling_cb);
        SSL_CTX_set_tlsext_status_arg(c->ctx, staple);
    }

cleanup:
    TCN_FREE_CSTRING(file);
    if (bio != NULL)
        BIO_free(bio);
    if (response != NULL)
        OCSP_RESPONSE_free(response);
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionNumber)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_number(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionConnect)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_connect(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionConnectGood)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_connect_good(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionConnectRenegotiate)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_connect_renegotiate(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionAccept)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_accept(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionAcceptGood)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_accept_good(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionAcceptRenegotiate)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_accept_renegotiate(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionHits)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_hits(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionCbHits)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_cb_hits(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionMisses)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_misses(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionTimeouts)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_timeouts(c->ctx);
    return rv;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionCacheFull)(TCN_STDARGS, jlong ctx)
{
    tcn_ssl_ctxt_t *c = J2P(ctx, tcn_ssl_ctxt_t *);
    jlong rv = SSL_CTX_sess_cache_full(c->ctx);
    return rv;
}

#else
/* OpenSSL is not supported.
 * Create empty stubs.
 */

TCN_IMPLEMENT_CALL(jlong, SSLContext, make)(TCN_STDARGS, jlong pool,
                                            jint protocol, jint mode)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(pool);
    UNREFERENCED(protocol);
    UNREFERENCED(mode);
    return 0;
}

TCN_IMPLEMENT_CALL(jint, SSLContext, free)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return APR_ENOTIMPL;
}

TCN_IMPLEMENT_CALL(void, SSLContext, setContextId)(TCN_STDARGS, jlong ctx,
                                                   jstring id)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(id);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setBIO)(TCN_STDARGS, jlong ctx,
                                             jlong bio, jint dir)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(bio);
    UNREFERENCED(dir);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setOptions)(TCN_STDARGS, jlong ctx,
                                                 jint opt)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(opt);
}

TCN_IMPLEMENT_CALL(void, SSLContext, clearOptions)(TCN_STDARGS, jlong ctx,
                                                   jint opt)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(opt);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setQuietShutdown)(TCN_STDARGS, jlong ctx,
                                                       jboolean mode)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(mode);
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCipherSuite)(TCN_STDARGS, jlong ctx,
                                                         jstring ciphers)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(ciphers);
    return JNI_FALSE;
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCARevocation)(TCN_STDARGS, jlong ctx,
                                                          jstring file,
                                                          jstring path)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(file);
    UNREFERENCED(path);
    return JNI_FALSE;
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCertificateChainFile)(TCN_STDARGS, jlong ctx,
                                                                  jstring file,
                                                                  jboolean skipfirst)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(file);
    UNREFERENCED(skipfirst);
    return JNI_FALSE;
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCACertificate)(TCN_STDARGS,
                                                           jlong ctx,
                                                           jstring file,
                                                           jstring path)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(file);
    UNREFERENCED(path);
    return JNI_FALSE;
}

TCN_IMPLEMENT_CALL(void, SSLContext, setShutdownType)(TCN_STDARGS, jlong ctx,
                                                      jint type)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(type);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setVerify)(TCN_STDARGS, jlong ctx,
                                                jint level, jint depth)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(level);
    UNREFERENCED(depth);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setRandom)(TCN_STDARGS, jlong ctx,
                                                jstring file)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(file);
}

TCN_IMPLEMENT_CALL(jboolean, SSLContext, setCertificate)(TCN_STDARGS, jlong ctx,
                                                         jstring cert, jstring key,
                                                         jstring password, jint idx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(cert);
    UNREFERENCED(key);
    UNREFERENCED(password);
    UNREFERENCED(idx);
    return JNI_FALSE;
}

TCN_IMPLEMENT_CALL(void, SSLContext, setSessionTicketKey)(TCN_STDARGS, jlong ctx,
                                                              jbyteArray key)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(key);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setSessionCacheTimeout)(TCN_STDARGS, jlong ctx,
                                                             jlong timeout)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(timeout);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setDHParameters)(TCN_STDARGS, jlong ctx,
                                                             jstring file)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(file);
}

TCN_IMPLEMENT_CALL(void, SSLContext, setOCSPStaplingFile)(TCN_STDARGS, jlong ctx,
                                                             jstring file)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    UNREFERENCED(file);
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionNumber)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionConnect)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionConnectGood)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionConnectRenegotiate)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionAccept)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionAcceptGood)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionAcceptRenegotiate)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionHits)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionCbHits)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionMisses)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionTimeouts)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

TCN_IMPLEMENT_CALL(jlong, SSLContext, sessionCacheFull)(TCN_STDARGS, jlong ctx)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(ctx);
    return 0;
}

#endif
