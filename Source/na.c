/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include "NetworkAuthenticationHelper.h"
#include "NetworkAuthenticationHelperGSS.h"
#include "KerberosHelper.h"
#include "KerberosHelperContext.h"

#include <Heimdal/krb5.h>
#include <Heimdal/hx509.h>

#include <GSS/gssapi.h>
#include <GSS/gssapi_ntlm.h>
#include <GSS/gssapi_krb5.h>
#include <GSS/gssapi_spi.h>

#include <Kernel/gssd/gssd_mach_types.h>

#include <CoreFoundation/CFRuntime.h>
#include <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>

#include <CoreServices/CoreServices.h>
#include <CoreServices/CoreServicesPriv.h>

#include "LKDCHelper.h"

static void add_user_selections(NAHRef na);


const CFStringRef kNAHServiceAFPServer = CFSTR("afpserver");
const CFStringRef kNAHServiceCIFSServer = CFSTR("cifs");
const CFStringRef kNAHServiceHostServer = CFSTR("host");
const CFStringRef kNAHServiceVNCServer = CFSTR("vnc");

static const char *nah_created = "nah-created";

static bool nah_use_gss_uam = false;
static dispatch_once_t init_globals;

enum NAHMechType {
    NO_MECH = 0,
    GSS_KERBEROS,
    GSS_KERBEROS_U2U,
    GSS_KERBEROS_IAKERB,
    GSS_KERBEROS_PKU2U,
    GSS_NTLM
};

struct NAHSelectionData {
    CFRuntimeBase base;
    NAHRef na;
    dispatch_semaphore_t wait;
    int waiting;
    int have_cred;
    int canceled;

    enum NAHMechType mech;
    CFStringRef client;
    CFStringRef clienttype;
    CFStringRef server;
    CFStringRef servertype;

    SecCertificateRef certificate;
    bool spnego;

    CFStringRef inferredLabel;

    krb5_ccache ccache;
};

struct NAHData {
    CFRuntimeBase base;
    CFAllocatorRef alloc;

    /* Input */

    CFMutableStringRef hostname;
    CFStringRef service;
    CFStringRef username;
    CFStringRef specificname; 
    CFDictionaryRef servermechs;

    CFStringRef spnegoServerName;

    /* credentials */
    CFArrayRef x509identities;
    CFStringRef password;

    CFArrayRef mechs;

    /* intermediate results */
    dispatch_queue_t q;
    dispatch_queue_t bgq;
    krb5_context context;
    hx509_context hxctx;

    /* final result */

    CFMutableArrayRef selections;
};

static void signal_result(NAHSelectionRef selection);
static Boolean wait_result(NAHSelectionRef selection);
static void nalog(CFStringRef fmt, ...)
    __attribute__ ((format(CFString, 1, 2)));

#define CFRELEASE(x) do { if ((x)) { CFRelease((x)); (x) = NULL; } } while(0)

/*
 *
 */

static void
nalog(CFStringRef fmt, ...)
{
    CFStringRef str;
    va_list ap;
    const char *s;

    va_start(ap, fmt);
    str = CFStringCreateWithFormatAndArguments(NULL, 0, fmt, ap);
    va_end(ap);

    if (str == NULL)
	return;

    if (__KRBCreateUTF8StringFromCFString (str, &s) == 0) {
	asl_log(NULL, NULL, ASL_LEVEL_DEBUG, "%s", s);
	__KRBReleaseUTF8String(s);
    }
    CFRelease(str);
}

static void
naselrelease(NAHSelectionRef nasel)
{
    if (nasel->waiting != 0)
	abort();
    if (nasel->wait)
	dispatch_release(nasel->wait);
    CFRELEASE(nasel->client);
    CFRELEASE(nasel->clienttype);
    CFRELEASE(nasel->server);
    CFRELEASE(nasel->servertype);
    if (nasel->ccache)
	krb5_cc_close(nasel->na->context, nasel->ccache);
    CFRELEASE(nasel->certificate);
    CFRELEASE(nasel->inferredLabel);
}

static CFStringRef
naseldebug(NAHSelectionRef nasel)
{
    if (!wait_result(nasel))
	return CFSTR("selection canceled");

    CFStringRef mech = NAHSelectionGetInfoForKey(nasel, kNAHMechanism);
    CFStringRef innermech = NAHSelectionGetInfoForKey(nasel, kNAHInnerMechanism);

    return CFStringCreateWithFormat(NULL, NULL,
				    CFSTR("<NetworkAuthenticationSelection: "
					  "%@<%@>, %@ %@ spnego: %s>"),
				    mech, innermech,
				    nasel->client,
				    nasel->server,
				    nasel->spnego ? "yes" : "no");
}

static CFStringRef
naselformatting(CFTypeRef cf, CFDictionaryRef formatOptions)
{
    return naseldebug((NAHSelectionRef)cf);
}

const CFStringRef kNAHErrorDomain = CFSTR("com.apple.NetworkAuthenticationHelper");

static void
createError(CFAllocatorRef alloc, CFErrorRef *error, CFIndex errorcode, CFStringRef fmt, ...)
{
    void *keys[1] = { (void *)CFSTR("NetworkAuthenticationHelper") };
    void *values[1];
    va_list va;

    if (error == NULL)
	return;

    va_start(va, fmt);
    values[0] = (void *)CFStringCreateWithFormatAndArguments(alloc, NULL, fmt, va);
    va_end(va);
    if (values[0] == NULL) {
	*error = NULL;
	return;
    }
    
    nalog(CFSTR("NAH: error: %@"), values[0]);

    *error = CFErrorCreateWithUserInfoKeysAndValues(alloc, kNAHErrorDomain, errorcode,
						    (const void * const *)keys,
						    (const void * const *)values, 1);
    CFRelease(values[0]);
}

static struct {
    CFStringRef name;
    enum NAHMechType mech;
} mechs[] = {
    { CFSTR("Kerberos"), GSS_KERBEROS },
    { CFSTR("KerberosUser2User"), GSS_KERBEROS_U2U },
    { CFSTR("PKU2U"), GSS_KERBEROS_PKU2U },
    { CFSTR("IAKerb"), GSS_KERBEROS_IAKERB },
    { CFSTR("NTLM"), GSS_NTLM }
};

static enum NAHMechType
name2mech(CFStringRef name)
{
    size_t n;

    if (name == NULL)
	return NO_MECH;

    for (n = 0; n < sizeof(mechs) / sizeof(mechs[0]); n++) {
	if (CFStringCompare(mechs[n].name, name, kCFCompareCaseInsensitive) == kCFCompareEqualTo)
	    return mechs[n].mech;
    }
    return NO_MECH;
}

static CFStringRef
mech2name(enum NAHMechType mech)
{
    size_t n;
    for (n = 0; n < sizeof(mechs) / sizeof(mechs[0]); n++) {
	if (mechs[n].mech == mech)
	    return mechs[n].name;
    }
    return NULL;
}

static CFTypeID
NAHSelectionGetTypeID(void)
{
    static CFTypeID naselid = _kCFRuntimeNotATypeID;
    static dispatch_once_t inited;

    dispatch_once(&inited, ^{
	    static const CFRuntimeClass naselclass = {
		0,
		"NetworkAuthenticationSelection",
		NULL,
		NULL,
		(void(*)(CFTypeRef))naselrelease,
		NULL,
		NULL,
		naselformatting,
		(CFStringRef (*)(CFTypeRef))naseldebug
	    };
	    naselid = _CFRuntimeRegisterClass(&naselclass);
	});

    return naselid;
}

static NAHSelectionRef
NAHSelectionAlloc(NAHRef na)
{
    CFTypeID id = NAHSelectionGetTypeID();
    NAHSelectionRef nasel;

    if (id == _kCFRuntimeNotATypeID)
	return NULL;

    nasel = (NAHSelectionRef)_CFRuntimeCreateInstance(na->alloc, id, sizeof(struct NAHSelectionData) - sizeof(CFRuntimeBase), NULL);
    if (nasel == NULL)
	return NULL;

    nasel->na = na;
    nasel->spnego = true;

    return nasel;

}

static void
nahrelease(NAHRef na)
{
    CFRELEASE(na->hostname);
    CFRELEASE(na->service);
    CFRELEASE(na->username);
    CFRELEASE(na->specificname);
    CFRELEASE(na->servermechs);
    CFRELEASE(na->spnegoServerName);

    CFRELEASE(na->x509identities);
    CFRELEASE(na->password);

    CFRELEASE(na->mechs);

    CFRELEASE(na->selections);

    if (na->q)
	dispatch_release(na->q);
    if (na->context)
	krb5_free_context(na->context);
    if (na->hxctx)
	hx509_context_free(&na->hxctx);
}

static CFTypeID
NAGetTypeID(void)
{
    static CFTypeID naid = _kCFRuntimeNotATypeID;
    static dispatch_once_t inited;

    dispatch_once(&inited, ^{
	    static const CFRuntimeClass naclass = {
		0,
		"NetworkAuthentication",
		NULL,
		NULL,
		(void(*)(CFTypeRef))nahrelease,
		NULL,
		NULL,
		NULL,
		NULL
	    };
	    naid = _CFRuntimeRegisterClass(&naclass);
	});

    return naid;
}

/*
 *
 */

static NAHRef
NAAlloc(CFAllocatorRef alloc)
{
    CFTypeID id = NAGetTypeID();
    NAHRef na;

    if (id == _kCFRuntimeNotATypeID)
	return NULL;

    na = (NAHRef)_CFRuntimeCreateInstance(alloc, id, sizeof(struct NAHData) - sizeof(CFRuntimeBase), NULL);
    if (na == NULL)
	return NULL;

    na->q = dispatch_queue_create("network-authentication", NULL);
    na->bgq = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    na->alloc = alloc;

    return na;
}

