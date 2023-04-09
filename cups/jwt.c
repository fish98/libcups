//
// JSON Web Token API implementation for CUPS.
//
// Copyright © 2023 by OpenPrinting.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

#include "cups-private.h"
#include "jwt.h"
#include "json-private.h"
#ifdef HAVE_OPENSSL
#  include <openssl/ecdsa.h>
#  include <openssl/evp.h>
#  include <openssl/rsa.h>
#else
#  include <gnutls/gnutls.h>
#endif // HAVE_OPENSSL


//
// Constants...
//

#define _CUPS_JWT_MAX_SIGNATURE	2048	// Enough for 512-bit signature


//
// Private types...
//

struct _cups_jwt_s			// JWT object
{
  cups_json_t	*jose;			// JOSE object
  char		*jose_string;		// JOSE string
  cups_json_t	*claims;		// JWT claims object
  char		*claims_string;		// JWT claims string
  cups_jwa_t	sigalg;			// Signature algorithm
  size_t	sigsize;		// Size of signature
  unsigned char	*signature;		// Signature
};


//
// Local globals...
//

static const char * const cups_jwa_strings[CUPS_JWA_MAX] =
{
  "none",				// No algorithm
  "HS256",				// HMAC using SHA-256
  "HS384",				// HMAC using SHA-384
  "HS512",				// HMAC using SHA-512
  "RS256",				// RSASSA-PKCS1-v1_5 using SHA-256
  "RS384",				// RSASSA-PKCS1-v1_5 using SHA-384
  "RS512",				// RSASSA-PKCS1-v1_5 using SHA-512
  "ES256",				// ECDSA using P-256 and SHA-256
  "ES384",				// ECDSA using P-384 and SHA-384
  "ES512"				// ECDSA using P-521 and SHA-512
};
static const char * const cups_jwa_algorithms[CUPS_JWA_MAX] =
{
  NULL,
  "sha2-256",
  "sha2-384",
  "sha2-512",
  "sha2-256",
  "sha2-384",
  "sha2-512",
  "sha2-256",
  "sha2-384",
  "sha2-512"
};


//
// Local functions...
//

#ifdef HAVE_OPENSSL
static BIGNUM	*make_bignum(cups_json_t *jwk, const char *key);
static EC_KEY	*make_ec_key(cups_json_t *jwk, bool verify);
static RSA	*make_rsa(cups_json_t *jwk);
#else // HAVE_GNUTLS
static gnutls_datum_t *make_datum(cups_json_t *jwk, const char *key);
static gnutls_privkey_t *make_private_key(cups_json_t *jwk);
static gnutls_pubkey_t *make_public_key(cups_json_t *jwk);
#endif // HAVE_OPENSSL
static bool	make_signature(cups_jwt_t *jwt, cups_jwa_t alg, cups_json_t *jwk, unsigned char *signature, size_t *sigsize);
static char	*make_string(cups_jwt_t *jwt, bool with_signature);


//
// 'cupsJWTDelete()' - Free the memory used for a JSON Web Token.
//

void
cupsJWTDelete(cups_jwt_t *jwt)		// I - JWT object
{
  if (jwt)
  {
    cupsJSONDelete(jwt->jose);
    free(jwt->jose_string);
    cupsJSONDelete(jwt->claims);
    free(jwt->claims_string);
    free(jwt->signature);
    free(jwt);
  }
}


//
// 'cupsJWTExportString()' - Export a JWT with the JWS Compact Serialization format.
//

char *					// O - JWT/JWS Compact Serialization string
cupsJWTExportString(cups_jwt_t *jwt)	// I - JWT object
{
  if (jwt)
    return (make_string(jwt, true));
  else
    return (NULL);
}


//
// 'cupsJWTGetAlgorithm()' - Get the signature algorithm used by a JSON Web Token.
//

cups_jwa_t				// O - Signature algorithm
cupsJWTGetAlgorithm(cups_jwt_t *jwt)	// I - JWT object
{
  return (jwt ? jwt->sigalg : CUPS_JWA_NONE);
}


//
// 'cupsJWTGetClaimNumber()' - Get the number value of a claim.
//

double					// O - Number value
cupsJWTGetClaimNumber(cups_jwt_t *jwt,	// I - JWT object
                      const char *claim)// I - Claim name
{
  cups_json_t	*node;			// Value node


  if (jwt && (node = cupsJSONFind(jwt->claims, claim)) != NULL)
    return (cupsJSONGetNumber(node));
  else
    return (0.0);
}


//
// 'cupsJWTGetClaimString()' - Get the string value of a claim.
//

