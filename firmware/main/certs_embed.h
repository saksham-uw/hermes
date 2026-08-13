#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char hermes_root_ca_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const char hermes_root_ca_end[] asm("_binary_AmazonRootCA1_pem_end");
extern const char hermes_device_cert_start[] asm("_binary_device_cert_pem_start");
extern const char hermes_device_cert_end[] asm("_binary_device_cert_pem_end");
extern const char hermes_device_key_start[] asm("_binary_device_private_key_start");
extern const char hermes_device_key_end[] asm("_binary_device_private_key_end");

static inline size_t hermes_pem_len(const char *start, const char *end) {
  return (size_t)(end - start);
}

#ifdef __cplusplus
}
#endif
