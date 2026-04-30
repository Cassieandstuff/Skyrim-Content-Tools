#include "export/yaml/YamlSidecar.h"

YamlSidecarResult WriteYamlSidecar(
    const std::string& outPath,
    const SpawnList&   spawnList,
    const WaitNode&    defaultWaitParams)
{
    // TODO: implement YAML sidecar serialization (yaml-cpp)
    (void)outPath;
    (void)spawnList;
    (void)defaultWaitParams;
    return { false, "YamlSidecar not yet implemented" };
}
