//
// OAuth API definitions for CUPS.
//
// Copyright © 2024 by OpenPrinting.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

#ifndef _CUPS_OAUTH_H_
#  define _CUPS_OAUTH_H_
#  include "base.h"
#  ifdef __cplusplus
extern "C" {
#  endif /* __cplusplus */


//
// Functions...
//

extern void		cupsOAuthClearTokens(const char *auth_server, const char *res_server) _CUPS_PUBLIC;
extern const char	*cupsOAuthGetAuthToken(const char *auth_server, const char *res_server, char *token, size_t toksize) _CUPS_PUBLIC;
extern const char	*cupsOAuthGetRefreshToken(const char *auth_server, const char *res_server, char *token, size_t toksize) _CUPS_PUBLIC;
extern void		cupsOAuthSetTokens(const char *auth_server, const char *res_server, const char *auth_token, const char *refresh_token) _CUPS_PUBLIC;


#  ifdef __cplusplus
}
#  endif /* __cplusplus */
#endif // !_CUPS_OAUTH_H_
