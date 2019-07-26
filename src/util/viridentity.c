/*
 * viridentity.c: helper APIs for managing user identities
 *
 * Copyright (C) 2012-2013 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library;  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include <unistd.h>
#if WITH_SELINUX
# include <selinux/selinux.h>
#endif

#include "internal.h"
#include "viralloc.h"
#include "virerror.h"
#include "viridentity.h"
#include "virlog.h"
#include "virobject.h"
#include "virthread.h"
#include "virutil.h"
#include "virstring.h"
#include "virprocess.h"
#include "virtypedparam.h"

#define VIR_FROM_THIS VIR_FROM_IDENTITY

VIR_LOG_INIT("util.identity");

struct _virIdentity {
    virObject parent;

    int nparams;
    int maxparams;
    virTypedParameterPtr params;
};

static virClassPtr virIdentityClass;
static virThreadLocal virIdentityCurrent;

static void virIdentityDispose(void *obj);

static int virIdentityOnceInit(void)
{
    if (!VIR_CLASS_NEW(virIdentity, virClassForObject()))
        return -1;

    if (virThreadLocalInit(&virIdentityCurrent,
                           (virThreadLocalCleanup)virObjectUnref) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot initialize thread local for current identity"));
        return -1;
    }

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virIdentity);

/**
 * virIdentityGetCurrent:
 *
 * Get the current identity associated with this thread. The
 * caller will own a reference to the returned identity, but
 * must not modify the object in any way, other than to
 * release the reference when done with virObjectUnref
 *
 * Returns: a reference to the current identity, or NULL
 */
virIdentityPtr virIdentityGetCurrent(void)
{
    virIdentityPtr ident;

    if (virIdentityInitialize() < 0)
        return NULL;

    ident = virThreadLocalGet(&virIdentityCurrent);
    return virObjectRef(ident);
}


/**
 * virIdentitySetCurrent:
 *
 * Set the new identity to be associated with this thread.
 * The caller should not modify the passed identity after
 * it has been set, other than to release its own reference.
 *
 * Returns 0 on success, or -1 on error
 */