const char *				// O - String value
cupsJWTGetClaimString(cups_jwt_t *jwt,	// I - JWT object
                      const char *claim)// I - Claim name
{
  cups_json_t	*node;			// Value node


  if (jwt && (node = cupsJSONFind(jwt->claims, claim)) != NULL)
    return (cupsJSONGetString(node));
  else
    return (NULL);
}


//
// 'cupsJWTGetClaimType()' - Get the value type of a claim.
//

cups_jtype_t				// O - JSON value type
cupsJWTGetClaimType(cups_jwt_t *jwt,	// I - JWT object
                    const char *claim)	// I - Claim name
{
  cups_json_t	*node;			// Value node


  if (jwt && (node = cupsJSONFind(jwt->claims, claim)) != NULL)
    return (cupsJSONGetType(node));
  else
    return (CUPS_JTYPE_NULL);
}


//
// 'cupsJWTGetClaimValue()' - Get the value node of a claim.
//

cups_json_t *				// O - JSON value node
cupsJWTGetClaimValue(cups_jwt_t *jwt,	// I - JWT object
                     const char *claim)	// I - Claim name
{
  if (jwt)
    return (cupsJSONFind(jwt->claims, claim));
  else
    return (NULL);
}


//
// 'cupsJWTGetClaims()' - Get the JWT claims as a JSON object.
//

cups_json_t *				// O - JSON object
cupsJWTGetClaims(cups_jwt_t *jwt)	// I - JWT object
{
  return (jwt ? jwt->claims : NULL);
}


//
// 'cupsJWTHasValidSignature()' - Determine whether the JWT has a valid signature.
//

bool					// O - `true` if value, `false` otherwise
cupsJWTHasValidSignature(
    cups_jwt_t  *jwt,			// I - JWT object
    cups_json_t *jwk)			// I - JWK key set
{
  bool		ret = false;		// Return value
  unsigned char	signature[_CUPS_JWT_MAX_SIGNATURE];
					// Signature
  size_t	sigsize = _CUPS_JWT_MAX_SIGNATURE;
					// Size of signature
  char		*text;			// Signature text
  size_t	text_len;		// Length of signature text
  unsigned char	hash[128];		// Hash
  ssize_t	hash_len;		// Length of hash
#ifdef HAVE_OPENSSL
  RSA		*rsa;			// RSA public key
  EC_KEY	*ec;			// ECDSA public key
  static int	nids[] = { NID_sha256, NID_sha384, NID_sha512 };
					// Hash NIDs
#else // HAVE_GNUTLS
  gnutls_pubkey_t	*key;		// Public key
  gnutls_datum_t	text_datum,	// Text datum
			sig_datum;	// Signature datum
  static gnutls_sign_algorithm_t algs[] = { GNUTLS_DIG_SHA256, GNUTLS_DIG_SHA384, GNUTLS_DIG_SHA512 };
					// Hash algorithms
#endif // HAVE_OPENSSL


  // Range check input...
  if (!jwt || !jwt->signature || !jwk)
    return (false);

  fprintf(stderr, "orig sig(%u) = %02X%02X%02X%02X...%02X%02X%02X%02X\n", (unsigned)jwt->sigsize, jwt->signature[0], jwt->signature[1], jwt->signature[2], jwt->signature[3], jwt->signature[jwt->sigsize - 4], jwt->signature[jwt->sigsize - 3], jwt->signature[jwt->sigsize - 2], jwt->signature[jwt->sigsize - 1]);
 DEBUG_printf(("1cupsJWTHasValidSignature: orig sig(%u) = %02X%02X%02X%02X...%02X%02X%02X%02X", (unsigned)jwt->sigsize, jwt->signature[0], jwt->signature[1], jwt->signature[2], jwt->signature[3], jwt->signature[jwt->sigsize - 4], jwt->signature[jwt->sigsize - 3], jwt->signature[jwt->sigsize - 2], jwt->signature[jwt->sigsize - 1]));
  switch (jwt->sigalg)
  {
    case CUPS_JWA_HS256 :
    case CUPS_JWA_HS384 :
    case CUPS_JWA_HS512 :
	// Calculate signature with keys...
	if (!make_signature(jwt, jwt->sigalg, jwk, signature, &sigsize))
	  break;

	DEBUG_printf(("1cupsJWTHasValidSignature: calc sig(%u) = %02X%02X%02X%02X...%02X%02X%02X%02X", (unsigned)sigsize, signature[0], signature[1], signature[2], signature[3], signature[sigsize - 4], signature[sigsize - 3], signature[sigsize - 2], signature[sigsize - 1]));

	// Compare and return the result...
	ret = jwt->sigsize == sigsize && !memcmp(jwt->signature, signature, sigsize);
	break;

    case CUPS_JWA_RS256 :
    case CUPS_JWA_RS384 :
    case CUPS_JWA_RS512 :
	// Get the message hash...
        text     = make_string(jwt, false);
        text_len = strlen(text);

#ifdef HAVE_OPENSSL
        hash_len = cupsHashData(cups_jwa_algorithms[jwt->sigalg], text, text_len, hash, sizeof(hash));

        if ((rsa = make_rsa(jwk)) != NULL)
        {
	  ret = RSA_verify(nids[jwt->sigalg - CUPS_JWA_RS256], hash, hash_len, jwt->signature, jwt->sigsize, rsa) == 1;

	  RSA_free(rsa);
        }

#else // HAVE_GNUTLS
        if ((key = make_public_key(jwk)) != NULL)
        {
          text_datum.data = text;
          text_datum.size = (unsigned)text_len;
          sig_datum.data  = jwt->signature;
          sig_datum.size  = (unsigned)jwt->sigsize;

          ret = !gnutls_pubkey_verify_data2(key, algs[jwt->sigalg - CUPS_JWA_RS256], 0, &text_datum, &sig_datum);
        }
#endif // HAVE_OPENSSL

        // Free memory
	free(text);
        break;

    case CUPS_JWA_ES256 :
    case CUPS_JWA_ES384 :
    case CUPS_JWA_ES512 :
	// Get the message hash...
        text     = make_string(jwt, false);
        text_len = strlen(text);

#ifdef HAVE_OPENSSL
        hash_len = cupsHashData(cups_jwa_algorithms[jwt->sigalg], text, text_len, hash, sizeof(hash));

        if ((ec = make_ec_key(jwk, true)) != NULL)
        {
	  ret = ECDSA_verify(0, hash, hash_len, jwt->signature, jwt->sigsize, ec) == 1;

	  EC_KEY_free(ec);
        }

#else // HAVE_GNUTLS
        if ((key = make_public_key(jwk)) != NULL)
        {
          text_datum.data = text;
          text_datum.size = (unsigned)text_len;
          sig_datum.data  = jwt->signature;
          sig_datum.size  = (unsigned)jwt->sigsize;

          ret = !gnutls_pubkey_verify_data2(key, algs[jwt->sigalg - CUPS_JWA_ES256], 0, &text_datum, &sig_datum);
        }
#endif // HAVE_OPENSSL

        // Free memory
	free(text);
        break;

    default :
        DEBUG_printf(("1cupsJWTHasValidSignature: Algorithm %d not supported.", jwt->sigalg));
	break;
  }

  return (ret);
}


