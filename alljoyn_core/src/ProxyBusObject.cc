/**
 * @file
 *
 * This file implements the ProxyBusObject class.
 */

/******************************************************************************
 *    Copyright (c) Open Connectivity Foundation (OCF), AllJoyn Open Source
 *    Project (AJOSP) Contributors and others.
 *
 *    SPDX-License-Identifier: Apache-2.0
 *
 *    All rights reserved. This program and the accompanying materials are
 *    made available under the terms of the Apache License, Version 2.0
 *    which accompanies this distribution, and is available at
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Copyright (c) Open Connectivity Foundation and Contributors to AllSeen
 *    Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for
 *    any purpose with or without fee is hereby granted, provided that the
 *    above copyright notice and this permission notice appear in all
 *    copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *    WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 *    WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 *    AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 *    DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 *    PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 *    TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 *    PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

#include <qcc/platform.h>

#include <map>
#include <set>
#include <vector>

#include <qcc/Condition.h>
#include <qcc/Debug.h>
#include <qcc/Event.h>
#include <qcc/ManagedObj.h>
#include <qcc/Mutex.h>
#include <qcc/String.h>
#include <qcc/StringSource.h>
#include <qcc/Util.h>

#include <alljoyn/AllJoynStd.h>
#include <alljoyn/BusAttachment.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/InterfaceDescription.h>
#include <alljoyn/Message.h>
#include <alljoyn/ProxyBusObject.h>
#include <alljoyn/Status.h>

#include "AllJoynPeerObj.h"
#include "BusInternal.h"
#include "LocalTransport.h"
#include "Router.h"
#include "XmlHelper.h"

#define QCC_MODULE "ALLJOYN_PBO"

#define SYNC_METHOD_ALERTCODE_OK     0
#define SYNC_METHOD_ALERTCODE_ABORT  1

using namespace qcc;
using namespace std;

namespace ajn {

#if defined(QCC_OS_GROUP_WINDOWS)
#pragma pack(push, CBContext, 4)
#endif
template <typename _cbType> struct CBContext {
    CBContext(ProxyBusObject::Listener* listener, _cbType callback, void* context) :
        listener(listener), callback(callback), context(context) { }

    ProxyBusObject::Listener* listener;
    _cbType callback;
    void* context;
};
#if defined(QCC_OS_GROUP_WINDOWS)
#pragma pack(pop, CBContext)
#endif

struct _PropertiesChangedCB {
    _PropertiesChangedCB(ProxyBusObject::PropertiesChangedListener& listener,
                         const char** properties,
                         size_t numProps,
                         void* context) :
        listener(listener), context(context), isRegistered(true), numRunning(0)
    {
        if (properties) {
            for (size_t i = 0; i < numProps; ++i) {
                this->properties.insert(String(properties[i]));
            }
        }
    }

    ProxyBusObject::PropertiesChangedListener& listener;
    void* context;
    set<std::string> properties;  // Properties to monitor - empty set == all properties.
    bool isRegistered;
    int32_t numRunning;
    _PropertiesChangedCB& operator=(const _PropertiesChangedCB&) { return *this; }
};

typedef ManagedObj<_PropertiesChangedCB> PropertiesChangedCB;

class CachedProps {
    qcc::Mutex lock;
    typedef std::map<std::string, MsgArg> ValueMap;
    ValueMap values;
    const InterfaceDescription* description;
    bool isFullyCacheable;
    size_t numProperties;
    uint32_t lastMessageSerial;
    bool enabled;

    bool IsCacheable(const char* propname);
    bool IsValidMessageSerial(uint32_t messageSerial);

  public:
    CachedProps() :
        lock(LOCK_LEVEL_PROXYBUSOBJECT_CACHEDPROPS_LOCK), values(), description(NULL),
        isFullyCacheable(false), numProperties(0), lastMessageSerial(0), enabled(false) { }

    CachedProps(const InterfaceDescription*intf) :
        lock(LOCK_LEVEL_PROXYBUSOBJECT_CACHEDPROPS_LOCK), values(), description(intf),
        isFullyCacheable(false), lastMessageSerial(0), enabled(false)
    {
        numProperties = description->GetProperties();
        if (numProperties > 0) {
            isFullyCacheable = true;
            const InterfaceDescription::Property** props = new const InterfaceDescription::Property* [numProperties];
            description->GetProperties(props, numProperties);
            for (size_t i = 0; i < numProperties; ++i) {
                if (props[i]->cacheable == false) {
                    isFullyCacheable = false;
                    break;
                }
            }
            delete[] props;
        }
    }

    CachedProps(const CachedProps& other) :
        lock(LOCK_LEVEL_PROXYBUSOBJECT_CACHEDPROPS_LOCK), values(other.values), description(other.description),
        isFullyCacheable(other.isFullyCacheable), numProperties(other.numProperties),
        lastMessageSerial(other.lastMessageSerial), enabled(other.enabled) { }

    CachedProps& operator=(const CachedProps& other) {
        if (&other != this) {
            values = other.values;
            description = other.description;
            isFullyCacheable = other.isFullyCacheable;
            numProperties = other.numProperties;
            lastMessageSerial = other.lastMessageSerial;
            enabled = other.enabled;
        }
        return *this;
    }

    ~CachedProps() { }

    bool Get(const char* propname, MsgArg& val);
    bool GetAll(MsgArg& val);
    void Set(const char* propname, const MsgArg& val, const uint32_t messageSerial);
    void SetAll(const MsgArg& allValues, const uint32_t messageSerial);
    void PropertiesChanged(MsgArg* changed, size_t numChanged, MsgArg* invalidated, size_t numInvalidated, const uint32_t messageSerial);
    void Enable();
};


/**
 * Internal context structure used between synchronous method_call and method_return
 */
struct _SyncReplyContext {
    _SyncReplyContext(BusAttachment& bus) : replyMsg(bus), thread(Thread::GetThread()) { }
    bool operator<(_SyncReplyContext& other) const { return this < &other; }
    Message replyMsg;
    Thread* thread;
    Event event;
};
typedef ManagedObj<_SyncReplyContext> SyncReplyContext;

class ProxyBusObject::Internal : public MessageReceiver, public BusAttachment::AddMatchAsyncCB {
    class AddMatchCBInfo {
      public:
        String ifaceName;
        bool& addingRef;
        AddMatchCBInfo(const String& iface, bool& addingRef) : ifaceName(iface), addingRef(addingRef) { }
      private:
        AddMatchCBInfo& operator=(const AddMatchCBInfo& other);
    };

    class MatchRuleInfo {
      public:
        bool adding;
        uint32_t refCount;
        MatchRuleInfo() : adding(false), refCount(0) { }
        MatchRuleInfo(bool adding) : adding(adding), refCount(0) { }
      private:
        MatchRuleInfo& operator=(const MatchRuleInfo& other);
    };

  public:
    Internal() :
        bus(nullptr),
        sessionId(0),
        hasProperties(false),
        isSecure(false),
        cacheProperties(false),
        lock(LOCK_LEVEL_PROXYBUSOBJECT_INTERNAL_LOCK),
        registeredPropChangedHandler(false),
        handlerThreads()
    {
        QCC_DbgPrintf(("Creating empty PBO internal: %p", this));
    }
    Internal(BusAttachment& bus,
             const char* objPath,
             const char* service,
             SessionId sessionId,
             bool isSecure) :
        bus(&bus),
        path(objPath ? objPath : ""),
        serviceName(service ? service : ""),
        uniqueName((!serviceName.empty() && (serviceName[0] == ':')) ? serviceName : ""),
        sessionId(sessionId),
        hasProperties(false),
        isSecure(isSecure),
        cacheProperties(false),
        lock(LOCK_LEVEL_PROXYBUSOBJECT_INTERNAL_LOCK),
        registeredPropChangedHandler(false),
        handlerThreads()
    {
        QCC_DbgPrintf(("Creating PBO internal: %p   path=%s   serviceName=%s   uniqueName=%s", this, path.c_str(), serviceName.c_str(), uniqueName.c_str()));
    }
    Internal(BusAttachment& bus,
             const char* objPath,
             const char* service,
             const char* unique,
             SessionId sessionId,
             bool isSecure) :
        bus(&bus),
        path(objPath ? objPath : ""),
        serviceName(service ? service : ""),
        uniqueName(unique ? unique : ""),
        sessionId(sessionId),
        hasProperties(false),
        isSecure(isSecure),
        cacheProperties(false),
        lock(LOCK_LEVEL_PROXYBUSOBJECT_INTERNAL_LOCK),
        registeredPropChangedHandler(false),
        handlerThreads()
    {
        QCC_DbgPrintf(("Creating PBO internal: %p   path=%s   serviceName=%s   uniqueName=%s", this, path.c_str(), serviceName.c_str(), uniqueName.c_str()));
    }

    ~Internal();

    /**
     * @internal
     * Add PropertiesChanged match rule for an interface
     *
     * @param intf the interface name
     * @param blocking true if this method may block on the AddMatch call
     */
    void AddPropertiesChangedRule(const char* intf, bool blocking);

    /**
     * @internal
     * Remove PropertiesChanged match rule for an interface
     *
     * @param intf the interface name
     */
    void RemovePropertiesChangedRule(const char* intf);

    /**
     * @internal
     * Remove all PropertiesChanged match rules for this proxy
     */
    void RemoveAllPropertiesChangedRules();

    /**
     * @internal
     * Handle property changed signals. (Internal use only)
     */
    void PropertiesChangedHandler(const InterfaceDescription::Member* member, const char* srcPath, Message& message);


    /**
     * @internal
     * Handle property AddMatch reply. (Internal use only)
     */
    void AddMatchCB(QStatus status, void* context);

    bool operator==(const ProxyBusObject::Internal& other) const
    {
        return ((this == &other) ||
                ((path == other.path) && (serviceName == other.serviceName)));
    }

    bool operator<(const ProxyBusObject::Internal& other) const
    {
        return ((path < other.path) ||
                ((path == other.path) && (serviceName < other.serviceName)));
    }

    BusAttachment* bus;                 /**< Bus associated with object */
    String path;                        /**< Object path of this object */
    String serviceName;                 /**< Remote destination alias */
    mutable String uniqueName;          /**< Remote destination unique name */
    SessionId sessionId;                /**< Session to use for communicating with remote object */
    bool hasProperties;                 /**< True if proxy object implements properties */
    mutable RemoteEndpoint b2bEp;       /**< B2B endpoint to use or NULL to indicates normal sessionId based routing */
    bool isSecure;                      /**< Indicates if this object is secure or not */
    bool cacheProperties;               /**< true if cacheable properties are cached */
    mutable Mutex lock;                 /**< Lock that protects access to internal state */
    Condition listenerDone;             /**< Signals that the properties changed listener is done */
    Condition handlerDone;              /**< Signals that the properties changed signal handler is done */
    Condition addMatchDone;             /**< Signals that AddMatch call has completed */
    bool registeredPropChangedHandler;  /**< true if our PropertiesChangedHandler is registered */
    map<qcc::Thread*, _PropertiesChangedCB*> handlerThreads;   /**< Thread actively calling PropertiesChangedListeners */

    /** The interfaces this object implements */
    map<std::string, const InterfaceDescription*> ifaces;

    /** The property caches for the various interfaces */
    mutable map<std::string, CachedProps> caches;

    /** Names of child objects of this object */
    vector<ProxyBusObject> children;

    /** Map of outstanding synchronous method calls to ProxyBusObjects. */
    mutable map<const ProxyBusObject* const, set<SyncReplyContext> > syncMethodCalls;
    mutable Condition syncMethodComplete;

    /** Match rule book keeping */
    typedef map<std::string, MatchRuleInfo> MatchRuleBookKeeping;
    MatchRuleBookKeeping matchRuleBookKeeping;

    /** Property changed handlers */
    multimap<std::string, PropertiesChangedCB> propertiesChangedCBs;
};

ProxyBusObject::Internal::~Internal()
{
    /* remove match rules added by the property caching & change notification mechanism */
    RemoveAllPropertiesChangedRules();

    lock.Lock(MUTEX_CONTEXT);
    bool unregHandler = registeredPropChangedHandler;
    lock.Unlock(MUTEX_CONTEXT);

    QCC_DbgPrintf(("Destroying PBO internal (%p) for %s on %s (%s)", this, path.c_str(), serviceName.c_str(), uniqueName.c_str()));
    if (bus) {
        if (unregHandler) {
            /*
             * Unregister the PropertiesChanged signal handler without holding
             * the PBO lock, because the signal handler itself acquires the
             * lock. The unregistration procedure busy-waits for a signal
             * handler to finish before proceeding with the unregistration, so
             * if we hold the lock here, we can create a deadlock.
             */
            const InterfaceDescription* iface = bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
            if (iface) {
                bus->UnregisterSignalHandler(this,
                                             static_cast<MessageReceiver::SignalHandler>(&Internal::PropertiesChangedHandler),
                                             iface->GetMember("PropertiesChanged"),
                                             path.c_str());
            }
        }

        bus->UnregisterAllHandlers(this);
    }

    lock.Lock(MUTEX_CONTEXT);
    /* Clean up properties changed listeners */
    while (!handlerThreads.empty()) {
        /*
         * The Properties Changed signal handler is still running.
         * Wait for it to complete.
         */
        handlerDone.Wait(lock);
    }

    while (!propertiesChangedCBs.empty()) {
        PropertiesChangedCB ctx = propertiesChangedCBs.begin()->second;
        ctx->isRegistered = false;
        propertiesChangedCBs.erase(propertiesChangedCBs.begin());
    }
    lock.Unlock(MUTEX_CONTEXT);
}

