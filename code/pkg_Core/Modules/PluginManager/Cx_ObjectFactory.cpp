// Copyright 2008-2011 Zhang Yun Gui, rhcad@hotmail.com
// http://sourceforge.net/projects/x3c/

// author: Zhang Yun Gui, Tao Jian Lin
// v2: 2011.1.5, ooyg: change class-table to hash_map
// v3: 2011.2.4, ooyg: Don't remove element in ReleaseModule().
// v4: 2011.2.7, ooyg: Implement the delay-loaded feature.

#include "StdAfx.h"
#include "Cx_ObjectFactory.h"

Cx_ObjectFactory::Cx_ObjectFactory()
{
}

Cx_ObjectFactory::~Cx_ObjectFactory()
{
}

bool Cx_ObjectFactory::IsCreatorRegister(const XCLSID& clsid)
{
    return FindEntry(clsid) != NULL;
}

HRESULT Cx_ObjectFactory::CreateObject(const XCLSID& clsid, 
                                       Ix_Object** ppv, 
                                       HMODULE fromdll)
{
    ASSERT(clsid.valid() && ppv != NULL);
    *ppv = NULL;

    int moduleIndex = -1;
    _XCLASSMETA_ENTRY* pEntry = FindEntry(clsid, &moduleIndex);

    if (pEntry && !pEntry->pfnObjectCreator && moduleIndex >= 0)
    {
        if (!LoadDelayPlugin(m_modules[moduleIndex].filename))
        {
            CLSMAP::iterator it = m_clsmap.find(clsid.str());
            if (it != m_clsmap.end())
            {
                it->second.second = -1; // next time: moduleIndex = -1
            }
        }
        pEntry = FindEntry(clsid, &moduleIndex);
    }

    if (pEntry && pEntry->pfnObjectCreator)
    {
        *ppv = pEntry->pfnObjectCreator(fromdll);
        ASSERT(*ppv);
        return S_OK;
    }
    else
    {
        return E_NOTIMPL;
    }
}

long Cx_ObjectFactory::CreateSpecialInterfaceObjects(const char* iid)
{
    ASSERT(iid && *iid);

    long count = 0;
    CLSMAP::const_iterator it = m_clsmap.begin();

    for (; it != m_clsmap.end(); ++it)
    {
        const _XCLASSMETA_ENTRY& cls = it->second.first;
        if (lstrcmpiA(iid, cls.iidSpecial) == 0
            && cls.pfnObjectCreator)
        {
            Ix_Object* pIF = NULL;
            pIF = (cls.pfnObjectCreator)(xGetModuleHandle());
            ASSERT(pIF);
            pIF->Release(xGetModuleHandle());
            count++;
        }
    }

    return count;
}

bool Cx_ObjectFactory::QuerySpecialInterfaceObject(
        long index, const char* iid, Ix_Object** ppv)
{
    bool bRet = IsValidIndexOf(m_modules, index) && ppv != NULL;
    if (!bRet)
    {
        return false;
    }

    *ppv = NULL;

    const CLSIDS& clsids = m_modules[index].clsids;
    CLSIDS::const_iterator it = clsids.begin();

    for (; it != clsids.end(); ++it)
    {
        CLSMAP::const_iterator mit = m_clsmap.find(it->str());

        if (mit != m_clsmap.end()
            && lstrcmpiA(iid, mit->second.first.iidSpecial) == 0
            && mit->second.first.pfnObjectCreator)
        {
            *ppv = (mit->second.first.pfnObjectCreator)(xGetModuleHandle());
            return true;
        }
    }

    return false;
}

bool Cx_ObjectFactory::HasCreatorReplaced(const XCLSID& clsid)
{
    clsid;
    return false;
}

_XCLASSMETA_ENTRY* Cx_ObjectFactory::FindEntry(const XCLSID& clsid, 
                                               int* moduleIndex)
{
    CLSMAP::iterator it = m_clsmap.find(clsid.str());
    if (moduleIndex)
    {
        *moduleIndex = (it == m_clsmap.end()) ? -1 : it->second.second;
    }
    return (it == m_clsmap.end()) ? NULL : &(it->second.first);
}