int virIdentitySetCurrent(virIdentityPtr ident)
{
    virIdentityPtr old;

    if (virIdentityInitialize() < 0)
        return -1;

    old = virThreadLocalGet(&virIdentityCurrent);

    if (virThreadLocalSet(&virIdentityCurrent,
                          virObjectRef(ident)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to set thread local identity"));
        virObjectUnref(ident);
        return -1;
    }

    virObjectUnref(old);

    return 0;
}


/**
 * virIdentityGetSystem:
 *
 * Returns an identity that represents the system itself.
 * This is the identity that the process is running as
 *
 * Returns a reference to the system identity, or NULL
 */
virIdentityPtr virIdentityGetSystem(void)
{
    VIR_AUTOFREE(char *) username = NULL;
    VIR_AUTOFREE(char *) groupname = NULL;
    unsigned long long startTime;
    virIdentityPtr ret = NULL;
#if WITH_SELINUX
    security_context_t con;
#endif

    if (!(ret = virIdentityNew()))
        goto error;

    if (virIdentitySetProcessID(ret, getpid()) < 0)
        goto error;

    if (virProcessGetStartTime(getpid(), &startTime) < 0)
        goto error;
    if (startTime != 0 &&
        virIdentitySetProcessTime(ret, startTime) < 0)
        goto error;

    if (!(username = virGetUserName(geteuid())))
        return ret;
    if (virIdentitySetUserName(ret, username) < 0)
        goto error;
    if (virIdentitySetUNIXUserID(ret, getuid()) < 0)
        goto error;

    if (!(groupname = virGetGroupName(getegid())))
        return ret;
    if (virIdentitySetGroupName(ret, groupname) < 0)
        goto error;
    if (virIdentitySetUNIXGroupID(ret, getgid()) < 0)
        goto error;

#if WITH_SELINUX
    if (is_selinux_enabled() > 0) {
        if (getcon(&con) < 0) {
            virReportSystemError(errno, "%s",
                                 _("Unable to lookup SELinux process context"));
            return ret;
        }
        if (virIdentitySetSELinuxContext(ret, con) < 0) {
            freecon(con);
            goto error;
        }
        freecon(con);
    }
#endif

    return ret;

 error:
    virObjectUnref(ret);
    return NULL;
}


/**
 * virIdentityNew:
 *
 * Creates a new empty identity object. After creating, one or
 * more identifying attributes should be set on the identity.
 *
 * Returns: a new empty identity
 */
virIdentityPtr virIdentityNew(void)
{
    virIdentityPtr ident;

    if (virIdentityInitialize() < 0)
        return NULL;

    if (!(ident = virObjectNew(virIdentityClass)))
        return NULL;

    return ident;
}


static void virIdentityDispose(void *object)
{
    virIdentityPtr ident = object;

    virTypedParamsFree(ident->params, ident->nparams);
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetUserName(virIdentityPtr ident,
                           const char **username)
{
    *username = NULL;
    return virTypedParamsGetString(ident->params,
                                   ident->nparams,
                                   VIR_CONNECT_IDENTITY_USER_NAME,
                                   username);
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetUNIXUserID(virIdentityPtr ident,
                             uid_t *uid)
{
    unsigned long long val;
    int rc;

    *uid = -1;
    rc = virTypedParamsGetULLong(ident->params,
                                 ident->nparams,
                                 VIR_CONNECT_IDENTITY_UNIX_USER_ID,
                                 &val);
    if (rc <= 0)
        return rc;

    *uid = (uid_t)val;

    return 1;
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetGroupName(virIdentityPtr ident,
                            const char **groupname)
{
    *groupname = NULL;
    return virTypedParamsGetString(ident->params,
                                   ident->nparams,
                                   VIR_CONNECT_IDENTITY_GROUP_NAME,
                                   groupname);
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetUNIXGroupID(virIdentityPtr ident,
                              gid_t *gid)
{
    unsigned long long val;
    int rc;

    *gid = -1;
    rc = virTypedParamsGetULLong(ident->params,
                                 ident->nparams,
                                 VIR_CONNECT_IDENTITY_UNIX_GROUP_ID,
                                 &val);
    if (rc <= 0)
        return rc;

    *gid = (gid_t)val;

    return 1;
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetProcessID(virIdentityPtr ident,
                            pid_t *pid)
{
    long long val;
    int rc;

    *pid = 0;
    rc = virTypedParamsGetLLong(ident->params,
                                ident->nparams,
                                VIR_CONNECT_IDENTITY_PROCESS_ID,
                                &val);
    if (rc <= 0)
        return rc;

    *pid = (pid_t)val;

    return 1;
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetProcessTime(virIdentityPtr ident,
                              unsigned long long *timestamp)
{
    *timestamp = 0;
    return virTypedParamsGetULLong(ident->params,
                                   ident->nparams,
                                   VIR_CONNECT_IDENTITY_PROCESS_TIME,
                                   timestamp);
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetSASLUserName(virIdentityPtr ident,
                               const char **username)
{
    *username = NULL;
    return virTypedParamsGetString(ident->params,
                                   ident->nparams,
                                   VIR_CONNECT_IDENTITY_SASL_USER_NAME,
                                   username);
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetX509DName(virIdentityPtr ident,
                            const char **dname)
{
    *dname = NULL;
    return virTypedParamsGetString(ident->params,
                                   ident->nparams,
                                   VIR_CONNECT_IDENTITY_X509_DISTINGUISHED_NAME,
                                   dname);
}


/*
 * Returns: 0 if not present, 1 if present, -1 on error
 */
int virIdentityGetSELinuxContext(virIdentityPtr ident,
                                 const char **context)
{
    *context = NULL;
    return virTypedParamsGetString(ident->params,
                                   ident->nparams,
                                   VIR_CONNECT_IDENTITY_SELINUX_CONTEXT,
                                   context);
}


int virIdentitySetUserName(virIdentityPtr ident,
                           const char *username)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_USER_NAME)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddString(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_USER_NAME,
                                   username);
}


int virIdentitySetUNIXUserID(virIdentityPtr ident,
                             uid_t uid)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_UNIX_USER_ID)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddULLong(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_UNIX_USER_ID,
                                   uid);
}


int virIdentitySetGroupName(virIdentityPtr ident,
                            const char *groupname)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_GROUP_NAME)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddString(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_GROUP_NAME,
                                   groupname);
}


int virIdentitySetUNIXGroupID(virIdentityPtr ident,
                              gid_t gid)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_UNIX_GROUP_ID)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddULLong(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_UNIX_GROUP_ID,
                                   gid);
}


int virIdentitySetProcessID(virIdentityPtr ident,
                            pid_t pid)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_PROCESS_ID)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddLLong(&ident->params,
                                  &ident->nparams,
                                  &ident->maxparams,
                                  VIR_CONNECT_IDENTITY_PROCESS_ID,
                                  pid);
}


