#pragma once

#include "CryptoProvider.h"
#include <string>

class OpenSSLCryptoProvider : public CryptoProvider {
public:
    OpenSSLCryptoProvider(const std::string& privateKeyPath);
    ~OpenSSLCryptoProvider();

    std::string sign(const std::string& data) override;
    bool verify(const std::string& data, const std::string& signature, const std::string& pubkey) override;

private:
    void* pkey; // EVP_PKEY*, opaque to avoid OpenSSL headers in .h
};