static bool
haveMech(NAHRef na, CFStringRef mech)
{
    if (na->servermechs == NULL)
	return false;
    if (CFDictionaryGetValue(na->servermechs, mech) != NULL)
	return true;
    return false;
}

/*
 * Status of selection
 */

const CFStringRef kNAHSelectionHaveCredential = CFSTR("kNAHSelectionHaveCredential");
const CFStringRef kNAHSelectionUserPrintable = CFSTR("kNAHSelectionUserPrintable");
const CFStringRef kNAHClientPrincipal = CFSTR("kNAHClientPrincipal");
const CFStringRef kNAHServerPrincipal = CFSTR("kNAHServerPrincipal");
const CFStringRef kNAHMechanism  = CFSTR("kNAHMechanism");
const CFStringRef kNAHInnerMechanism = CFSTR("kNAHInnerMechanism");
const CFStringRef kNAHCredentialType  = CFSTR("kNAHCredentialType");
const CFStringRef kNAHUseSPNEGO  = CFSTR("kNAHUseSPNEGO");

const CFStringRef kNAHClientNameType = CFSTR("kNAHClientNameType");
const CFStringRef kNAHClientNameTypeGSSD = CFSTR("kNAHClientNameTypeGSSD");

const CFStringRef kNAHServerNameType = CFSTR("kNAHServerNameType");
const CFStringRef kNAHServerNameTypeGSSD = CFSTR("kNAHServerNameTypeGSSD");

const CFStringRef kNAHNTUsername = CFSTR("kNAHNTUsername");
const CFStringRef kNAHNTServiceBasedName = CFSTR("kNAHNTServiceBasedName");
const CFStringRef kNAHNTKRB5PrincipalReferral = CFSTR("kNAHNTKRB5PrincipalReferral");
const CFStringRef kNAHNTKRB5Principal = CFSTR("kNAHNTKRB5Principal");
const CFStringRef kNAHNTUUID = CFSTR("kNAHNTUUID");


const CFStringRef kNAHInferredLabel = CFSTR("kNAHInferredLabel");


/*
 * Add selection
 */

enum {
    USE_SPNEGO = 1,
    FORCE_ADD = 2
};

static NAHSelectionRef
addSelection(NAHRef na,
	     CFStringRef client,
	     CFStringRef clienttype,
	     CFStringRef server,
	     CFStringRef servertype,
	     enum NAHMechType mech,
	     int *duplicate,
	     unsigned long flags)
{
    NAHSelectionRef nasel;
    int matching;
    CFIndex n;
    
    if (clienttype == NULL)
	clienttype = kNAHNTUsername;

    if (servertype == NULL)
	servertype = kNAHNTServiceBasedName;

    matching = (flags & FORCE_ADD) || (na->specificname == NULL) || CFStringHasPrefix(client, na->specificname);

    nalog(CFSTR("addSelection: %@ (%d) %@ %@ %s %s"),
	  mech2name(mech), (int)mech, client, server, (flags & USE_SPNEGO) ? "SPNEGO" : "raw",
	  matching ? "matching" : "no-matching");
    
    /* If no matching, skip this credential */
    if (matching == 0)
	return NULL;

    /* check for dups */
    for (n = 0; n < CFArrayGetCount(na->selections); n++) {
	nasel = (NAHSelectionRef)CFArrayGetValueAtIndex(na->selections, n);
	if (nasel->mech != mech)
	    continue;
	if (CFStringCompare(nasel->client, client, 0) != kCFCompareEqualTo)
	    continue;
	if (nasel->server && server && CFStringCompare(nasel->server, server, 0) != kCFCompareEqualTo)
	    continue;
	if (CFStringCompare(nasel->servertype, servertype, 0) != kCFCompareEqualTo)
	    continue;
	if (duplicate)
	    *duplicate = 1;
	return nasel;
    }
    if (duplicate)
	*duplicate = 0;

    nasel = NAHSelectionAlloc(na);
    if (nasel == NULL)
	return NULL;

    nasel->client = CFRetain(client);
    if (server) {
	nasel->server = CFRetain(server);
	nasel->waiting = 0;
	nasel->wait = NULL;
    } else {
	nasel->server = NULL;
	nasel->waiting = 0;
	nasel->wait = dispatch_semaphore_create(0);
    }
    nasel->clienttype = clienttype;
    CFRetain(clienttype);
    nasel->servertype = servertype;
    CFRetain(servertype);

    nasel->mech = mech;
    nasel->spnego = (flags & USE_SPNEGO) ? true : false;

    CFArrayAppendValue(na->selections, nasel);

    CFRelease(nasel); /* referenced by array */

    return nasel;
}

static bool
findUsername(CFAllocatorRef alloc, NAHRef na, CFDictionaryRef info)
{
    char *name;

    if (info) {
	na->username = CFDictionaryGetValue(info, kNAHUserName);
	if (na->username) {
	    CFRange range, ur;

	    CFRetain(na->username);

	    if (CFStringFindWithOptions(na->username, CFSTR("@"), CFRangeMake(0, CFStringGetLength(na->username)), 0, &range)) {
		ur.location = 0;
		ur.length = range.location;
		na->specificname = CFStringCreateWithSubstring(na->alloc, na->username, ur);
	    } else if (CFStringFindWithOptions(na->username, CFSTR("\\"), CFRangeMake(0, CFStringGetLength(na->username)), 0, &range)) {
		ur.location = range.location + 1;
		ur.length = CFStringGetLength(na->username) - ur.location;
		na->specificname = CFStringCreateWithSubstring(na->alloc, na->username, ur);
	    } else {
		na->specificname = na->username;
		CFRetain(na->specificname);
	    }

	    nalog(CFSTR("NAH: specific name is: %@ foo"), na->specificname);

	    return true;
	}
    }

    name = getlogin();
    if (name == NULL)
	return false;

    na->username = CFStringCreateWithCString(alloc, name, kCFStringEncodingUTF8);
    if (na->username == NULL)
	return false;

    return true;
}

static void
classic_lkdc_background(NAHSelectionRef nasel)
{
    LKDCHelperErrorType ret;
    char *hostname = NULL, *realm = NULL;
    CFStringRef u;

    ret = __KRBCreateUTF8StringFromCFString(nasel->na->hostname, &hostname);
    if (ret)
	goto out;

    ret = LKDCDiscoverRealm(hostname, &realm);
    if (ret)
	goto out;

    nasel->server = CFStringCreateWithFormat(nasel->na->alloc, NULL,
					     CFSTR("%@/%s@%s"),
					     nasel->na->service, realm, realm);

    u = nasel->client;

    nasel->client =
	CFStringCreateWithFormat(nasel->na->alloc, 0, CFSTR("%@@%s"), u, realm);
    CFRELEASE(u);

 out:
    __KRBReleaseUTF8String(hostname);
    free(realm);
    signal_result(nasel);
}

static Boolean
is_local_hostname(CFStringRef hostname)
{
    if (CFStringHasSuffix(hostname, CFSTR(".local")))
	return true;
    if (CFStringHasSuffix(hostname, CFSTR(".members.mac.com")))
	return true;
    if (CFStringHasSuffix(hostname, CFSTR(".members.me.com")))
	return true;
    return false;
}

static void
classic_lkdc(NAHRef na, unsigned long flags)
{
    NAHSelectionRef nasel;
    CFStringRef u = NULL;
    CFIndex n;
    int duplicate;

    if (!is_local_hostname(na->hostname))
	return;

    /* if we have certs, lets push those names too */
    for (n = 0; na->x509identities && n < CFArrayGetCount(na->x509identities); n++) {
	SecCertificateRef cert = (void *)CFArrayGetValueAtIndex(na->x509identities, n);
	unsigned char digest[CC_SHA1_DIGEST_LENGTH];
	char str[CC_SHA1_DIGEST_LENGTH * 2 + 1], *cp;
	CC_SHA1_CTX ctx;
	size_t m;

        /* get the cert data */
	CFDataRef certData = SecCertificateCopyData(cert);
        if (certData == NULL)
	    continue;

	CC_SHA1_Init(&ctx);
	CC_SHA1_Update(&ctx,
		       CFDataGetBytePtr(certData), CFDataGetLength(certData));
	CC_SHA1_Final(digest, &ctx);

	cp = str;
	for (m = 0; m < CC_SHA1_DIGEST_LENGTH; m++) {
	    sprintf(cp, "%02X", digest[m]);
	    cp += 2;
	}
	*cp = '\0';

	u = CFStringCreateWithCString(na->alloc, str, kCFStringEncodingUTF8);
	CFRELEASE(certData);
	if (u == NULL)
	    continue;

	{
	    CFStringRef label = NULL;
	    SecCertificateInferLabel(cert, &label);
	    if (label) {
		nalog(CFSTR("Adding classic LKDC for %@"), label);
		CFRelease(label);
	    }
	}

	nasel = addSelection(na,
			     u, kNAHNTKRB5Principal,
			     NULL, kNAHNTKRB5PrincipalReferral,
			     GSS_KERBEROS, &duplicate, flags);
	CFRELEASE(u);
	if (nasel == NULL || duplicate)
	    continue;

	CFRetain(cert);
	nasel->certificate = cert;

	CFRetain(na);
	dispatch_async(na->bgq, ^{
		classic_lkdc_background(nasel);
		CFRelease(na);
	    });
    }

    CFRELEASE(u);

    if (na->password) {
	nasel = addSelection(na,
			     na->username, kNAHNTKRB5Principal,
			     NULL, kNAHNTKRB5PrincipalReferral,
			     GSS_KERBEROS, &duplicate, flags);
	if (nasel && !duplicate) {
	    CFRetain(na);
	    dispatch_async(na->bgq, ^{
		    classic_lkdc_background(nasel);
		    CFRelease(na);
		});
	}
    }
}