int virIdentitySetProcessTime(virIdentityPtr ident,
                              unsigned long long timestamp)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_PROCESS_TIME)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddULLong(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_PROCESS_TIME,
                                   timestamp);
}



int virIdentitySetSASLUserName(virIdentityPtr ident,
                               const char *username)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_SASL_USER_NAME)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddString(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_SASL_USER_NAME,
                                   username);
}


int virIdentitySetX509DName(virIdentityPtr ident,
                            const char *dname)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_X509_DISTINGUISHED_NAME)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddString(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_X509_DISTINGUISHED_NAME,
                                   dname);
}


int virIdentitySetSELinuxContext(virIdentityPtr ident,
                                 const char *context)
{
    if (virTypedParamsGet(ident->params,
                          ident->nparams,
                          VIR_CONNECT_IDENTITY_SELINUX_CONTEXT)) {
        virReportError(VIR_ERR_OPERATION_DENIED, "%s",
                       _("Identity attribute is already set"));
        return -1;
    }

    return virTypedParamsAddString(&ident->params,
                                   &ident->nparams,
                                   &ident->maxparams,
                                   VIR_CONNECT_IDENTITY_SELINUX_CONTEXT,
                                   context);
}


int virIdentitySetParameters(virIdentityPtr ident,
                             virTypedParameterPtr params,
                             int nparams)
{
    if (virTypedParamsValidate(params, nparams,
                               VIR_CONNECT_IDENTITY_USER_NAME,
                               VIR_TYPED_PARAM_STRING,
                               VIR_CONNECT_IDENTITY_UNIX_USER_ID,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_CONNECT_IDENTITY_GROUP_NAME,
                               VIR_TYPED_PARAM_STRING,
                               VIR_CONNECT_IDENTITY_UNIX_GROUP_ID,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_CONNECT_IDENTITY_PROCESS_ID,
                               VIR_TYPED_PARAM_LLONG,
                               VIR_CONNECT_IDENTITY_PROCESS_TIME,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_CONNECT_IDENTITY_SASL_USER_NAME,
                               VIR_TYPED_PARAM_STRING,
                               VIR_CONNECT_IDENTITY_X509_DISTINGUISHED_NAME,
                               VIR_TYPED_PARAM_STRING,
                               VIR_CONNECT_IDENTITY_SELINUX_CONTEXT,
                               VIR_TYPED_PARAM_STRING,
                               NULL) < 0)
        return -1;

    virTypedParamsFree(ident->params, ident->nparams);
    ident->params = NULL;
    ident->nparams = 0;
    ident->maxparams = 0;
    if (virTypedParamsCopy(&ident->params, params, nparams) < 0)
        return -1;
    ident->nparams = nparams;
    ident->maxparams = nparams;

    return 0;
}


int virIdentityGetParameters(virIdentityPtr ident,
                             virTypedParameterPtr *params,
                             int *nparams)
{
    *params = NULL;
    *nparams = 0;

    if (virTypedParamsCopy(params, ident->params, ident->nparams) < 0)
        return -1;

    *nparams = ident->nparams;

    return 0;
}
