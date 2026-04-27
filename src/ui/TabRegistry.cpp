#include "TabRegistry.h"

TabRegistry& TabRegistry::Get()
{
    static TabRegistry inst;
    return inst;
}

void TabRegistry::Register(AppTabDef def)
{
    tabs_.push_back(std::move(def));
}

AppTabDef* TabRegistry::Find(AppTab id)
{
    for (auto& t : tabs_)
        if (t.id == id) return &t;
    return nullptr;
}