static void
add_realms(NAHRef na, char **realms, unsigned long flags)
{
    CFStringRef u, s;
    size_t n;

    for (n = 0; realms[n] != NULL; n++) {
	u = CFStringCreateWithFormat(na->alloc, 0, CFSTR("%@@%s"), na->username, realms[n]);
	s = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@/%@@%s"), na->service, na->hostname, realms[n]);

	if (u && s)
	    addSelection(na, u, kNAHNTKRB5Principal,
			 s, kNAHNTKRB5PrincipalReferral, GSS_KERBEROS, NULL, flags);
	CFRELEASE(u);
	CFRELEASE(s);
    }
}


static void
use_classic_kerberos(NAHRef na, unsigned long flags)
{
    CFRange range, dr, ur;
    char **realms, *str;
    int ret;

    if (is_local_hostname(na->hostname))
	return;

    ret = __KRBCreateUTF8StringFromCFString(na->hostname, &str);
    if (ret)
	return;

    /*
     * If user have @REALM, lets try that out
     */

    if (CFStringFindWithOptions(na->username, CFSTR("@"), CFRangeMake(0, CFStringGetLength(na->username)), 0, &range)) {
	CFStringRef domain = NULL, s = NULL;
	CFMutableStringRef domainm = NULL;

	dr.location = range.location + 1;
	dr.length = CFStringGetLength(na->username) - dr.location;

	domain = CFStringCreateWithSubstring(na->alloc, na->username, dr);
	if (domain) {
	    domainm = CFStringCreateMutableCopy(na->alloc, 0, domain);

	    if (domainm) {
		CFStringUppercase(domainm, NULL);

		s = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@/%@@%@"),
					     na->service, na->hostname, domainm);

		if (s)
		    addSelection(na, na->username, kNAHNTKRB5Principal,
				 s, kNAHNTKRB5PrincipalReferral,  GSS_KERBEROS, NULL, flags);
	    }
	}
	CFRELEASE(domainm);
	CFRELEASE(domain);
	CFRELEASE(s);
    }

    if (CFStringFindWithOptions(na->username, CFSTR("\\"), CFRangeMake(0, CFStringGetLength(na->username)), 0, &range)) {
	CFStringRef domain = NULL, user = NULL, user2 = NULL, s = NULL;
	CFMutableStringRef domainm = NULL;

	dr.location = 0;
	dr.length = range.location;

	ur.location = range.location + 1;
	ur.length = CFStringGetLength(na->username) - ur.location;

	user = CFStringCreateWithSubstring(na->alloc, na->username, ur);
	domain = CFStringCreateWithSubstring(na->alloc, na->username, dr);
	if (domain && user) {
	    domainm = CFStringCreateMutableCopy(na->alloc, 0, domain);
	    user2 = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@@%@"), user, domain);

	    if (domainm && user2) {
		CFStringUppercase(domainm, NULL);

		s = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@/%@@%@"),
					     na->service, na->hostname, domainm);

		if (s)
		    addSelection(na, user2, kNAHNTKRB5Principal,
				 s, kNAHNTKRB5PrincipalReferral,  GSS_KERBEROS, NULL, flags|FORCE_ADD);
	    }
	}
	CFRELEASE(domainm);
	CFRELEASE(domain);
	CFRELEASE(user2);
	CFRELEASE(user);
	CFRELEASE(s);
    }

    /*
     * Try the host realm
     */

    ret = krb5_get_host_realm(na->context, str, &realms);
    __KRBReleaseUTF8String(str);
    if (ret == 0) {
	add_realms(na, realms, flags);
	krb5_free_host_realm(na->context, realms);
    }

    /*
     * Also, just for the heck of it, check default realms
     */

    ret = krb5_get_default_realms(na->context, &realms);
    if (ret == 0) {
	add_realms(na, realms, flags);
	krb5_free_host_realm(na->context, realms);
    }
}

/*
 *
 */

static void
use_existing_principals(NAHRef na, int only_lkdc, unsigned long flags)
{
    krb5_cccol_cursor cursor;
    krb5_principal client;
    krb5_error_code ret;
    CFStringRef server;
    krb5_ccache id;
    CFStringRef u;
    char *c;

    ret = krb5_cccol_cursor_new(na->context, &cursor);
    if (ret)
	return;

    while ((ret = krb5_cccol_cursor_next(na->context, cursor, &id)) == 0 && id != NULL) {
	NAHSelectionRef nasel;
	int is_lkdc;

	ret = krb5_cc_get_principal(na->context, id, &client);
	if (ret) {
	    krb5_cc_close(na->context, id);
	    continue;
	}

	is_lkdc = krb5_principal_is_lkdc(na->context, client);

	if ((only_lkdc && !is_lkdc) || (!only_lkdc && is_lkdc)) {
	    krb5_free_principal(na->context, client);
	    krb5_cc_close(na->context, id);
	    continue;
	}

	ret = krb5_unparse_name(na->context, client, &c);
	if (ret) {
	    krb5_free_principal(na->context, client);
	    krb5_cc_close(na->context, id);
	    continue;
	}

	u = CFStringCreateWithCString(na->alloc, c, kCFStringEncodingUTF8);
	free(c);
	if (u == NULL) {
	    krb5_free_principal(na->context, client);
	    krb5_cc_close(na->context, id);
	    continue;
	}

	if (is_lkdc) {
	    CFStringRef cr = NULL;
	    krb5_data data;

	    ret = krb5_cc_get_config(na->context, id, NULL, "lkdc-hostname", &data);
	    if (ret == 0) {
		cr = CFStringCreateWithBytes(na->alloc, data.data, data.length, kCFStringEncodingUTF8, false);
		krb5_data_free(&data);
	    }

	    if (cr == NULL || CFStringCompare(na->hostname, cr, 0) != kCFCompareEqualTo) {
		krb5_free_principal(na->context, client);
		krb5_cc_close(na->context, id);
		continue;
	    }

	    /* Create server principal */
	    server = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@/%s@%s"),
					      na->service, client->realm, client->realm);

	    nalog(CFSTR("Adding existing LKDC cache: %@ -> %@"), u, server);

	} else {
	    server = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@/%@@%s"), na->service, na->hostname,
					      krb5_principal_get_realm(na->context, client));
	    nalog(CFSTR("Adding existing cache: %@ -> %@"), u, server);
	}
	krb5_free_principal(na->context, client);

	nasel = addSelection(na, u, kNAHNTKRB5Principal,
			     server, kNAHNTKRB5PrincipalReferral, GSS_KERBEROS, NULL, flags);
	CFRELEASE(u);
	CFRELEASE(server);
	if (nasel != NULL && nasel->ccache == NULL) {
	    krb5_data data;
	    nasel->ccache = id;
	    nasel->have_cred = 1;

	    if (nasel->inferredLabel == NULL) {
		ret = krb5_cc_get_config(na->context, id, NULL, "FriendlyName", &data);
		if (ret == 0) {
		    nasel->inferredLabel = CFStringCreateWithBytes(na->alloc, data.data, data.length, kCFStringEncodingUTF8, false);
		    krb5_data_free(&data);
		}
	    }

	} else
	    krb5_cc_close(na->context, id);

    }

    krb5_cccol_cursor_free(na->context, &cursor);
}

static CFStringRef kWELLKNOWN_LKDC = CFSTR("WELLKNOWN:COM.APPLE.LKDC");

static void
wellknown_lkdc(NAHRef na, enum NAHMechType mechtype, unsigned long flags)
{
    CFStringRef s, u;
    CFIndex n;

    u = CFStringCreateWithFormat(na->alloc, NULL,
				 CFSTR("%@@%@"),
				 na->username, kWELLKNOWN_LKDC);
    if (u == NULL)
	return;

    s = CFStringCreateWithFormat(na->alloc, NULL,
				 CFSTR("%@/localhost@%@"), na->service,
				 kWELLKNOWN_LKDC, kWELLKNOWN_LKDC);
    if (s == NULL) {
	CFRELEASE(u);
	return;
    }

    if (na->password)
	addSelection(na, u, kNAHNTKRB5Principal, 
		     s, kNAHNTKRB5Principal, mechtype, NULL, flags);
    CFRELEASE(u);

    /* if we have certs, lets push those names too */
    for (n = 0; na->x509identities && n < CFArrayGetCount(na->x509identities); n++) {
	SecCertificateRef cert = (void *)CFArrayGetValueAtIndex(na->x509identities, n);
	CFStringRef csstr;

	csstr = _CSCopyKerberosPrincipalForCertificate(cert);
	if (csstr == NULL) {
	    hx509_cert hxcert;
	    char *str;
	    int ret;

	    ret = hx509_cert_init_SecFramework(na->hxctx, cert, &hxcert);
	    if (ret)
		continue;

	    ret = hx509_cert_get_appleid(na->hxctx, hxcert, &str);
	    hx509_cert_free(hxcert);
	    if (ret)
		continue;

	    u = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%s@%@"), 
					 str, kWELLKNOWN_LKDC);
	    krb5_xfree(str);
	    if (u == NULL)
		continue;
	} else {
	    u = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@@%@"),
					 csstr, kWELLKNOWN_LKDC);
	    if (u == NULL)
		continue;
	}

	NAHSelectionRef nasel = addSelection(na, u, kNAHNTKRB5Principal, s, kNAHNTKRB5PrincipalReferral, mechtype, NULL, flags);
	CFRELEASE(u);
	if (nasel) {
	    CFRetain(cert);
	    nasel->certificate = cert;
	}
    }

    CFRELEASE(s);
}

