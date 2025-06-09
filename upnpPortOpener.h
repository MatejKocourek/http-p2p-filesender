#pragma once
#include <natupnp.h>
#include <random>
#include <iostream>
#include <comdef.h>

static std::random_device dev;
static std::mt19937 rng(dev());
static std::uniform_int_distribution<std::mt19937::result_type> randomDynamicPort(49152, 65535);
static size_t counter = 0;

using _com_util::CheckError;

class PortMapping {
    CComPtr<IStaticPortMappingCollection> pStaticPortMappingCollection;
    CComPtr<IStaticPortMapping> pStaticPortMapping;// = NULL;
public:
    PortMapping(long port)
    {
        const bstr_t bstrInternalClient = L"192.168.64.162";
        const bstr_t bstrDescription = L"Testing";
        const bstr_t bstrProtocol = L"TCP";

        if (counter++ == 0)
            CheckError(CoInitialize(NULL));

        CComPtr<IUPnPNAT> piNAT;
        CheckError(piNAT.CoCreateInstance(__uuidof(UPnPNAT)));

        CheckError(piNAT->get_StaticPortMappingCollection(&pStaticPortMappingCollection));

        if (pStaticPortMappingCollection == nullptr)
            throw std::runtime_error("Cannot find any UPnP IGD device in the network. Check if UPnP is enabled on your router.");

        CheckError(pStaticPortMappingCollection->Add(port, bstrProtocol, port, bstrInternalClient, VARIANT_TRUE, bstrDescription, &pStaticPortMapping));
    }

    /// <summary>
    /// Retrieves the description associated with this port mapping.
    /// </summary>
    bstr_t getDescription() const
    {
        BSTR str;
        CheckError(pStaticPortMapping->get_Description(&str));

        bstr_t res;
        res.Attach(str);

        return res;
    }

    /// <summary>
    /// Retrieves whether the port mapping is enabled.
    /// </summary>
    bool getEnabled() const
    {
        VARIANT_BOOL res;
        CheckError(pStaticPortMapping->get_Enabled(&res));

        return res != VARIANT_FALSE;
    }

    /// <summary>
    /// Retrieves the external IP address for this port mapping on the NAT computer.
    /// </summary>
    bstr_t getExternalIPAddress() const
    {
        BSTR str;
        CheckError(pStaticPortMapping->get_ExternalIPAddress(&str));

        bstr_t res;
        res.Attach(str);

        return res;
    }

    /// <summary>
    /// Retrieves the external port on the NAT computer for this port mapping.
    /// </summary>
    long getExternalPort() const
    {
        long res;
        CheckError(pStaticPortMapping->get_ExternalPort(&res));
        return res;
    }

    /// <summary>
    /// Retrieves the name of the internal client for this port mapping.
    /// </summary>
    bstr_t getInternalClient() const
    {
        BSTR str;
        CheckError(pStaticPortMapping->get_InternalClient(&str));

        bstr_t res;
        res.Attach(str);

        return res;
    }

    /// <summary>
    /// Retrieves the internal port on the client computer for this port mapping.
    /// </summary>
    long getInternalPort() const
    {
        long res;
        CheckError(pStaticPortMapping->get_InternalPort(&res));
        return res;
    }

    /// <summary>
    /// Retrieves the protocol associated with this port mapping.
    /// </summary>
    bstr_t getProtocol() const
    {
        BSTR str;
        CheckError(pStaticPortMapping->get_Protocol(&str));

        bstr_t res;
        res.Attach(str);

        return res;
    }

    ~PortMapping()
    {
        CheckError(pStaticPortMappingCollection->Remove(getExternalPort(), getProtocol()));
        if (--counter == 0)
            CoUninitialize();
    }
};

PortMapping openRandomDynamicPort()
{
    try
    {
        auto port = randomDynamicPort(rng);

        std::cerr << "Trying to open port " << port << std::endl;

        PortMapping pm(port);

        return pm;
    }
    catch (const _com_error& e)
    {
        throw std::runtime_error(e.Description());
    }
}