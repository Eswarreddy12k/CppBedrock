#pragma once

#include <string>

class CryptoProvider {
public:
    virtual ~CryptoProvider() = default;

    // Sign the data, return signature as base64 or hex string
    virtual std::string sign(const std::string& data) = 0;

    // Verify the signature using the given public key
    virtual bool verify(const std::string& data, const std::string& signature, const std::string& pubkey) = 0;
};