static bool
is_smb(NAHRef na)
{
    if (CFStringCompare(na->service, kNAHServiceHostServer, 0) == kCFCompareEqualTo ||
	CFStringCompare(na->service, kNAHServiceCIFSServer, 0) == kCFCompareEqualTo)
	return true;
    return false;
}

static void
guess_kerberos(NAHRef na)
{
    bool try_lkdc_classic = true;
    bool try_wlkdc = false;
    bool try_iakerb_with_lkdc = false;
    bool have_kerberos = false;
    krb5_error_code ret;
    unsigned long flags = USE_SPNEGO;

    if (nah_use_gss_uam
	&& na->password
	&& haveMech(na, kGSSAPIMechIAKERB)
	&& haveMech(na, kGSSAPIMechSupportsAppleLKDC)
	&& !is_smb(na))
    {
	/* if we support IAKERB and AppleLDKC and is not SMB (client can't handle it, rdar://problem/8437184), let go for iakerb with */
	try_iakerb_with_lkdc = true;
    } else if (haveMech(na, kGSSAPIMechPKU2UOID) || haveMech(na, kGSSAPIMechSupportsAppleLKDC)) {
	try_wlkdc = true;
    } else if (CFStringCompare(na->service, kNAHServiceVNCServer, 0) == kCFCompareEqualTo) {
	try_wlkdc = true;
    }

    /* 
     * let check if we should disable LKDC classic mode
     *
     * There is two cases where we know that we don't want to do LKDC
     * - Server supports PKU2U or announce support for AppleLKDC
     * - Server didn't do above and didn't have LKDC in the announced name
     *   This later is true for windows servers and 10.7 SMB servers,
     *   10.7 afp server does announce LKDC names though (tracked by 9002742).
     */

    if (haveMech(na, kGSSAPIMechPKU2UOID) || haveMech(na, kGSSAPIMechSupportsAppleLKDC)) {
	try_lkdc_classic = false;
	nalog(CFSTR("Turing off LKDC classic since server announces support for wellknown name: %@"), na->servermechs);
    } else if (na->spnegoServerName) {
	CFRange res = CFStringFind(na->spnegoServerName, CFSTR("@LKDC"), 0);
	if (res.location == kCFNotFound) {
	    nalog(CFSTR("Turing off LKDC classic since spnegoServerName didn't contain LKDC: %@"),
		  na->spnegoServerName);
	    try_lkdc_classic = false;
	}
    }

    /*
     * If we are using an old AFP server, disable SPNEGO
     */
    if (CFStringCompare(na->service, kNAHServiceAFPServer, 0) == kCFCompareEqualTo &&
	!haveMech(na, kGSSAPIMechSupportsAppleLKDC))
    {
	flags &= (~USE_SPNEGO);
    }

    
    have_kerberos = (na->servermechs == NULL) ||
	haveMech(na, kGSSAPIMechIAKERB) ||
	haveMech(na, kGSSAPIMechKerberosOID) ||
	haveMech(na, kGSSAPIMechKerberosMicrosoftOID) ||
	haveMech(na, kGSSAPIMechPKU2UOID);

    nalog(CFSTR("NAHCreate-krb: have_kerberos=%s try_iakerb_with_lkdc=%s try-wkdc=%s try-lkdc-classic=%s use-spnego=%s"),
	  have_kerberos ? "yes" : "no",
	  try_iakerb_with_lkdc ? "yes" : "no",
	  try_wlkdc ? "yes" : "no",
	  try_lkdc_classic ? "yes" : "no",
	  (flags & USE_SPNEGO) ? "yes" : "no");
    
    if (!have_kerberos)
	return;

    /*
     *
     */

    ret = krb5_init_context(&na->context);
    if (ret)
	return;

    ret = hx509_context_init(&na->hxctx);
    if (ret)
	return;

    /*
     * We'll use matching LKDC credentials to this host since they are
     * faster then public key operations.
     */

    use_existing_principals(na, 1, flags);

    /*
     * IAKERB with LKDC
     */
    
    if (try_iakerb_with_lkdc) {
	wellknown_lkdc(na, GSS_KERBEROS_IAKERB, flags);
    }
    
    /*
     * Wellknown:LKDC
     */

    if (try_wlkdc)
	wellknown_lkdc(na, GSS_KERBEROS, flags);

    /*
     * Do classic Kerberos too
     */

    if (na->password)
	use_classic_kerberos(na, flags);


    /*
     * Classic LKDC style, causes mDNS lookups, so avoid if possible
     */

    if (try_lkdc_classic) {
	/* classic name */
	classic_lkdc(na, flags);
    }

    /*
     * We'll use existing credentials if we have them
     */

    use_existing_principals(na, 0, flags);
}

static void
guess_ntlm(NAHRef na)
{
    CFStringRef s;
    unsigned long flags = USE_SPNEGO;

    if (!haveMech(na, kGSSAPIMechNTLMOID))
	return;

    if (na->servermechs) {
	CFDataRef data = CFDictionaryGetValue(na->servermechs, kGSSAPIMechNTLMOID);
	if (data && CFDataGetLength(data) == 3 && memcmp(CFDataGetBytePtr(data), "raw", 3) == 0)
	    flags &= (~USE_SPNEGO);
    }

    s = CFStringCreateWithFormat(na->alloc, 0, CFSTR("%@@%@"), na->service, na->hostname);
    if (s == NULL)
	return;

    if (na->password) {
	CFRange range, ur, dr;
	CFStringRef u;
	unsigned long flags2 = 0;

	if (CFStringFindWithOptions(na->username, CFSTR("@"), CFRangeMake(0, CFStringGetLength(na->username)), 0, &range)) {
	    u = na->username;
	    CFRetain(u);

	    flags2 = FORCE_ADD;
	} else if (CFStringFindWithOptions(na->username, CFSTR("\\"), CFRangeMake(0, CFStringGetLength(na->username)), 0, &range)) {
	    CFStringRef ustr, dstr;

	    dr.location = 0;
	    dr.length = range.location;

	    ur.location = range.location + 1;
	    ur.length = CFStringGetLength(na->username) - ur.location;

	    dstr = CFStringCreateWithSubstring(NULL, na->username, dr);
	    ustr = CFStringCreateWithSubstring(NULL, na->username, ur);

	    if (dstr && ustr)
		u = CFStringCreateWithFormat(na->alloc, 0, CFSTR("%@@%@"), ustr, dstr);
	    CFRELEASE(ustr);
	    CFRELEASE(dstr);

	    flags2 = FORCE_ADD;
	} else {
	    u = CFStringCreateWithFormat(na->alloc, 0, CFSTR("%@@\\%@"), na->username, na->hostname);
	}

	if (u) {
	    addSelection(na, u, kNAHNTUsername, s, NULL, GSS_NTLM, NULL, flags | flags2);
	    CFRELEASE(u);
	}

	if (na->specificname) {
	    u = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@@\\%@"), na->specificname, na->hostname);
	    addSelection(na, u, kNAHNTUsername, s, NULL, GSS_NTLM, NULL, flags);
	    CFRELEASE(u);
	}
    }

    /* pick up ntlm credentials in caches */

    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    if (sema == NULL)
	goto out;

    (void)gss_iter_creds(NULL, 0, GSS_NTLM_MECHANISM, ^(gss_OID oid, gss_cred_id_t cred) {
	    OM_uint32 min_stat;
	    gss_name_t name = GSS_C_NO_NAME;
	    gss_buffer_desc buffer = { 0, NULL };

	    if (cred == NULL) {
		dispatch_semaphore_signal(sema);
		return;
	    }

	    gss_inquire_cred(&min_stat, cred, &name, NULL, NULL, NULL);
	    gss_display_name(&min_stat, name, &buffer, NULL);
	    gss_release_name(&min_stat, &name);

	    CFStringRef u = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%.*s"),
						     (int)buffer.length, buffer.value);

	    gss_release_buffer(&min_stat, &buffer);
	    
	    if (u == NULL)
		return;

	    /* if we where given a name, and it matches, add it */

	    NAHSelectionRef nasel = addSelection(na, u, kNAHNTUsername, s, NULL, GSS_NTLM, NULL, flags);
	    CFRELEASE(u);
	    if (nasel) {
		nasel->have_cred = 1;
	    }
	});

    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    dispatch_release(sema);

 out:
    CFRELEASE(s);
}

/*
 *
 */

const CFStringRef kNAHNegTokenInit = CFSTR("kNAHNegTokenInit");
const CFStringRef kNAHUserName = CFSTR("kNAHUserName");
const CFStringRef kNAHCertificates = CFSTR("kNAHCertificates");
const CFStringRef kNAHPassword = CFSTR("kNAHPassword");

