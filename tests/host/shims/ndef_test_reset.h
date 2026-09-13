#pragma once
// Host-test hook into the NDEF engine (ndef.cpp): reset the emulated
// tag state so tests start clean. Only meaningful in the host build.
#ifdef NUCULA_HOST_TEST
#ifdef __cplusplus
extern "C" {
#endif
void ndef_test_reset(void);
#ifdef __cplusplus
}
#endif
#endif
