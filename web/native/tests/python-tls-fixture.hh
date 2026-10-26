/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <string>

inline void create_python_tls_fixture(const char *directory)
{
  if (mkdir(directory, 0700)) assert(errno == EEXIST);
  EVP_PKEY *key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
  X509 *cert = X509_new();
  assert(key && cert);
  assert(X509_set_version(cert, 2) == 1);
  assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 17) == 1);
  assert(X509_gmtime_adj(X509_getm_notBefore(cert), -60));
  assert(X509_gmtime_adj(X509_getm_notAfter(cert), 86400));
  assert(X509_set_pubkey(cert, key) == 1);
  X509_NAME *name = X509_get_subject_name(cert);
  assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>("blender-web.test"), -1, -1, 0) == 1);
  assert(X509_set_issuer_name(cert, name) == 1);
  X509V3_CTX ctx;
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  for (auto [id, value] : {std::pair<int, const char *>(NID_basic_constraints, "critical,CA:TRUE"),
                         {NID_subject_alt_name, "DNS:blender-web.test"}}) {
    X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &ctx, id, value);
    assert(extension && X509_add_ext(cert, extension, -1) == 1);
    X509_EXTENSION_free(extension);
  }
  assert(X509_sign(cert, key, EVP_sha256()) > 0);
  FILE *file = fopen((std::string(directory) + "/test.cert.pem").c_str(), "wb");
  assert(file && PEM_write_X509(file, cert) == 1);
  fclose(file);
  file = fopen((std::string(directory) + "/test.key.pem").c_str(), "wb");
  assert(file && PEM_write_PrivateKey(file, key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
  fclose(file);
  X509_free(cert);
  EVP_PKEY_free(key);
}