/**
 * Figure out the new status code based on the reply message's error.
 * If the status code is ER_BUS_REPLY_IS_ERROR_MESSAGE then the error
 * message is searched to compute the status code.
 * @param reply the reply message
 * @param[in,out] status the status code
 */
static void GetReplyErrorStatus(Message& reply, QStatus& status)
{
    if (ER_BUS_REPLY_IS_ERROR_MESSAGE != status) {
        return;
    }
    if (reply->GetErrorName() == NULL) {
        return;
    }
    if (strcmp(reply->GetErrorName(), PermissionMgmtObj::ERROR_PERMISSION_DENIED) == 0) {
        status = ER_PERMISSION_DENIED;
    } else if (strcmp(reply->GetErrorName(), org::alljoyn::Bus::ErrorName) == 0 && reply->GetArg(1)) {
        status = static_cast<QStatus>(reply->GetArg(1)->v_uint16);
    }
}

/**
 * Figure out the new status code based on the the reply message's error.
 * If the status code is ER_BUS_REPLY_IS_ERROR_MESSAGE then the error
 * message is searched to compute the status code.
 * @param reply the reply message
 * @param[in,out] status the status code
 * @param[out] errorName the error name
 * @param[out] errorDescription the error message
 */
static void GetReplyErrorStatusMessage(Message& reply, QStatus& status, qcc::String& errorName, qcc::String& errorDescription)
{
    GetReplyErrorStatus(reply, status);
    errorName = reply->GetErrorName(&errorDescription);
}

/**
 * Figure out whether the reply message is a permission denied error message.
 * If so, the status code will be replaced with ER_PERMISSION_DENIED.
 * @param reply the reply message
 * @param[in,out] status the status code
 */
static void AdjustErrorForPermissionDenied(Message& reply, QStatus& status)
{
    if (ER_PERMISSION_DENIED == status) {
        return;
    }
    if (ER_BUS_REPLY_IS_ERROR_MESSAGE != status) {
        return;
    }
    QStatus tmpStatus = status;
    GetReplyErrorStatus(reply, tmpStatus);
    if (ER_PERMISSION_DENIED == tmpStatus) {
        status = tmpStatus;
        return;
    }
}

static inline bool SecurityApplies(const ProxyBusObject* obj, const InterfaceDescription* ifc)
{
    InterfaceSecurityPolicy ifcSec = ifc->GetSecurityPolicy();
    if (ifcSec == AJ_IFC_SECURITY_REQUIRED) {
        return true;
    } else {
        return obj->IsSecure() && (ifcSec != AJ_IFC_SECURITY_OFF);
    }
}

/*
 * @internal
 *
 * Helper class which allows ProxyBusObject to retrieve introspection descriptions from
 * pre-16.04 nodes.
 *
 * @see ASACORE-2744
 */
class ProxyBusObject::LegacyIntrospectionHandler {
  public:
    LegacyIntrospectionHandler(ProxyBusObject* parent) :
        proxyBusObject(parent) {
    }

    LegacyIntrospectionHandler(const LegacyIntrospectionHandler& other) :
        proxyBusObject(other.proxyBusObject) {
    }

    /**
     * @internal
     * Detects if the given introspection XML was generated by a legacy object (pre-16.04).
     *
     * @param[in] xml The introspection XML.
     *
     * @return
     *       - True if the XML is from a legacy object.
     *       - False otherwise.
     */
    bool IsLegacyXml(const qcc::String& xml) const;

    /**
     * @internal
     * Detects if the given introspection XML contains
     * the "org.allseen.Introspectable" interface.
     *
     * @param[in] xml The introspection XML.
     *
     * @return
     *       - True if the XML contains the AllSeen introspection interface.
     *       - False otherwise.
     */
    bool RemoteObjectSupportsAllSeenIntrospection(const qcc::String& xml) const;

    /**
     * @internal
     * Parses an introspection XML generated by a legacy object (pre-16.04).
     *
     * Used by IntrospectRemoteObject() to provide compatibility with pre-16.04 objects.
     * For 16.04+ objects, IntrospectRemoteObject() only calls the remote object's Introspect() method.
     * Pre-16.04 implementations of Introspect() do not include descriptions in the returned
     * introspection XML. To obtain descriptions from pre-16.04 nodes, IntrospectWithDescription()
     * has to be called. When a pre-16.04 XML is detected, this function can be called to:
     *     - Fetch a list of the available description languages from the remote legacy object,
     *     - For each language, call IntrospectWithDescription(),
     *     - Pass the list of obtained XMLs with descriptions to XmlHelper which will parse them,
     *       adding the descriptions to the InterfaceDescription in the ProxyBusObject.
     *
     * @see ASACORE-2744
     *
     * @param[in] xml The introspection XML.
     * @param[in] identifier An optional identifying string to include in error logging messages.
     *
     * @return
     *       - ER_OK if the XMLs were successfully obtained and parsed.
     *       - An error status otherwise.
     */
    QStatus ParseLegacyXml(const char* xml, const char* identifier = nullptr);

  private:
    /**
     * @internal
     * Fetches all legacy introspection XMLs with descriptions and stores them in the map.
     *
     * This function:
     *     - Fetches a list of the available description languages from the remote legacy object
     *       by calling GetDescriptionLanguages() on the object,
     *     - For each language, calls IntrospectWithDescription(),
     *     - Adds the obtained XMLs with descriptions to the xmls map.
     *
     * @param[out] xmls The map with the obtained XMLs.
     *
     * @return
     *       - ER_OK if the XMLs were successfully obtained.
     *       - An error status otherwise.
     */
    QStatus GetXmlsWithDescriptions(XmlHelper::XmlToLanguageMap* xmls);

    /**
     * @internal
     * Fetches the available description languages from a remote legacy object.
     *
     * This function obtains a list of the available description languages from a pre-16.04
     * node and stores it in the languages set.
     *
     * @param[out] languages The set with the obtained languages.
     *
     * @return
     *       - ER_OK if the language list was successfully obtained.
     *       - An error status otherwise.
     */
    QStatus GetDescriptionLanguagesForLegacyNode(std::set<qcc::String>& languages);

    /**
     * @internal
     * Obtains the introspection XML with descriptions in a given language from a remote legacy object.
     *
     * This function calls IntrospectWithDescription() for a given language on the remote object.
     * If the call succeeds, the obtained introspection XML is stored in the xml argument.
     *
     * @param[out] xml The obtained introspection XML.
     * @param[in] languageTag The requested language description.
     *
     * @return
     *       - ER_OK if the XML was successfully obtained.
     *       - An error status otherwise.
     */
    QStatus GetDescriptionXmlForLanguage(qcc::String& xml, const qcc::String& languageTag);

    /**
     * @internal
     * Adds an introspection XML with descriptions to the given map.
     *
     * The map stores pairs: language tag + introspection XML stored as qcc::XmlParseContext.
     *
     * @param[in] xml The introspection XML to store.
     * @param[in] languageTag The language of the descriptions in the introspection XML.
     * @param[in, out] xmls The map to store the XMLs.
     *
     * @return
     *       - ER_OK if the XML was successfully transformed to qcc::XmlParseContext and stored.
     *       - An error status otherwise.
     */
    QStatus AddDescriptionXmlToMap(const qcc::String& xml, const qcc::String& languageTag, XmlHelper::XmlToLanguageMap* xmls) const;

    /**
     * @internal
     * Parses the introspection XML and the additional XMLs with descriptions.
     *
     * This method uses XmlHelper to parse the XML without descriptions obtained from the legacy
     * node (xml parameter), as well as the additional XMLs with descriptions in different languages
     * stored in the xmlsWithDescriptions map. The XML without descriptions is used to create the interface
     * structure (add members, properties, etc.). The XMLs from the map are only to decorate the created
     * interface with descriptions. As a result, the interface within the ProxyBusObject is filled
     * with descriptions in all the provided languages.
     *
     * @param[in] xml The introspection XML without descriptions.
     * @param[in] xmlsWithDescriptions Map containing XMLs with descriptions.
     * @param[in] identifier An optional identifying string to include in error logging messages.
     *
     * @return
     *       - ER_OK if the XMLs were successfully parsed.
     *       - An error status otherwise.
     */
    QStatus ParseXmlAndDescriptions(const char* xml, const XmlHelper::XmlToLanguageMap* xmlsWithDescriptions, const char* identifier = nullptr);

    ProxyBusObject* proxyBusObject;
};

bool ProxyBusObject::LegacyIntrospectionHandler::IsLegacyXml(const qcc::String& xml) const
{
    /* This function checks if the given introspection XML comes from a pre-16.04 node
     * (such a node and such an XML are called "legacy" here).
     * XMLs created by 16.04+ nodes, both TCL and SCL, will contain the 1.1 DTD
     * (see the org::allseen::Introspectable::IntrospectDocType string).
     * XMLs created by legacy SCL nodes will contain a DTD, such as
     * org::allseen::Introspectable::IntrospectDocType, but with version set to 1.0
     * instead of 1.1.
     * XMLs created by legacy TCL nodes do not contain the DTD at all.
     * Therefore, each XML which does not contain the 1.1 DTD is considered legacy here.
     */
    return xml.find(org::allseen::Introspectable::IntrospectDocType) == qcc::String::npos;
}

bool ProxyBusObject::LegacyIntrospectionHandler::RemoteObjectSupportsAllSeenIntrospection(const qcc::String& xml) const
{
    const qcc::String ajIntrospectableInterfaceElement =
        "<interface name=\"" + qcc::String(org::allseen::Introspectable::InterfaceName) + "\">";

    return xml.find(ajIntrospectableInterfaceElement) != qcc::String::npos;
}

QStatus ProxyBusObject::LegacyIntrospectionHandler::ParseLegacyXml(const char* xml, const char* ident)
{
    QStatus status;
    XmlHelper::XmlToLanguageMap descriptions;
    status = GetXmlsWithDescriptions(&descriptions);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to retrieve XMLs with descriptions for legacy node"));
        return status;
    }

    return ParseXmlAndDescriptions(xml, &descriptions, ident);
}

QStatus ProxyBusObject::LegacyIntrospectionHandler::GetXmlsWithDescriptions(XmlHelper::XmlToLanguageMap* xmls)
{
    QStatus status = ER_OK;

    std::set<qcc::String> languages;
    status = GetDescriptionLanguagesForLegacyNode(languages);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to retrieve list of available description languages for legacy node"));
        return status;
    }
    for (std::set<qcc::String>::const_iterator itL = languages.begin(); itL != languages.end(); itL++) {
        qcc::String xml;
        status = GetDescriptionXmlForLanguage(xml, *itL);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to obtain introspection XML for language %s", itL->c_str()));
            return status;
        }
        status = AddDescriptionXmlToMap(xml, *itL, xmls);
        if (status != ER_OK) {
            return status;
        }
    }

    return status;
}

QStatus ProxyBusObject::LegacyIntrospectionHandler::GetDescriptionLanguagesForLegacyNode(std::set<qcc::String>& languages)
{
    QStatus status = ER_OK;

    const InterfaceDescription* introspectableIntf = proxyBusObject->GetInterface(org::allseen::Introspectable::InterfaceName);
    if (introspectableIntf == nullptr) {
        introspectableIntf = proxyBusObject->internal->bus->GetInterface(org::allseen::Introspectable::InterfaceName);
        QCC_ASSERT(introspectableIntf != nullptr);
        proxyBusObject->AddInterface(*introspectableIntf);
    }

    const InterfaceDescription::Member* getLanguagesIntf = introspectableIntf->GetMember("GetDescriptionLanguages");
    QCC_ASSERT(getLanguagesIntf != nullptr);
    Message reply(*proxyBusObject->internal->bus);
    status = proxyBusObject->MethodCall(*getLanguagesIntf, nullptr, 0, reply, DefaultCallTimeout);
    if (status != ER_OK) {
        if (status == ER_BUS_REPLY_IS_ERROR_MESSAGE) {
            GetReplyErrorStatus(reply, status);
        }
        QCC_LogError(status, ("Failed to call GetDescriptionLanguages on remote legacy object"));
        return status;
    }

    MsgArg* retrievedLanguages;
    size_t numRetrievedLanguages;
    status = reply->GetArg(0)->Get("as", &numRetrievedLanguages, &retrievedLanguages);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to retrieve available languages from response"));
        return status;
    }

    for (size_t i = 0; i < numRetrievedLanguages; ++i) {
        char* language;
        status = retrievedLanguages[i].Get("s", &language);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to retrieve language tag from response"));
            return status;
        }
        languages.insert(language);
    }

    return status;
}