//
// 'cupsJWTImportString()' - Import a JSON Web Token or JSON Web Signature.
//

cups_jwt_t *				// O - JWT object
cupsJWTImportString(const char *token)	// I - JWS Compact Serialization string
{
  cups_jwt_t	*jwt;			// JWT object
  const char	*tokptr;		// Pointer into the token
  size_t	datalen;		// Size of data
  char		data[65536];		// Data
  const char	*alg;			// Signature algorithm, if any


  // Allocate a JWT...
  if ((jwt = calloc(1, sizeof(cups_jwt_t))) == NULL)
    return (NULL);

  // Extract the JOSE header...
  datalen = sizeof(data) - 1;
  if (!httpDecode64(data, &datalen, token, &tokptr))
    goto import_error;

  if (!tokptr || *tokptr != '.')
    goto import_error;

  tokptr ++;
  data[datalen] = '\0';
  jwt->jose_string = strdup(data);
  if ((jwt->jose = cupsJSONImportString(data)) == NULL)
    goto import_error;

  // Extract the JWT claims...
  datalen = sizeof(data) - 1;
  if (!httpDecode64(data, &datalen, tokptr, &tokptr))
    goto import_error;

  if (!tokptr || *tokptr != '.')
    goto import_error;

  tokptr ++;
  data[datalen] = '\0';
  jwt->claims_string = strdup(data);
  if ((jwt->claims = cupsJSONImportString(data)) == NULL)
    goto import_error;

  // Extract the signature, if any...
  datalen = sizeof(data);
  if (!httpDecode64(data, &datalen, tokptr, &tokptr))
    goto import_error;

  if (!tokptr || *tokptr)
    goto import_error;

  if (datalen > 0)
  {
    if ((jwt->signature = malloc(datalen)) == NULL)
      goto import_error;

    memcpy(jwt->signature, data, datalen);
    jwt->sigsize = datalen;
  }

  if ((alg = cupsJSONGetString(cupsJSONFind(jwt->jose, "alg"))) != NULL)
  {
    cups_jwa_t	sigalg;			// Signing algorithm

    for (sigalg = CUPS_JWA_NONE; sigalg < CUPS_JWA_MAX; sigalg ++)
    {
      if (!strcmp(alg, cups_jwa_strings[sigalg]))
      {
        jwt->sigalg = sigalg;
        break;
      }
    }
  }

  // Can't have signature with none or no signature for !none...
  if ((jwt->sigalg == CUPS_JWA_NONE) != (jwt->sigsize == 0))
    goto import_error;

  // Return the new JWT...
  return (jwt);

  import_error:

  _cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Invalid JSON web token."), 1);
  cupsJWTDelete(jwt);

  return (NULL);
}