NAHRef
NAHCreate(CFAllocatorRef alloc,
	  CFStringRef hostname,
	  CFStringRef service,
	  CFDictionaryRef info)
{
    NAHRef na = NAAlloc(alloc);
    CFStringRef canonname;
    char *hostnamestr = NULL;
    
    dispatch_once(&init_globals, ^{
	Boolean have_key = false;
	nah_use_gss_uam = CFPreferencesGetAppBooleanValue(CFSTR("GSSEnable"), CFSTR("com.apple.NetworkAuthenticationHelper"), &have_key);
	if (!have_key)
	    nah_use_gss_uam = true;
    });

    nalog(CFSTR("NAHCreate: hostname=%@ service=%@"), hostname, service);
    
    /* first undo the damage BrowserServices have done to the hostname */

    if (_CFNetServiceDeconstructServiceName(hostname, &hostnamestr)) {
	canonname = CFStringCreateWithCString(na->alloc, hostnamestr, kCFStringEncodingUTF8);
	free(hostnamestr);
	if (canonname == NULL)
	    return NULL;
    } else {
	canonname = hostname;
	CFRetain(canonname);
    }

    na->hostname = CFStringCreateMutableCopy(alloc, 0, canonname);
    if (na->hostname == NULL) {
	CFRELEASE(na);
	return NULL;
    }
    CFStringTrim(na->hostname, CFSTR("."));

    nalog(CFSTR("NAHCreate: will use hostname=%@"), na->hostname);

    na->service = CFRetain(service);

    if (!findUsername(alloc, na, info)) {
	CFRELEASE(na);
	return NULL;
    }

    nalog(CFSTR("NAHCreate: username=%@ username %s"), na->username, na->specificname ? 
	  "given" : "generated");


    if (info) {
	na->password = CFDictionaryGetValue(info, kNAHPassword);
	if (na->password) {
	    nalog(CFSTR("NAHCreate: password"));
	    CFRetain(na->password);
	}
    }

    na->selections = CFArrayCreateMutable(na->alloc, 0, &kCFTypeArrayCallBacks);
    if (na->selections == NULL) {
	CFRELEASE(na);
	return NULL;
    }

    if (info) {
	CFDictionaryRef nti;
	CFArrayRef certs;

	nti = CFDictionaryGetValue(info, kNAHNegTokenInit);
	if (nti) {
	    na->servermechs = CFDictionaryGetValue(nti, kSPNEGONegTokenInitMechs);
	    if (na->servermechs)
		CFRetain(na->servermechs);
	    na->spnegoServerName = CFDictionaryGetValue(nti, kSPNEGONegTokenInitHintsHostname);
	    if (na->spnegoServerName) {
		nalog(CFSTR("NAHCreate: SPNEGO hints name %@"), na->spnegoServerName);
		CFRetain(na->spnegoServerName);
	    }
	}

	certs = CFDictionaryGetValue(info, kNAHCertificates);
	if (certs) {
	    CFTypeID type = CFGetTypeID(certs);

	    nalog(CFSTR("NAHCreate: %@"), certs);

	    if (CFArrayGetTypeID() == type) {
		CFRetain(certs);
		na->x509identities = certs;
	    } else if (SecCertificateGetTypeID() == type || SecIdentityGetTypeID() == type) {
		CFMutableArrayRef a = CFArrayCreateMutable(na->alloc, 0, &kCFTypeArrayCallBacks);
		CFArrayAppendValue(a, certs);
		na->x509identities = a;
	    } else {
		CFStringRef desc = CFCopyDescription(certs);
		nalog(CFSTR("unknown type of certificates: %@"), desc);
		CFRELEASE(desc);
	    }
	}
    }

    /* here starts the guessing game */

    add_user_selections(na);

    guess_kerberos(na);

    /* only do NTLM for SMB */
    if (na->x509identities == NULL && is_smb(na))
	guess_ntlm(na);

    return na;
}

CFArrayRef
NAHGetSelections(NAHRef na)
{
    return na->selections;
}

static CFTypeRef
searchArray(CFArrayRef array, CFTypeRef key)
{
    CFDictionaryRef dict;
    CFIndex n;

    for (n = 0 ; n < CFArrayGetCount(array); n++) {
	dict = CFArrayGetValueAtIndex(array, n);
	if (CFGetTypeID(dict) != CFDictionaryGetTypeID())
	    continue;
	CFTypeRef dictkey = CFDictionaryGetValue(dict, kSecPropertyKeyLabel);
	if (CFEqual(dictkey, key))
	    return CFDictionaryGetValue(dict, kSecPropertyKeyValue);
    }
    return NULL;
}

static void
setFriendlyName(NAHRef na,
		NAHSelectionRef selection,
		SecCertificateRef cert,
		krb5_ccache id,
		int is_lkdc)
{
    CFStringRef inferredLabel = NULL;

    if (cert) {
	
	inferredLabel = _CSCopyAppleIDAccountForAppleIDCertificate(cert, NULL);
	if (inferredLabel == NULL) {
	    const CFStringRef dotmac = CFSTR(".Mac Sharing Certificate");
	    const CFStringRef mobileMe = CFSTR("MobileMe Sharing Certificate");
	    void *values[4] = { (void *)kSecOIDDescription, (void *)kSecOIDCommonName, (void *)kSecOIDOrganizationalUnitName, (void *)kSecOIDX509V1SubjectName };
	    CFArrayRef attrs = CFArrayCreate(NULL, (const void **)values, sizeof(values) / sizeof(values[0]), &kCFTypeArrayCallBacks);
	    
	    if (NULL == attrs)
		return;
	    
	    CFDictionaryRef certval = SecCertificateCopyValues(cert, attrs, NULL);
	    CFRelease(attrs);
	    if (NULL == certval)
		return;
	    
	    CFDictionaryRef subject = CFDictionaryGetValue(certval, kSecOIDX509V1SubjectName);
	    if (NULL != subject) {
		
		CFArrayRef val = CFDictionaryGetValue(subject, kSecPropertyKeyValue);
		
		if (NULL != val) {
		    
		    CFStringRef description = searchArray(val, kSecOIDDescription);
		    
		    if (NULL != description &&
			(kCFCompareEqualTo == CFStringCompare(description, dotmac, 0) || kCFCompareEqualTo == CFStringCompare(description, mobileMe, 0)))
		    {
			CFStringRef commonName = searchArray(val, kSecOIDCommonName);
			CFStringRef organizationalUnit = searchArray(val, kSecOIDOrganizationalUnitName);
			
			if (NULL != commonName && NULL != organizationalUnit) {
			    inferredLabel = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@@%@"), commonName, organizationalUnit);
			}
		    }
		}
	    }
	    
	    CFRelease(certval);
	}
	if (inferredLabel == NULL)
	    SecCertificateInferLabel(cert, &inferredLabel);
    } else if (na->specificname || is_lkdc) {
	inferredLabel = na->username;
	CFRetain(inferredLabel);
    } else {
	inferredLabel = selection->client;
	CFRetain(inferredLabel);
    }

    if (inferredLabel) {
	char *label;

	if (__KRBCreateUTF8StringFromCFString(inferredLabel, &label) == noErr) {
	    krb5_data data;

	    data.data = label;
	    data.length = strlen(label) + 1;
	    
	    krb5_cc_set_config(na->context, id, NULL, "FriendlyName", &data);
	    free(label);
	}
	selection->inferredLabel = inferredLabel;
    }
}

