/* Minimal stand-in so headers that mention CURL compile.
   The Xbox build has no PSN/internet remote play, so curl is never used. */
#ifndef CHIAKI_XBOX_FAKE_CURL_H
#define CHIAKI_XBOX_FAKE_CURL_H
typedef void CURL;
#endif