//
// 'cupsJWTNew()' - Create a new, empty JSON Web Token.
//

cups_jwt_t *				// O - JWT object
cupsJWTNew(const char *type)		// I - JWT type or `NULL` for default ("JWT")
{
  cups_jwt_t	*jwt;			// JWT object


  if ((jwt = calloc(1, sizeof(cups_jwt_t))) != NULL)
  {
    if ((jwt->jose = cupsJSONNew(NULL, NULL, CUPS_JTYPE_OBJECT)) != NULL)
    {
      cupsJSONNewString(jwt->jose, cupsJSONNewKey(jwt->jose, NULL, "typ"), type ? type : "JWT");

      if ((jwt->claims = cupsJSONNew(NULL, NULL, CUPS_JTYPE_OBJECT)) != NULL)
        return (jwt);
    }
  }

  cupsJWTDelete(jwt);
  return (NULL);
}


//
// 'cupsJWTSetClaimNumber()' - Set a claim number.
//

void
cupsJWTSetClaimNumber(cups_jwt_t *jwt,	// I - JWT object
                      const char *claim,// I - Claim name
                      double     value)	// I - Number value
{
  // Range check input
  if (!jwt || !claim)
    return;

  // Remove existing claim string, if any...
  free(jwt->claims_string);
  jwt->claims_string = NULL;

  // Remove existing claim, if any...
  _cupsJSONDelete(jwt->claims, claim);

  // Add claim...
  cupsJSONNewNumber(jwt->claims, cupsJSONNewKey(jwt->claims, NULL, claim), value);
}


//
// 'cupsJWTSetClaimString()' - Set a claim string.
//

void
cupsJWTSetClaimString(cups_jwt_t *jwt,	// I - JWT object
                      const char *claim,// I - Claim name
                      const char *value)// I - String value
{
  // Range check input
  if (!jwt || !claim || !value)
    return;

  // Remove existing claim string, if any...
  free(jwt->claims_string);
  jwt->claims_string = NULL;

  // Remove existing claim, if any...
  _cupsJSONDelete(jwt->claims, claim);

  // Add claim...
  cupsJSONNewString(jwt->claims, cupsJSONNewKey(jwt->claims, NULL, claim), value);
}


//
// 'cupsJWTSetClaimValue()' - Set a claim value.
//

void
cupsJWTSetClaimValue(
    cups_jwt_t  *jwt,			// I - JWT object
    const char  *claim,			// I - Claim name
    cups_json_t *value)			// I - JSON value node
{
  // Range check input
  if (!jwt || !claim)
    return;

  // Remove existing claim string, if any...
  free(jwt->claims_string);
  jwt->claims_string = NULL;

  // Remove existing claim, if any...
  _cupsJSONDelete(jwt->claims, claim);

  // Add claim...
  _cupsJSONAdd(jwt->claims, cupsJSONNewKey(jwt->claims, NULL, claim), value);
}


//
// 'cupsJWTSign()' - Sign a JSON Web Token, creating a JSON Web Signature.
//

bool					// O - `true` on success, `false` on error
cupsJWTSign(cups_jwt_t  *jwt,		// I - JWT object
            cups_jwa_t  alg,		// I - Signing algorithm
            cups_json_t *jwk)		// I - JWK key set
{
  unsigned char	signature[_CUPS_JWT_MAX_SIGNATURE];
					// Signature
  size_t	sigsize = _CUPS_JWT_MAX_SIGNATURE;
					// Size of signature


  // Range check input...
  if (!jwt || alg <= CUPS_JWA_NONE || alg >= CUPS_JWA_MAX || !jwk)
  {
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, strerror(EINVAL), 0);
    return (false);
  }

  // Remove existing JOSE string, if any...
  free(jwt->jose_string);
  _cupsJSONDelete(jwt->jose, "alg");
  cupsJSONNewString(jwt->jose, cupsJSONNewKey(jwt->jose, NULL, "alg"), cups_jwa_strings[alg]);

  jwt->jose_string = cupsJSONExportString(jwt->jose);

  // Clear existing signature...
  free(jwt->signature);
  jwt->signature = NULL;
  jwt->sigsize   = 0;
  jwt->sigalg    = CUPS_JWA_NONE;

  // Create new signature...
  if (!make_signature(jwt, alg, jwk, signature, &sigsize))
    return (false);

  if ((jwt->signature = malloc(sigsize)) == NULL)
    return (false);

  memcpy(jwt->signature, signature, sigsize);
  jwt->sigalg  = alg;
  jwt->sigsize = sigsize;

  return (true);
}


