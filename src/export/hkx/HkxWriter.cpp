#include "export/hkx/HkxWriter.h"

HkxWriteResult WriteActorHkx(
    const ActorAnimExport& actorExport,
    const std::string&     behaviorOutPath,
    const std::string&     animOutPath)
{
    // TODO: implement HKX export pipeline
    (void)actorExport;
    (void)behaviorOutPath;
    (void)animOutPath;
    return { false, "HkxWriter not yet implemented" };
}