QStatus ProxyBusObject::LegacyIntrospectionHandler::GetDescriptionXmlForLanguage(qcc::String& xml, const qcc::String& languageTag)
{
    QStatus status = ER_OK;

    const InterfaceDescription* introspectableIntf = proxyBusObject->GetInterface(org::allseen::Introspectable::InterfaceName);
    if (introspectableIntf == nullptr) {
        introspectableIntf = proxyBusObject->internal->bus->GetInterface(org::allseen::Introspectable::InterfaceName);
        QCC_ASSERT(introspectableIntf != nullptr);
        proxyBusObject->AddInterface(*introspectableIntf);
    }

    const InterfaceDescription::Member* introspectWithDescriptionIntf = introspectableIntf->GetMember("IntrospectWithDescription");
    QCC_ASSERT(introspectWithDescriptionIntf != nullptr);
    MsgArg msgArg("s", languageTag.c_str());
    Message reply(*proxyBusObject->internal->bus);
    status = proxyBusObject->MethodCall(*introspectWithDescriptionIntf, &msgArg, 1, reply, DefaultCallTimeout);
    if (status != ER_OK) {
        if (status == ER_BUS_REPLY_IS_ERROR_MESSAGE) {
            GetReplyErrorStatus(reply, status);
        }
        QCC_LogError(status, ("Failed to call IntrospectRemoteObject on remote legacy object"));
        return status;
    }

    char* introspectionXml = nullptr;
    const MsgArg* replyArg = reply->GetArg(0);
    status = replyArg->Get("s", &introspectionXml);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to parse introspection XML from response"));
        return status;
    }

    xml.assign(introspectionXml);
    return status;
}

QStatus ProxyBusObject::LegacyIntrospectionHandler::AddDescriptionXmlToMap(const qcc::String& xml, const qcc::String& languageTag, XmlHelper::XmlToLanguageMap* xmls) const
{
    QStatus status = ER_OK;

    StringSource source(xml);
    std::unique_ptr<XmlParseContext> xmlParseContext(new XmlParseContext(source));
    status = XmlElement::Parse(*xmlParseContext);
    if (status != ER_OK) {
        QCC_LogError(status, ("Error when parsing introspection XML"));
        return ER_BUS_BAD_XML;
    }

    xmls->insert(std::make_pair(languageTag, std::move(xmlParseContext)));
    return status;
}

QStatus ProxyBusObject::LegacyIntrospectionHandler::ParseXmlAndDescriptions(const char* xml, const XmlHelper::XmlToLanguageMap* xmlsWithDescriptions, const char* ident)
{
    StringSource source(xml);
    XmlParseContext pc(source);

    QStatus status = XmlElement::Parse(pc);
    if (status == ER_OK) {
        XmlHelper xmlHelper(proxyBusObject->internal->bus, ident ? ident : proxyBusObject->internal->path.c_str());
        status = xmlHelper.AddProxyObjects(*proxyBusObject, pc.GetRoot(), xmlsWithDescriptions);
    }
    return status;
}

QStatus ProxyBusObject::GetAllProperties(const char* iface, MsgArg& values, uint32_t timeout) const
{
    String errorName;
    String errorDescription;
    return GetAllProperties(iface, values, errorName, errorDescription, timeout);
}

QStatus ProxyBusObject::GetAllProperties(const char* iface, MsgArg& value, qcc::String& errorName, qcc::String& errorDescription, uint32_t timeout) const
{
    QStatus status;
    const InterfaceDescription* valueIface = internal->bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        /* If all values are stored in the cache, we can reply immediately */
        bool cached = false;
        internal->lock.Lock(MUTEX_CONTEXT);
        if (internal->cacheProperties) {
            map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
            if (it != internal->caches.end()) {
                cached = it->second.GetAll(value);
            }
        }
        internal->lock.Unlock(MUTEX_CONTEXT);
        if (cached) {
            QCC_DbgPrintf(("GetAllProperties(%s) -> cache hit", iface));
            return ER_OK;
        }

        QCC_DbgPrintf(("GetAllProperties(%s) -> perform method call", iface));
        uint8_t flags = 0;
        /*
         * If the object or the property interface is secure method call must be encrypted.
         */
        if (SecurityApplies(this, valueIface)) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        Message reply(*internal->bus);
        MsgArg arg = MsgArg("s", iface);
        const InterfaceDescription* propIface = internal->bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        if (propIface == NULL) {
            status = ER_BUS_NO_SUCH_INTERFACE;
        } else {
            const InterfaceDescription::Member* getAllProperties = propIface->GetMember("GetAll");
            QCC_ASSERT(getAllProperties);
            status = MethodCall(*getAllProperties, &arg, 1, reply, timeout, flags);
            if (ER_OK == status) {
                value = *(reply->GetArg(0));
                /* use the retrieved property values to update the cache, if applicable */
                internal->lock.Lock(MUTEX_CONTEXT);
                if (internal->cacheProperties) {
                    map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
                    if (it != internal->caches.end()) {
                        it->second.SetAll(value, reply->GetCallSerial());
                    }
                }
                internal->lock.Unlock(MUTEX_CONTEXT);
            } else {
                GetReplyErrorStatusMessage(reply, status, errorName, errorDescription);
            }
        }
    }
    return status;
}


void ProxyBusObject::GetAllPropsMethodCBCommon(Message& message, void* context, bool isCustomErrorSet)
{
    CBContext<Listener::GetPropertyAsyncCB>* ctxWithCustomError = nullptr;
    CBContext<Listener::GetPropertyCB>* ctx = nullptr;
    if (isCustomErrorSet) {
        ctxWithCustomError = reinterpret_cast<CBContext<Listener::GetAllPropertiesAsyncCB>*>(context);
    } else {
        ctx = reinterpret_cast<CBContext<Listener::GetAllPropertiesCB>*>(context);
    }

    QCC_ASSERT((ctx != nullptr) || (ctxWithCustomError != nullptr));

    std::pair<void*, qcc::String>* wrappedContext;
    if (isCustomErrorSet) {
        wrappedContext = reinterpret_cast<std::pair<void*, qcc::String>*>(ctxWithCustomError->context);
    } else {
        wrappedContext = reinterpret_cast<std::pair<void*, qcc::String>*>(ctx->context);
    }

    void* unwrappedContext = wrappedContext->first;
    const char* iface = wrappedContext->second.c_str();

    if (message->GetType() == MESSAGE_METHOD_RET) {
        /* use the retrieved property values to update the cache, if applicable */
        internal->lock.Lock(MUTEX_CONTEXT);
        if (internal->cacheProperties) {
            map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
            if (it != internal->caches.end()) {
                it->second.SetAll(*message->GetArg(0), message->GetCallSerial());
            }
        }
        internal->lock.Unlock(MUTEX_CONTEXT);
        /* alert the application */
        if (isCustomErrorSet) {
            // TODO: Should error strings be allowed on messages that are of type MESSAGE_METHOD_RET?
            String errorDescription;
            String errorName = message->GetErrorName(&errorDescription);
            (ctxWithCustomError->listener->*ctxWithCustomError->callback)(ER_OK, this, *message->GetArg(0), errorName, errorDescription, unwrappedContext);
            delete ctxWithCustomError;
        } else {
            (ctx->listener->*ctx->callback)(ER_OK, this, *message->GetArg(0), unwrappedContext);
            delete ctx;
        }

    } else {
        const MsgArg noVal;
        QStatus status = ER_BUS_NO_SUCH_PROPERTY;
        if (message->GetErrorName()) {
            const char* err;
            uint16_t rawStatus;
            if (message->GetArgs("sq", &err, &rawStatus) == ER_OK) {
                status = static_cast<QStatus>(rawStatus);
                QCC_DbgPrintf(("Asynch GetAllProperties call returned %s", err));
            }
        }
        if (isCustomErrorSet) {
            String errorDescription;
            String errorName = message->GetErrorName(&errorDescription);
            (ctxWithCustomError->listener->*ctxWithCustomError->callback)(status, this, noVal, errorName, errorDescription, unwrappedContext);
            delete ctxWithCustomError;
        } else {
            (ctx->listener->*ctx->callback)(status, this, noVal, unwrappedContext);
            delete ctx;
        }
    }
    delete wrappedContext;
}

void ProxyBusObject::GetAllPropsMethodCB(Message& message, void* context)
{
    GetAllPropsMethodCBCommon(message, context, false);
}

void ProxyBusObject::GetAllPropsMethodAsyncCB(Message& message, void* context)
{
    GetAllPropsMethodCBCommon(message, context, true);
}

QStatus ProxyBusObject::GetAllPropertiesAsyncCommon(const char* iface,
                                                    ProxyBusObject::Listener* listener,
                                                    ProxyBusObject::Listener::GetPropertyCB callback,
                                                    ProxyBusObject::Listener::GetPropertyAsyncCB callbackWithCustomError,
                                                    void* context,
                                                    uint32_t timeout)
{
    QCC_ASSERT((callback != nullptr) || (callbackWithCustomError != nullptr));
    QStatus status;
    const InterfaceDescription* valueIface = internal->bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        /* If all values are stored in the cache, we can reply immediately */
        bool cached = false;
        MsgArg value;
        internal->lock.Lock(MUTEX_CONTEXT);
        if (internal->cacheProperties) {
            map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
            if (it != internal->caches.end()) {
                cached = it->second.GetAll(value);
            }
        }
        internal->lock.Unlock(MUTEX_CONTEXT);
        if (cached) {
            QCC_DbgPrintf(("GetAllPropertiesAsync(%s) -> cache hit", iface));
            if (callback != nullptr) {
                internal->bus->GetInternal().GetLocalEndpoint()->ScheduleCachedGetPropertyReply(this, listener, callback, context, value);
            } else {
                internal->bus->GetInternal().GetLocalEndpoint()->ScheduleCachedGetPropertyReply(this, listener, callbackWithCustomError, context, value);
            }
            return ER_OK;
        }

        QCC_DbgPrintf(("GetAllPropertiesAsync(%s) -> perform method call", iface));
        uint8_t flags = 0;
        /*
         * If the object or the property interface is secure method call must be encrypted.
         */
        if (SecurityApplies(this, valueIface)) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        MsgArg arg = MsgArg("s", iface);
        const InterfaceDescription* propIface = internal->bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        if (propIface == NULL) {
            status = ER_BUS_NO_SUCH_INTERFACE;
        } else {
            std::pair<void*, qcc::String>* wrappedContext = new std::pair<void*, qcc::String>(context, iface);
            const InterfaceDescription::Member* getAllProperties = propIface->GetMember("GetAll");
            QCC_ASSERT(getAllProperties);

            if (callback != nullptr) {
                CBContext<Listener::GetAllPropertiesCB>* ctx = new CBContext<Listener::GetAllPropertiesCB>(listener, callback, wrappedContext);
                status = MethodCallAsync(*getAllProperties,
                                         this,
                                         static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::GetAllPropsMethodCB),
                                         &arg,
                                         1,
                                         reinterpret_cast<void*>(ctx),
                                         timeout,
                                         flags);
                if (status != ER_OK) {
                    delete wrappedContext;
                    delete ctx;
                }
            } else {
                CBContext<Listener::GetAllPropertiesAsyncCB>* ctx = new CBContext<Listener::GetAllPropertiesAsyncCB>(listener, callbackWithCustomError, wrappedContext);
                status = MethodCallAsync(*getAllProperties,
                                         this,
                                         static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::GetAllPropsMethodAsyncCB),
                                         &arg,
                                         1,
                                         reinterpret_cast<void*>(ctx),
                                         timeout,
                                         flags);
                if (status != ER_OK) {
                    delete wrappedContext;
                    delete ctx;
                }
            }
        }
    }
    return status;
}

QStatus ProxyBusObject::GetAllPropertiesAsync(const char* iface,
                                              ProxyBusObject::Listener* listener,
                                              ProxyBusObject::Listener::GetPropertyCB callback,
                                              void* context,
                                              uint32_t timeout)
{
    return GetAllPropertiesAsyncCommon(iface, listener, callback, nullptr, context, timeout);
}

QStatus ProxyBusObject::GetAllPropertiesAsync(const char* iface,
                                              ProxyBusObject::Listener* listener,
                                              ProxyBusObject::Listener::GetPropertyAsyncCB callback,
                                              void* context,
                                              uint32_t timeout)
{
    return GetAllPropertiesAsyncCommon(iface, listener, nullptr, callback, context, timeout);
}

QStatus ProxyBusObject::GetProperty(const char* iface, const char* property, MsgArg& value, uint32_t timeout) const
{
    String errorName;
    String errorDescription;
    return GetProperty(iface, property, value, errorName, errorDescription, timeout);
}

QStatus ProxyBusObject::GetProperty(const char* iface, const char* property, MsgArg& value, qcc::String& errorName, qcc::String& errorDescription, uint32_t timeout) const
{
    QStatus status;
    const InterfaceDescription* valueIface = internal->bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        /* if the property is cached, we can reply immediately */
        bool cached = false;
        internal->lock.Lock(MUTEX_CONTEXT);
        if (internal->cacheProperties) {
            map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
            if (it != internal->caches.end()) {
                cached = it->second.Get(property, value);
            }
        }
        internal->lock.Unlock(MUTEX_CONTEXT);
        if (cached) {
            QCC_DbgPrintf(("GetProperty(%s, %s) -> cache hit", iface, property));
            return ER_OK;
        }

        QCC_DbgPrintf(("GetProperty(%s, %s) -> perform method call", iface, property));
        uint8_t flags = 0;
        /*
         * If the object or the property interface is secure method call must be encrypted.
         */
        if (SecurityApplies(this, valueIface)) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        Message reply(*internal->bus);
        MsgArg inArgs[2];
        size_t numArgs = ArraySize(inArgs);
        MsgArg::Set(inArgs, numArgs, "ss", iface, property);
        const InterfaceDescription* propIface = internal->bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        if (propIface == NULL) {
            status = ER_BUS_NO_SUCH_INTERFACE;
        } else {
            const InterfaceDescription::Member* getProperty = propIface->GetMember("Get");
            QCC_ASSERT(getProperty);
            status = MethodCall(*getProperty, inArgs, numArgs, reply, timeout, flags);
            if (ER_OK == status) {
                value = *(reply->GetArg(0));
                /* use the retrieved property value to update the cache, if applicable */
                internal->lock.Lock(MUTEX_CONTEXT);
                if (internal->cacheProperties) {
                    map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
                    if (it != internal->caches.end()) {
                        it->second.Set(property, value, reply->GetCallSerial());
                    }
                }
                internal->lock.Unlock(MUTEX_CONTEXT);
            } else if (ER_BUS_REPLY_IS_ERROR_MESSAGE == status) {
                GetReplyErrorStatusMessage(reply, status, errorName, errorDescription);
            }
        }
    }
    return status;
}