static int
acquire_kerberos(NAHRef na,
		 NAHSelectionRef selection,
		 CFStringRef password,
		 SecCertificateRef cert,
		 CFErrorRef *error)
{
    krb5_init_creds_context icc = NULL;
    krb5_get_init_creds_opt *opt = NULL;
    krb5_principal client = NULL;
    int destroy_cache = 0;
    krb5_ccache id = NULL;
    krb5_error_code ret;
    krb5_creds cred;
    char *str = NULL;
    int parseflags = 0;
    int is_lkdc = 0;

    memset(&cred, 0, sizeof(cred));

    nalog(CFSTR("acquire_kerberos: %@ with pw:%s cert:%s"),
	  selection->client,
	  password ? "yes" : "no",
	  cert ? "yes" : "no");

    ret = __KRBCreateUTF8StringFromCFString(selection->client, &str);
    if (ret)
	goto out;

    /*
     * Check if this is an enterprise name
     * XXX horrible, caller should tell us
     */
    {
	char *p = strchr(str, '@');
	if (p && (p = strchr(p + 1, '@')) != NULL)
	    parseflags |= KRB5_PRINCIPAL_PARSE_ENTERPRISE;
    }

    ret = krb5_parse_name_flags(na->context, str, parseflags, &client);
    __KRBReleaseUTF8String(str);
    if (ret)
	goto out;

    ret = krb5_unparse_name(na->context, client, &str);
    if (ret == 0) {
	nalog(CFSTR("acquire_kerberos: trying with %s as client principal"), str);
	free(str);
    }

    ret = krb5_get_init_creds_opt_alloc(na->context, &opt);
    if (ret)
	goto out;

    if (cert) {
	ret = krb5_get_init_creds_opt_set_pkinit(na->context, opt, client,
						 NULL, "KEYCHAIN:",
						 NULL, NULL, 0,
						 NULL, NULL, NULL);
	if (ret)
	    goto out;
    }

    krb5_get_init_creds_opt_set_canonicalize(na->context, opt, TRUE);

    ret = krb5_init_creds_init(na->context, client, NULL, NULL,
			       0, opt, &icc);
    if (ret)
	goto out;

    if (krb5_principal_is_lkdc(na->context, client)) {
        char *tcphostname = NULL;

	ret = __KRBCreateUTF8StringFromCFString(na->hostname, &str);
	if (ret)
	    goto out;
	asprintf(&tcphostname, "tcp/%s", str);
	__KRBReleaseUTF8String(str);
	if (tcphostname == NULL) {
	    ret = ENOMEM;
	    goto out;
	}
	krb5_init_creds_set_kdc_hostname(na->context, icc, tcphostname);
	free(tcphostname);
    }

    if (cert) {
	hx509_cert hxcert;

	ret = hx509_cert_init_SecFramework(na->hxctx, cert, &hxcert);
	if (ret)
	    goto out;

	ret = krb5_init_creds_set_pkinit_client_cert(na->context, icc, hxcert);
	hx509_cert_free(hxcert);
	if (ret)
	    goto out;

    } else if (password) {
	ret = __KRBCreateUTF8StringFromCFString(password, &str);
	if (ret)
	    goto out;
	ret = krb5_init_creds_set_password(na->context, icc, str);
	__KRBReleaseUTF8String(str);
	if (ret)
	    goto out;
    } else {
	abort();
    }

    ret = krb5_init_creds_get(na->context, icc);
    if (ret)
	goto out;

    ret = krb5_init_creds_get_creds(na->context, icc, &cred);
    if (ret)
	goto out;

    ret = krb5_cc_cache_match(na->context, cred.client, &id);
    if (ret) {
	ret = krb5_cc_new_unique(na->context, NULL, NULL, &id);
	if (ret)
	    goto out;
	destroy_cache = 1;
    }

    ret = krb5_cc_initialize(na->context, id, cred.client);
    if (ret)
	goto out;

    ret = krb5_cc_store_cred(na->context, id, &cred);
    if (ret)
	goto out;

    ret = krb5_init_creds_store_config(na->context, icc, id);
    if (ret)
	goto out;

    /* the KDC might have done referrals games, let update the principals */

    {
	CFStringRef newclient, newserver;
	const char *realm = krb5_principal_get_realm(na->context, cred.client);

	is_lkdc = krb5_realm_is_lkdc(realm);

	ret = krb5_unparse_name(na->context, cred.client, &str);
	if (ret)
	    goto out;

	newclient = CFStringCreateWithCString(na->alloc, str, kCFStringEncodingUTF8);
	free(str);
	if (newclient == NULL) {
	    ret = ENOMEM;
	    goto out;
	}

	nalog(CFSTR("acquire_kerberos: got %@ as client principal"), newclient);

	if (CFStringCompare(newclient, selection->client, 0) != kCFCompareEqualTo) {

	    CFRELEASE(selection->client);
	    selection->client = newclient;

	    if (is_lkdc) {
		newserver = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@/%s@%s"),
						     na->service, realm, realm);
	    } else {
		newserver = CFStringCreateWithFormat(na->alloc, NULL, CFSTR("%@/%@@%s"),
						     na->service, na->hostname, realm);
	    }
	    if (newserver) {
		CFRELEASE(selection->server);
		selection->server = newserver;
	    }
	} else {
	    CFRELEASE(newclient);
	}
    }

    setFriendlyName(na, selection, cert, id, is_lkdc);
    {
	krb5_data data;
	data.data = "1";
	data.length = 1;
	krb5_cc_set_config(na->context, id, NULL, nah_created, &data);
    }


 out:
    if (ret) {
	const char *e = krb5_get_error_message(na->context, ret);
	createError(NULL, error, ret, CFSTR("acquire_kerberos failed %@: %d - %s"),
	      selection->client, ret, e);
	krb5_free_error_message(na->context, e);
    } else {
	nalog(CFSTR("acquire_kerberos successful"));
    }

    if (opt)
	krb5_get_init_creds_opt_free(na->context, opt);

    if (icc)
	krb5_init_creds_free(na->context, icc);

    if (id) {
	if (ret != 0 && destroy_cache)
	    krb5_cc_destroy(na->context, id);
	else
	    krb5_cc_close(na->context, id);
    }
    krb5_free_cred_contents(na->context, &cred);

    if (client)
	krb5_free_principal(na->context, client);

    return ret;
}

/*
 *
 */

Boolean
NAHSelectionAcquireCredentialHaveResult(NAHSelectionRef selection,
					CFDictionaryRef info,
					dispatch_queue_t q,
					void (^result)(CFErrorRef error))
{
    void (^r)(CFErrorRef error) = (void (^)(CFErrorRef))Block_copy(result);

    if (selection->mech == GSS_KERBEROS) {

	nalog(CFSTR("NAHSelectionAcquireCredential: kerberos client: %@ (server %@)"),
	      selection->client, selection->server);

	/* if we already have a cache, skip acquire unless force */
	if (selection->ccache) {
	    nalog(CFSTR("have ccache"));
	    KRBCredChangeReferenceCount(selection->client, 1, 1);
	    dispatch_async(q, ^{
		    r(NULL);
		    CFRelease(selection->na);
		    Block_release(r);
		});
	    return true;
	}

	if (selection->na->password == NULL && selection->certificate == NULL) {
	    nalog(CFSTR("krb5: no password or cert, punting"));
	    CFRelease(selection->na);
	    Block_release(r);
	    return false;
	}

	dispatch_async(selection->na->bgq, ^{
		CFErrorRef error = NULL;
		int ret;

		ret = acquire_kerberos(selection->na,
				       selection,
				       selection->na->password,
				       selection->certificate,
				       &error);

		dispatch_async(q, ^{
			r(error);
			Block_release(r);
			CFRelease(selection->na);
			if (error)
			    CFRelease(error);
		    });
	    });

	return true;
    } else if (selection->mech == GSS_NTLM) {
	gss_auth_identity_desc identity;
	gss_name_t name = GSS_C_NO_NAME;
	gss_buffer_desc gbuf;
	char *password, *user;
	OM_uint32 major, minor, junk;
	dispatch_semaphore_t s;
	char *str;

	nalog(CFSTR("NAHSelectionAcquireCredential: ntlm"));

	if (selection->have_cred) {
	    dispatch_async(q, ^{
		    r(NULL);
		    CFRelease(selection->na);
		    Block_release(r);
		});
	    return true;
	}

	if (selection->na->password == NULL) {
	    Block_release(r);
	    CFRelease(selection->na);
	    return false;
	}

	__KRBCreateUTF8StringFromCFString(selection->client, &user);

	gbuf.value = user;
	gbuf.length = strlen(user);

	major = gss_import_name(&minor, &gbuf, GSS_C_NT_USER_NAME, &name);
	__KRBReleaseUTF8String(user);
	if (major) {
	    Block_release(r);
	    CFRelease(selection->na);
	    return false;
	}

	s = dispatch_semaphore_create(0);
	__KRBCreateUTF8StringFromCFString(selection->client, &user);
	__KRBCreateUTF8StringFromCFString(selection->na->password, &password);

	selection->inferredLabel = selection->client;
	CFRetain(selection->inferredLabel);

	/* drop when name is completly supported */
	str = strchr(user, '@');
	if (str)
	    *str++ = '\0';

	identity.username = user;
	if (str)
	    identity.realm = str;
	else
	    identity.realm = "";
	identity.password = password;

	major = gss_acquire_cred_ex(name,
				    0,
				    GSS_C_INDEFINITE,
				    GSS_NTLM_MECHANISM,
				    GSS_C_INITIATE,
				    &identity,
				    ^(gss_status_id_t status, gss_cred_id_t cred,
				      gss_OID_set set, OM_uint32 flags) {
					CFErrorRef error = NULL;
					if (cred)  {
					    gss_buffer_desc buffer;
					    OM_uint32 min_stat;

					    buffer.value = user;
					    buffer.length = strlen(user);

					    gss_cred_label_set(&min_stat, cred, "FriendlyName", &buffer);

					    buffer.value = "1";
					    buffer.length = 1;
					    gss_cred_label_set(&min_stat, cred, nah_created, &buffer);
					} else {
					    createError(NULL, &error, 1, CFSTR("failed to create ntlm cred"));
					}
					
					dispatch_semaphore_signal(s);
					r(error);
					CFRelease(selection->na);
					Block_release(r);
				    });
	gss_release_name(&junk, &name);
	if (major == GSS_S_COMPLETE) {
	    dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
	} else {
	    CFErrorRef error;
	    createError(NULL, &error, major, CFSTR("Failed to acquire NTLM credentials"));
	    r(error);
	    CFRelease(selection->na);
	    Block_release(r);
	    CFRELEASE(error);
	}

	__KRBReleaseUTF8String(user);
	__KRBReleaseUTF8String(password);

	dispatch_release(s);
	return true;
    } else if (selection->mech == GSS_KERBEROS_IAKERB) {
	gss_name_t name = GSS_C_NO_NAME;
	gss_buffer_desc gbuf;
	char *user;
	OM_uint32 major, minor, junk;
	gss_cred_id_t cred;
	
	nalog(CFSTR("NAHSelectionAcquireCredential: iakerb %@"), selection->client);
	
	if (selection->have_cred || selection->na->password == NULL) {
	    nalog(CFSTR("NAHSelectionAcquireCredential: iakerb no password"));
	    Block_release(r);
	    CFRelease(selection->na);
	    return false;
	}
	
	__KRBCreateUTF8StringFromCFString(selection->client, &user);
	
	gbuf.value = user;
	gbuf.length = strlen(user);
	
	major = gss_import_name(&minor, &gbuf, GSS_C_NT_USER_NAME, &name);
	__KRBReleaseUTF8String(user);
	if (major) {
	    Block_release(r);
	    CFRelease(selection->na);
	    return false;
	}
	
	selection->inferredLabel = selection->client;
	CFRetain(selection->inferredLabel);
	
	CFMutableDictionaryRef dict = NULL;
	
	dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	
	CFDictionaryAddValue(dict, kGSSICPassword, selection->na->password);

	major = gss_aapl_initial_cred(name, GSS_IAKERB_MECHANISM, dict, &cred, NULL);
	CFRelease(dict);
	gss_release_name(&junk, &name);	
	if (major) {
	    nalog(CFSTR("NAHSelectionAcquireCredential: failed with %d"), major);
	    Block_release(r);
	    CFRelease(selection->na);
	    return false;
	}

	gss_buffer_set_t dataset = GSS_C_NO_BUFFER_SET;
	
	major = gss_inquire_cred_by_oid(&minor, cred, GSS_C_NT_UUID, &dataset);
	if (major || dataset->count != 1) {
	    nalog(CFSTR("NAHSelectionAcquireCredential: failed with no uuid"));
	    gss_release_buffer_set(&junk, &dataset);
	    Block_release(r);
	    CFRelease(selection->na);
	    return false;
	}
	
	CFStringRef newclient = CFStringCreateWithBytes(NULL, dataset->elements[0].value, dataset->elements[0].length, kCFStringEncodingUTF8, false);
	if (newclient) {
	    CFRELEASE(selection->client);
	    selection->client = newclient;
	    selection->clienttype = kNAHNTUUID;
	}
	gss_release_buffer_set(&junk, &dataset);
	gss_release_cred(&junk, &cred);

	r(NULL);
	CFRelease(selection->na);
	Block_release(r);
	
	return true;
    } else {
	nalog(CFSTR("NAHSelectionAcquireCredential: unknown"));
    }

    return false;
}