#ifdef HAVE_OPENSSL
//
// 'make_bignum()' - Make a BIGNUM for the specified key.
//

static BIGNUM *				// O - BIGNUM object or `NULL` on error
make_bignum(cups_json_t *jwk,		// I - JSON web key
            const char  *key)		// I - Object key
{
  const char	*value,			// Key value
		*value_end;		// End of value
  unsigned char	value_bytes[1024];	// Decoded value
  size_t	value_len;		// Length of value


  // See if we have the value...
  if ((value = cupsJSONGetString(cupsJSONFind(jwk, key))) == NULL)
    return (NULL);

  // Decode and validate...
  value_len = sizeof(value_bytes);
  if (!httpDecode64((char *)value_bytes, &value_len, value, &value_end) || (value_end && *value_end))
    return (NULL);

  // Convert to a BIGNUM...
  return (BN_bin2bn(value_bytes, value_len, NULL));
}


//
// 'make_ec_key()' - Make an ECDSA signing/verification object.
//

static EC_KEY *				// O - EC object or `NULL` on error
make_ec_key(cups_json_t *jwk,		// I - JSON web key
            bool        verify)		// I - `true` for verification only, `false` for signing/verification
{
  EC_KEY	*ec = NULL;		// EC object
  EC_GROUP	*group;			// Group parameters
  EC_POINT	*point;			// Public key point
  const char	*crv;			// EC curve ("P-256", "P-384", or "P-521")
  BIGNUM	*x,			// X coordinate
		*y,			// Y coordinate
		*d;			// Private key


  crv = cupsJSONGetString(cupsJSONFind(jwk, "crv"));
  x   = make_bignum(jwk, "x");
  y   = make_bignum(jwk, "y");
  d   = verify ? NULL : make_bignum(jwk, "d");

  if (!crv || ((!x || !y) && !d))
    goto ec_done;

  if (!strcmp(crv, "P-256"))
    ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  else if (!strcmp(crv, "P-384"))
    ec = EC_KEY_new_by_curve_name(NID_secp384r1);
  else if (!strcmp(crv, "P-521"))
    ec = EC_KEY_new_by_curve_name(NID_secp521r1);
  else
    goto ec_done;

  group = (EC_GROUP *)EC_KEY_get0_group(ec);
  point = EC_POINT_new(group);

  if (d)
  {
    // Set private key...
    EC_KEY_set_private_key(ec, d);

    // Create a new public key
    EC_POINT_mul(group, point, d, NULL, NULL, NULL);
  }
  else
  {
    // Create a public key using the supplied coordinates...
    EC_POINT_set_affine_coordinates_GFp(group, point, x, y, NULL);
  }

  // Set public key...
  EC_KEY_set_public_key(ec, point);

  ec_done:

  if (!ec)
  {
    BN_free(x);
    BN_free(y);
    BN_free(d);
  }

  return (ec);
}


//
// 'make_rsa()' - Create an RSA signing/verification object.
//

static RSA *				// O - RSA object or `NULL` on error
make_rsa(cups_json_t *jwk)		// I - JSON web key
{
  RSA		*rsa = NULL;		// RSA object
  BIGNUM	*n,			// Public key modulus
		*e,			// Public key exponent
		*d,			// Private key exponent
		*p,			// Private key first prime factor
		*q,			// Private key second prime factor
		*dp,			// First factor exponent
		*dq,			// Second factor exponent
		*qi;			// First CRT coefficient


  n  = make_bignum(jwk, "n");
  e  = make_bignum(jwk, "e");
  d  = make_bignum(jwk, "d");
  p  = make_bignum(jwk, "p");
  q  = make_bignum(jwk, "q");
  dp = make_bignum(jwk, "dp");
  dq = make_bignum(jwk, "dq");
  qi = make_bignum(jwk, "qi");

  if (!n || !e)
    goto rsa_done;

  rsa = RSA_new();
  RSA_set0_key(rsa, n, e, d);
  if (p && q)
    RSA_set0_factors(rsa, p, q);
  if (dp && dq && qi)
    RSA_set0_crt_params(rsa, dp, dq, qi);

  rsa_done:

  if (!rsa)
  {
    BN_free(n);
    BN_free(e);
    BN_free(d);
    BN_free(p);
    BN_free(q);
    BN_free(dp);
    BN_free(dq);
    BN_free(qi);
  }

  return (rsa);
}


#else // HAVE_GNUTLS
//
// 'make_datum()' - Make a datum value for a parameter.
//