void ProxyBusObject::GetPropMethodCBCommon(Message& message, void* context, bool isCustomErrorSet)
{
    CBContext<Listener::GetPropertyAsyncCB>* ctxWithCustomError = nullptr;
    CBContext<Listener::GetPropertyCB>* ctx = nullptr;
    if (isCustomErrorSet) {
        ctxWithCustomError = reinterpret_cast<CBContext<Listener::GetPropertyAsyncCB>*>(context);
    } else {
        ctx = reinterpret_cast<CBContext<Listener::GetPropertyCB>*>(context);
    }
    QCC_ASSERT((ctx != nullptr) || (ctxWithCustomError != nullptr));

    std::pair<void*, std::pair<qcc::String, qcc::String> >* wrappedContext;
    if (isCustomErrorSet) {
        wrappedContext = reinterpret_cast<std::pair<void*, std::pair<qcc::String, qcc::String> >*>(ctxWithCustomError->context);
    } else {
        wrappedContext = reinterpret_cast<std::pair<void*, std::pair<qcc::String, qcc::String> >*>(ctx->context);
    }
    void* unwrappedContext = wrappedContext->first;
    const char* iface = wrappedContext->second.first.c_str();
    const char* property = wrappedContext->second.second.c_str();

    if (message->GetType() == MESSAGE_METHOD_RET) {
        /* use the retrieved property value to update the cache, if applicable */
        internal->lock.Lock(MUTEX_CONTEXT);
        if (internal->cacheProperties) {
            map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
            if (it != internal->caches.end()) {
                it->second.Set(property, *message->GetArg(0), message->GetCallSerial());
            }
        }
        internal->lock.Unlock(MUTEX_CONTEXT);
        /* let the application know we've got a result */
        if (isCustomErrorSet) {
            // TODO: Should error strings be allowed on messages that are of type MESSAGE_METHOD_RET?
            String errorDescription;
            String errorName = message->GetErrorName(&errorDescription);
            (ctxWithCustomError->listener->*ctxWithCustomError->callback)(ER_OK, this, *message->GetArg(0), errorName, errorDescription, unwrappedContext);
            delete ctxWithCustomError;
        } else {
            (ctx->listener->*ctx->callback)(ER_OK, this, *message->GetArg(0), unwrappedContext);
            delete ctx;
        }
    } else {
        const MsgArg noVal;
        QStatus status = ER_BUS_NO_SUCH_PROPERTY;
        const char* err = NULL;
        if (message->GetErrorName() != NULL) {
            uint16_t rawStatus;
            if (message->GetArgs("sq", &err, &rawStatus) == ER_OK) {
                status = static_cast<QStatus>(rawStatus);
                QCC_DbgPrintf(("Asynch GetProperty call returned %s", err));
            }
        }

        if (isCustomErrorSet) {
            String errorDescription;
            String errorName = message->GetErrorName(&errorDescription);
            (ctxWithCustomError->listener->*ctxWithCustomError->callback)(status, this, noVal, errorName, errorDescription, unwrappedContext);
            delete ctxWithCustomError;
        } else {
            (ctx->listener->*ctx->callback)(status, this, noVal, unwrappedContext);
            delete ctx;
        }
    }

    delete wrappedContext;
}

void ProxyBusObject::GetPropMethodCB(Message& message, void* context)
{
    GetPropMethodCBCommon(message, context, false);
}

void ProxyBusObject::GetPropMethodAsyncCB(Message& message, void* context)
{
    GetPropMethodCBCommon(message, context, true);
}

QStatus ProxyBusObject::GetPropertyAsyncCommon(const char* iface,
                                               const char* property,
                                               ProxyBusObject::Listener* listener,
                                               ProxyBusObject::Listener::GetPropertyCB callback,
                                               ProxyBusObject::Listener::GetPropertyAsyncCB callbackWithCustomError,
                                               void* context,
                                               uint32_t timeout)
{
    QCC_ASSERT((callback != nullptr) || (callbackWithCustomError != nullptr));
    QStatus status;
    const InterfaceDescription* valueIface = internal->bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        /* if the property is cached, we can reply immediately */
        bool cached = false;
        MsgArg value;
        internal->lock.Lock(MUTEX_CONTEXT);
        if (internal->cacheProperties) {
            map<std::string, CachedProps>::iterator it = internal->caches.find(iface);
            if (it != internal->caches.end()) {
                cached = it->second.Get(property, value);
            }
        }
        internal->lock.Unlock(MUTEX_CONTEXT);
        if (cached) {
            QCC_DbgPrintf(("GetPropertyAsync(%s, %s) -> cache hit", iface, property));
            if (callback != nullptr) {
                internal->bus->GetInternal().GetLocalEndpoint()->ScheduleCachedGetPropertyReply(this, listener, callback, context, value);
            } else {
                internal->bus->GetInternal().GetLocalEndpoint()->ScheduleCachedGetPropertyReply(this, listener, callbackWithCustomError, context, value);
            }
            return ER_OK;
        }

        QCC_DbgPrintf(("GetProperty(%s, %s) -> perform method call", iface, property));
        uint8_t flags = 0;
        if (SecurityApplies(this, valueIface)) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        MsgArg inArgs[2];
        size_t numArgs = ArraySize(inArgs);
        MsgArg::Set(inArgs, numArgs, "ss", iface, property);
        const InterfaceDescription* propIface = internal->bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        if (propIface == NULL) {
            status = ER_BUS_NO_SUCH_INTERFACE;
        } else {
            /* we need to keep track of interface and property name to cache the GetProperty reply */
            std::pair<void*, std::pair<qcc::String, qcc::String> >* wrappedContext = new std::pair<void*, std::pair<qcc::String, qcc::String> >(context, std::make_pair(qcc::String(iface), qcc::String(property)));
            const InterfaceDescription::Member* getProperty = propIface->GetMember("Get");
            QCC_ASSERT(getProperty);

            if (callback != nullptr) {
                CBContext<Listener::GetPropertyCB>* ctx = new CBContext<Listener::GetPropertyCB>(listener, callback, wrappedContext);
                status = MethodCallAsync(*getProperty,
                                         this,
                                         static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::GetPropMethodCB),
                                         inArgs,
                                         numArgs,
                                         reinterpret_cast<void*>(ctx),
                                         timeout,
                                         flags);
                if (status != ER_OK) {
                    delete ctx;
                    delete wrappedContext;
                }
            } else {
                CBContext<Listener::GetPropertyAsyncCB>* ctx = new CBContext<Listener::GetPropertyAsyncCB>(listener, callbackWithCustomError, wrappedContext);
                status = MethodCallAsync(*getProperty,
                                         this,
                                         static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::GetPropMethodAsyncCB),
                                         inArgs,
                                         numArgs,
                                         reinterpret_cast<void*>(ctx),
                                         timeout,
                                         flags);
                if (status != ER_OK) {
                    delete ctx;
                    delete wrappedContext;
                }
            }
        }
    }
    return status;
}

QStatus ProxyBusObject::GetPropertyAsync(const char* iface,
                                         const char* property,
                                         ProxyBusObject::Listener* listener,
                                         ProxyBusObject::Listener::GetPropertyCB callback,
                                         void* context,
                                         uint32_t timeout)
{
    return GetPropertyAsyncCommon(iface, property, listener, callback, nullptr, context, timeout);
}

QStatus ProxyBusObject::GetPropertyAsync(const char* iface,
                                         const char* property,
                                         ProxyBusObject::Listener* listener,
                                         ProxyBusObject::Listener::GetPropertyAsyncCB callback,
                                         void* context,
                                         uint32_t timeout)
{
    return GetPropertyAsyncCommon(iface, property, listener, nullptr, callback, context, timeout);
}

QStatus ProxyBusObject::SetProperty(const char* iface, const char* property, MsgArg& value, uint32_t timeout) const
{
    String errorName;
    String errorDescription;
    return SetProperty(iface, property, value, errorName, errorDescription, timeout);
}

QStatus ProxyBusObject::SetProperty(const char* iface, const char* property, MsgArg& value, qcc::String& errorName, qcc::String& errorDescription, uint32_t timeout) const
{
    QStatus status;
    const InterfaceDescription* valueIface = internal->bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        uint8_t flags = 0;
        /*
         * If the object or the property interface is secure method call must be encrypted.
         */
        if (SecurityApplies(this, valueIface)) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        Message reply(*internal->bus);
        MsgArg inArgs[3];
        size_t numArgs = ArraySize(inArgs);

        MsgArg::Set(inArgs, numArgs, "ssv", iface, property, &value);
        const InterfaceDescription* propIface = internal->bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        if (propIface == NULL) {
            status = ER_BUS_NO_SUCH_INTERFACE;
        } else {
            const InterfaceDescription::Member* setProperty = propIface->GetMember("Set");
            QCC_ASSERT(setProperty);
            status = MethodCall(*setProperty,
                                inArgs,
                                numArgs,
                                reply,
                                timeout,
                                flags);
            if (ER_BUS_REPLY_IS_ERROR_MESSAGE == status) {
                GetReplyErrorStatusMessage(reply, status, errorName, errorDescription);
            }
        }
    }
    return status;
}

void ProxyBusObject::SetPropMethodCBCommon(Message& message, void* context, bool isCustomErrorSet)
{
    QStatus status = ER_OK;
    CBContext<Listener::SetPropertyAsyncCB>* ctxWithCustomError = nullptr;
    CBContext<Listener::SetPropertyCB>* ctx = nullptr;
    if (isCustomErrorSet) {
        ctxWithCustomError = reinterpret_cast<CBContext<Listener::SetPropertyAsyncCB>*>(context);
    } else {
        ctx = reinterpret_cast<CBContext<Listener::SetPropertyCB>*>(context);
    }
    QCC_ASSERT((ctx != nullptr) || (ctxWithCustomError != nullptr));

    if (message->GetType() != MESSAGE_METHOD_RET) {
        status = ER_BUS_NO_SUCH_PROPERTY;
        if (message->GetErrorName() != NULL) {
            const char* err;
            uint16_t rawStatus;
            if (message->GetArgs("sq", &err, &rawStatus) == ER_OK) {
                status = static_cast<QStatus>(rawStatus);
                QCC_DbgPrintf(("Asynch SetProperty call returned %s", err));
            }
        }
    }

    if (isCustomErrorSet) {
        String errorDescription;
        String errorName = message->GetErrorName(&errorDescription);
        (ctxWithCustomError->listener->*ctxWithCustomError->callback)(status, this, errorName, errorDescription, ctxWithCustomError->context);
        delete ctxWithCustomError;
    } else {
        (ctx->listener->*ctx->callback)(status, this, ctx->context);
        delete ctx;
    }
}

void ProxyBusObject::SetPropMethodCB(Message& message, void* context)
{
    SetPropMethodCBCommon(message, context, false);
}

void ProxyBusObject::SetPropMethodAsyncCB(Message& message, void* context)
{
    SetPropMethodCBCommon(message, context, true);
}

QStatus ProxyBusObject::RegisterPropertiesChangedListener(const char* iface,
                                                          const char** properties,
                                                          size_t propertiesSize,
                                                          ProxyBusObject::PropertiesChangedListener& listener,
                                                          void* context)
{
    QCC_DbgTrace(("ProxyBusObject::RegisterPropertiesChangedListener(this = %p, iface = %s, properties = %p, propertiesSize = %u, listener = %p, context = %p",
                  this, iface, properties, propertiesSize, &listener, context));
    const InterfaceDescription* ifc = internal->bus->GetInterface(iface);
    if (!ifc) {
        return ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    }
    for (size_t i  = 0; i < propertiesSize; ++i) {
        if (!ifc->HasProperty(properties[i])) {
            return ER_BUS_NO_SUCH_PROPERTY;
        }
    }

    bool replace = false;
    String ifaceStr = iface;
    PropertiesChangedCB ctx(listener, properties, propertiesSize, context);
    pair<std::string, PropertiesChangedCB> cbItem(ifaceStr, ctx);
    internal->lock.Lock(MUTEX_CONTEXT);
    // remove old version first
    multimap<std::string, PropertiesChangedCB>::iterator it = internal->propertiesChangedCBs.lower_bound(iface);
    multimap<std::string, PropertiesChangedCB>::iterator end = internal->propertiesChangedCBs.upper_bound(iface);
    while (it != end) {
        PropertiesChangedCB propChangedCb = it->second;
        if (&propChangedCb->listener == &listener) {
            propChangedCb->isRegistered = false;
            internal->propertiesChangedCBs.erase(it);
            replace = true;
            break;
        }
        ++it;
    }
    internal->propertiesChangedCBs.insert(cbItem);
    internal->lock.Unlock(MUTEX_CONTEXT);

    QStatus status = ER_OK;
    if (!replace) {
        if (internal->uniqueName.empty()) {
            internal->uniqueName = internal->bus->GetNameOwner(internal->serviceName.c_str());
        }
        internal->AddPropertiesChangedRule(iface, true);
    }

    return status;
}