Boolean
NAHSelectionAcquireCredentialAsync(NAHSelectionRef selection,
				  CFDictionaryRef info,
				  dispatch_queue_t q,
				  void (^result)(CFErrorRef error))
{
    void (^r)(CFErrorRef error) = (void (^)(CFErrorRef))Block_copy(result);

    CFRetain(selection->na);

    dispatch_async(selection->na->bgq, ^{
	    if (!wait_result(selection)) {
		CFErrorRef error;
		createError(NULL, &error, 1, CFSTR("Failed to get server for %@"), selection->client);
		r(error);
		CFRELEASE(error);
		CFRelease(selection->na);
	    } else {
		NAHSelectionAcquireCredentialHaveResult(selection, info, q, r);
	    }
	    Block_release(r);
	});

    return true;
}

const CFStringRef kNAHForceRefreshCredential = CFSTR("kNAHForceRefreshCredential");

Boolean
NAHSelectionAcquireCredential(NAHSelectionRef selection,
			     CFDictionaryRef info,
			     CFErrorRef *error)
{
    __block Boolean b;
    Boolean ret;

    CFRetain(selection->na);

    if (!wait_result(selection)) {
	CFRelease(selection->na);
	return false;
    }

    dispatch_sync(selection->na->q, ^{
	    if (selection->waiting != 0)
		abort();
	    
	    if (selection->wait == NULL)
		selection->wait = dispatch_semaphore_create(0);
	    selection->waiting = 1;
	});

    CFRetain(selection->na);
    ret = NAHSelectionAcquireCredentialHaveResult(selection, info, selection->na->bgq, ^(CFErrorRef e) {
	    if (e) {
		CFRetain(e);
		*error = e;
		b = false;
	    } else {
		b = true;
	    }
	    signal_result(selection);
	    CFRelease(selection->na);
	});
    if (ret == false) {
	dispatch_sync(selection->na->q, ^{ 
	    selection->waiting--;
	    if (selection->waiting == 0) {
		dispatch_release(selection->wait);
		selection->wait = NULL;
	    }
	});
	*error = NULL;
	return false;
    }

    if (!wait_result(selection))
	return false;

    return b;
}



static void
signal_result(NAHSelectionRef s)
{
    dispatch_sync(s->na->q, ^{
	    if (s->wait == NULL)
		return;
	    while (s->waiting > 0) {
		dispatch_semaphore_signal(s->wait);
		s->waiting--;
	    }
	    dispatch_release(s->wait);
	    s->wait = NULL;
	});
}

static Boolean
wait_result(NAHSelectionRef s)
{
    __block dispatch_semaphore_t sema;

    dispatch_sync(s->na->q, ^{
	    if (s->canceled) {
		sema = NULL;
	    } else {
		sema = s->wait;
		if (sema)
		    s->waiting++;
	    }
	});
    if (sema)
	dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

    if (s->canceled) {
	CFRelease(s);
	return false;
    }
    return true;
}

/*
 *
 */

CFTypeRef
NAHSelectionGetInfoForKey(NAHSelectionRef selection, CFStringRef key)
{
    if (!wait_result(selection))
	return NULL;

    if (CFStringCompare(kNAHSelectionHaveCredential, key, 0) == kCFCompareEqualTo) {
	if (selection->ccache)
	    return kCFBooleanTrue;
	return kCFBooleanFalse;
    } else if (CFStringCompare(kNAHSelectionUserPrintable, key, 0) == kCFCompareEqualTo) {
	return selection->client; /* XXX make prettier ? */
    } else if (CFStringCompare(kNAHServerPrincipal, key, 0) == kCFCompareEqualTo) {
	return selection->server;
    } else if (CFStringCompare(kNAHClientPrincipal, key, 0) == kCFCompareEqualTo) {
	return selection->client;
    } else if (CFStringCompare(kNAHMechanism, key, 0) == kCFCompareEqualTo) {
	/* if not told otherwise, wrap everything in spengo wrappings */
	if (!selection->spnego)
	    return mech2name(selection->mech);
	return kGSSAPIMechSPNEGO;
    } else if (CFStringCompare(kNAHInnerMechanism, key, 0) == kCFCompareEqualTo) {
	return mech2name(selection->mech);
    } else if (CFStringCompare(kNAHUseSPNEGO, key, 0) == kCFCompareEqualTo) {
	return selection->spnego ? kCFBooleanTrue : kCFBooleanFalse;
    } else if (CFStringCompare(kNAHCredentialType, key, 0) == kCFCompareEqualTo) {
	return mech2name(selection->mech);
    } else if (CFStringCompare(kNAHInferredLabel, key, 0) == kCFCompareEqualTo) {
	return selection->inferredLabel;
    }

    return NULL;
}

/*
 * Returns the data for kNetFSAuthenticationInfoKey
 */

CFDictionaryRef
NAHSelectionCopyAuthInfo(NAHSelectionRef selection)
{
    CFMutableDictionaryRef dict;
    CFStringRef string;
    int gssdclient, gssdserver;

    if (!wait_result(selection))
	return NULL;

    if (selection->server == NULL)
	return NULL;

    dict = CFDictionaryCreateMutable (kCFAllocatorDefault, 5,
				      &kCFCopyStringDictionaryKeyCallBacks,
				      &kCFTypeDictionaryValueCallBacks);
    if (dict == NULL)
	return NULL;

    CFDictionaryAddValue(dict, kNAHMechanism,
			 NAHSelectionGetInfoForKey(selection, kNAHMechanism));

    CFDictionaryAddValue(dict, kNAHCredentialType,
			 NAHSelectionGetInfoForKey(selection, kNAHCredentialType));

    CFDictionaryAddValue(dict, kNAHClientNameType, selection->clienttype);

    if (CFStringCompare(selection->clienttype, kNAHNTUUID, 0) == 0) {
	gssdclient = GSSD_USER; /* XXX add GSSD_UUID */
    } else if (CFStringCompare(selection->clienttype, kNAHNTKRB5Principal, 0) == 0) {
	gssdclient = GSSD_KRB5_PRINCIPAL;
    } else if (CFStringCompare(selection->clienttype, kNAHNTUsername, 0) == 0) {
	gssdclient = GSSD_NTLM_PRINCIPAL;
    } else {
	gssdclient = GSSD_USER;
    }

    CFDictionaryAddValue(dict, kNAHServerNameType, selection->servertype);

    if (CFStringCompare(selection->servertype, kNAHNTServiceBasedName, 0) == 0)
	gssdserver = GSSD_HOSTBASED;
    else if (CFStringCompare(selection->servertype, kNAHNTKRB5PrincipalReferral, 0) == 0)
	gssdserver = GSSD_KRB5_REFERRAL;
    else if (CFStringCompare(selection->servertype, kNAHNTKRB5Principal, 0) == 0)
	gssdserver = GSSD_KRB5_PRINCIPAL;
    else
	gssdserver = GSSD_HOSTBASED;


    CFDictionaryAddValue(dict, kNAHClientNameTypeGSSD, CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &gssdclient));
    CFDictionaryAddValue(dict, kNAHServerNameTypeGSSD, CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &gssdserver));

    CFDictionaryAddValue(dict, kNAHClientPrincipal,
			 NAHSelectionGetInfoForKey(selection, kNAHClientPrincipal));
    CFDictionaryAddValue(dict, kNAHServerPrincipal,
			 NAHSelectionGetInfoForKey(selection, kNAHServerPrincipal));

    /* add label if we have one */
    if ((string = NAHSelectionGetInfoForKey(selection, kNAHInferredLabel)) != NULL)
	CFDictionaryAddValue(dict, kNAHInferredLabel, string);

    CFDictionaryAddValue(dict, kNAHUseSPNEGO, NAHSelectionGetInfoForKey(selection, kNAHUseSPNEGO));

    return dict;
}

/*
 *
 */

void
NAHCancel(NAHRef na)
{
    dispatch_sync(na->q, ^{
	    NAHSelectionRef s;
	    CFIndex n, num;

	    num = CFArrayGetCount(na->selections);
	    for (n = 0; n < num; n++) {
		s = (NAHSelectionRef)CFArrayGetValueAtIndex(na->selections, n);

		s->canceled = 1;

		while (s->waiting > 0) {
		    CFRetain(s);
		    dispatch_semaphore_signal(s->wait);
		    s->waiting--;
		}
	    }
	});
}