static gnutls_datum_t *			// O - Datum or `NULL`
make_datum(cups_json_t *jwk,		// I - JSON web key
           const char  *key)		// I - Object key
{
  const char		*value,		// Key value
			*value_end;	// End of value
  unsigned char		value_bytes[1024];
					// Decoded value
  size_t		value_len;	// Length of value
  gnutls_datum_t	*datum;		// GNU TLS datum


  // See if we have the value...
  if ((value = cupsJSONGetString(cupsJSONFind(jwk, key))) == NULL)
    return (NULL);

  // Decode and validate...
  value_len = sizeof(value_bytes);
  if (!httpDecode64((char *)value_bytes, &value_len, value, &value_end) || (value_end && *value_end))
    return (NULL);

  // Convert to a datum...
  if ((datum = (gnutls_datum_t *)calloc(1, sizeof(gnutls_datum_t) + value_len)) != NULL)
  {
    // Set pointer and length, and copy value bytes...
    datum->data = (unsigned char *)(datum + 1);
    datum->size = (unsigned)value_len;

    memcpy(datum + 1, value_bytes, value_len);
  }

  return (datum);
}


//
// 'make_private_key()' - Make a private key for EC or RSA signing.
//

static gnutls_privkey_t *		// O - Private key or `NULL`
make_private_key(cups_json_t *jwk)	// I - JSON web key
{
  const char		*kty;		// Key type
  gnutls_privkey_t	*key = NULL;	// Private key


  if ((kty = cupsJSONGetString(cupsJSONFind(jwk, "kty"))) == NULL)
  {
    // No type so we can't load it...
    return (NULL);
  }
  else if (!strcmp(kty, "RSA"))
  {
    // Get RSA parameters...
    gnutls_datum_t	*n,		// Public key modulus
			*e,		// Public key exponent
			*d,		// Private key exponent
			*p,		// Private key first prime factor
			*q,		// Private key second prime factor
			*dp,		// First factor exponent
			*dq,		// Second factor exponent
			*qi;		// First CRT coefficient

    n  = make_datum(jwk, "n");
    e  = make_datum(jwk, "e");
    d  = make_datum(jwk, "d");
    p  = make_datum(jwk, "p");
    q  = make_datum(jwk, "q");
    dp = make_datum(jwk, "dp");
    dq = make_datum(jwk, "dq");
    qi = make_datum(jwk, "qi");

    if (n && e && d && p && q && (key = (gnutls_privkey_t *)calloc(1, sizeof(gnutls_privkey_t))) != NULL)
    {
      // Import RSA private key...
      if (gnutls_privkey_init(key))
      {
	free(key);
	key = NULL;
      }
      else if (gnutls_privkey_import_rsa_raw(key, n, e, d, p, q, qi, dp, dq))
      {
	free(key);
	key = NULL;
      }
    }

    // Free memory...
    free(n);
    free(e);
    free(d);
    free(p);
    free(q);
    free(dp);
    free(dq);
    free(qi);
  }
  else if (!strcmp(kty, "EC"))
  {
    // Get EC parameters...
    const char		*crv;		// EC curve ("P-256", "P-384", or "P-521")
    gnutls_ecc_curve_t	curve;		// Curve constant
    gnutls_datum	*x,		// X coordinate
			*y,		// Y coordinate
			*d;		// Private key

    crv = cupsJSONGetString(cupsJSONFind(jwk, "crv"));

    if (!crv)
      return (NULL);
    else if (!strcmp(crv, "P-256"))
      curve = GNUTLS_ECC_CURVE_SECP256R1;
    else if (!strcmp(crv, "P-384"))
      curve = GNUTLS_ECC_CURVE_SECP384R1;
    else if (!strcmp(crv, "P-521"))
      curve = GNUTLS_ECC_CURVE_SECP521R1;
    else
      return (NULL);

    x = make_datum(jwk, "x");
    y = make_datum(jwk, "y");
    d = make_datum(jwk, "d");

    if (x && y && d && (key = (gnutls_privkey_t *)calloc(1, sizeof(gnutls_privkey_t))) != NULL)
    {
      // Import EC private key...
      if (gnutls_privkey_init(key))
      {
	free(key);
	key = NULL;
      }
      else if (gnutls_privkey_import_ecc_raw(key, curve, x, y, d))
      {
	free(key);
	key = NULL;
      }
    }

    // Free memory...
    free(x);
    free(y);
    free(d);
  }

  // Return whatever key we got...
  return (key);
}


//
// 'make_public_key()' - Make a public key for EC or RSA verification.
//