QStatus ProxyBusObject::UnregisterPropertiesChangedListener(const char* iface,
                                                            ProxyBusObject::PropertiesChangedListener& listener)
{
    QCC_DbgTrace(("ProxyBusObject::UnregisterPropertiesChangedListener(iface = %s, listener = %p)", iface, &listener));
    if (!internal->bus->GetInterface(iface)) {
        return ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    }

    String ifaceStr = iface;
    bool removed = false;

    internal->lock.Lock(MUTEX_CONTEXT);
    map<qcc::Thread*, _PropertiesChangedCB*>::iterator thread = internal->handlerThreads.find(Thread::GetThread());
    if (thread != internal->handlerThreads.end() && (&thread->second->listener == &listener)) {
        QCC_LogError(ER_DEADLOCK, ("Attempt to unregister listener from said listener would cause deadlock"));
        internal->lock.Unlock(MUTEX_CONTEXT);
        return ER_DEADLOCK;
    }

    multimap<std::string, PropertiesChangedCB>::iterator it = internal->propertiesChangedCBs.lower_bound(iface);
    multimap<std::string, PropertiesChangedCB>::iterator end = internal->propertiesChangedCBs.upper_bound(iface);
    while (it != end) {
        PropertiesChangedCB ctx = it->second;
        if (&ctx->listener == &listener) {
            ctx->isRegistered = false;
            internal->propertiesChangedCBs.erase(it);
            removed = true;

            while (ctx->numRunning > 0) {
                /*
                 * Some thread is trying to remove listeners while the listeners are
                 * being called.  Wait until the listener callbacks are done first.
                 */
                internal->listenerDone.Wait(internal->lock);
            }

            break;
        }
        ++it;
    }
    internal->lock.Unlock(MUTEX_CONTEXT);

    QStatus status = ER_OK;
    if (removed) {
        internal->RemovePropertiesChangedRule(iface);
    }

    return status;
}

void ProxyBusObject::Internal::PropertiesChangedHandler(const InterfaceDescription::Member* member, const char* srcPath, Message& message)
{
    QCC_UNUSED(member);
    QCC_UNUSED(srcPath);

    QCC_DbgTrace(("Internal::PropertiesChangedHandler(this = %p, member = %s, srcPath = %s, message = <>)", this, member->name.c_str(), srcPath));

    const char* ifaceName;
    MsgArg* changedProps;
    size_t numChangedProps;
    MsgArg* invalidProps;
    size_t numInvalidProps;

    if (uniqueName != message->GetSender()) {
        /*
         * Message may not be for us if different senders but same object paths:
         * RegisterSignalHandler is bound to the path, not the sender.
         */
        return;
    }

    QStatus status = message->GetArgs("sa{sv}as", &ifaceName, &numChangedProps, &changedProps, &numInvalidProps, &invalidProps);
    if (status != ER_OK) {
        QCC_LogError(status, ("invalid message args"));
        return;
    }

    lock.Lock(MUTEX_CONTEXT);
    /* first, update caches */
    if (cacheProperties) {
        map<std::string, CachedProps>::iterator it = caches.find(ifaceName);
        if (it != caches.end()) {
            it->second.PropertiesChanged(changedProps, numChangedProps, invalidProps, numInvalidProps, message->GetCallSerial());
        }
    }

    /* then, alert listeners */
    handlerThreads[Thread::GetThread()] = nullptr;
    multimap<std::string, PropertiesChangedCB>::iterator it = propertiesChangedCBs.lower_bound(ifaceName);
    multimap<std::string, PropertiesChangedCB>::iterator end = propertiesChangedCBs.upper_bound(ifaceName);
    list<PropertiesChangedCB> handlers;
    while (it != end) {
        if (it->second->isRegistered) {
            handlers.push_back(it->second);
        }
        ++it;
    }
    lock.Unlock(MUTEX_CONTEXT);

    size_t i;
    MsgArg changedOut;
    MsgArg* changedOutDict = (numChangedProps > 0) ? new MsgArg[numChangedProps] : NULL;
    size_t changedOutDictSize;
    MsgArg invalidOut;
    const char** invalidOutArray = (numInvalidProps > 0) ? new const char*[numInvalidProps] : NULL;
    size_t invalidOutArraySize;

    while (handlers.begin() != handlers.end()) {
        PropertiesChangedCB ctx = *handlers.begin();

        lock.Lock(MUTEX_CONTEXT);
        bool isRegistered = ctx->isRegistered;
        handlerThreads[Thread::GetThread()] = ctx.unwrap();
        ++ctx->numRunning;
        lock.Unlock(MUTEX_CONTEXT);

        if (isRegistered) {
            changedOutDictSize = 0;
            invalidOutArraySize = 0;

            if (ctx->properties.empty()) {
                // handler wants all changed/invalid properties in signal
                changedOut.Set("a{sv}", numChangedProps, changedProps);
                changedOutDictSize = numChangedProps;
                for (i = 0; i < numInvalidProps; ++i) {
                    const char* propName;
                    invalidProps[i].Get("s", &propName);
                    invalidOutArray[invalidOutArraySize++] = propName;

                }
                invalidOut.Set("as", numInvalidProps, invalidOutArray);
            } else {
                for (i = 0; i < numChangedProps; ++i) {
                    const char* propName;
                    MsgArg* propValue;
                    changedProps[i].Get("{sv}", &propName, &propValue);
                    if (ctx->properties.find(propName) != ctx->properties.end()) {
                        changedOutDict[changedOutDictSize++].Set("{sv}", propName, propValue);
                    }
                }
                if (changedOutDictSize > 0) {
                    changedOut.Set("a{sv}", changedOutDictSize, changedOutDict);
                } else {
                    changedOut.Set("a{sv}", 0, NULL);
                }

                for (i = 0; i < numInvalidProps; ++i) {
                    const char* propName;
                    invalidProps[i].Get("s", &propName);
                    if (ctx->properties.find(propName) != ctx->properties.end()) {
                        invalidOutArray[invalidOutArraySize++] = propName;
                    }
                }
                if (invalidOutArraySize > 0) {
                    invalidOut.Set("as", invalidOutArraySize, invalidOutArray);
                } else {
                    invalidOut.Set("as", 0, NULL);
                }
            }

            // only call listener if anything to report
            if ((changedOutDictSize > 0) || (invalidOutArraySize > 0)) {
                ProxyBusObject pbo(ManagedObj<ProxyBusObject::Internal>::wrap(this));
                ctx->listener.PropertiesChanged(pbo, ifaceName, changedOut, invalidOut, ctx->context);
            }
        }
        handlers.pop_front();

        lock.Lock(MUTEX_CONTEXT);
        --ctx->numRunning;
        handlerThreads[Thread::GetThread()] = nullptr;
        listenerDone.Broadcast();
        lock.Unlock(MUTEX_CONTEXT);
    }

    delete [] changedOutDict;
    delete [] invalidOutArray;

    lock.Lock(MUTEX_CONTEXT);
    handlerThreads.erase(Thread::GetThread());;
    handlerDone.Signal();
    lock.Unlock(MUTEX_CONTEXT);
}

QStatus ProxyBusObject::SetPropertyAsyncCommon(const char* iface,
                                               const char* property,
                                               MsgArg& value,
                                               ProxyBusObject::Listener* listener,
                                               ProxyBusObject::Listener::SetPropertyCB callback,
                                               ProxyBusObject::Listener::SetPropertyAsyncCB callbackWithCustomError,
                                               void* context,
                                               uint32_t timeout)
{
    QCC_ASSERT((callback != nullptr) || (callbackWithCustomError != nullptr));
    QStatus status;
    const InterfaceDescription* valueIface = internal->bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        uint8_t flags = 0;
        if (SecurityApplies(this, valueIface)) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        MsgArg inArgs[3];
        size_t numArgs = ArraySize(inArgs);
        MsgArg::Set(inArgs, numArgs, "ssv", iface, property, &value);
        const InterfaceDescription* propIface = internal->bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        if (propIface == NULL) {
            status = ER_BUS_NO_SUCH_INTERFACE;
        } else {
            const InterfaceDescription::Member* setProperty = propIface->GetMember("Set");
            QCC_ASSERT(setProperty);

            if (callback != nullptr) {
                CBContext<Listener::SetPropertyCB>* ctx = new CBContext<Listener::SetPropertyCB>(listener, callback, context);
                status = MethodCallAsync(*setProperty,
                                         this,
                                         static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::SetPropMethodCB),
                                         inArgs,
                                         numArgs,
                                         reinterpret_cast<void*>(ctx),
                                         timeout,
                                         flags);
                if (status != ER_OK) {
                    delete ctx;
                }
            } else {
                CBContext<Listener::SetPropertyAsyncCB>* ctx = new CBContext<Listener::SetPropertyAsyncCB>(listener, callbackWithCustomError, context);
                status = MethodCallAsync(*setProperty,
                                         this,
                                         static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::SetPropMethodAsyncCB),
                                         inArgs,
                                         numArgs,
                                         reinterpret_cast<void*>(ctx),
                                         timeout,
                                         flags);
                if (status != ER_OK) {
                    delete ctx;
                }
            }
        }
    }
    return status;
}

QStatus ProxyBusObject::SetPropertyAsync(const char* iface,
                                         const char* property,
                                         MsgArg& value,
                                         ProxyBusObject::Listener* listener,
                                         ProxyBusObject::Listener::SetPropertyCB callback,
                                         void* context,
                                         uint32_t timeout)
{
    return SetPropertyAsyncCommon(iface, property, value, listener, callback, nullptr, context, timeout);
}

QStatus ProxyBusObject::SetPropertyAsync(const char* iface,
                                         const char* property,
                                         MsgArg& value,
                                         ProxyBusObject::Listener* listener,
                                         ProxyBusObject::Listener::SetPropertyAsyncCB callback,
                                         void* context,
                                         uint32_t timeout)
{
    return SetPropertyAsyncCommon(iface, property, value, listener, nullptr, callback, context, timeout);
}

size_t ProxyBusObject::GetInterfaces(const InterfaceDescription** ifaces, size_t numIfaces) const
{
    internal->lock.Lock(MUTEX_CONTEXT);
    size_t count = internal->ifaces.size();
    if (ifaces) {
        count = min(count, numIfaces);
        map<std::string, const InterfaceDescription*>::const_iterator it = internal->ifaces.begin();
        for (size_t i = 0; i < count && it != internal->ifaces.end(); ++i, ++it) {
            ifaces[i] = it->second;
        }
    }
    internal->lock.Unlock(MUTEX_CONTEXT);
    return count;
}

const InterfaceDescription* ProxyBusObject::GetInterface(const char* ifaceName) const
{
    std::string key = ifaceName;
    internal->lock.Lock(MUTEX_CONTEXT);
    map<std::string, const InterfaceDescription*>::const_iterator it = internal->ifaces.find(key);
    const InterfaceDescription* ret = (it == internal->ifaces.end()) ? NULL : it->second;
    internal->lock.Unlock(MUTEX_CONTEXT);
    return ret;
}


QStatus ProxyBusObject::AddInterface(const InterfaceDescription& iface) {
    std::string key(qcc::String(iface.GetName()));
    pair<std::string, const InterfaceDescription*> item(key, &iface);
    bool addRule = false;

    internal->lock.Lock(MUTEX_CONTEXT);
    pair<map<std::string, const InterfaceDescription*>::const_iterator, bool> ret = internal->ifaces.insert(item);
    QStatus status = ret.second ? ER_OK : ER_BUS_IFACE_ALREADY_EXISTS;

    if ((status == ER_OK) && internal->cacheProperties && iface.HasCacheableProperties()) {
        internal->caches.insert(std::make_pair(key, CachedProps(&iface)));
        addRule = true;
    }

    if ((status == ER_OK) && !internal->hasProperties) {
        const InterfaceDescription* propIntf = internal->bus->GetInterface(::ajn::org::freedesktop::DBus::Properties::InterfaceName);
        QCC_ASSERT(propIntf);
        if (iface == *propIntf) {
            internal->hasProperties = true;
        } else if (iface.GetProperties() > 0) {
            AddInterface(*propIntf);
        }
    }
    internal->lock.Unlock(MUTEX_CONTEXT);

    if (addRule) {
        /* add match rules in case the PropertiesChanged signals are emitted as global broadcast */
        internal->AddPropertiesChangedRule(iface.GetName(), false);
    }

    return status;
}


QStatus ProxyBusObject::AddInterface(const char* ifaceName)
{
    const InterfaceDescription* iface = internal->bus->GetInterface(ifaceName);
    if (!iface) {
        return ER_BUS_NO_SUCH_INTERFACE;
    } else {
        return AddInterface(*iface);
    }
}