/*
 * Reference counting
 */


static Boolean
CredChange(CFStringRef referenceKey, int count, const char *label)
{
    const char *mechname;
    gss_OID oid;

    if (referenceKey == NULL)
	return false;

    nalog(CFSTR("NAHCredChange: %@ count: %d label: %s"), 
	  referenceKey, count, label ? label : "<nolabel>");

    if (CFStringHasPrefix(referenceKey, CFSTR("krb5:"))) {
	oid = GSS_KRB5_MECHANISM;
	mechname = "kerberos";
    } else if (CFStringHasPrefix(referenceKey, CFSTR("ntlm:"))) {
	oid = GSS_NTLM_MECHANISM;
	mechname = "ntlm";
    } else
	return false;

    {
	gss_cred_id_t cred;
	gss_buffer_desc gbuf;
	OM_uint32 min_stat, maj_stat;
	CFStringRef name;
	gss_name_t gname;
	OSStatus ret;
	gss_OID_set_desc ntlmset;
	char *n;

	ntlmset.elements = oid;
	ntlmset.count = 1;

	name = CFStringCreateWithSubstring(NULL, referenceKey, CFRangeMake(5, CFStringGetLength(referenceKey) - 5));
	if (name == NULL)
	    return false;

	ret = __KRBCreateUTF8StringFromCFString(name, &n);
	CFRelease(name);
	if (ret)
	    return false;

	gbuf.value = n;
	gbuf.length = strlen(n);

	maj_stat = gss_import_name(&min_stat, &gbuf, GSS_C_NT_USER_NAME, &gname);
	if (maj_stat != GSS_S_COMPLETE) {
	    free(n);
	    return false;
	}

	maj_stat = gss_acquire_cred(&min_stat, gname, GSS_C_INDEFINITE, &ntlmset, GSS_C_INITIATE, &cred, NULL, NULL);
	gss_release_name(&min_stat, &gname);

	if (maj_stat != GSS_S_COMPLETE) {
	    nalog(CFSTR("ChangeCred: cred name %s/%s not found"), n, mechname);
	    free(n);
	    return false;
	}
	free(n);

	/* check that the credential is refcounted */
	{
	    gss_buffer_desc buffer;
	    maj_stat = gss_cred_label_get(&min_stat, cred, nah_created, &buffer);
	    if (maj_stat) {
		gss_release_cred(&min_stat, &cred);
		return false;
	    }
	    gss_release_buffer(&min_stat, &buffer);
	}

	if (count == 0) {
	    /* do nothing */
	} else if (count > 0) {
	    gss_cred_hold(&min_stat, cred);
	} else {
	    gss_cred_unhold(&min_stat, cred);
	}

	if (label) {
	    gss_buffer_desc buffer = {
		.value = "1",
		.length = 1
	    };

	    gss_cred_label_set(&min_stat, cred, label, &buffer);
	}

	gss_release_cred(&min_stat, &cred);
	return true;
    }
}

Boolean
NAHAddReferenceAndLabel(NAHSelectionRef selection,
			CFStringRef identifier)
{
    CFStringRef ref;
    Boolean res;
    char *ident;

    if (!wait_result(selection))
	return false;

    ref = NAHCopyReferenceKey(selection);
    if (ref == NULL)
	return false;
    
    nalog(CFSTR("NAHAddReferenceAndLabel: %@ label: %@"), ref, identifier);

    if (__KRBCreateUTF8StringFromCFString(identifier, &ident) != noErr) {
	CFRelease(ref);
	return false;
    }

    res = CredChange(ref, 1, ident);
    CFRelease(ref);
    __KRBReleaseUTF8String(ident);

    return res;
}
		
CFStringRef
NAHCopyReferenceKey(NAHSelectionRef selection)
{
    CFStringRef type;
    if (selection->client == NULL)
	return NULL;

    switch (selection->mech) {
    case GSS_KERBEROS:
    case GSS_KERBEROS_PKU2U:
    case GSS_KERBEROS_IAKERB:
	type = CFSTR("krb5");
	break;
    case GSS_NTLM:
	type = CFSTR("ntlm");
	break;
    default:
	return NULL;
    }
    return CFStringCreateWithFormat(NULL, 0, CFSTR("%@:%@"),
				    type, selection->client);
}

void
NAHFindByLabelAndRelease(CFStringRef identifier)
{
    OSStatus ret;
    char *str;
    
    nalog(CFSTR("NAHFindByLabelAndRelease: looking for label %@"), identifier);

    ret = __KRBCreateUTF8StringFromCFString(identifier, &str);
    if (ret)
	return;

    gss_iter_creds(NULL, 0, GSS_C_NO_OID, ^(gss_OID mech, gss_cred_id_t cred) {
	    OM_uint32 min_stat, maj_stat;
	    gss_buffer_desc buffer;

	    if (cred == NULL)
		return;

	    maj_stat = gss_cred_label_get(&min_stat, cred, nah_created, &buffer);
	    if (maj_stat) {
		gss_release_cred(&min_stat, &cred);
		return;
	    }
	    gss_release_buffer(&min_stat, &buffer);
	    
	    buffer.value = NULL;
	    buffer.length = 0;

	    /* if there is a label, unhold */
	    maj_stat = gss_cred_label_get(&min_stat, cred, str, &buffer);
	    gss_release_buffer(&min_stat, &buffer);
	    if (maj_stat == GSS_S_COMPLETE) {
		nalog(CFSTR("NAHFindByLabelAndRelease: found credential unholding"));
		gss_cred_label_set(&min_stat, cred, str, NULL);
		gss_cred_unhold(&min_stat, cred);
	    }
	    gss_release_cred(&min_stat, &cred);
	});

    __KRBReleaseUTF8String(str);
}

Boolean
NAHCredAddReference(CFStringRef referenceKey)
{
    return CredChange(referenceKey, 1, NULL);
}

Boolean
NAHCredRemoveReference(CFStringRef referenceKey)
{
    return CredChange(referenceKey, -1, NULL);
}

/*
 *
 */

static CFStringRef kDomainKey = CFSTR("domain");
static CFStringRef kUsername = CFSTR("user");

static CFStringRef kMech = CFSTR("mech");
static CFStringRef kClient = CFSTR("client");


static void
add_user_selections(NAHRef na)
{
    CFArrayRef array;
    enum NAHMechType mech = GSS_KERBEROS;
    CFIndex n;

    array = CFPreferencesCopyAppValue(CFSTR("UserSelections"),
				      CFSTR("com.apple.NetworkAuthenticationHelper"));
    if (array == NULL || CFGetTypeID(array) != CFArrayGetTypeID()) {
	CFRELEASE(array);
	return;
    }


    for (n = 0; n < CFArrayGetCount(array); n++) {
	CFDictionaryRef dict = CFArrayGetValueAtIndex(array, n);
	CFStringRef server = NULL;
	CFStringRef d, u, m, c;

	if (CFGetTypeID(dict) != CFDictionaryGetTypeID())
	    continue;

	m = CFDictionaryGetValue(dict, kMech);
	d = CFDictionaryGetValue(dict, kDomainKey);
	u = CFDictionaryGetValue(dict, kUsername);
	c = CFDictionaryGetValue(dict, kClient);

	if (c == NULL || CFGetTypeID(c) != CFStringGetTypeID())
	    continue;
	if (m == NULL || CFGetTypeID(m) != CFStringGetTypeID())
	    continue;
	if (d == NULL || CFGetTypeID(d) != CFStringGetTypeID())
	    continue;
	if (u == NULL && CFGetTypeID(u) != CFStringGetTypeID())
	    continue;

	/* find if matching */
	/* exact matching for now, should really be domain matching */
	if (CFStringCompare(d, na->hostname, kCFCompareCaseInsensitive) != kCFCompareEqualTo)
	    continue;

	if (u == NULL) {
	    if (CFStringCompare(d, na->username, 0) != kCFCompareEqualTo)
		continue;
	}

	mech = name2mech(m);
	if (mech == NO_MECH)
	    continue;

	server = CFStringCreateWithFormat(na->alloc, 0, CFSTR("%@@%@"),
					  na->service, na->hostname);

	/* add selection */
	if (server && c)
	    addSelection(na, c, NULL, server, NULL, mech, NULL, true);
	CFRELEASE(server);
    }
    CFRelease(array);
}




/*
 * GSS-API Support
 */

gss_cred_id_t
NAHSelectionGetGSSCredential(NAHSelectionRef selection)
{
    if (!wait_result(selection))
	return NULL;

    return NULL;
}

gss_name_t
NAHSelectionGetGSSAcceptorName(NAHSelectionRef selection)
{
    if (!wait_result(selection))
	return NULL;

    return NULL;
}

gss_OID
NAHSelectionGetGSSMech(NAHSelectionRef selection)
{
    if (!wait_result(selection))
	return  NULL;

    return NULL;
}

/*
 * These are the maching the OID version kGSSAPIMech<name>OID
 */

CFStringRef kGSSAPIMechNTLM = CFSTR("NTLM");
CFStringRef kGSSAPIMechKerberos = CFSTR("Kerberos");
CFStringRef kGSSAPIMechKerberosU2U = CFSTR("KerberosUser2User");
CFStringRef kGSSAPIMechKerberosMicrosoft = CFSTR("KerberosMicrosoft");
CFStringRef kGSSAPIMechIAKerb = CFSTR("IAKerb");
CFStringRef kGSSAPIMechPKU2U = CFSTR("PKU2U");
CFStringRef kGSSAPIMechSPNEGO = CFSTR("SPENGO");