static gnutls_pubkey_t *		// O - Public key or `NULL`
make_public_key(cups_json_t *jwk)	// I - JSON web key
{
  const char		*kty;		// Key type
  gnutls_pubkey_t	*key = NULL;	// Private key


  if ((kty = cupsJSONGetString(cupsJSONFind(jwk, "kty"))) == NULL)
  {
    // No type so we can't load it...
    return (NULL);
  }
  else if (!strcmp(kty, "RSA"))
  {
    // Get RSA parameters...
    gnutls_datum_t	*n,		// Public key modulus
			*e;		// Public key exponent


    n  = make_bignum(jwk, "n");
    e  = make_bignum(jwk, "e");

    if (n && e && (key = (gnutls_pubkey_t *)calloc(1, sizeof(gnutls_pubkey_t))) != NULL)
    {
      // Import RSA private key...
      if (gnutls_pubkey_init(key))
      {
	free(key);
	key = NULL;
      }
      else if (gnutls_pubkey_import_rsa_raw(key, n, e))
      {
	free(key);
	key = NULL;
      }
    }

    // Free memory and return...
    free(n);
    free(e);
  }
  else if (!strcmp(kty, "EC"))
  {
    // Get EC parameters...
    const char		*crv;		// EC curve ("P-256", "P-384", or "P-521")
    gnutls_ecc_curve_t	curve;		// Curve constant
    gnutls_datum	*x,		// X coordinate
			*y,		// Y coordinate
			*d;		// Private key

    crv = cupsJSONGetString(cupsJSONFind(jwk, "crv"));

    if (!crv)
      return (NULL);
    else if (!strcmp(crv, "P-256"))
      curve = GNUTLS_ECC_CURVE_SECP256R1;
    else if (!strcmp(crv, "P-384"))
      curve = GNUTLS_ECC_CURVE_SECP384R1;
    else if (!strcmp(crv, "P-521"))
      curve = GNUTLS_ECC_CURVE_SECP521R1;
    else
      return (NULL);

    x = make_datum(jwk, "x");
    y = make_datum(jwk, "y");
    d = make_datum(jwk, "d");

    if (x && y && d && (key = (gnutls_privkey_t *)calloc(1, sizeof(gnutls_privkey_t))) != NULL)
    {
      // Import EC private key...
      if (gnutls_privkey_init(key))
      {
	free(key);
	key = NULL;
      }
      else if (gnutls_privkey_import_ecc_raw(key, curve, x, y, d))
      {
	free(key);
	key = NULL;
      }
    }

    // Free memory...
    free(x);
    free(y);
    free(d);
  }

  return (key);
}
#endif // HAVE_OPENSSL


//
// 'make_signature()' - Make a signature.
//