void ProxyBusObject::Internal::AddMatchCB(QStatus status, void* context)
{
    QCC_UNUSED(status);

    AddMatchCBInfo* info = reinterpret_cast<AddMatchCBInfo*>(context);
    lock.Lock(MUTEX_CONTEXT);
    info->addingRef = false;
    /* enable property caches */
    map<std::string, CachedProps>::iterator it = caches.find(info->ifaceName);
    if (it != caches.end()) {
        it->second.Enable();
    }
    addMatchDone.Broadcast();
    lock.Unlock(MUTEX_CONTEXT);
    delete info;
}

void ProxyBusObject::Internal::AddPropertiesChangedRule(const char* intf, bool blocking)
{
    QCC_DbgTrace(("%s(%s)", __FUNCTION__, intf));

    bool registerHandler = false;
    bool callAddMatch = false;

    lock.Lock(MUTEX_CONTEXT);
    MatchRuleBookKeeping::iterator it = matchRuleBookKeeping.find(intf);
    if (it == matchRuleBookKeeping.end()) {
        callAddMatch = true;
        pair<MatchRuleBookKeeping::iterator, bool> r;
        /*
         * Setup placeholder.  Other threads that call this function with the
         * same interface after us will block until our AddMatch call completes.
         */
        r = matchRuleBookKeeping.insert(pair<std::string, MatchRuleInfo>(qcc::String(intf), true));
        it = r.first;
    }

    QCC_ASSERT(it != matchRuleBookKeeping.end());
    ++it->second.refCount;

    if (!registeredPropChangedHandler) {
        registerHandler = true;
        registeredPropChangedHandler = true;
    }
    lock.Unlock(MUTEX_CONTEXT);

    if (registerHandler) {
        QCC_DbgPrintf(("Registering signal handler"));
        const InterfaceDescription* propIntf = bus->GetInterface(::ajn::org::freedesktop::DBus::Properties::InterfaceName);
        QCC_ASSERT(propIntf);
        bus->RegisterSignalHandler(this,
                                   static_cast<MessageReceiver::SignalHandler>(&Internal::PropertiesChangedHandler),
                                   propIntf->GetMember("PropertiesChanged"),
                                   path.c_str());
    }

    if (callAddMatch) {
        String rule = String("type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='") + intf + "'";
        AddMatchCBInfo* cbInfo = new AddMatchCBInfo(intf, it->second.adding);
        QStatus status = bus->AddMatchAsync(rule.c_str(), this, cbInfo);
        if (status != ER_OK) {
            --it->second.refCount;
            delete cbInfo;
            return;
        }
    }

    lock.Lock(MUTEX_CONTEXT);

    /* If we already have a match rule installed for this interface, enable the property
     * cache. This is an idempotent operation, so we don't care if we did this before.
     * If we don't have the match rule yet, PBO::Internal::AddMatchCB will enable the
     * cache for us. */
    if (!it->second.adding) {
        map<std::string, CachedProps>::iterator cit = caches.find(intf);
        if (cit != caches.end()) {
            cit->second.Enable();
        }
    }

    if (blocking) {
        while (it->second.adding) {
            addMatchDone.Wait(lock);
        }
    }
    lock.Unlock(MUTEX_CONTEXT);
}

void ProxyBusObject::Internal::RemovePropertiesChangedRule(const char* intf)
{
    lock.Lock(MUTEX_CONTEXT);
    MatchRuleBookKeeping::iterator it = matchRuleBookKeeping.find(intf);
    if (it != matchRuleBookKeeping.end()) {
        /*
         * Check if there is a callback pending that will access this iterator.
         * If so, need to wait for that callback to complete before removing it.
         */
        while (it->second.adding) {
            addMatchDone.Wait(lock);
        }
        if (--it->second.refCount == 0) {
            String rule = String("type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='") + intf + "'";
            bus->RemoveMatchNonBlocking(rule.c_str());
            matchRuleBookKeeping.erase(it);
        }
    }
    lock.Unlock(MUTEX_CONTEXT);
}

void ProxyBusObject::Internal::RemoveAllPropertiesChangedRules()
{
    lock.Lock(MUTEX_CONTEXT);
    MatchRuleBookKeeping::iterator it = matchRuleBookKeeping.begin();
    for (; it != matchRuleBookKeeping.end(); ++it) {
        /*
         * Check if there is a callback pending that will access the same data
         * referenced by this iterator.  If so, need to wait for that callback
         * to complete before removing it.
         */
        while (it->second.adding) {
            addMatchDone.Wait(lock);
        }
        String rule = String("type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='") + it->first.c_str() + "'";
        bus->RemoveMatchNonBlocking(rule.c_str());
    }
    matchRuleBookKeeping.clear();
    lock.Unlock(MUTEX_CONTEXT);
}

bool ProxyBusObject::IsValid() const {
    return internal->bus != nullptr;
}

bool ProxyBusObject::IsSecure() const {
    return internal->isSecure;
}

void ProxyBusObject::EnablePropertyCaching()
{
    vector<String> ifcNames;
    internal->lock.Lock(MUTEX_CONTEXT);
    ifcNames.reserve(internal->ifaces.size());
    if (!internal->cacheProperties) {
        internal->cacheProperties = true;
        map<std::string, const InterfaceDescription*>::const_iterator it = internal->ifaces.begin();
        for (; it != internal->ifaces.end(); ++it) {
            if (it->second->HasCacheableProperties()) {
                internal->caches.insert(std::make_pair(qcc::String(it->first.c_str()), CachedProps(it->second)));
                ifcNames.push_back(it->first.c_str());
            }
        }
    }
    internal->lock.Unlock(MUTEX_CONTEXT);
    for (vector<String>::const_iterator it = ifcNames.begin(); it != ifcNames.end(); ++it) {
        /* add match rules in case the PropertiesChanged signals are emitted as global broadcast */
        internal->AddPropertiesChangedRule(it->c_str(), false);
    }
}

size_t ProxyBusObject::GetChildren(ProxyBusObject** children, size_t numChildren)
{
    internal->lock.Lock(MUTEX_CONTEXT);
    size_t count = internal->children.size();
    if (children) {
        count = min(count, numChildren);
        for (size_t i = 0; i < count; i++) {
            children[i] = &(internal->children[i]);
        }
    }
    internal->lock.Unlock(MUTEX_CONTEXT);
    return count;
}

size_t ProxyBusObject::GetManagedChildren(void* children, size_t numChildren)
{
    /*
     * Use ManagedObj<ProxyBusObject> rather than _ProxyBusObject to avoid
     * deprecation warnings/errors.
     */
    ManagedObj<ProxyBusObject>** pboChildren = reinterpret_cast<ManagedObj<ProxyBusObject>**>(children);
    internal->lock.Lock(MUTEX_CONTEXT);
    size_t count = internal->children.size();
    if (pboChildren) {
        count = min(count, numChildren);
        for (size_t i = 0; i < count; i++) {
            pboChildren[i] = new ManagedObj<ProxyBusObject>((internal->children)[i]);
        }
    }
    internal->lock.Unlock(MUTEX_CONTEXT);
    return count;
}

ProxyBusObject* ProxyBusObject::GetChild(const char* inPath)
{
    /* Add a trailing slash to this path */
    qcc::String pathSlash = (internal->path == "/") ? internal->path : internal->path + "/";

    /* Create absolute version of inPath */
    qcc::String inPathStr = ('/' == inPath[0]) ? inPath : pathSlash + inPath;

    /* Sanity check to make sure path is possible */
    if ((0 != inPathStr.find(pathSlash)) || (inPathStr[inPathStr.length() - 1] == '/')) {
        return NULL;
    }

    /* Find each path element as a child within the parent's vector of children */
    size_t idx = internal->path.size() + 1;
    ProxyBusObject* cur = this;
    internal->lock.Lock(MUTEX_CONTEXT);
    while (idx != qcc::String::npos) {
        size_t end = inPathStr.find_first_of('/', idx);
        qcc::String item = inPathStr.substr(0, end);
        vector<ProxyBusObject>& ch = cur->internal->children;
        vector<ProxyBusObject>::iterator it = ch.begin();
        while (it != ch.end()) {
            if (it->GetPath() == item) {
                cur = &(*it);
                break;
            }
            ++it;
        }
        if (it == ch.end()) {
            internal->lock.Unlock(MUTEX_CONTEXT);
            return NULL;
        }
        idx = ((qcc::String::npos == end) || ((end + 1) == inPathStr.size())) ? qcc::String::npos : end + 1;
    }
    internal->lock.Unlock(MUTEX_CONTEXT);
    return cur;
}

void* ProxyBusObject::GetManagedChild(const char* inPath)
{
    ProxyBusObject* ch = GetChild(inPath);
    if (ch) {
        return new ManagedObj<ProxyBusObject>(*ch);
    }
    return nullptr;
}

QStatus ProxyBusObject::AddChild(const ProxyBusObject& child)
{
    qcc::String childPath = child.GetPath();

    /* Sanity check to make sure path is possible */
    if (((internal->path.size() > 1) && (0 != childPath.find(internal->path + "/"))) ||
        ((internal->path.size() == 1) && (childPath[0] != '/')) ||
        (childPath[childPath.length() - 1] == '/')) {
        return ER_BUS_BAD_CHILD_PATH;
    }

    /* Find each path element as a child within the parent's vector of children */
    /* Add new children as necessary */
    size_t idx = internal->path.size() + 1;
    ProxyBusObject* cur = this;
    internal->lock.Lock(MUTEX_CONTEXT);
    while (idx != qcc::String::npos) {
        size_t end = childPath.find_first_of('/', idx);
        qcc::String item = childPath.substr(0, end);
        vector<ProxyBusObject>& ch = cur->internal->children;
        vector<ProxyBusObject>::iterator it = ch.begin();
        while (it != ch.end()) {
            if (it->GetPath() == item) {
                cur = &(*it);
                break;
            }
            ++it;
        }
        if (it == ch.end()) {
            if (childPath == item) {
                ch.push_back(child);
                internal->lock.Unlock(MUTEX_CONTEXT);
                return ER_OK;
            } else {
                const char* tempServiceName = internal->serviceName.c_str();
                const char* tempPath = item.c_str();
                const char* tempUniqueName = internal->uniqueName.c_str();
                ProxyBusObject ro(*internal->bus, tempServiceName, tempUniqueName, tempPath, internal->sessionId);
                ch.push_back(ro);
                cur = &ro;
            }
        }
        idx = ((qcc::String::npos == end) || ((end + 1) == childPath.size())) ? qcc::String::npos : end + 1;
    }
    internal->lock.Unlock(MUTEX_CONTEXT);
    return ER_BUS_OBJ_ALREADY_EXISTS;
}

QStatus ProxyBusObject::RemoveChild(const char* inPath)
{
    QStatus status;

    /* Add a trailing slash to this path */
    qcc::String pathSlash = (internal->path == "/") ? internal->path : internal->path + "/";

    /* Create absolute version of inPath */
    qcc::String childPath = ('/' == inPath[0]) ? inPath : pathSlash + inPath;

    /* Sanity check to make sure path is possible */
    if ((0 != childPath.find(pathSlash)) || (childPath[childPath.length() - 1] == '/')) {
        return ER_BUS_BAD_CHILD_PATH;
    }

    /* Navigate to child and remove it */
    size_t idx = internal->path.size() + 1;
    ProxyBusObject* cur = this;
    internal->lock.Lock(MUTEX_CONTEXT);
    while (idx != qcc::String::npos) {
        size_t end = childPath.find_first_of('/', idx);
        qcc::String item = childPath.substr(0, end);
        vector<ProxyBusObject>& ch = cur->internal->children;
        vector<ProxyBusObject>::iterator it = ch.begin();
        while (it != ch.end()) {
            if (it->GetPath() == item) {
                if (end == qcc::String::npos) {
                    ch.erase(it);
                    internal->lock.Unlock(MUTEX_CONTEXT);
                    return ER_OK;
                } else {
                    cur = &(*it);
                    break;
                }
            }
            ++it;
        }
        if (it == ch.end()) {
            status = ER_BUS_OBJ_NOT_FOUND;
            internal->lock.Unlock(MUTEX_CONTEXT);
            QCC_LogError(status, ("Cannot find object path %s", item.c_str()));
            return status;
        }
        idx = ((qcc::String::npos == end) || ((end + 1) == childPath.size())) ? qcc::String::npos : end + 1;
    }
    /* Shouldn't get here */
    internal->lock.Unlock(MUTEX_CONTEXT);
    return ER_FAIL;
}