int Cx_ObjectFactory::FindModule(HMODULE hModule)
{
    int i = GetSize(m_modules);
    while (--i >= 0 && m_modules[i].hdll != hModule) ;

    return i;
}

Ix_Module* Cx_ObjectFactory::GetModule(HMODULE hModule)
{
    int index = FindModule(hModule);
    if (index >= 0)
    {
        return m_modules[index].module;
    }

    typedef Ix_Module* (*FUNC_MODULE)(Ix_ObjectFactory*, HMODULE);
    FUNC_MODULE pfn = (FUNC_MODULE)GetProcAddress(
        hModule, "_xGetModuleInterface");

    if (pfn != NULL)
    {
        Ix_Module* pModule = (*pfn)(this, hModule);
        return pModule;
    }
    else
    {
        return NULL;
    }
}

long Cx_ObjectFactory::RegisterClassEntryTable(HMODULE hModule)
{
    int moduleIndex = FindModule(hModule);
    ASSERT(moduleIndex >= 0);   // must call RegisterPlugin before

    Ix_Module* pModule = GetModule(hModule);
    ASSERT(pModule);            // must call RegisterPlugin before

    if (!m_modules[moduleIndex].clsids.empty())
    {
        return GetSize(m_modules[moduleIndex].clsids);
    }

    typedef DWORD (*FUNC_GET)(DWORD*, DWORD*, _XCLASSMETA_ENTRY*, DWORD);
    FUNC_GET pfn = (FUNC_GET)GetProcAddress(hModule, "_xGetClassEntryTable");

    if (!pfn)       // is not a plugin
    {
        return -1;
    }

    DWORD buildInfo = 0;
    int classCount = (*pfn)(&buildInfo, NULL, NULL, 0);

    if (classCount <= 0)
    {
        return 0;
    }

    std::vector<_XCLASSMETA_ENTRY> table(classCount);
    DWORD size = sizeof(_XCLASSMETA_ENTRY);

    classCount = (*pfn)(NULL, &size, &table[0], classCount);

    for (int i = 0; i < classCount; i++)
    {
        _XCLASSMETA_ENTRY& cls = table[i];

        if (cls.clsid.valid())
        {
            RegisterClass(moduleIndex, cls);
        }
        if (cls.iidSpecial && *cls.iidSpecial)
        {
            char tmpclsid[40] = { 0 };

            sprintf_s(tmpclsid, 40, "iid%lx:%d", hModule, i);
            cls.clsid = XCLSID(tmpclsid);
            RegisterClass(moduleIndex, cls);
        }
    }

    return classCount;
}

bool Cx_ObjectFactory::RegisterClass(int moduleIndex, 
                                     const _XCLASSMETA_ENTRY& cls)
{
    ASSERT(moduleIndex >= 0 && cls.clsid.valid());
    _XCLASSMETA_ENTRY* pOldCls = FindEntry(cls.clsid);

    if (pOldCls && pOldCls->pfnObjectCreator)
    {
        char msg[256] = { 0 };
        sprintf_s(msg, 256, 
            "The classid '%s' is already registered by '%s', "
            "then '%s' register fail.", 
            cls.clsid.str(), pOldCls->className, cls.className);
        ASSERT_MESSAGE(false, msg);
        return false;
    }

    m_clsmap[cls.clsid.str()] = MAPITEM(cls, moduleIndex);
    m_modules[moduleIndex].clsids.push_back(cls.clsid);

    return true;
}

void Cx_ObjectFactory::ReleaseModule(HMODULE hModule)
{
    int index = FindModule(hModule);
    ASSERT(index >= 0);

    MODULEINFO& item = m_modules[index];
    CLSIDS::const_iterator it = item.clsids.begin();

    for (; it != item.clsids.end(); ++it)
    {
        CLSMAP::iterator mit = m_clsmap.find(it->str());
        if (mit != m_clsmap.end())
        {
            m_clsmap.erase(mit);
        }
    }

    if (item.owned)
    {
        FreeLibrary(hModule);
    }

    // don't remove: m_modules.erase(m_modules.begin() + index);
    item.hdll = NULL;
    item.module = NULL;
    item.clsids.clear();
}