static bool				// O  - `true` on success, `false` on failure
make_signature(cups_jwt_t    *jwt,	// I  - JWT
               cups_jwa_t    alg,	// I  - Algorithm
               cups_json_t   *jwk,	// I  - JSON Web Key Set
               unsigned char *signature,// I  - Signature buffer
               size_t        *sigsize)	// IO - Signature size
{
  bool			ret = false;	// Return value
  char			*text;		// JWS Signing Input
  size_t		text_len;	// Length of signing input
#ifdef HAVE_OPENSSL
  static int		nids[] = { NID_sha256, NID_sha384, NID_sha512 };
					// Hash NIDs
#else // HAVE_GNUTLS
  gnutls_privkey_t	*key;		// Private key
  gnutls_datum_t	text_datum,	// Text datum
			sig_datum;	// Signature datum
  static gnutls_sign_algorithm_t algs[] = { GNUTLS_DIG_SHA256, GNUTLS_DIG_SHA384, GNUTLS_DIG_SHA512 };
					// Hash algorithms
#endif // HAVE_OPENSSL


  // Initialize default signature...
  *sigsize = 0;

  // Get text to sign...
  text     = make_string(jwt, false);
  text_len = strlen(text);

  if (alg >= CUPS_JWA_HS256 && alg <= CUPS_JWA_HS512)
  {
    // SHA-256/384/512 HMAC
    const char		*k;		// "k" value
    unsigned char	key[256];	// Key value
    size_t		key_len;	// Length of key
    ssize_t		hmac_len;	// Length of HMAC

    // Get key...
    memset(key, 0, sizeof(key));
    k       = cupsJSONGetString(cupsJSONFind(jwk, "k"));
    key_len = sizeof(key);
    if (!httpDecode64((char *)key, &key_len, k, NULL))
      goto done;

    if ((hmac_len = cupsHMACData(cups_jwa_algorithms[alg], key, key_len, text, text_len, signature, _CUPS_JWT_MAX_SIGNATURE)) < 0)
      goto done;

    *sigsize = (size_t)hmac_len;
    ret      = true;
  }
  else if (alg >= CUPS_JWA_RS256 && alg <= CUPS_JWA_RS512)
  {
    // RSASSA-PKCS1-v1_5 SHA-256/384/512
    unsigned char hash[128];		// SHA-256/384/512 hash
    ssize_t	hash_len;		// Length of hash
    unsigned	siglen = (unsigned)*sigsize;
					// Length of signature
#ifdef HAVE_OPENSSL
    RSA		*rsa;			// RSA public/private key

    if ((rsa = make_rsa(jwk)) != NULL)
    {
      hash_len = cupsHashData(cups_jwa_algorithms[alg], text, text_len, hash, sizeof(hash));
      if (RSA_sign(nids[alg - CUPS_JWA_RS256], hash, hash_len, signature, &siglen, rsa) == 1)
      {
        *sigsize = siglen;
        ret      = true;
      }

      RSA_free(rsa);
    }
#else // HAVE_GNUTLS
    if ((key = make_private_key(jwk)) != NULL)
    {
      text_datum.data = text;
      text_datum.size = (unsigned)text_len;
      sig_datum.data  = NULL;
      sig_datum.size  = 0;

      if (!gnutls_privkey_sign_data(key, algs[alg - CUPS_JWA_RS256], 0, &hash_datum, &sig_datum) && sig_datum.size <= *sigsize)
      {
        memcpy(signature, sig_datum.data, sig_datum.size);
        *sigsize = sig_datum.size;
        ret      = true;
      }

      gnutls_free(sig_datum.data);
      gnutls_privkey_deinit(key);
      free(key);
    }
#endif // HAVE_OPENSSL
  }
  else if (alg >= CUPS_JWA_ES256 && alg <= CUPS_JWA_ES512)
  {
    // ECDSA P-256 SHA-256/384/512
    unsigned char hash[128];		// SHA-256/384/512 hash
    ssize_t	hash_len;		// Length of hash
    unsigned	siglen = (unsigned)*sigsize;
					// Length of signature
#ifdef HAVE_OPENSSL
    EC_KEY		*ec;		// EC public/private key

    if ((ec = make_ec_key(jwk, false)) != NULL)
    {
      hash_len = cupsHashData(cups_jwa_algorithms[alg], text, text_len, hash, sizeof(hash));
      if (ECDSA_sign(0, hash, hash_len, signature, &siglen, ec) == 1)
      {
        *sigsize = siglen;
        ret      = true;
      }

      EC_KEY_free(ec);
    }
#else // HAVE_GNUTLS
    if ((key = make_private_key(jwk)) != NULL)
    {
      text_datum.data = text;
      text_datum.size = (unsigned)text_len;
      sig_datum.data  = NULL;
      sig_datum.size  = 0;

      if (!gnutls_privkey_sign_data(key, algs[alg - CUPS_JWA_ES256], 0, &hash_datum, &sig_datum) && sig_datum.size <= *sigsize)
      {
        memcpy(signature, sig_datum.data, sig_datum.size);
        *sigsize = sig_datum.size;
        ret      = true;
      }

      gnutls_free(sig_datum.data);
      gnutls_privkey_deinit(key);
      free(key);
    }
#endif // HAVE_OPENSSL
  }

  done:

  free(text);

  return (ret);
}


//
// 'make_string()' - Make a JWT/JWS Compact Serialization string.
//

static char *				// O - JWT/JWS string
make_string(cups_jwt_t *jwt,		// I - JWT object
            bool       with_signature)	// I - Include signature field?
{
  char		*s = NULL,		// JWT/JWS string
		*ptr,			// Pointer into string
		*end;			// End of string
  size_t	jose_len,		// Length of JOSE header
		claims_len,		// Length of claims string
		len;			// Allocation length


  // Get the JOSE header and claims object strings...
  if (!jwt->claims_string)
    jwt->claims_string = cupsJSONExportString(jwt->claims);

  if (!jwt->jose_string || !jwt->claims_string)
    return (NULL);

  jose_len   = strlen(jwt->jose_string);
  claims_len = strlen(jwt->claims_string);

  // Calculate the maximum Base64URL-encoded string length...
  len = ((jose_len + 2) * 4 / 3) + 1 + ((claims_len + 2) * 4 / 3) + 1 + ((_CUPS_JWT_MAX_SIGNATURE + 2) * 4 / 3) + 1;

  if ((s = malloc(len)) == NULL)
    return (NULL);

  ptr = s;
  end = s + len;

  httpEncode64(ptr, (size_t)(end - ptr), jwt->jose_string, jose_len, true);
  ptr += strlen(ptr);
  *ptr++ = '.';

  httpEncode64(ptr, (size_t)(end - ptr), jwt->claims_string, claims_len, true);
  ptr += strlen(ptr);

  if (with_signature)
  {
    *ptr++ = '.';

    if (jwt->sigsize)
      httpEncode64(ptr, (size_t)(end - ptr), (char *)jwt->signature, jwt->sigsize, true);
  }

  return (s);
}