QStatus ProxyBusObject::MethodCallAsync(const InterfaceDescription::Member& method,
                                        MessageReceiver* receiver,
                                        MessageReceiver::ReplyHandler replyHandler,
                                        const MsgArg* args,
                                        size_t numArgs,
                                        void* context,
                                        uint32_t timeout,
                                        uint8_t flags) const
{

    QStatus status;
    Message msg(*internal->bus);
    LocalEndpoint localEndpoint = internal->bus->GetInternal().GetLocalEndpoint();
    if (!localEndpoint->IsValid()) {
        return ER_BUS_ENDPOINT_CLOSING;
    }
    /*
     * This object must implement the interface for this method
     */
    if (!ImplementsInterface(method.iface->GetName())) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
        QCC_LogError(status, ("Object %s does not implement %s", internal->path.c_str(), method.iface->GetName()));
        return status;
    }
    if (!replyHandler) {
        flags |= ALLJOYN_FLAG_NO_REPLY_EXPECTED;
        QCC_ASSERT(context == nullptr);
    }
    /*
     * If the interface is secure or encryption is explicitly requested the method call must be encrypted.
     */
    if (SecurityApplies(this, method.iface)) {
        flags |= ALLJOYN_FLAG_ENCRYPTED;
    }
    if ((flags & ALLJOYN_FLAG_ENCRYPTED) && !internal->bus->IsPeerSecurityEnabled()) {
        return ER_BUS_SECURITY_NOT_ENABLED;
    }
    status = msg->CallMsg(method.signature, internal->serviceName, internal->sessionId, internal->path, method.iface->GetName(), method.name, args, numArgs, flags);
    if (status == ER_OK) {
        if (!(flags & ALLJOYN_FLAG_NO_REPLY_EXPECTED)) {
            status = localEndpoint->RegisterReplyHandler(receiver, replyHandler, method, msg, context, timeout);
        }
        if (status == ER_OK) {
            if (internal->b2bEp->IsValid()) {
                status = internal->b2bEp->PushMessage(msg);
            } else {
                BusEndpoint busEndpoint = BusEndpoint::cast(localEndpoint);
                status = internal->bus->GetInternal().GetRouter().PushMessage(msg, busEndpoint);
            }
            if (status != ER_OK) {
                bool unregistered = localEndpoint->UnregisterReplyHandler(msg);
                if (!unregistered) {
                    /*
                     * Unregister failed, so the reply handler must have already been called.
                     *
                     * The contract of this function is that the reply handler will be called iff
                     * the status is ER_OK, so set the status to ER_OK to indicate that the reply
                     * handler was called.
                     */
                    status = ER_OK;
                }
            }
        }
    }
    return status;
}

QStatus ProxyBusObject::MethodCallAsync(const char* ifaceName,
                                        const char* methodName,
                                        MessageReceiver* receiver,
                                        MessageReceiver::ReplyHandler replyHandler,
                                        const MsgArg* args,
                                        size_t numArgs,
                                        void* context,
                                        uint32_t timeout,
                                        uint8_t flags) const
{
    internal->lock.Lock(MUTEX_CONTEXT);
    map<std::string, const InterfaceDescription*>::const_iterator it = internal->ifaces.find(std::string(ifaceName));
    if (it == internal->ifaces.end()) {
        internal->lock.Unlock(MUTEX_CONTEXT);
        return ER_BUS_NO_SUCH_INTERFACE;
    }
    const InterfaceDescription::Member* member = it->second->GetMember(methodName);
    internal->lock.Unlock(MUTEX_CONTEXT);
    if (NULL == member) {
        return ER_BUS_INTERFACE_NO_SUCH_MEMBER;
    }
    return MethodCallAsync(*member, receiver, replyHandler, args, numArgs, context, timeout, flags);
}

QStatus ProxyBusObject::MethodCall(const InterfaceDescription::Member& method,
                                   const MsgArg* args,
                                   size_t numArgs,
                                   Message& replyMsg,
                                   uint32_t timeout,
                                   uint8_t flags,
                                   Message* callMsg) const
{
    QStatus status;
    Message msg(*internal->bus);
    LocalEndpoint localEndpoint = internal->bus->GetInternal().GetLocalEndpoint();
    if (!localEndpoint->IsValid()) {
        return ER_BUS_ENDPOINT_CLOSING;
    }
    /*
     * if we're being called from the LocalEndpoint (callback) thread, do not allow
     * blocking calls unless BusAttachment::EnableConcurrentCallbacks has been called first
     */
    bool isDaemon = internal->bus->GetInternal().GetRouter().IsDaemon();
    if (localEndpoint->IsReentrantCall() && !isDaemon) {
        status = ER_BUS_BLOCKING_CALL_NOT_ALLOWED;
        goto MethodCallExit;
    }
    /*
     * This object must implement the interface for this method
     */
    if (!ImplementsInterface(method.iface->GetName())) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
        QCC_LogError(status, ("Object %s does not implement %s", internal->path.c_str(), method.iface->GetName()));
        goto MethodCallExit;
    }
    /*
     * If the object or interface is secure or encryption is explicitly requested the method call must be encrypted.
     */
    if (SecurityApplies(this, method.iface)) {
        flags |= ALLJOYN_FLAG_ENCRYPTED;
    }
    if ((flags & ALLJOYN_FLAG_ENCRYPTED) && !internal->bus->IsPeerSecurityEnabled()) {
        status = ER_BUS_SECURITY_NOT_ENABLED;
        goto MethodCallExit;
    }
    status = msg->CallMsg(method.signature, internal->serviceName, internal->sessionId, internal->path, method.iface->GetName(), method.name, args, numArgs, flags);
    if (status != ER_OK) {
        goto MethodCallExit;
    }
    /*
     * If caller asked for a copy of the sent message, copy it now that we've successfully created it.
     */
    if (NULL != callMsg) {
        *callMsg = msg;
    }
    /*
     * See if we need to send any manifests in advance of this message.
     */
    status = internal->bus->GetInternal().GetPermissionManager().GetPermissionMgmtObj()->SendManifests(this, &msg);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to SendManifests"));
        goto MethodCallExit;
    }

    if (flags & ALLJOYN_FLAG_NO_REPLY_EXPECTED) {
        /*
         * Push the message to the router and we are done
         */
        if (internal->b2bEp->IsValid()) {
            status = internal->b2bEp->PushMessage(msg);
        } else {
            BusEndpoint busEndpoint = BusEndpoint::cast(localEndpoint);
            status = internal->bus->GetInternal().GetRouter().PushMessage(msg, busEndpoint);
        }
    } else {
        SyncReplyContext ctxt(*internal->bus);
        /*
         * Synchronous calls are really asynchronous calls that block waiting for a builtin
         * reply handler to be called.
         */
        SyncReplyContext* heapCtx = new SyncReplyContext(ctxt);
        status = localEndpoint->RegisterReplyHandler(const_cast<MessageReceiver*>(static_cast<const MessageReceiver* const>(this)),
                                                     static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::SyncReplyHandler),
                                                     method,
                                                     msg,
                                                     heapCtx,
                                                     timeout);
        if (status != ER_OK) {
            delete heapCtx;
            heapCtx = NULL;
            goto MethodCallExit;
        }

        if (internal->b2bEp->IsValid()) {
            status = internal->b2bEp->PushMessage(msg);
        } else {
            BusEndpoint busEndpoint = BusEndpoint::cast(localEndpoint);
            status = internal->bus->GetInternal().GetRouter().PushMessage(msg, busEndpoint);
        }

        Thread* thisThread = Thread::GetThread();
        if (status == ER_OK) {
            internal->lock.Lock(MUTEX_CONTEXT);
            if (!isExiting) {
                internal->syncMethodCalls[this].insert(ctxt);
                internal->lock.Unlock(MUTEX_CONTEXT);
                /*
                 * In case of a timeout, the SyncReplyHandler will be called by
                 * the LocalEndpoint replyTimer. So wait forever to be signaled
                 * by the SyncReplyHandler or the ProxyBusObject destructor (in
                 * case the ProxyBusObject is being destroyed) or this thread is
                 * stopped.
                 */
                status = Event::Wait(ctxt->event);
                internal->lock.Lock(MUTEX_CONTEXT);

                internal->syncMethodCalls[this].erase(ctxt);
                internal->syncMethodComplete.Broadcast();
            } else {
                status = ER_BUS_STOPPING;
            }
            internal->lock.Unlock(MUTEX_CONTEXT);
        }

        if (status == ER_OK) {
            replyMsg = ctxt->replyMsg;
        } else if ((status == ER_ALERTED_THREAD) && (SYNC_METHOD_ALERTCODE_ABORT == thisThread->GetAlertCode())) {
            thisThread->ResetAlertCode();
            /*
             * We can't touch anything in this case since the external thread that was waiting
             * can't know whether this object still exists.
             */
            status = ER_BUS_METHOD_CALL_ABORTED;
        } else if (localEndpoint->UnregisterReplyHandler(msg)) {
            /*
             * The handler was deregistered so we need to delete the context here.
             */
            delete heapCtx;
        }
        if (status == ER_ALERTED_THREAD) {
            thisThread->ResetAlertCode();
        }
    }

MethodCallExit:
    /*
     * Let caller know that the method call reply was an error message
     */
    if (status == ER_OK) {
        if (replyMsg->GetType() == MESSAGE_ERROR) {
            status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
        } else if (replyMsg->GetType() == MESSAGE_INVALID && !(flags & ALLJOYN_FLAG_NO_REPLY_EXPECTED)) {
            status = ER_FAIL;
        }
    } else {
        /*
         * We should not need to duplicate the status information into a synthesized
         * replyMessage.  However 14.12 and prior behaved this way, so preserve the
         * existing behavior.
         */
        String sender;
        if (internal->bus->IsStarted()) {
            sender = internal->bus->GetInternal().GetLocalEndpoint()->GetUniqueName();
        }

        replyMsg->ErrorMsg(sender, status, 0);
    }

    if ((status == ER_OK) && internal->uniqueName.empty()) {
        internal->uniqueName = replyMsg->GetSender();
    }
    if (ER_OK != status) {
        AdjustErrorForPermissionDenied(replyMsg, status);
    }
    return status;
}

QStatus ProxyBusObject::MethodCall(const char* ifaceName,
                                   const char* methodName,
                                   const MsgArg* args,
                                   size_t numArgs,
                                   Message& replyMsg,
                                   uint32_t timeout,
                                   uint8_t flags) const
{
    internal->lock.Lock(MUTEX_CONTEXT);
    map<std::string, const InterfaceDescription*>::const_iterator it = internal->ifaces.find(std::string(ifaceName));
    if (it == internal->ifaces.end()) {
        internal->lock.Unlock(MUTEX_CONTEXT);
        return ER_BUS_NO_SUCH_INTERFACE;
    }
    const InterfaceDescription::Member* member = it->second->GetMember(methodName);
    internal->lock.Unlock(MUTEX_CONTEXT);
    if (NULL == member) {
        return ER_BUS_INTERFACE_NO_SUCH_MEMBER;
    }
    return MethodCall(*member, args, numArgs, replyMsg, timeout, flags);
}

void ProxyBusObject::SetSecure(bool isSecure) {
    internal->isSecure = isSecure;
}

void ProxyBusObject::SyncReplyHandler(Message& msg, void* context)
{
    if (context != NULL) {
        SyncReplyContext* ctx = reinterpret_cast<SyncReplyContext*> (context);

        /* Set the reply message */
        (*ctx)->replyMsg = msg;

        /* Wake up sync method_call thread */
        QStatus status = (*ctx)->event.SetEvent();
        if (ER_OK != status) {
            QCC_LogError(status, ("SetEvent failed"));
        }
        delete ctx;
    }
}

QStatus ProxyBusObject::SecureConnection(bool forceAuth)
{
    return internal->bus->SecureConnection(internal->serviceName.c_str(), forceAuth);
}

QStatus ProxyBusObject::SecureConnectionAsync(bool forceAuth)
{
    return internal->bus->SecureConnectionAsync(internal->serviceName.c_str(), forceAuth);
}

const qcc::String& ProxyBusObject::GetPath(void) const {
    return internal->path;
}

const qcc::String& ProxyBusObject::GetServiceName(void) const {
    return internal->serviceName;
}

const qcc::String& ProxyBusObject::GetUniqueName(void) const {
    return internal->uniqueName;
}

SessionId ProxyBusObject::GetSessionId(void) const {
    return internal->sessionId;
}

QStatus ProxyBusObject::IntrospectRemoteObject(uint32_t timeout)
{
    /* Need to have introspectable interface in order to call Introspect */
    const InterfaceDescription* introIntf = GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
    if (!introIntf) {
        introIntf = internal->bus->GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
        QCC_ASSERT(introIntf);
        AddInterface(*introIntf);
    }

    /* Attempt to retrieve introspection from the remote object using sync call */
    Message reply(*internal->bus);
    const InterfaceDescription::Member* introMember = introIntf->GetMember("Introspect");
    QCC_ASSERT(introMember);
    QStatus status = MethodCall(*introMember, NULL, 0, reply, timeout);

    /* Parse the XML reply */
    if (ER_OK == status) {
        QCC_DbgPrintf(("Introspection XML: %s\n", reply->GetArg(0)->v_string.str));
        qcc::String ident = reply->GetSender();
        if (internal->uniqueName.empty()) {
            internal->uniqueName = ident;
        }
        ident += " : ";
        ident += reply->GetObjectPath();

        const char* introspectionXml = reply->GetArg(0)->v_string.str;
        if (legacyIntrospectionHandler->IsLegacyXml(introspectionXml)) {
            /* Introspect() output from a legacy node will not contain descriptions.
             * If we are dealing with an object which supports descriptions
             * (older legacy objects only support the org.freedesktop.DBus.Introspectable
             * introspection which does not define descriptions), we need to retrieve them
             * by calling IntrospectWithDescription() for this node.
             * See also documentation for ParseLegacyXml().
             */
            if (legacyIntrospectionHandler->RemoteObjectSupportsAllSeenIntrospection(introspectionXml)) {
                /* Our object does support descriptions. We will try to fetch
                 * them by calling IntrospectWithDescription().
                 */
                status = legacyIntrospectionHandler->ParseLegacyXml(introspectionXml, ident.c_str());
            } else {
                /* Our object does not support descriptions.
                 * No need for additional requests or processing.
                 */
                status = ParseXml(introspectionXml, ident.c_str());
            }
        } else {
            /* Introspect() called on a 16.04+ node will contain descriptions.
             * No need for additional requests or processing.
             */
            status = ParseXml(introspectionXml, ident.c_str());
        }
    }
    return status;
}

QStatus ProxyBusObject::IntrospectRemoteObjectAsync(ProxyBusObject::Listener* listener,
                                                    ProxyBusObject::Listener::IntrospectCB callback,
                                                    void* context,
                                                    uint32_t timeout)
{
    /* Need to have introspectable interface in order to call Introspect */
    const InterfaceDescription* introIntf = GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
    if (!introIntf) {
        introIntf = internal->bus->GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
        QCC_ASSERT(introIntf);
        AddInterface(*introIntf);
    }

    /* Attempt to retrieve introspection from the remote object using async call */
    const InterfaceDescription::Member* introMember = introIntf->GetMember("Introspect");
    QCC_ASSERT(introMember);
    CBContext<Listener::IntrospectCB>* ctx = new CBContext<Listener::IntrospectCB>(listener, callback, context);
    QStatus status = MethodCallAsync(*introMember,
                                     this,
                                     static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::IntrospectMethodCB),
                                     NULL,
                                     0,
                                     reinterpret_cast<void*>(ctx),
                                     timeout);
    if (ER_OK != status) {
        delete ctx;
    }
    return status;
}

void ProxyBusObject::IntrospectMethodCB(Message& msg, void* context)
{
    QStatus status;
    CBContext<Listener::IntrospectCB>* ctx = reinterpret_cast<CBContext<Listener::IntrospectCB>*>(context);

    if (msg->GetType() == MESSAGE_METHOD_RET) {
        /* Parse the XML reply to update this ProxyBusObject instance (plus any new interfaces) */
        char* xml;
        status = msg->GetArgs("s", &xml);
        if (ER_OK == status) {
            QCC_DbgPrintf(("Introspection XML: %s", xml));
            qcc::String ident = msg->GetSender();
            if (internal->uniqueName.empty()) {
                internal->uniqueName = ident;
            }
            ident += " : ";
            ident += msg->GetObjectPath();
            if (legacyIntrospectionHandler->IsLegacyXml(xml)) {
                /* Introspect() output from a legacy node will not contain descriptions.
                 * If we are dealing with an object which supports descriptions
                 * (older legacy objects only support the org.freedesktop.DBus.Introspectable
                 * introspection which does not define descriptions), we need to retrieve them
                 * by calling IntrospectWithDescription() for this node.
                 * See also documentation for ParseLegacyXml().
                 */
                if (legacyIntrospectionHandler->RemoteObjectSupportsAllSeenIntrospection(xml)) {
                    /* Our object does support descriptions. We will try to fetch
                     * them by calling IntrospectWithDescription().
                     */
                    status = legacyIntrospectionHandler->ParseLegacyXml(xml, ident.c_str());
                } else {
                    /* Our object does not support descriptions.
                     * No need for additional requests or processing.
                     */
                    status = ParseXml(xml, ident.c_str());
                }
            } else {
                /* Introspect() called on a 16.04+ node will contain descriptions.
                 * No need for additional requests or processing.
                 */
                status = ParseXml(xml, ident.c_str());
            }
        }
    } else if (msg->GetErrorName() != NULL && ::strcmp("org.freedesktop.DBus.Error.ServiceUnknown", msg->GetErrorName()) == 0) {
        status = ER_BUS_NO_SUCH_SERVICE;
    } else {
        status = ER_FAIL;
    }

    /* Call the callback */
    (ctx->listener->*ctx->callback)(status, this, ctx->context);
    delete ctx;
}

QStatus ProxyBusObject::ParseXml(const char* xml, const char* ident)
{
    StringSource source(xml);

    /* Parse the XML to update this ProxyBusObject instance (plus any new children and interfaces) */
    XmlParseContext pc(source);
    QStatus status = XmlElement::Parse(pc);
    if (status == ER_OK) {
        XmlHelper xmlHelper(internal->bus, ident ? ident : internal->path.c_str());
        status = xmlHelper.AddProxyObjects(*this, pc.GetRoot());
    }
    return status;
}

ProxyBusObject::~ProxyBusObject()
{
    /*
     * Need to wake up threads waiting on a synchronous method call since the
     * object it is calling into is being destroyed.  It's actually a pretty bad
     * situation to have one thread destroy a PBO instance that another thread
     * is calling into, but we try to handle that situation as well as possible.
     */
    internal->lock.Lock(MUTEX_CONTEXT);
    isExiting = true;
    set<SyncReplyContext>& replyCtxSet = internal->syncMethodCalls[this];
    set<SyncReplyContext>::iterator it;
    for (it = replyCtxSet.begin(); it != replyCtxSet.end(); ++it) {
        Thread* thread = (*it)->thread;
        QCC_LogError(ER_BUS_METHOD_CALL_ABORTED, ("Thread %s (%p) deleting ProxyBusObject called into by thread %s (%p)",
                                                  Thread::GetThreadName(), Thread::GetThread(),
                                                  thread->GetName(), thread));
        thread->Alert(SYNC_METHOD_ALERTCODE_ABORT);
    }

    /*
     * Now we wait for the outstanding synchronous method calls for this PBO to
     * get cleaned up.
     */
    while (!replyCtxSet.empty()) {
        internal->syncMethodComplete.Wait(internal->lock);
    }
    internal->syncMethodCalls.erase(this);
    internal->lock.Unlock(MUTEX_CONTEXT);
}


ProxyBusObject::ProxyBusObject(BusAttachment& bus, const char* service, const char* path, SessionId sessionId, bool isSecure) :
    internal(bus, path, service, sessionId, isSecure),
    legacyIntrospectionHandler(new LegacyIntrospectionHandler(this)),
    isExiting(false)
{
    /* The Peer interface is implicitly defined for all objects */
    AddInterface(org::freedesktop::DBus::Peer::InterfaceName);
}

ProxyBusObject::ProxyBusObject(BusAttachment& bus, const char* service, const char* uniqueName, const char* path, SessionId sessionId, bool isSecure) :
    internal(bus, path, service, uniqueName, sessionId, isSecure),
    legacyIntrospectionHandler(new LegacyIntrospectionHandler(this)),
    isExiting(false)
{
    /* The Peer interface is implicitly defined for all objects */
    AddInterface(org::freedesktop::DBus::Peer::InterfaceName);
}

ProxyBusObject::ProxyBusObject() :
    internal(),
    legacyIntrospectionHandler(new LegacyIntrospectionHandler(this)),
    isExiting(false)
{
}

ProxyBusObject::ProxyBusObject(const ProxyBusObject& other) :
    internal(other.internal),
    legacyIntrospectionHandler(new LegacyIntrospectionHandler(*(other.legacyIntrospectionHandler))),
    isExiting(false)
{
}

ProxyBusObject::ProxyBusObject(ManagedObj<ProxyBusObject::Internal> internal) :
    internal(internal),
    legacyIntrospectionHandler(new LegacyIntrospectionHandler(this)),
    isExiting(false)
{
}

bool ProxyBusObject::operator==(const ProxyBusObject& other) const {
    return internal == other.internal;
}
bool ProxyBusObject::operator<(const ProxyBusObject& other) const {
    return internal < other.internal;
}

ProxyBusObject& ProxyBusObject::operator=(const ProxyBusObject& other)
{
    if (this != &other) {
        internal = other.internal;
        legacyIntrospectionHandler.reset(new LegacyIntrospectionHandler(*other.legacyIntrospectionHandler));
        isExiting = false;
    }
    return *this;
}

void ProxyBusObject::SetB2BEndpoint(RemoteEndpoint& b2bEp)
{
    internal->b2bEp = b2bEp;
}

bool CachedProps::Get(const char* propname, MsgArg& val)
{
    bool found = false;
    lock.Lock(MUTEX_CONTEXT);
    ValueMap::iterator it = values.find(propname);
    if (it != values.end()) {
        found = true;
        val = it->second;
    }
    lock.Unlock(MUTEX_CONTEXT);
    return found;
}

bool CachedProps::GetAll(MsgArg& val)
{
    if (!isFullyCacheable || numProperties == 0) {
        return false;
    }

    bool found = false;
    lock.Lock(MUTEX_CONTEXT);
    if (values.size() == numProperties) {
        found = true;
        MsgArg* dict = new MsgArg[numProperties];
        ValueMap::iterator it = values.begin();
        for (int i = 0; it != values.end(); ++it, ++i) {
            MsgArg* inner;
            it->second.Get("v", &inner);
            dict[i].Set("{sv}", it->first.c_str(), inner);
            /* dict[i].Set("{sv}", it->first.c_str(), &(it->second)); */
        }
        val.Set("a{sv}", numProperties, dict);
        val.Stabilize();
        delete[] dict;
    }
    lock.Unlock(MUTEX_CONTEXT);
    return found;
}

bool CachedProps::IsValidMessageSerial(uint32_t messageSerial)
{
    uint32_t threshold = (uint32_t) (0x1 << 31);
    if (messageSerial >= lastMessageSerial) {
        // messageSerial should be higher than the last.
        // The check returns true unless the diff is too big.
        // In this case we assume an out-of-order message is processed.
        // The message was sent prior to a wrap around of the uint32_t counter.
        return ((messageSerial - lastMessageSerial) < threshold);
    }
    // The messageSerial is smaller than the last. This is an out-of-order
    // message (return false) unless the diff is too big. if the diff is high
    // we assume we hit a wrap around of the message serial counter (return true).
    return ((lastMessageSerial - messageSerial) > threshold);
}

bool CachedProps::IsCacheable(const char* propname)
{
    const InterfaceDescription::Property* prop = description->GetProperty(propname);
    return (prop != NULL && prop->cacheable);
}

void CachedProps::Set(const char* propname, const MsgArg& val, const uint32_t messageSerial)
{
    if (!IsCacheable(propname)) {
        return;
    }

    lock.Lock(MUTEX_CONTEXT);
    if (!enabled) {
        lock.Unlock(MUTEX_CONTEXT);
        return;
    }

    if (!IsValidMessageSerial(messageSerial)) {
        values.clear();
    } else {
        values[qcc::String(propname)] = val;
        lastMessageSerial = messageSerial;
    }
    lock.Unlock(MUTEX_CONTEXT);
}

void CachedProps::SetAll(const MsgArg& allValues, const uint32_t messageSerial)
{
    lock.Lock(MUTEX_CONTEXT);
    if (!enabled) {
        lock.Unlock(MUTEX_CONTEXT);
        return;
    }

    size_t nelem;
    MsgArg* elems;
    QStatus status = allValues.Get("a{sv}", &nelem, &elems);
    if (status != ER_OK) {
        goto error;
    }

    if (!IsValidMessageSerial(messageSerial)) {
        status = ER_FAIL;
        goto error;
    }

    for (size_t i = 0; i < nelem; ++i) {
        const char* prop;
        MsgArg* val;
        status = elems[i].Get("{sv}", &prop, &val);
        if (status != ER_OK) {
            goto error;
        }
        if (IsCacheable(prop)) {
            values[qcc::String(prop)].Set("v", val);
            values[qcc::String(prop)].Stabilize();
        }
    }

    lastMessageSerial = messageSerial;

    lock.Unlock(MUTEX_CONTEXT);
    return;

error:
    /* We can't make sense of the property values for some reason.
     * Play it safe and invalidate all properties */
    QCC_LogError(status, ("Failed to parse GetAll return value or inconsistent message serial number. Invalidating property cache."));
    values.clear();
    lock.Unlock(MUTEX_CONTEXT);
}

void CachedProps::PropertiesChanged(MsgArg* changed, size_t numChanged, MsgArg* invalidated, size_t numInvalidated, const uint32_t messageSerial)
{
    lock.Lock(MUTEX_CONTEXT);
    if (!enabled) {
        lock.Unlock(MUTEX_CONTEXT);
        return;
    }

    QStatus status;

    if (!IsValidMessageSerial(messageSerial)) {
        status = ER_FAIL;
        goto error;
    }

    for (size_t i = 0; i < numChanged; ++i) {
        const char* prop;
        MsgArg* val;
        status = changed[i].Get("{sv}", &prop, &val);
        if (status != ER_OK) {
            goto error;
        }
        if (IsCacheable(prop)) {
            values[qcc::String(prop)].Set("v", val);
            values[qcc::String(prop)].Stabilize();
        }
    }

    for (size_t i = 0; i < numInvalidated; ++i) {
        char* prop;
        status = invalidated[i].Get("s", &prop);
        if (status != ER_OK) {
            goto error;
        }
        values.erase(prop);
    }

    lastMessageSerial = messageSerial;

    lock.Unlock(MUTEX_CONTEXT);
    return;

error:
    /* We can't make sense of the property update signal for some reason.
     * Play it safe and invalidate all properties */
    QCC_LogError(status, ("Failed to parse PropertiesChanged signal or inconsistent message serial number. Invalidating property cache."));
    values.clear();
    lock.Unlock(MUTEX_CONTEXT);
}

BusAttachment& ProxyBusObject::GetBusAttachment() const
{
    return *(internal->bus);
}

void CachedProps::Enable()
{
    lock.Lock(MUTEX_CONTEXT);
    enabled = true;
    lock.Unlock(MUTEX_CONTEXT);
}

}
